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
#ifndef __LCD_TARGET_H__
#define __LCD_TARGET_H__

#define LCD_OPTIMIZED_UPDATE
#define LCD_OPTIMIZED_UPDATE_RECT

extern fb_data *framebuffer;
#define LCD_FRAMEBUF_ADDR(col, row) (framebuffer + (row)*LCD_WIDTH + (col))

extern void lcd_set_active(bool active);
extern void y1_display_raw_screen(const char *name);
extern void y1_set_resolution_setting(int mode);
extern void y1_set_always_on_screen(bool on);
extern bool y1_is_always_on_screen(void);
extern bool y1_should_always_on(void);
extern void y1_set_display_power_saving(bool enable);
extern bool y1_is_display_power_saving(void);
extern bool y1_playback_active(void);
extern void y1_backlight_idle_check(void);
extern void y1_aod_notify_event(void);
extern void y1_aod_activity_notify(void);
extern bool y1_aod_timeout_expired(void);

#endif /* __LCD_TARGET_H__ */
