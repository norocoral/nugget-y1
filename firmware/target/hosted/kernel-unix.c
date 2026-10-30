/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (c) 2010 Thomas Martitz
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


#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "audio.h"
#include "panic.h"
#include "timer.h"

/* Forward-declaration */
void interrupt(void);

#if defined(INNIOASIS_Y1)
#include "innioasis/y1/bt-y1.h"
extern bool y1_is_screen_sleeping(void);
extern bool y1_is_display_power_saving(void);
extern bool y1_should_always_on(void);
extern bool y1_playback_active(void);
extern int  y1_wait_for_input_events(int timeout_ms);
extern bool y1_button_hold_active(void);
#endif

static pthread_cond_t wfi_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t wfi_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned int tick_interval_ms = 10;
static pthread_t tick_thread_id;
static bool tick_thread_started = false;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

#if defined(INNIOASIS_Y1)
static bool y1_can_deep_idle_tick(uint64_t now_ms, uint64_t last_active_ms)
{
    if (now_ms - last_active_ms < 5000ULL)
        return false;
    if (!y1_is_screen_sleeping() || y1_is_display_power_saving() || y1_should_always_on())
        return false;
    if (y1_playback_active() || y1_bt_is_receiver_mode() || y1_bt_is_scanning())
        return false;
    if (y1_button_hold_active())
        return false;
    return true;
}
#endif

static void *tick_worker(void *arg)
{
    (void)arg;
    unsigned int iv_ms = tick_interval_ms ? tick_interval_ms : 10;
#if defined(INNIOASIS_Y1)
    uint64_t last_active_ms = monotonic_ms();
#endif

    while (1)
    {
#if defined(INNIOASIS_Y1)
        uint64_t before_ms = monotonic_ms();
        if (y1_can_deep_idle_tick(before_ms, last_active_ms))
        {
            /* Screen is off and player is idle: block in kernel poll() on input
             * devices for up to 250ms so CPU 0 stays in uninterrupted SPM Deep Idle,
             * while waking in < 1ms the instant any button is pressed. */
            int ev = y1_wait_for_input_events(250);
            uint64_t after_ms = monotonic_ms();
            uint64_t elapsed_ms = (after_ms > before_ms) ? (after_ms - before_ms) : iv_ms;
            long ticks_elapsed = (long)(elapsed_ms / iv_ms);
            if (ticks_elapsed > 1)
                current_tick += (ticks_elapsed - 1);
            if (ev > 0)
                last_active_ms = after_ms;

            call_tick_tasks();
            interrupt();
            continue;
        }
        if (!y1_is_screen_sleeping() || y1_playback_active() || y1_button_hold_active())
            last_active_ms = before_ms;
#endif
        usleep(iv_ms * 1000U);
        call_tick_tasks();
        interrupt();
    }
    return NULL;
}

/*
 * wait on the sem which the signal handler posts to save cpu time (aka sleep)
 *
 * other mechanisms could use them as well */
void wait_for_interrupt(void)
{
    pthread_cond_wait(&wfi_cond, &wfi_mtx);
}

/*
 * Wakeup the kernel, if sleeping (shall not be called from a signal handler) */
void interrupt(void)
{
    pthread_cond_signal(&wfi_cond);
}

void tick_start(unsigned int interval_in_ms)
{
    tick_interval_ms = interval_in_ms ? interval_in_ms : 10;

    /* Grab the mutex already now and leave it to this thread. */
    pthread_mutex_lock(&wfi_mtx);

    if (!tick_thread_started)
    {
        tick_thread_started = true;
        int ret = pthread_create(&tick_thread_id, NULL, tick_worker, NULL);
        if (ret != 0)
            panicf("%s(): %s\n", __func__, strerror(ret));
    }
}

#define cycles_to_microseconds(cycles) \
    ((int)((10000*cycles)/(TIMER_FREQ / 100)))


static timer_t timer_tid;
static int timer_prio = -1;
void (*global_unreg_callback)(void);
void (*global_timer_callback)(void);

static void timer_cb(union sigval arg)
{
    /* check for spurious callbacks [arg.sival_ptr] */
    if (global_timer_callback && global_timer_callback == arg.sival_ptr)
        global_timer_callback();
}

bool timer_register(int reg_prio, void (*unregister_callback)(void),
                    long cycles, void (*timer_callback)(void))
{
    int ret = 0;
    struct itimerspec ts;
    sigevent_t sigev;
    long in_us = cycles_to_microseconds(cycles);

    if (reg_prio <= timer_prio || in_us <= 0)
        return false;

    if(timer_prio >= 0)
    {
        if (global_unreg_callback) /* timer has callback user needs to unreg */
            global_unreg_callback();
        else /* no callback -- delete timer */
            timer_delete(timer_tid);
    }

    memset(&sigev, 0, sizeof(sigevent_t));
    sigev.sigev_notify = SIGEV_THREAD,
    sigev.sigev_notify_function = timer_cb;
    sigev.sigev_value.sival_ptr = timer_callback; /* store cb to check later */

    div_t q = div(in_us, 1000000);
    ts.it_value.tv_sec = ts.it_interval.tv_sec = q.quot;
    ts.it_value.tv_nsec = ts.it_interval.tv_nsec = q.rem*1000;

    /* add the timer */
    ret |= timer_create(CLOCK_REALTIME, &sigev, &timer_tid);
    ret |= timer_settime(timer_tid, 0, &ts, NULL);

    global_timer_callback = timer_callback;
    global_unreg_callback = unregister_callback;
    timer_prio = reg_prio;

    return ret == 0;
}

bool timer_set_period(long cycles)
{
    struct itimerspec ts;
    long in_us = cycles_to_microseconds(cycles);
    div_t q = div(in_us, 1000000);
    ts.it_value.tv_sec = ts.it_interval.tv_sec = q.quot;
    ts.it_value.tv_nsec = ts.it_interval.tv_nsec = q.rem*1000;

    return timer_settime(timer_tid, 0, &ts, NULL) == 0;
}

void timer_unregister(void)
{
    timer_delete(timer_tid);
    timer_prio = -1;
    global_unreg_callback = NULL;
    global_timer_callback = NULL;
}
