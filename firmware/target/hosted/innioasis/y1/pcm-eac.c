/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox Innioasis Y1 Port
 * MediaTek MT6572 direct /dev/eac PCM Sink Driver
 ****************************************************************************/

#define RB_FILESYSTEM_OS
#include "autoconf.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

#ifndef makedev
#define makedev(maj, min) (((maj) << 8) | (min))
#endif

#include "system.h"
#include "debug.h"
#include "kernel.h"
#include "panic.h"
#include "pcm.h"
#include "pcm_sampr.h"
#include "pcm-internal.h"
#include "settings.h"
#include "pcm_mixer.h"
#include "pcm_sink.h"
#include "pcm_sw_volume.h"
#include "audiohw.h"
#include "bt-y1.h"
#include "pcm-eac.h"
#include "y1-log.h"

#define SET_SPEAKER_OFF          0x400443a2
#define SET_HEADPHONE_ON         0x400443a4
#define SET_HEADPHONE_OFF        0x400443a5
#define SET_EARPIECE_OFF         0x400443a7
#define AUDDRV_ALLOCATE_DL1_BUF  0xc0044310
#define AUDDRV_FREE_DL1_BUF      0xc0044311
#define FREE_MEMIF_BUF           0xc004431f
#define SET_MEMIF_FP             0xc0044320
#define FREE_MEMIF_FP            0xc0044321
#define AUDDRV_SET_AFEREG        0xc0044300
#define AUDDRV_SET_ANAREG        0xc0044302
#define AUDDRV_GET_ANAREG        0xc0044303
#define AUD_DRV_AFE_CLK          0xc0044355
#define AUD_DRV_AIF_CLK          0xc0044351

struct Register_Control {
    uint32_t offset;
    uint32_t value;
    uint32_t mask;
};

static int eac_fd = -1;
static unsigned long current_sampr = 44100;
static unsigned long programmed_sampr;
static bool audio_output_enabled;
static bool y1_deep_idle_enabled = true;
static bool y1_cpu_powerdown_enabled = true;
static int wakelock_fd = -1;
static volatile sig_atomic_t dma_stopped = 1;
static volatile sig_atomic_t dma_locked = 0;
#define Y1_BT_PCM_RING_FRAMES 8192
static int16_t bt_pcm_ring[Y1_BT_PCM_RING_FRAMES * 2];
static size_t bt_pcm_ring_read;
static size_t bt_pcm_ring_write;
static volatile bool bt_receiver_active;
static volatile bool bt_receiver_mode_power_hold;
static long bt_receiver_last_pcm_tick;
static pthread_mutex_t bt_pcm_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pcm_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcm_cond = PTHREAD_COND_INITIALIZER;

static void sink_set_freq(uint16_t freq);
static bool y1_set_deep_idle(bool enable);
static void y1_audio_set_output_enabled(bool enable);
static int vol_cb_to_hp_idx(int vol_cb);
static int vol_cb_to_sw_remainder(int vol_cb, int hp_idx);

static bool y1_audio_receiver_is_active(void)
{
    pthread_mutex_lock(&bt_pcm_mtx);
    bool active = bt_receiver_active;
    pthread_mutex_unlock(&bt_pcm_mtx);
    return active;
}

static void y1_play_wake_lock(bool hold)
{
    if (wakelock_fd < 0)
        wakelock_fd = open("/sys/power/wake_lock", O_WRONLY | O_CLOEXEC);
    if (wakelock_fd < 0)
        return;
    if (hold)
        (void)!write(wakelock_fd, "y1play\n", 7);
    else {
        int ufd = open("/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC);
        if (ufd >= 0) {
            (void)!write(ufd, "y1play\n", 7);
            close(ufd);
        }
    }
}

static void y1_audio_clocks(int on)
{
    if (eac_fd < 0)
        return;
    ioctl(eac_fd, AUD_DRV_AFE_CLK, on);
    ioctl(eac_fd, AUD_DRV_AIF_CLK, on);
}
static int pending_vol_l = 0;
static int pending_vol_r = 0;

void y1_audio_receiver_set_active(bool active, unsigned long sample_rate)
{
    if (active && !y1_bt_is_receiver_mode())
        return;

    pthread_mutex_lock(&bt_pcm_mtx);
    if (bt_receiver_active == active) {
        pthread_mutex_unlock(&bt_pcm_mtx);
        return;
    }
    bt_receiver_active = active;
    bt_pcm_ring_read = 0;
    bt_pcm_ring_write = 0;
    bt_receiver_last_pcm_tick = current_tick;
    pthread_mutex_unlock(&bt_pcm_mtx);

    if (active) {
        y1_set_deep_idle(false);
        y1_play_wake_lock(true);
        sink_set_freq((uint16_t)sample_rate);
        if (global_settings.bt_receiver_full_volume)
            audiohw_set_volume(60, 60);
    } else if (dma_stopped != 0) {
        y1_audio_set_output_enabled(false);
        if (!bt_receiver_mode_power_hold) {
            y1_play_wake_lock(false);
            y1_set_deep_idle(true);
        }
    }

    pthread_mutex_lock(&pcm_mtx);
    pthread_cond_signal(&pcm_cond);
    pthread_mutex_unlock(&pcm_mtx);
}

void y1_audio_receiver_mode_set(bool enabled)
{
    bt_receiver_mode_power_hold = enabled;
    if (enabled) {
        y1_set_deep_idle(false);
        y1_play_wake_lock(true);
    } else if (!y1_audio_receiver_is_active()) {
        y1_play_wake_lock(false);
        y1_set_deep_idle(true);
    }
}

size_t y1_audio_receiver_write(const int16_t *samples, size_t frames)
{
    if (!samples || !y1_bt_is_receiver_mode())
        return 0;

    size_t written = 0;
    pthread_mutex_lock(&bt_pcm_mtx);
    while (bt_receiver_active && written < frames) {
        size_t next = (bt_pcm_ring_write + 1) % Y1_BT_PCM_RING_FRAMES;
        if (next == bt_pcm_ring_read)
            break;
        bt_pcm_ring[bt_pcm_ring_write * 2] = samples[written * 2];
        bt_pcm_ring[bt_pcm_ring_write * 2 + 1] = samples[written * 2 + 1];
        bt_pcm_ring_write = next;
        written++;
    }
    if (frames > 0)
        bt_receiver_last_pcm_tick = current_tick;
    pthread_mutex_unlock(&bt_pcm_mtx);
    return written;
}

static int16_t y1_receiver_apply_volume(int16_t sample, int volume_cb)
{
    static const int32_t gain_q15[] = {
        29204, 32768, 36766, 41259, 46286, 51970, 58270, 65381
    };

    if (volume_cb <= -740)
        return 0;

    int hp_idx = vol_cb_to_hp_idx(volume_cb);
    int sw_cb = vol_cb_to_sw_remainder(volume_cb, hp_idx);
    if (sw_cb <= -740)
        return 0;
    if (sw_cb < -10)
        sw_cb = -10;
    else if (sw_cb > 60)
        sw_cb = 60;

    int32_t scaled = ((int32_t)sample *
                      gain_q15[(sw_cb + 10) / 10]) >> 15;
    if (scaled > INT16_MAX)
        scaled = INT16_MAX;
    else if (scaled < INT16_MIN)
        scaled = INT16_MIN;
    return (int16_t)scaled;
}

/* MT6323 AUDTOP_CON5 (0x70a): HPL bits [14:12], HPR bits [10:8], 0..7.
 * 2 dB/step, index 7 = +14 dB. Low bits 0x0014 keep the HP path powered.
 * Rockbox still uses 1 dB UI steps (-74..+6). Hardware is shifted up by
 * +14 dB so analog max is used at 0 dB UI and the floor is not silent. */
#define MT6323_AUDTOP_CON5       0x70a
#define MT6323_HP_PGA_MUX        0x0014
#define MT6323_HP_PGA_MAX_IDX    7
#define MT6323_HP_PGA_STEP_CB    20
#define MT6323_HP_PGA_MAX_CB     (MT6323_HP_PGA_MAX_IDX * MT6323_HP_PGA_STEP_CB)
#define MT6323_HP_PGA_OFFSET_CB  MT6323_HP_PGA_MAX_CB

static inline int set_afereg(int fd, uint32_t offset, uint32_t val, uint32_t mask)
{
    struct Register_Control reg = { offset, val, mask };
    return ioctl(fd, AUDDRV_SET_AFEREG, &reg);
}

static inline int set_anareg(int fd, uint32_t offset, uint32_t val, uint32_t mask)
{
    struct Register_Control reg = { offset, val, mask };
    return ioctl(fd, AUDDRV_SET_ANAREG, &reg);
}

static inline uint32_t get_anareg(int fd, uint32_t offset)
{
    struct Register_Control reg = { offset, 0, ~0U };
    if (ioctl(fd, AUDDRV_GET_ANAREG, &reg) == 0)
        return reg.value;
    return 0;
}

int y1_read_pmic_reg(uint32_t offset)
{
    if (eac_fd < 0)
        return -1;
    struct Register_Control reg = { offset, 0, ~0U };
    if (ioctl(eac_fd, AUDDRV_GET_ANAREG, &reg) == 0)
        return (int)reg.value;
    return -1;
}

void y1_write_pmic_reg(uint32_t offset, uint32_t val, uint32_t mask)
{
    if (eac_fd < 0)
        return;
    set_anareg(eac_fd, offset, val, mask);
}

static inline void top_ctl_trigger(int fd)
{
    uint32_t val = get_anareg(fd, 0x4016);
    uint32_t toggle = ((~val) << 8) & 0x300;
    set_anareg(fd, 0x4016, toggle, 0x300);
}

static void log_audio_debug(const char *msg)
{
    y1_log(msg);
}

static bool y1_write_power_state(const char *value)
{
    int fd = open("/sys/power/dpidle_state", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        log_audio_debug("[pcm-eac] Failed to open deep-idle control\n");
        return false;
    }

    size_t length = strlen(value);
    bool success = write(fd, value, length) == (ssize_t)length;
    close(fd);

    if (!success)
        log_audio_debug("[pcm-eac] Failed to write MT6572 deep-idle control\n");
    return success;
}

static bool y1_set_deep_idle(bool enable)
{
    const char *deep_idle = enable ? "1\n" : "0\n";
    const char *cpu_powerdown = enable ? "cpupdn 1\n" : "cpupdn 0\n";
    bool success = true;

    if (y1_cpu_powerdown_enabled != enable) {
        if (y1_write_power_state(cpu_powerdown))
            y1_cpu_powerdown_enabled = enable;
        else
            success = false;
    }

    if (y1_deep_idle_enabled != enable) {
        if (y1_write_power_state(deep_idle))
            y1_deep_idle_enabled = enable;
        else
            success = false;
    }

    return success;
}

void audiohw_preinit(void) { }
void audiohw_postinit(void) { }
void audiohw_set_frequency(int fsel) { (void)fsel; }

static int vol_cb_to_hp_idx(int vol_cb)
{
    int target_cb;
    int analog_cb;

    if (vol_cb <= -740)
        return 0;

    target_cb = vol_cb + MT6323_HP_PGA_OFFSET_CB;
    if (target_cb <= 0)
        return 0;

    /* Analog at or above target so the 1 dB remainder is 0 or -1 dB, not a
     * large digital cut that buries the DAC in its noise floor. */
    analog_cb = ((target_cb + MT6323_HP_PGA_STEP_CB - 1) / MT6323_HP_PGA_STEP_CB)
                * MT6323_HP_PGA_STEP_CB;
    if (analog_cb > MT6323_HP_PGA_MAX_CB)
        analog_cb = MT6323_HP_PGA_MAX_CB;
    return analog_cb / MT6323_HP_PGA_STEP_CB;
}

static int vol_cb_to_sw_remainder(int vol_cb, int hp_idx)
{
    int target_cb;
    int analog_cb;

    if (vol_cb <= -740)
        return PCM_MUTE_LEVEL;

    target_cb = vol_cb + MT6323_HP_PGA_OFFSET_CB;
    analog_cb = hp_idx * MT6323_HP_PGA_STEP_CB;
    return target_cb - analog_cb;
}

static void apply_hp_analog_gain(int idx_l, int idx_r)
{
    uint32_t con5;

    if (eac_fd < 0)
        return;

    con5 = MT6323_HP_PGA_MUX |
           ((uint32_t)idx_l << 12) |
           ((uint32_t)idx_r << 8);
    set_anareg(eac_fd, MT6323_AUDTOP_CON5, con5, 0xffff);
}

static void y1_audio_set_output_enabled(bool enable)
{
    if (eac_fd < 0 || audio_output_enabled == enable)
        return;

    if (!enable)
    {
        set_afereg(eac_fd, 0x10, 0, 1 << 1);
        ioctl(eac_fd, SET_HEADPHONE_OFF, 0);
        set_anareg(eac_fd, 0x4014, 0, 1);
        set_anareg(eac_fd, 0x4000, 0, 1);
        top_ctl_trigger(eac_fd);
        set_afereg(eac_fd, 0x10, 0, 1 << 0);
        y1_audio_clocks(0);
        audio_output_enabled = false;
        programmed_sampr = 0;
        return;
    }

    y1_audio_clocks(1);
    set_anareg(eac_fd, 0x106, 1, 0xffff);
    set_anareg(eac_fd, 0x10c, 0x100, 0x100);
    set_afereg(eac_fd, 0x10, 1, 1);
    audio_output_enabled = true;
    sink_set_freq((uint16_t)current_sampr);
    set_anareg(eac_fd, 0x700, 0x7800, 0x7f80);
    set_anareg(eac_fd, 0x704, 0x00f0, 0x00ff);
    set_anareg(eac_fd, 0x702, 0x0500, 0x0700);
    set_anareg(eac_fd, 0x700, 0x0050, 0x0070);
    set_anareg(eac_fd, 0x710, 0x0008, 0x0008);
    set_anareg(eac_fd, 0x700, 0x0180, 0x0180);
    set_anareg(eac_fd, 0x704, 0x0003, 0x0003);
    set_anareg(eac_fd, 0x70c, 0xf7f2, 0xffff);
    set_anareg(eac_fd, 0x700, 0x7000, 0xf000);
    set_anareg(eac_fd, 0x70a, 0x0014, 0xffff);
    set_anareg(eac_fd, 0x708, 0x007c, 0xffff);
    usleep(10000);
    set_anareg(eac_fd, 0x70c, 0xf5ba, 0xffff);
    apply_hp_analog_gain(vol_cb_to_hp_idx(pending_vol_l),
                         vol_cb_to_hp_idx(pending_vol_r));
    set_anareg(eac_fd, 0x4014, 1, 1);
    top_ctl_trigger(eac_fd);
    set_anareg(eac_fd, 0x4000, 1, 1);
    ioctl(eac_fd, SET_HEADPHONE_ON, 3);

    if ((dma_stopped == 0 && dma_locked == 0) ||
        y1_audio_receiver_is_active())
        set_afereg(eac_fd, 0x10, 1 << 1, 1 << 1);
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    /* vol_l / vol_r are centibels (-740 to +60), 1 dB UI steps. Analog HP
     * PGA is the coarse control; PCM only supplies the 1 dB remainder. */
    if (y1_bt_is_receiver_mode() && global_settings.bt_receiver_full_volume)
        vol_l = vol_r = 60;

    int idx_l = vol_cb_to_hp_idx(vol_l);
    int idx_r = vol_cb_to_hp_idx(vol_r);
    int sw_l = vol_cb_to_sw_remainder(vol_l, idx_l);
    int sw_r = vol_cb_to_sw_remainder(vol_r, idx_r);

    pending_vol_l = vol_l;
    pending_vol_r = vol_r;

    if (y1_bt_a2dp_is_streaming())
    {
        if (sw_l > 0) sw_l = 0;
        if (sw_r > 0) sw_r = 0;
    }

    pcm_set_master_volume(sw_l, sw_r);
    apply_hp_analog_gain(idx_l, idx_r);
}
static pthread_t pcm_thread;

static volatile bool pcm_thread_exit = false;

static const void *pcm_data = NULL;
static size_t pcm_size = 0;

static void *pcm_worker(void *arg)
{
    (void)arg;

    while (!pcm_thread_exit)
    {
        pthread_mutex_lock(&pcm_mtx);
        while ((dma_stopped == 1 || dma_locked == 1) &&
               !y1_audio_receiver_is_active() && !pcm_thread_exit)
        {
            pthread_cond_wait(&pcm_cond, &pcm_mtx);
        }
        pthread_mutex_unlock(&pcm_mtx);

        if (pcm_thread_exit)
            break;

        if (y1_audio_receiver_is_active())
        {
            int16_t samples[512 * 2];
            size_t frames = 0;
            pthread_mutex_lock(&bt_pcm_mtx);
            while (frames < 512 && bt_pcm_ring_read != bt_pcm_ring_write) {
                samples[frames * 2] = bt_pcm_ring[bt_pcm_ring_read * 2];
                samples[frames * 2 + 1] = bt_pcm_ring[bt_pcm_ring_read * 2 + 1];
                bt_pcm_ring_read = (bt_pcm_ring_read + 1) %
                                   Y1_BT_PCM_RING_FRAMES;
                frames++;
            }
            pthread_mutex_unlock(&bt_pcm_mtx);

            if (frames == 0) {
                pthread_mutex_lock(&bt_pcm_mtx);
                long last_pcm_tick = bt_receiver_last_pcm_tick;
                pthread_mutex_unlock(&bt_pcm_mtx);
                if (audio_output_enabled &&
                    !TIME_BEFORE(current_tick,
                                 last_pcm_tick + MAX(HZ / 2, 1)))
                    y1_audio_set_output_enabled(false);
                usleep(20000);
                continue;
            }

            if (!y1_audio_receiver_is_active())
                continue;

            if (!audio_output_enabled)
                y1_audio_set_output_enabled(true);
            if (!y1_audio_receiver_is_active()) {
                y1_audio_set_output_enabled(false);
                continue;
            }

            int volume_l = pending_vol_l;
            int volume_r = pending_vol_r;
            if (y1_bt_is_receiver_mode() &&
                !global_settings.bt_receiver_full_volume) {
                for (size_t i = 0; i < frames; i++) {
                    samples[i * 2] =
                        y1_receiver_apply_volume(samples[i * 2], volume_l);
                    samples[i * 2 + 1] =
                        y1_receiver_apply_volume(samples[i * 2 + 1],
                                                 volume_r);
                }
            }

            size_t offset = 0;
            size_t bytes = frames * 2 * sizeof(int16_t);
            while (offset < bytes && y1_audio_receiver_is_active() &&
                   eac_fd >= 0) {
                ssize_t result = write(eac_fd,
                                       (const uint8_t *)samples + offset,
                                       bytes - offset);
                if (result > 0)
                    offset += (size_t)result;
                else if (result < 0 && errno == EINTR)
                    continue;
                else if (result < 0 && errno == EAGAIN)
                    usleep(1000);
                else
                    break;
            }
            continue;
        }

        if (pcm_size == 0)
        {
            /* Request new PCM buffer from Rockbox without holding mutex */
            if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &pcm_data, &pcm_size))
            {
                usleep(1000);
                continue;
            }
            pcm_play_dma_status_callback(PCM_DMAST_STARTED);
        }

        if (eac_fd >= 0 && pcm_data && pcm_size > 0)
        {
            const char *buf = (const char *)pcm_data;
            size_t remaining = pcm_size;

            while (remaining > 0 && dma_stopped == 0 && dma_locked == 0 && !pcm_thread_exit)
            {
                /* Always write exact multiples of 4 bytes (16-bit stereo frame = 4 bytes) */
                size_t chunk = remaining > 4096 ? 4096 : (remaining & ~3);
                if (chunk == 0)
                    break;

                bool bt_active = y1_bt_a2dp_is_streaming();
                if (bt_active && audio_output_enabled)
                    y1_audio_set_output_enabled(false);
                else if (!bt_active && !audio_output_enabled && dma_stopped == 0)
                    y1_audio_set_output_enabled(true);

                ssize_t written;
                if (bt_active) {
                    /* Keep A2DP paced without driving the analog DAC. */
                    y1_bt_a2dp_write_pcm(buf, chunk, current_sampr);
                    if (current_sampr)
                        usleep((useconds_t)((chunk / 4) * 1000000UL / current_sampr));
                    written = (ssize_t)chunk;
                } else {
                    written = write(eac_fd, buf, chunk);
                }
                if (!bt_active && written < 0 && errno != EAGAIN && errno != EINTR) {
                    char wmsg[128];
                    snprintf(wmsg, sizeof(wmsg), "[pcm-eac] write error: %zd (errno=%d %s)\n",
                             written, errno, strerror(errno));
                    log_audio_debug(wmsg);
                }

                if (written > 0)
                {
                    /* Ensure byte offset is aligned to 4 bytes */
                    written &= ~3;
                    if (written == 0) {
                        usleep(1000);
                        continue;
                    }
                    remaining -= written;
                    buf += written;
                }
                else if (written < 0 && (errno == EAGAIN || errno == EINTR))
                {
                    usleep(1000);
                }
                else
                {
                    usleep(2000);
                    break;
                }
            }

            if (dma_stopped == 1 || pcm_thread_exit)
            {
                pcm_size = 0;
                pcm_data = NULL;
            }
            else
            {
                pcm_data = buf;
                pcm_size = remaining;
            }
        }
        else
        {
            static int no_fd_log = 0;
            if (no_fd_log++ % 500 == 0) {
                log_audio_debug("[pcm-eac] Warning: eac_fd < 0 in worker loop\n");
            }
            usleep(2000);
        }
    }

    return NULL;
}

static int play_lock_recursion = -10000;

static void sink_lock(void)
{
    ++play_lock_recursion;
    if (play_lock_recursion == 1)
    {
        pthread_mutex_lock(&pcm_mtx);
        dma_locked = 1;
        pthread_mutex_unlock(&pcm_mtx);
    }
}

static void sink_unlock(void)
{
    --play_lock_recursion;
    if (play_lock_recursion == 0)
    {
        pthread_mutex_lock(&pcm_mtx);
        dma_locked = 0;
        pthread_cond_signal(&pcm_cond);
        pthread_mutex_unlock(&pcm_mtx);
    }
}

static void sink_dma_init(void)
{
    if (eac_fd < 0)
    {
        /* 1. Discover exact dynamic minor number assigned by kernel */
        int maj = 10, min = -1;
        FILE *fp = fopen("/sys/class/misc/eac/dev", "r");
        if (fp) {
            if (fscanf(fp, "%d:%d", &maj, &min) == 2 && min >= 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "[pcm-eac] Discovered /sys/class/misc/eac/dev: major %d, minor %d\n", maj, min);
                log_audio_debug(msg);
                unlink("/dev/eac");
                mknod("/dev/eac", S_IFCHR | 0666, makedev(maj, min));
            }
            fclose(fp);
        }

        eac_fd = open("/dev/eac", O_RDWR);
        if (eac_fd < 0)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "[pcm-eac] ERROR: open(/dev/eac) failed: %s (errno=%d)\n", strerror(errno), errno);
            log_audio_debug(msg);
        }
        else
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "[pcm-eac] open(/dev/eac) SUCCESS (fd=%d, major=%d, minor=%d)\n", eac_fd, maj, min);
            log_audio_debug(msg);

            /* 1. Turn on Audio Clocks (Clock 4: AFE, Clock 1: AIF) */
            ioctl(eac_fd, AUD_DRV_AFE_CLK, 1);
            ioctl(eac_fd, AUD_DRV_AIF_CLK, 1);

            /* 2. Reset and release any previous buffer or filp reservation */
            ioctl(eac_fd, FREE_MEMIF_FP, 0);
            ioctl(eac_fd, AUDDRV_FREE_DL1_BUF, 0);
            ioctl(eac_fd, FREE_MEMIF_BUF, 0);

            /* 3. Allocate coherent DL1 DMA ring buffer (32768 bytes) */
            int ret_alloc = ioctl(eac_fd, AUDDRV_ALLOCATE_DL1_BUF, 32768);
            snprintf(msg, sizeof(msg), "[pcm-eac] AUDDRV_ALLOCATE_DL1_BUF(32768) returned %d (errno=%d %s)\n",
                     ret_alloc, errno, ret_alloc < 0 ? strerror(errno) : "OK");
            log_audio_debug(msg);

            /* 4. Configure DL1 interface to this file descriptor */
            int ret_fp = ioctl(eac_fd, SET_MEMIF_FP, 0);
            snprintf(msg, sizeof(msg), "[pcm-eac] SET_MEMIF_FP(0) returned %d (errno=%d %s)\n",
                     ret_fp, errno, ret_fp < 0 ? strerror(errno) : "OK");
            log_audio_debug(msg);

            /* 5. Set up AFE Digital Interconnections: DL1 -> I2S Out */
            set_afereg(eac_fd, 0x24, 1 << 21, 1 << 21); /* DL1 Left -> I2S Out Left (CONN1 bit 21) */
            set_afereg(eac_fd, 0x28, 1 << 6,  1 << 6);  /* DL1 Right -> I2S Out Right (CONN2 bit 6) */

            /* 6. Configure initial sample rate (44.1kHz default: rate_code = 9) */
            set_afereg(eac_fd, 0x14, 9, 0x0f);                  /* AFE_DAC_CON1: DL1 44.1kHz */
            set_afereg(eac_fd, 0x3a0, (9 << 4) | 1, 0xf1);      /* AFE_IRQ_MCU_CON: IRQ1 44.1kHz & IRQ1 Enable */
            set_afereg(eac_fd, 0x3ac, 1024, ~0U);               /* AFE_IRQ_MCU_CNT1: 1024 samples period */
            set_afereg(eac_fd, 0x108, 0x73001803, ~0U);         /* AFE_PMIC_NEWIF_CFG0 */
            set_afereg(eac_fd, 0x10c, 0xf74f0000, ~0U);         /* AFE_PMIC_NEWIF_CFG1 */
            set_afereg(eac_fd, 0x34,  0x00000909, ~0U);         /* AFE_I2S_CON1: 44.1kHz, 16-bit stereo */
            set_afereg(eac_fd, 0x124, 1, 1);                    /* AFE_PMIC_NEWIF_CFG2: Enable I2S DAC */
            set_afereg(eac_fd, 0x4c4, 0, 0x10);                 /* Clear bit 4 of 0x4c4 */

            /* 7. Enable AFE Master Switch (bit 0: AFE_ON) and DL1 DMA (bit 1: DL1_ON) */
            set_afereg(eac_fd, 0x10, (1 << 0) | (1 << 1), (1 << 0) | (1 << 1));

            /* 8. Mute unused endpoints */
            ioctl(eac_fd, SET_SPEAKER_OFF, 0);
            ioctl(eac_fd, SET_EARPIECE_OFF, 0);

            /* 9. Power up MT6323 PMIC Audio Clocks */
            set_anareg(eac_fd, 0x106, 1, 0xffff);        /* PMIC TOP_CKPDN_CON0: Power on audio clock */
            set_anareg(eac_fd, 0x10c, 0x100, 0x100);    /* PMIC TOP_CKG_CON1 bit 8: Audio 13M clock gating */

            /* 10. Power up MT6323 PMIC Audio Downlink DAC & Clocks */
            set_anareg(eac_fd, 0x700, 0x7800, 0x7f80);
            set_anareg(eac_fd, 0x704, 0x00f0, 0x00ff);
            set_anareg(eac_fd, 0x702, 0x0500, 0x0700); /* Maximum preamp buffer gain Right (5 << 8) */
            set_anareg(eac_fd, 0x700, 0x0050, 0x0070); /* Maximum preamp buffer gain Left (5 << 4) */
            set_anareg(eac_fd, 0x710, 0x0008, 0x0008);
            set_anareg(eac_fd, 0x700, 0x0180, 0x0180);
            set_anareg(eac_fd, 0x704, 0x0003, 0x0003);

            /* 11. Power up and configure MT6323 PMIC Headphone Amplifier */
            set_anareg(eac_fd, 0x70c, 0xf7f2, 0xffff);
            set_anareg(eac_fd, 0x700, 0x7000, 0xf000);
            set_anareg(eac_fd, 0x70a, 0x0014, 0xffff);
            set_anareg(eac_fd, 0x708, 0x007c, 0xffff);
            usleep(10000); /* 10ms settling time */
            set_anareg(eac_fd, 0x70c, 0xf5ba, 0xffff);
            apply_hp_analog_gain(vol_cb_to_hp_idx(pending_vol_l),
                                 vol_cb_to_hp_idx(pending_vol_r));
            ioctl(eac_fd, SET_HEADPHONE_ON, 3);

            /* 12. Configure MT6323 PMIC Digital Audio Downlink (NewIF & Codec) */
            set_anareg(eac_fd, 0x4024, (7 << 12) | 0x330, 0xffff); /* PMIC NewIF DL 44.1kHz (0x7330) */
            set_anareg(eac_fd, 0x4002, 9, 0x0f);                   /* PMIC Codec DL 44.1kHz */
            set_anareg(eac_fd, 0x4006, 0, 0xffff);                 /* PMIC DL Left digital gain: 0dB */
            set_anareg(eac_fd, 0x4008, 0, 0xffff);                 /* PMIC DL Right digital gain: 0dB */
            set_anareg(eac_fd, 0x4014, 1, 1);                      /* PMIC Downlink channel enable */

            /* 13. PMIC CDC Clock Synchronization & Master Enable */
            top_ctl_trigger(eac_fd);
            set_anareg(eac_fd, 0x4000, 1, 1);                      /* PMIC TOP_CON0: Audio Digital Master ON */
            audio_output_enabled = true;
            programmed_sampr = current_sampr;
            log_audio_debug("[pcm-eac] MT6323 PMIC Digital Audio Codec & Headphone Amp fully configured.\n");
        }
    }

    y1_audio_set_output_enabled(false);
    pcm_thread_exit = false;
    dma_stopped = 1;
    dma_locked = 1;
    play_lock_recursion = -10000;
    pthread_create(&pcm_thread, NULL, pcm_worker, NULL);
    struct sched_param sp;
    sp.sched_priority = 80;
    pthread_setschedparam(pcm_thread, SCHED_FIFO, &sp);
}

static void sink_dma_postinit(void)
{
    play_lock_recursion = 0;
    dma_locked = 0;
    audiohw_postinit();
    y1_bt_init();
}

static void sink_set_freq(uint16_t freq)
{
    unsigned long sampr = (freq < HW_NUM_FREQ) ? hw_freq_sampr[freq] : freq;
    if (sampr == current_sampr && sampr == programmed_sampr)
    {
        return;
    }
    current_sampr = sampr;

    if (eac_fd >= 0 && audio_output_enabled)
    {
        uint32_t dac_con1;
        uint32_t rate_code;
        uint32_t pmic_newif_rate;

        char fmsg[96];
        snprintf(fmsg, sizeof(fmsg), "[pcm-eac] sink_set_freq: raw=%u -> sampr=%lu Hz\n", freq, sampr);
        log_audio_debug(fmsg);

        switch (sampr)
        {
            case 48000:
                dac_con1 = 0x83001803;
                rate_code = 10;
                pmic_newif_rate = 8;
                break;
            case 32000:
                dac_con1 = 0x63001803;
                rate_code = 8;
                pmic_newif_rate = 6;
                break;
            case 24000:
                dac_con1 = 0x53001803;
                rate_code = 6;
                pmic_newif_rate = 5;
                break;
            case 22050:
                dac_con1 = 0x43001803;
                rate_code = 5;
                pmic_newif_rate = 4;
                break;
            case 16000:
                dac_con1 = 0x33001823;
                rate_code = 4;
                pmic_newif_rate = 3;
                break;
            case 12000:
                dac_con1 = 0x23001803;
                rate_code = 2;
                pmic_newif_rate = 2;
                break;
            case 11025:
                dac_con1 = 0x13001803;
                rate_code = 1;
                pmic_newif_rate = 1;
                break;
            case 8000:
                dac_con1 = 0x03001823;
                rate_code = 0;
                pmic_newif_rate = 0;
                break;
            case 44100:
            default:
                dac_con1 = 0x73001803;
                rate_code = 9;
                pmic_newif_rate = 7;
                break;
        }

        /* Pause DL1 DMA during clock and PLL reconfiguration to prevent jitter/glitching */
        set_afereg(eac_fd, 0x10, 0, 1 << 1);
        usleep(5000);

        /* 1. DL1 sample rate in AFE_DAC_CON1 (0x14 bits 0..3) */
        bool rate_programmed = true;
        if (set_afereg(eac_fd, 0x14, rate_code, 0x0f) < 0)
            rate_programmed = false;

        /* 2. IRQ1 sample rate in AFE_IRQ_MCU_CON (0x3a0 bits 4..7) & enable (bit 0) */
        if (set_afereg(eac_fd, 0x3a0, (rate_code << 4) | 1, 0xf1) < 0)
            rate_programmed = false;

        /* 3. IRQ1 period count in AFE_IRQ_MCU_CNT1 (0x3ac) = 1024 samples */
        if (set_afereg(eac_fd, 0x3ac, 1024, ~0U) < 0)
            rate_programmed = false;

        /* 4. PMIC New IF DAC output config (0x108) */
        if (set_afereg(eac_fd, 0x108, dac_con1, ~0U) < 0)
            rate_programmed = false;

        /* 5. I2S Out format & sample rate (AFE_I2S_CON1 / 0x34).
         * Init uses 0x00000909: bits[11:8] = rate, bits[3:0] = 0x9
         * (bit 0 I2S enable, bit 3 16-bit). Putting rate_code in the low
         * nibble clears enable for even rates (48 kHz is 10), which muted
         * AUTO 48 kHz tracks. */
        if (set_afereg(eac_fd, 0x34, (rate_code << 8) | 0x09, ~0U) < 0)
            rate_programmed = false;

        /* 6. PMIC NewIF Downlink Frequency (0x4024) */
        if (set_anareg(eac_fd, 0x4024, (pmic_newif_rate << 12) | 0x330, 0xffff) < 0)
            rate_programmed = false;

        /* 7. PMIC Codec Downlink Sampling Rate (0x4002) */
        if (set_anareg(eac_fd, 0x4002, rate_code, 0x0f) < 0)
            rate_programmed = false;

        /* 8. Trigger PMIC TopCtl Clock Synchronization */
        top_ctl_trigger(eac_fd);
        usleep(5000);

        /* Resume DL1 DMA if playback was active */
        if (dma_stopped == 0 && dma_locked == 0)
        {
            set_afereg(eac_fd, 0x10, 1 << 1, 1 << 1);
        }
        if (rate_programmed)
            programmed_sampr = sampr;
        else
            log_audio_debug("[pcm-eac] Failed to program one or more sample-rate registers; will retry.\n");
    }
}

static void sink_dma_start(const void *addr, size_t size)
{
    pcm_data = addr;
    pcm_size = size;
    y1_set_deep_idle(false);

    pthread_mutex_lock(&pcm_mtx);
    dma_stopped = 0;
    pthread_mutex_unlock(&pcm_mtx);

    if (eac_fd >= 0)
    {
        y1_play_wake_lock(true);
        y1_audio_set_output_enabled(!y1_bt_a2dp_is_streaming());
        if (audio_output_enabled)
        {
            sink_set_freq((uint16_t)current_sampr);
            set_afereg(eac_fd, 0x10, (1 << 0) | (1 << 1),
                       (1 << 0) | (1 << 1));
        }
    }

    pthread_mutex_lock(&pcm_mtx);
    pthread_cond_signal(&pcm_cond);
    pthread_mutex_unlock(&pcm_mtx);

    pcm_play_dma_status_callback(PCM_DMAST_STARTED);
}

static void sink_dma_stop(void)
{
    pthread_mutex_lock(&pcm_mtx);
    dma_stopped = 1;
    pthread_mutex_unlock(&pcm_mtx);

    if (eac_fd >= 0)
    {
        y1_audio_set_output_enabled(false);
        y1_play_wake_lock(false);
    }
    y1_set_deep_idle(true);
}

void audiohw_close(void)
{
    if (eac_fd >= 0)
    {
        y1_audio_set_output_enabled(false);
        ioctl(eac_fd, FREE_MEMIF_FP, 0);
        ioctl(eac_fd, AUDDRV_FREE_DL1_BUF, 0);
        ioctl(eac_fd, FREE_MEMIF_BUF, 0);
        ioctl(eac_fd, AUD_DRV_AFE_CLK, 0);
        ioctl(eac_fd, AUD_DRV_AIF_CLK, 0);
        close(eac_fd);
        eac_fd = -1;
    }
}

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
        .volume_type  = PCM_NATIVE_VOLUME_TYPE,
    },
    .ops = {
        .init     = sink_dma_init,
        .postinit = sink_dma_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_dma_start,
        .stop     = sink_dma_stop,
    },
};
