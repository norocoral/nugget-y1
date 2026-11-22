🌐 [한국어](krREADME.md) | **English**

# `liby1` — Innioasis Y1 Hardware Abstraction Layer

`liby1` is a tiny, zero-dependency C library that encapsulates all hardware drivers, low-level MediaTek MT6572 / MT6323 PMIC operations, and kernel interfaces for the **Innioasis Y1** digital audio player.

It enables C/C++ developers to create standalone software, games, emulators, custom media players (like MeUI), and GUI experiences for the Y1 without reverse-engineering kernel ioctls, PMIC registers, or sysfs paths. Rockbox itself uses `liby1` under the hood.

> [!TIP]
> For the complete API reference, architecture breakdown, game loop tutorials, and porting guides, read the **[`liby1` Developer Manual & Porting Guide](../docs/LIBY1_GUIDE.md)** ([한국어](../docs/kr/LIBY1_GUIDE.md)).

---

## SD Card Multi-Experience & `.system` Directory Layout

The Y1 bootloader features dynamic multi-experience auto-discovery directly from the physical MicroSD card:

| Location | Purpose |
| :--- | :--- |
| `/mnt/sdcard/.system/configapp/configapp` | **ConfigApp:** Pre-boot & `Main Menu -> System -> System Settings` UI (Launch, BT/Wi-Fi, USB, Serial, Debug) |
| `/mnt/sdcard/.meui/meui` | **Launch in ConfigApp:** MeUI frontend (auto-discovered via hidden-folder scan) |
| `/mnt/sdcard/.custom/launch` | **Launch in ConfigApp:** User-defined software (any hidden `.folder` with an executable) |
| `/mnt/sdcard/.system/app` | Standalone auto-boot application (runs at power-on without menu) |
| `/mnt/sdcard/.system/lib/liby1.a` | Static `liby1` hardware abstraction archive for offline linking |
| `/mnt/sdcard/.system/lib/liby1.so` | Shared dynamic hardware library |
| `/mnt/sdcard/.system/include/liby1.h`| Public C header for `liby1` |
| `/mnt/sdcard/.system/lib/liby1video.so`| NEON-accelerated H.264 / AAC video decoder library |
| `/mnt/sdcard/.system/y1_debug.txt` | System and hardware diagnostic log |

---

## Subsystems Provided by `liby1`

1. **Core, Sysfs, Logging, Battery & Power (`y1_core.c`)**
   - `y1_init()`, `y1_shutdown()`
   - `y1_battery_percent()`, `y1_battery_voltage_mv()`, `y1_battery_current_ma()`, `y1_battery_is_charging()`, `y1_power_usb_online()`
   - `y1_log()`, `y1_logf()`, `y1_sys_poweroff()`, `y1_sys_reboot()`
2. **Display & Backlight (`y1_display.c`)**
   - `y1_fb_init()`, `y1_fb_present_rgb565()`, `y1_fb_present_xrgb8888()`, `y1_fb_blit_rgb565_rect()`, `y1_fb_flip()`
   - `y1_fb_blit_rgb565_classic_scaled()` (smooth 320x240 -> 480x360 2x2-to-3x3 area-averaged hardware blit for retro games & emulators)
   - `y1_fb_display_raw_screen()`
   - `y1_backlight_set_percent()`, `y1_backlight_set_raw()`
   - `y1_display_enter_early_suspend()` / `y1_display_exit_early_suspend()` (powers down LCD clocks, VSync, touch, and Mali GPU in `mem` sleep state)
3. **CPU Frequency & Dual-Core Hotplug (`y1_cpu.c`)**
   - `y1_cpu_set_mode()` (`Y1_CPU_MODE_DYNAMIC`, `POWERSAVE` 598MHz, `BALANCED` 1.0GHz, `PERFORMANCE` 1.2GHz, `TURBO` 1.3GHz)
   - `y1_cpu_set_dual_core()`, `y1_cpu_set_screen_off_clamp()`, `y1_cpu_get_freq_mhz()`
4. **Input, Click-Wheel & 3.5mm Headphone Jack (`y1_input.c`)**
   - `y1_input_init()`, `y1_input_poll()`, `y1_input_map_keycode()`, `y1_input_set_touch_scanning()`
   - `y1_jack_is_inserted()`, `y1_jack_has_mic()`, `y1_jack_get_raw_state()`
5. **Audio DAC, Headphone Amp & Microphone ADC (`y1_audio.c`)**
   - `y1_audio_open()`, `y1_audio_set_sample_rate()` (8kHz - 48kHz), `y1_audio_set_hp_analog_gain()` (0..7 in `MT6323_AUDTOP_CON5`)
   - `y1_audio_write_pcm()` (4-byte stereo frame-aligned DMA ring buffer writer)
   - `y1_audio_wake_lock()` (prevents CPU sleep during active playback)
   - `y1_audio_enable_recording_path()` (Internal Mic, TRRS Headset Mic, FM/Line-In)
   - `y1_pmic_read_reg()`, `y1_pmic_write_reg()`, `y1_afe_write_reg()`
6. **FM Radio Tuner (`y1_tuner.c`)**
   - `y1_fm_power()`, `y1_fm_tune()`, `y1_fm_set_mute()`, `y1_fm_get_rssi()`, `y1_fm_is_tuned()`
7. **USB Mass Storage Gadget (`y1_usb.c`)**
   - `y1_usb_attach_mass_storage()`, `y1_usb_detach_mass_storage()`, `y1_usb_check_host_ejected()`
   - `y1_usb_tune_performance()` (tunes `mmcblk1` queue: `noop`/`deadline`, 2 MiB `read_ahead_kb`, `nr_requests=512`, `max_sectors_kb`, and locks dual-core 1.3 GHz while attached)
   - `y1_usb_set_ipod_mode()`, `y1_usb_get_ipod_mode()` (Apple iPod 5.5G Enhanced VID/PID, `ipod_vpd.ko` EVPD XML, and single-LUN spoofing)
8. **iPod 5.5G USB Mode & Native iTunes Engine (`y1_ipod_sync.c`)**
   - `y1_cpu_boost_for_conversion()` (locks CPU to 1300 MHz and online dual cores during heavy operations)
   - `y1_ipod_ensure_device_files()` (initializes `/iPod_Control/Device/SysInfo` and `SysInfoExtended` XML)
   - `y1_ipod_native_is_active()`, `y1_ipod_native_invalidate()`, `y1_ipod_native_load_ramcache()`, `y1_ipod_native_free_ramcache()` (loads `iTunesDB` directly into Rockbox TagCache RAM without `.tcd` translation)
   - `y1_ipod_native_has_albumart()`, `y1_ipod_native_load_albumart_rgb565()` (decodes `ArtworkDB` + `F*.ithmb` RGB565 on the fly)
   - `y1_ipod_native_playlist_count()`, `y1_ipod_native_playlist_name()`, `y1_ipod_native_playlist_track_count()`, `y1_ipod_native_playlist_track_path()` (reads `iTunesDB` playlists & `SLst` smart playlists directly from memory)
   - `y1_ipod_native_record_play()`, `y1_ipod_native_record_rating()`, `y1_ipod_native_record_favorite()` (writes live `/iPod_Control/iTunes/Play Counts` `mhdp` and `OTGPlaylistInfo` `mhpo`)
   - Standalone CLI utility: `/bin/ipod_sync init`
9. **Bluetooth A2DP & Receiver Mode (`y1_bt.c`)**
   - `y1_bt_init()`, `y1_bt_set_enabled()`, `y1_bt_start_scan()`, `y1_bt_connect_device()`, `y1_bt_disconnect()`, `y1_bt_forget_device()`
   - `y1_bt_set_active_device_codec()` (`SBC`, `SBC XQ`, `AAC`), `y1_bt_set_receiver_mode()`
10. **Wi-Fi (`y1_wifi.c`)**
   - `y1_wifi_init()`, `y1_wifi_set_enabled()`, `y1_wifi_start_scan()`, `y1_wifi_get_networks()`, `y1_wifi_connect()`, `y1_wifi_disconnect()`, `y1_wifi_forget()` (used by ConfigApp; loads `wlan_mt6582.ko` / `mtk_wmt_wifi.ko` and `WIFI_RAM_CODE_MT6582`)

---

## Developer Workflows

### 1. Building Standalone C Programs with `liby1`
```bash
# Build liby1.a, liby1.so, and examples/hello_y1
make -C hosted_linux_port/liby1 CROSS_COMPILE=arm-linux-gnueabihf-

# Compile your own C program statically against liby1
arm-linux-gnueabihf-gcc -O2 -Ihosted_linux_port/liby1/include \
    my_app.c hosted_linux_port/liby1/build/liby1.a -o launch -lpthread -lm -lrt
```

### 2. Live Interactive Debugging Over USB (ConfigApp Serial Terminal)
Hold **MENU** at boot (or open **System Settings** in Rockbox) and open **Serial Terminal** -> enable **Serial Console** (or select **Serial Console** in the SD-missing Text GUI). Connect the Y1 to your PC via USB and open a serial terminal (`115200 8N1` on `/dev/ttyACM0` or `COMx`) for a full BusyBox root shell with live debugging utilities (`ps`, `top`, `dmesg`, `kill`).

### 3. Fail-Safe Watchdog
If your software deadlocks during development, hold **Play/Pause for 10 seconds** to trigger a hardware shutdown at any time.

---

## Related Documentation
- [iPod 5.5G USB Mode & Sync Engine Guide (English)](../docs/IPOD_SYNC.md)
- [iPod 5.5G USB 모드 및 동기화 엔진 가이드 (한국어)](../docs/kr/IPOD_SYNC.md)
- [liby1 Complete Developer Manual & Porting Guide (English)](../docs/LIBY1_GUIDE.md)
- [liby1 개발자 매뉴얼 및 포팅 가이드 (한국어)](../docs/kr/LIBY1_GUIDE.md)
- [Modular Build System Guide (English)](../docs/BUILD_GUIDE.md)
- [모듈식 빌드 시스템 가이드 (한국어)](../docs/kr/BUILD_GUIDE.md)
