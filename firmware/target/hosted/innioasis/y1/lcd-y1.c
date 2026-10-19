/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Innioasis Y1 Native Framebuffer Display Driver (MediaTek MT6572)
 *
 * Supports both 16-bit RGB565 and 32-bit ARGB8888 hardware framebuffers,
 * automatic stride handling, and MTK overlay unblanking / pan display.
 ****************************************************************************/

#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

#include "lcd.h"
#include "lcd-target.h"
#include "backlight-target.h"
#include "panic.h"
#include "kernel.h"
#include "settings.h"
#include "misc.h"
#include "audio.h"

#ifndef VESA_NO_BLANKING
#define VESA_NO_BLANKING 0
#endif
#ifndef VESA_POWERDOWN
#define VESA_POWERDOWN   4
#endif

/* MediaTek Hardware Framebuffer IOCTLs */
#ifndef MTKFB_POWERON
#define MTKFB_POWERON            0x4f0c /* _IO('O', 12) */
#endif
#ifndef MTKFB_POWEROFF
#define MTKFB_POWEROFF           0x4f0d /* _IO('O', 13) */
#endif
#ifndef MTKFB_CHANGE_UPDATESPEED
#define MTKFB_CHANGE_UPDATESPEED 0x40044f13 /* _IOW('O', 19, unsigned long) */
#endif
#ifndef MTKFB_RESET_UPDATESPEED
#define MTKFB_RESET_UPDATESPEED  0x4f35     /* _IO('O', 53) */
#endif

static int fb_fd = -1;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static uint8_t *mapped_fb = NULL;

static int red_shift = 16;
static int green_shift = 8;
static int blue_shift = 0;

fb_data *framebuffer = NULL; /* global variable referenced by lcd-memframe / lcd-target.h */

static void klog(const char *msg)
{
    int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0) {
        write(kfd, msg, strlen(msg));
        close(kfd);
    }
    write(1, msg, strlen(msg));
}

static inline uint32_t rgb565_to_xrgb8888(fb_data px)
{
    uint32_t r = (px >> 11) & 0x1F;
    uint32_t g = (px >> 5) & 0x3F;
    uint32_t b = px & 0x1F;

    /* Expand 5/6 bits to 8 bits */
    uint32_t r8 = (r << 3) | (r >> 2);
    uint32_t g8 = (g << 2) | (g >> 4);
    uint32_t b8 = (b << 3) | (b >> 2);

    return 0xFF000000 | (r8 << red_shift) | (g8 << green_shift) | (b8 << blue_shift);
}

static void redraw_hw(void)
{
    if (fb_fd < 0) return;
    vinfo.yoffset = 0;
    vinfo.xoffset = 0;
    ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
}

void lcd_init_device(void)
{
    char logbuf[128];
    const char *devs[] = {"/dev/graphics/fb0", "/dev/fb0", NULL};

    for (int i = 0; devs[i]; i++) {
        fb_fd = open(devs[i], O_RDWR | O_CLOEXEC);
        if (fb_fd >= 0) {
            snprintf(logbuf, sizeof(logbuf), "[lcd-y1] Opened %s (fd=%d)\n", devs[i], fb_fd);
            klog(logbuf);
            break;
        }
    }

    if (fb_fd < 0) {
        klog("[lcd-y1] ERROR: Failed to open /dev/graphics/fb0 or /dev/fb0!\n");
        return;
    }

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        klog("[lcd-y1] ERROR: FBIOGET_FSCREENINFO failed\n");
    }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        klog("[lcd-y1] ERROR: FBIOGET_VSCREENINFO failed\n");
    }

    snprintf(logbuf, sizeof(logbuf), "[lcd-y1] Hardware FB: %ux%u, %u bpp, line_len=%u, smem_len=%u\n",
             vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length, finfo.smem_len);
    klog(logbuf);

    if (vinfo.bits_per_pixel == 32) {
        if (vinfo.red.length > 0) {
            red_shift = vinfo.red.offset;
            green_shift = vinfo.green.offset;
            blue_shift = vinfo.blue.offset;
        } else {
            red_shift = 16;
            green_shift = 8;
            blue_shift = 0;
        }
        snprintf(logbuf, sizeof(logbuf), "[lcd-y1] 32bpp shifts: R=%d G=%d B=%d\n", red_shift, green_shift, blue_shift);
        klog(logbuf);
    }

    /* Unblank the display hardware */
    ioctl(fb_fd, FBIOBLANK, VESA_NO_BLANKING);

    /* Map framebuffer into memory */
    size_t map_size = finfo.smem_len ? finfo.smem_len : (vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8));
    mapped_fb = (uint8_t *)mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (mapped_fb == MAP_FAILED) {
        mapped_fb = NULL;
        klog("[lcd-y1] ERROR: mmap framebuffer failed!\n");
        return;
    }

    /* Preserve boot splash in mapped_fb until first Rockbox lcd_update() */
    framebuffer = FBADDR(0, 0);

#ifdef HAVE_LCD_ENABLE
    lcd_set_active(true);
#endif
    klog("[lcd-y1] LCD device initialized successfully.\n");
}

void lcd_enable(bool on)
{
    if (fb_fd < 0) return;
    if (lcd_active() == on) return;

    lcd_set_active(on);
    if (on) {
        ioctl(fb_fd, MTKFB_POWERON, 0);
        ioctl(fb_fd, FBIOBLANK, VESA_NO_BLANKING);
        send_event(LCD_EVENT_ACTIVATION, NULL);
        lcd_update();
    } else {
        ioctl(fb_fd, MTKFB_POWEROFF, 0);
        ioctl(fb_fd, FBIOBLANK, VESA_POWERDOWN);
    }
}

static bool y1_always_on_enabled = false;
static bool y1_display_powersave = false;
static long last_powersave_tick = 0;

void y1_set_always_on_screen(bool on)
{
    y1_always_on_enabled = on;
}

bool y1_is_always_on_screen(void)
{
    return y1_always_on_enabled;
}

bool y1_should_always_on(void)
{
    if (!global_settings.always_on_screen)
        return false;

    if (global_settings.aod_wps_only && get_current_activity() != ACTIVITY_WPS)
        return false;

    if (global_settings.aod_playback_only)
    {
        int stat = audio_status();
        if (!((stat & AUDIO_STATUS_PLAY) && !(stat & AUDIO_STATUS_PAUSE)))
            return false;
    }

    return true;
}

void y1_set_display_power_saving(bool enable)
{
    y1_display_powersave = enable;
    if (fb_fd >= 0) {
        if (enable) {
            unsigned long speed = 1;
            ioctl(fb_fd, MTKFB_CHANGE_UPDATESPEED, &speed);
            last_powersave_tick = 0;
            lcd_update();
        } else {
            ioctl(fb_fd, MTKFB_RESET_UPDATESPEED, 0);
            lcd_update();
        }
    }
}

bool y1_is_display_power_saving(void)
{
    return y1_display_powersave;
}

static int y1_resolution_mode = 0; /* 0 = Normal (480x360), 1 = Classic (320x240 stretched) */
static uint16_t scale_x[480];
static uint16_t scale_y[360];
static bool tables_inited = false;

static void init_scale_tables(void)
{
    if (tables_inited) return;
    for (int x = 0; x < 480; x++) {
        scale_x[x] = (x * 2) / 3; /* 0..479 -> 0..319 */
    }
    for (int y = 0; y < 360; y++) {
        scale_y[y] = (y * 2) / 3; /* 0..359 -> 0..239 */
    }
    tables_inited = true;
}

void y1_set_resolution_setting(int mode)
{
    y1_resolution_mode = mode;
    init_scale_tables();
}

void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void lcd_update_rect(int x, int y, int width, int height)
{
    if (fb_fd < 0 || !mapped_fb) return;

#ifdef HAVE_LCD_ENABLE
    if (!lcd_active()) return;
#endif

    if (y1_display_powersave) {
        if (!y1_should_always_on()) {
            y1_set_display_power_saving(false);
            backlight_hw_off();
            return;
        }
        /* When display is in Always-On Screen power saving mode:
         * Rate-limit updates to at most once per second (1 FPS).
         * This saves 97% of memory bus bandwidth and GPU/DDP power consumption. */
        if (last_powersave_tick != 0 && TIME_BEFORE(current_tick, last_powersave_tick + HZ)) {
            return;
        }
        last_powersave_tick = current_tick;
    }

    int stride = finfo.line_length;
    if (stride == 0) {
        stride = vinfo.xres * (vinfo.bits_per_pixel / 8);
    }

    fb_data *src_base = FBADDR(0, 0);
    if (!src_base) return;

    if (y1_resolution_mode == 1) {
        init_scale_tables();
        if (vinfo.bits_per_pixel == 32) {
            for (int r = 0; r < 360; r++) {
                const fb_data *src_row = src_base + scale_y[r] * LCD_WIDTH;
                uint32_t *dst_row = (uint32_t *)(mapped_fb + r * stride);
                for (int c = 0; c < 480; c++) {
                    dst_row[c] = rgb565_to_xrgb8888(src_row[scale_x[c]]);
                }
            }
        } else {
            for (int r = 0; r < 360; r++) {
                const fb_data *src_row = src_base + scale_y[r] * LCD_WIDTH;
                uint16_t *dst_row = (uint16_t *)(mapped_fb + r * stride);
                for (int c = 0; c < 480; c++) {
                    dst_row[c] = src_row[scale_x[c]];
                }
            }
        }
        redraw_hw();
        return;
    }

    /* Clamp boundaries */
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > LCD_WIDTH) width = LCD_WIDTH - x;
    if (y + height > LCD_HEIGHT) height = LCD_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    if (vinfo.bits_per_pixel == 32) {
        /* Convert 16-bit RGB565 rockbox buffer to 32-bit ARGB8888 hardware buffer */
        for (int r = y; r < y + height; r++) {
            const fb_data *src_row = src_base + r * LCD_WIDTH + x;
            uint32_t *dst_row = (uint32_t *)(mapped_fb + r * stride + x * 4);
            for (int c = 0; c < width; c++) {
                dst_row[c] = rgb565_to_xrgb8888(src_row[c]);
            }
        }
    } else {
        /* 16-bit hardware buffer (direct copy) */
        for (int r = y; r < y + height; r++) {
            const fb_data *src_row = src_base + r * LCD_WIDTH + x;
            uint8_t *dst_row = mapped_fb + r * stride + x * sizeof(fb_data);
            memcpy(dst_row, src_row, width * sizeof(fb_data));
        }
    }

    redraw_hw();
}

#ifdef HAVE_LCD_SHUTDOWN
void lcd_shutdown(void)
{
    if (mapped_fb && fb_fd >= 0) {
        size_t map_size = finfo.smem_len ? finfo.smem_len : (vinfo.xres * vinfo.yres * 4);
        munmap(mapped_fb, map_size);
        mapped_fb = NULL;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
}
#endif

void y1_display_raw_screen(const char *name)
{
    char path[128];
    int img_fd = -1;

    /* Check rootfs asset first */
    snprintf(path, sizeof(path), "/etc/%s.raw", name);
    img_fd = open(path, O_RDONLY | O_CLOEXEC);

    /* Fallbacks on storage */
    if (img_fd < 0) {
        snprintf(path, sizeof(path), "/.rockbox/%s.raw", name);
        img_fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (img_fd < 0) {
        snprintf(path, sizeof(path), "/mnt/sdcard/%s.raw", name);
        img_fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (img_fd < 0) {
        snprintf(path, sizeof(path), "/mnt/sdcard/.rockbox/%s.raw", name);
        img_fd = open(path, O_RDONLY | O_CLOEXEC);
    }

    if (img_fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[lcd-y1] Screen '%s.raw' not found\n", name);
        klog(msg);
        return;
    }

    if (fb_fd < 0 || !mapped_fb) {
        lcd_init_device();
    }
    if (fb_fd < 0 || !mapped_fb) {
        close(img_fd);
        return;
    }

    int stride = finfo.line_length ? finfo.line_length : (vinfo.xres * (vinfo.bits_per_pixel / 8));
    int w = vinfo.xres;
    int h = vinfo.yres;
    fb_data *fb16 = FBADDR(0, 0);

    if (vinfo.bits_per_pixel == 32) {
        if (stride == w * 4) {
            ssize_t rb = read(img_fd, mapped_fb, w * h * 4);
            (void)rb;
        } else {
            for (int r = 0; r < h; r++) {
                ssize_t rb = read(img_fd, mapped_fb + r * stride, w * 4);
                if (rb <= 0) break;
            }
        }
        /* Synchronize Rockbox internal 16-bit framebuffer so UI doesn't immediately overwrite with old content */
        if (fb16) {
            for (int r = 0; r < h; r++) {
                uint32_t *row32 = (uint32_t *)(mapped_fb + r * stride);
                for (int c = 0; c < w; c++) {
                    uint32_t px = row32[c];
                    uint16_t r5 = (px >> 19) & 0x1F;
                    uint16_t g6 = (px >> 10) & 0x3F;
                    uint16_t b5 = (px >> 3) & 0x1F;
                    fb16[r * LCD_WIDTH + c] = (r5 << 11) | (g6 << 5) | b5;
                }
            }
        }
    } else {
        uint32_t line_buf[480];
        for (int r = 0; r < h; r++) {
            ssize_t rb = read(img_fd, line_buf, sizeof(line_buf));
            if (rb <= 0) break;
            uint16_t *row16 = (uint16_t *)(mapped_fb + r * stride);
            for (int c = 0; c < w; c++) {
                uint32_t px = line_buf[c];
                uint16_t r5 = (px >> 19) & 0x1F;
                uint16_t g6 = (px >> 10) & 0x3F;
                uint16_t b5 = (px >> 3) & 0x1F;
                uint16_t val16 = (r5 << 11) | (g6 << 5) | b5;
                row16[c] = val16;
                if (fb16) fb16[r * LCD_WIDTH + c] = val16;
            }
        }
    }
    close(img_fd);
    redraw_hw();
}


