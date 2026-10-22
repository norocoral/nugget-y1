/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Antigravity
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#define RB_FILESYSTEM_OS
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "button.h"
#include "button-target.h"
#include "kernel.h"
#include "sysfs.h"
#include "settings.h"
#include "powermgmt.h"
#include "y1-log.h"

#ifndef KEY_HEADSETHOOK
#define KEY_HEADSETHOOK 226
#endif
#ifndef KEY_SEND
#define KEY_SEND        231
#endif
#ifndef KEY_HANGUEL
#define KEY_HANGUEL     122
#endif
#ifndef KEY_STOPCD
#define KEY_STOPCD      166
#endif

/* Headphone Inline Remote (MFB) State Machine */
static bool mfb_is_down = false;
static long mfb_press_tick = 0;
static long mfb_last_release_tick = 0;
static int  mfb_click_count = 0;
static bool mfb_hold_2s_fired = false;
static bool mfb_hold_5s_fired = false;
static bool mfb_waiting_5s = false;
static long mfb_hold_2s_tick = 0;
static long mfb_last_pmic_check = 0;

/* Physical Play/Pause 10-second hold to shut down */
static bool phys_play_is_down = false;
static long phys_play_press_tick = 0;
static bool phys_play_poweroff_fired = false;

static void y1_kmsg_log(const char *msg)
{
    y1_log(msg);
}

static inline int mfb_action_to_multimedia_btn(int action)
{
    switch(action)
    {
        case MFB_ACTION_PLAYPAUSE: return BUTTON_MULTIMEDIA_PLAYPAUSE;
        case MFB_ACTION_NEXT:      return BUTTON_MULTIMEDIA_NEXT;
        case MFB_ACTION_PREV:      return BUTTON_MULTIMEDIA_PREV;
        case MFB_ACTION_VOL_UP:    return BUTTON_MULTIMEDIA_VOLUME_UP;
        case MFB_ACTION_VOL_DOWN:  return BUTTON_MULTIMEDIA_VOLUME_DOWN;
        case MFB_ACTION_FFWD:      return BUTTON_MULTIMEDIA_FFWD;
        case MFB_ACTION_REW:       return BUTTON_MULTIMEDIA_REW;
        case MFB_ACTION_KEYLOCK:   return BUTTON_MULTIMEDIA_KEYLOCK;
        case MFB_ACTION_MUTE:      return BUTTON_MULTIMEDIA_MUTE;
        default:                   return 0;
    }
}

static void y1_trigger_mfb_action(int action_setting)
{
    int btn = mfb_action_to_multimedia_btn(action_setting);
    if (btn != 0)
    {
        char kbuf[80];
        snprintf(kbuf, sizeof(kbuf), "[button-y1] MFB trigger action=%d -> 0x%08x\n", action_setting, btn);
        y1_kmsg_log(kbuf);
        button_queue_post(btn, 0);
    }
}

static void y1_mfb_tick(void)
{
    long now = current_tick;

    /* Physical Play/Pause held for 10 seconds -> shut down device */
    if (phys_play_is_down && !phys_play_poweroff_fired)
    {
        if (TIME_AFTER(now, phys_play_press_tick + (10 * HZ)))
        {
            phys_play_poweroff_fired = true;
            y1_kmsg_log("[button-y1] Play/Pause held 10s -> sys_poweroff()\n");
            sys_poweroff();
        }
    }

    /* 1. Handle continued hold from 2s -> 5s via MT6323 PMIC ACCDET_MEM_IN (0x0794) */
    if (mfb_waiting_5s)
    {
        if (TIME_AFTER(now, mfb_last_pmic_check + (HZ / 10)))
        {
            mfb_last_pmic_check = now;
            int reg = y1_read_pmic_reg(0x0794);
            /* Bits 7:6 of 0x0794 are comparator AB: 0 means MFB is still shorted to GND */
            if (reg < 0 || ((reg >> 6) & 3) != 0)
            {
                /* Released before 5s: trigger the 2s hold action now */
                mfb_waiting_5s = false;
                y1_write_pmic_reg(0x0786, 0x0200, 0xffff);
                y1_trigger_mfb_action(global_settings.mfb_hold_2s);
            }
            else if (TIME_AFTER(now, mfb_hold_2s_tick + (3 * HZ) + (HZ / 5)))
            {
                /* Held continuously for 5s total (~1.8s kernel + 3.2s continued): trigger 5s action */
                mfb_waiting_5s = false;
                mfb_hold_5s_fired = true;
                y1_write_pmic_reg(0x0786, 0x0200, 0xffff);
                y1_trigger_mfb_action(global_settings.mfb_hold_5s);
            }
        }
        return;
    }

    /* 2. Handle standard key-down hold timing (if driver holds key down) */
    if (mfb_is_down)
    {
        long held = now - mfb_press_tick;
        if (!mfb_hold_2s_fired && held >= (2 * HZ))
        {
            mfb_hold_2s_fired = true;
            mfb_click_count = 0;
            if (global_settings.mfb_hold_5s == MFB_ACTION_NONE)
            {
                y1_trigger_mfb_action(global_settings.mfb_hold_2s);
            }
        }
        if (!mfb_hold_5s_fired && held >= (5 * HZ))
        {
            mfb_hold_5s_fired = true;
            y1_trigger_mfb_action(global_settings.mfb_hold_5s);
        }
    }
    else if (mfb_click_count > 0)
    {
        /* Multi-click detection window (550 ms to account for kernel multi_key_detection polling) */
        if (TIME_AFTER(now, mfb_last_release_tick + (HZ * 55 / 100)))
        {
            int clicks = mfb_click_count;
            mfb_click_count = 0;
            if (clicks == 1)
            {
                y1_trigger_mfb_action(global_settings.mfb_single_click);
            }
            else if (clicks == 2)
            {
                y1_trigger_mfb_action(global_settings.mfb_double_click);
            }
            else if (clicks >= 3)
            {
                y1_trigger_mfb_action(global_settings.mfb_triple_click);
            }
        }
    }
}

bool button_handle_key_event(int keycode, int value, bool is_accdet)
{
    /* Track physical Play/Pause button hold for 10s power-off */
    if (!is_accdet && (keycode == KEY_PLAYPAUSE || keycode == 164 ||
                       keycode == KEY_PLAY || keycode == 207 ||
                       keycode == 119 /* KEY_PAUSE */))
    {
        if (value != 0)
        {
            if (!phys_play_is_down)
            {
                phys_play_is_down = true;
                phys_play_press_tick = current_tick;
                phys_play_poweroff_fired = false;
            }
        }
        else
        {
            phys_play_is_down = false;
        }
        return false;
    }

    /*
     * On the MediaTek MT6572 (g368_nyx) kernel, the ACCDET headset driver never
     * emits KEY_HEADSETHOOK (226). Instead, on the "ACCDET" input device it emits:
     *   - Short press MFB: KEY_PLAYPAUSE (164) (or KEY_SEND = 231) as down+up pulse
     *   - Long press MFB (~1.8s): KEY_STOPCD (166) (or KEY_HANGUEL = 122) as down+up pulse
     *   - Inline UP (+): KEY_PREVIOUSSONG (165) (or KEY_VOLUMEUP = 115)
     *   - Inline DOWN (-): KEY_NEXTSONG (163) (or KEY_VOLUMEDOWN = 114)
     */
    bool is_mfb_long = (keycode == KEY_STOPCD || keycode == 166 ||
                        keycode == KEY_HANGUEL || keycode == 122);
    bool is_mfb_short = (keycode == KEY_HEADSETHOOK || keycode == 226 ||
                         keycode == KEY_SEND || keycode == 231 ||
                         keycode == 169 /* KEY_PHONE */ ||
                         (is_accdet && (keycode == KEY_PLAYPAUSE || keycode == 164 ||
                                        keycode == KEY_PLAY || keycode == 207 ||
                                        keycode == 119 /* KEY_PAUSE */)));

    if (is_accdet || is_mfb_long || is_mfb_short)
    {
        char kbuf[96];
        snprintf(kbuf, sizeof(kbuf), "[button-y1] ACCDET event: code=%d val=%d is_accdet=%d\n",
                 keycode, value, is_accdet ? 1 : 0);
        y1_kmsg_log(kbuf);
    }

    /* Handle 3-button headset inline UP / DOWN buttons on ACCDET device */
    if (is_accdet && !is_mfb_short && !is_mfb_long)
    {
        if (keycode == 165 /* KEY_PREVIOUSSONG */ || keycode == KEY_VOLUMEUP)
        {
            if (value != 0)
                button_queue_post(BUTTON_MULTIMEDIA_VOLUME_UP, 0);
            return true;
        }
        if (keycode == 163 /* KEY_NEXTSONG */ || keycode == KEY_VOLUMEDOWN)
        {
            if (value != 0)
                button_queue_post(BUTTON_MULTIMEDIA_VOLUME_DOWN, 0);
            return true;
        }
    }

    if (!is_mfb_short && !is_mfb_long)
        return false;

    long now = current_tick;

    /* Keep ACCDET_DEBOUNCE0 (0x0786) responsive (15.6ms instead of 62.5ms) for fast multi-clicks */
    y1_write_pmic_reg(0x0786, 0x0200, 0xffff);

    if (is_mfb_long)
    {
        /* Kernel multi_key_detection emits KEY_STOPCD (166) after ~1.8s hold */
        if (value != 0)
        {
            mfb_is_down = false;
            mfb_click_count = 0;
            mfb_hold_2s_fired = true;
            mfb_hold_5s_fired = false;

            int reg = y1_read_pmic_reg(0x0794);
            if (global_settings.mfb_hold_5s != MFB_ACTION_NONE &&
                reg >= 0 && ((reg >> 6) & 3) == 0)
            {
                /* MFB is still held down and 5s action is configured: monitor for 5s hold */
                mfb_waiting_5s = true;
                mfb_hold_2s_tick = now;
                mfb_last_pmic_check = now;
            }
            else
            {
                mfb_waiting_5s = false;
                y1_trigger_mfb_action(global_settings.mfb_hold_2s);
            }
        }
        return true;
    }

    /* Short press MFB event */
    if (value != 0) /* Key DOWN */
    {
        mfb_waiting_5s = false;
        if (!mfb_is_down)
        {
            mfb_is_down = true;
            mfb_press_tick = now;
            mfb_hold_2s_fired = false;
            mfb_hold_5s_fired = false;
        }
    }
    else /* Key UP */
    {
        if (mfb_is_down)
        {
            mfb_is_down = false;
            if (mfb_hold_2s_fired && !mfb_hold_5s_fired &&
                global_settings.mfb_hold_5s != MFB_ACTION_NONE)
            {
                y1_trigger_mfb_action(global_settings.mfb_hold_2s);
            }
            else if (!mfb_hold_2s_fired && !mfb_hold_5s_fired)
            {
                mfb_click_count++;
                mfb_last_release_tick = now;
                if (mfb_click_count >= 3)
                {
                    mfb_click_count = 0;
                    y1_trigger_mfb_action(global_settings.mfb_triple_click);
                }
            }
        }
    }

    return true;
}

void y1_mfb_init(void)
{
    static bool inited = false;
    if (!inited) {
        tick_add_task(y1_mfb_tick);
        inited = true;
    }
}

int button_map(int keycode)
{
    int mapped = 0;
    switch(keycode)
    {
        case KEY_UP:            /* 103: Click wheel counter-clockwise */
            mapped = BUTTON_SCROLL_BACK;
            break;

        case KEY_DOWN:          /* 108: Click wheel clockwise */
            mapped = BUTTON_SCROLL_FWD;
            break;

        case 232:               /* MT6572 hardware matrix keypad center switch */
        case KEY_ENTER:         /* 28: Enter */
        case KEY_KPENTER:       /* 96 */
        case 352:               /* KEY_OK */
        case 353:               /* KEY_SELECT */
        case 314:               /* BTN_SELECT */
        case 315:               /* BTN_START */
        case 304:               /* BTN_A */
            mapped = BUTTON_SELECT;
            break;

        case KEY_BACK:          /* 158: Top Menu / Back button */
        case KEY_MENU:          /* 139 */
            mapped = BUTTON_MENU;
            break;

        case KEY_PLAYPAUSE:     /* 164: Bottom Play / Pause button */
        case KEY_PLAY:          /* 207 */
        case 119:               /* KEY_PAUSE */
            mapped = BUTTON_PLAY;
            break;

        case KEY_LEFT:          /* 105: Left / Previous */
        case 165:               /* KEY_PREVIOUSSONG */
            mapped = BUTTON_LEFT;
            break;

        case KEY_RIGHT:         /* 106: Right / Next */
        case 163:               /* KEY_NEXTSONG */
            mapped = BUTTON_RIGHT;
            break;

        case KEY_VOLUMEDOWN:    /* 114: Side volume down */
        case 160:
            mapped = BUTTON_VOL_DOWN;
            break;

        case KEY_VOLUMEUP:      /* 115: Side volume up */
        case 161:
            mapped = BUTTON_VOL_UP;
            break;

        case KEY_POWER:         /* 116: Power button */
            mapped = BUTTON_POWER;
            break;

        default:
            mapped = 0;
            break;
    }

    if (mapped) {
        char kbuf[64];
        snprintf(kbuf, sizeof(kbuf), "[button-y1] keycode=%d -> button=0x%04x\n", keycode, mapped);
        y1_kmsg_log(kbuf);
    }

    return mapped;
}

bool headphones_inserted(void)
{
    static bool cached_state = true;
    static int last_raw_state = -1;
    static long last_check_tick = 0;
    long now = current_tick;

    /* Check switch state at most 10 times per second to minimize sysfs I/O */
    if (last_check_tick != 0 && TIME_BEFORE(now, last_check_tick + (HZ / 10)))
        return cached_state;

    last_check_tick = now;

    int fd = open("/sys/class/switch/h2w/state", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[16];
        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len > 0) {
            buf[len] = '\0';
            int state = 0;
            if (sscanf(buf, "%d", &state) == 1) {
                if (state != last_raw_state) {
                    last_raw_state = state;
                    /* When 4-pole headset (state == 1) is inserted, reduce button press debounce to 15.6ms */
                    if (state == 1) {
                        y1_write_pmic_reg(0x0786, 0x0200, 0xffff);
                    }
                    char kbuf[80];
                    snprintf(kbuf, sizeof(kbuf), "[button-y1] h2w state=%d\n", state);
                    y1_kmsg_log(kbuf);
                }
                cached_state = (state > 0);
            }
        }
    }
    return cached_state;
}
