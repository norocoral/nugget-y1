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

#define RB_FILESYSTEM_OS
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
#include "y1-log.h"
#include "bt-y1.h"
#include "sysfs.h"

#ifndef VESA_NO_BLANKING
#define VESA_NO_BLANKING 0
#endif
#ifndef VESA_POWERDOWN
#define VESA_POWERDOWN   4
#endif

/* MediaTek Hardware Framebuffer IOCTLs (from Y1 kernel vmlinux) */
#undef MTKFB_POWERON
#define MTKFB_POWERON                   0x4f48     /* _IO('O', 72) */
#undef MTKFB_POWEROFF
#define MTKFB_POWEROFF                  0x4f49     /* _IO('O', 73) */
#ifndef MTKFB_GET_DEFAULT_UPDATESPEED
#define MTKFB_GET_DEFAULT_UPDATESPEED   0x80044f48 /* _IOR('O', 72, unsigned int) */
#endif
#ifndef MTKFB_GET_CURR_UPDATESPEED
#define MTKFB_GET_CURR_UPDATESPEED      0x80044f49 /* _IOR('O', 73, unsigned int) */
#endif
#undef MTKFB_CHANGE_UPDATESPEED
#define MTKFB_CHANGE_UPDATESPEED        0x40044f4a /* _IOW('O', 74, unsigned int) */


static int fb_fd = -1;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static uint8_t *mapped_fb = NULL;

static int red_shift = 16;
static int green_shift = 8;
static int blue_shift = 0;

static uint32_t rgb565_lut[65536] __attribute__((aligned(64)));
static bool lut_inited = false;
static uint32_t sw_backbuf[480 * 360] __attribute__((aligned(64)));
#if LCD_DEPTH == 16
static fb_data navigation_old_frame[LCD_WIDTH * LCD_HEIGHT];
static long navigation_fade_start;
static bool navigation_fade_active;
static unsigned navigation_fade_alpha;
#endif

fb_data *framebuffer = NULL; /* global variable referenced by lcd-memframe / lcd-target.h */

static void klog(const char *msg)
{
    y1_log(msg);
}

static inline uint32_t rgb565_to_xrgb8888_calc(uint32_t px)
{
    uint32_t r = (px >> 11) & 0x1F;
    uint32_t g = (px >> 5) & 0x3F;
    uint32_t b = px & 0x1F;

    /* Expand 5/6 bits to 8 bits */
    uint32_t r8 = (r << 3) | (r >> 2);
    uint32_t g8 = (g << 2) | (g >> 4);
    uint32_t b8 = (b << 3) | (b >> 2);

    return 0xFF000000U | (r8 << red_shift) | (g8 << green_shift) | (b8 << blue_shift);
}

static void init_rgb565_lut(void)
{
    for (uint32_t i = 0; i < 65536; i++) {
        rgb565_lut[i] = rgb565_to_xrgb8888_calc(i);
    }
    lut_inited = true;
}

static inline uint32_t rgb565_to_xrgb8888(fb_data px)
{
    return rgb565_lut[(uint16_t)px];
}

/* Fast SWAR 24-bit RGB888 50/50 pixel average */
static inline uint32_t avg_xrgb8888(uint32_t a, uint32_t b)
{
    return (a & b) + (((a ^ b) & 0xFEFEFEFEU) >> 1);
}

/* Fast SWAR 16-bit RGB565 50/50 pixel average */
static inline uint16_t avg_rgb565(uint16_t a, uint16_t b)
{
    return (uint16_t)((a & b) + (((a ^ b) & 0xF7DEU) >> 1));
}

static void redraw_hw(void)
{
    if (fb_fd < 0) return;
    vinfo.yoffset = 0;
    vinfo.xoffset = 0;
    vinfo.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    int r = ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
    if (r < 0) {
        y1_logf("[lcd-y1] FBIOPAN_DISPLAY failed: ret=%d (errno=%d)\n", r, errno);
    }
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

    init_rgb565_lut();

    /* Ensure MT6572 display controller power is enabled */
    int r_pon = ioctl(fb_fd, MTKFB_POWERON);
    int err_pon = errno;
    y1_logf("[lcd-y1] lcd_init_device: MTKFB_POWERON ret=%d (errno=%d)\n",
            r_pon, err_pon);

    /* Map framebuffer into memory */
    size_t map_size = finfo.smem_len ? finfo.smem_len : (vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8));
    mapped_fb = (uint8_t *)mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (mapped_fb == MAP_FAILED) {
        mapped_fb = NULL;
        klog("[lcd-y1] ERROR: mmap framebuffer failed!\n");
        return;
    }

    redraw_hw();

    /* Preserve boot splash in mapped_fb until first Rockbox lcd_update() */
    framebuffer = FBADDR(0, 0);

#ifdef HAVE_LCD_ENABLE
    lcd_set_active(true);
#endif
    klog("[lcd-y1] LCD device initialized successfully.\n");
}

void lcd_enable(bool on)
{
    bool cur_act = lcd_active();
    if (cur_act == on)
        return;

    if (fb_fd < 0)
        return;

#if LCD_DEPTH == 16
    navigation_fade_active = false;
#endif
    lcd_set_active(on);
    if (on) {
        sysfs_set_string("/proc/dispsys/eco_mode", "0\n");
        send_event(LCD_EVENT_ACTIVATION, NULL);
        lcd_update();
    } else {
        sysfs_set_string("/proc/dispsys/eco_mode", "1\n");
    }
}

bool y1_playback_active(void)
{
    int stat = audio_status();
    if (y1_bt_is_receiver_mode()) {
        uint8_t play_status = y1_bt_receiver_get_play_status();
        return play_status == 1 || play_status == 3 || play_status == 4;
    }
    return (stat & AUDIO_STATUS_PLAY) && !(stat & AUDIO_STATUS_PAUSE);
}

static int y1_resolution_mode = 0; /* 0 = Normal (480x360), 1 = Classic (320x240 stretched) */

void y1_set_resolution_setting(int mode)
{
    y1_resolution_mode = mode;
}

void lcd_update(void)
{
    if (y1_resolution_mode == 1)
        lcd_update_rect(0, 0, 320, 240);
    else
        lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

#if LCD_DEPTH == 16
static long navigation_fade_duration(void)
{
    long duration = (HZ * 30 + 999) / 1000;
    return duration > 0 ? duration : 1;
}

void lcd_navigation_fade_capture(void)
{
    if (!global_settings.navigation_fade)
    {
        navigation_fade_active = false;
        return;
    }
    if (navigation_fade_active || !framebuffer)
        return;

    fb_data *src = FBADDR(0, 0);
    if (!src)
        return;

    memcpy(navigation_old_frame, src, sizeof(navigation_old_frame));
    navigation_fade_start = current_tick;
    navigation_fade_alpha = 0;
    navigation_fade_active = true;
}

static uint16_t navigation_blend_pixel(uint16_t old_pixel,
                                       uint16_t new_pixel)
{
    unsigned a = navigation_fade_alpha;
    unsigned inv_a = 256 - a;
    unsigned red = (((old_pixel >> 11) & 0x1f) * inv_a +
                    ((new_pixel >> 11) & 0x1f) * a) >> 8;
    unsigned green = (((old_pixel >> 5) & 0x3f) * inv_a +
                      ((new_pixel >> 5) & 0x3f) * a) >> 8;
    unsigned blue = ((old_pixel & 0x1f) * inv_a +
                     (new_pixel & 0x1f) * a) >> 8;
    return (red << 11) | (green << 5) | blue;
}
#endif

void lcd_update_rect(int x, int y, int width, int height)
{
    if (fb_fd < 0 || !mapped_fb) {
        y1_logf("[lcd-y1] lcd_update_rect skipped: fb_fd=%d mapped_fb=%p\n", fb_fd, mapped_fb);
        return;
    }

#ifdef HAVE_LCD_ENABLE
    if (!lcd_active()) {
        y1_logf("[lcd-y1] lcd_update_rect(%d,%d,%d,%d) skipped: !lcd_active\n", x, y, width, height);
        return;
    }
#endif

    if (!lut_inited) {
        init_rgb565_lut();
    }

#if LCD_DEPTH == 16
    bool blend_navigation = navigation_fade_active;
    if (blend_navigation)
    {
        if (!global_settings.navigation_fade)
        {
            navigation_fade_active = false;
            blend_navigation = false;
        }
        else
        {
            long duration = navigation_fade_duration();
            long elapsed = current_tick - navigation_fade_start;
            if (elapsed >= duration)
            {
                navigation_fade_alpha = 256;
            }
            else
                navigation_fade_alpha = (unsigned)(elapsed * 256 / duration);

            x = 0;
            y = 0;
            width = y1_resolution_mode == 1 ? 320 : LCD_WIDTH;
            height = y1_resolution_mode == 1 ? 240 : LCD_HEIGHT;
        }
    }
#else
    const bool blend_navigation = false;
#endif

redraw_navigation_fade:

    int stride = finfo.line_length;
    if (stride == 0) {
        stride = vinfo.xres * (vinfo.bits_per_pixel / 8);
    }

    fb_data *src_base = FBADDR(0, 0);
    if (!src_base) return;

    if (y1_resolution_mode == 1) {
        /* Clamp to 320x240 Classic bounds */
        if (x < 0) { width += x; x = 0; }
        if (y < 0) { height += y; y = 0; }
        if (x + width > 320) width = 320 - x;
        if (y + height > 240) height = 240 - y;
        if (width <= 0 || height <= 0) return;

        /* Align dirty vertical range to 2-row source blocks (3-row destination blocks) */
        int sy0 = (y / 2) * 2;
        int sy1 = ((y + height + 1) / 2) * 2;
        if (sy1 > 240) sy1 = 240;
        int dy0 = (sy0 * 3) / 2;
        int dy1 = (sy1 * 3) / 2;

        if (vinfo.bits_per_pixel == 32) {
            /* Smooth 2x2 -> 3x3 area-averaged bilinear interpolation in cached backbuffer */
            for (int sy = sy0, dy = dy0; sy < sy1; sy += 2, dy += 3) {
                const fb_data *srow0 = src_base + sy * LCD_WIDTH;
                const fb_data *srow1 = src_base + (sy + 1) * LCD_WIDTH;
                uint32_t *drow0 = &sw_backbuf[dy * 480];
                uint32_t *drow1 = &sw_backbuf[(dy + 1) * 480];
                uint32_t *drow2 = &sw_backbuf[(dy + 2) * 480];

                for (int sx = 0, dx = 0; sx < 320; sx += 2, dx += 3) {
                    uint16_t p_t0 = (uint16_t)srow0[sx];
                    uint16_t p_t1 = (uint16_t)srow0[sx + 1];
                    uint16_t p_b0 = (uint16_t)srow1[sx];
                    uint16_t p_b1 = (uint16_t)srow1[sx + 1];
                    if (blend_navigation && navigation_fade_alpha < 256)
                    {
                        p_t0 = navigation_blend_pixel(
                            (uint16_t)navigation_old_frame[sy * LCD_WIDTH + sx], p_t0);
                        p_t1 = navigation_blend_pixel(
                            (uint16_t)navigation_old_frame[sy * LCD_WIDTH + sx + 1], p_t1);
                        p_b0 = navigation_blend_pixel(
                            (uint16_t)navigation_old_frame[(sy + 1) * LCD_WIDTH + sx], p_b0);
                        p_b1 = navigation_blend_pixel(
                            (uint16_t)navigation_old_frame[(sy + 1) * LCD_WIDTH + sx + 1], p_b1);
                    }
                    uint32_t t0 = rgb565_lut[p_t0];
                    uint32_t t1 = rgb565_lut[p_t1];
                    uint32_t b0 = rgb565_lut[p_b0];
                    uint32_t b1 = rgb565_lut[p_b1];

                    uint32_t tm = avg_xrgb8888(t0, t1);
                    uint32_t bm = avg_xrgb8888(b0, b1);

                    drow0[dx]     = t0;
                    drow0[dx + 1] = tm;
                    drow0[dx + 2] = t1;

                    drow1[dx]     = avg_xrgb8888(t0, b0);
                    drow1[dx + 1] = avg_xrgb8888(tm, bm);
                    drow1[dx + 2] = avg_xrgb8888(t1, b1);

                    drow2[dx]     = b0;
                    drow2[dx + 1] = bm;
                    drow2[dx + 2] = b1;
                }
            }

            /* Burst-copy updated rows from cached RAM backbuffer to hardware framebuffer */
            if (stride == 480 * 4) {
                memcpy(mapped_fb + dy0 * stride,
                       &sw_backbuf[dy0 * 480],
                       (size_t)(dy1 - dy0) * (480 * 4));
            } else {
                for (int r = dy0; r < dy1; r++) {
                    memcpy(mapped_fb + r * stride, &sw_backbuf[r * 480], 480 * 4);
                }
            }
        } else {
            uint16_t *sw_backbuf16 = (uint16_t *)sw_backbuf;
            for (int sy = sy0, dy = dy0; sy < sy1; sy += 2, dy += 3) {
                const fb_data *srow0 = src_base + sy * LCD_WIDTH;
                const fb_data *srow1 = src_base + (sy + 1) * LCD_WIDTH;
                uint16_t *drow0 = &sw_backbuf16[dy * 480];
                uint16_t *drow1 = &sw_backbuf16[(dy + 1) * 480];
                uint16_t *drow2 = &sw_backbuf16[(dy + 2) * 480];

                for (int sx = 0, dx = 0; sx < 320; sx += 2, dx += 3) {
                    uint16_t t0 = (uint16_t)srow0[sx];
                    uint16_t t1 = (uint16_t)srow0[sx + 1];
                    uint16_t b0 = (uint16_t)srow1[sx];
                    uint16_t b1 = (uint16_t)srow1[sx + 1];
                    if (blend_navigation && navigation_fade_alpha < 256)
                    {
                        t0 = navigation_blend_pixel(
                            (uint16_t)navigation_old_frame[sy * LCD_WIDTH + sx], t0);
                        t1 = navigation_blend_pixel(
                            (uint16_t)navigation_old_frame[sy * LCD_WIDTH + sx + 1], t1);
                        b0 = navigation_blend_pixel(
                            (uint16_t)navigation_old_frame[(sy + 1) * LCD_WIDTH + sx], b0);
                        b1 = navigation_blend_pixel(
                            (uint16_t)navigation_old_frame[(sy + 1) * LCD_WIDTH + sx + 1], b1);
                    }

                    uint16_t tm = avg_rgb565(t0, t1);
                    uint16_t bm = avg_rgb565(b0, b1);

                    drow0[dx]     = t0;
                    drow0[dx + 1] = tm;
                    drow0[dx + 2] = t1;

                    drow1[dx]     = avg_rgb565(t0, b0);
                    drow1[dx + 1] = avg_rgb565(tm, bm);
                    drow1[dx + 2] = avg_rgb565(t1, b1);

                    drow2[dx]     = b0;
                    drow2[dx + 1] = bm;
                    drow2[dx + 2] = b1;
                }
            }
            for (int r = dy0; r < dy1; r++) {
                memcpy(mapped_fb + r * stride, &sw_backbuf16[r * 480], 480 * 2);
            }
        }
        redraw_hw();
#if LCD_DEPTH == 16
        if (navigation_fade_active)
        {
            sleep(1);
            long duration = navigation_fade_duration();
            long elapsed = current_tick - navigation_fade_start;
            if (elapsed < duration)
                navigation_fade_alpha = (unsigned)(elapsed * 256 / duration);
            else
            {
                navigation_fade_alpha = 256;
                navigation_fade_active = false;
            }
            goto redraw_navigation_fade;
        }
#endif
        return;
    }

    /* Clamp boundaries for Normal (480x360) mode */
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > LCD_WIDTH) width = LCD_WIDTH - x;
    if (y + height > LCD_HEIGHT) height = LCD_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    if (vinfo.bits_per_pixel == 32) {
        /* Convert 16-bit RGB565 rockbox buffer into cached 32-bit backbuffer using LUT */
        for (int r = y; r < y + height; r++) {
            const fb_data *src_row = src_base + r * LCD_WIDTH + x;
            uint32_t *dst_row = &sw_backbuf[r * 480 + x];
            for (int c = 0; c < width; c++) {
                uint16_t pixel = (uint16_t)src_row[c];
                if (blend_navigation && navigation_fade_alpha < 256)
                    pixel = navigation_blend_pixel(
                        (uint16_t)navigation_old_frame[r * LCD_WIDTH + x + c],
                        pixel);
                dst_row[c] = rgb565_lut[pixel];
            }
        }
        /* Burst-copy from cached backbuffer to hardware framebuffer to prevent tearing */
        if (x == 0 && width == 480 && stride == 480 * 4) {
            memcpy(mapped_fb + y * stride,
                   &sw_backbuf[y * 480],
                   (size_t)height * (480 * 4));
        } else {
            for (int r = y; r < y + height; r++) {
                memcpy(mapped_fb + r * stride + x * 4,
                       &sw_backbuf[r * 480 + x],
                       (size_t)width * 4);
            }
        }
    } else {
        /* 16-bit hardware buffer (direct copy) */
        for (int r = y; r < y + height; r++) {
            const fb_data *src_row = src_base + r * LCD_WIDTH + x;
            uint8_t *dst_row = mapped_fb + r * stride + x * sizeof(fb_data);
            if (blend_navigation && navigation_fade_alpha < 256)
            {
                uint16_t *dst = (uint16_t *)dst_row;
                for (int c = 0; c < width; c++)
                    dst[c] = navigation_blend_pixel(
                        (uint16_t)navigation_old_frame[r * LCD_WIDTH + x + c],
                        (uint16_t)src_row[c]);
            }
            else
                memcpy(dst_row, src_row, width * sizeof(fb_data));
        }
    }

    redraw_hw();
#if LCD_DEPTH == 16
    if (navigation_fade_active)
    {
        sleep(1);
        long duration = navigation_fade_duration();
        long elapsed = current_tick - navigation_fade_start;
        if (elapsed < duration)
            navigation_fade_alpha = (unsigned)(elapsed * 256 / duration);
        else
        {
            navigation_fade_alpha = 256;
            navigation_fade_active = false;
        }
        goto redraw_navigation_fade;
    }
#endif
}

#ifdef HAVE_LCD_SHUTDOWN
void lcd_shutdown(void)
{
    y1_logf("[lcd-y1] lcd_shutdown() called (mapped_fb=%p, fb_fd=%d)\n", mapped_fb, fb_fd);
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

bool y1_display_raw_screen(const char *name)
{
    char path[128];
    int img_fd = -1;

    /* 1. Check user custom splash on storage first */
    snprintf(path, sizeof(path), "/.rockbox/%s.raw", name);
    img_fd = open(path, O_RDONLY | O_CLOEXEC);

    if (img_fd < 0) {
        snprintf(path, sizeof(path), "/mnt/sdcard/.rockbox/%s.raw", name);
        img_fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (img_fd < 0) {
        snprintf(path, sizeof(path), "/mnt/sdcard/%s.raw", name);
        img_fd = open(path, O_RDONLY | O_CLOEXEC);
    }

    /* 2. Check built-in rootfs asset */
    if (img_fd < 0) {
        snprintf(path, sizeof(path), "/etc/%s.raw", name);
        img_fd = open(path, O_RDONLY | O_CLOEXEC);
    }

    if (img_fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[lcd-y1] Screen '%s.raw' not found\n", name);
        klog(msg);
        return false;
    }

    if (fb_fd < 0 || !mapped_fb) {
        lcd_init_device();
    }
    if (fb_fd < 0 || !mapped_fb) {
        close(img_fd);
        return false;
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
            if (y1_resolution_mode == 1) {
                for (int r = 0; r < 240; r++) {
                    int sr = (r * 3) / 2;
                    uint32_t *row32 = (uint32_t *)(mapped_fb + sr * stride);
                    for (int c = 0; c < 320; c++) {
                        int sc = (c * 3) / 2;
                        uint32_t px = row32[sc];
                        uint16_t r5 = (px >> 19) & 0x1F;
                        uint16_t g6 = (px >> 10) & 0x3F;
                        uint16_t b5 = (px >> 3) & 0x1F;
                        fb16[r * LCD_WIDTH + c] = (r5 << 11) | (g6 << 5) | b5;
                    }
                }
            } else {
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
    return true;
}
