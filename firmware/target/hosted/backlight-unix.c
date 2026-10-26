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
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include "config.h"
#include "backlight-target.h"
#include "sysfs.h"
#include "panic.h"
#include "lcd.h"
#include "kernel.h"
#include "misc.h"
#include "settings.h"
#if defined(INNIOASIS_Y1)
#include "innioasis/y1/y1-log.h"
#include "innioasis/y1/button-target.h"
#endif

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
#if defined(BACKLIGHT_INNIOASIS)
static long y1_aod_last_activity_tick;
#endif

bool backlight_hw_init(void)
{
    current_brightness = DEFAULT_BRIGHTNESS_SETTING;
#if defined(BACKLIGHT_INNIOASIS)
    y1_aod_last_activity_tick = current_tick;
#endif
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
#include "innioasis/y1/bt-y1.h"
#include "innioasis/y1/y1-log.h"
extern void y1_enter_hold_mode(void);
static int bl_fd = -1;

enum y1_aod_pause_state {
    Y1_AOD_PAUSE_IDLE,
    Y1_AOD_PAUSE_HOLD,
    Y1_AOD_PAUSE_FADE_OUT,
    Y1_AOD_PAUSE_OFF,
    Y1_AOD_PAUSE_FADE_IN
};

static enum y1_aod_pause_state y1_aod_pause_state;
static int y1_aod_level;
static int y1_aod_fade_start_level;
static int y1_aod_fade_target_level;
static long y1_aod_pause_tick;
static long y1_aod_fade_start_tick;
static long y1_aod_last_fade_tick;
static volatile bool y1_aod_event_pending;
static bool y1_aod_event_active;
static bool y1_aod_event_fading;
static long y1_aod_event_deadline;

#define Y1_AOD_PAUSE_DELAY (HZ * 15)
#define Y1_AOD_EVENT_DURATION (HZ * 15)
#define Y1_AOD_FADE_DURATION (HZ * 3 / 4)
#if HZ < 20
#define Y1_AOD_FADE_INTERVAL 1
#else
#define Y1_AOD_FADE_INTERVAL (HZ / 20)
#endif

static void write_bl_hw(int val)
{
    if (bl_fd < 0) {
        bl_fd = open(sysfs_bl_brightness, O_WRONLY | O_CLOEXEC);
    }
    if (bl_fd >= 0) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d\n", val);
        lseek(bl_fd, 0, SEEK_SET);
        ssize_t w = write(bl_fd, buf, len);
        y1_logf("[backlight-unix] write_bl_hw(%d): bl_fd=%d, written=%d (errno=%d)\n",
                val, bl_fd, (int)w, errno);
    } else {
        y1_logf("[backlight-unix] write_bl_hw(%d) ERROR: failed to open %s (errno=%d)\n",
                val, sysfs_bl_brightness ? sysfs_bl_brightness : "NULL", errno);
    }
}

static int y1_aod_brightness_level(void)
{
    int brightness = global_settings.aod_brightness < 0 ?
                     current_brightness / 2 :
                     global_settings.aod_brightness;
    brightness = MAX(0, MIN(brightness, 100));
    return (brightness * 255) / 100;
}

static int y1_aod_timeout_seconds(void)
{
    static const int timeout_seconds[] = {
        0, 5 * 60, 10 * 60, 30 * 60, 60 * 60, 2 * 60 * 60, 5 * 60 * 60
    };
    int choice = global_settings.aod_timeout;

    if (choice < 0 || (unsigned)choice >= ARRAYLEN(timeout_seconds))
        return 0;

    return timeout_seconds[choice];
}

bool y1_aod_timeout_expired(void)
{
    int timeout = y1_aod_timeout_seconds();
    return timeout > 0 &&
           TIME_AFTER(current_tick, y1_aod_last_activity_tick + timeout * HZ);
}

void y1_aod_activity_notify(void)
{
    y1_aod_last_activity_tick = current_tick;
}

void y1_aod_user_wake(void)
{
    y1_aod_activity_notify();
    y1_aod_pause_state = Y1_AOD_PAUSE_IDLE;
}

void y1_aod_timeout_setting_changed(int choice)
{
    (void)choice;
    y1_aod_activity_notify();
}

static void y1_aod_set_level(int level)
{
    if (level < 0)
        level = 0;
    if (level > 255)
        level = 255;
    y1_aod_level = level;
    write_bl_hw(level);
}

void y1_aod_brightness_setting_changed(int brightness)
{
    (void)brightness;
    if (last_bl == BACKLIGHT_POWER_REDUCED && y1_should_always_on())
        y1_aod_set_level(y1_aod_brightness_level());
}

static void y1_aod_begin_fade(int target)
{
    y1_aod_fade_start_level = y1_aod_level;
    y1_aod_fade_target_level = target;
    y1_aod_fade_start_tick = current_tick;
    y1_aod_last_fade_tick = current_tick - Y1_AOD_FADE_INTERVAL;
}

static void y1_aod_start_fade(int target)
{
    y1_aod_begin_fade(target);
    y1_aod_pause_state = target ? Y1_AOD_PAUSE_FADE_IN :
                                  Y1_AOD_PAUSE_FADE_OUT;
}

static bool y1_aod_update_fade(void)
{
    if (TIME_BEFORE(current_tick,
                    y1_aod_last_fade_tick + Y1_AOD_FADE_INTERVAL))
        return false;

    y1_aod_last_fade_tick = current_tick;
    long elapsed = current_tick - y1_aod_fade_start_tick;
    if (elapsed >= Y1_AOD_FADE_DURATION) {
        y1_aod_set_level(y1_aod_fade_target_level);
        return true;
    }

    int delta = y1_aod_fade_target_level - y1_aod_fade_start_level;
    int level = y1_aod_fade_start_level +
                (int)((long long)delta * elapsed / Y1_AOD_FADE_DURATION);
    y1_aod_set_level(level);
    return false;
}

static void y1_aod_resume_from_off(void)
{
    if (!global_settings.always_on_screen || !y1_should_always_on())
        return;

    last_bl = BACKLIGHT_POWER_REDUCED;
    y1_enter_hold_mode();
    y1_set_screen_off_power_mode(true);
    y1_aod_set_level(0);
    lcd_enable(true);
    y1_set_display_power_saving(true);
    y1_aod_start_fade(y1_aod_brightness_level());
}

static bool y1_aod_pause_update(void)
{
    if (!global_settings.always_on_screen ||
        !global_settings.aod_playback_only ||
        (global_settings.aod_wps_only &&
         get_current_activity() != ACTIVITY_WPS)) {
        y1_aod_pause_state = Y1_AOD_PAUSE_IDLE;
        return false;
    }

    bool playing = y1_playback_active();
    if (playing) {
        if (y1_aod_pause_state == Y1_AOD_PAUSE_HOLD) {
            y1_aod_pause_state = Y1_AOD_PAUSE_IDLE;
            y1_set_display_power_saving(true);
            return true;
        }
        if (y1_aod_pause_state == Y1_AOD_PAUSE_FADE_OUT ||
            y1_aod_pause_state == Y1_AOD_PAUSE_OFF) {
            if (last_bl == BACKLIGHT_POWER_OFF)
                y1_aod_resume_from_off();
            else {
                y1_set_display_power_saving(true);
                y1_aod_start_fade(y1_aod_brightness_level());
            }
            return true;
        }
        if (y1_aod_pause_state == Y1_AOD_PAUSE_FADE_IN) {
            if (y1_aod_update_fade())
                y1_aod_pause_state = Y1_AOD_PAUSE_IDLE;
            return true;
        }
        return false;
    }

    if (y1_aod_pause_state == Y1_AOD_PAUSE_IDLE &&
        last_bl == BACKLIGHT_POWER_REDUCED &&
        y1_is_display_power_saving()) {
        y1_set_display_power_saving(false);
        y1_aod_pause_tick = current_tick;
        y1_aod_pause_state = Y1_AOD_PAUSE_HOLD;
        return true;
    }

    if (y1_aod_pause_state == Y1_AOD_PAUSE_HOLD) {
        if (TIME_AFTER(current_tick,
                       y1_aod_pause_tick + Y1_AOD_PAUSE_DELAY))
            y1_aod_start_fade(0);
        return true;
    }

    if (y1_aod_pause_state == Y1_AOD_PAUSE_FADE_OUT) {
        if (y1_aod_update_fade()) {
            y1_aod_pause_state = Y1_AOD_PAUSE_OFF;
            backlight_hw_off();
        }
        return true;
    }

    return y1_aod_pause_state == Y1_AOD_PAUSE_OFF;
}

void y1_aod_notify_event(void)
{
    if (!global_settings.always_on_screen || !global_settings.aod_important_events)
        return;
    if (!y1_should_always_on())
        return;
    y1_aod_event_pending = true;
}

static bool y1_aod_event_allowed(void)
{
    if (!global_settings.always_on_screen)
        return false;

    return y1_should_always_on();
}

static void y1_aod_event_wake(void)
{
    if (!global_settings.always_on_screen || !y1_should_always_on())
        return;

    if (last_bl == BACKLIGHT_POWER_OFF)
    {
        y1_enter_hold_mode();
        y1_set_screen_off_power_mode(true);
        lcd_enable(true);
        last_bl = BACKLIGHT_POWER_REDUCED;
        y1_aod_set_level(y1_aod_brightness_level());
    }
    else if (y1_aod_level == 0)
        y1_aod_set_level(y1_aod_brightness_level());

    y1_set_display_power_saving(false);
}

static void y1_aod_event_restore_idle(void)
{
    if (last_bl != BACKLIGHT_POWER_REDUCED)
        return;

    if (y1_should_always_on())
    {
        y1_set_display_power_saving(true);
        y1_set_screen_off_power_mode(true);
    }
    else
        backlight_hw_off();
}

static bool y1_aod_event_update(void)
{
    if (y1_aod_event_pending) {
        y1_aod_event_pending = false;
        if (global_settings.aod_important_events &&
            y1_aod_event_allowed() &&
            (last_bl == BACKLIGHT_POWER_REDUCED ||
             last_bl == BACKLIGHT_POWER_OFF)) {
            y1_aod_event_wake();
            y1_aod_event_deadline = current_tick + Y1_AOD_EVENT_DURATION;
            y1_aod_event_active = true;
            y1_aod_event_fading = false;
        }
    }

    if (!y1_aod_event_active)
        return false;

    if (!global_settings.aod_important_events ||
        !y1_aod_event_allowed()) {
        y1_aod_event_active = false;
        y1_aod_event_fading = false;
        y1_aod_event_restore_idle();
        return false;
    }

    if (last_bl != BACKLIGHT_POWER_REDUCED) {
        y1_aod_event_active = false;
        y1_aod_event_fading = false;
        return false;
    }

    if (!y1_aod_event_fading &&
        TIME_AFTER(current_tick, y1_aod_event_deadline)) {
        y1_aod_begin_fade(0);
        y1_aod_event_fading = true;
    }

    if (y1_aod_event_fading && y1_aod_update_fade()) {
        y1_aod_event_active = false;
        y1_aod_event_fading = false;
        y1_aod_event_restore_idle();
    }
    return true;
}
#endif

#if defined(BACKLIGHT_INNIOASIS)
bool y1_is_backlight_active(void)
{
    return last_bl == BACKLIGHT_POWER_ON;
}

bool y1_is_screen_sleeping(void)
{
    return last_bl != BACKLIGHT_POWER_ON;
}
#endif

void backlight_hw_on(void)
{
    y1_logf("[backlight-unix] backlight_hw_on() called (last_bl=%d, current_brightness=%d)\n",
            last_bl, current_brightness);
    if (last_bl != BACKLIGHT_POWER_ON) {
#if defined(BACKLIGHT_INNIOASIS)
        if (y1_is_always_on_screen() || y1_is_display_power_saving()) {
            y1_logf("[backlight-unix] backlight_hw_on: resetting powersave\n");
            y1_set_display_power_saving(false);
        }
        y1_set_screen_off_power_mode(false);
#endif
        last_bl = BACKLIGHT_POWER_ON;
        if (sysfs_bl_power)
            sysfs_set_int(sysfs_bl_power, last_bl);
#if defined(BACKLIGHT_INNIOASIS)
        /* Writing 'on' to /sys/power/state wakes the kernel earlysuspend subsystem
         * and triggers mtkfb_late_resume (DISP_PowerEnable(TRUE), DISP_PanelEnable(TRUE)) */
        int on_fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
        if (on_fd >= 0) {
            (void)!write(on_fd, "on\n", 3);
            close(on_fd);
            y1_logf("[backlight-unix] backlight_hw_on: wrote 'on' to /sys/power/state\n");
        }
        y1_set_touch_scanning(true);
#endif
#ifdef HAVE_LCD_ENABLE
        y1_logf("[backlight-unix] backlight_hw_on: calling lcd_enable(true)\n");
        lcd_enable(true);
#endif
#if defined(BACKLIGHT_INNIOASIS)
        int hw_val = current_brightness > 0 ? (current_brightness * 255) / 100 : (DEFAULT_BRIGHTNESS_SETTING * 255) / 100;
        y1_logf("[backlight-unix] backlight_hw_on: write_bl_hw(%d)\n", hw_val);
        write_bl_hw(hw_val);
#endif
    } else {
        y1_logf("[backlight-unix] backlight_hw_on: last_bl already BACKLIGHT_POWER_ON, ensuring lcd_enable(true)\n");
#ifdef HAVE_LCD_ENABLE
        lcd_enable(true);
#endif
    }
}

void backlight_hw_off(void)
{
#if defined(BACKLIGHT_INNIOASIS)
    int target_bl = y1_should_always_on() ? BACKLIGHT_POWER_REDUCED : BACKLIGHT_POWER_OFF;
    y1_logf("[backlight-unix] backlight_hw_off() called (last_bl=%d, target_bl=%d, should_aod=%d)\n",
            last_bl, target_bl, (int)y1_should_always_on());
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
            y1_logf("[backlight-unix] backlight_hw_off: AOD mode -> lcd_enable(true)\n");
            lcd_enable(true);
#endif
            int hw_val = y1_aod_brightness_level();
            y1_aod_level = hw_val;
            y1_logf("[backlight-unix] backlight_hw_off: AOD brightness write_bl_hw(%d)\n", hw_val);
            write_bl_hw(hw_val);
            y1_set_display_power_saving(true);
            return;
        }
        if (y1_is_display_power_saving()) {
            y1_set_display_power_saving(false);
        }
        y1_logf("[backlight-unix] backlight_hw_off: Screen Off -> write_bl_hw(0), lcd_enable(false)\n");
        write_bl_hw(0);
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
    if (y1_aod_event_update())
        return;

    if (y1_aod_pause_update())
        return;

    int target_bl = y1_should_always_on() ? BACKLIGHT_POWER_REDUCED :
                                            BACKLIGHT_POWER_OFF;
    if ((last_bl == BACKLIGHT_POWER_REDUCED ||
         last_bl == BACKLIGHT_POWER_OFF) && last_bl != target_bl)
        backlight_hw_off();
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
        y1_aod_level = y1_aod_brightness_level();
        write_bl_hw(y1_aod_level);
    }
#else
    sysfs_set_int(sysfs_bl_brightness, brightness);
#endif
}

#ifdef HAVE_LCD_SLEEP
#if defined(BACKLIGHT_INNIOASIS)
void lcd_awake(void)
{
    y1_logf("[backlight-unix] lcd_awake() called\n");
    sysfs_set_string("/sys/power/state", "on");
    y1_set_touch_scanning(true);
#ifdef HAVE_LCD_ENABLE
    lcd_enable(true);
#endif
}

void lcd_sleep(void)
{
    y1_logf("[backlight-unix] lcd_sleep() called (receiver=%d, should_aod=%d, playing=%d, aod_pause_state=%d)\n",
            (int)y1_bt_is_receiver_mode(), (int)y1_should_always_on(), (int)y1_playback_active(),
            (int)y1_aod_pause_state);
    if (y1_bt_is_receiver_mode() ||
        y1_should_always_on() || y1_playback_active() ||
        y1_aod_pause_state == Y1_AOD_PAUSE_HOLD ||
        y1_aod_pause_state == Y1_AOD_PAUSE_FADE_OUT)
        return;

#ifdef HAVE_LCD_ENABLE
    y1_logf("[backlight-unix] lcd_sleep: calling lcd_enable(false)\n");
    lcd_enable(false);
#endif

    /* Put capacitive touch controller into sleep mode to avoid idle drain and spurious wakeups */
    y1_set_touch_scanning(false);

    if (!button_center_wakeup_available()) {
        y1_log("Y1: center-button wake is not enabled; remaining awake for input\n");
#ifdef HAVE_LCD_ENABLE
        lcd_enable(true);
#endif
        y1_set_touch_scanning(true);
        return;
    }

    /* Suspend to RAM until a key wakes the kernel. Playback never reaches here. */
    int fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        y1_log("Y1: failed to open /sys/power/state for suspend\n");
#ifdef HAVE_LCD_ENABLE
        lcd_enable(true);
#endif
        y1_set_touch_scanning(true);
        return;
    }

    y1_logf("[backlight-unix] lcd_sleep: requesting kernel suspend ('mem' -> /sys/power/state)\n");
    ssize_t written;
    do {
        written = write(fd, "mem", 3);
    } while (written < 0 && errno == EINTR);
    int write_error = written < 0 ? errno : (written != 3 ? EIO : 0);
    int close_result = close(fd);
    int close_error = close_result < 0 ? errno : 0;
    y1_logf("[backlight-unix] lcd_sleep: kernel returned from suspend (written=%d, err=%d)\n",
            (int)written, write_error ? write_error : close_error);

    if (written != 3 || close_result < 0) {
        char message[96];
        snprintf(message, sizeof(message),
                 "Y1: suspend request failed (write=%d, errno=%d)\n",
                 (int)written, write_error ? write_error : close_error);
        y1_log(message);
        /* If suspend could not enter 'mem' (e.g. held wakelock), ensure SPM deep idle is requested */
        sysfs_set_string("/sys/power/dpidle_state", "cpupdn 1\n");
        sysfs_set_string("/sys/power/dpidle_state", "1\n");
    }

    /* NOTE: We deliberately do NOT write 'on' to /sys/power/state here.
     * Writing 'on' here would immediately wake MTKFB and all hardware subsystems
     * even if suspend failed or if a background timer woke the SoC briefly.
     * 'on' is strictly written by lcd_awake() or backlight_hw_on() when the user
     * presses a key to turn the screen back on. */
}
#else
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
#endif
