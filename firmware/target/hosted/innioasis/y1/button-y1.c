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

#ifndef KEY_HEADSETHOOK
#define KEY_HEADSETHOOK 226
#endif

/* Headphone Inline Remote (MFB) State Machine */
static bool mfb_is_down = false;
static long mfb_press_tick = 0;
static long mfb_last_release_tick = 0;
static int  mfb_click_count = 0;
static bool mfb_hold_2s_fired = false;
static bool mfb_hold_5s_fired = false;

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
        char kbuf[64];
        snprintf(kbuf, sizeof(kbuf), "[button-y1] MFB trigger action=%d -> 0x%08x\n", action_setting, btn);
        int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (kfd >= 0) {
            write(kfd, kbuf, strlen(kbuf));
            close(kfd);
        }
        button_queue_post(btn, 0);
    }
}

static void y1_mfb_tick(void)
{
    long now = current_tick;

    if (mfb_is_down)
    {
        long held = now - mfb_press_tick;
        if (!mfb_hold_2s_fired && held >= (2 * HZ))
        {
            mfb_hold_2s_fired = true;
            mfb_click_count = 0;
            y1_trigger_mfb_action(global_settings.mfb_hold_2s);
        }
        if (!mfb_hold_5s_fired && held >= (5 * HZ))
        {
            mfb_hold_5s_fired = true;
            y1_trigger_mfb_action(global_settings.mfb_hold_5s);
        }
    }
    else if (mfb_click_count > 0)
    {
        /* Multi-click detection window (350 ms = 35 * HZ / 100) */
        if (TIME_AFTER(now, mfb_last_release_tick + (HZ * 35 / 100)))
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
        }
    }
}

bool button_handle_key_event(int keycode, int value)
{
    /* 226 is KEY_HEADSETHOOK */
    if (keycode != KEY_HEADSETHOOK && keycode != 226)
        return false;

    long now = current_tick;

    if (value != 0) /* Key DOWN */
    {
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
            if (!mfb_hold_2s_fired && !mfb_hold_5s_fired)
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
        int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (kfd >= 0) {
            write(kfd, kbuf, strlen(kbuf));
            close(kfd);
        }
    }

    return mapped;
}

bool headphones_inserted(void)
{
    static bool cached_state = true;
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
                cached_state = (state > 0);
            }
        }
    }
    return cached_state;
}
