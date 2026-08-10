#!/usr/bin/env bash
# Find the board and flash it.
#
# Everything this does can be typed by hand — it exists because the two things
# that go wrong are always the same two, and both are easy to mistake for a
# broken board:
#
#   1. The port name. USB Serial/JTAG shows up as /dev/cu.usbmodem*, a serial
#      bridge as /dev/cu.usbserial-* or /dev/cu.wchusbserial*, and which one
#      this board enumerates as depends on the cable and the carrier.
#   2. The device node appears a moment before the CDC endpoint will accept a
#      connection. Flashing immediately fails with a serial-open error perhaps
#      half the time, which reads exactly like a board that will not flash.
#
# Usage:
#   tools/flash.sh                 # find the port, flash, then monitor
#   tools/flash.sh --no-monitor    # flash and exit (for scripts)
#   PORT=/dev/cu.usbmodem101 tools/flash.sh    # skip detection
#
# If it will not enter flash mode: hold BOOT, press RESET, release both, retry.
# Exit with Ctrl+] from the monitor.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MONITOR=1
[ "${1:-}" = "--no-monitor" ] && MONITOR=0

if ! command -v idf.py > /dev/null 2>&1; then
    # Activate the IDF environment if the caller has not. Same version the
    # project is documented against; a different one is the caller's business.
    EXPORT=~/esp/v5.4.3/esp-idf/export.sh
    if [ ! -f "$EXPORT" ]; then
        echo "idf.py not on PATH and $EXPORT not found." >&2
        echo "Run '. <your-idf>/export.sh' first — see docs/esp-idf-development.md." >&2
        exit 1
    fi
    # shellcheck disable=SC1090
    . "$EXPORT" > /dev/null 2>&1
fi

PORT="${PORT:-}"
if [ -z "$PORT" ]; then
    for p in /dev/cu.usbmodem* /dev/cu.usbserial-* /dev/cu.wchusbserial*; do
        [ -e "$p" ] || continue
        PORT="$p"
        break
    done
fi

if [ -z "$PORT" ]; then
    echo "No board found." >&2
    echo "Looked for: /dev/cu.usbmodem*  /dev/cu.usbserial-*  /dev/cu.wchusbserial*" >&2
    echo >&2
    echo "Check the USB-C cable actually carries data — a charge-only cable powers" >&2
    echo "the board (the panel will even light up) and enumerates nothing." >&2
    echo "Currently attached: $(ls /dev/cu.* 2>/dev/null | tr '\n' ' ')" >&2
    exit 2
fi

echo "found $PORT"
sleep 2        # let the CDC endpoint settle; see (2) above

cd "$ROOT" || exit 1
if [ "$MONITOR" = "1" ]; then
    exec idf.py -p "$PORT" flash monitor
else
    exec idf.py -p "$PORT" flash
fi
