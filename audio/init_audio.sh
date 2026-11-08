#!/bin/sh
# init_audio.sh - Innioasis Y1 Audio Initialisation Script
# Sets up /dev/eac and ALSA mixer switches before launching Rockbox

echo "[init_audio] Starting Innioasis Y1 audio routing..."

# 1. Ensure /dev/eac node exists
if [ ! -e /dev/eac ]; then
    if [ -f /sys/class/misc/eac/dev ]; then
        IFS=: read MAJOR MINOR < /sys/class/misc/eac/dev
        mknod /dev/eac c "$MAJOR" "$MINOR" 2>/dev/null
    else
        mknod /dev/eac c 10 255 2>/dev/null
    fi
fi

# 2. Hardware unmute via /dev/eac ioctl
if [ -x /bin/unmute_eac ]; then
    /bin/unmute_eac
elif [ -x /usr/bin/unmute_eac ]; then
    /usr/bin/unmute_eac
else
    echo "[init_audio] unmute_eac binary not found, relying on ALSA / tinymix"
fi

# 3. ALSA / TinyALSA mixer settings (if kernel exposes ALSA controls)
if command -v tinymix >/dev/null 2>&1; then
    echo "[init_audio] Configuring TinyALSA controls..."
    tinymix "Audio_Amp_L_Switch" 1 2>/dev/null || true
    tinymix "Audio_Amp_R_Switch" 1 2>/dev/null || true
    tinymix "Audio_PGA1_Setting" 4 2>/dev/null || true
    tinymix "Audio_PGA2_Setting" 4 2>/dev/null || true
    tinymix "Headphone_Volume" 28 2>/dev/null || true
fi

if command -v amixer >/dev/null 2>&1; then
    echo "[init_audio] Configuring ALSA controls via amixer..."
    amixer sset "Headphone" 80% unmute 2>/dev/null || true
    amixer sset "Audio_Amp_L" 1 2>/dev/null || true
    amixer sset "Audio_Amp_R" 1 2>/dev/null || true
    amixer sset "Headphone Volume" 28 2>/dev/null || true
fi

# 4. Report audio status
if [ -f /proc/asound/cards ]; then
    echo "[init_audio] ALSA Soundcards detected:"
    cat /proc/asound/cards
else
    echo "[init_audio] Note: Direct /dev/eac hardware audio in use."
fi

echo "[init_audio] Audio initialisation complete."

