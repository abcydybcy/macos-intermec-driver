#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- C build (native CUPS filter) ---
echo "Building rastertointermec (C)..."

cc -O2 -Wall -Wextra \
    -o "$SCRIPT_DIR/dist/rastertointermec" \
    "$SCRIPT_DIR/rastertointermec.c" \
    -lcups -lm

# --- tcpserial bridge ---
echo "Building tcpserial..."

cc -O2 -Wall -Wextra \
    -o "$SCRIPT_DIR/dist/tcpserial" \
    "$SCRIPT_DIR/tcpserial.c"

# --- CUPS serial backend ---
echo "Building intserial backend..."

cc -O2 -Wall -Wextra \
    -o "$SCRIPT_DIR/dist/intserial" \
    "$SCRIPT_DIR/serial.c"

echo ""
echo "Build complete."
echo "  Filter:  $SCRIPT_DIR/dist/rastertointermec"
echo "  Bridge:  $SCRIPT_DIR/dist/tcpserial"
echo "  Backend: $SCRIPT_DIR/dist/intserial"
echo ""
echo "Install (macOS):"
echo ""
echo "  sudo cp $SCRIPT_DIR/dist/rastertointermec /usr/libexec/cups/filter/rastertointermec"
echo "  sudo cp $SCRIPT_DIR/dist/intserial /usr/libexec/cups/backend/intserial"
echo "  sudo chown root:_lp /usr/libexec/cups/filter/rastertointermec /usr/libexec/cups/backend/intserial"
echo "  sudo chmod 755 /usr/libexec/cups/filter/rastertointermec"
echo "  sudo chmod 700 /usr/libexec/cups/backend/intserial"
echo ""
echo "Then reconfigure the printer to use URI: intserial:/dev/cu.usbserial-110?baud=9600"
