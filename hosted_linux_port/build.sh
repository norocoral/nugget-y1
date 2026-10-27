
#!/usr/bin/env bash
# build.sh - Build script for Innioasis Y1 Hosted Rockbox Linux
# Compiles Rockbox, builds a minimal initramfs rootfs, and packages boot_rockbox.img.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Workspace detection
# ---------------------------------------------------------------------------

WORKSPACE_ROOT="${WORKSPACE_ROOT:-}"

if [[ -z "$WORKSPACE_ROOT" ]]; then
    search_dir="$SCRIPT_DIR"

    while :; do
        if [[ -d "$search_dir/!stock_rom" ]]; then
            WORKSPACE_ROOT="$search_dir"
            break
        fi

        parent_dir="$(dirname "$search_dir")"
        [[ "$parent_dir" == "$search_dir" ]] && break
        search_dir="$parent_dir"
    done
fi

if [[ -z "$WORKSPACE_ROOT" || ! -d "$WORKSPACE_ROOT" ]]; then
    echo "[!] Could not find workspace root containing !stock_rom."
    echo "    Set WORKSPACE_ROOT explicitly."
    exit 1
fi

if [[ -f "$SCRIPT_DIR/../tools/configure" ]]; then
    ROCKBOX_SRC="$(cd "$SCRIPT_DIR/.." && pwd)"
elif [[ -f "$SCRIPT_DIR/tools/configure" ]]; then
    ROCKBOX_SRC="$SCRIPT_DIR"
else
    ROCKBOX_SRC="$WORKSPACE_ROOT/!rockbox_fromsource/rockbox-master/rockbox-master"
fi

ROOTFS_DIR="$SCRIPT_DIR/rootfs"
BUILD_DIR="$ROCKBOX_SRC/build_y1"
SPLASH_DIR="$SCRIPT_DIR/splash"
STOCK_BOOT="${STOCK_BOOT:-$WORKSPACE_ROOT/!stock_rom/boot.img}"

CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

# Use ccache only when installed.
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-5G}"
    export CCACHE_COMPILERCHECK=content

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
    DEFAULT_JOBS="$(nproc)"
elif command -v getconf >/dev/null 2>&1; then
    DEFAULT_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
else
    DEFAULT_JOBS=2
fi

JOBS="${JOBS:-$DEFAULT_JOBS}"

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
# Build information
# ---------------------------------------------------------------------------

echo "=========================================================="
echo " Innioasis Y1 Hosted Rockbox Build"
echo "=========================================================="
echo "Toolchain:   $CC"
echo "Rockbox Src: $ROCKBOX_SRC"
echo "Build Dir:   $BUILD_DIR"
echo "Rootfs:      $ROOTFS_DIR"
echo "Parallelism: $JOBS jobs"
echo "=========================================================="

if [[ ! -f "$ROCKBOX_SRC/tools/configure" ]]; then
    echo "[!] Rockbox configure script not found: $ROCKBOX_SRC/tools/configure"
    exit 1
fi

if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "[!] Cross-compiler not found: ${CROSS_COMPILE}gcc"
    exit 1
fi

if [[ "$CC" == "ccache "* ]] && ! command -v ccache >/dev/null 2>&1; then
    echo "[!] ccache was selected but is not installed."
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

# ---------------------------------------------------------------------------
# 1. Set up root filesystem and splash images
# ---------------------------------------------------------------------------

echo "[1/5] Setting up root filesystem..."

python3 "$SCRIPT_DIR/tools/setup_rootfs.py"

rm -rf "$ROOTFS_DIR/.rockbox"
rm -f "$ROOTFS_DIR/rockbox"
rm -f "$ROOTFS_DIR"/etc/*.png

mkdir -p "$ROOTFS_DIR/etc" "$ROOTFS_DIR/bin"

# Convert splash and status images.
for img in boot sdcard connect errconnect error; do
    SRC_IMG=""

    if [[ -f "$SPLASH_DIR/$img.png" ]]; then
        SRC_IMG="$SPLASH_DIR/$img.png"
    elif [[ -f "$WORKSPACE_ROOT/$img.png" ]]; then
        SRC_IMG="$WORKSPACE_ROOT/$img.png"
    elif [[ -f "$ROCKBOX_SRC/$img.png" ]]; then
        SRC_IMG="$ROCKBOX_SRC/$img.png"
    fi

    if [[ -n "$SRC_IMG" ]]; then
        echo "[*] Converting $SRC_IMG..."
        python3 "$SCRIPT_DIR/tools/convert_splash.py" \
            "$SRC_IMG" "$ROOTFS_DIR/etc/$img.raw"
    fi
done

# ---------------------------------------------------------------------------
# Compile independent native utilities in parallel
# ---------------------------------------------------------------------------

echo "[*] Compiling native utilities with parallel jobs..."

"${CC[@]}" -O2 -Wall \
    "$SCRIPT_DIR/initramfs/init.c" \
    -o "$ROOTFS_DIR/init" &
INIT_PID=$!

"${CC[@]}" -O2 \
    "$SCRIPT_DIR/audio/unmute_eac.c" \
    -o "$ROOTFS_DIR/bin/unmute_eac" &
AUDIO_PID=$!

BUILD_FAILED=0

wait "$INIT_PID" || BUILD_FAILED=1
wait "$AUDIO_PID" || BUILD_FAILED=1

if [[ "$BUILD_FAILED" -ne 0 ]]; then
    echo "[!] Native utility compilation failed."
    exit 1
fi

# Strip both binaries concurrently.
"$STRIP" "$ROOTFS_DIR/init" &
STRIP_INIT_PID=$!

"$STRIP" "$ROOTFS_DIR/bin/unmute_eac" &
STRIP_AUDIO_PID=$!

BUILD_FAILED=0
wait "$STRIP_INIT_PID" || BUILD_FAILED=1
wait "$STRIP_AUDIO_PID" || BUILD_FAILED=1

if [[ "$BUILD_FAILED" -ne 0 ]]; then
    echo "[!] Failed to strip native utilities."
    exit 1
fi

chmod +x "$ROOTFS_DIR/init" "$ROOTFS_DIR/bin/unmute_eac"

# ---------------------------------------------------------------------------
# 2. BusyBox rescue shell
# ---------------------------------------------------------------------------

echo "[2/5] Setting up BusyBox..."

if [[ ! -s "$ROOTFS_DIR/bin/busybox" ]]; then
    echo "[*] Looking for static ARM BusyBox..."

    if [[ -f "$SCRIPT_DIR/tools/busybox-armv7l" ]]; then
        cp "$SCRIPT_DIR/tools/busybox-armv7l" "$ROOTFS_DIR/bin/busybox"
    elif [[ -f "$WORKSPACE_ROOT/busybox-armv7l" ]]; then
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

    mkdir -p "$ROOTFS_DIR/sbin"
    ln -sf /bin/busybox "$ROOTFS_DIR/sbin/poweroff"
    ln -sf /bin/busybox "$ROOTFS_DIR/sbin/reboot"
fi

# ---------------------------------------------------------------------------
# Copy required ARM glibc libraries
# ---------------------------------------------------------------------------

echo "[*] Setting up ARM runtime libraries..."

mkdir -p "$ROOTFS_DIR/lib"

rm -f \
    "$ROOTFS_DIR"/lib/libm* \
    "$ROOTFS_DIR"/lib/libdl* \
    "$ROOTFS_DIR"/lib/libpthread* \
    "$ROOTFS_DIR"/lib/librt*

# Remove stale temporary or duplicate libraries left in the persistent rootfs.
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

# ---------------------------------------------------------------------------
# Bundle Bluetooth modules and firmware
# ---------------------------------------------------------------------------

if [[ -d "$SCRIPT_DIR/bt_blobs" ]]; then
    echo "[*] Bundling Bluetooth modules and firmware..."

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

# ---------------------------------------------------------------------------
# 3. Configure and build Rockbox
# ---------------------------------------------------------------------------

echo "[3/5] Configuring and compiling Rockbox..."

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

chmod +x "$ROCKBOX_SRC/tools/configure"

if [[ -f Makefile ]] && ! grep -Fqx 'export SHARED_CFLAGS=-fPIC -fvisibility=hidden' Makefile; then
    echo "[*] Refreshing stale Y1 plugin build configuration..."
    make clean
    "$ROCKBOX_SRC/tools/configure" --target=310 --type=N
elif [[ ! -f Makefile ]]; then
    "$ROCKBOX_SRC/tools/configure" --target=310 --type=N
fi

if [[ ! -f Makefile ]]; then
    echo "[!] Configuration did not generate a Makefile."
    exit 1
fi

# Remove empty or invalid object files left by interrupted builds or stale
# filesystem/cache artifacts so make recompiles them instead of failing at link.
find "$BUILD_DIR" -type f -name '*.o' -size 0 -delete 2>/dev/null || true
while IFS= read -r -d '' object; do
    if ! "$READELF" -h "$object" >/dev/null 2>&1; then
        echo "[*] Removing invalid object file: $object"
        rm -f -- "$object"
    fi
done < <(find "$BUILD_DIR" -type f -name '*.o' -print0)

# Ensure genlang-features stays in sync with features to avoid missing LANG_ defines
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

# Ensure lang.h is generated BEFORE parallel object compilation to prevent race conditions
if [[ ! -f "$BUILD_DIR/lang/lang.h" ]]; then
    echo "[*] Generating language header before parallel compilation..."
    make -j1 "$BUILD_DIR/lang/lang.h"
fi

echo "[*] Building Rockbox with $JOBS parallel jobs..."
make -j"$JOBS"

echo "[*] Creating Rockbox SD card package..."
make -j"$JOBS" zip

if [[ ! -s "$BUILD_DIR/rockbox.zip" ]]; then
    echo "[!] Rockbox SD package was not generated."
    exit 1
fi

if [[ ! -s "$BUILD_DIR/rockbox" ]]; then
    echo "[!] Rockbox executable was not generated."
    exit 1
fi

# ---------------------------------------------------------------------------
# 4. Prepare SD card package
# ---------------------------------------------------------------------------

echo "[4/5] Preparing Rockbox binaries and assets..."

cp "$BUILD_DIR/rockbox.zip" "$SCRIPT_DIR/rockbox_sdcard.zip"

SD_STAGE="$(mktemp -d "$SCRIPT_DIR/.sdcard-stage.XXXXXX")"

mkdir -p "$SD_STAGE/.rockbox/bt" "$SD_STAGE/.rockbox/lib" "$SD_STAGE/Videos"

echo "[*] Building external SD card video library (liby1video.so)..."
"${CC[@]}" -O3 -mfpu=neon -mfloat-abi=hard -fPIC -shared \
    -Wl,-rpath,/mnt/sdcard/.rockbox/lib \
    "$SCRIPT_DIR/video/liby1video.c" \
    "$SCRIPT_DIR"/video/h264_objs/libavcodec/*.o \
    "$SCRIPT_DIR"/video/h264_objs/libavutil/*.o \
    "$SCRIPT_DIR"/video/faad_objs/*.o \
    -o "$SD_STAGE/.rockbox/lib/liby1video.so" \
    -ldl -lm
"$STRIP" --strip-unneeded \
    -R .comment \
    -R .note.gnu.build-id \
    "$SD_STAGE/.rockbox/lib/liby1video.so"

# Bundle optional ARMHF FFmpeg / codec shared libraries if present in sysroot or video/libs
for libdir in "$SCRIPT_DIR/video/libs" "/usr/arm-linux-gnueabihf/lib" "/usr/lib/arm-linux-gnueabihf"; do
    if [[ -d "$libdir" ]]; then
        for pattern in "libm.so*" "libgcc_s.so*" "libz.so*" "libavutil.so*" "libswscale.so*" "libswresample.so*" "libavcodec.so*" "libavformat.so*"; do
            for sofile in "$libdir"/$pattern; do
                if [[ -f "$sofile" ]]; then
                    cp -L "$sofile" "$SD_STAGE/.rockbox/lib/" 2>/dev/null || true
                fi
            done
        done
    fi
done

cp "$BUILD_DIR/rockbox" "$SD_STAGE/.rockbox/rockbox"

"$STRIP" -s \
    -R .comment \
    -R .note.gnu.build-id \
    "$SD_STAGE/.rockbox/rockbox"

chmod +x "$SD_STAGE/.rockbox/rockbox"

# Add raw splash images.
shopt -s nullglob
RAW_ASSETS=("$ROOTFS_DIR"/etc/*.raw)

if ((${#RAW_ASSETS[@]})); then
    cp "${RAW_ASSETS[@]}" "$SD_STAGE/.rockbox/"
fi

# Add Bluetooth blobs.
BT_ASSETS=()
if [[ -d "$SCRIPT_DIR/bt_blobs" ]]; then
    BT_ASSETS=("$SCRIPT_DIR"/bt_blobs/*)
fi

if ((${#BT_ASSETS[@]})); then
    cp -a "${BT_ASSETS[@]}" "$SD_STAGE/.rockbox/bt/"
fi

cd "$SD_STAGE"

zip -u -q "$SCRIPT_DIR/rockbox_sdcard.zip" .rockbox/rockbox

for asset in .rockbox/*.raw .rockbox/bt/* .rockbox/lib/* Videos; do
    [[ -e "$asset" ]] || continue
    zip -u -q -r "$SCRIPT_DIR/rockbox_sdcard.zip" "$asset"
done

cd "$SCRIPT_DIR"

rm -rf -- "$SD_STAGE"
SD_STAGE=""

echo "[+] Created SD card archive: $SCRIPT_DIR/rockbox_sdcard.zip"

# ---------------------------------------------------------------------------
# 5. Build initramfs and boot image
# ---------------------------------------------------------------------------

echo "[5/5] Packaging boot image..."

python3 "$SCRIPT_DIR/tools/build_initramfs.py" \
    "$ROOTFS_DIR" \
    -o "$SCRIPT_DIR/ramdisk_rockbox.img"

# Unpack the stock boot image only when the kernel is missing.
if [[ ! -s "$SCRIPT_DIR/stock_unpacked/kernel_mtk.bin" ]]; then
    if [[ ! -s "$STOCK_BOOT" ]]; then
        echo "[!] Stock boot image not found: $STOCK_BOOT"
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

python3 "$SCRIPT_DIR/tools/mkbootimg_mtk.py" pack \
    -k "$SCRIPT_DIR/stock_unpacked/kernel_mtk.bin" \
    -r "$SCRIPT_DIR/ramdisk_rockbox.img" \
    -o "$SCRIPT_DIR/boot_rockbox.img"

if [[ ! -s "$SCRIPT_DIR/boot_rockbox.img" ]]; then
    echo "[!] Boot image packaging failed."
    exit 1
fi

# ---------------------------------------------------------------------------
# Build summary
# ---------------------------------------------------------------------------

BOOT_SIZE="$(stat -c%s "$SCRIPT_DIR/boot_rockbox.img")"

if ((BOOT_SIZE > MAX_BOOT_SIZE)); then
    OVER=$((BOOT_SIZE - MAX_BOOT_SIZE))
    echo "[!] ERROR: Boot image exceeds the 6 MiB partition limit by $OVER bytes."
    echo "    Refusing to report this image as flashable."
    exit 1
fi

echo
echo "=========================================================="
echo " BUILD COMPLETE!"
echo "=========================================================="
echo "Flashable Boot Image: $SCRIPT_DIR/boot_rockbox.img"
echo "Boot Image Size:      $BOOT_SIZE bytes"
echo "SD Card Assets:       $SCRIPT_DIR/rockbox_sdcard.zip"
echo "Partition Limit:      $MAX_BOOT_SIZE bytes (6 MiB)"
REMAINING=$((MAX_BOOT_SIZE - BOOT_SIZE))
echo "[+] SIZE CHECK PASSED: $REMAINING bytes remaining."

echo "=========================================================="
