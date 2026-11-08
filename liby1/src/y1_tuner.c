/***************************************************************************
 * liby1 - MediaTek MT6572 / MT6627 FM Radio Tuner Driver (/dev/fm)
 ****************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "liby1.h"

#define FM_IOCTL_POWERUP   _IOWR('R', 0, int)
#define FM_IOCTL_POWERDOWN _IOWR('R', 1, int)
#define FM_IOCTL_TUNE      _IOWR('R', 2, struct y1_fm_tune_parm)
#define FM_IOCTL_SEEK      _IOWR('R', 3, struct y1_fm_seek_parm)
#define FM_IOCTL_SETVOL    _IOWR('R', 4, uint32_t)
#define FM_IOCTL_GETVOL    _IOWR('R', 5, uint32_t *)
#define FM_IOCTL_MUTE      _IOWR('R', 6, uint32_t)
#define FM_IOCTL_GETRSSI   _IOWR('R', 7, int *)

struct y1_fm_tune_parm {
    uint8_t  band;
    uint16_t freq; /* Frequency in 100 kHz units: 875..1080 */
    uint8_t  space;
};

struct y1_fm_seek_parm {
    uint8_t  band;
    uint16_t freq;
    uint8_t  seekdir;
    uint8_t  space;
};

static int  fm_fd = -1;
static int  current_freq_hz = 87500000;
static bool tuner_powered = false;
static bool is_muted = false;

static void fm_open_dev(void)
{
    if (fm_fd >= 0)
        return;

    const char *devs[] = { "/dev/fm", "/dev/radio0", "/dev/stpfm", NULL };
    for (int i = 0; devs[i]; i++) {
        fm_fd = open(devs[i], O_RDWR | O_CLOEXEC);
        if (fm_fd >= 0) {
            y1_logf("[liby1-fm] Opened FM radio device: %s (fd=%d)\n", devs[i], fm_fd);
            break;
        }
    }
}

static void fm_close_dev(void)
{
    if (fm_fd >= 0) {
        close(fm_fd);
        fm_fd = -1;
    }
}

int y1_fm_power(bool on)
{
    if (on) {
        tuner_powered = true;
        fm_open_dev();
        if (fm_fd >= 0) {
            int pwr = 1;
            ioctl(fm_fd, FM_IOCTL_POWERUP, &pwr);
        }
        y1_fm_tune(current_freq_hz);
        y1_log("[liby1-fm] FM Tuner powered ON\n");
    } else {
        tuner_powered = false;
        if (fm_fd >= 0) {
            int pwr = 0;
            ioctl(fm_fd, FM_IOCTL_POWERDOWN, &pwr);
            fm_close_dev();
        }
        y1_log("[liby1-fm] FM Tuner powered OFF\n");
    }
    return 0;
}

int y1_fm_tune(int freq_hz)
{
    current_freq_hz = freq_hz;
    if (fm_fd >= 0 && tuner_powered) {
        struct y1_fm_tune_parm parm;
        memset(&parm, 0, sizeof(parm));
        parm.band = 1; /* 87.5 - 108.0 MHz */
        parm.freq = (uint16_t)(freq_hz / 100000);
        parm.space = 1; /* 100 kHz */
        ioctl(fm_fd, FM_IOCTL_TUNE, &parm);
    }
    return 0;
}

int y1_fm_get_freq(void)
{
    return current_freq_hz;
}

int y1_fm_set_mute(bool mute)
{
    is_muted = mute;
    if (fm_fd >= 0 && tuner_powered) {
        uint32_t m = is_muted ? 1 : 0;
        ioctl(fm_fd, FM_IOCTL_MUTE, m);
    }
    return 0;
}

bool y1_fm_is_muted(void)
{
    return is_muted;
}

int y1_fm_get_rssi(void)
{
    if (fm_fd >= 0 && tuner_powered) {
        int rssi = 0;
        if (ioctl(fm_fd, FM_IOCTL_GETRSSI, &rssi) == 0)
            return rssi;
    }
    return 75;
}

bool y1_fm_is_tuned(void)
{
    if (fm_fd >= 0 && tuner_powered) {
        int rssi = 0;
        if (ioctl(fm_fd, FM_IOCTL_GETRSSI, &rssi) == 0)
            return rssi > 15;
    }
    return (current_freq_hz >= 87500000 && current_freq_hz <= 108000000);
}

