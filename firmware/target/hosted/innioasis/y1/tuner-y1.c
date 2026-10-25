/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox Innioasis Y1 Port
 * MediaTek MT6572 / MT6627 FM Radio Tuner Driver
 ****************************************************************************/

#define RB_FILESYSTEM_OS
#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "kernel.h"
#include "tuner.h"
#include "fmradio.h"
#include "tuner-y1.h"
#include "y1-log.h"

/* Standard MediaTek MT6572/MT6627 FM Radio IOCTL interface */
#define FM_IOCTL_POWERUP       _IOWR('R', 0, int)
#define FM_IOCTL_POWERDOWN     _IOWR('R', 1, int)
#define FM_IOCTL_TUNE          _IOWR('R', 2, struct fm_tune_parm)
#define FM_IOCTL_SEEK          _IOWR('R', 3, struct fm_seek_parm)
#define FM_IOCTL_SETVOL        _IOWR('R', 4, uint32_t)
#define FM_IOCTL_GETVOL        _IOWR('R', 5, uint32_t *)
#define FM_IOCTL_MUTE          _IOWR('R', 6, uint32_t)
#define FM_IOCTL_GETRSSI       _IOWR('R', 7, int *)
#define FM_IOCTL_SCAN          _IOWR('R', 8, struct fm_scan_parm)

struct fm_tune_parm {
    uint8_t band;
    uint16_t freq; /* Frequency in 100 kHz units: 875..1080 */
    uint8_t space;
};

struct fm_seek_parm {
    uint8_t band;
    uint16_t freq;
    uint8_t seekdir;
    uint8_t space;
};

struct fm_scan_parm {
    uint8_t band;
    uint8_t space;
    uint8_t num;
    uint16_t sr_table[64];
};

static int fm_fd = -1;
static int current_freq = 87500000;
static bool tuner_sleeping = true;
static bool is_muted = false;
static bool force_mono = false;

static void y1_fm_open(void)
{
    if (fm_fd >= 0)
        return;

    const char *devs[] = { "/dev/fm", "/dev/radio0", "/dev/stpfm", NULL };
    for (int i = 0; devs[i]; i++) {
        fm_fd = open(devs[i], O_RDWR | O_CLOEXEC);
        if (fm_fd >= 0) {
            y1_logf("[tuner-y1] Opened FM radio device: %s (fd=%d)\n", devs[i], fm_fd);
            break;
        }
    }
}

static void y1_fm_close(void)
{
    if (fm_fd >= 0) {
        close(fm_fd);
        fm_fd = -1;
    }
}

void y1_tuner_init(void)
{
    current_freq = 87500000;
    tuner_sleeping = true;
    is_muted = false;
    force_mono = false;
}

int y1_tuner_set(int setting, int value)
{
    switch (setting)
    {
        case RADIO_SLEEP:
            tuner_sleeping = (value != 0);
            if (tuner_sleeping) {
                if (fm_fd >= 0) {
                    int pwr = 0;
                    ioctl(fm_fd, FM_IOCTL_POWERDOWN, &pwr);
                    y1_fm_close();
                }
                y1_log("[tuner-y1] FM Tuner powered OFF (sleep mode)\n");
            } else {
                y1_fm_open();
                if (fm_fd >= 0) {
                    int pwr = 1;
                    ioctl(fm_fd, FM_IOCTL_POWERUP, &pwr);
                }
                y1_tuner_set(RADIO_FREQUENCY, current_freq);
                y1_log("[tuner-y1] FM Tuner powered ON (active mode)\n");
            }
            break;

        case RADIO_FREQUENCY:
            current_freq = value;
            if (fm_fd >= 0 && !tuner_sleeping) {
                struct fm_tune_parm parm;
                memset(&parm, 0, sizeof(parm));
                parm.band = 1; /* 87.5 - 108.0 MHz */
                parm.freq = (uint16_t)(value / 100000);
                parm.space = 1; /* 100 kHz */
                ioctl(fm_fd, FM_IOCTL_TUNE, &parm);
            }
            break;

        case RADIO_SCAN_FREQUENCY:
            y1_tuner_set(RADIO_FREQUENCY, value);
            return y1_tuner_get(RADIO_TUNED);

        case RADIO_MUTE:
            is_muted = (value != 0);
            if (fm_fd >= 0 && !tuner_sleeping) {
                uint32_t m = is_muted ? 1 : 0;
                ioctl(fm_fd, FM_IOCTL_MUTE, m);
            }
            break;

        case RADIO_FORCE_MONO:
            force_mono = (value != 0);
            break;

        default:
            return -1;
    }
    return 0;
}

int y1_tuner_get(int setting)
{
    switch (setting)
    {
        case RADIO_PRESENT:
            return 1;

        case RADIO_TUNED:
            if (fm_fd >= 0 && !tuner_sleeping) {
                int rssi = 0;
                if (ioctl(fm_fd, FM_IOCTL_GETRSSI, &rssi) == 0)
                    return rssi > 15 ? 1 : 0;
            }
            /* Default valid FM frequency tune detection */
            return (current_freq >= 87500000 && current_freq <= 108000000) ? 1 : 0;

        case RADIO_STEREO:
            return force_mono ? 0 : 1;

        case RADIO_RSSI:
            if (fm_fd >= 0 && !tuner_sleeping) {
                int rssi = 0;
                if (ioctl(fm_fd, FM_IOCTL_GETRSSI, &rssi) == 0)
                    return rssi;
            }
            return 75;

        case RADIO_RSSI_MIN:
            return 0;

        case RADIO_RSSI_MAX:
            return 100;

        default:
            return -1;
    }
}

