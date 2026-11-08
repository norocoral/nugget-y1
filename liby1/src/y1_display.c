/***************************************************************************
 * liby1 - Framebuffer Display, Backlight & Early-Suspend Driver (MT6572)
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "liby1.h"

/* MediaTek Hardware Framebuffer IOCTLs */
#ifndef MTKFB_POWERON
#define MTKFB_POWERON  0x4f48
#endif
#ifndef MTKFB_POWEROFF
#define MTKFB_POWEROFF 0x4f49
#endif

#define Y1_BL_SYSFS_PATH "/sys/class/leds/lcd-backlight/brightness"

static int fb_fd = -1;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static uint8_t *mapped_fb = NULL;

static int red_shift = 16;
static int green_shift = 8;
static int blue_shift = 0;

static uint32_t rgb565_lut[65536] __attribute__((aligned(64)));
static bool lut_inited = false;
static uint32_t sw_backbuf[Y1_FB_WIDTH * Y1_FB_HEIGHT] __attribute__((aligned(64)));

static int  bl_fd = -1;
static int  last_bl_hw_val = -1;
static bool early_suspended = false;

static inline uint32_t rgb565_to_xrgb8888_calc(uint32_t px)
{
    uint32_t r = (px >> 11) & 0x1F;
    uint32_t g = (px >> 5) & 0x3F;
    uint32_t b = px & 0x1F;

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

uint32_t y1_fb_rgb565_to_xrgb8888(uint16_t px)
{
    if (!lut_inited)
        init_rgb565_lut();
    return rgb565_lut[px];
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

int y1_fb_init(void)
{
    if (fb_fd >= 0 && mapped_fb)
        return 0;

    const char *devs[] = {"/dev/graphics/fb0", "/dev/fb0", NULL};
    for (int i = 0; devs[i]; i++) {
        fb_fd = open(devs[i], O_RDWR | O_CLOEXEC);
        if (fb_fd >= 0) {
            y1_logf("[liby1-fb] Opened %s (fd=%d)\n", devs[i], fb_fd);
            break;
        }
    }

    if (fb_fd < 0) {
        y1_log("[liby1-fb] ERROR: Failed to open /dev/graphics/fb0 or /dev/fb0!\n");
        return -1;
    }

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        y1_log("[liby1-fb] ERROR: FBIOGET_FSCREENINFO failed\n");
    }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        y1_log("[liby1-fb] ERROR: FBIOGET_VSCREENINFO failed\n");
    }

    y1_logf("[liby1-fb] Hardware FB: %ux%u, %u bpp, line_len=%u, smem_len=%u\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length, finfo.smem_len);

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
    }

    init_rgb565_lut();

    /* Power on MT6572 display controller */
    int r_pon = ioctl(fb_fd, MTKFB_POWERON);
    y1_logf("[liby1-fb] MTKFB_POWERON ret=%d (errno=%d)\n", r_pon, errno);

    size_t map_size = finfo.smem_len ? finfo.smem_len : (vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8));
    mapped_fb = (uint8_t *)mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (mapped_fb == MAP_FAILED) {
        mapped_fb = NULL;
        close(fb_fd);
        fb_fd = -1;
        y1_log("[liby1-fb] ERROR: mmap framebuffer failed!\n");
        return -1;
    }

    y1_fb_flip();
    return 0;
}

void y1_fb_close(void)
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
    if (bl_fd >= 0) {
        close(bl_fd);
        bl_fd = -1;
    }
}

bool y1_fb_is_ready(void)
{
    return (fb_fd >= 0 && mapped_fb != NULL);
}

int y1_fb_width(void)
{
    return (fb_fd >= 0 && vinfo.xres) ? (int)vinfo.xres : Y1_FB_WIDTH;
}

int y1_fb_height(void)
{
    return (fb_fd >= 0 && vinfo.yres) ? (int)vinfo.yres : Y1_FB_HEIGHT;
}

int y1_fb_bpp(void)
{
    return (fb_fd >= 0 && vinfo.bits_per_pixel) ? (int)vinfo.bits_per_pixel : 32;
}

int y1_fb_stride(void)
{
    if (fb_fd >= 0 && finfo.line_length > 0)
        return (int)finfo.line_length;
    return y1_fb_width() * (y1_fb_bpp() / 8);
}

uint8_t *y1_fb_mmap_ptr(void)
{
    return mapped_fb;
}

void y1_fb_flip(void)
{
    if (fb_fd < 0)
        return;
    vinfo.yoffset = 0;
    vinfo.xoffset = 0;
    vinfo.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    if (ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo) < 0) {
        y1_logf("[liby1-fb] FBIOPAN_DISPLAY failed (errno=%d)\n", errno);
    }
}

void y1_fb_set_eco_mode(bool eco_on)
{
    y1_sysfs_write("/proc/dispsys/eco_mode", eco_on ? "1\n" : "0\n");
}

void y1_fb_blit_rgb565_rect(const uint16_t *src_base, int src_stride_px,
                            int x, int y, int width, int height,
                            y1_pixel_blend_fn blend_cb, void *blend_user)
{
    if (fb_fd < 0 || !mapped_fb || !src_base)
        return;

    if (!lut_inited)
        init_rgb565_lut();

    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > Y1_FB_WIDTH) width = Y1_FB_WIDTH - x;
    if (y + height > Y1_FB_HEIGHT) height = Y1_FB_HEIGHT - y;
    if (width <= 0 || height <= 0)
        return;

    int stride = y1_fb_stride();

    if (vinfo.bits_per_pixel == 32) {
        for (int r = y; r < y + height; r++) {
            const uint16_t *src_row = src_base + r * src_stride_px + x;
            uint32_t *dst_row = &sw_backbuf[r * Y1_FB_WIDTH + x];
            if (blend_cb) {
                for (int c = 0; c < width; c++) {
                    uint16_t px = blend_cb(x + c, r, src_row[c], blend_user);
                    dst_row[c] = rgb565_lut[px];
                }
            } else {
                for (int c = 0; c < width; c++) {
                    dst_row[c] = rgb565_lut[src_row[c]];
                }
            }
        }
        if (x == 0 && width == Y1_FB_WIDTH && stride == Y1_FB_WIDTH * 4) {
            memcpy(mapped_fb + y * stride,
                   &sw_backbuf[y * Y1_FB_WIDTH],
                   (size_t)height * (Y1_FB_WIDTH * 4));
        } else {
            for (int r = y; r < y + height; r++) {
                memcpy(mapped_fb + r * stride + x * 4,
                       &sw_backbuf[r * Y1_FB_WIDTH + x],
                       (size_t)width * 4);
            }
        }
    } else {
        for (int r = y; r < y + height; r++) {
            const uint16_t *src_row = src_base + r * src_stride_px + x;
            uint16_t *dst_row = (uint16_t *)(mapped_fb + r * stride + x * 2);
            if (blend_cb) {
                for (int c = 0; c < width; c++) {
                    dst_row[c] = blend_cb(x + c, r, src_row[c], blend_user);
                }
            } else {
                memcpy(dst_row, src_row, (size_t)width * 2);
            }
        }
    }

    y1_fb_flip();
}

void y1_fb_blit_rgb565_classic_scaled(const uint16_t *src_base, int src_stride_px,
                                      int x, int y, int width, int height,
                                      y1_pixel_blend_fn blend_cb, void *blend_user)
{
    if (fb_fd < 0 || !mapped_fb || !src_base)
        return;

    if (!lut_inited)
        init_rgb565_lut();

    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > Y1_CLASSIC_WIDTH) width = Y1_CLASSIC_WIDTH - x;
    if (y + height > Y1_CLASSIC_HEIGHT) height = Y1_CLASSIC_HEIGHT - y;
    if (width <= 0 || height <= 0)
        return;

    int sy0 = (y / 2) * 2;
    int sy1 = ((y + height + 1) / 2) * 2;
    if (sy1 > Y1_CLASSIC_HEIGHT) sy1 = Y1_CLASSIC_HEIGHT;
    int dy0 = (sy0 * 3) / 2;
    int dy1 = (sy1 * 3) / 2;

    int stride = y1_fb_stride();

    if (vinfo.bits_per_pixel == 32) {
        for (int sy = sy0, dy = dy0; sy < sy1; sy += 2, dy += 3) {
            const uint16_t *srow0 = src_base + sy * src_stride_px;
            const uint16_t *srow1 = src_base + (sy + 1) * src_stride_px;
            uint32_t *drow0 = &sw_backbuf[dy * Y1_FB_WIDTH];
            uint32_t *drow1 = &sw_backbuf[(dy + 1) * Y1_FB_WIDTH];
            uint32_t *drow2 = &sw_backbuf[(dy + 2) * Y1_FB_WIDTH];

            for (int sx = 0, dx = 0; sx < Y1_CLASSIC_WIDTH; sx += 2, dx += 3) {
                uint16_t p_t0 = srow0[sx];
                uint16_t p_t1 = srow0[sx + 1];
                uint16_t p_b0 = srow1[sx];
                uint16_t p_b1 = srow1[sx + 1];
                if (blend_cb) {
                    p_t0 = blend_cb(sx, sy, p_t0, blend_user);
                    p_t1 = blend_cb(sx + 1, sy, p_t1, blend_user);
                    p_b0 = blend_cb(sx, sy + 1, p_b0, blend_user);
                    p_b1 = blend_cb(sx + 1, sy + 1, p_b1, blend_user);
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

        if (stride == Y1_FB_WIDTH * 4) {
            memcpy(mapped_fb + dy0 * stride,
                   &sw_backbuf[dy0 * Y1_FB_WIDTH],
                   (size_t)(dy1 - dy0) * (Y1_FB_WIDTH * 4));
        } else {
            for (int r = dy0; r < dy1; r++) {
                memcpy(mapped_fb + r * stride, &sw_backbuf[r * Y1_FB_WIDTH], Y1_FB_WIDTH * 4);
            }
        }
    } else {
        uint16_t *sw_backbuf16 = (uint16_t *)sw_backbuf;
        for (int sy = sy0, dy = dy0; sy < sy1; sy += 2, dy += 3) {
            const uint16_t *srow0 = src_base + sy * src_stride_px;
            const uint16_t *srow1 = src_base + (sy + 1) * src_stride_px;
            uint16_t *drow0 = &sw_backbuf16[dy * Y1_FB_WIDTH];
            uint16_t *drow1 = &sw_backbuf16[(dy + 1) * Y1_FB_WIDTH];
            uint16_t *drow2 = &sw_backbuf16[(dy + 2) * Y1_FB_WIDTH];

            for (int sx = 0, dx = 0; sx < Y1_CLASSIC_WIDTH; sx += 2, dx += 3) {
                uint16_t t0 = srow0[sx];
                uint16_t t1 = srow0[sx + 1];
                uint16_t b0 = srow1[sx];
                uint16_t b1 = srow1[sx + 1];
                if (blend_cb) {
                    t0 = blend_cb(sx, sy, t0, blend_user);
                    t1 = blend_cb(sx + 1, sy, t1, blend_user);
                    b0 = blend_cb(sx, sy + 1, b0, blend_user);
                    b1 = blend_cb(sx + 1, sy + 1, b1, blend_user);
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
            memcpy(mapped_fb + r * stride, &sw_backbuf16[r * Y1_FB_WIDTH], Y1_FB_WIDTH * 2);
        }
    }

    y1_fb_flip();
}

void y1_fb_present_rgb565(const uint16_t *pixels_480x360)
{
    y1_fb_blit_rgb565_rect(pixels_480x360, Y1_FB_WIDTH, 0, 0, Y1_FB_WIDTH, Y1_FB_HEIGHT, NULL, NULL);
}

void y1_fb_present_xrgb8888(const uint32_t *pixels_480x360)
{
    if (fb_fd < 0 || !mapped_fb || !pixels_480x360)
        return;

    int stride = y1_fb_stride();
    if (vinfo.bits_per_pixel == 32) {
        if (stride == Y1_FB_WIDTH * 4) {
            memcpy(mapped_fb, pixels_480x360, Y1_FB_WIDTH * Y1_FB_HEIGHT * 4);
        } else {
            for (int r = 0; r < Y1_FB_HEIGHT; r++) {
                memcpy(mapped_fb + r * stride, pixels_480x360 + r * Y1_FB_WIDTH, Y1_FB_WIDTH * 4);
            }
        }
        y1_fb_flip();
    }
}

bool y1_fb_display_raw_screen(const char *name, uint16_t *out_fb16,
                              int fb16_stride_px, bool classic_320x240)
{
    if (!name)
        return false;

    char path[160];
    int img_fd = -1;

    const char *dirs[] = {
        "/.system",
        "/mnt/sdcard/.system",
        "/.rockbox",
        "/mnt/sdcard/.rockbox",
        "/mnt/sdcard",
        "/etc",
        NULL
    };

    for (int i = 0; dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s.raw", dirs[i], name);
        img_fd = open(path, O_RDONLY | O_CLOEXEC);
        if (img_fd >= 0)
            break;
    }

    if (img_fd < 0) {
        y1_logf("[liby1-fb] Screen '%s.raw' not found\n", name);
        return false;
    }

    if (fb_fd < 0 || !mapped_fb) {
        y1_fb_init();
    }
    if (fb_fd < 0 || !mapped_fb) {
        close(img_fd);
        return false;
    }

    int stride = y1_fb_stride();
    int w = y1_fb_width();
    int h = y1_fb_height();
    if (fb16_stride_px <= 0)
        fb16_stride_px = Y1_FB_WIDTH;

    if (vinfo.bits_per_pixel == 32) {
        if (stride == w * 4) {
            ssize_t rb = read(img_fd, mapped_fb, (size_t)(w * h * 4));
            (void)rb;
        } else {
            for (int r = 0; r < h; r++) {
                ssize_t rb = read(img_fd, mapped_fb + r * stride, (size_t)(w * 4));
                if (rb <= 0)
                    break;
            }
        }
        if (out_fb16) {
            if (classic_320x240) {
                for (int r = 0; r < Y1_CLASSIC_HEIGHT; r++) {
                    int sr = (r * 3) / 2;
                    uint32_t *row32 = (uint32_t *)(mapped_fb + sr * stride);
                    for (int c = 0; c < Y1_CLASSIC_WIDTH; c++) {
                        int sc = (c * 3) / 2;
                        uint32_t px = row32[sc];
                        uint16_t r5 = (px >> 19) & 0x1F;
                        uint16_t g6 = (px >> 10) & 0x3F;
                        uint16_t b5 = (px >> 3) & 0x1F;
                        out_fb16[r * fb16_stride_px + c] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
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
                        out_fb16[r * fb16_stride_px + c] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                    }
                }
            }
        }
    } else {
        uint32_t line_buf[Y1_FB_WIDTH];
        for (int r = 0; r < h; r++) {
            ssize_t rb = read(img_fd, line_buf, sizeof(line_buf));
            if (rb <= 0)
                break;
            uint16_t *row16 = (uint16_t *)(mapped_fb + r * stride);
            for (int c = 0; c < w; c++) {
                uint32_t px = line_buf[c];
                uint16_t r5 = (px >> 19) & 0x1F;
                uint16_t g6 = (px >> 10) & 0x3F;
                uint16_t b5 = (px >> 3) & 0x1F;
                uint16_t val16 = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                row16[c] = val16;
                if (out_fb16)
                    out_fb16[r * fb16_stride_px + c] = val16;
            }
        }
    }

    close(img_fd);
    y1_fb_flip();
    return true;
}

void y1_backlight_set_raw(int val)
{
    if (val < 0) val = 0;
    if (val > 255) val = 255;

    if (val == last_bl_hw_val && bl_fd >= 0)
        return;

    if (bl_fd < 0) {
        bl_fd = open(Y1_BL_SYSFS_PATH, O_WRONLY | O_CLOEXEC);
    }
    if (bl_fd >= 0) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d\n", val);
        lseek(bl_fd, 0, SEEK_SET);
        ssize_t w = write(bl_fd, buf, (size_t)len);
        if (w < 0) {
            close(bl_fd);
            bl_fd = open(Y1_BL_SYSFS_PATH, O_WRONLY | O_CLOEXEC);
            if (bl_fd >= 0)
                w = write(bl_fd, buf, (size_t)len);
        }
        if (w >= 0)
            last_bl_hw_val = val;
    } else {
        y1_logf("[liby1-bl] ERROR: failed to open %s (errno=%d)\n", Y1_BL_SYSFS_PATH, errno);
    }
}

void y1_backlight_set_percent(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    y1_backlight_set_raw((pct * 255) / 100);
}

int y1_backlight_get_raw(void)
{
    return last_bl_hw_val;
}

void y1_display_enter_early_suspend(void)
{
    /* Hold userspace wakelock so kernel autosleep does not freeze userspace
     * threads while writing 'mem' to /sys/power/state to run early_suspend:
     * - mtkfb_early_suspend: powers off LCD panel, display engine, 60Hz VSync,
     *   and gates off all 25 MMSYS0/MMSYS1/DBI display clocks
     * - tpd_suspend: puts APT32F touch controller into low-power sleep
     * - mali_early_suspend_handler: powers off Mali-400 GPU */
    y1_sysfs_write("/sys/power/wake_lock", "rockbox_active\n");

    y1_sysfs_write("/proc/spm_fs/dpidle", "1 cpu_pdn\n");
    y1_sysfs_write("/proc/spm_fs/dpidle", "1 infra_pdn\n");
    y1_sysfs_write("/proc/spm_fs/dpidle", "1 pwrlevel\n");
    y1_sysfs_write("/sys/power/idle_state", "1\n");
    y1_sysfs_write("/sys/power/dpidle_state", "cpupdn 1\n");
    y1_sysfs_write("/sys/power/dpidle_state", "time 0\n");
    y1_sysfs_write("/sys/power/dpidle_state", "bypass 1\n");
    y1_sysfs_write("/sys/power/dpidle_state", "1\n");
    y1_sysfs_write("/sys/power/rgidle_state", "1\n");

    if (y1_sysfs_write("/sys/power/state", "mem\n")) {
        early_suspended = true;
        last_bl_hw_val = -1;
        y1_log("[liby1-bl] entered kernel early_suspend ('mem' -> /sys/power/state)\n");
    }
}

void y1_display_exit_early_suspend(void)
{
    y1_sysfs_write("/sys/power/dpidle_state", "bypass 0\n");
    if (y1_sysfs_write("/sys/power/state", "on\n")) {
        y1_log("[liby1-bl] wrote 'on' to /sys/power/state\n");
    }
    if (early_suspended) {
        /* Wait for mtkfb_late_resume (which has a 20ms internal msleep) to finish
         * enabling display clocks and clearing is_early_suspended before FB/BL writes */
        usleep(35000);
        early_suspended = false;
        last_bl_hw_val = -1;
    }
    y1_input_set_touch_scanning(true);
}

bool y1_display_is_early_suspended(void)
{
    return early_suspended;
}

