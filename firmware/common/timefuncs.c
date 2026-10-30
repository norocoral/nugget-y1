/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
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

#include <stdio.h> /* get NULL */
#include "config.h"

#include "kernel.h"
#include "rtc.h"
#ifdef HAVE_RTC_IRQ
#include "rtc-target.h"
#endif
#include "timefuncs.h"
#include "debug.h"

#if defined(INNIOASIS_Y1)
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/rtc.h>

#ifndef makedev
#define makedev(maj, min) (((maj) << 8) | (min))
#endif

#define Y1_RTC_SAVE_PATH       "/mnt/sdcard/.rockbox/rtc.dat"
#define Y1_RTC_MIN_VALID_EPOCH 1672531200LL /* 2023-01-01 00:00:00 UTC (ignores Aug 2022 reset) */

static bool y1_force_rtc_read = false;

static int y1_open_rtc_dev(int flags)
{
    int fd = open("/dev/rtc0", flags | O_CLOEXEC);
    if (fd < 0)
        fd = open("/dev/rtc", flags | O_CLOEXEC);
    if (fd < 0) {
        mknod("/dev/rtc0", S_IFCHR | 0666, makedev(254, 0));
        fd = open("/dev/rtc0", flags | O_CLOEXEC);
    }
    return fd;
}

static void y1_save_rtc_file(time_t t)
{
    if ((int64_t)t < Y1_RTC_MIN_VALID_EPOCH)
        return;
    int fd = open(Y1_RTC_SAVE_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd >= 0) {
        int64_t val = (int64_t)t;
        (void)!write(fd, &val, sizeof(val));
        fsync(fd);
        close(fd);
    }
}

static time_t y1_load_rtc_file(void)
{
    int fd = open(Y1_RTC_SAVE_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    int64_t val = 0;
    ssize_t r = read(fd, &val, sizeof(val));
    close(fd);
    if (r == (ssize_t)sizeof(val) && val >= Y1_RTC_MIN_VALID_EPOCH)
        return (time_t)val;
    return 0;
}

static void y1_write_hw_rtc(const struct tm *tm_val)
{
    int fd = y1_open_rtc_dev(O_WRONLY);
    if (fd >= 0) {
        struct rtc_time rt;
        rt.tm_sec   = tm_val->tm_sec;
        rt.tm_min   = tm_val->tm_min;
        rt.tm_hour  = tm_val->tm_hour;
        rt.tm_mday  = tm_val->tm_mday;
        rt.tm_mon   = tm_val->tm_mon;
        rt.tm_year  = tm_val->tm_year;
        rt.tm_wday  = tm_val->tm_wday;
        rt.tm_yday  = tm_val->tm_yday;
        rt.tm_isdst = 0;
        ioctl(fd, RTC_SET_TIME, &rt);
        close(fd);
    }
}

static time_t y1_read_hw_rtc(void)
{
    int fd = y1_open_rtc_dev(O_RDONLY);
    if (fd < 0)
        return 0;
    struct rtc_time rt;
    int rc = ioctl(fd, RTC_RD_TIME, &rt);
    close(fd);
    if (rc < 0)
        return 0;
    struct tm tmv;
    tmv.tm_sec   = rt.tm_sec;
    tmv.tm_min   = rt.tm_min;
    tmv.tm_hour  = rt.tm_hour;
    tmv.tm_mday  = rt.tm_mday;
    tmv.tm_mon   = rt.tm_mon;
    tmv.tm_year  = rt.tm_year;
    tmv.tm_wday  = 0;
    tmv.tm_yday  = 0;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    return ((int64_t)t >= Y1_RTC_MIN_VALID_EPOCH) ? t : 0;
}

static void y1_rtc_sync_on_boot(void)
{
    static bool synced = false;
    if (synced)
        return;
    synced = true;

    time_t sys_t  = time(NULL);
    time_t hw_t   = y1_read_hw_rtc();
    time_t file_t = y1_load_rtc_file();

    time_t best_t = sys_t;
    if (hw_t > best_t)
        best_t = hw_t;
    if (file_t > best_t)
        best_t = file_t;

    if ((int64_t)best_t >= Y1_RTC_MIN_VALID_EPOCH && best_t != sys_t) {
        struct timeval tv = { .tv_sec = best_t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        struct tm best_tm;
        if (localtime_r(&best_t, &best_tm))
            y1_write_hw_rtc(&best_tm);
        y1_force_rtc_read = true;
    }
}
#endif

static struct tm tm;

time_t dostime_mktime(uint16_t dosdate, uint16_t dostime)
{
    /* this knows our mktime() only uses these struct tm fields */
    struct tm tm;
    tm.tm_sec  = ((dostime      ) & 0x1f) * 2;
    tm.tm_min  = ((dostime >>  5) & 0x3f);
    tm.tm_hour = ((dostime >> 11)       );
    tm.tm_mday = ((dosdate      ) & 0x1f);
    tm.tm_mon  = ((dosdate >>  5) & 0x0f) - 1;
    tm.tm_year = ((dosdate >>  9)       ) + 80;

    return mktime(&tm);
}

void dostime_localtime(time_t time, uint16_t* dosdate, uint16_t* dostime)
{
    struct tm tm;
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
    gmtime_r(&time, &tm);
#else
    localtime_r(&time, &tm);
#endif

    *dostime = ((tm.tm_sec  /  2) <<  0)|
               ((tm.tm_min      ) <<  5)|
               ((tm.tm_hour     ) << 11);
    *dosdate = ((tm.tm_mday     ) <<  0)|
               ((tm.tm_mon  +  1) <<  5)|
               ((tm.tm_year - 80) <<  9);
}

#if !CONFIG_RTC
static inline bool rtc_dirty(void)
{
    return true;
}

static inline int rtc_read_datetime(struct tm *tm)
{
    tm->tm_sec = 0;
    tm->tm_min = 0;
    tm->tm_hour = 0;
    tm->tm_mday = 1;
    tm->tm_mon = 0;
    tm->tm_year = 70;
    tm->tm_wday = 4;
    tm->tm_yday = 0;
    return 1;
}
#endif /* !CONFIG_RTC */

#if CONFIG_RTC
bool valid_time(const struct tm *tm)
{
    if (!tm || (tm->tm_hour < 0 || tm->tm_hour > 23 ||
        tm->tm_sec < 0 || tm->tm_sec > 59 || 
        tm->tm_min < 0 || tm->tm_min > 59 ||
        tm->tm_year < 100 || tm->tm_year > 199 ||
        tm->tm_mon < 0 || tm->tm_mon > 11 ||
        tm->tm_wday < 0 || tm->tm_wday > 6 ||
        tm->tm_mday < 1 || tm->tm_mday > 31))
        return false;
    else
        return true;
}

/* Don't read the RTC more than once per second
 * returns true if the rtc needs to be read
 * targets may override with their own implementation
 */
#ifndef HAVE_RTC_IRQ
static inline bool rtc_dirty(void)
{
    static long timeout = 0;

    /* Don't read the RTC more than once per second */
    if (TIME_AFTER(current_tick, timeout))
    {
        /* Once per second, 1/5th of a second off */
        timeout = current_tick / HZ * HZ + 6*HZ / 5;
        return true;
    }

    return false;
}
#endif /* HAVE_RTC_IRQ */
#endif /* CONFIG_RTC */

struct tm *get_time(void)
{
#if defined(INNIOASIS_Y1)
    y1_rtc_sync_on_boot();
    if (rtc_dirty() || y1_force_rtc_read)
    {
        y1_force_rtc_read = false;
        rtc_read_datetime(&tm);
        tm.tm_isdst = -1; /* Not implemented for now */
    }
#else
    if (rtc_dirty())
    {
        rtc_read_datetime(&tm);
        tm.tm_isdst = -1; /* Not implemented for now */
    }
#endif

    return &tm;
}

int set_time(const struct tm *tm)
{
#if CONFIG_RTC
    int rc;

    if (valid_time(tm))
    {
#if defined(INNIOASIS_Y1)
        struct tm tm_copy = *tm;
        tm_copy.tm_isdst = -1;
        time_t new_t = mktime(&tm_copy);
        if (new_t != (time_t)-1) {
            struct timeval tv = { .tv_sec = new_t, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            y1_write_hw_rtc(&tm_copy);
            y1_save_rtc_file(new_t);
            y1_force_rtc_read = true;
        }
#endif
        rc = rtc_write_datetime(tm);

        if (rc < 0)
            return -1;
        else
            return 0;
    }
    else
    {
        return -2;
    }
#else /* No RTC */
    (void)tm;
    return -1;
#endif /* RTC */
}

#if CONFIG_RTC
void set_day_of_week(struct tm *tm)
{
    int y=tm->tm_year+1900;
    int d=tm->tm_mday;
    int m=tm->tm_mon;
    static const char mo[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

    if(m == 0 || m == 1) y--;
    tm->tm_wday = (d + mo[m] + y + y/4 - y/100 + y/400) % 7;
}

void set_day_of_year(struct tm *tm)
{
    int y=tm->tm_year+1900;
    int d=tm->tm_mday;
    int m=tm->tm_mon;
    d+=m*30;
    if( ( (m>1) && !(y%4) ) &&  (  (y%100) ||  !(y%400)  )  )
        d++;
    if(m>6)
    {
        d+=4;
        m-=7;
    }
    tm->tm_yday = d + ((m+1) /2);
}
#endif /* CONFIG_RTC */

