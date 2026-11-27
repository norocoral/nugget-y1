#!/usr/bin/env bash
set -euo pipefail

APP="${1:-configapp}"
SD_ARG="${2:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

QEMU_BIN=""
for q in qemu-arm qemu-arm-static; do
    if command -v "$q" >/dev/null 2>&1; then
        QEMU_BIN="$q"
        break
    fi
done

if [ -z "$QEMU_BIN" ]; then
    echo "ERROR: qemu-arm / qemu-arm-static not found."
    echo "Install it in WSL with: sudo apt update && sudo apt install -y qemu-user"
    exit 1
fi

export Y1_EMU_DIR="/tmp/y1_emu"
export Y1_EMU_SDCARD="${SD_ARG:-${Y1_EMU_SDCARD:-$ROOT_DIR/dist/rockbox_sdcard}}"
mkdir -p "$Y1_EMU_DIR" "$Y1_EMU_SDCARD/.system/lib"
truncate -s $((480 * 360 * 4)) "$Y1_EMU_DIR/fb0"
[ -p "$Y1_EMU_DIR/input_fifo" ] || mkfifo "$Y1_EMU_DIR/input_fifo"
[ -p "$Y1_EMU_DIR/audio_fifo" ] || mkfifo "$Y1_EMU_DIR/audio_fifo"

echo "85" > "$Y1_EMU_DIR/bat_cap"
echo "Discharging" > "$Y1_EMU_DIR/bat_status"
echo "4050000" > "$Y1_EMU_DIR/bat_volt"
echo "0" > "$Y1_EMU_DIR/bat_curr"
echo "0" > "$Y1_EMU_DIR/usb_online"
echo "1" > "$Y1_EMU_DIR/h2w_state"
echo "165" > "$Y1_EMU_DIR/bl_brightness"
echo "1209000" > "$Y1_EMU_DIR/cpu_freq"
echo "performance" > "$Y1_EMU_DIR/cpu_gov"
echo "1" > "$Y1_EMU_DIR/cpu1_online"
echo "0-st7701_hvga_dsi_vdo_boe" > "$Y1_EMU_DIR/proc_lcm"
cat > "$Y1_EMU_DIR/meminfo" << 'EOF'
MemTotal:         494376 kB
MemFree:          412400 kB
MemAvailable:     438200 kB
Buffers:           12400 kB
Cached:            25800 kB
EOF
cat > "$Y1_EMU_DIR/cpuinfo" << 'EOF'
Processor	: ARMv7 Processor rev 3 (v7l)
processor	: 0
BogoMIPS	: 2408.25

processor	: 1
BogoMIPS	: 2408.25

Features	: swp half thumb fastmult vfp edsp thumbee neon vfpv3 tls vfpv4 idiva idivt
CPU implementer	: 0x41
CPU architecture: 7
CPU variant	: 0x0
CPU part	: 0xc07
CPU revision	: 3

Hardware	: MT6572
Revision	: 0000
Serial		: 0000000000000000
EOF

SHIM_SO="$Y1_EMU_DIR/liby1_emu_shim.so"
arm-linux-gnueabihf-gcc -O2 -shared -fPIC "$SCRIPT_DIR/y1_emu_shim.c" -o "$SHIM_SO" -ldl

case "$APP" in
    configapp) TARGET="$ROOT_DIR/configapp/build/configapp" ;;
    meui)      TARGET="$ROOT_DIR/meui/build/meui" ;;
    rockbox)
        if [ -x "$Y1_EMU_SDCARD/.rockbox/rockbox" ]; then
            TARGET="$Y1_EMU_SDCARD/.rockbox/rockbox"
        else
            TARGET="$ROOT_DIR/../../!rockbox_fromsource/rockbox-master/rockbox-master/build_y1/rockbox"
        fi
        ;;
    *)         TARGET="$APP" ;;
esac

if [ ! -x "$TARGET" ]; then
    echo "ERROR: Binary not found: $TARGET"
    exit 1
fi

python3 "$SCRIPT_DIR/viewer.py" &
VIEWER_PID=$!

EMU_PID=""
cleanup() {
    [ -n "$EMU_PID" ] && kill -TERM "$EMU_PID" 2>/dev/null || true
    kill -TERM "$VIEWER_PID" 2>/dev/null || true
    sleep 0.2
    [ -n "$EMU_PID" ] && kill -KILL "$EMU_PID" 2>/dev/null || true
    kill -KILL "$VIEWER_PID" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

echo "SD card emulated at: $Y1_EMU_SDCARD"
echo "Open http://127.0.0.1:8765 in your browser. Press Ctrl+C to quit."
"$QEMU_BIN" -L /usr/arm-linux-gnueabihf \
    -E "LD_PRELOAD=$SHIM_SO" \
    -E "LD_LIBRARY_PATH=$Y1_EMU_SDCARD/.system/lib:$ROOT_DIR/liby1/build:/usr/arm-linux-gnueabihf/lib" \
    -E "Y1_EMU_DIR=$Y1_EMU_DIR" \
    -E "Y1_EMU_SDCARD=$Y1_EMU_SDCARD" \
    "$TARGET" &
EMU_PID=$!
wait "$EMU_PID"
