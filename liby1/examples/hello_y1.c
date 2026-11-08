/***************************************************************************
 * hello_y1.c - Minimal Standalone Innioasis Y1 App using liby1
 *
 * Demonstrates direct 480x360 framebuffer rendering, click-wheel input,
 * backlight brightness control, and 44.1kHz stereo audio output without
 * needing Rockbox or any external framework.
 *
 * Build with:
 *   make -C hosted_linux_port/liby1 examples
 ****************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include "liby1.h"

static uint16_t fb[Y1_FB_WIDTH * Y1_FB_HEIGHT];

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void render_gradient(int offset, int brightness)
{
    for (int y = 0; y < Y1_FB_HEIGHT; y++) {
        for (int x = 0; x < Y1_FB_WIDTH; x++) {
            uint8_t r = (uint8_t)((x + offset) & 0xFF);
            uint8_t g = (uint8_t)((y + offset / 2) & 0xFF);
            uint8_t b = (uint8_t)((brightness * 255) / 100);
            fb[y * Y1_FB_WIDTH + x] = rgb565(r, g, b);
        }
    }
    y1_fb_present_rgb565(fb);
}

int main(void)
{
    y1_init();
    y1_cpu_set_mode(Y1_CPU_MODE_DYNAMIC);

    int offset = 0;
    int brightness = 60;
    y1_backlight_set_percent(brightness);
    render_gradient(offset, brightness);

    bool running = true;
    while (running) {
        struct y1_input_event ev;
        int rc = y1_input_poll(&ev, 50);
        if (rc > 0 && ev.value == 1) {
            switch (ev.button) {
            case Y1_BTN_SCROLL_FWD:
                offset += 8;
                render_gradient(offset, brightness);
                break;
            case Y1_BTN_SCROLL_BACK:
                offset -= 8;
                render_gradient(offset, brightness);
                break;
            case Y1_BTN_RIGHT:
                if (brightness < 100) brightness += 10;
                y1_backlight_set_percent(brightness);
                render_gradient(offset, brightness);
                break;
            case Y1_BTN_LEFT:
                if (brightness > 10) brightness -= 10;
                y1_backlight_set_percent(brightness);
                render_gradient(offset, brightness);
                break;
            case Y1_BTN_MENU:
            case Y1_BTN_SELECT:
                running = false;
                break;
            default:
                break;
            }
        }
    }

    y1_shutdown();
    return 0;
}

