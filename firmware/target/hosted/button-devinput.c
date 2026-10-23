/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2020 Solomon Peachy

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
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include "kernel.h"
#include "sysfs.h"
#include "button.h"
#include "panic.h"
#if defined(INNIOASIS_Y1)
#include "innioasis/y1/button-target.h"
#include "innioasis/y1/y1-log.h"
#endif

#ifdef HAVE_SCROLLWHEEL
#include "powermgmt.h"
#if defined(HAVE_BACKLIGHT) || defined(HAVE_BUTTON_LIGHT)
#include "backlight.h"
#endif
#endif /* HAVE_SCROLLWHEEL */

/* TODO:  HAVE_SCROLLWHEEL is a hack.  Instead of posting the exact number
   of clicks, instead do it similar to the ipod clickwheel and post
   the wheel angular velocity (degrees per second)

   * Track the relative position  (ie based on click events)
   * Use WHEELCLICKS_PER_ROTATION to convert clicks to angular distance
   * Compute to angular velocity (degrees per second)

 */
#define NR_POLL_DESC    16

static int num_devices = 0;
static struct pollfd poll_fds[NR_POLL_DESC];
static bool is_accdet_dev[NR_POLL_DESC];
#if defined(INNIOASIS_Y1)
static bool center_button_wakeup_enabled;

static bool y1_enable_center_button_wakeup(int event_number)
{
    static const unsigned int center_keys[] = {
        232, KEY_ENTER, KEY_KPENTER, KEY_OK, KEY_SELECT,
        BTN_SELECT, BTN_START, BTN_A
    };
    const size_t bits_per_word = sizeof(unsigned long) * 8;
    unsigned long key_bits[KEY_MAX / (sizeof(unsigned long) * 8) + 1];
    memset(key_bits, 0, sizeof(key_bits));

    if (ioctl(poll_fds[event_number].fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)),
              key_bits) < 0)
        return false;

    bool has_center_key = false;
    for (size_t i = 0; i < sizeof(center_keys) / sizeof(center_keys[0]); i++) {
        unsigned int key = center_keys[i];
        if (key <= KEY_MAX &&
            (key_bits[key / bits_per_word] & (1UL << (key % bits_per_word)))) {
            has_center_key = true;
            break;
        }
    }

    if (!has_center_key)
        return false;

    char path[96];
    snprintf(path, sizeof(path),
             "/sys/class/input/event%d/device/power/wakeup", event_number);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    static const char enabled[] = "enabled\n";
    ssize_t written = write(fd, enabled, sizeof(enabled) - 1);
    close(fd);
    if (written != (ssize_t)(sizeof(enabled) - 1))
        return false;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    char state[16] = { 0 };
    ssize_t length = read(fd, state, sizeof(state) - 1);
    close(fd);
    return length >= 7 && !strncmp(state, "enabled", 7);
}

bool button_center_wakeup_available(void)
{
    return center_button_wakeup_enabled;
}
#endif

void button_add_input_device(int i)
{
    int fd = poll_fds[i].fd;
    if (fd >= 0)
        close(fd);

    is_accdet_dev[i] = false;

    char path[32];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    poll_fds[i].fd = fd >= 0 ? fd : -1;
    if(fd >= 0)
    {
        poll_fds[i].events = POLLIN;
        poll_fds[i].revents = 0;
        if (num_devices <= i)
            num_devices = i + 1;

        char dev_name[64];
        memset(dev_name, 0, sizeof(dev_name));
        if (ioctl(fd, EVIOCGNAME(sizeof(dev_name) - 1), dev_name) < 0)
        {
            char sys_path[64];
            snprintf(sys_path, sizeof(sys_path), "/sys/class/input/event%d/device/name", i);
            int sfd = open(sys_path, O_RDONLY | O_CLOEXEC);
            if (sfd >= 0) {
                ssize_t r = read(sfd, dev_name, sizeof(dev_name) - 1);
                if (r > 0) {
                    dev_name[r] = '\0';
                    for (ssize_t k = 0; k < r; k++) {
                        if (dev_name[k] == '\n' || dev_name[k] == '\r')
                            dev_name[k] = '\0';
                    }
                }
                close(sfd);
            }
        }

        if (strstr(dev_name, "ACCDET") != NULL ||
            strstr(dev_name, "accdet") != NULL ||
            strstr(dev_name, "headset") != NULL ||
            strstr(dev_name, "Headset") != NULL)
        {
            is_accdet_dev[i] = true;
        }

#if defined(INNIOASIS_Y1)
        if (y1_enable_center_button_wakeup(i))
            center_button_wakeup_enabled = true;
#endif

        char logmsg[160];
        snprintf(logmsg, sizeof(logmsg), "[button-devinput] Opened %s (name='%s', accdet=%d, fd=%d)\n",
                 path, dev_name, is_accdet_dev[i] ? 1 : 0, fd);
        int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (kfd >= 0) {
            write(kfd, logmsg, strlen(logmsg));
            close(kfd);
        }
    }
}

void button_init_device(void)
{
    num_devices = 0;
#if defined(INNIOASIS_Y1)
    center_button_wakeup_enabled = false;
#endif
    for(int i = 0; i < NR_POLL_DESC; i++)
    {
        poll_fds[i].fd = -1;
        button_add_input_device(i);
    }
#if defined(INNIOASIS_Y1)
    if (!center_button_wakeup_enabled)
        y1_log("[button-devinput] Center-button wake source unavailable; suspend disabled\n");
#endif
#if defined(HAVE_CUSTOM_KEY_HANDLER)
    y1_mfb_init();
#endif
}

void button_remove_input_device(int i)
{
    int fd = poll_fds[i].fd;
    if (fd < 0)
        return;

    if (i == num_devices - 1)
        num_devices = i;

    close(fd);
    poll_fds[i].fd = -1;
}

void button_close_device(void)
{
    num_devices = 0;
    /* close descriptors */
    for(int i = 0; i < NR_POLL_DESC; i++)
    {
        button_remove_input_device(i);
    }
}

#ifdef BUTTON_DELAY_RELEASE
static int button_delay_release = 0;
static int delay_tick = 0;
#endif

#ifdef HAVE_TOUCHSCREEN
/* Last known touchscreen coordinates. */
static int _last_x = 0;
static int _last_y = 0;

/* Last known touchscreen state. */
static enum
{
    TOUCHSCREEN_STATE_UNKNOWN = 0,
    TOUCHSCREEN_STATE_UP,
    TOUCHSCREEN_STATE_DOWN
} _last_touch_state = TOUCHSCREEN_STATE_UNKNOWN;

/* Do _NOT_ change this.  Unofficial Linux API */
#define EVENT_VALUE_TOUCHSCREEN_RELEASE  -1
/* This is only used as an arbitrary placeholder */
#define EVENT_VALUE_TOUCHSCREEN_PRESS    1

static int ts_enabled = 1;

void touchscreen_last_state(int *x, int *y, int *state)
{
    *x = _last_x;
    *y = _last_y;
    *state = _last_touch_state;
}

void touchscreen_enable_device(bool en)
{
    ts_enabled = en;
}

static void handle_touchscreen_event(__u16 code, __s32 value)
{
    switch(code)
    {
        case ABS_X:
        case ABS_MT_POSITION_X:
        {
            _last_x = value;
            break;
        }

        case ABS_Y:
        case ABS_MT_POSITION_Y:
        {
            _last_y = value;
            break;
        }

        case ABS_MT_TRACKING_ID:
        {
            if(value == EVENT_VALUE_TOUCHSCREEN_RELEASE)
            {
                _last_touch_state = TOUCHSCREEN_STATE_UP;
            }
            else
            {
                _last_touch_state = TOUCHSCREEN_STATE_DOWN;
            }
            break;
        }
    }
}
#endif

#ifdef HAVE_BUTTON_DATA
#define BDATA int *data
#else
#define BDATA void
#endif
int button_read_device(BDATA)
{
    static int button_bitmap = 0;
    struct input_event event;

#if defined(HAVE_BUTTON_DATA) && !defined(HAVE_TOUCHSCREEN)
    (void)data;
#endif

#ifdef HAVE_SCROLLWHEEL
    int wheel_ticks = 0;
#endif

#ifdef BUTTON_DELAY_RELEASE
    /* First de-assert delayed-release buttons */
    if (button_delay_release && current_tick >= delay_tick)
    {
        button_bitmap &= ~button_delay_release;
        button_delay_release = 0;
    }
#endif

    /* check if there are any events pending and process them */
    while(poll(poll_fds, num_devices, 0)) {
        for(int i = 0; i < num_devices; i++) {
            /* read only if non-blocking */
            if(poll_fds[i].revents & POLLIN) {
                int size = read(poll_fds[i].fd, &event, sizeof(event));
                if(size == (int)sizeof(event)) {
                    switch(event.type) {
                    case EV_KEY: {
#if defined(HAVE_CUSTOM_KEY_HANDLER)
                        if (button_handle_key_event(event.code, event.value, is_accdet_dev[i]))
                            break;
#endif
                        /* map linux event code to rockbox button bitmap */
                        int bmap = button_map(event.code);

                        /* event.value == 0x10000 means press
                         * event.value == 0 means release
                         */
                        if(event.value) {
#ifdef HAVE_SCROLLWHEEL
                            /* Filter out wheel ticks */
                            if (bmap & BUTTON_SCROLL_BACK)
                                wheel_ticks--;
                            else if (bmap & BUTTON_SCROLL_FWD)
                                wheel_ticks++;
                            bmap &= ~(BUTTON_SCROLL_BACK|BUTTON_SCROLL_FWD);
#endif
#ifdef BUTTON_DELAY_RELEASE
                            bmap &= ~BUTTON_DELAY_RELEASE;
#endif
#if defined(HAVE_TOUCHSCREEN) && defined(BUTTON_TOUCH)
                            /* Some touchscreens give us actual touch/untouch as a "key" */
                            if (bmap & BUTTON_TOUCH) {
                                handle_touchscreen_event(ABS_MT_TRACKING_ID, EVENT_VALUE_TOUCHSCREEN_PRESS);
                                bmap &= ~BUTTON_TOUCH;
                            }
#endif
                            button_bitmap |= bmap;
                        } else {
#if defined(HAVE_TOUCHSCREEN) && defined(BUTTON_TOUCH)
                            /* Some touchscreens give us actual touch/untouch as a "key" */
                            if (bmap & BUTTON_TOUCH) {
                                handle_touchscreen_event(ABS_MT_TRACKING_ID, EVENT_VALUE_TOUCHSCREEN_RELEASE);
                                bmap &= ~BUTTON_TOUCH;
                            }
#endif
#ifdef BUTTON_DELAY_RELEASE
                            /* Delay the release of any requested buttons */
                            if (bmap & BUTTON_DELAY_RELEASE) {
                                button_delay_release |= bmap & ~BUTTON_DELAY_RELEASE;
                                delay_tick = current_tick + HZ/20;
                                bmap = 0;
                            }
#endif
#ifdef HAVE_SCROLLWHEEL
                            /* Wheel gives us press+release back to back; ignore the release */
                            bmap &= ~(BUTTON_SCROLL_BACK|BUTTON_SCROLL_FWD);
#endif
                            button_bitmap &= ~bmap;
                        }
                        break;
                    }
#ifdef HAVE_TOUCHSCREEN
                    case EV_ABS: {
                        if (ts_enabled) {
                            handle_touchscreen_event(event.code, event.value);
                        } else {
                             /* If disabled... ignore */
                            _last_touch_state = TOUCHSCREEN_STATE_UNKNOWN;
                        }
                        break;
                    }
#endif
                    default:
                        /* Ignore other event types */
                        break;
                    }
                }
            }
            /* device was removed/disconnected — close it to stop poll returning POLLHUP forever */
            else if (poll_fds[i].revents & (POLLERR | POLLHUP)) {
                button_remove_input_device(i);
            }
        }
    }

#ifdef HAVE_SCROLLWHEEL
    /* Reset backlight and poweroff timers only when not in HOLD / sleep mode */
    if (wheel_ticks) {
#if defined(INNIOASIS_Y1)
        extern bool y1_is_display_power_saving(void);
        extern bool is_keys_locked(void);
        if (is_backlight_on(false) && !y1_is_display_power_saving() && !is_keys_locked()) {
#ifdef HAVE_BACKLIGHT
            backlight_on();
#endif
#ifdef HAVE_BUTTON_LIGHT
            buttonlight_on();
#endif
            reset_poweroff_timer();
        }
#else
#ifdef HAVE_BACKLIGHT
        backlight_on();
#endif
#ifdef HAVE_BUTTON_LIGHT
        buttonlight_on();
#endif
        reset_poweroff_timer();
#endif
    }

    if (wheel_ticks > 0)
    {
        while (wheel_ticks-- > 0)
        {
            button_queue_post(BUTTON_SCROLL_FWD, 0);
        }
    }
    else if (wheel_ticks < 0)
    {
        while (wheel_ticks++ < 0)
        {
            button_queue_post(BUTTON_SCROLL_BACK, 0);
        }
    }
#endif /* HAVE_SCROLLWHEEL */

#ifdef HAVE_TOUCHSCREEN
    int touch = touchscreen_to_pixels(_last_x, _last_y, data);

    if(_last_touch_state == TOUCHSCREEN_STATE_DOWN)
    {
        return button_bitmap | touch;
    }
#endif

    return button_bitmap;
}
