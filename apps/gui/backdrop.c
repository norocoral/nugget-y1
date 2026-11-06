/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 Dave Chapman
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "lcd.h"
#include "screen_access.h"
#ifdef HAVE_REMOTE_LCD
#include "lcd-remote.h"
#endif
#include "backdrop.h"

bool backdrop_load(const char* filename, char *backdrop_buffer)
{
    struct bitmap bm;
    int ret;

    /* load the image */
    bm.data = backdrop_buffer;
    ret = read_bmp_file(filename, &bm, LCD_BACKDROP_BYTES,
                        FORMAT_NATIVE | FORMAT_DITHER, NULL);

    if (ret <= 0 || bm.width <= 0 || bm.height <= 0)
        return false;

    int src_w = bm.width;
    int src_h = bm.height;
    fb_data *fb = (fb_data *)backdrop_buffer;

    int target_w = screens[SCREEN_MAIN].lcdwidth;
    int target_h = screens[SCREEN_MAIN].lcdheight;
    if (target_w <= 0 || target_w > LCD_WIDTH)
        target_w = LCD_WIDTH;
    if (target_h <= 0 || target_h > LCD_HEIGHT)
        target_h = LCD_HEIGHT;

    if (src_w == target_w && src_h == target_h)
    {
        /* Image dimensions match current display resolution. */
        if (target_w < LCD_WIDTH)
        {
            /* Expand row stride from target_w to LCD_WIDTH (e.g. 320 to 480 in Classic Mode).
             * Because LCD_WIDTH > target_w, destination offset >= source offset for every row y.
             * Iterate backwards to safely remap in-place. */
            for (int y = target_h - 1; y >= 0; y--)
            {
                memmove(&fb[y * LCD_WIDTH], &fb[y * target_w], target_w * sizeof(fb_data));
                memset(&fb[y * LCD_WIDTH + target_w], 0, (LCD_WIDTH - target_w) * sizeof(fb_data));
            }
            if (target_h < LCD_HEIGHT)
            {
                memset(&fb[target_h * LCD_WIDTH], 0, (LCD_HEIGHT - target_h) * LCD_WIDTH * sizeof(fb_data));
            }
        }
        /* If target_w == LCD_WIDTH, row stride is already LCD_WIDTH. Nothing to do! */
    }
    else
    {
        /* Image dimensions differ from target display resolution.
         * Resample image to target_w x target_h with row stride LCD_WIDTH. */
        fb_data *src_copy = malloc(src_w * src_h * sizeof(fb_data));
        if (src_copy)
        {
            memcpy(src_copy, fb, src_w * src_h * sizeof(fb_data));
            for (int y = 0; y < target_h; y++)
            {
                int sy = (y * src_h) / target_h;
                int dst_row = y * LCD_WIDTH;
                int src_row = sy * src_w;
                for (int x = 0; x < target_w; x++)
                {
                    int sx = (x * src_w) / target_w;
                    fb[dst_row + x] = src_copy[src_row + sx];
                }
                if (target_w < LCD_WIDTH)
                {
                    memset(&fb[dst_row + target_w], 0, (LCD_WIDTH - target_w) * sizeof(fb_data));
                }
            }
            if (target_h < LCD_HEIGHT)
            {
                memset(&fb[target_h * LCD_WIDTH], 0, (LCD_HEIGHT - target_h) * LCD_WIDTH * sizeof(fb_data));
            }
            free(src_copy);
        }
        else
        {
            /* Low-memory fallback: use stack line buffer */
            fb_data line_buf[LCD_WIDTH];
            if (src_w * src_h <= target_w * target_h)
            {
                /* Upscaling: iterate backwards */
                for (int y = target_h - 1; y >= 0; y--)
                {
                    int sy = (y * src_h) / target_h;
                    int src_row = sy * src_w;
                    for (int x = 0; x < target_w; x++)
                    {
                        int sx = (x * src_w) / target_w;
                        line_buf[x] = fb[src_row + sx];
                    }
                    memcpy(&fb[y * LCD_WIDTH], line_buf, target_w * sizeof(fb_data));
                    if (target_w < LCD_WIDTH)
                    {
                        memset(&fb[y * LCD_WIDTH + target_w], 0, (LCD_WIDTH - target_w) * sizeof(fb_data));
                    }
                }
            }
            else
            {
                /* Downscaling: iterate forwards */
                for (int y = 0; y < target_h; y++)
                {
                    int sy = (y * src_h) / target_h;
                    int src_row = sy * src_w;
                    for (int x = 0; x < target_w; x++)
                    {
                        int sx = (x * src_w) / target_w;
                        line_buf[x] = fb[src_row + sx];
                    }
                    memcpy(&fb[y * LCD_WIDTH], line_buf, target_w * sizeof(fb_data));
                    if (target_w < LCD_WIDTH)
                    {
                        memset(&fb[y * LCD_WIDTH + target_w], 0, (LCD_WIDTH - target_w) * sizeof(fb_data));
                    }
                }
            }
            if (target_h < LCD_HEIGHT)
            {
                memset(&fb[target_h * LCD_WIDTH], 0, (LCD_HEIGHT - target_h) * LCD_WIDTH * sizeof(fb_data));
            }
        }
    }

    return true;
}
  
  
void backdrop_show(char *backdrop_buffer)
{
    lcd_set_backdrop((fb_data*)backdrop_buffer);
}
  

#if defined(HAVE_REMOTE_LCD)

#if LCD_REMOTE_DEPTH > 1
/* api functions */
bool remote_backdrop_load(const char *filename, char* backdrop_buffer)
{
    struct bitmap bm;
    int ret;

    /* load the image */
    bm.data = backdrop_buffer;
    ret = read_bmp_file(filename, &bm, REMOTE_LCD_BACKDROP_BYTES,
                        FORMAT_NATIVE | FORMAT_DITHER | FORMAT_REMOTE, NULL);

    if (ret <= 0 || bm.width <= 0 || bm.height <= 0)
        return false;

    if (bm.width != LCD_REMOTE_WIDTH || bm.height != LCD_REMOTE_HEIGHT)
    {
        int src_w = bm.width;
        int src_h = bm.height;
        fb_remote_data *fb = (fb_remote_data *)backdrop_buffer;

        if (src_w * src_h <= LCD_REMOTE_WIDTH * LCD_REMOTE_HEIGHT)
        {
            for (int y = LCD_REMOTE_HEIGHT - 1; y >= 0; y--)
            {
                int sy = (y * src_h) / LCD_REMOTE_HEIGHT;
                int dst_row = y * LCD_REMOTE_WIDTH;
                int src_row = sy * src_w;
                for (int x = LCD_REMOTE_WIDTH - 1; x >= 0; x--)
                {
                    int sx = (x * src_w) / LCD_REMOTE_WIDTH;
                    fb[dst_row + x] = fb[src_row + sx];
                }
            }
        }
        else
        {
            for (int y = 0; y < LCD_REMOTE_HEIGHT; y++)
            {
                int sy = (y * src_h) / LCD_REMOTE_HEIGHT;
                int dst_row = y * LCD_REMOTE_WIDTH;
                int src_row = sy * src_w;
                for (int x = 0; x < LCD_REMOTE_WIDTH; x++)
                {
                    int sx = (x * src_w) / LCD_REMOTE_WIDTH;
                    fb[dst_row + x] = fb[src_row + sx];
                }
            }
        }
    }

    return true;
}

void remote_backdrop_show(char *backdrop_buffer)
{
    lcd_remote_set_backdrop((fb_remote_data*)backdrop_buffer);
}

#else /* needs stubs */

bool remote_backdrop_load(const char *filename, char* backdrop_buffer)
{
    (void)filename; (void) backdrop_buffer;
    return false;
}
void remote_backdrop_show(char* backdrop_buffer)
{
    (void)backdrop_buffer;
}
#endif
#endif
