/***************************************************************************
 * liby1 - MediaTek MT6572 / MT6627 FM Radio Tuner Driver (/dev/fm)
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "liby1.h"

/* MediaTek MT6572/MT6627 FM ioctl codes verified from stock mtk_fm_drv.ko */
#define FM_IOC_MAGIC          0xf5
#define FM_IOCTL_POWERUP      _IOWR(FM_IOC_MAGIC, 0x00, int)
#define FM_IOCTL_POWERDOWN    _IOWR(FM_IOC_MAGIC, 0x01, int)
#define FM_IOCTL_TUNE         _IOWR(FM_IOC_MAGIC, 0x02, int)
#define FM_IOCTL_SEEK         _IOWR(FM_IOC_MAGIC, 0x03, int)
#define FM_IOCTL_SETVOL       _IOWR(FM_IOC_MAGIC, 0x04, int)
#define FM_IOCTL_GETVOL       _IOWR(FM_IOC_MAGIC, 0x05, int)
#define FM_IOCTL_MUTE         _IOWR(FM_IOC_MAGIC, 0x06, int)
#define FM_IOCTL_GETRSSI      _IOWR(FM_IOC_MAGIC, 0x07, int)
#define FM_IOCTL_GETMONOSTERO _IOWR(FM_IOC_MAGIC, 0x0d, int)
#define FM_IOCTL_SETMONOSTERO _IOWR(FM_IOC_MAGIC, 0x0e, int)
#define FM_IOCTL_ANA_SWITCH   _IOWR(FM_IOC_MAGIC, 0x1e, int)
#define FM_IOCTL_IS_PWRUP     _IOWR(FM_IOC_MAGIC, 0x20, int)
#define FM_IOCTL_GET_I2S_INFO _IOWR(FM_IOC_MAGIC, 0x21, int)

struct __attribute__((packed)) y1_fm_tune_parm {
    uint8_t  err;
    uint8_t  band;  /* 1 = 87.5 - 108.0 MHz */
    uint8_t  space; /* 1 = 100 kHz */
    uint8_t  hilo;  /* 0 = auto */
    uint16_t freq;  /* Frequency in 100 kHz units: 875..1080 */
};

struct __attribute__((packed)) y1_fm_seek_parm {
    uint8_t  err;
    uint8_t  band;
    uint8_t  space;
    uint8_t  hilo;
    uint16_t freq;
    uint8_t  seekdir; /* 0 = up, 1 = down */
    uint8_t  seekth;
};

struct y1_fm_i2s_info {
    int status;
    int mode;
    int rate;
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
        fm_open_dev();
        tuner_powered = true;
        if (fm_fd >= 0) {
            struct y1_fm_tune_parm parm;
            memset(&parm, 0, sizeof(parm));
            parm.band  = 1;
            parm.space = 1;
            parm.hilo  = 0;
            parm.freq  = (uint16_t)(current_freq_hz / 100000);
            int pwrup_ret = ioctl(fm_fd, FM_IOCTL_POWERUP, &parm);
            if (pwrup_ret < 0) {
                y1_logf("[liby1-fm] FM_IOCTL_POWERUP failed (ret=%d, errno=%d: %s, err=%u), retrying...\n",
                        pwrup_ret, errno, strerror(errno), (unsigned)parm.err);
                usleep(150000);
                memset(&parm, 0, sizeof(parm));
                parm.band  = 1;
                parm.space = 1;
                parm.hilo  = 0;
                parm.freq  = (uint16_t)(current_freq_hz / 100000);
                pwrup_ret = ioctl(fm_fd, FM_IOCTL_POWERUP, &parm);
            }
            if (pwrup_ret == 0) {
                int ana = 0; /* 0 = long/headphone antenna */
                int ana_ret = ioctl(fm_fd, FM_IOCTL_ANA_SWITCH, &ana);
                uint32_t vol = 15;
                int vol_ret = ioctl(fm_fd, FM_IOCTL_SETVOL, &vol);
                struct y1_fm_i2s_info i2s = { 0, 1, 1 };
                int i2s_ret = ioctl(fm_fd, FM_IOCTL_GET_I2S_INFO, &i2s);
                y1_logf("[liby1-fm] POWERUP OK (freq=%u, err=%u, ana=%d, vol=%d, i2s=%d)\n",
                        (unsigned)parm.freq, (unsigned)parm.err, ana_ret, vol_ret, i2s_ret);
            } else {
                y1_logf("[liby1-fm] FM_IOCTL_POWERUP FAILED (ret=%d, errno=%d: %s, err=%u)\n",
                        pwrup_ret, errno, strerror(errno), (unsigned)parm.err);
            }
        }
        y1_audio_set_fm_route(true);
        y1_fm_set_mute(is_muted);
        y1_log("[liby1-fm] FM Tuner powered ON\n");
    } else {
        tuner_powered = false;
        y1_audio_set_fm_route(false);
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
        parm.band  = 1;
        parm.space = 1;
        parm.hilo  = 0;
        parm.freq  = (uint16_t)(freq_hz / 100000);
        int ret = ioctl(fm_fd, FM_IOCTL_TUNE, &parm);
        if (ret == 0 && parm.freq > 0)
            current_freq_hz = (int)parm.freq * 100000;
        else
            y1_logf("[liby1-fm] TUNE(%d) failed (ret=%d, errno=%d: %s, err=%u)\n",
                    freq_hz / 100000, ret, errno, strerror(errno), (unsigned)parm.err);
    }
    return 0;
}

int y1_fm_seek(int freq_hz, bool up)
{
    if (fm_fd < 0 || !tuner_powered)
        return -1;

    struct y1_fm_seek_parm parm;
    memset(&parm, 0, sizeof(parm));
    parm.band    = 1;
    parm.space   = 1;
    parm.hilo    = 0;
    parm.freq    = (uint16_t)(freq_hz / 100000);
    parm.seekdir = up ? 0 : 1;
    parm.seekth  = 0;
    int ret = ioctl(fm_fd, FM_IOCTL_SEEK, &parm);
    if (ret == 0 && parm.freq > 0) {
        current_freq_hz = (int)parm.freq * 100000;
        y1_logf("[liby1-fm] SEEK(%s from %d) -> %d\n", up ? "up" : "down", freq_hz / 100000, (int)parm.freq);
        return current_freq_hz;
    }
    y1_logf("[liby1-fm] SEEK(%s from %d) failed (ret=%d, errno=%d: %s, err=%u, freq=%u)\n",
            up ? "up" : "down", freq_hz / 100000, ret, errno, strerror(errno),
            (unsigned)parm.err, (unsigned)parm.freq);
    return -1;
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
        ioctl(fm_fd, FM_IOCTL_MUTE, &m);
    }
    if (tuner_powered)
        y1_audio_set_fm_route(!is_muted);
    return 0;
}

bool y1_fm_is_muted(void)
{
    return is_muted;
}

int y1_fm_get_rssi(void)
{
    int rssi = 50;
    if (fm_fd >= 0 && tuner_powered) {
        if (ioctl(fm_fd, FM_IOCTL_GETRSSI, &rssi) == 0) {
            if (rssi < 0) rssi = 0;
            if (rssi > 100) rssi = 100;
        }
    }
    return rssi;
}

bool y1_fm_is_tuned(void)
{
    if (!tuner_powered)
        return false;
    return y1_fm_get_rssi() >= 15;
}

int y1_fm_set_mono(bool mono)
{
    if (fm_fd >= 0 && tuner_powered) {
        int val = mono ? 1 : 0;
        ioctl(fm_fd, FM_IOCTL_SETMONOSTERO, &val);
    }
    return 0;
}

bool y1_fm_is_stereo(void)
{
    int val = 0;
    if (fm_fd >= 0 && tuner_powered) {
        if (ioctl(fm_fd, FM_IOCTL_GETMONOSTERO, &val) == 0)
            return val == 0; /* 0 = stereo, 1 = mono in MTK fm_drv */
    }
    return false;
}
