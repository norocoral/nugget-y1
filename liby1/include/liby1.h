/***************************************************************************
 * liby1 - Innioasis Y1 Hardware Abstraction Layer (MediaTek MT6572 / MT6323)
 *
 * A lightweight, self-contained C library providing direct hardware access
 * for the Innioasis Y1 digital audio player without requiring Rockbox.
 *
 * Subsystems covered:
 *   - Core / System / Logging / Power / Battery (/mnt/sdcard/.system)
 *   - Framebuffer Display (480x360 native & 320x240 classic area-scaled)
 *   - Backlight & Kernel Early-Suspend / Late-Resume Power Management
 *   - CPU Frequency Scaling & Dual-Core Hotplug Control
 *   - Click-Wheel, Keypad, 3.5mm Jack (h2w), & Inline Remote (ACCDET) Input
 *   - MT6572 AFE (/dev/eac) + MT6323 PMIC DAC / Headphone Amp / Mic Audio
 *   - MT6627 FM Radio Tuner (/dev/fm)
 *   - Android USB Mass Storage Gadget (/sys/class/android_usb/android0)
 ****************************************************************************/

#ifndef LIBY1_H
#define LIBY1_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBY1_VERSION_MAJOR 1
#define LIBY1_VERSION_MINOR 0
#define LIBY1_VERSION_PATCH 0

/* =========================================================================
 * System Paths on MicroSD (.system directory)
 * ========================================================================= */
#define Y1_SDCARD_ROOT          "/mnt/sdcard"
#define Y1_SYSTEM_DIR           "/mnt/sdcard/.system"
#define Y1_SYSTEM_LINK          "/.system"
#define Y1_DEBUG_LOG_PATH       "/mnt/sdcard/.system/y1_debug.txt"
#define Y1_BT_DIR               "/mnt/sdcard/.system/bt"
#define Y1_BT_PAIRED_DB_PATH    "/mnt/sdcard/.system/bt_paired.dat"
#define Y1_SYS_LIB_DIR          "/mnt/sdcard/.system/lib"
#define Y1_SYS_BIN_DIR          "/mnt/sdcard/.system/bin"
#define Y1_VIDEO_CACHE_DIR      "/mnt/sdcard/.system/video_cache"
#define Y1_ANDROID_RAMDISK_PATH "/mnt/sdcard/.system/ramdisk_android.cpio.gz"

/* =========================================================================
 * 1. Core, Sysfs, Logging, Power & Battery
 * ========================================================================= */

/** Initialize all basic liby1 subsystems (display, backlight, input, audio). */
int  y1_init(void);

/** Shut down liby1 subsystems cleanly. */
void y1_shutdown(void);

/** Low-level sysfs/procfs helpers. */
bool y1_sysfs_write(const char *path, const char *val);
bool y1_sysfs_write_int(const char *path, int val);
bool y1_sysfs_read(const char *path, char *buf, size_t size);
bool y1_sysfs_read_int(const char *path, int *out_val);

/** Debug logging (writes to stdout, /dev/kmsg, and /mnt/sdcard/.system/y1_debug.txt). */
void y1_log_set_enabled(bool enabled);
bool y1_log_is_enabled(void);
void y1_log_raw(const char *msg, size_t len);
void y1_log(const char *msg);
void y1_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Battery and power supply telemetry. */
int  y1_battery_percent(void);       /* 0..100 % */
int  y1_battery_voltage_mv(void);    /* Millivolts (e.g. 3850) */
int  y1_battery_current_ma(void);    /* Milliamps */
bool y1_battery_is_charging(void);
bool y1_power_usb_online(void);      /* True if USB or AC power is connected */

/** System power control. */
void y1_sys_poweroff(void) __attribute__((noreturn));
void y1_sys_reboot(void) __attribute__((noreturn));

/* =========================================================================
 * 2. Display (Framebuffer) & Backlight / Sleep Management
 * ========================================================================= */

#define Y1_FB_WIDTH         480
#define Y1_FB_HEIGHT        360
#define Y1_CLASSIC_WIDTH    320
#define Y1_CLASSIC_HEIGHT   240

/** Pixel blend callback used for optional UI transition fading. */
typedef uint16_t (*y1_pixel_blend_fn)(int x, int y, uint16_t cur_px, void *user);

/** Open and map /dev/graphics/fb0 and power on the MT6572 display controller. */
int  y1_fb_init(void);
void y1_fb_close(void);
bool y1_fb_is_ready(void);

/** Get hardware framebuffer geometry and direct pointer. */
int      y1_fb_width(void);
int      y1_fb_height(void);
int      y1_fb_bpp(void);
int      y1_fb_stride(void);
uint8_t *y1_fb_mmap_ptr(void);

/** Trigger MTK hardware overlay pan/refresh (FBIOPAN_DISPLAY). */
void y1_fb_flip(void);

/** Enable/disable display controller eco_mode (/proc/dispsys/eco_mode). */
void y1_fb_set_eco_mode(bool eco_on);

/** Convert 16-bit RGB565 pixel to 32-bit hardware XRGB8888 format via LUT. */
uint32_t y1_fb_rgb565_to_xrgb8888(uint16_t px);

/**
 * Blit a 16-bit RGB565 source buffer (stride = src_stride_px pixels) to the
 * 480x360 hardware framebuffer and trigger a display refresh.
 */
void y1_fb_blit_rgb565_rect(const uint16_t *src_base, int src_stride_px,
                            int x, int y, int width, int height,
                            y1_pixel_blend_fn blend_cb, void *blend_user);

/**
 * Blit a 320x240 16-bit RGB565 buffer scaled smoothly (2x2 -> 3x3 area average)
 * onto the 480x360 hardware framebuffer.
 */
void y1_fb_blit_rgb565_classic_scaled(const uint16_t *src_base, int src_stride_px,
                                      int x, int y, int width, int height,
                                      y1_pixel_blend_fn blend_cb, void *blend_user);

/** Blit a full 480x360 16-bit RGB565 frame. */
void y1_fb_present_rgb565(const uint16_t *pixels_480x360);

/** Blit a full 480x360 32-bit XRGB8888 frame. */
void y1_fb_present_xrgb8888(const uint32_t *pixels_480x360);

/**
 * Load and display a 480x360x4 (.raw BGRA) splash screen by name.
 * Searches /.system/<name>.raw, /mnt/sdcard/.system/<name>.raw,
 * /.rockbox/<name>.raw, and /etc/<name>.raw.
 * Optionally copies a downsampled RGB565 representation into out_fb16.
 */
bool y1_fb_display_raw_screen(const char *name, uint16_t *out_fb16,
                              int fb16_stride_px, bool classic_320x240);

/** Hardware backlight control (/sys/class/leds/lcd-backlight/brightness). */
void y1_backlight_set_raw(int val_0_to_255);     /* Direct 0..255 */
void y1_backlight_set_percent(int pct_0_to_100); /* 0..100% */
int  y1_backlight_get_raw(void);

/** Kernel early_suspend ('mem') and late_resume ('on') screen power state. */
void y1_display_enter_early_suspend(void);
void y1_display_exit_early_suspend(void);
bool y1_display_is_early_suspended(void);

/* =========================================================================
 * 3. CPU Frequency & Core Hotplug Control
 * ========================================================================= */

enum y1_cpu_mode_id {
    Y1_CPU_MODE_DYNAMIC     = 0, /* Ondemand 598 MHz - 1001 MHz */
    Y1_CPU_MODE_POWERSAVE   = 1, /* Fixed 598 MHz (Lowest VCORE 1.05V) */
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

/* =========================================================================
 * 4. Input (Click-Wheel, Buttons, Touch Scan, Headphone Jack)
 * ========================================================================= */

/* Logical button bitmask for standalone applications */
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
    int      keycode;     /* Raw Linux input_event code */
    int      value;       /* 1 = press, 0 = release, 2 = repeat */
    uint32_t button;      /* Mapped Y1_BTN_* flag */
    bool     is_accdet;   /* True if emitted by headset ACCDET input device */
};

/** Open all /dev/input/event* devices for standalone polling. */
int  y1_input_init(void);
void y1_input_close(void);

/** Poll for the next button or wheel event (timeout_ms: 0=non-blocking, -1=infinite). */
int  y1_input_poll(struct y1_input_event *out_ev, int timeout_ms);

/** Map a raw Linux keycode on the Y1 to a Y1_BTN_* mask. */
uint32_t y1_input_map_keycode(int keycode);

/** Enable or disable touch-wheel scanning (/proc/tpd_keys_enable). */
void y1_input_set_touch_scanning(bool enabled);

/** Headphone jack detection (/sys/class/switch/h2w/state: 0=none, 1=4-pole mic, 2=3-pole). */
int  y1_jack_get_raw_state(void);
bool y1_jack_is_inserted(void);
bool y1_jack_has_mic(void);

/* =========================================================================
 * 5. Audio (MT6572 /dev/eac AFE + MT6323 PMIC DAC / Headphone Amp / ADC)
 * ========================================================================= */

enum y1_audio_rec_source {
    Y1_REC_SOURCE_INTERNAL_MIC  = 0,
    Y1_REC_SOURCE_HEADPHONE_MIC = 1,
    Y1_REC_SOURCE_LINE_FM       = 2
};

/** Open and configure /dev/eac, AFE clocks, 32KB DL1 DMA, and MT6323 Headphone Amp. */
int  y1_audio_open(unsigned long sample_rate);
void y1_audio_close(void);
bool y1_audio_is_open(void);
int  y1_audio_get_fd(void);

/** Program hardware sample rate (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000). */
bool y1_audio_set_sample_rate(unsigned long sample_rate);
unsigned long y1_audio_get_sample_rate(void);

/** Set MT6323 PMIC Headphone Amplifier analog PGA gain index (0..7, 2dB/step). */
void y1_audio_set_hp_analog_gain(int idx_l, int idx_r);

/** Pause or resume AFE DL1 DMA transfer without powering down the headphone amp. */
void y1_audio_set_dma_running(bool running);

/** Write 16-bit stereo interleaved PCM frames to /dev/eac with 4-byte frame alignment. */
ssize_t y1_audio_write_pcm(const void *pcm_stereo_s16, size_t bytes);

/** Configure MT6572 SPM deep-idle CPU powerdown state (disable cpupdn during active DMA). */
bool y1_audio_set_deep_idle(bool allow_cpu_powerdown);

/** Hold or release the "y1play" kernel wakelock. */
void y1_audio_wake_lock(bool hold);

/** Configure MT6323 PMIC analog recording mux & mic bias. */
void y1_audio_enable_recording_path(enum y1_audio_rec_source source);
void y1_audio_disable_recording_path(void);
void y1_audio_set_recording_pga_db(int gain_db);

/** Direct MT6323 PMIC analog register & MT6572 AFE register access via /dev/eac. */
int  y1_pmic_read_reg(uint32_t offset);
int  y1_pmic_write_reg(uint32_t offset, uint32_t val, uint32_t mask);
int  y1_afe_write_reg(uint32_t offset, uint32_t val, uint32_t mask);
void y1_pmic_tune_accdet_debounce(void);

/* =========================================================================
 * 6. FM Radio Tuner (MT6572 / MT6627 /dev/fm)
 * ========================================================================= */

int  y1_fm_power(bool on);
int  y1_fm_tune(int freq_hz); /* e.g. 87500000 to 108000000 */
int  y1_fm_get_freq(void);
int  y1_fm_set_mute(bool mute);
bool y1_fm_is_muted(void);
int  y1_fm_get_rssi(void);
bool y1_fm_is_tuned(void);

/* =========================================================================
 * 7. USB Mass Storage Gadget (/sys/class/android_usb/android0)
 * ========================================================================= */

bool y1_usb_attach_mass_storage(void);
void y1_usb_detach_mass_storage(void);
bool y1_usb_is_storage_attached(void);
bool y1_usb_check_host_ejected(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBY1_H */

