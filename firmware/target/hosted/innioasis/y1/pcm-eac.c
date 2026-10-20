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
#include "pcm_mixer.h"
#include "pcm_sink.h"
#include "pcm_sw_volume.h"
#include "audiohw.h"
#include "bt-y1.h"

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
static int pending_vol_l = 0;
static int pending_vol_r = 0;

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

static inline void set_afereg(int fd, uint32_t offset, uint32_t val, uint32_t mask)
{
    struct Register_Control reg = { offset, val, mask };
    ioctl(fd, AUDDRV_SET_AFEREG, &reg);
}

static inline void set_anareg(int fd, uint32_t offset, uint32_t val, uint32_t mask)
{
    struct Register_Control reg = { offset, val, mask };
    ioctl(fd, AUDDRV_SET_ANAREG, &reg);
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
    /* 1. Write to stderr (redirected to /mnt/sdcard/boot_log.txt) */
    fputs(msg, stderr);

    /* 2. Write to persistent audio_debug.txt */
    int dfd = open("/mnt/sdcard/audio_debug.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (dfd >= 0) {
        (void)!write(dfd, msg, strlen(msg));
        close(dfd);
    }

    /* 3. Write to /dev/kmsg */
    int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0) {
        (void)!write(kfd, "<6>[pcm-eac] ", 13);
        (void)!write(kfd, msg, strlen(msg));
        close(kfd);
    }
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

void audiohw_set_volume(int vol_l, int vol_r)
{
    /* vol_l / vol_r are centibels (-740 to +60), 1 dB UI steps. Analog HP
     * PGA is the coarse control; PCM only supplies the 1 dB remainder. */
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
static pthread_mutex_t pcm_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcm_cond = PTHREAD_COND_INITIALIZER;

static volatile sig_atomic_t dma_stopped = 1;
static volatile sig_atomic_t dma_locked  = 0;
static volatile bool pcm_thread_exit = false;

static const void *pcm_data = NULL;
static size_t pcm_size = 0;

static void *pcm_worker(void *arg)
{
    (void)arg;
    static const char zero_buf[4096] = { 0 };

    while (!pcm_thread_exit)
    {
        pthread_mutex_lock(&pcm_mtx);
        while ((dma_stopped == 1 || dma_locked == 1) && !pcm_thread_exit)
        {
            pthread_cond_wait(&pcm_cond, &pcm_mtx);
        }
        pthread_mutex_unlock(&pcm_mtx);

        if (pcm_thread_exit)
            break;

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
                ssize_t written = write(eac_fd, bt_active ? zero_buf : buf, chunk);
                if (written < 0 && errno != EAGAIN && errno != EINTR) {
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
                    if (bt_active) {
                        y1_bt_a2dp_write_pcm(buf, (size_t)written, current_sampr);
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
            log_audio_debug("[pcm-eac] MT6323 PMIC Digital Audio Codec & Headphone Amp fully configured.\n");
        }
    }

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
    if (sampr == current_sampr)
    {
        return; /* Frequency hasn't changed; avoid re-syncing clocks between tracks */
    }
    current_sampr = sampr;

    if (eac_fd >= 0)
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
        set_afereg(eac_fd, 0x14, rate_code, 0x0f);

        /* 2. IRQ1 sample rate in AFE_IRQ_MCU_CON (0x3a0 bits 4..7) & enable (bit 0) */
        set_afereg(eac_fd, 0x3a0, (rate_code << 4) | 1, 0xf1);

        /* 3. IRQ1 period count in AFE_IRQ_MCU_CNT1 (0x3ac) = 1024 samples */
        set_afereg(eac_fd, 0x3ac, 1024, ~0U);

        /* 4. PMIC New IF DAC output config (0x108) */
        set_afereg(eac_fd, 0x108, dac_con1, ~0U);

        /* 5. I2S Out format & sample rate (0x34) */
        set_afereg(eac_fd, 0x34, (rate_code << 8) | rate_code, ~0U);

        /* 6. PMIC NewIF Downlink Frequency (0x4024) */
        set_anareg(eac_fd, 0x4024, (pmic_newif_rate << 12) | 0x330, 0xffff);

        /* 7. PMIC Codec Downlink Sampling Rate (0x4002) */
        set_anareg(eac_fd, 0x4002, rate_code, 0x0f);

        /* 8. Trigger PMIC TopCtl Clock Synchronization */
        top_ctl_trigger(eac_fd);
        usleep(5000);

        /* Resume DL1 DMA if playback was active */
        if (dma_stopped == 0 && dma_locked == 0)
        {
            set_afereg(eac_fd, 0x10, 1 << 1, 1 << 1);
        }
    }
}

static void sink_dma_start(const void *addr, size_t size)
{
    pcm_data = addr;
    pcm_size = size;

    if (eac_fd >= 0)
    {
        /* Ensure AFE Master Switch and DL1 DMA are active */
        set_afereg(eac_fd, 0x10, (1 << 0) | (1 << 1), (1 << 0) | (1 << 1));
    }

    pthread_mutex_lock(&pcm_mtx);
    dma_stopped = 0;
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
        /* Pause DL1 DMA transfer (clear bit 1 of 0x10) while keeping AFE on */
        set_afereg(eac_fd, 0x10, 0, 1 << 1);
    }
}

void audiohw_close(void)
{
    if (eac_fd >= 0)
    {
        ioctl(eac_fd, SET_HEADPHONE_OFF, 0);
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

