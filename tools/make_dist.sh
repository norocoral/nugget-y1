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

# 5. Generate SHA-256 Checksums
echo "[*] Computing SHA-256 checksums..."
(
    cd "$DIST_DIR"
    sha256sum boot_nugget.img *.zip > sha256sums.txt
)

# 6. Generate Release Notes template
cat << 'EOF' > "$DIST_DIR/RELEASE_NOTES.md"
# Innioasis Y1 Nugget OS 2.0 & liby1 Release

## ✨ What's New in 2.0
* **Native iTunes Integration in Rockbox (Zero-Translation):**
  * Loads `/iPod_Control/iTunes/iTunesDB` directly into Rockbox TagCache RAM (`tcramcache.hdr`) without writing `.tcd` files.
  * Decodes `/iPod_Control/Artwork/ArtworkDB` & `F*.ithmb` RGB565 cover art on the fly.
  * Browses and plays `iTunesDB` playlists and live-evaluated smart playlists (`SLst`) directly from memory.
  * Writes `/iPod_Control/iTunes/Play Counts` (`mhdp`) and `OTGPlaylistInfo` (`mhpo`) live on track finish, rating change, and WPS **Toggle Favorite**.
* **Authentic iPod 5.5G USB & Kernel EVPD Spoofing:**
  * `patch_kernel_ipod.py` + `ipod_vpd.ko` provide Single-LUN (`MaxLUN=0`), Config 2 (`bNumConfigurations=2`), space-padded `"Apple   iPod            1.62"`, and SCSI EVPD `SysInfoExtended` XML for Windows iTunes and macOS Finder/Apple Music.
* **High-Speed USB 2.0 Mass Storage (`y1_usb_tune_performance`):**
  * Automatically tunes `mmcblk1` (`noop`/`deadline`, 2 MiB `read_ahead_kb`, `nr_requests=512`, `max_sectors_kb`) and locks dual-core 1.3 GHz performance while USB Mass Storage is attached.
* **Faster WSL2 Build System (`build.sh`):**
  * Native Linux ext4 `rsync` build cache (`~/.cache/y1_build/`), `--only-rockbox` flag, and incremental asset caching.

## 📦 Release Assets
* **`boot_nugget.img`**: Flashable Linux boot image for the MT6572 `BOOTIMG` partition (contains kernel, display drivers, reactive MicroSD loader, and Bootloader Menu).
* **`rockbox_sdcard.zip`**: Complete MicroSD card package. Extract directly to the root of a FAT32/exFAT MicroSD card.
  * `/.rockbox/`: Rockbox binary, audio decoders, plugins, fonts, and themes.
  * `/.system/`: Hardware drivers (`liby1.so`, `liby1video.so`), Bluetooth firmware, splash screens, and Android dual-boot ramdisk.
* **`liby1-sdk.zip`**: Standalone C Hardware Abstraction Layer SDK for developers creating custom applications without Rockbox.
* **`sha256sums.txt`**: Cryptographic checksums for verifying download integrity.

## 🚀 Flashing & Installation
1. **Flash Boot Image:** Open **SP Flash Tool**, load `MT6572_Android_scatter.txt`, select ONLY the `BOOTIMG` partition, assign `boot_nugget.img`, and click Download. Connect the Y1 while powered off.
2. **Setup SD Card:** Extract `rockbox_sdcard.zip` directly onto the root of your MicroSD card.
3. **Power On:** Insert the card and turn on the device. Hold **MENU** while turning on to force the Bootloader Menu.
EOF

echo
echo "=========================================================="
echo " GITHUB DISTRIBUTION READY in hosted_linux_port/dist/"
echo "=========================================================="
ls -lh "$DIST_DIR"
echo "=========================================================="

