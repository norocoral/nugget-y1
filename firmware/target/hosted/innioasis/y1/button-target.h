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
#ifndef _BUTTON_TARGET_H_
#define _BUTTON_TARGET_H_

/* Main unit's buttons */
#define BUTTON_SELECT       0x00000001
#define BUTTON_MENU         0x00000002
#define BUTTON_LEFT         0x00000004
#define BUTTON_RIGHT        0x00000008
#define BUTTON_SCROLL_FWD   0x00000010
#define BUTTON_SCROLL_BACK  0x00000020
#define BUTTON_PLAY         0x00000040
#define BUTTON_VOL_UP       0x00000080
#define BUTTON_VOL_DOWN     0x00000100
#define BUTTON_POWER        0x00000200

#define BUTTON_MAIN (BUTTON_SELECT | BUTTON_MENU | BUTTON_LEFT | BUTTON_RIGHT | \
                     BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK | BUTTON_PLAY | \
                     BUTTON_VOL_UP | BUTTON_VOL_DOWN | BUTTON_POWER)

/* Software power-off button */
#define POWEROFF_BUTTON BUTTON_POWER
#define POWEROFF_COUNT  25

#define HAVE_CUSTOM_KEY_HANDLER
int button_map(int keycode);
bool button_handle_key_event(int keycode, int value);
void y1_mfb_init(void);
bool headphones_inserted(void);

#endif /* _BUTTON_TARGET_H_ */

