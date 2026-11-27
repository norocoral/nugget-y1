#!/usr/bin/env bash
# build.sh - Modular Build System for Innioasis Y1 Linux & liby1 SDK / Rockbox
# Supports standalone C development via liby1, modular component builds,
# interactive Terminal GUI (--gui), and full Rockbox + SD (.system & .rockbox) packaging.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_FILE="$SCRIPT_DIR/build.conf"

# ---------------------------------------------------------------------------
# Default configuration (overridden by build.conf and CLI flags)
# ---------------------------------------------------------------------------
BUILD_LIBY1=1
BUILD_ROOTFS=1
BUILD_BOOTIMG=1
BUILD_VIDEO_LIB=1
BUILD_ROCKBOX=1
BUILD_MEUI=1
BUILD_CONFIGAPP=1
DEFAULT_PLAYER="${DEFAULT_PLAYER:-}"
PACKAGE_SDCARD=1
BUILD_DIST=0
BUILD_DIST_IUCE=0
DIST_VERSION="${DIST_VERSION:-}"
BUNDLE_BT_BLOBS=1
BUNDLE_ANDROID_RAMDISK=1
CUSTOM_APP_BIN=""
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
USE_CCACHE=1
JOBS="${JOBS:-0}"

if [[ -f "$CONF_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$CONF_FILE"
fi

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Innioasis Y1 Modular Build System (liby1 Hardware Layer + Linux Boot + Rockbox)

Options:
  --gui, --tui, --menu, menuconfig   Launch the interactive Terminal GUI configurator
  --dist                             Build & assemble complete GitHub release distribution into dist/
  --dist-iuce                        Same as --dist, plus package dist/rom.zip for Innioasis Updater CE
  --only-liby1                       Build only liby1 (liby1.a, liby1.so, hello_y1)
  --only-rootfs                      Build only liby1 and initramfs rootfs utilities
  --only-bootimg                     Build only liby1, rootfs, and boot_nugget.img
  --only-video                       Build only liby1video.so
  --only-rockbox                     Build only Rockbox (rockbox + rockbox.zip)
  --only-meui                        Build only liby1 + MeUI standalone player
  --only-configapp                   Build only liby1 + ConfigApp
  --no-rockbox                       Build standalone Y1 SDK & boot image without Rockbox
  --no-meui                          Skip building/staging MeUI
  --default-player <name>            Set default_player (rockbox, meui, configapp)
  --custom-app <path>                Bundle custom ARM binary at /.system/app on SD card
  -j, --jobs <N>                     Number of parallel build jobs (0 = auto-detect)
  -h, --help                         Show this help message
EOF
}

LAUNCH_TUI=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --gui|--tui|--menu|configure|menuconfig)
            LAUNCH_TUI=1
            shift
            ;;
        --dist)
            BUILD_DIST=1
            BUILD_ROCKBOX=1
            shift
            ;;
        --dist-iuce)
            BUILD_DIST=1
            BUILD_DIST_IUCE=1
            BUILD_ROCKBOX=1
            shift
            ;;
        --version)
            DIST_VERSION="${2:-}"
            shift 2
            ;;
        --only-liby1)
            BUILD_LIBY1=1
            BUILD_ROOTFS=0
            BUILD_BOOTIMG=0
            BUILD_VIDEO_LIB=0
            BUILD_ROCKBOX=0
            PACKAGE_SDCARD=0
            shift
            ;;
        --only-rootfs)
            BUILD_LIBY1=1
            BUILD_ROOTFS=1
            BUILD_BOOTIMG=0
            BUILD_VIDEO_LIB=0
            BUILD_ROCKBOX=0
            PACKAGE_SDCARD=0
            shift
            ;;
        --only-bootimg)
            BUILD_LIBY1=1
            BUILD_ROOTFS=1
            BUILD_BOOTIMG=1
            BUILD_VIDEO_LIB=0
            BUILD_ROCKBOX=0
            PACKAGE_SDCARD=0
            shift
            ;;
        --only-video)
            BUILD_LIBY1=0
            BUILD_ROOTFS=0
            BUILD_BOOTIMG=0
            BUILD_VIDEO_LIB=1
            BUILD_ROCKBOX=0
            PACKAGE_SDCARD=0
            shift
            ;;
        --only-rockbox)
            BUILD_LIBY1=0
            BUILD_ROOTFS=0
            BUILD_BOOTIMG=0
            BUILD_VIDEO_LIB=0
            BUILD_ROCKBOX=1
            PACKAGE_SDCARD=0
            shift
            ;;
        --only-meui)
            BUILD_LIBY1=1
            BUILD_ROOTFS=0
            BUILD_BOOTIMG=0
            BUILD_VIDEO_LIB=0
            BUILD_ROCKBOX=0
            BUILD_MEUI=1
            BUILD_CONFIGAPP=0
            PACKAGE_SDCARD=0
            shift
            ;;
        --only-configapp)
            BUILD_LIBY1=1
            BUILD_ROOTFS=0
            BUILD_BOOTIMG=0
            BUILD_VIDEO_LIB=0
            BUILD_ROCKBOX=0
            BUILD_MEUI=0
            BUILD_CONFIGAPP=1
            PACKAGE_SDCARD=0
            shift
            ;;
        --no-rockbox)
            BUILD_ROCKBOX=0
            shift
            ;;
        --no-meui)
            BUILD_MEUI=0
            shift
            ;;
        --default-player)
            DEFAULT_PLAYER="${2:-}"
            shift 2
            ;;
        --custom-app)
            CUSTOM_APP_BIN="${2:-}"
            shift 2
            ;;
        -j|--jobs)
            JOBS="${2:-0}"
            shift 2
            ;;
        -j[0-9]*)
            JOBS="${1#-j}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[!] Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

if [[ "$LAUNCH_TUI" -eq 1 ]]; then
    set +e
    python3 "$SCRIPT_DIR/tools/build_tui.py" "$CONF_FILE"
    TUI_RC=$?
    set -e
    if [[ "$TUI_RC" -eq 3 ]]; then
        BUILD_DIST=1
    elif [[ "$TUI_RC" -ne 0 ]]; then
        echo "[*] Configuration session ended without starting a build."
        exit 0
    fi
    if [[ -f "$CONF_FILE" ]]; then
        # shellcheck disable=SC1090
        source "$CONF_FILE"
    fi
fi

# ---------------------------------------------------------------------------
# Workspace detection
# ---------------------------------------------------------------------------

WORKSPACE_ROOT="${WORKSPACE_ROOT:-}"

if [[ -z "$WORKSPACE_ROOT" ]]; then
    if [[ -d "$SCRIPT_DIR/../../!rockbox_fromsource" ]]; then
        WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
    elif [[ -d "$SCRIPT_DIR/../!rockbox_fromsource" ]]; then
        WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
    else
        search_dir="$SCRIPT_DIR"
        while :; do
            if [[ -d "$search_dir/!stock_rom" || -d "$search_dir/!rockbox_fromsource" || -d "$search_dir/!nugget" ]]; then
                WORKSPACE_ROOT="$search_dir"
                break
            fi
            parent_dir="$(dirname "$search_dir")"
            [[ "$parent_dir" == "$search_dir" ]] && break
            search_dir="$parent_dir"
        done
    fi
fi

if [[ -z "${ROCKBOX_SRC:-}" ]]; then
    if [[ -n "$WORKSPACE_ROOT" && -d "$WORKSPACE_ROOT/!rockbox_fromsource/rockbox-master/rockbox-master" ]]; then
        ROCKBOX_SRC="$WORKSPACE_ROOT/!rockbox_fromsource/rockbox-master/rockbox-master"
    elif [[ -f "$SCRIPT_DIR/../rockbox/tools/configure" ]]; then
        ROCKBOX_SRC="$(cd "$SCRIPT_DIR/../rockbox" && pwd)"
    elif [[ -f "$SCRIPT_DIR/rockbox/tools/configure" ]]; then
        ROCKBOX_SRC="$SCRIPT_DIR/rockbox"
    elif [[ -f "$SCRIPT_DIR/../tools/configure" ]]; then
        ROCKBOX_SRC="$(cd "$SCRIPT_DIR/.." && pwd)"
    elif [[ -f "$SCRIPT_DIR/tools/configure" ]]; then
        ROCKBOX_SRC="$SCRIPT_DIR"
    elif [[ -n "$WORKSPACE_ROOT" && -d "$WORKSPACE_ROOT/rockbox-master" ]]; then
        ROCKBOX_SRC="$WORKSPACE_ROOT/rockbox-master"
    else
        ROCKBOX_SRC="$(cd "$SCRIPT_DIR/.." && pwd)"
    fi
fi

ROOTFS_DIR="$SCRIPT_DIR/rootfs"
if [[ -z "${FORCE_IN_PLACE_BUILD:-}" && "$ROCKBOX_SRC" == /mnt/* && "${HOME:-}" != /mnt/* && -n "${HOME:-}" ]]; then
    WSL_EXT4_CACHE="$HOME/.cache/y1_build"
    ROCKBOX_WORK_SRC="$WSL_EXT4_CACHE/rockbox_src"
    BUILD_DIR="$WSL_EXT4_CACHE/build_y1"
else
    ROCKBOX_WORK_SRC="$ROCKBOX_SRC"
    BUILD_DIR="$ROCKBOX_SRC/build_y1"
fi
SPLASH_DIR="$SCRIPT_DIR/splash"

if [[ -z "${STOCK_BOOT:-}" ]]; then
    if [[ -f "$SCRIPT_DIR/stock_rom/boot.img" ]]; then
        STOCK_BOOT="$SCRIPT_DIR/stock_rom/boot.img"
    elif [[ -f "${WORKSPACE_ROOT:-$SCRIPT_DIR}/!stock_rom/boot.img" ]]; then
        STOCK_BOOT="${WORKSPACE_ROOT:-$SCRIPT_DIR}/!stock_rom/boot.img"
    elif [[ -f "$SCRIPT_DIR/boot.img" ]]; then
        STOCK_BOOT="$SCRIPT_DIR/boot.img"
    fi
fi

# Use ccache when enabled and installed.
if [[ "${USE_CCACHE:-1}" -eq 1 ]] && command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-5G}"
    export CCACHE_COMPILERCHECK=content
    export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-time_macros,include_file_mtime,include_file_ctime}"

    CC=(ccache "${CROSS_COMPILE}gcc")
else
    CC=("${CROSS_COMPILE}gcc")
fi

STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"

MAX_BOOT_SIZE=6291456 # 6 MiB

# ---------------------------------------------------------------------------
# Parallel build configuration
# ---------------------------------------------------------------------------

if command -v nproc >/dev/null 2>&1; then
    DEFAULT_JOBS="$(( $(nproc) * 2 ))"
elif command -v getconf >/dev/null 2>&1; then
    DEFAULT_JOBS="$(( $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2) * 2 ))"
else
    DEFAULT_JOBS=4
fi

if [[ -z "${JOBS:-}" || "$JOBS" == "0" ]]; then
    JOBS="$DEFAULT_JOBS"
fi

if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "[!] JOBS must be a positive integer (got: $JOBS)"
    exit 1
fi

# Avoid exporting MAKEFLAGS that could override explicit make options.
unset MAKEFLAGS 2>/dev/null || true

# ---------------------------------------------------------------------------
# Temporary files and cleanup
# ---------------------------------------------------------------------------

SD_STAGE=""

cleanup() {
    if [[ -n "$SD_STAGE" && -d "$SD_STAGE" ]]; then
        rm -rf -- "$SD_STAGE"
    fi
}

trap cleanup EXIT

# ---------------------------------------------------------------------------
# Build information & prerequisite validation
# ---------------------------------------------------------------------------

if [[ "${BUILD_DIST:-0}" -eq 1 && -z "${DIST_VERSION:-}" ]]; then
    if [[ -t 0 ]]; then
        read -rp "Enter distribution version number: " DIST_VERSION
    fi
    if [[ -z "${DIST_VERSION:-}" ]]; then
        echo "[!] Error: Version number is required when building for --dist (pass --version <num> or DIST_VERSION)."
        exit 1
    fi
fi
export DIST_VERSION BUILD_DIST_IUCE

echo "=========================================================="
echo " Innioasis Y1 Modular Build System"
echo "=========================================================="
if [[ -n "${DIST_VERSION:-}" ]]; then
    echo "Dist Version:   $DIST_VERSION"
fi
echo "Toolchain:      ${CC[*]}"
echo "Parallelism:    $JOBS jobs"
echo "Components:     liby1=$BUILD_LIBY1 rootfs=$BUILD_ROOTFS bootimg=$BUILD_BOOTIMG video=$BUILD_VIDEO_LIB rockbox=$BUILD_ROCKBOX sdcard=$PACKAGE_SDCARD"
if [[ -n "$CUSTOM_APP_BIN" ]]; then
    echo "Custom App:     $CUSTOM_APP_BIN -> /.system/app"
fi
echo "=========================================================="

if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "[!] Cross-compiler not found: ${CROSS_COMPILE}gcc"
    exit 1
fi

if ! command -v "$STRIP" >/dev/null 2>&1; then
    echo "[!] Cross-compiler strip not found: $STRIP"
    exit 1
fi

if ! command -v "$READELF" >/dev/null 2>&1; then
    echo "[!] Cross-compiler readelf not found: $READELF"
    exit 1
fi

if [[ "$BUILD_ROCKBOX" -eq 1 && ! -f "$ROCKBOX_SRC/tools/configure" ]]; then
    echo "[!] Rockbox configure script not found: $ROCKBOX_SRC/tools/configure"
    echo "    Run with --no-rockbox or set BUILD_ROCKBOX=0 in build.conf for standalone builds."
    exit 1
fi

# ---------------------------------------------------------------------------
# Step 1: Build liby1 Hardware Abstraction Library
# ---------------------------------------------------------------------------

if [[ "$BUILD_LIBY1" -eq 1 ]]; then
    echo "[1/6] Building liby1 Hardware Layer (liby1.a, liby1.so, hello_y1)..."
    make -C "$SCRIPT_DIR/liby1" \
        CROSS_COMPILE="$CROSS_COMPILE" \
        -j"$JOBS"
else
    echo "[1/6] Skipping liby1 build (BUILD_LIBY1=0)."
fi

# ---------------------------------------------------------------------------
# Step 2: Set up root filesystem, splash screens, and native utilities
# ---------------------------------------------------------------------------

if [[ "$BUILD_ROOTFS" -eq 1 ]]; then
    echo "[2/6] Setting up root filesystem and native utilities..."

    python3 "$SCRIPT_DIR/tools/setup_rootfs.py"

    rm -rf "$ROOTFS_DIR/.rockbox" "$ROOTFS_DIR/.system"
    rm -f "$ROOTFS_DIR/rockbox"
    rm -f "$ROOTFS_DIR"/etc/*.png

    mkdir -p "$ROOTFS_DIR/etc" "$ROOTFS_DIR/bin"

    # Convert splash and status images.
    for img in boot sdcard connect errconnect error; do
        SRC_IMG=""

        if [[ "$img" == "boot" && -f "$SPLASH_DIR/bootorg.png" ]]; then
            SRC_IMG="$SPLASH_DIR/bootorg.png"
        elif [[ -f "$SPLASH_DIR/$img.png" ]]; then
            SRC_IMG="$SPLASH_DIR/$img.png"
        elif [[ -n "$WORKSPACE_ROOT" && -f "$WORKSPACE_ROOT/$img.png" ]]; then
            SRC_IMG="$WORKSPACE_ROOT/$img.png"
        elif [[ -f "$ROCKBOX_SRC/$img.png" ]]; then
            SRC_IMG="$ROCKBOX_SRC/$img.png"
        fi

        if [[ -n "$SRC_IMG" ]]; then
            if [[ "$img" == "boot" && -n "${DIST_VERSION:-}" ]]; then
                MFG_DATE="$(date +%Y.%m.%d)"
                echo "[*] Stamping boot.png with MFG $MFG_DATE / VER $DIST_VERSION..."
                python3 "$SCRIPT_DIR/tools/convert_splash.py" \
                    --overlay "MFG $MFG_DATE" \
                    --overlay "VER $DIST_VERSION" \
                    "$SRC_IMG" "$ROOTFS_DIR/etc/$img.raw"
            elif [[ ! -s "$ROOTFS_DIR/etc/$img.raw" || "$SRC_IMG" -nt "$ROOTFS_DIR/etc/$img.raw" ]]; then
                echo "[*] Converting $SRC_IMG..."
                python3 "$SCRIPT_DIR/tools/convert_splash.py" \
                    "$SRC_IMG" "$ROOTFS_DIR/etc/$img.raw"
            fi
        fi
    done

    # Generate bootloader menu screen assets (Pick1..7 for SD .system, USB/Terminal for rootfs)
    if [[ -d "$SCRIPT_DIR/bootl_image_assets" ]]; then
        BOOTL_STAMP="$SCRIPT_DIR/bootl_raw/.stamp"
        NEED_BOOTL=0
        if [[ ! -f "$BOOTL_STAMP" || ! -s "$ROOTFS_DIR/etc/terminal.raw" ]]; then
            NEED_BOOTL=1
        elif [[ -n "$(find "$SCRIPT_DIR/bootl_image_assets" -type f -newer "$BOOTL_STAMP" -print -quit 2>/dev/null)" ]]; then
            NEED_BOOTL=1
        fi
        if [[ "$NEED_BOOTL" -eq 1 ]]; then
            echo "[*] Generating bootloader menu screen assets..."
            rm -f "$ROOTFS_DIR/etc/bootl_pick"*.raw "$ROOTFS_DIR/etc/terminal.raw" "$SCRIPT_DIR/bootl_raw/bootl_pick"*.raw
            mkdir -p "$SCRIPT_DIR/bootl_raw"
            python3 "$SCRIPT_DIR/tools/generate_bootl_assets.py" \
                "$SCRIPT_DIR/bootl_image_assets" \
                "$ROOTFS_DIR/etc" \
                "$SCRIPT_DIR/bootl_raw"
            touch "$BOOTL_STAMP"
        fi
    fi

    echo "[*] Compiling native utilities with parallel jobs..."

    NUGGET_BUILD_DATE="$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    NUGGET_GIT_REV="$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    "${CC[@]}" -O2 -Wall \
        -DNUGGET_BUILD_DATE="\"$NUGGET_BUILD_DATE\"" \
        -DNUGGET_GIT_REV="\"$NUGGET_GIT_REV\"" \
        "$SCRIPT_DIR/initramfs/init.c" \
        -o "$ROOTFS_DIR/init" &
    INIT_PID=$!

    "${CC[@]}" -O2 \
        "$SCRIPT_DIR/audio/unmute_eac.c" \
        -o "$ROOTFS_DIR/bin/unmute_eac" &
    AUDIO_PID=$!

    "${CC[@]}" -O2 -Wall -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -DY1_IPOD_SYNC_MAIN \
        -I"$SCRIPT_DIR/liby1/include" -I"$SCRIPT_DIR/liby1/src" \
        "$SCRIPT_DIR/liby1/src/y1_core.c" \
        "$SCRIPT_DIR/liby1/src/y1_display.c" \
        "$SCRIPT_DIR/liby1/src/y1_cpu.c" \
        "$SCRIPT_DIR/liby1/src/y1_input.c" \
        "$SCRIPT_DIR/liby1/src/y1_audio.c" \
        "$SCRIPT_DIR/liby1/src/y1_tuner.c" \
        "$SCRIPT_DIR/liby1/src/y1_usb.c" \
        "$SCRIPT_DIR/liby1/src/y1_ipod_sync.c" \
        -lm \
        -o "$ROOTFS_DIR/bin/ipod_sync" &
    IPOD_PID=$!

    BUILD_FAILED=0

    wait "$INIT_PID" || BUILD_FAILED=1
    wait "$AUDIO_PID" || BUILD_FAILED=1
    wait "$IPOD_PID" || BUILD_FAILED=1

    if [[ "$BUILD_FAILED" -ne 0 ]]; then
        echo "[!] Native utility compilation failed."
        exit 1
    fi

    # Strip native binaries concurrently.
    "$STRIP" "$ROOTFS_DIR/init" &
    STRIP_INIT_PID=$!

    "$STRIP" "$ROOTFS_DIR/bin/unmute_eac" &
    STRIP_AUDIO_PID=$!

    "$STRIP" "$ROOTFS_DIR/bin/ipod_sync" &
    STRIP_IPOD_PID=$!

    BUILD_FAILED=0
    wait "$STRIP_INIT_PID" || BUILD_FAILED=1
    wait "$STRIP_AUDIO_PID" || BUILD_FAILED=1
    wait "$STRIP_IPOD_PID" || BUILD_FAILED=1

    if [[ "$BUILD_FAILED" -ne 0 ]]; then
        echo "[!] Failed to strip native utilities."
        exit 1
    fi

    chmod +x "$ROOTFS_DIR/init" "$ROOTFS_DIR/bin/unmute_eac" "$ROOTFS_DIR/bin/ipod_sync"

    echo "[*] Setting up BusyBox rescue shell..."

    if [[ ! -s "$ROOTFS_DIR/bin/busybox" ]]; then
        echo "[*] Looking for static ARM BusyBox..."

        if [[ -f "$SCRIPT_DIR/tools/busybox-armv7l" ]]; then
            cp "$SCRIPT_DIR/tools/busybox-armv7l" "$ROOTFS_DIR/bin/busybox"
        elif [[ -n "$WORKSPACE_ROOT" && -f "$WORKSPACE_ROOT/busybox-armv7l" ]]; then
            cp "$WORKSPACE_ROOT/busybox-armv7l" "$ROOTFS_DIR/bin/busybox"
        elif command -v busybox-armv7l >/dev/null 2>&1; then
            cp "$(command -v busybox-armv7l)" "$ROOTFS_DIR/bin/busybox"
        else
            echo "[*] Downloading static ARM BusyBox..."

            if ! command -v curl >/dev/null 2>&1; then
                echo "[!] curl is required to download BusyBox."
                exit 1
            fi

            BUSYBOX_URL="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-armv7l"
            BUSYBOX_TMP="$ROOTFS_DIR/bin/busybox.download"

            if curl -fL --retry 2 --connect-timeout 15 \
                "$BUSYBOX_URL" -o "$BUSYBOX_TMP" &&
                [[ -s "$BUSYBOX_TMP" ]]; then
                mv "$BUSYBOX_TMP" "$ROOTFS_DIR/bin/busybox"
            else
                rm -f "$BUSYBOX_TMP"
                echo "[!] BusyBox download failed."
                echo "    Install a static ARM BusyBox at tools/busybox-armv7l."
                exit 1
            fi
        fi
    fi

    if [[ -s "$ROOTFS_DIR/bin/busybox" ]]; then
        chmod +x "$ROOTFS_DIR/bin/busybox"

        ln -sf busybox "$ROOTFS_DIR/bin/sh"

        echo "[*] Installing BusyBox applet symlinks (ls, top, ps, cat, etc.)..."
        python3 -c '
import os, sys, re
bb_path = sys.argv[1]
bin_dir = sys.argv[2]
d = open(bb_path, "rb").read()
bb_size = len(d)
candidates = re.findall(b"(?:[a-z0-9_.-]{1,20}\x00){15,}", d)
for c in candidates:
    names = [x.decode("ascii") for x in c.split(b"\x00") if x]
    if "ls" in names and "cat" in names and "grep" in names:
        for name in names:
            link_path = os.path.join(bin_dir, name)
            if os.path.lexists(link_path):
                if not os.path.islink(link_path) and os.path.getsize(link_path) == bb_size:
                    os.remove(link_path)
            if not os.path.lexists(link_path):
                os.symlink("busybox", link_path)
        print(f"[+] Installed {len(names)} BusyBox applet symlinks in /bin")
        break
' "$ROOTFS_DIR/bin/busybox" "$ROOTFS_DIR/bin"

        mkdir -p "$ROOTFS_DIR/sbin"
        ln -sf /bin/busybox "$ROOTFS_DIR/sbin/poweroff"
        ln -sf /bin/busybox "$ROOTFS_DIR/sbin/reboot"
    fi

    echo "[*] Setting up ARM runtime libraries..."

    mkdir -p "$ROOTFS_DIR/lib"

    rm -f \
        "$ROOTFS_DIR"/lib/libm* \
        "$ROOTFS_DIR"/lib/libdl* \
        "$ROOTFS_DIR"/lib/libpthread* \
        "$ROOTFS_DIR"/lib/librt*

    find "$ROOTFS_DIR/lib" -mindepth 1 -maxdepth 1 -type f -delete

    for lib in ld-linux-armhf.so.3 libc.so.6; do
        LIB_SOURCE=""
        if [[ -d /usr/arm-linux-gnueabihf/lib ]]; then
            LIB_SOURCE="$(
                find /usr/arm-linux-gnueabihf/lib \
                    -name "$lib" -type f -print -quit 2>/dev/null || true
            )"
        fi

        if [[ -z "$LIB_SOURCE" || ! -s "$LIB_SOURCE" ]]; then
            echo "[!] Required ARM runtime library not found or empty: $lib"
            exit 1
        fi

        LIB_TMP="$ROOTFS_DIR/lib/$lib.tmp.$$"
        cp -L "$LIB_SOURCE" "$LIB_TMP"
        if [[ ! -s "$LIB_TMP" ]]; then
            echo "[!] Failed to copy ARM runtime library: $lib"
            rm -f "$LIB_TMP"
            exit 1
        fi

        if ! "$STRIP" --strip-unneeded "$LIB_TMP"; then
            echo "[!] Failed to strip ARM runtime library: $lib"
            rm -f "$LIB_TMP"
            exit 1
        fi

        if [[ ! -s "$LIB_TMP" ]]; then
            echo "[!] Stripping produced an empty ARM runtime library: $lib"
            rm -f "$LIB_TMP"
            exit 1
        fi

        mv -f "$LIB_TMP" "$ROOTFS_DIR/lib/$lib"
    done

    if [[ "${BUNDLE_BT_BLOBS:-1}" -eq 1 && -d "$SCRIPT_DIR/bt_blobs" ]]; then
        echo "[*] Bundling Bluetooth modules and firmware into rootfs..."

        mkdir -p "$ROOTFS_DIR/lib/modules" "$ROOTFS_DIR/etc/firmware"

        for file in mtk_stp_wmt.ko mtk_stp_bt.ko; do
            if [[ -f "$SCRIPT_DIR/bt_blobs/$file" ]]; then
                cp "$SCRIPT_DIR/bt_blobs/$file" "$ROOTFS_DIR/lib/modules/"
            fi
        done

        for file in mt6572_82_patch_e1_0_hdr.bin WMT_SOC.cfg; do
            if [[ -f "$SCRIPT_DIR/bt_blobs/$file" ]]; then
                cp "$SCRIPT_DIR/bt_blobs/$file" "$ROOTFS_DIR/etc/firmware/"
            fi
        done
    fi
else
    echo "[2/6] Skipping rootfs setup (BUILD_ROOTFS=0)."
fi

# ---------------------------------------------------------------------------
# Step 3: Configure and build Rockbox (optional, decoupled)
# ---------------------------------------------------------------------------

if [[ "$BUILD_ROCKBOX" -eq 1 ]]; then
    echo "[3/6] Configuring and compiling Rockbox (with liby1)..."

    if [[ "$ROCKBOX_WORK_SRC" != "$ROCKBOX_SRC" ]]; then
        echo "[*] Syncing Rockbox source to native Linux ext4 cache ($ROCKBOX_WORK_SRC)..."
        mkdir -p "$ROCKBOX_WORK_SRC"
        if command -v rsync >/dev/null 2>&1; then
            rsync -a --delete --exclude='build_y1/' --exclude='.git/' \
                "$ROCKBOX_SRC/" "$ROCKBOX_WORK_SRC/"
        else
            cp -ru "$ROCKBOX_SRC/." "$ROCKBOX_WORK_SRC/"
        fi
    fi

    if [[ -d "$SCRIPT_DIR/liby1/src" ]]; then
        mkdir -p "$ROCKBOX_WORK_SRC/vendor/liby1/include" "$ROCKBOX_WORK_SRC/vendor/liby1/src"
        cp -ru "$SCRIPT_DIR/liby1/include/"* "$ROCKBOX_WORK_SRC/vendor/liby1/include/" 2>/dev/null || cp -rf "$SCRIPT_DIR/liby1/include/"* "$ROCKBOX_WORK_SRC/vendor/liby1/include/"
        cp -ru "$SCRIPT_DIR/liby1/src/"* "$ROCKBOX_WORK_SRC/vendor/liby1/src/" 2>/dev/null || cp -rf "$SCRIPT_DIR/liby1/src/"* "$ROCKBOX_WORK_SRC/vendor/liby1/src/"
        if [[ -d "$SCRIPT_DIR/liby1/third_party" ]]; then
            mkdir -p "$ROCKBOX_WORK_SRC/vendor/liby1/third_party"
            cp -ru "$SCRIPT_DIR/liby1/third_party/"* "$ROCKBOX_WORK_SRC/vendor/liby1/third_party/" 2>/dev/null || cp -rf "$SCRIPT_DIR/liby1/third_party/"* "$ROCKBOX_WORK_SRC/vendor/liby1/third_party/"
        fi
        if [[ "$ROCKBOX_WORK_SRC" != "$ROCKBOX_SRC" && -d "$ROCKBOX_SRC/vendor/liby1" ]]; then
            cp -ru "$SCRIPT_DIR/liby1/include/"* "$ROCKBOX_SRC/vendor/liby1/include/" 2>/dev/null || true
            cp -ru "$SCRIPT_DIR/liby1/src/"* "$ROCKBOX_SRC/vendor/liby1/src/" 2>/dev/null || true
            if [[ -d "$SCRIPT_DIR/liby1/third_party" ]]; then
                mkdir -p "$ROCKBOX_SRC/vendor/liby1/third_party"
                cp -ru "$SCRIPT_DIR/liby1/third_party/"* "$ROCKBOX_SRC/vendor/liby1/third_party/" 2>/dev/null || true
            fi
        fi
    fi

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    [[ -x "$ROCKBOX_WORK_SRC/tools/configure" ]] || chmod +x "$ROCKBOX_WORK_SRC/tools/configure"

    if [[ -f Makefile ]] && ! grep -Fqx 'export SHARED_CFLAGS=-fPIC -fvisibility=hidden' Makefile; then
        echo "[*] Refreshing stale Y1 plugin build configuration..."
        make clean
        "$ROCKBOX_WORK_SRC/tools/configure" --target=310 --type=N
    elif [[ ! -f Makefile || "$ROCKBOX_WORK_SRC/tools/configure" -nt Makefile ]]; then
        "$ROCKBOX_WORK_SRC/tools/configure" --target=310 --type=N
    fi

    if [[ ! -f Makefile ]]; then
        echo "[!] Configuration did not generate a Makefile."
        exit 1
    fi

    find "$BUILD_DIR" -type f -name '*.o' -size -52c -delete 2>/dev/null || true

    make -j1 "$BUILD_DIR/apps/features"

    if [[ -f "$BUILD_DIR/apps/features" ]]; then
        tr '\n' ':' < "$BUILD_DIR/apps/features" > "$BUILD_DIR/apps/genlang-features.tmp"
        if [[ ! -f "$BUILD_DIR/apps/genlang-features" ]] || ! cmp -s "$BUILD_DIR/apps/genlang-features.tmp" "$BUILD_DIR/apps/genlang-features"; then
            echo "[*] Syncing genlang-features with updated device capabilities..."
            mv -f "$BUILD_DIR/apps/genlang-features.tmp" "$BUILD_DIR/apps/genlang-features"
            rm -f "$BUILD_DIR/lang/lang.h" "$BUILD_DIR/lang/lang_core.c" "$BUILD_DIR/lang_enum.h" "$BUILD_DIR/apps/lang/"*.lng "$BUILD_DIR/apps/lang/"*.vstrings
        else
            rm -f "$BUILD_DIR/apps/genlang-features.tmp"
        fi
    fi

    if [[ ! -f "$BUILD_DIR/lang/lang.h" ]]; then
        echo "[*] Generating language header before parallel compilation..."
        make -j1 "$BUILD_DIR/lang/lang.h"
    fi

    echo "[*] Building Rockbox with $JOBS parallel jobs..."
    if [[ -n "${DIST_VERSION:-}" ]]; then
        make -j"$JOBS" SVNVERSION="$DIST_VERSION"
        echo "[*] Creating Rockbox base archive (version $DIST_VERSION)..."
        make -j"$JOBS" zip SVNVERSION="$DIST_VERSION"
    else
        make -j"$JOBS"
        echo "[*] Creating Rockbox base archive..."
        make -j"$JOBS" zip
    fi

    if [[ ! -s "$BUILD_DIR/rockbox.zip" ]]; then
        echo "[!] Rockbox SD package was not generated."
        exit 1
    fi

    if [[ ! -s "$BUILD_DIR/rockbox" ]]; then
        echo "[!] Rockbox executable was not generated."
        exit 1
    fi

    cd "$SCRIPT_DIR"
else
    echo "[3/6] Skipping Rockbox compilation (BUILD_ROCKBOX=0)."
fi

# ---------------------------------------------------------------------------
# Step 4: Build hardware video helper library (liby1video.so)
# ---------------------------------------------------------------------------

VIDEO_SO_OUT="$SCRIPT_DIR/video/liby1video.so"

if [[ "$BUILD_VIDEO_LIB" -eq 1 ]]; then
    echo "[4/6] Building external SD card video library (liby1video.so)..."
    "${CC[@]}" -O3 -mfpu=neon -mfloat-abi=hard -fPIC -shared \
        -I"$SCRIPT_DIR/video" \
        -I"$SCRIPT_DIR/video/include" \
        -Wl,-rpath,/mnt/sdcard/.system/lib:/mnt/sdcard/.rockbox/lib \
        "$SCRIPT_DIR/video/liby1video.c" \
        "$SCRIPT_DIR"/video/h264_objs/libavcodec/*.o \
        "$SCRIPT_DIR"/video/h264_objs/libavutil/*.o \
        "$SCRIPT_DIR"/video/faad_objs/*.o \
        -o "$VIDEO_SO_OUT" \
        -ldl -lm
    "$STRIP" --strip-unneeded \
        -R .comment \
        -R .note.gnu.build-id \
        "$VIDEO_SO_OUT"
else
    echo "[4/6] Skipping liby1video.so build (BUILD_VIDEO_LIB=0)."
fi

# ---------------------------------------------------------------------------
# Step 5: Prepare SD card package (.system/ for system/drivers/SDK, .rockbox/ for Rockbox)
# ---------------------------------------------------------------------------

if [[ "$PACKAGE_SDCARD" -eq 1 ]]; then
    echo "[5/6] Preparing SD card package (.system/ & .rockbox/)..."

    SD_STAGE="$(mktemp -d "/tmp/y1-sdcard-stage.XXXXXX")"

    mkdir -p \
        "$SD_STAGE/.system/bt" \
        "$SD_STAGE/.system/lib" \
        "$SD_STAGE/.system/include" \
        "$SD_STAGE/.system/bin" \
        "$SD_STAGE/.system/video_cache" \
        "$SD_STAGE/Videos"

    # Set default player (auto-select meui when Rockbox is disabled)
    if [[ -z "$DEFAULT_PLAYER" ]]; then
        if [[ "$BUILD_ROCKBOX" -eq 1 ]]; then
            DEFAULT_PLAYER="rockbox"
        elif [[ "$BUILD_MEUI" -eq 1 ]]; then
            DEFAULT_PLAYER="meui"
        else
            DEFAULT_PLAYER="configapp"
        fi
    fi
    echo "$DEFAULT_PLAYER" > "$SD_STAGE/.system/default_player"

    # Stage liby1 SDK artifacts into /.system/lib and /.system/include
    if [[ -f "$SCRIPT_DIR/liby1/build/liby1.so" ]]; then
        cp "$SCRIPT_DIR/liby1/build/liby1.so" "$SD_STAGE/.system/lib/liby1.so"
    fi
    if [[ -f "$SCRIPT_DIR/liby1/build/liby1.a" ]]; then
        cp "$SCRIPT_DIR/liby1/build/liby1.a" "$SD_STAGE/.system/lib/liby1.a"
    fi
    if [[ -f "$SCRIPT_DIR/liby1/include/liby1.h" ]]; then
        cp "$SCRIPT_DIR/liby1/include/liby1.h" "$SD_STAGE/.system/include/liby1.h"
    fi
    if [[ -f "$SCRIPT_DIR/video/y1_video_lib.h" ]]; then
        cp "$SCRIPT_DIR/video/y1_video_lib.h" "$SD_STAGE/.system/include/y1_video_lib.h"
    fi

    # Stage liby1video.so into /.system/lib/
    if [[ -f "$VIDEO_SO_OUT" ]]; then
        cp "$VIDEO_SO_OUT" "$SD_STAGE/.system/lib/liby1video.so"
    fi

    # Bundle optional ARMHF FFmpeg / codec shared libraries into /.system/lib/
    for libdir in "$SCRIPT_DIR/video/libs" "/usr/arm-linux-gnueabihf/lib" "/usr/lib/arm-linux-gnueabihf"; do
        if [[ -d "$libdir" ]]; then
            for pattern in "libm.so*" "libgcc_s.so*" "libz.so*" "libavutil.so*" "libswscale.so*" "libswresample.so*" "libavcodec.so*" "libavformat.so*"; do
                for sofile in "$libdir"/$pattern; do
                    if [[ -f "$sofile" ]]; then
                        cp -L "$sofile" "$SD_STAGE/.system/lib/" 2>/dev/null || true
                    fi
                done
            done
        fi
    done

    # Stage ARM FFmpeg binary into /.system/bin/ffmpeg
    if [[ ! -s "$SD_STAGE/.system/bin/ffmpeg" ]]; then
        if [[ -x "$SCRIPT_DIR/tools/ffmpeg" ]]; then
            cp "$SCRIPT_DIR/tools/ffmpeg" "$SD_STAGE/.system/bin/ffmpeg"
            chmod +x "$SD_STAGE/.system/bin/ffmpeg"
        elif [[ -x "$SCRIPT_DIR/tools/ffmpeg-armv7l" ]]; then
            cp "$SCRIPT_DIR/tools/ffmpeg-armv7l" "$SD_STAGE/.system/bin/ffmpeg"
            chmod +x "$SD_STAGE/.system/bin/ffmpeg"
        fi
    fi

    # Stage raw splash and bootloader menu screens into /.system/
    shopt -s nullglob
    RAW_ASSETS=("$ROOTFS_DIR"/etc/*.raw)
    if ((${#RAW_ASSETS[@]})); then
        cp "${RAW_ASSETS[@]}" "$SD_STAGE/.system/"
    fi

    # Bundle stock Android ramdisk into /.system/ramdisk_android.cpio.gz
    if [[ "${BUNDLE_ANDROID_RAMDISK:-1}" -eq 1 ]]; then
        if [[ ! -s "$SCRIPT_DIR/stock_unpacked/ramdisk.cpio.gz" && -s "$STOCK_BOOT" ]]; then
            python3 "$SCRIPT_DIR/tools/mkbootimg_mtk.py" unpack \
                "$STOCK_BOOT" \
                -o "$SCRIPT_DIR/stock_unpacked"
        fi
        if [[ -f "$SCRIPT_DIR/stock_unpacked/ramdisk.cpio.gz" ]]; then
            cp "$SCRIPT_DIR/stock_unpacked/ramdisk.cpio.gz" "$SD_STAGE/.system/ramdisk_android.cpio.gz"
        fi
    fi

    # Stage Bluetooth kernel modules and firmware into /.system/bt/
    if [[ "${BUNDLE_BT_BLOBS:-1}" -eq 1 && -d "$SCRIPT_DIR/bt_blobs" ]]; then
        BT_ASSETS=("$SCRIPT_DIR"/bt_blobs/*)
        if ((${#BT_ASSETS[@]})); then
            cp -a "${BT_ASSETS[@]}" "$SD_STAGE/.system/bt/"
        fi
    fi

    # Stage bootloader menu screen assets into /.system/
    if [[ -d "$SCRIPT_DIR/bootl_raw" ]]; then
        cp -a "$SCRIPT_DIR/bootl_raw"/*.raw "$SD_STAGE/.system/" 2>/dev/null || true
    fi

    # Build and stage standalone Pick 9 System Settings app into /.system/configapp/
    if [[ "$BUILD_CONFIGAPP" -eq 1 && -f "$SCRIPT_DIR/configapp/Makefile" ]]; then
        make -C "$SCRIPT_DIR/configapp" CROSS_COMPILE="$CROSS_COMPILE" STAGE_DIR="$SD_STAGE" clean stage
        chmod +x "$SD_STAGE/.system/configapp/configapp"
    fi

    # Build and stage standalone MeUI media player into /.meui/
    if [[ "$BUILD_MEUI" -eq 1 && -f "$SCRIPT_DIR/meui/Makefile" ]]; then
        make -C "$SCRIPT_DIR/meui" CROSS_COMPILE="$CROSS_COMPILE" STAGE_DIR="$SD_STAGE" clean stage
        chmod +x "$SD_STAGE/.meui/meui"
    fi

    # Stage optional custom standalone C application binary at /.system/app
    if [[ -n "$CUSTOM_APP_BIN" ]]; then
        if [[ ! -f "$CUSTOM_APP_BIN" ]]; then
            echo "[!] Custom application binary not found: $CUSTOM_APP_BIN"
            exit 1
        fi
        cp "$CUSTOM_APP_BIN" "$SD_STAGE/.system/app"
        chmod +x "$SD_STAGE/.system/app"
    fi

    # Stage iPod_Control structure and default SysInfo for instant PC/Mac detection
    mkdir -p "$SD_STAGE/iPod_Control/Device"
    mkdir -p "$SD_STAGE/iPod_Control/iTunes"
    mkdir -p "$SD_STAGE/iPod_Control/Music"
    mkdir -p "$SD_STAGE/iPod_Control/Artwork"
    mkdir -p "$SD_STAGE/Playlists"
    for i in $(seq -w 0 19); do
        mkdir -p "$SD_STAGE/iPod_Control/Music/F$i"
    done
    cat << 'EOF_SYSINFO' > "$SD_STAGE/iPod_Control/Device/SysInfo"
BoardHwName: iPod M25\r
pszSerialNumber: 8K7289LAV9K\r
ModelNumStr: MA450\r
FirewireGuid: 0x000A270019B48C1F\r
HddFirmwareRev: BU011A\r
RegionCode: FB(0x001A)\r
PolicyFlags: 0x00000000\r
buildID: 0x06118000 (6.1.1)\r
visibleBuildID: 0x01128000 (1.2.1)\r
boardHwRev: 0x00000000 (0.0 0)\r
boardHwSwInterfaceRev: 0x000C0005 (0.0.12 5)\r
bootLoaderImageRev: 0x00000000 (0.0 0)\r
diskModeImageRev: 0x00000000 (0.0 0)\r
diagImageRev: 0x00000000 (0.0 0)\r
osImageRev: 0x00000000 (0.0 0)\r
iPodFamily: 0x00000006\r
updaterFamily: 0x00000006\r
EOF_SYSINFO
    sed -i 's/\\r/\r/' "$SD_STAGE/iPod_Control/Device/SysInfo"
    if [[ -f "$SCRIPT_DIR/dist/rockbox_sdcard/iPod_Control/Device/SysInfoExtended" ]]; then
        cp "$SCRIPT_DIR/dist/rockbox_sdcard/iPod_Control/Device/SysInfoExtended" "$SD_STAGE/iPod_Control/Device/SysInfoExtended"
    fi

    # Keep Rockbox files strictly inside .rockbox/
    if [[ "$BUILD_ROCKBOX" -eq 1 ]]; then
        cp "$BUILD_DIR/rockbox.zip" "$SD_STAGE/rockbox_sdcard.zip"
        mkdir -p "$SD_STAGE/.rockbox"
        cp "$BUILD_DIR/rockbox" "$SD_STAGE/.rockbox/rockbox"
        "$STRIP" -s \
            -R .comment \
            -R .note.gnu.build-id \
            "$SD_STAGE/.rockbox/rockbox"
        chmod +x "$SD_STAGE/.rockbox/rockbox"

        cd "$SD_STAGE"
        zip -u -q "$SD_STAGE/rockbox_sdcard.zip" .rockbox/rockbox
        ZIP_EXTRAS=(.system Videos iPod_Control Playlists)
        [[ -d "$SD_STAGE/.meui" ]] && ZIP_EXTRAS+=(.meui)
        zip -u -q -r "$SD_STAGE/rockbox_sdcard.zip" "${ZIP_EXTRAS[@]}"
        mv -f "$SD_STAGE/rockbox_sdcard.zip" "$SCRIPT_DIR/rockbox_sdcard.zip"
        cd "$SCRIPT_DIR"
        echo "[+] Created SD card archive: $SCRIPT_DIR/rockbox_sdcard.zip"
    else
        rm -f "$SCRIPT_DIR/y1_sdcard.zip"
        cd "$SD_STAGE"
        ZIP_EXTRAS=(.system Videos iPod_Control Playlists)
        [[ -d "$SD_STAGE/.meui" ]] && ZIP_EXTRAS+=(.meui)
        zip -q -r "$SD_STAGE/y1_sdcard.zip" "${ZIP_EXTRAS[@]}"
        mv -f "$SD_STAGE/y1_sdcard.zip" "$SCRIPT_DIR/y1_sdcard.zip"
        cd "$SCRIPT_DIR"
        echo "[+] Created standalone SD card archive: $SCRIPT_DIR/y1_sdcard.zip"
    fi

    chmod -R u+w "$SD_STAGE" 2>/dev/null || true
    rm -rf -- "$SD_STAGE"
    SD_STAGE=""
else
    echo "[5/6] Skipping SD card packaging (PACKAGE_SDCARD=0)."
fi

# ---------------------------------------------------------------------------
# Step 6: Build initramfs and Android kernel boot image
# ---------------------------------------------------------------------------

if [[ "$BUILD_BOOTIMG" -eq 1 ]]; then
    echo "[6/6] Packaging boot image..."

    if [[ -f "$SCRIPT_DIR/kernel_mods/ipod_vpd.c" ]]; then
        mkdir -p "$ROOTFS_DIR/lib/modules"
        "${CC[@]}" -O2 -Wall -Wextra -marm -march=armv7-a -mfloat-abi=soft \
            -fno-stack-protector -fno-pic -mlong-calls -ffreestanding -nostdlib \
            -c "$SCRIPT_DIR/kernel_mods/ipod_vpd.c" \
            -o "$ROOTFS_DIR/lib/modules/ipod_vpd.ko"
    fi

    python3 "$SCRIPT_DIR/tools/build_initramfs.py" \
        "$ROOTFS_DIR" \
        -o "$SCRIPT_DIR/ramdisk_nugget.img"

    if [[ ! -s "$SCRIPT_DIR/stock_unpacked/kernel_mtk.bin" ]]; then
        if [[ -z "${STOCK_BOOT:-}" || ! -s "${STOCK_BOOT:-}" ]]; then
            echo "[!] Stock boot.img not found."
            echo "    Download Y1 Type A stock ROM (rom.zip) from:"
            echo "    https://github.com/y1-community/y1-ata-rom/releases/download/0.1/rom.zip"
            echo "    Extract boot.img to $SCRIPT_DIR/stock_rom/boot.img (or set STOCK_BOOT=/path/to/boot.img)."
            exit 1
        fi

        python3 "$SCRIPT_DIR/tools/mkbootimg_mtk.py" unpack \
            "$STOCK_BOOT" \
            -o "$SCRIPT_DIR/stock_unpacked"
    fi

    if [[ ! -s "$SCRIPT_DIR/stock_unpacked/kernel_mtk.bin" ]]; then
        echo "[!] Stock kernel extraction failed."
        exit 1
    fi

    KERNEL_BIN="$SCRIPT_DIR/stock_unpacked/kernel_mtk.bin"
    if [[ -f "$SCRIPT_DIR/tools/patch_kernel_ipod.py" ]]; then
        KERNEL_IPOD="$SCRIPT_DIR/stock_unpacked/kernel_ipod_mtk.bin"
        python3 "$SCRIPT_DIR/tools/patch_kernel_ipod.py" \
            "$KERNEL_BIN" \
            "$KERNEL_IPOD"
        KERNEL_BIN="$KERNEL_IPOD"
    fi

    python3 "$SCRIPT_DIR/tools/mkbootimg_mtk.py" pack \
        -k "$KERNEL_BIN" \
        -r "$SCRIPT_DIR/ramdisk_nugget.img" \
        -o "$SCRIPT_DIR/boot_nugget.img"

    if [[ ! -s "$SCRIPT_DIR/boot_nugget.img" ]]; then
        echo "[!] Boot image packaging failed."
        exit 1
    fi

    cp -f "$SCRIPT_DIR/boot_nugget.img" "$SCRIPT_DIR/boot.img"

    BOOT_SIZE="$(stat -c%s "$SCRIPT_DIR/boot_nugget.img")"

    if ((BOOT_SIZE > MAX_BOOT_SIZE)); then
        OVER=$((BOOT_SIZE - MAX_BOOT_SIZE))
        echo "[!] ERROR: Boot image exceeds the 6 MiB partition limit by $OVER bytes."
        echo "    Refusing to report this image as flashable."
        exit 1
    fi
else
    echo "[6/6] Skipping boot image packaging (BUILD_BOOTIMG=0)."
fi

# ---------------------------------------------------------------------------
# Build summary
# ---------------------------------------------------------------------------

echo
echo "=========================================================="
echo " BUILD COMPLETE!"
echo "=========================================================="
if [[ "$BUILD_LIBY1" -eq 1 ]]; then
    echo "liby1 Static/Shared:  $SCRIPT_DIR/liby1/build/liby1.a, liby1.so"
fi
if [[ "$BUILD_BOOTIMG" -eq 1 && -f "$SCRIPT_DIR/boot_nugget.img" ]]; then
    BOOT_SIZE="$(stat -c%s "$SCRIPT_DIR/boot_nugget.img")"
    REMAINING=$((MAX_BOOT_SIZE - BOOT_SIZE))
    echo "Flashable Boot Image: $SCRIPT_DIR/boot_nugget.img ($BOOT_SIZE bytes, $REMAINING remaining)"
fi
if [[ "$PACKAGE_SDCARD" -eq 1 ]]; then
    if [[ "$BUILD_ROCKBOX" -eq 1 ]]; then
        echo "SD Card Archive:      $SCRIPT_DIR/rockbox_sdcard.zip (.system/ & .rockbox/)"
    else
        echo "SD Card Archive:      $SCRIPT_DIR/y1_sdcard.zip (.system/)"
    fi
fi
echo "=========================================================="

if [[ "${BUILD_DIST:-0}" -eq 1 ]]; then
    echo
    bash "$SCRIPT_DIR/tools/make_dist.sh"
fi
