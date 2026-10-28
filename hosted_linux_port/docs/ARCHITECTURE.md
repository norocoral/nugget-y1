🌐 [한국어](kr/ARCHITECTURE.md) | **English**

# System Architecture: Innioasis Y1 Hosted Linux Port

This document details the software architecture, process model, boot lifecycle, and storage layout of the Innioasis Y1 Rockbox port on downstream minimal Linux.

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
    end

    subgraph KERNEL["Downstream Linux Kernel 3.4.67"]
        K_EAC["/dev/eac Audio Device"]
        K_FB["/dev/fb0 Framebuffer"]
        K_EV["/dev/input/event* Nodes"]
        K_MMC["mmc_block & Filesystem Drivers"]
        K_CPU["cpufreq & Hotplug Subsystem"]
    end

    subgraph INITRAMFS["Initramfs (PID 1 /init)"]
        INIT_BOOST["Boot CPU Clock Boost (1.2 GHz Dual-Core)"]
        INIT_MDEV["mdev Dynamic Device Node Creation"]
        INIT_STORAGE["Reactive MicroSD Mount (/mnt/sdcard)"]
        INIT_SPLASH["Direct Framebuffer Status Displays"]
        INIT_EXEC["execve Rockbox Binary"]
    end

    subgraph ROCKBOX["Hosted Rockbox Process"]
        RB_CORE["Rockbox Core Engine & Buffer Manager"]
        RB_PCM["PCM Driver (/dev/eac Interface)"]
        RB_LCD["Display Driver (480x360 Native / 320x240 Classic)"]
        RB_KEY["Input Driver (Wheel, Tactile Keys, MFB)"]
        RB_PWR["Power Management & Display Sleep Controls"]
    end

    HW --> KERNEL
    KERNEL --> INITRAMFS
    INITRAMFS --> ROCKBOX
    ROCKBOX --> KERNEL
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
    participant RB as Rockbox Executable

    LK->>K: Transfer execution with MTK ATAGs
    K->>K: Initialize SoC drivers, clocks, memory
    K->>I: Execute /init from initramfs
    I->>I: boost_cpu_for_boot() -> Both cores @ 1.2 GHz performance
    I->>I: Mount /proc, /sys, /dev
    I->>I: Write /etc/boot.raw to /dev/fb0
    I->>SD: Reactive poll for mmcblk1p1 (10ms interval)
    alt MicroSD Card Detected
        I->>SD: Mount mmcblk1p1 to /mnt/sdcard
        I->>I: Create symlinks to /.rockbox
        I->>RB: execve("/mnt/sdcard/.rockbox/rockbox")
    else Card Missing or Unreadable
        I->>I: Write /etc/sdcard.raw to /dev/fb0 and initialize USB Mass Storage
    end
    RB->>RB: Initialize memory buffers, settings, codecs
    RB->>RB: Apply theme and skins
    RB->>I: y1_boot_boost_finish() -> Transition CPU to user profile
    RB->>RB: Enter main event and playback loop
```

---

## 3. Storage Hierarchy & Partitioning

The hardware contains internal eMMC (`mmcblk0`) and external MicroSD (`mmcblk1`).

### 3.1 Partition Scheme
- `mmcblk0` (Internal eMMC): Contains stock MediaTek partitions (`BOOTIMG`, `RECOVERY`, `LOGO`, `SYSTEM`, `CACHE`, `DATA`). Rockbox overwrites only the `BOOTIMG` partition with `boot_rockbox.img` (6 MiB max). The rest of eMMC is left unmounted.
- `mmcblk1p1` (External MicroSD): Formatted as FAT32, exFAT, NTFS, or ext4. Acts as the primary root storage drive for Rockbox:
  ```text
  /mnt/sdcard/
  |-- .rockbox/
  |   |-- rockbox          # ELF ARM hard-float executable
  |   |-- codecs/          # Audio decoders (.codec)
  |   |-- rocks/           # Plugins and viewers (.rock, .ovl)
  |   |-- fonts/           # UI fonts (.fnt, .fnticons)
  |   |-- themes/          # Theme configurations (.cfg)
  |   |-- wps/             # While Playing Screen skins (.wps, .sbs)
  |   |-- config.cfg       # Persisted settings
  |   `-- boot.raw         # Custom bootscreen
  `-- Music/               # Media library
  ```

---

## 4. Hardware Abstraction Layer (HAL) Mapping

| Subsystem | File Path | Technical Role |
| :--- | :--- | :--- |
| PCM Audio | [`firmware/target/hosted/innioasis/y1/pcm-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/pcm-y1.c) | Communicates with `/dev/eac`. Handles 44.1/48kHz clock changes and pop suppression. |
| Audio Controls | [`firmware/target/hosted/innioasis/y1/audio-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/audio-y1.c) | Headphone amplifier analog gain (+12dB) and mute GPIO lines. |
| CPU Frequency | [`firmware/target/hosted/innioasis/y1/cpufreq-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/cpufreq-y1.c) | Governor control, core offlining, screen-off throttling, and boot boost completion. |
| LCD / Display | [`firmware/target/hosted/innioasis/y1/lcd-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/lcd-y1.c) | Framebuffer writing (`/dev/fb0`), 480x360 native / 320x240 stretched scaling, 1 FPS AOD refresh. |
| Keypad & Wheel | [`firmware/target/hosted/innioasis/y1/button-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/button-y1.c) | Translates `evdev` events from click wheel and buttons into Rockbox `BUTTON_*` codes. |
| Power & Battery | [`firmware/target/hosted/innioasis/y1/power-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/power-y1.c) | Battery voltage and charge status reading from MT6323 sysfs. |

