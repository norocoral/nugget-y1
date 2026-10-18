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
    /* Always report connected or read from sysfs switch if present */
    return true;
}

