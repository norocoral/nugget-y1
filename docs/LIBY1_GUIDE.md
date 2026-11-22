🌐 [한국어](kr/LIBY1_GUIDE.md) | **English**

# `liby1` Developer Guide & API Reference

`liby1` is a standalone C hardware abstraction library for the Innioasis Y1 (MediaTek MT6572 SoC, MT6323 PMIC, MT6627 wireless/FM combo) running downstream Linux kernel 3.4.67. It exposes the device's framebuffer, `/dev/eac` audio engine, `/dev/input/event*` devices, `/dev/fm` tuner, CPU/hotplug sysfs controls, wakelock/suspend state, USB gadget sysfs controls, and `iTunesDB`/`ArtworkDB` parser.

All public declarations are in [`liby1.h`](../liby1/include/liby1.h).

---

## 1. Target Platform & ABI

| Parameter | Specification |
| :--- | :--- |
| SoC | MediaTek MT6572 (2x ARM Cortex-A7, 598 MHz – 1300 MHz) |
| ABI / Target Triple | `arm-linux-gnueabihf` (ARMv7-A, VFPv4, NEON, hard-float) |
| Memory | 512 MB LPDDR2 |
| Display | 480x360 LCD on `/dev/graphics/fb0` (XRGB8888 hardware overlay; RGB565 software presentation) |
| Audio | MT6572 AFE (`/dev/eac`, 32 KB DL1 DMA, 16-bit stereo interleaved) + MT6323 PMIC DAC/PGA/ADC |
| Input | Capacitive touch wheel + 5 tactile buttons + 3.5mm TRRS `h2w`/`accdet` |
| Connectivity | USB 2.0 High-Speed (`android_usb` Mass Storage, ACM serial `/dev/ttyGS0`–`/dev/ttyGS1`), Bluetooth 4.0 (MT6627), FM Radio (`/dev/fm`) |
| Storage | MicroSD (`/mnt/sdcard`, FAT32/exFAT/ext4) |

---

## 2. Bootloader Integration & Runtime Environment

Applications can be launched from the pre-boot menu (`Pick 1`–`Pick 3`) or as the fallback `/.system/app` binary. Before `exec()`, PID 1 `/init` sets:

- Working directory: `/mnt/sdcard` (or the application's directory)
- `PATH=/bin:/sbin:/usr/bin:/usr/sbin:/mnt/sdcard/.system/bin:/mnt/sdcard/.rockbox/bin:/mnt/sdcard/.meui/bin:/mnt/sdcard/.custom/bin`
- `LD_LIBRARY_PATH=/mnt/sdcard/.meui/lib:/mnt/sdcard/.custom/lib:/mnt/sdcard/.system/lib:/mnt/sdcard/.rockbox/lib:/lib:/usr/lib`
- `HOME=/mnt/sdcard`
- `TERM=linux`

Because `LD_LIBRARY_PATH` includes `/mnt/sdcard/.custom/lib` and `/mnt/sdcard/.system/lib`, dynamically linked binaries can depend on `liby1.so` without installing anything to rootfs. Static linking against `liby1.a` is also supported.

### SD Card Layout

```text
/mnt/sdcard/
├── .system/                      # Shared system directory (symlinked to /.system)
│   ├── include/liby1.h           # Public C header
│   ├── lib/liby1.a               # Static library
│   ├── lib/liby1.so              # Shared library
│   ├── lib/liby1video.so         # Video decoder library
│   ├── bin/                      # Auxiliary binaries
│   ├── bt/                       # MediaTek Bluetooth modules & firmware
│   ├── y1_debug.txt              # Debug log output (when y1_log_set_enabled(true))
│   └── app                       # Optional default auto-boot binary
├── .custom/                      # Bootloader Pick 3
│   ├── launch                    # ELF binary or chmod +x shell script
│   ├── bin/
│   └── lib/
├── .meui/                        # Bootloader Pick 2
│   ├── meui
│   └── lib/
└── .rockbox/                     # Bootloader Pick 1
    └── rockbox
```

---

## 3. Subsystem Guides & API Reference

### 3.1 Core, Sysfs, Logging, Power & Battery

`y1_init()` initializes the core runtime in a fixed sequence: it ensures `/mnt/sdcard/.system` exists, opens and maps `/dev/graphics/fb0` (`y1_fb_init()`), sets backlight brightness to 50%, enables touch-wheel scanning (`y1_input_set_touch_scanning(true)`), and opens all `/dev/input/event*` devices (`y1_input_init()`). It does **not** open `/dev/eac` (audio) or `/dev/fm` (radio); initialize those only when your application needs them.

`y1_shutdown()` reverses initialization by closing audio, powering down the FM tuner, closing input file descriptors, closing the framebuffer, and closing log file descriptors. Call it before returning from `main()` so the bootloader or next process inherits clean hardware state.

```c
int  y1_init(void);
void y1_shutdown(void);

bool y1_sysfs_write(const char *path, const char *val);
bool y1_sysfs_write_int(const char *path, int val);
bool y1_sysfs_read(const char *path, char *buf, size_t size);
bool y1_sysfs_read_int(const char *path, int *out_val);

void y1_log_set_enabled(bool enabled);
bool y1_log_is_enabled(void);
void y1_log_raw(const char *msg, size_t len);
void y1_log(const char *msg);
void y1_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int  y1_battery_percent(void);       /* 0..100 */
int  y1_battery_voltage_mv(void);    /* millivolts */
int  y1_battery_current_ma(void);    /* milliamps */
bool y1_battery_is_charging(void);
bool y1_power_usb_online(void);

void y1_sys_poweroff(void) __attribute__((noreturn));
void y1_sys_reboot(void) __attribute__((noreturn));
```

- **Sysfs helpers:** `y1_sysfs_read()` strips trailing `\r`/`\n` and NUL-terminates `buf`. Both read and write helpers open with `O_CLOEXEC` and close immediately, returning `false` on open/I/O error.
- **Logging:** Disabled by default. Calling `y1_log_set_enabled(true)` writes messages simultaneously to `stdout`, `/dev/kmsg`, and `/mnt/sdcard/.system/y1_debug.txt`.
- **Battery & power:** `y1_battery_*()` reads the MT6323 power supply sysfs nodes (`/sys/class/power_supply/battery/`). `y1_sys_poweroff()` and `y1_sys_reboot()` call `sync()` before issuing the kernel reboot syscall.

---

### 3.2 Display, Framebuffer, Backlight & Sleep

The Y1 panel is 480x360 and `/dev/graphics/fb0` is configured in 32-bit `XRGB8888` format. Because most embedded UIs and retro ports render in 16-bit `RGB565`, `liby1` maintains a precomputed RGB565-to-XRGB8888 lookup table so software RGB565 buffers can be blitted directly to the hardware framebuffer without manual pixel conversion.

Each presentation function (`y1_fb_present_rgb565()`, `y1_fb_present_xrgb8888()`, `y1_fb_blit_rgb565_rect()`, `y1_fb_blit_rgb565_classic_scaled()`) writes to the mapped framebuffer and automatically calls `y1_fb_flip()` (`FBIOPAN_DISPLAY`) to commit the frame to the MT6572 display overlay. If you write directly to `y1_fb_mmap_ptr()`, you must call `y1_fb_flip()` yourself after updating the buffer.

```c
#define Y1_FB_WIDTH         480
#define Y1_FB_HEIGHT        360
#define Y1_CLASSIC_WIDTH    320
#define Y1_CLASSIC_HEIGHT   240

typedef uint16_t (*y1_pixel_blend_fn)(int x, int y, uint16_t cur_px, void *user);

int      y1_fb_init(void);
void     y1_fb_close(void);
bool     y1_fb_is_ready(void);

int      y1_fb_width(void);
int      y1_fb_height(void);
int      y1_fb_bpp(void);
int      y1_fb_stride(void);
uint8_t *y1_fb_mmap_ptr(void);

void     y1_fb_flip(void);
void     y1_fb_set_eco_mode(bool eco_on);
uint32_t y1_fb_rgb565_to_xrgb8888(uint16_t px);

void y1_fb_blit_rgb565_rect(const uint16_t *src_base, int src_stride_px,
                            int x, int y, int width, int height,
                            y1_pixel_blend_fn blend_cb, void *blend_user);

void y1_fb_blit_rgb565_classic_scaled(const uint16_t *src_base, int src_stride_px,
                                      int x, int y, int width, int height,
                                      y1_pixel_blend_fn blend_cb, void *blend_user);

void y1_fb_present_rgb565(const uint16_t *pixels_480x360);
void y1_fb_present_xrgb8888(const uint32_t *pixels_480x360);

bool y1_fb_display_raw_screen(const char *name, uint16_t *out_fb16,
                              int fb16_stride_px, bool classic_320x240);

void y1_backlight_set_raw(int val_0_to_255);
void y1_backlight_set_percent(int pct_0_to_100);
int  y1_backlight_get_raw(void);

void y1_display_enter_early_suspend(void);
void y1_display_exit_early_suspend(void);
bool y1_display_is_early_suspended(void);
void y1_power_set_idle_sleep(bool allow_suspend);
```

#### Scaling & Splash Screens
- **Partial updates:** `y1_fb_blit_rgb565_rect()` blits a sub-rectangle `(x, y, width, height)` from a buffer whose row width in pixels is `src_stride_px`. Pass `NULL` for `blend_cb` unless per-pixel transition blending is needed.
- **320x240 Classic scaling:** `y1_fb_blit_rgb565_classic_scaled()` upscales a 320x240 RGB565 region to 480x360 using `2x2 -> 3x3` area averaging (1.5x exact ratio, avoiding nearest-neighbor shimmer).
- **Raw BGRA screens:** `y1_fb_display_raw_screen("boot", ...)` searches `/.system/<name>.raw`, `/mnt/sdcard/.system/<name>.raw`, `/.rockbox/<name>.raw`, and `/etc/<name>.raw` for a `480x360x4` BGRA image. If `out_fb16` is non-NULL, it also writes a converted RGB565 copy into `out_fb16`.

#### Display Power & Kernel Suspend Lifecycle
The MT6572 kernel uses Android `earlysuspend` + `wakelock`:
1. **Screen off while staying active** (e.g., background audio playback): call `y1_display_enter_early_suspend()`. This acquires `"rockbox_active"` in `/sys/power/wake_lock` and writes `"mem\n"` to `/sys/power/state`. Because a userspace wakelock is held, the kernel runs `earlysuspend` hooks (powering down LCD clocks, backlight, touch scanning, and GPU) without freezing userspace threads.
2. **Full `mem` suspend when idle**: call `y1_power_set_idle_sleep(true)`. This releases `"rockbox_active"` and `"y1play"` wakelocks (unless USB Mass Storage is attached) and writes `"mem\n"` to `/sys/power/state`, allowing the kernel to suspend to RAM.
3. **Waking back up**: on wake input, call `y1_power_set_idle_sleep(false)` immediately to re-acquire `"rockbox_active"` (preventing the kernel from re-suspending), then call `y1_display_exit_early_suspend()` (writes `"on\n"` to `/sys/power/state` and restores the display) and restore backlight brightness.

---

### 3.3 CPU Frequency & Core Hotplug

The MT6572 has two Cortex-A7 cores (`cpu0` and `cpu1`). Keeping `cpu1` online or locking high clocks increases idle and playback draw, while running `598 MHz` single-core can drop frames in software renderers or slow bulk database parsing.

```c
enum y1_cpu_mode_id {
    Y1_CPU_MODE_DYNAMIC     = 0, /* Ondemand 598 MHz - 1001 MHz */
    Y1_CPU_MODE_POWERSAVE   = 1, /* Fixed 598 MHz */
    Y1_CPU_MODE_BALANCED    = 2, /* Fixed 1001 MHz */
    Y1_CPU_MODE_PERFORMANCE = 3, /* Fixed 1196 MHz */
    Y1_CPU_MODE_TURBO       = 4  /* Fixed 1300 MHz */
};

void y1_cpu_set_mode(int mode);
int  y1_cpu_get_mode(void);
void y1_cpu_set_dual_core(bool dual_core);
bool y1_cpu_is_dual_core(void);
int  y1_cpu_get_freq_mhz(void);
void y1_cpu_set_screen_off_clamp(bool screen_off);
void y1_cpu_boost_for_conversion(bool enable);
```

- `y1_cpu_set_mode()` selects the `cpufreq` governor and target speed for `cpu0`.
- `y1_cpu_set_dual_core(false)` writes `0` to `/sys/devices/system/cpu/cpu1/online` to power-gate `cpu1`; `true` brings `cpu1` back online.
- `y1_cpu_set_screen_off_clamp(true)` clamps frequency to powersave and offlines `cpu1` when the display enters suspend; pass `false` on wake to restore the active profile.
- `y1_cpu_boost_for_conversion(true)` temporarily raises clocks and enables `cpu1` for short bursts (e.g., loading `iTunesDB` or decoding artwork), and `false` restores the previous mode.

---

### 3.4 Input & 3.5mm Jack

Physical buttons, capacitive click-wheel ticks, and inline 3.5mm headset buttons arrive as Linux `EV_KEY` events across multiple `/dev/input/event*` nodes (`mtk-kpd`, `tpd`/touch wheel, and `ACCDET`). `y1_input_init()` scans and opens these devices non-blockingly and tags which file descriptors belong to `ACCDET`.

```c
#define Y1_BTN_NONE         0x0000
#define Y1_BTN_SELECT       0x0001
#define Y1_BTN_MENU         0x0002
#define Y1_BTN_LEFT         0x0004
#define Y1_BTN_RIGHT        0x0008
#define Y1_BTN_SCROLL_FWD   0x0010
#define Y1_BTN_SCROLL_BACK  0x0020
#define Y1_BTN_PLAY         0x0040
#define Y1_BTN_HEADSET_MFB  0x0080
#define Y1_BTN_HEADSET_LONG 0x0100

struct y1_input_event {
    int      keycode;     /* Raw Linux input_event.code */
    int      value;       /* 1 = press, 0 = release, 2 = repeat */
    uint32_t button;      /* Mapped Y1_BTN_* mask */
    bool     is_accdet;   /* True if from headset ACCDET device */
};

int      y1_input_init(void);
void     y1_input_close(void);
int      y1_input_poll(struct y1_input_event *out_ev, int timeout_ms);
uint32_t y1_input_map_keycode(int keycode);
void     y1_input_set_touch_scanning(bool enabled);

int  y1_jack_get_raw_state(void); /* /sys/class/switch/h2w/state: 0=none, 1=4-pole mic, 2=3-pole */
bool y1_jack_is_inserted(void);
bool y1_jack_has_mic(void);
```

#### Polling Semantics & Hardware Quirks
- **Return value of `y1_input_poll()`:** Returns `1` when an `EV_KEY` event is written to `out_ev`, `0` when `timeout_ms` expires with no key event, and `-1` if no input devices are open. Pass `timeout_ms = 0` inside a 60 FPS loop, or `-1` to block until input arrives.
- **Drain all queued events per frame:** Always call `while (y1_input_poll(&ev, 0) > 0)` rather than reading once per frame; fast click-wheel rotation generates multiple `Y1_BTN_SCROLL_FWD` / `Y1_BTN_SCROLL_BACK` events per frame.
- **Wheel press/release pairs:** The capacitive wheel driver emits `value = 1` followed immediately by `value = 0` for each tick. Act on `ev.value == 1` (or `ev.value != 0` if handling repeats) and ignore `ev.value == 0` for scroll ticks.
- **ACCDET remapping:** Some 3.5mm headset inline remotes emit `KEY_PLAYPAUSE`, which `y1_input_map_keycode()` maps to `Y1_BTN_PLAY`. When the event originates from an `ACCDET` device (`is_accdet == true`), `y1_input_poll()` automatically remaps `Y1_BTN_PLAY` to `Y1_BTN_HEADSET_MFB` so inline headset clicks can be distinguished from the physical Play button.
- **Touch scanning power saving:** Call `y1_input_set_touch_scanning(false)` when the screen is off and wheel input is not needed (`/proc/tpd_keys_enable`).
- **Headphone jack state:** `y1_jack_get_raw_state()` reads `/sys/class/switch/h2w/state` (`0` = unplugged, `1` = 4-pole TRRS with mic, `2` = 3-pole TRS without mic). `y1_jack_is_inserted()` is true for `1` or `2`; `y1_jack_has_mic()` is true only for `1`.

```c
struct y1_input_event ev;
while (y1_input_poll(&ev, 0) > 0) {
    if (ev.value == 1) { /* Key press */
        switch (ev.button) {
        case Y1_BTN_SCROLL_FWD:  /* clockwise tick */ break;
        case Y1_BTN_SCROLL_BACK: /* counter-clockwise tick */ break;
        case Y1_BTN_SELECT:      /* center button */ break;
        case Y1_BTN_MENU:        /* top menu button */ break;
        case Y1_BTN_HEADSET_MFB: /* 3.5mm inline remote */ break;
        }
    }
}
```

---

### 3.5 Audio: `/dev/eac` AFE & MT6323 PMIC

Downstream MediaTek kernel 3.4.67 does not expose standard ALSA PCM playback nodes. Instead, digital audio is routed through the Enhanced Audio Controller character device (`/dev/eac`) into the MT6572 Audio Frontend (AFE) 32 KB DL1 DMA ring buffer, and from there over I2S to the MT6323 PMIC stereo DAC and headphone amplifier.

`liby1` manages the `/dev/eac` ioctls, clock dividers, MT6323 analog registers, and DMA frame alignment.

```c
enum y1_audio_rec_source {
    Y1_REC_SOURCE_INTERNAL_MIC  = 0,
    Y1_REC_SOURCE_HEADPHONE_MIC = 1,
    Y1_REC_SOURCE_LINE_FM       = 2
};

int           y1_audio_open(unsigned long sample_rate);
void          y1_audio_close(void);
bool          y1_audio_is_open(void);
int           y1_audio_get_fd(void);

bool          y1_audio_set_sample_rate(unsigned long sample_rate);
unsigned long y1_audio_get_sample_rate(void);

void    y1_audio_set_hp_analog_gain(int idx_l, int idx_r);
void    y1_audio_set_dma_running(bool running);
ssize_t y1_audio_write_pcm(const void *pcm_stereo_s16, size_t bytes);

bool y1_audio_set_deep_idle(bool allow_cpu_powerdown);
void y1_audio_wake_lock(bool hold);

void    y1_audio_enable_recording_path(enum y1_audio_rec_source source);
void    y1_audio_disable_recording_path(void);
void    y1_audio_set_recording_pga_db(int gain_db);
bool    y1_audio_rec_start(enum y1_audio_rec_source source, unsigned long sample_rate);
ssize_t y1_audio_rec_read(void *pcm_stereo_s16, size_t bytes);
void    y1_audio_rec_stop(void);

void y1_audio_set_fm_route(bool enable);

int  y1_pmic_read_reg(uint32_t offset);
int  y1_pmic_write_reg(uint32_t offset, uint32_t val, uint32_t mask);
int  y1_afe_write_reg(uint32_t offset, uint32_t val, uint32_t mask);
void y1_pmic_tune_accdet_debounce(void);
```

#### Playback Lifecycle & Hardware Constraints
1. **Open:** Call `y1_audio_open(sample_rate)` with one of the supported rates (`8000`, `11025`, `12000`, `16000`, `22050`, `24000`, `32000`, `44100`, `48000`). Returns `0` on success, negative on failure. Opening configures AFE clocks, allocates the 32 KB DL1 DMA buffer, and powers on the MT6323 headphone amp.
2. **Prevent DMA underrun & suspend:** While actively streaming PCM:
   - Call `y1_audio_wake_lock(true)` to hold `"y1play"` so the kernel does not enter `mem` suspend when the screen turns off.
   - Call `y1_audio_set_deep_idle(false)` so MT6572 SPM does not power down the CPU/bus between DMA interrupts.
3. **Write PCM frames:** `y1_audio_write_pcm()` expects 16-bit signed (`int16_t`) interleaved stereo (`L, R`) samples (4 bytes per stereo frame). If `/dev/eac` accepts a partial byte count that is not a multiple of 4, `liby1` pads the remaining bytes to a 4-byte boundary so Left/Right channels never swap or drift out of phase.
4. **Analog gain:** `y1_audio_set_hp_analog_gain(idx_l, idx_r)` clamps indices to `0..7` (2 dB/step) and writes bits `[14:12]` (L) and `[10:8]` (R) of `MT6323_AUDTOP_CON5` (`0x70a`), preserving the headphone mux/power bits `[4:0]` (`0x0014`) to avoid pops. You can call this before or after `y1_audio_open()` (the last indices are cached and applied when `/dev/eac` opens).
5. **Sample rate switching:** Call `y1_audio_set_sample_rate(new_rate)` when switching tracks. If `new_rate` equals the current rate, it returns `true` immediately without touching hardware. Otherwise it gates PMIC downlink/master, resets the DL1 ring buffer pointers, reprograms the AFE/PMIC dividers, and toggles the CDC clock sync bit.
6. **Pause vs. close:** For short pauses where you want zero re-open latency, call `y1_audio_set_dma_running(false)` to pause AFE DL1 DMA without powering down the headphone amp. When playback is stopped or idle, call `y1_audio_set_deep_idle(true)`, `y1_audio_wake_lock(false)`, and `y1_audio_close()`.

```c
if (y1_audio_open(44100) == 0) {
    y1_audio_set_hp_analog_gain(6, 6);
    y1_audio_wake_lock(true);
    y1_audio_set_deep_idle(false);

    while (playing) {
        /* fill stereo_buf with 1024 interleaved L/R int16_t frames */
        y1_audio_write_pcm(stereo_buf, sizeof(stereo_buf));
    }

    y1_audio_set_deep_idle(true);
    y1_audio_wake_lock(false);
    y1_audio_close();
}
```

#### Recording & FM Audio Routing
- **Uplink PCM recording:** Call `y1_audio_rec_start(source, sample_rate)` (enables the MT6323 analog mux/mic bias and AFE uplink), read 16-bit stereo frames in a loop with `y1_audio_rec_read()`, adjust preamp gain with `y1_audio_set_recording_pga_db(gain_db)`, and stop with `y1_audio_rec_stop()`.
- **FM radio audio:** Call `y1_audio_set_fm_route(true)` after powering on the FM tuner to route MT6627 I2S/MRG audio through the MT6572 AFE downlink to headphones.
- **Direct register access:** `y1_pmic_read_reg()`, `y1_pmic_write_reg()`, and `y1_afe_write_reg()` issue `/dev/eac` register ioctls using read-modify-write `mask`. `y1_pmic_tune_accdet_debounce()` adjusts `ACCDET` PMIC debounce timings for reliable 3.5mm headset MFB click detection.

---

### 3.6 FM Radio Tuner (`/dev/fm`)

The MT6627 FM receiver is exposed via `/dev/fm` (falling back to `/dev/radio0` or `/dev/stpfm`). Frequencies are passed in **Hz** (e.g. `87500000` for 87.5 MHz, `108000000` for 108.0 MHz); `liby1` converts Hz to the 100 kHz units expected by `mtk_fm_drv.ko`.

```c
int  y1_fm_power(bool on);
int  y1_fm_tune(int freq_hz); /* 87500000 to 108000000 */
int  y1_fm_seek(int freq_hz, bool up);
int  y1_fm_get_freq(void);
int  y1_fm_set_mute(bool mute);
bool y1_fm_is_muted(void);
int  y1_fm_get_rssi(void);
bool y1_fm_is_tuned(void);
int  y1_fm_set_mono(bool mono);
bool y1_fm_is_stereo(void);
```

- Typical startup sequence: `y1_fm_power(true)` -> `y1_audio_open(44100)` (if not already open) -> `y1_audio_set_fm_route(true)` -> `y1_fm_tune(98500000)` -> `y1_fm_set_mute(false)`.
- `y1_fm_seek(start_freq_hz, up)` searches upward (`up = true`) or downward (`up = false`) from `start_freq_hz` and returns the locked frequency in Hz (or negative on failure).
- On exit, mute and disable FM routing (`y1_audio_set_fm_route(false)`), then call `y1_fm_power(false)`.

---

### 3.7 USB Mass Storage Gadget

`liby1` exposes the MicroSD card (`/dev/block/mmcblk1`) to a host PC via `/sys/class/android_usb/android0`.

```c
bool y1_usb_attach_mass_storage(void);
void y1_usb_detach_mass_storage(void);
bool y1_usb_is_storage_attached(void);
bool y1_usb_check_host_ejected(void);
bool y1_usb_is_serial_active(void);
void y1_usb_tune_performance(bool enable);
```

- **Attach / Detach:** Call `y1_usb_attach_mass_storage()` when USB Mass Storage mode is entered. Unmount or flush any open SD card files before attaching so host writes do not corrupt filesystem metadata. While attached, `y1_power_set_idle_sleep(true)` is a no-op so the USB gadget stays alive.
- **Detecting safe host eject:** Poll `y1_usb_check_host_ejected()` periodically while attached; when the host OS safely ejects the volume, call `y1_usb_detach_mass_storage()`.
- **I/O tuning:** `y1_usb_tune_performance(true)` switches `/sys/block/mmcblk1/queue/scheduler` to `noop`/`deadline`, sets `read_ahead_kb` to `2048` and `nr_requests` to `512`/`256`, and raises the USB CPU frequency. Calling `false` (done automatically on detach) restores the saved scheduler and queue parameters.

---

### 3.8 iPod USB Mode & Native `iTunesDB` API

This subsystem has two parts: USB identity spoofing (`y1_usb_set_ipod_mode`) and direct in-memory reading/writing of an existing `/mnt/sdcard/iPod_Control/` directory (`y1_ipod_native_*`).

```c
void y1_usb_set_ipod_mode(bool enabled);
bool y1_usb_get_ipod_mode(void);

bool y1_ipod_ensure_device_files(void);

bool y1_ipod_native_is_active(void);
void y1_ipod_native_invalidate(void);

bool y1_ipod_native_load_ramcache(struct y1_ipod_ramcache_data *out);
void y1_ipod_native_free_ramcache(struct y1_ipod_ramcache_data *data);

bool y1_ipod_native_record_play(const char *rb_path, uint32_t play_delta,
                                uint32_t unix_time, uint32_t bookmark_ms);
bool y1_ipod_native_record_rating(const char *rb_path, int rating_rb);
bool y1_ipod_native_record_favorite(const char *rb_path, bool is_favorite);

bool y1_ipod_native_has_albumart(const char *rb_path);
bool y1_ipod_native_load_albumart_rgb565(const char *rb_path,
                                          uint16_t **out_rgb565,
                                          int *out_w, int *out_h);

int         y1_ipod_native_playlist_count(void);
const char *y1_ipod_native_playlist_name(int pl_idx);
int         y1_ipod_native_playlist_track_count(int pl_idx);
const char *y1_ipod_native_playlist_track_path(int pl_idx, int tr_idx);
```

- **USB spoofing (disabled by default):** When `y1_usb_set_ipod_mode(true)` is set before calling `y1_usb_attach_mass_storage()`, `liby1` loads `ipod_vpd.ko` (SCSI EVPD spoofing) and configures Apple iPod 5.5G USB descriptors (`VID 0x05AC`, `PID 0x1209`). `y1_ipod_ensure_device_files()` generates `/mnt/sdcard/iPod_Control/Device/SysInfo` and `SysInfoExtended` so iTunes/libgpod recognizes the device.
- **Reading `iTunesDB` & Playlists:** `y1_ipod_native_is_active()` checks whether `/mnt/sdcard/iPod_Control/iTunes/iTunesDB` is present. `y1_ipod_native_load_ramcache()` parses `iTunesDB` into Rockbox-compatible tag tables (`struct y1_ipod_ramcache_data`); free it with `y1_ipod_native_free_ramcache()`. After USB detach, call `y1_ipod_native_invalidate()` so updated databases are re-parsed.
- **Album Art:** `y1_ipod_native_load_albumart_rgb565()` looks up the track in `ArtworkDB`, extracts the matching `.ithmb` slot, and allocates an RGB565 buffer (`*out_rgb565`) with dimensions `*out_w` x `*out_h`. The caller must `free(*out_rgb565)`.
- **Play Counts, Ratings & Favorites:** `y1_ipod_native_record_play()`, `y1_ipod_native_record_rating()`, and `y1_ipod_native_record_favorite()` write updates back to `iPod_Control/iTunes/Play Counts` and `OTGPlaylistInfo` so they sync back to iTunes on the next USB connection.
- See [IPOD_SYNC.md](IPOD_SYNC.md) for the binary format and USB descriptor details.

---

## 4. Packaging, Debugging & Watchdog

### 4.1 ConfigApp Launch & Debug Info
- **Launch (Hidden-Folder Discovery):** Place an ELF executable or `chmod +x` shell script inside any hidden folder on `/mnt/sdcard/` (folder name containing `.`, e.g. `/.meui/meui` or `/.custom/launch`) to have ConfigApp's **Launch** page list and execute it.
- **Debug Info:** Open **Debug Info** in ConfigApp to view live hardware telemetry or save `/mnt/sdcard/debuginfoYYYYMMDD_HHMMSS.txt`.
- **Wi-Fi & Bluetooth (`y1_wifi.c`, `y1_bt.c`):** `y1_wifi_*` and `y1_bt_*` manage MT6572 CONNSYS (`/dev/stpwmt`, `/dev/stpbt`, `/dev/wmtWifi`, `wlan_mt6582.ko`).

### 4.2 USB Serial Shell (ConfigApp Serial Terminal)
1. Hold **MENU** while powering on (or open **Main Menu -> System -> System Settings** in Rockbox) and open **Serial Terminal** -> **Serial Console** (or select **Serial Console** in the SD-missing Text GUI).
2. Connect via USB-C and open a serial terminal at `115200 8N1` (`/dev/ttyACM0` on Linux/macOS, `COMx` on Windows).
3. Root shells are bound to `/dev/ttyGS0` and `/dev/ttyGS1`. Exiting the shell or toggling **Serial Console** off stops the gadget.

### 4.3 Hardware Safety Watchdog
PID 1 monitors the physical **Play/Pause** `evdev` key independently of user-space applications. Holding **Play/Pause** continuously for 10 seconds calls `sync()` and forces hardware power-off, allowing recovery from frozen or deadlocked user-space code without disconnecting the battery.

---

## 5. Minimal Application & Makefile

```c
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "liby1.h"

int main(void) {
    if (y1_init() != 0)
        return 1;

    y1_cpu_set_mode(Y1_CPU_MODE_PERFORMANCE);
    y1_backlight_set_percent(80);

    uint16_t fb[Y1_FB_WIDTH * Y1_FB_HEIGHT];
    memset(fb, 0, sizeof(fb));

    bool running = true;
    while (running) {
        struct y1_input_event ev;
        while (y1_input_poll(&ev, 0) > 0) {
            if (ev.value == 1 && ev.button == Y1_BTN_MENU)
                running = false;
        }
        y1_fb_present_rgb565(fb);
        usleep(16000);
    }

    y1_cpu_set_mode(Y1_CPU_MODE_DYNAMIC);
    y1_shutdown();
    return 0;
}
```

```makefile
CC       := arm-linux-gnueabihf-gcc
CFLAGS   := -O2 -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -Wall -Wextra
INCLUDES := -I../liby1/include
LIBY1_A  := ../liby1/build/liby1.a
LDFLAGS  := -lpthread -lm -lrt

TARGET   := launch
SRCS     := main.c

all: $(TARGET)

$(TARGET): $(SRCS) $(LIBY1_A)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) $(LIBY1_A) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
```
