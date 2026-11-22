🌐 [한국어](kr/ARCHITECTURE.md) | **English**

# System Architecture: Innioasis Y1 Hosted Linux Port

This document details the software architecture, process model, boot lifecycle, and storage layout of the Innioasis Y1 Rockbox port and `liby1` Hardware SDK on minimal downstream Linux.

---

## 1. System Block Diagram

```mermaid
flowchart TD
    subgraph HW["Hardware Layer"]
        CPU["MT6572 Dual Cortex-A7 (1.2 GHz)"]
        PMIC["MT6323 PMIC & DAC"]
        LCD["2.8 inch LCD (480x360 BGRA)"]
        SD["MicroSD Card (mmcblk1p1)"]
        BTN["Keypad & Capacitive Wheel"]
        JACK["3.5mm Headphone Jack (accdet)"]
        WIFI_BT["MT6627 BT / FM Radio"]
    end

    subgraph KERNEL["Downstream Linux Kernel 3.4.67"]
        K_EAC["/dev/eac Audio Device"]
        K_FB["/dev/fb0 Framebuffer"]
        K_EV["/dev/input/event* Nodes"]
        K_MMC["mmc_block & Filesystem Drivers"]
        K_CPU["cpufreq & Hotplug Subsystem"]
        K_FM["/dev/fm Tuner Device"]
        K_USB["Android USB Gadget (android0)"]
    end

    subgraph INITRAMFS["Initramfs (PID 1 /init)"]
        INIT_BOOST["Boot CPU Clock Boost (1.2 GHz Dual-Core)"]
        INIT_STORAGE["Reactive MicroSD Mount (/mnt/sdcard)"]
        INIT_MENU["ConfigApp Pre-Boot (--bootmenu) / SD-Missing Text GUI"]
        INIT_EXEC["Handoff to Rockbox or Standalone App"]
    end

    subgraph LIBY1["liby1 Hardware Abstraction Layer (liby1.a / liby1.so)"]
        Y1_CORE["Core, Sysfs, Battery, Power & Logging"]
        Y1_DISP["Framebuffer (480x360 Native / 320x240 Scaled), Backlight, Suspend"]
        Y1_CPU["CPU Governors, Dual-Core Hotplug, Screen-Off Clamp"]
        Y1_INP["Click-Wheel, Tactile Keys, ACCDET Headphone Jack"]
        Y1_AUDIO["EAC DL1 DMA, MT6323 PMIC DAC & HP Amp, Mic ADC"]
        Y1_TUNER["MT6627 FM Radio Control"]
        Y1_BT_WIFI["MT6572 CONNSYS Bluetooth (A2DP) & Wi-Fi"]
        Y1_USB["USB Mass Storage Gadget Control"]
    end

    subgraph APPS["Application Space"]
        RB["Hosted Rockbox Process (/mnt/sdcard/.rockbox/rockbox)"]
        CUSTOM_APP["Custom Standalone C App (/mnt/sdcard/.system/app)"]
    end

    HW --> KERNEL
    KERNEL --> INITRAMFS
    INITRAMFS --> APPS
    APPS --> LIBY1
    LIBY1 --> KERNEL
```

---

## 2. Boot Sequence & Process Lifecycle

```mermaid
sequenceDiagram
    autonumber
    participant LK as MediaTek Little Kernel (LK)
    participant K as Linux Kernel (3.4.67)
    participant I as PID 1 (/init in C)
    participant SD as MicroSD Card (mmcblk1p1)
    participant APP as Rockbox or Custom App

    LK->>K: Transfer execution with MTK ATAGs
    K->>K: Initialize SoC drivers, clocks, memory
    K->>I: Execute /init from initramfs
    I->>I: boost_cpu_for_boot() -> Both cores @ 1.2 GHz performance
    I->>I: Mount /proc, /sys, /dev
    I->>I: Display /etc/boot.raw to /dev/fb0
    I->>SD: Reactive poll for mmcblk1p1 (10ms interval)
    alt MicroSD Card Detected
        I->>SD: Mount mmcblk1p1 to /mnt/sdcard
        I->>I: Create symlink /.system -> /mnt/sdcard/.system
        alt Bootloader Menu Enabled or MENU button held
            I->>I: Launch ConfigApp (--bootmenu)
            opt Android Selected
                I->>I: Clean rootfs & extract ramdisk_android.cpio.gz -> execve("/init")
            end
            opt USB Selected
                I->>I: Attach mmcblk1p1 to USB LUN0 for PC file sync
            end
        end
        alt Custom App Present
            I->>APP: execve("/mnt/sdcard/.system/app")
        else Standard Rockbox
            I->>APP: execve("/mnt/sdcard/.rockbox/rockbox")
        end
    else Card Missing or Unreadable
        I->>I: Display /etc/sdcard.raw and wait
    end
    APP->>APP: Initialize via liby1 (display, input, audio, CPU profile)
    APP->>APP: Enter main event and playback loop
```

---

## 3. Storage Hierarchy & Partitioning

The hardware contains internal eMMC (`mmcblk0`) and external MicroSD (`mmcblk1`).

### 3.1 Partition Scheme
- `mmcblk0` (Internal eMMC): Contains stock MediaTek partitions (`BOOTIMG`, `RECOVERY`, `LOGO`, `SYSTEM`, `CACHE`, `DATA`). Nugget OS overwrites only the `BOOTIMG` partition with `boot_nugget.img` (6 MiB max). The rest of eMMC is left untouched.
- `mmcblk1p1` (External MicroSD): Formatted as FAT32, exFAT, NTFS, or ext4. Houses system libraries, hardware SDK assets, and application data:

```text
/mnt/sdcard/
|-- .system/                      # System, hardware, and developer files (symlinked to /.system)
|   |-- lib/
|   |   |-- liby1.so              # Dynamic liby1 hardware abstraction library
|   |   |-- liby1.a               # Static liby1 library
|   |   `-- liby1video.so         # NEON H.264/AAC hardware video decoder
|   |-- include/
|   |   `-- liby1.h               # Public C header
|   |-- bin/
|   |   `-- ffmpeg                # Static ARM FFmpeg binary (optional)
|   |-- bt/                       # Bluetooth modules & firmware
|   |-- bt_paired.dat             # Paired devices database
|   |-- ramdisk_android.cpio.gz   # Stock Android ramdisk for dual-boot
|   |-- y1_debug.txt              # Hardware diagnostic log
|   |-- system.cfg                # Bootloader menu configuration
|   |-- *.raw                     # Splash and bootloader menu screens
|   `-- app                       # Optional standalone user executable
|-- .rockbox/                     # Rockbox-only assets
|   |-- rockbox                   # ELF ARM hard-float executable
|   |-- codecs/                   # Audio decoders (.codec)
|   |-- rocks/                    # Plugins and viewers (.rock, .ovl)
|   |-- fonts/                    # UI fonts (.fnt, .fnticons)
|   |-- themes/                   # Theme configurations (.cfg)
|   |-- wps/                      # While Playing Screen skins (.wps, .sbs)
|   `-- config.cfg                # Rockbox persisted settings
|-- Videos/                       # Video files
`-- Music/                        # Music library
```

---

## 4. Hardware Abstraction Layer (`liby1`) Mapping

| Subsystem | `liby1` Module | Technical Role |
| :--- | :--- | :--- |
| Core / Power / Battery | [`hosted_linux_port/liby1/src/y1_core.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/hosted_linux_port/liby1/src/y1_core.c) | System initialization, MT6323 PMIC register access, battery telemetry, and `/mnt/sdcard/.system/y1_debug.txt` logging. |
| Display & Backlight | [`hosted_linux_port/liby1/src/y1_display.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/hosted_linux_port/liby1/src/y1_display.c) | Framebuffer writing (`/dev/graphics/fb0`), 480x360 native, 320x240 classic bilinear upscaler, and kernel early suspend. |
| CPU Frequency & Hotplug | [`hosted_linux_port/liby1/src/y1_cpu.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/hosted_linux_port/liby1/src/y1_cpu.c) | cpufreq scaling (598MHz powersave to 1.3GHz turbo), CPU1 core offlining, and screen-off clamp. |
| Input & Click-Wheel | [`hosted_linux_port/liby1/src/y1_input.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/hosted_linux_port/liby1/src/y1_input.c) | Translates `evdev` events from click wheel, tactile buttons, and ACCDET 3.5mm headphone jack. |
| Audio Subsystem | [`hosted_linux_port/liby1/src/y1_audio.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/hosted_linux_port/liby1/src/y1_audio.c) | `/dev/eac` AFE DL1 DMA setup, MT6323 PMIC DAC & Headphone Amp gain staging, 4-byte frame alignment, and deep-idle SPM powerdown gating. |
| FM Radio Tuner | [`hosted_linux_port/liby1/src/y1_tuner.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/hosted_linux_port/liby1/src/y1_tuner.c) | MT6627 FM radio control via `/dev/fm` ioctls. |
| USB Gadget | [`hosted_linux_port/liby1/src/y1_usb.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/hosted_linux_port/liby1/src/y1_usb.c) | Android USB Mass Storage LUN0 bind/unbind for PC music transfer. |
