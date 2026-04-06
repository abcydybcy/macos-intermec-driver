/*
 * serial.c — CUPS backend for Intermec PF8t over serial (USB-serial)
 *
 * Replaces both the socket backend + tcpserial bridge with a single
 * CUPS backend that talks directly to the serial port with userspace
 * XON/XOFF flow control and ESim ^ee error reporting.
 *
 * Install as: /usr/libexec/cups/backend/intserial
 *
 * CUPS device URI: intserial:/dev/cu.usbserial-110?baud=9600
 *
 * When called with no arguments, outputs device discovery line.
 * When called by CUPS scheduler with 6-7 arguments, sends the job.
 *
 * Exit codes per CUPS backend protocol:
 *   0 = CUPS_BACKEND_OK
 *   1 = CUPS_BACKEND_FAILED
 *   4 = CUPS_BACKEND_STOP  (stop queue, needs user intervention)
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define CUPS_BACKEND_OK      0
#define CUPS_BACKEND_FAILED  1
#define CUPS_BACKEND_STOP    4

#define BUF_SIZE    4096
#define QUEUE_SIZE  (256 * 1024)
#define XOFF_BYTE   0x13
#define XON_BYTE    0x11
#define NAK_BYTE    0x15

static volatile sig_atomic_t g_quit = 0;
static void sig_handler(int sig) { (void)sig; g_quit = 1; }

/* -------------------------------------------------------------------------
 * Logging — CUPS expects "LEVEL: message" on stderr
 * ---------------------------------------------------------------------- */

#define log_debug(fmt, ...)   fprintf(stderr, "DEBUG: [intserial] " fmt "\n", ##__VA_ARGS__)
#define log_info(fmt, ...)    fprintf(stderr, "INFO: [intserial] " fmt "\n", ##__VA_ARGS__)
#define log_warn(fmt, ...)    fprintf(stderr, "WARNING: [intserial] " fmt "\n", ##__VA_ARGS__)
#define log_error(fmt, ...)   fprintf(stderr, "ERROR: [intserial] " fmt "\n", ##__VA_ARGS__)
#define log_state(fmt, ...)   fprintf(stderr, "STATE: " fmt "\n", ##__VA_ARGS__)
#define log_attr(fmt, ...)    fprintf(stderr, "ATTR: " fmt "\n", ##__VA_ARGS__)

/* -------------------------------------------------------------------------
 * ESim error codes (from spec pages 158-159)
 * ---------------------------------------------------------------------- */

struct esim_error {
    int code;           /* ^ee response code */
    char type;          /* A=abort, B=media, C=ribbon, D=hardware, F=flash, G=mode, H=pause */
    const char *desc;
    int stop_queue;     /* 1 = stop queue (needs user intervention) */
};

static const struct esim_error error_table[] = {
    {  0, '-', "No error",                                0 },
    {  1, 'A', "Syntax error",                            0 },
    {  2, 'A', "Object exceeds image buffer border",      0 },
    {  3, 'A', "Data length error",                       0 },
    {  4, 'A', "Insufficient memory to store data",       1 },
    {  5, 'A', "Memory configuration error",              1 },
    {  7, 'A', "Out of media",                            1 },
    {  8, 'A', "Form or image name duplicate",            0 },
    {  9, 'A', "Form or image not found",                 0 },
    { 11, 'D', "Printhead up (cover open)",               1 },
    { 13, 'B', "LTS detection waiting, peel pause",       0 },
    { 16, 'A', "No form retrieved before variable input", 0 },
    { 17, 'C', "Out of ribbon",                           1 },
    { 50, 'A', "Does not fit in area specified",          0 },
    { 51, 'A', "Data length too long",                    0 },
    { 61, 'D', "High motor temperature",                  1 },
    { 62, 'D', "High printhead temperature",              1 },
    { 71, 'F', "Wait after default setup",                0 },
    { 72, 'F', "Flashing not completed",                  1 },
    { 73, 'F', "Download error",                          0 },
    { 81, 'G', "Cutter jammed or not installed",          1 },
    { 89, 'G', "Dump mode (after auto-detection)",        0 },
    { 92, 'H', "Pause printing mode",                     0 },
    { 94, 'G', "Autosensing mode",                        0 },
    { 99, '-', "Other error",                             1 },
    { -1, 0,   NULL,                                      0 },
};

static const struct esim_error *lookup_error(int code)
{
    for (int i = 0; error_table[i].desc; i++) {
        if (error_table[i].code == code)
            return &error_table[i];
    }
    return NULL;
}

/* Map error codes to CUPS state reasons where applicable */
static void report_cups_state(int code)
{
    /* Clear previous marker states */
    log_state("-com.intermec.error");

    switch (code) {
    case 0:
        /* All clear */
        break;
    case 7:
        log_state("+media-empty-error");
        log_error("Printer is out of media");
        break;
    case 11:
        log_state("+cover-open");
        log_error("Printhead is up (cover open)");
        break;
    case 17:
        log_state("+marker-supply-empty-error");
        log_error("Printer is out of ribbon");
        break;
    case 62:
        log_state("+marker-supply-empty-warning");
        log_warn("High printhead temperature — pausing");
        break;
    case 61:
        log_state("+com.intermec.error");
        log_warn("High motor temperature — pausing");
        break;
    case 81:
        log_state("+com.intermec.error");
        log_error("Cutter jammed or not installed");
        break;
    default:
        if (code != 0) {
            log_state("+com.intermec.error");
        }
        break;
    }
}

/* -------------------------------------------------------------------------
 * Serial port
 * ---------------------------------------------------------------------- */

static speed_t baud_const(int baud)
{
    switch (baud) {
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return B9600;
    }
}

static int open_serial(const char *path, int baud)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        log_error("Cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        log_error("tcgetattr %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&t);
    cfsetispeed(&t, baud_const(baud));
    cfsetospeed(&t, baud_const(baud));

    t.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
    t.c_cflag |= CS8 | CLOCAL | CREAD;

    /* No kernel flow control — userspace XON/XOFF */
    t.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK |
                    ISTRIP | INLCR | IGNCR | ICRNL);

    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        log_error("tcsetattr %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    log_info("Opened %s @ %d baud, 8N1, XON/XOFF", path, baud);
    return fd;
}

/* Forward declaration */
static int parse_ee_response(const unsigned char *buf, ssize_t len);

/* -------------------------------------------------------------------------
 * Change serial port baud rate on an open fd
 * ---------------------------------------------------------------------- */

static int set_baud(int fd, int baud)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return -1;
    cfsetispeed(&t, baud_const(baud));
    cfsetospeed(&t, baud_const(baud));
    if (tcsetattr(fd, TCSANOW, &t) < 0) return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Negotiate higher baud rate with printer
 *
 * ESim Y command: Y<baud>,<parity>,<databits>,<stopbits>
 * Baud values: 19=19200, 96=9600, 48=4800, 24=2400, 12=1200
 *
 * Strategy:
 *   1. At current baud, send Y19,N,8,1 to switch printer to 19200
 *   2. Wait for printer to reconfigure
 *   3. Switch our serial port to 19200
 *   4. Verify with ^ee
 *   5. On failure, try to fall back to original baud
 * ---------------------------------------------------------------------- */

/*
 * Try to communicate with the printer at a given baud rate.
 * Returns 0 if printer responds to ^ee, -1 otherwise.
 */
static int try_communicate(int ser_fd, int baud)
{
    unsigned char buf[256];

    set_baud(ser_fd, baud);
    tcflush(ser_fd, TCIOFLUSH);
    usleep(50000);

    tcflush(ser_fd, TCIFLUSH);
    write(ser_fd, "\r\n^ee\r\n", 7);
    tcdrain(ser_fd);

    struct pollfd pfd = { .fd = ser_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 1000);
    if (ret > 0) {
        ssize_t n = read(ser_fd, buf, sizeof(buf));
        if (n > 0 && parse_ee_response(buf, n) >= 0)
            return 0;
    }
    return -1;
}

/*
 * Establish communication and set up 19200 baud.
 *
 * Strategy:
 *   1. Try 19200 first (printer may already be configured)
 *   2. If no response, try 9600 (factory default)
 *   3. If at 9600, reset printer with ^@ then send Y19,N,8,1
 *   4. Switch to 19200 and verify
 *   5. Returns the active baud rate
 */
static int setup_serial(int ser_fd)
{
    /* Try 19200 first (happy path — already configured) */
    log_info("Trying 19200 baud...");
    if (try_communicate(ser_fd, 19200) == 0) {
        log_info("Printer already at 19200 baud");
        return 19200;
    }

    /* Try 9600 (factory default / after power cycle) */
    log_info("Trying 9600 baud...");
    if (try_communicate(ser_fd, 9600) != 0) {
        /* No response — reset and retry */
        log_warn("No response, sending ^@ reset at 9600...");
        set_baud(ser_fd, 9600);
        tcflush(ser_fd, TCIOFLUSH);
        write(ser_fd, "\r\n^@\r\n", 6);
        tcdrain(ser_fd);
        sleep(2);

        if (try_communicate(ser_fd, 9600) != 0) {
            log_error("Printer not responding after reset");
            return -1;
        }
    }

    /* At 9600 — switch to 19200 */
    log_info("Switching to 19200...");
    tcflush(ser_fd, TCIOFLUSH);
    write(ser_fd, "\r\nY19,N,8,1\r\n", 14);
    tcdrain(ser_fd);
    usleep(300000);

    if (try_communicate(ser_fd, 19200) == 0) {
        log_info("Now running at 19200 baud");
        return 19200;
    }

    /* Fall back to 9600 */
    log_warn("19200 failed, using 9600");
    if (try_communicate(ser_fd, 9600) == 0)
        return 9600;

    log_error("Lost communication with printer");
    return -1;
}

/* -------------------------------------------------------------------------
 * Parse ^ee response from serial RX buffer
 *
 * Response format: "XX\r\n" or "XX,YY,ZZ\r\n"
 * Returns the first (highest priority) error code, or -1 if none parsed.
 * Also strips XON/XOFF/NAK from the buffer.
 * ---------------------------------------------------------------------- */

static int parse_ee_response(const unsigned char *buf, ssize_t len)
{
    /* Extract digit/comma characters */
    char status[128] = {0};
    size_t si = 0;
    for (ssize_t i = 0; i < len && si < sizeof(status) - 1; i++) {
        if (buf[i] >= '0' && buf[i] <= '9')
            status[si++] = buf[i];
        else if (buf[i] == ',' && si > 0)
            status[si++] = buf[i];
    }

    if (si == 0)
        return -1;

    /* Parse first code */
    int code = atoi(status);
    return code;
}

/* -------------------------------------------------------------------------
 * Query printer status via ^ee
 * Returns: error code (0 = OK), or -1 if no response
 * ---------------------------------------------------------------------- */

static int query_status(int ser_fd)
{
    unsigned char buf[256];

    /* Flush any pending data */
    tcflush(ser_fd, TCIFLUSH);

    const char *cmd = "^ee\r\n";
    write(ser_fd, cmd, strlen(cmd));

    /* Wait for response */
    struct pollfd pfd = { .fd = ser_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 2000);  /* 2 second timeout */
    if (ret <= 0)
        return -1;

    ssize_t n = read(ser_fd, buf, sizeof(buf));
    if (n <= 0)
        return -1;

    int code = parse_ee_response(buf, n);

    if (code >= 0) {
        const struct esim_error *err = lookup_error(code);
        if (err) {
            if (code == 0)
                log_info("Printer status: %02d — %s", code, err->desc);
            else
                log_warn("Printer status: %02d [%c] — %s", code, err->type, err->desc);
            report_cups_state(code);
        } else {
            log_warn("Printer status: %02d (unknown code)", code);
        }
    }

    return code;
}

/* -------------------------------------------------------------------------
 * Wait for printer to recover from an error
 * Polls ^ee every 3 seconds until status is 00 or we're told to quit.
 * Returns 0 if recovered, -1 if gave up.
 * ---------------------------------------------------------------------- */

static int wait_for_recovery(int ser_fd)
{
    log_info("Waiting for printer to recover...");

    for (int attempt = 0; attempt < 100 && !g_quit; attempt++) {
        sleep(3);
        int code = query_status(ser_fd);
        if (code == 0) {
            log_info("Printer recovered");
            log_state("-cover-open");
            log_state("-media-empty-error");
            log_state("-marker-supply-empty-error");
            log_state("-com.intermec.error");
            return 0;
        }
        if (code < 0) {
            log_warn("No response from printer (attempt %d)", attempt + 1);
        }
    }

    log_error("Printer did not recover");
    return -1;
}

/* -------------------------------------------------------------------------
 * Parse device URI: intserial:/dev/cu.usbserial-110?baud=9600
 * ---------------------------------------------------------------------- */

static void parse_uri(const char *uri, char *devpath, size_t devpath_sz, int *baud)
{
    *baud = 9600;
    devpath[0] = '\0';

    /* Skip scheme */
    const char *p = uri;
    if (strncmp(p, "intserial:", 10) == 0)
        p += 10;

    /* Copy device path (up to '?' or end) */
    size_t i = 0;
    while (*p && *p != '?' && i < devpath_sz - 1)
        devpath[i++] = *p++;
    devpath[i] = '\0';

    /* Parse query parameters */
    if (*p == '?') {
        p++;
        const char *baud_str = strstr(p, "baud=");
        if (baud_str) {
            *baud = atoi(baud_str + 5);
            if (*baud <= 0) *baud = 9600;
        }
    }
}

/* -------------------------------------------------------------------------
 * Send job data to printer with XON/XOFF flow control
 * ---------------------------------------------------------------------- */

static int send_job(int ser_fd, int input_fd)
{
    unsigned char rxbuf[BUF_SIZE];
    unsigned char *queue = malloc(QUEUE_SIZE);
    if (!queue) {
        log_error("Out of memory");
        return CUPS_BACKEND_FAILED;
    }

    size_t q_head = 0, q_tail = 0;
    int paused = 0;
    int input_done = 0;
    size_t total_tx = 0;
    int xoff_count = 0;
    int result = CUPS_BACKEND_OK;

    while (!g_quit) {
        struct pollfd pfd[2];
        int nfds = 1;

        /* Serial port — always listen for input, write if we have data */
        pfd[0].fd = ser_fd;
        pfd[0].events = POLLIN;
        if (q_head != q_tail && !paused)
            pfd[0].events |= POLLOUT;

        /* Input fd — read if queue has space and input not done */
        if (!input_done) {
            size_t q_used = (q_head >= q_tail) ? (q_head - q_tail) : (QUEUE_SIZE - q_tail + q_head);
            if (q_used < QUEUE_SIZE - BUF_SIZE) {
                pfd[1].fd = input_fd;
                pfd[1].events = POLLIN;
                nfds = 2;
            }
        }

        /* If queue is empty and input is done, we're finished */
        if (input_done && q_head == q_tail)
            break;

        int ret = poll(pfd, nfds, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Read from input → enqueue */
        if (nfds == 2 && (pfd[1].revents & POLLIN)) {
            ssize_t n = read(input_fd, rxbuf, sizeof(rxbuf));
            if (n <= 0) {
                input_done = 1;
                log_debug("Input EOF, draining queue");
            } else {
                for (ssize_t i = 0; i < n; i++) {
                    queue[q_head] = rxbuf[i];
                    q_head = (q_head + 1) % QUEUE_SIZE;
                }
            }
        }

        /* Read from serial — handle XON/XOFF and responses */
        if (pfd[0].revents & POLLIN) {
            ssize_t n = read(ser_fd, rxbuf, sizeof(rxbuf));
            for (ssize_t i = 0; i < n; i++) {
                if (rxbuf[i] == XOFF_BYTE) {
                    paused = 1;
                    xoff_count++;
                    size_t queued = (q_head >= q_tail) ? (q_head - q_tail) : (QUEUE_SIZE - q_tail + q_head);
                    log_debug("XOFF #%d (pausing, %zu queued)", xoff_count, queued);

                    /* Query error status */
                    const char *ee_cmd = "^ee\r\n";
                    write(ser_fd, ee_cmd, strlen(ee_cmd));
                } else if (rxbuf[i] == XON_BYTE) {
                    if (paused) {
                        paused = 0;
                        log_debug("XON (resuming)");
                    }
                } else if (rxbuf[i] == NAK_BYTE) {
                    /* NAK precedes error codes in serial response — skip it */
                } else if (rxbuf[i] == '>') {
                    /* Command prompt — printer ready for next command */
                    log_debug("Prompt '>'");
                } else if (rxbuf[i] >= '0' && rxbuf[i] <= '9') {
                    /* Possibly start of ^ee response — collect digits */
                    char codebuf[32] = {0};
                    size_t ci = 0;
                    codebuf[ci++] = rxbuf[i];
                    /* Grab remaining digits/commas from buffer */
                    for (i++; i < n && ci < sizeof(codebuf) - 1; i++) {
                        if (rxbuf[i] >= '0' && rxbuf[i] <= '9')
                            codebuf[ci++] = rxbuf[i];
                        else if (rxbuf[i] == ',')
                            codebuf[ci++] = rxbuf[i];
                        else if (rxbuf[i] == '\r' || rxbuf[i] == '\n')
                            break;
                        else {
                            i--;  /* push back non-digit */
                            break;
                        }
                    }

                    int code = atoi(codebuf);
                    const struct esim_error *err = lookup_error(code);

                    if (code == 0) {
                        log_debug("Status: 00 (OK)");
                        /* Clear error, XON should follow */
                    } else if (err) {
                        log_warn("Printer error %02d [%c]: %s",
                                 code, err->type, err->desc);
                        report_cups_state(code);

                        if (err->stop_queue) {
                            log_error("Recoverable error — waiting for user intervention");
                            if (wait_for_recovery(ser_fd) < 0) {
                                result = CUPS_BACKEND_STOP;
                                goto done;
                            }
                        }
                    } else {
                        log_warn("Unknown printer status: %s", codebuf);
                    }
                } else if (rxbuf[i] == '\r' || rxbuf[i] == '\n') {
                    /* Ignore CR/LF */
                } else {
                    log_debug("RX: 0x%02x '%c'", rxbuf[i],
                              (rxbuf[i] >= 0x20 && rxbuf[i] < 0x7f) ? rxbuf[i] : '.');
                }
            }
        }

        /* Write queued data to serial */
        if ((pfd[0].revents & POLLOUT) && !paused && q_head != q_tail) {
            size_t avail = (q_head >= q_tail) ? (q_head - q_tail) : (QUEUE_SIZE - q_tail);
            if (avail > 64) avail = 64;
            ssize_t w = write(ser_fd, queue + q_tail, avail);
            if (w > 0) {
                q_tail = (q_tail + w) % QUEUE_SIZE;
                total_tx += w;
            } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                log_error("Serial write failed: %s", strerror(errno));
                result = CUPS_BACKEND_FAILED;
                goto done;
            }
        }

        if (pfd[0].revents & POLLERR) {
            log_error("Serial port error");
            result = CUPS_BACKEND_FAILED;
            goto done;
        }
    }

done:
    log_info("Job complete. TX %zu bytes, %d XOFFs", total_tx, xoff_count);
    free(queue);
    return result;
}

/* -------------------------------------------------------------------------
 * main
 *
 * CUPS backend protocol:
 *   0 args: device discovery
 *   5-6 args: job-id user title copies options [file]
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* Device discovery mode */
    if (argc == 1) {
        printf("serial intserial \"Unknown\" "
               "\"Intermec PF8t Serial\" "
               "\"MFG:Intermec;MDL:PF8t;CMD:ESim;\"\n");
        return CUPS_BACKEND_OK;
    }

    if (argc < 6 || argc > 7) {
        fprintf(stderr,
                "Usage: intserial job-id user title copies options [file]\n"
                "       intserial          (device discovery)\n"
                "Device URI: intserial:/dev/cu.usbserial-110?baud=9600\n");
        return CUPS_BACKEND_FAILED;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Get device URI from environment (set by CUPS) */
    const char *uri = getenv("DEVICE_URI");
    if (!uri || !*uri) {
        log_error("DEVICE_URI not set");
        return CUPS_BACKEND_FAILED;
    }

    char devpath[512];
    int baud;
    parse_uri(uri, devpath, sizeof(devpath), &baud);

    if (!devpath[0]) {
        log_error("No device path in URI: %s", uri);
        return CUPS_BACKEND_FAILED;
    }

    log_info("Job %s from %s: '%s' (%s copies)",
             argv[1], argv[2], argv[3], argv[4]);
    log_info("URI: %s → %s @ %d", uri, devpath, baud);

    /* Open serial port at 9600 (safe default for initial contact) */
    int ser_fd = open_serial(devpath, 9600);
    if (ser_fd < 0)
        return CUPS_BACKEND_FAILED;

    /* Establish communication and switch to 19200 baud */
    int active_baud = setup_serial(ser_fd);
    if (active_baud < 0) {
        close(ser_fd);
        return CUPS_BACKEND_FAILED;
    }

    /* Check printer status before sending job */
    int status = query_status(ser_fd);
    if (status > 0) {
        const struct esim_error *err = lookup_error(status);
        if (err && err->stop_queue) {
            log_error("Printer error before job: %02d — %s", status, err->desc);
            if (wait_for_recovery(ser_fd) < 0) {
                close(ser_fd);
                return CUPS_BACKEND_STOP;
            }
        }
    }

    /* Open input: file argument or stdin */
    int input_fd;
    if (argc == 7) {
        input_fd = open(argv[6], O_RDONLY);
        if (input_fd < 0) {
            log_error("Cannot open %s: %s", argv[6], strerror(errno));
            close(ser_fd);
            return CUPS_BACKEND_FAILED;
        }
    } else {
        input_fd = 0;  /* stdin */
    }

    /* Send the job */
    int result = send_job(ser_fd, input_fd);

    /* Final status check */
    usleep(500000);
    status = query_status(ser_fd);
    if (status > 0) {
        const struct esim_error *err = lookup_error(status);
        if (err)
            log_warn("Post-job status: %02d — %s", status, err->desc);
    }

    if (argc == 7)
        close(input_fd);
    close(ser_fd);

    return result;
}
