#!/usr/bin/env bash
# make_dist.sh - Package Innioasis Y1 GitHub Release Distribution
# Assembles boot_nugget.img, rockbox_sdcard.zip, liby1-sdk.zip, and sha256sums.txt.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="$SCRIPT_DIR/dist"

echo "=========================================================="
echo " Preparing Innioasis Y1 GitHub Release Distribution"
echo "=========================================================="

# 1. Clean and recreate dist directory
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

# 2. Verify or build primary distribution artifacts
if [[ ! -f "$SCRIPT_DIR/boot_nugget.img" || (! -f "$SCRIPT_DIR/rockbox_sdcard.zip" && ! -f "$SCRIPT_DIR/y1_sdcard.zip") ]]; then
    echo "[*] Distribution artifacts not found. Initiating full build..."
    "$SCRIPT_DIR/build.sh"
fi

if [[ ! -s "$SCRIPT_DIR/boot_nugget.img" ]]; then
    echo "[!] Error: Missing flashable boot image: $SCRIPT_DIR/boot_nugget.img"
    exit 1
fi

# 3. Stage boot_nugget.img and SD card archives
echo "[*] Copying core release artifacts to dist/..."
cp "$SCRIPT_DIR/boot_nugget.img" "$DIST_DIR/"
if [[ -f "$SCRIPT_DIR/logo.bin" ]]; then
    cp "$SCRIPT_DIR/logo.bin" "$DIST_DIR/"
fi
if [[ -f "$SCRIPT_DIR/rockbox_sdcard.zip" ]]; then
    cp "$SCRIPT_DIR/rockbox_sdcard.zip" "$DIST_DIR/"
fi
if [[ -f "$SCRIPT_DIR/y1_sdcard.zip" ]]; then
    cp "$SCRIPT_DIR/y1_sdcard.zip" "$DIST_DIR/"
fi

# 4. Package standalone liby1 SDK archive
echo "[*] Assembling liby1-sdk.zip for developers..."
SDK_STAGE="$(mktemp -d "$DIST_DIR/.sdk-stage.XXXXXX")"
mkdir -p "$SDK_STAGE/liby1/include" "$SDK_STAGE/liby1/lib" "$SDK_STAGE/liby1/examples"

cp "$SCRIPT_DIR/liby1/include/liby1.h" "$SDK_STAGE/liby1/include/"
cp "$SCRIPT_DIR/liby1/README.md" "$SDK_STAGE/liby1/"
cp "$SCRIPT_DIR/liby1/krREADME.md" "$SDK_STAGE/liby1/" 2>/dev/null || true
cp "$SCRIPT_DIR/liby1/Makefile" "$SDK_STAGE/liby1/"
cp "$SCRIPT_DIR/liby1/examples/hello_y1.c" "$SDK_STAGE/liby1/examples/"

if [[ -f "$SCRIPT_DIR/liby1/build/liby1.a" ]]; then
    cp "$SCRIPT_DIR/liby1/build/liby1.a" "$SDK_STAGE/liby1/lib/"
fi
if [[ -f "$SCRIPT_DIR/liby1/build/liby1.so" ]]; then
    cp "$SCRIPT_DIR/liby1/build/liby1.so" "$SDK_STAGE/liby1/lib/"
fi

(
    cd "$SDK_STAGE"
    zip -q -r "$DIST_DIR/liby1-sdk.zip" liby1
)
rm -rf "$SDK_STAGE"

if [[ "${BUILD_DIST_IUCE:-0}" -eq 1 ]]; then
    SCATTER_FILE="$SCRIPT_DIR/stock_rom/MT6572_Android_scatter.txt"
    if [[ ! -s "$SCATTER_FILE" ]]; then
        echo "[!] Error: Missing scatter file for Innioasis Updater CE: $SCATTER_FILE"
        exit 1
    fi
    echo "[*] Packaging rom.zip (boot.img + MT6572_Android_scatter.txt) for Innioasis Updater CE..."
    IUCE_STAGE="$(mktemp -d "$DIST_DIR/.iuce-stage.XXXXXX")"
    cp "$SCRIPT_DIR/boot_nugget.img" "$IUCE_STAGE/boot.img"
    cp "$SCATTER_FILE" "$IUCE_STAGE/MT6572_Android_scatter.txt"
    if [[ -s "$SCRIPT_DIR/logo.bin" ]]; then
        cp "$SCRIPT_DIR/logo.bin" "$IUCE_STAGE/logo.bin"
    else
        echo "[!] Error: Missing logo.bin for Innioasis Updater CE: $SCRIPT_DIR/logo.bin"
        exit 1
    fi
    (
        cd "$IUCE_STAGE"
        zip -q "$DIST_DIR/rom.zip" boot.img logo.bin MT6572_Android_scatter.txt
    )
    rm -rf "$IUCE_STAGE"
fi

if [[ -n "${DIST_VERSION:-}" ]]; then
    printf '%s\n' "$DIST_VERSION" > "$DIST_DIR/VERSION.txt"
fi

# 5. Generate SHA-256 Checksums
echo "[*] Computing SHA-256 checksums..."
(
    cd "$DIST_DIR"
    sha256sum boot_nugget.img *.zip > sha256sums.txt
)

echo
echo "=========================================================="
echo " Distribution ready in dist/"
echo "=========================================================="
ls -lh "$DIST_DIR"
echo "=========================================================="

