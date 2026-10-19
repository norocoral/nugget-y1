/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2019 by Roman Stolyarov
 * Copyright (C) 2025 by Melissa Autumn
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#define RB_FILESYSTEM_OS
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include "config.h"
#include "backlight-target.h"
#include "sysfs.h"
#include "panic.h"
#include "lcd.h"

#if defined(BACKLIGHT_RG_NANO)
static const char * const sysfs_bl_brightness =
    "/sys/class/backlight/backlight/brightness";

static const char * const sysfs_bl_power =
    "/sys/class/backlight/backlight/bl_power";
#elif defined(BACKLIGHT_HIBY)
static const char * const sysfs_bl_brightness =
    "/sys/class/backlight/backlight_pwm0/brightness";

static const char * const sysfs_bl_power =
    /* Framebuffer powers off both touch (if available) and screen */
    "/sys/class/graphics/fb0/blank";

#elif defined(BACKLIGHT_INNIOASIS)
static const char * const sysfs_bl_brightness =
    "/sys/class/leds/lcd-backlight/brightness";

static const char * const sysfs_bl_power = NULL;
#else
static const char * const sysfs_bl_brightness =
    "/sys/class/backlight/pwm-backlight.0/brightness";

static const char * const sysfs_bl_power =
    "/sys/class/backlight/pwm-backlight.0/bl_power";
#endif

static int current_brightness = DEFAULT_BRIGHTNESS_SETTING;

bool backlight_hw_init(void)
{
    current_brightness = DEFAULT_BRIGHTNESS_SETTING;
    backlight_hw_on();
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
#ifdef HAVE_BUTTON_LIGHT
    buttonlight_hw_on();
#ifdef HAVE_BUTTONLIGHT_BRIGHTNESS
    buttonlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
#endif
#endif
    return true;
}

static int last_bl = -1;

/* Ref: https://www.kernel.org/doc/html/latest/gpu/backlight.html#c.backlight_properties */
#define BACKLIGHT_POWER_ON 0
#define BACKLIGHT_POWER_REDUCED 1
#define BACKLIGHT_POWER_OFF 4

#if defined(BACKLIGHT_INNIOASIS)
#include "cpufreq-y1.h"
#include "lcd-target.h"
extern void y1_enter_hold_mode(void);
static int bl_fd = -1;
static void write_bl_hw(int val)
{
    if (bl_fd < 0) {
        bl_fd = open(sysfs_bl_brightness, O_WRONLY | O_CLOEXEC);
    }
    if (bl_fd >= 0) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d\n", val);
        lseek(bl_fd, 0, SEEK_SET);
        (void)!write(bl_fd, buf, len);
    }
}
#endif

void backlight_hw_on(void)
{
    if (last_bl != BACKLIGHT_POWER_ON) {
#if defined(BACKLIGHT_INNIOASIS)
        if (y1_is_always_on_screen() || y1_is_display_power_saving()) {
            y1_set_display_power_saving(false);
        }
        y1_set_screen_off_power_mode(false);
#endif
#ifdef HAVE_LCD_ENABLE
        lcd_enable(true);
#endif
        last_bl = BACKLIGHT_POWER_ON;
        if (sysfs_bl_power)
            sysfs_set_int(sysfs_bl_power, last_bl);
#if defined(BACKLIGHT_INNIOASIS)
        int hw_val = current_brightness > 0 ? (current_brightness * 255) / 100 : (DEFAULT_BRIGHTNESS_SETTING * 255) / 100;
        write_bl_hw(hw_val);
        sysfs_set_int("/sys/class/leds/button-backlight/brightness", 255);
#endif
    }
}

void backlight_hw_off(void)
{
#if defined(BACKLIGHT_INNIOASIS)
    int target_bl = y1_should_always_on() ? BACKLIGHT_POWER_REDUCED : BACKLIGHT_POWER_OFF;
    if (last_bl != target_bl) {
        int prev_bl = last_bl;
        last_bl = target_bl;
        if (prev_bl == BACKLIGHT_POWER_ON || prev_bl == -1) {
            /* Apply HOLD mode immediately when screen goes idle */
            y1_enter_hold_mode();
            y1_set_screen_off_power_mode(true);
        }
        if (target_bl == BACKLIGHT_POWER_REDUCED) {
            /* Always On Screen mode:
             * Instead of turning off backlight completely:
             * 1. Ensure LCD is active (in case we are waking into AOD from screen-off)
             * 2. Reduce brightness to 50% of the current brightness
             * 3. Turn off button backlight
             * 4. Enable power saving mode on the display (1 FPS refresh rate)
             * Keep LCD panel active (do not suspend) */
#ifdef HAVE_LCD_ENABLE
            lcd_enable(true);
#endif
            int dimmed = current_brightness / 2;
            if (dimmed < 5) dimmed = 5;
            int hw_val = (dimmed * 255) / 100;
            write_bl_hw(hw_val);
            sysfs_set_int("/sys/class/leds/button-backlight/brightness", 0);
            y1_set_display_power_saving(true);
            return;
        }
        if (y1_is_display_power_saving()) {
            y1_set_display_power_saving(false);
        }
        write_bl_hw(0);
        sysfs_set_int("/sys/class/leds/button-backlight/brightness", 0);
        if (sysfs_bl_power)
            sysfs_set_int(sysfs_bl_power, last_bl);
#ifdef HAVE_LCD_ENABLE
        lcd_enable(false);
#endif
    }
#else
    if (last_bl != BACKLIGHT_POWER_REDUCED) {
        last_bl = BACKLIGHT_POWER_REDUCED;
        if (sysfs_bl_power)
            sysfs_set_int(sysfs_bl_power, last_bl);
#ifdef HAVE_LCD_ENABLE
        lcd_enable(false);
#endif
    }
#endif
}

#if defined(BACKLIGHT_INNIOASIS)
void y1_backlight_idle_check(void)
{
    if (last_bl == BACKLIGHT_POWER_REDUCED || last_bl == BACKLIGHT_POWER_OFF) {
        backlight_hw_off();
    }
}
#endif

void backlight_hw_brightness(int brightness)
{
    /* cap range, just in case */
    if (brightness > MAX_BRIGHTNESS_SETTING)
        brightness = MAX_BRIGHTNESS_SETTING;
    if (brightness < 0)
        brightness = 0;

    if (brightness > 0)
        current_brightness = brightness;

#if defined(BACKLIGHT_INNIOASIS)
    if (last_bl == BACKLIGHT_POWER_ON || last_bl == -1) {
        int hw_val = (brightness * 255) / 100;
        write_bl_hw(hw_val);
    } else if (last_bl == BACKLIGHT_POWER_REDUCED && y1_should_always_on()) {
        int dimmed = brightness / 2;
        if (dimmed < 5) dimmed = 5;
        write_bl_hw((dimmed * 255) / 100);
    }
#else
    sysfs_set_int(sysfs_bl_brightness, brightness);
#endif
}

#ifdef HAVE_LCD_SLEEP
void lcd_awake(void)
{
    /* Nothing to do */
}

void lcd_sleep(void)
{
    if (last_bl != BACKLIGHT_POWER_OFF) {
        last_bl = BACKLIGHT_POWER_OFF;
        sysfs_set_int(sysfs_bl_power, last_bl);
#ifdef HAVE_LCD_ENABLE
        lcd_enable(false);
#endif
    }
}
#endif
