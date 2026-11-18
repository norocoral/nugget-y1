🌐 [한국어](kr/IPOD_SYNC.md) | **English**

# iPod 5.5G USB Mode & Native iTunes Integration

Nugget for Innioasis Y1 includes **native iTunes database integration** built directly into Rockbox via `liby1`, paired with Apple iPod 5.5G (Enhanced) hardware spoofing (`patch_kernel_ipod.py` + `ipod_vpd.ko`).

This allows the Innioasis Y1 to connect directly to **Apple iTunes (Windows & macOS)**, **macOS Finder / Apple Music**, or third-party managers (**libgpod**, **Foobar2000 foo_dop**, **MediaMonkey**, **Clementine**) as an authentic 80GB iPod Video (5.5th Generation, Model `MA448` / `MA450`), while Rockbox reads `iTunesDB`, `ArtworkDB`, `.ithmb` artwork, and playlists directly in memory **without writing translated `.tcd`, `.bmp`, or `.m3u8` files to the SD card**.

```mermaid
flowchart LR
    subgraph HOST["Host Computer (Mac / PC / Linux)"]
        ITUNES["Apple iTunes / Finder / Music App / libgpod"]
    end

    subgraph USB_LAYER["Hardware USB Gadget (MT6572)"]
        GADGET["Android USB Gadget (05ac:1209)
        Single-LUN, Config 2, ipod_vpd.ko EVPD XML"]
    end

    subgraph Y1_ENGINE["Native iTunes Engine (liby1 + Rockbox)"]
        INCOMING["Native Readers (RAM-only)
        iTunesDB -> tcramcache.hdr
        ArtworkDB + .ithmb -> RGB565
        Playlists & Smart Playlists (SLst)"]
        OUTGOING["Live Writers
        Play Counts (mhdp)
        Ratings, Bookmarks, OTG Favorites (mhpo)"]
    end

    subgraph STORAGE["MicroSD Card (/mnt/sdcard)"]
        IPOD_DIR["/iPod_Control/
        iTunesDB, ArtworkDB, F*.ithmb
        Play Counts, OTGPlaylistInfo"]
    end

    HOST <-->|High-Speed USB 2.0 (tuned mmcblk1)| USB_LAYER
    USB_LAYER <--> GADGET
    IPOD_DIR -->|Read Directly in RAM| INCOMING
    OUTGOING -->|Write Live on Play/Rate/Fav| IPOD_DIR
```

---

## Table of Contents
1. [Hardware Spoofing (iPod 5.5G Enhanced)](#1-hardware-spoofing-ipod-55g-enhanced)
2. [Bidirectional Sync Architecture](#2-bidirectional-sync-architecture)
   - [2.1 Pre-Connect Phase (Rockbox → iTunes)](#21-pre-connect-phase-rockbox--itunes)
   - [2.2 Post-Disconnect Phase (iTunes → Rockbox)](#22-post-disconnect-phase-itunes--rockbox)
3. [Component & Metadata Conversion Details](#3-component--metadata-conversion-details)
   - [3.1 TagCache Binary Database Generation](#31-tagcache-binary-database-generation)
   - [3.2 Album Artwork & Video Thumbnail Extraction](#32-album-artwork--video-thumbnail-extraction)
   - [3.3 Video Subsystem Integration (`video_db.dat`)](#33-video-subsystem-integration-video_dbdat)
   - [3.4 Playlists & Live Smart Playlist Rules (`.m3u8`)](#34-playlists--live-smart-playlist-rules-m3u8)
   - [3.5 Play Counts, Ratings & On-The-Go (OTG) Playlists](#35-play-counts-ratings--on-the-go-otg-playlists)
4. [CPU Frequency & Dual-Core Turbo Locking](#4-cpu-frequency--dual-core-turbo-locking)
5. [User Settings & Configuration](#5-user-settings--configuration)
6. [Standalone CLI Tool (`/bin/ipod_sync`)](#6-standalone-cli-tool-binipod_sync)
7. [Host Compatibility Guide](#7-host-compatibility-guide)

---

## 1. Hardware Spoofing (iPod 5.5G Enhanced)

When **iPod USB Mode** is active, the Linux kernel gadget subsystem overrides the standard MediaTek USB descriptors with Apple iPod 5.5G identification parameters:

| Parameter | Value | Meaning / Compatibility |
| :--- | :--- | :--- |
| **USB Vendor ID (VID)** | `0x05ac` | Apple, Inc. |
| **USB Product ID (PID)** | `0x1209` | iPod Video 5.5G (Enhanced) |
| **Device Release (bcdDevice)** | `0x0002` | Firmware 1.2 / 1.3 protocol revision |
| **Manufacturer String** | `Apple` | Required for Apple Mobile Device Service |
| **Product String** | `iPod` | Standard Apple USB identifier |
| **USB Serial Number** | `000A270019B48C1F` (or persistent hex GUID) | Derived from `/iPod_Control/Device/SysInfo` |
| **SCSI Vendor / Inquiry** | `"Apple   iPod            1.62"` | Identifies mass storage LUN as an authentic Apple iPod disk |
| **Device Model** | `MA448` / `MA450` (80GB Black / White) | Reported in `SysInfoExtended` XML |

### Device Initialization Files
When the USB cable is attached, `liby1` ensures the existence of required Apple directory trees and device definition files:
- `/iPod_Control/Device/SysInfo`: Contains board revision (`0x000C0005`), serial number, FireWire GUID, and family ID (`0x0000000C`).
- `/iPod_Control/Device/SysInfoExtended`: XML property list containing firmware capabilities, supported image formats (Format 1027, 1031, 1055, 1060), and video playback profiles.
- Root Symlinks: Case-insensitive symlinks (`/ipod_control`, `/IPOD_CONTROL`) mapping to `/mnt/sdcard/iPod_Control` to satisfy legacy syncing tools.

---

## 2. Native iTunes Architecture (Zero-Translation)

Instead of translating `iTunesDB` into `.tcd` / `.bmp` / `.m3u8` files on disk, Rockbox reads and writes Apple's native files directly via `liby1`:

### 2.1 Incoming (iTunes → Rockbox, RAM-Only)
1. **Direct `iTunesDB` → TagCache RAM (`tcramcache.hdr`)**: `apps/tagcache.c` calls `y1_ipod_native_load_ramcache()` on boot and after USB disconnect, populating Rockbox's in-memory TagCache directly from `/iPod_Control/iTunes/iTunesDB` without writing `database_*.tcd` files.
2. **On-the-Fly `ArtworkDB` + `.ithmb` Decoding**: `apps/recorder/albumart.c` and `apps/buffering.c` resolve virtual `ithmb://` cover art paths and decode RGB565 artwork from `/iPod_Control/Artwork/ArtworkDB` and `F*.ithmb` directly into Rockbox's bitmap buffer.
3. **In-Memory Playlists & Smart Playlists (`SLst`)**: `apps/root_menu.c` exposes `iTunesDB` standard playlists and live-evaluated `SLst` smart playlists directly inside Rockbox's Playlist Catalog.

### 2.2 Outgoing (Rockbox → iTunes, Live Writes)
1. **Live `Play Counts` (`mhdp`)**: As tracks finish (`apps/tagtree.c`) or star ratings/bookmarks change (`apps/onplay.c`), `y1_ipod_native_record_play()` and `y1_ipod_native_record_rating()` update `/iPod_Control/iTunes/Play Counts` immediately (preserving existing star ratings on untouched tracks).
2. **Live `OTGPlaylistInfo` (`mhpo`) Favorites**: The **Toggle Favorite** action in Rockbox's WPS context menu (`apps/onplay.c`) calls `y1_ipod_native_record_favorite()`, writing `/iPod_Control/iTunes/OTGPlaylistInfo` so iTunes imports queued favorites on next sync.
3. **USB Attach/Detach Invalidation**: `usb-y1.c` ensures device files, invalidates the in-memory cache on USB attach/eject, and triggers a TagCache RAM reload on disconnect.

---

## 3. Component & Kernel Spoofing Details

### 3.1 Kernel USB & SCSI EVPD Spoofing (`patch_kernel_ipod.py` + `ipod_vpd.ko`)
- **`patch_kernel_ipod.py`** patches the MT6572 kernel for authentic iPod 5.5G enumeration:
  - Single LUN (`MaxLUN = 0`), 16-bit SCSI INQUIRY `AllocationLength` clamp, space-padded `"Apple   iPod            1.62"` standard inquiry, and `bNumConfigurations = 2` (Config 2 for Apple Mobile Device / Finder support).
- **`ipod_vpd.ko`** (loaded dynamically when `y1_ipod_usb_mode` is ON) serves SCSI INQUIRY EVPD pages (`0x00`, `0xC0`, `0xC2..0xFF`) containing the `SysInfoExtended` XML descriptor required by iTunes (Windows) and Finder/Music.app (macOS).

### 3.2 USB 2.0 Mass Storage Speed Tuning (`y1_usb_tune_performance`)
When USB Mass Storage is attached, `y1_usb.c` (and `initramfs/init.c`) automatically tunes the MicroSD block queue and CPU:
- Switches `/sys/block/mmcblk1/queue/scheduler` to `noop` (fallback `deadline`).
- Raises `read_ahead_kb` to `2048`, `nr_requests` to `512`, and `max_sectors_kb` up to `min(max_hw_sectors_kb, 512)`.
- Disables `iostats` and `add_random` overhead and enables `nofua = 1` on LUN0.
- Locks both Cortex-A7 cores at 1.3 GHz (`performance` governor) + `/proc/cpufreq/usb_raise_freq = 1` while attached, and restores previous settings on detach/eject.

---

## 4. CPU Frequency & Dual-Core Turbo Locking

Conversion of thousands of tracks, `.ithmb` image decoding, and TagCache hashing are computationally heavy. To maximize battery life during normal playback while keeping sync times fast, `liby1` implements a **refcounted turbo boost** (`y1_cpu_boost_for_conversion`):

```c
// Lock CPU to maximum performance
y1_cpu_boost_for_conversion(true);

/*
 * 1. CPU0 scaling_governor -> "performance"
 * 2. CPU0 scaling_setspeed  -> "1300000" (1.30 GHz)
 * 3. CPU1 online            -> "1" (Core 1 forced online)
 * 4. CPU1 scaling_governor -> "performance"
 * 5. MediaTek hotplug       -> disabled (prevents kernel from offlining Core 1)
 */

// Perform synchronization...

// Restore previous frequency and governor automatically
y1_cpu_boost_for_conversion(false);
```

During conversion, the device displays real-time progress on the LCD:
```
[====================>        ] 72%
Converting: Album Art (342/480)...
```

---

## 5. User Settings & Configuration

iPod USB Mode is enabled by default and can be toggled via software or configuration files.

### 1. Rockbox UI Settings
- Navigate to: **Settings → General Settings → System → Innioasis Y1 Settings → iPod USB Mode**
- Options: **Yes (iPod 5.5G)** / **No (Standard Mass Storage)**
- When set to **No**, the Y1 presents itself as a standard MediaTek USB mass storage device (`0e8d:0001` / `Y1 Rockbox Player`).

### 2. Configuration Files
- **Rockbox Config**: `/.rockbox/config.cfg`
  ```ini
  y1 ipod usb mode: on
  ```
- **System Flag (checked by initramfs)**: `/.system/ipod_usb_mode`
  - `1`: Enabled
  - `0`: Disabled

---

## 5.5 How to Use After Syncing with iTunes

Once you eject/unplug the Y1 from iTunes (or macOS Finder / Music), Rockbox automatically invalidates and reloads its in-memory database from `/iPod_Control/iTunes/iTunesDB`—**no manual "Update Database" scan is required**.

1. **Browse Music, Artists, Albums, Genres:**
   - Open **Database** from the Rockbox Main Menu.
   - *(Do not use **Files**, because iTunes stores audio under scrambled paths like `/iPod_Control/Music/F00/ABCD.mp3`; **Database** displays all iTunes metadata directly from `iTunesDB` in RAM.)*
2. **Browse iTunes Playlists & Smart Playlists:**
   - Open **Playlists** from the Rockbox Main Menu to browse standard `iTunesDB` playlists and live-evaluated smart playlists (`SLst`). Select a playlist to start playback immediately.
3. **Album Artwork:**
   - Displayed automatically on the While Playing Screen (WPS) and in supported themes directly from `ArtworkDB` and `F*.ithmb`.
4. **Play Counts, Star Ratings & Favorites (Synced Back to iTunes):**
   - **Play Counts:** Automatically recorded to `/iPod_Control/iTunes/Play Counts` as tracks finish playing.
   - **Star Ratings:** On the WPS, open the Context Menu (hold **Select**) → **Set Rating** (updates `Play Counts`).
   - **Favorites (OTG):** On the WPS, open the Context Menu → **Toggle Favorite** (writes `/iPod_Control/iTunes/OTGPlaylistInfo` for iTunes to import on next sync).

---

## 6. Standalone CLI Tool (`/bin/ipod_sync`)

A lightweight initialization binary is installed at `/bin/ipod_sync` inside the rootfs:

```bash
# Initialize iPod directory hierarchy and device descriptors (SysInfo / SysInfoExtended)
/bin/ipod_sync init
```

This utility can be invoked from `initramfs`, shell scripts, or custom applications.

---

## 7. Host Compatibility Guide

| Host Platform | Supported Software | Notes |
| :--- | :--- | :--- |
| **Windows 10 / 11** | Apple iTunes 12.x (Desktop & Store) | Full sync: Music, Videos, Playlists, Artwork, Play Counts |
| **Windows (All)** | Foobar2000 + `foo_dop` | Excellent lightweight iPod management component |
| **Windows (All)** | MediaMonkey 4 / 5 | Full iPod sync support |
| **macOS (Mojave & earlier)** | Apple iTunes | Native iPod synchronization |
| **macOS (Catalina to Sequoia)** | Apple Music / Finder | Syncs directly through Finder sidebar as an iPod |
| **Linux (All distributions)** | `libgpod` / Rhythmbox / Clementine | Full support for reading/writing `iTunesDB` and artwork |
| **Cross-Platform** | MusicBee, Strawberry Music Player | Detects device as an Apple iPod mass storage device |

---

## 8. Windows & iTunes Troubleshooting Guide

If Windows detects the device and installs drivers, but iTunes does not show the iPod icon:

1. **FAT32 Filesystem Requirement**:
   - Apple iTunes on Windows **only** supports classic iPods formatted with **FAT32**.
   - If your MicroSD card is formatted as **exFAT** or NTFS, Windows will mount it in File Explorer, but iTunes will deliberately ignore it.
   - **Solution**: Format the MicroSD card as FAT32 (using tools like *GUIFormat* / *FAT32 Format* for cards larger than 32GB).

2. **Standalone Apple iTunes Desktop Installer**:
   - The Microsoft Store version of iTunes often lacks background `iPod Support Service` permissions or USB redirection drivers.
   - **Solution**: Install the standalone Apple 64-bit installer (`iTunes64Setup.exe` from Apple's official download page). Ensure `Apple Mobile Device Support` and `iPod Support` are running in `services.msc`.

3. **Check Device Manager (Single LUN vs 2x Drives)**:
   - Under **Disk drives**, the drive must appear as **exactly one** `Apple___ iPod____________ USB Device`.
   - Under **Universal Serial Bus controllers**, it appears as `USB Mass Storage Device` (standard for iPod Classic/Video 5.5G `05ac:1209`).
   - If it previously appeared as **`2x Apple___ iPod____________ USB Device`**:
     - The MediaTek MT6572 kernel hardcodes `nluns = 2`. In MediaTek's `f_mass_storage.c`, when `bicr == 0`, the USB `GET_MAX_LUN` request calculates `max_lun = nluns - 1 = 1`, telling the host that two LUNs (0 and 1) exist. Both LUNs shared the inquiry string, producing 2 identical disk drives in Device Manager.
     - When iTunes detects a USB mass storage device with multiple LUNs, `iPodService` rejects the device as an unsupported multi-card reader, as authentic iPods only possess a single LUN.
     - In latest Nugget, `bicr` is set to `2`. In MediaTek's kernel:
       1) `bicr != 0` forces `GET_MAX_LUN` to return `0` (strictly **1 LUN only**).
       2) `bicr != 1` clears the `cdrom` bit (`curlun->cdrom = 0`), keeping SCSI peripheral type `0x00` (Direct-Access Disk).
     - **Action**: In Device Manager, right-click **both** old `Apple___ iPod____________ USB Device` entries, select **Uninstall device**, unplug the USB cable, and reconnect.

4. **Single-LUN Architecture & BICR = 2**:
   - Nugget strictly forces `GET_MAX_LUN = 0` via `bicr = 2` when iPod USB mode is active. Only LUN0 is exposed to the USB host.
