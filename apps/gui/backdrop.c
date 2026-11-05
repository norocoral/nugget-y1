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
#include "config.h"
#include "lcd.h"
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

    if (bm.width != LCD_WIDTH || bm.height != LCD_HEIGHT)
    {
        int src_w = bm.width;
        int src_h = bm.height;
        fb_data *fb = (fb_data *)backdrop_buffer;

        if (src_w * src_h <= LCD_WIDTH * LCD_HEIGHT)
        {
            /* Upscaling (e.g. 320x240 -> 480x360): iterate backward */
            for (int y = LCD_HEIGHT - 1; y >= 0; y--)
            {
                int sy = (y * src_h) / LCD_HEIGHT;
                int dst_row = y * LCD_WIDTH;
                int src_row = sy * src_w;
                for (int x = LCD_WIDTH - 1; x >= 0; x--)
                {
                    int sx = (x * src_w) / LCD_WIDTH;
                    fb[dst_row + x] = fb[src_row + sx];
                }
            }
        }
        else
        {
            /* Downscaling (source is larger than screen): iterate forward */
            for (int y = 0; y < LCD_HEIGHT; y++)
            {
                int sy = (y * src_h) / LCD_HEIGHT;
                int dst_row = y * LCD_WIDTH;
                int src_row = sy * src_w;
                for (int x = 0; x < LCD_WIDTH; x++)
                {
                    int sx = (x * src_w) / LCD_WIDTH;
                    fb[dst_row + x] = fb[src_row + sx];
                }
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
