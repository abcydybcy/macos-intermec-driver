# Intermec EasyCoder PF8t CUPS Driver

MacOS CUPS driver for the Intermec EasyCoder C4 and PF8 thermal label printers (203 DPI, ESim v7.x protocol).
Will probably work with other models and Linux (will need a change in the cups pipeline to use pdftoraster instrad of cgpdftoraster).   
This is vibecoded because I couldn't be bothered with C. Tested on MacOS, works - quite well actually.

## Components

| File                 | Description                                                                   |
|----------------------|-------------------------------------------------------------------------------|
| `rastertointermec.c` | CUPS raster filter — converts PDF raster to ESim GW bitmap commands           |
| `serial.c`           | CUPS backend (`intserial`) — serial port with userspace XON/XOFF flow control |
| `intermec-pf8t.ppd`  | PPD file with metric label sizes and printer options                          |
| `build.sh`           | Build script for all components                                               |
| `tcpserial.c`        | Standalone TCP-to-serial bridge (for testing)                                 |

## Requirements

- macOS with Xcode Command Line Tools (`xcode-select --install`)
- USB-serial adapter (or any other form of a serial port)
- CUPS (included in macOS)

## Build

```bash
./build.sh
```

This compiles three binaries into `dist/`:

- `rastertointermec` — CUPS raster filter
- `intserial` — CUPS serial backend
- `tcpserial` — standalone TCP-serial bridge (optional)

## Install

### 1. Copy the filter and backend

```bash
sudo cp dist/rastertointermec /usr/libexec/cups/filter/rastertointermec
sudo cp dist/intserial /usr/libexec/cups/backend/intserial
sudo chown root:_lp /usr/libexec/cups/filter/rastertointermec /usr/libexec/cups/backend/intserial
sudo chmod 755 /usr/libexec/cups/filter/rastertointermec
sudo chmod 700 /usr/libexec/cups/backend/intserial
```

### 2. Add the printer

Replace `/dev/cu.usbserial-110` with your actual serial device path:

```bash
sudo lpadmin -p IntermecPF8t \
  -v "intserial:/dev/cu.usbserial-110?baud=9600" \
  -P intermec-pf8t.ppd \
  -E
```

### 3. Restart CUPS

```bash
sudo launchctl stop org.cups.cupsd && sudo launchctl start org.cups.cupsd
```

### 4. Find your serial device

If you don't know the device path:

```bash
ls /dev/cu.usbserial-*
```

## Updating

After making changes to the source:

```bash
./build.sh
sudo cp dist/rastertointermec /usr/libexec/cups/filter/rastertointermec
sudo cp dist/intserial /usr/libexec/cups/backend/intserial
sudo chmod 700 /usr/libexec/cups/backend/intserial
```

To update the PPD (e.g. after changing label sizes or options):

```bash
sudo cp intermec-pf8t.ppd /private/etc/cups/ppd/IntermecPF8t.ppd
sudo launchctl stop org.cups.cupsd && sudo launchctl start org.cups.cupsd
```

## Print Options

Available in the macOS print dialog under "Printer Features":

| Option        | Values                                                                      | Default          |
|---------------|-----------------------------------------------------------------------------|------------------|
| Media Size    | 100x50, 100x70, 80x50, 60x40, 60x30, 50x30, 50x25, 40x40, 40x30 mm + Custom | 100x50 mm        |
| Print Speed   | 2 ips (50 mm/s), 3 ips (75 mm/s), 4 ips (100 mm/s)                          | 2 ips            |
| Darkness      | Light (5), Medium-light (8), Medium (10), Medium-dark (12), Dark (15)       | Dark (15)        |
| Print Mode    | Direct thermal, Thermal transfer                                            | Thermal transfer |
| Auto Contrast | Off, Light, Medium, Strong                                                  | Off              |

## How It Works

### Pipeline

```
PDF -> cgpdftoraster -> rastertointermec -> intserial -> serial port -> printer
```

1. **cgpdftoraster** (Apple) renders PDF to 8-bit grayscale raster at 203 DPI
2. **rastertointermec** converts raster to ESim commands:
    - Optional auto-contrast (histogram stretch + S-curve)
    - Atkinson dithering with serpentine scanning (8-bit to 1-bit)
    - Deduplicates identical pages for efficient multi-copy printing
    - Emits ESim setup commands (density, speed, label size) and GW bitmap + P print commands
3. **intserial** sends data to the printer over serial:
    - Auto-negotiates from 9600 to 19200 baud using ESim Y command
    - Userspace XON/XOFF flow control (printer sends XOFF to pause, XON to resume)
    - Queries printer status via `^ee` command, reports errors to CUPS
    - Resets printer with `^@` if needed

### Serial Protocol

- 19200 baud, 8N1 (auto-negotiated from 9600 default)
- XON/XOFF flow control (always enabled, RTS/CTS not supported)
- ESim v7.x command set, CR+LF line terminators

## Troubleshooting

### Check CUPS logs

```bash
tail -f /var/log/cups/error_log | grep -E "intserial|rastertointermec"
```

### Printer not responding

The printer may be stuck at a non-default baud rate. Reset it:

```bash
python3 -c "
import serial, time
for baud in [19200, 9600]:
    ser = serial.Serial('/dev/cu.usbserial-110', baud, timeout=2)
    ser.write(b'\r\n^ee\r\n')
    time.sleep(0.5)
    resp = ser.read(100)
    if resp:
        print(f'Printer at {baud}: {resp}')
        if baud != 9600:
            ser.write(b'\r\nY96,N,8,1\r\n')
            time.sleep(1)
            print('Reset to 9600')
        ser.close()
        break
    ser.close()
"
```

Requires `pip3 install pyserial`.

### Recreate the printer

If PPD changes aren't reflected in the print dialog:

```bash
sudo lpadmin -x IntermecPF8t
sudo lpadmin -p IntermecPF8t \
  -v "intserial:/dev/cu.usbserial-110?baud=9600" \
  -P intermec-pf8t.ppd \
  -E
sudo launchctl stop org.cups.cupsd && sudo launchctl start org.cups.cupsd
```

### Alternative: TCP-serial bridge

If you prefer using the CUPS socket backend instead of `intserial`:

```bash
./dist/tcpserial /dev/cu.usbserial-110
```

Then configure the printer with URI `socket://localhost:9100`.
