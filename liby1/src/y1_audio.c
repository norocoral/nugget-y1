/***************************************************************************
 * liby1 - MT6572 AFE (/dev/eac) & MT6323 PMIC DAC / Headphone Amp / ADC
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <poll.h>

#ifndef makedev
#define makedev(maj, min) (((maj) << 8) | (min))
#endif

#include "liby1.h"

#define SET_SPEAKER_OFF          0x400443a2
#define SET_HEADPHONE_ON         0x400443a4
#define SET_HEADPHONE_OFF        0x400443a5
#define SET_EARPIECE_OFF         0x400443a7
#define AUDDRV_ALLOCATE_DL1_BUF  0xc0044310
#define AUDDRV_FREE_DL1_BUF      0xc0044311
#define AUDDRV_ALLOCATE_VUL_BUF  0xc0044314
#define AUDDRV_FREE_VUL_BUF      0xc0044315
#define AUDDRV_ALLOCATE_AWB_BUF  0xc004431a
#define AUDDRV_FREE_AWB_BUF      0xc004431b
#define FREE_MEMIF_BUF           0xc004431f
#define SET_MEMIF_FP             0xc0044320
#define FREE_MEMIF_FP            0xc0044321
#define AUDDRV_SET_AFEREG        0xc0044300
#define AUDDRV_SET_ANAREG        0xc0044302
#define AUDDRV_GET_ANAREG        0xc0044303
#define AUD_DRV_AIF_CLK          0xc0044351
#define AUD_DRV_AFE_CLK          0xc0044355
#define AUD_DRV_FM_I2S_OFF       0xc0044360
#define AUD_DRV_FM_I2S_ON        0xc0044362

#define MEM_TYPE_VUL             5
#define MEM_TYPE_AWB             6

#define MT6323_AUDTOP_CON5       0x070a

struct y1_reg_ctl {
    uint32_t offset;
    uint32_t value;
    uint32_t mask;
};

static int eac_fd = -1;
static unsigned long current_sampr = 44100;
static unsigned long programmed_sampr = 0;
static int current_hp_idx_l = -1;
static int current_hp_idx_r = -1;
static int saved_hp_idx_l = 7;
static int saved_hp_idx_r = 7;
static bool dma_is_running = false;
static bool deep_idle_enabled = true;
static bool cpu_powerdown_enabled = true;
static int wakelock_fd = -1;

static inline int set_afereg(int fd, uint32_t offset, uint32_t val, uint32_t mask)
{
    struct y1_reg_ctl reg = { offset, val, mask };
    return ioctl(fd, AUDDRV_SET_AFEREG, &reg);
}

static inline int set_anareg(int fd, uint32_t offset, uint32_t val, uint32_t mask)
{
    struct y1_reg_ctl reg = { offset, val, mask };
    return ioctl(fd, AUDDRV_SET_ANAREG, &reg);
}

static inline uint32_t get_anareg(int fd, uint32_t offset)
{
    struct y1_reg_ctl reg = { offset, 0, ~0U };
    if (ioctl(fd, AUDDRV_GET_ANAREG, &reg) == 0)
        return reg.value;
    return 0;
}

static inline void top_ctl_trigger(int fd)
{
    uint32_t val = get_anareg(fd, 0x4016);
    uint32_t toggle = (val ^ 0x300) & 0x300;
    set_anareg(fd, 0x4016, toggle, 0x300);
}

static void audio_clocks(int on)
{
    if (eac_fd < 0)
        return;
    ioctl(eac_fd, AUD_DRV_AFE_CLK, on);
    ioctl(eac_fd, AUD_DRV_AIF_CLK, on);
}

static void ensure_eac_node(void)
{
    int maj = 10, min = -1;
    FILE *fp = fopen("/sys/class/misc/eac/dev", "r");
    if (fp) {
        if (fscanf(fp, "%d:%d", &maj, &min) == 2 && min >= 0) {
            struct stat st;
            if (stat("/dev/eac", &st) != 0 || !S_ISCHR(st.st_mode) || st.st_rdev != (dev_t)makedev(maj, min)) {
                y1_logf("[liby1-audio] Discovered /sys/class/misc/eac/dev: major %d, minor %d\n", maj, min);
                unlink("/dev/eac");
                mknod("/dev/eac", S_IFCHR | 0666, makedev(maj, min));
            }
        }
        fclose(fp);
    }
}

static void calc_rate_params(unsigned long sampr, uint32_t *dac_con1, uint32_t *rate_code, uint32_t *pmic_newif_rate)
{
    switch (sampr) {
    case 48000:
        *dac_con1 = 0x83001803;
        *rate_code = 10;
        *pmic_newif_rate = 8;
        break;
    case 32000:
        *dac_con1 = 0x63001803;
        *rate_code = 8;
        *pmic_newif_rate = 6;
        break;
    case 24000:
        *dac_con1 = 0x53001803;
        *rate_code = 6;
        *pmic_newif_rate = 5;
        break;
    case 22050:
        *dac_con1 = 0x43001803;
        *rate_code = 5;
        *pmic_newif_rate = 4;
        break;
    case 16000:
        *dac_con1 = 0x33001823;
        *rate_code = 4;
        *pmic_newif_rate = 3;
        break;
    case 12000:
        *dac_con1 = 0x23001803;
        *rate_code = 2;
        *pmic_newif_rate = 2;
        break;
    case 11025:
        *dac_con1 = 0x13001803;
        *rate_code = 1;
        *pmic_newif_rate = 1;
        break;
    case 8000:
        *dac_con1 = 0x03001823;
        *rate_code = 0;
        *pmic_newif_rate = 0;
        break;
    case 44100:
    default:
        *dac_con1 = 0x73001803;
        *rate_code = 9;
        *pmic_newif_rate = 7;
        break;
    }
}

int y1_pmic_read_reg(uint32_t offset)
{
    if (eac_fd < 0)
        return -1;
    struct y1_reg_ctl reg = { offset, 0, ~0U };
    if (ioctl(eac_fd, AUDDRV_GET_ANAREG, &reg) == 0)
        return (int)reg.value;
    return -1;
}

int y1_pmic_write_reg(uint32_t offset, uint32_t val, uint32_t mask)
{
    if (eac_fd < 0)
        return -1;
    return set_anareg(eac_fd, offset, val, mask);
}

int y1_afe_write_reg(uint32_t offset, uint32_t val, uint32_t mask)
{
    if (eac_fd < 0)
        return -1;
    return set_afereg(eac_fd, offset, val, mask);
}

void y1_pmic_tune_accdet_debounce(void)
{
    static int accdet_fd = -1;
    if (accdet_fd < 0) {
        accdet_fd = open("/dev/accdet", O_RDWR);
        if (accdet_fd < 0) {
            int dfd = open("/sys/class/misc/accdet/dev", O_RDONLY);
            if (dfd >= 0) {
                char buf[32];
                ssize_t n = read(dfd, buf, sizeof(buf) - 1);
                close(dfd);
                if (n > 0) {
                    buf[n] = '\0';
                    unsigned int maj = 0, min = 0;
                    if (sscanf(buf, "%u:%u", &maj, &min) == 2 && maj > 0) {
                        dev_t dev = (dev_t)(((maj & 0xfffU) << 8) | (min & 0xffU));
                        mknod("/dev/accdet", S_IFCHR | 0666, dev);
                        accdet_fd = open("/dev/accdet", O_RDWR);
                    }
                }
            }
        }
    }
    if (accdet_fd >= 0) {
        /* ACCDET_IOC_SET_CALL_STATE (0x4101) = 1 keeps MICBIAS and ACCDET
         * polling active past 5s idle and across screen-off/suspend when a
         * 4-pole mic headset is inserted; clear it to 0 otherwise to avoid
         * unnecessary MICBIAS drain in deep sleep. */
        ioctl(accdet_fd, 0x4101, y1_jack_has_mic() ? 1 : 0);
    }

    int fd = eac_fd;
    bool temp_fd = false;
    if (fd < 0) {
        fd = open("/dev/eac", O_RDWR);
        temp_fd = (fd >= 0);
    }
    if (fd >= 0) {
        set_anareg(fd, 0x010c, 0x0200, 0x0200); /* Clear RG_ACCDET_CK_PDN */
        set_anareg(fd, 0x077e, 0x0007, 0x0007); /* Enable ACCDET PWM & MICBIAS */
        set_anareg(fd, 0x077c, 0x0001, 0x0001); /* Enable ACCDET module */
        set_anareg(fd, 0x0786, 0x0180, 0xffff); /* Fast hook-key debounce0 (~12ms) */
        set_anareg(fd, 0x0788, 0x0180, 0xffff); /* Fast hook-key debounce1 (~12ms) */
        if (temp_fd)
            close(fd);
    }
}

bool y1_audio_is_open(void)
{
    return eac_fd >= 0;
}

int y1_audio_get_fd(void)
{
    return eac_fd;
}

int y1_audio_open(unsigned long sample_rate)
{
    if (sample_rate == 0)
        sample_rate = current_sampr ? current_sampr : 44100;
    current_sampr = sample_rate;

    if (eac_fd >= 0)
        return 0;

    ensure_eac_node();
    eac_fd = open("/dev/eac", O_RDWR | O_CLOEXEC);
    if (eac_fd < 0) {
        y1_logf("[liby1-audio] ERROR: open(/dev/eac) failed: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }
    y1_logf("[liby1-audio] open(/dev/eac) SUCCESS (fd=%d)\n", eac_fd);

    /* 1. Turn on Audio Clocks and allocate coherent DL1 DMA ring buffer */
    audio_clocks(1);
    ioctl(eac_fd, FREE_MEMIF_FP, 0);
    ioctl(eac_fd, AUDDRV_FREE_DL1_BUF, 0);
    ioctl(eac_fd, FREE_MEMIF_BUF, 0);

    int ret_alloc = ioctl(eac_fd, AUDDRV_ALLOCATE_DL1_BUF, 65536);
    y1_logf("[liby1-audio] AUDDRV_ALLOCATE_DL1_BUF(65536) returned %d (errno=%d %s)\n",
            ret_alloc, errno, ret_alloc < 0 ? strerror(errno) : "OK");

    int ret_fp = ioctl(eac_fd, SET_MEMIF_FP, 0);
    y1_logf("[liby1-audio] SET_MEMIF_FP(0) returned %d (errno=%d %s)\n",
            ret_fp, errno, ret_fp < 0 ? strerror(errno) : "OK");

    /* 2. Set up AFE Digital Interconnections & mute unused endpoints */
    set_afereg(eac_fd, 0x24, 1 << 21, 1 << 21); /* DL1 Left -> I2S Out Left (CONN1 bit 21) */
    set_afereg(eac_fd, 0x28, 1 << 6,  1 << 6);  /* DL1 Right -> I2S Out Right (CONN2 bit 6) */
    set_afereg(eac_fd, 0x10c, 0xf74f0000, ~0U); /* AFE_PMIC_NEWIF_CFG1 */
    set_afereg(eac_fd, 0x124, 1, 1);            /* AFE_PMIC_NEWIF_CFG2: Enable I2S DAC */
    set_afereg(eac_fd, 0x4c4, 0, 0x10);         /* Clear bit 4 of 0x4c4 */
    ioctl(eac_fd, SET_SPEAKER_OFF, 0);
    ioctl(eac_fd, SET_EARPIECE_OFF, 0);
    programmed_sampr = 0;

    /* 3. Power up MT6323 PMIC Audio Clocks & AFE Master Switch BEFORE sample rate & DAC power-up */
    set_anareg(eac_fd, 0x106, 1, 0xffff);       /* PMIC TOP_CKPDN_CON0: Power on audio clock */
    set_anareg(eac_fd, 0x10c, 0x100, 0x100);    /* PMIC TOP_CKG_CON1 bit 8: Audio 13M clock gating */
    set_afereg(eac_fd, 0x10, 1, 1);             /* Enable AFE_ON (bit 0) */
    audio_clocks(1);

    /* 4. Program hardware sample rate & wake PMIC digital clock domain (0x4014, top_ctl_trigger, 0x4000) */
    y1_audio_set_sample_rate(current_sampr);

    /* Keep PMIC digital downlink muted while analog DAC & HP amp power up */
    set_anareg(eac_fd, 0x4006, 0xffff, 0xffff);
    set_anareg(eac_fd, 0x4008, 0xffff, 0xffff);

    /* 5. Power up Downlink DAC & analog L/R paths with clean unity preamp gain (2 instead of 5) */
    set_anareg(eac_fd, 0x700, 0x7800, 0x7f80);
    set_anareg(eac_fd, 0x704, 0x00f0, 0x00ff);
    set_anareg(eac_fd, 0x702, 0x0200, 0x0700); /* Clean unity preamp buffer gain Right (2 << 8) */
    set_anareg(eac_fd, 0x700, 0x0020, 0x0070); /* Clean unity preamp buffer gain Left (2 << 4) */
    set_anareg(eac_fd, 0x710, 0x0008, 0x0008);
    set_anareg(eac_fd, 0x700, 0x0180, 0x0180);
    set_anareg(eac_fd, 0x704, 0x0003, 0x0003);

    /* 6. Configure MT6323 PMIC Headphone Amplifier with pop suppression and target PGA gain */
    uint32_t init_con5_gain = ((uint32_t)(saved_hp_idx_l & 0x7) << 12) |
                              ((uint32_t)(saved_hp_idx_r & 0x7) << 8);
    current_hp_idx_l = saved_hp_idx_l;
    current_hp_idx_r = saved_hp_idx_r;
    set_anareg(eac_fd, 0x70c, 0xf7f2, 0xffff);
    set_anareg(eac_fd, 0x700, 0x7000, 0xf000);
    set_anareg(eac_fd, 0x70a, 0x0014 | init_con5_gain, 0xffff);
    set_anareg(eac_fd, 0x708, 0x007c, 0xffff);
    usleep(12000); /* 12ms depop settling time */
    set_anareg(eac_fd, 0x70c, 0xf5ba, 0xffff);

    /* 7. Prefill DMA ring buffer with silence before enabling output */
    {
        static const uint8_t zero_buf[4096] = { 0 };
        (void)write(eac_fd, zero_buf, sizeof(zero_buf));
    }

    /* 8. Finalize PMIC CDC Clock Synchronization, Master Enable, and Headphone Amp Unmute */
    set_anareg(eac_fd, 0x4014, 1, 1);
    top_ctl_trigger(eac_fd);
    set_anareg(eac_fd, 0x4000, 1, 1);
    ioctl(eac_fd, SET_HEADPHONE_ON, 3);
    usleep(2000);

    /* Unmute PMIC digital downlink gain (0 dB unity) after analog & clocks settle */
    set_anareg(eac_fd, 0x4006, 0, 0xffff);
    set_anareg(eac_fd, 0x4008, 0, 0xffff);

    if (dma_is_running) {
        set_afereg(eac_fd, 0x10, (1 << 0) | (1 << 1), (1 << 0) | (1 << 1));
    }

    y1_log("[liby1-audio] MT6323 PMIC Digital Audio Codec & Headphone Amp fully configured.\n");
    return 0;
}

void y1_audio_close(void)
{
    if (eac_fd < 0)
        return;

    /* Mute PMIC digital downlink and ramp down headphone amp with depop before power-off */
    set_anareg(eac_fd, 0x4006, 0xffff, 0xffff);
    set_anareg(eac_fd, 0x4008, 0xffff, 0xffff);
    usleep(2000);
    set_afereg(eac_fd, 0x10, 0, 1 << 1);
    set_anareg(eac_fd, 0x70c, 0xf7f2, 0xffff);
    usleep(5000);
    ioctl(eac_fd, SET_HEADPHONE_OFF, 0);
    set_anareg(eac_fd, 0x4014, 0, 1);
    set_anareg(eac_fd, 0x4000, 0, 1);
    top_ctl_trigger(eac_fd);

    /* Power down MT6323 analog headphone amp, charge pump, preamps, and DACs */
    set_anareg(eac_fd, 0x70a, 0x0000, 0xffff);
    set_anareg(eac_fd, 0x708, 0x0000, 0xffff);
    set_anareg(eac_fd, 0x70c, 0x0000, 0xffff);
    set_anareg(eac_fd, 0x704, 0x0000, 0x00ff);
    set_anareg(eac_fd, 0x702, 0x0000, 0x0700);
    set_anareg(eac_fd, 0x700, 0x0000, 0xfffa);
    set_anareg(eac_fd, 0x710, 0x0000, 0x0008);

    /* Keep PMIC ACCDET clock active for headset MFB while screen/audio is off */
    y1_pmic_tune_accdet_debounce();
    set_afereg(eac_fd, 0x10, 0, 1 << 0);

    /* Release DL1 DMA buffer and close /dev/eac so AudDrv_release() drops kernel audio wakelocks */
    ioctl(eac_fd, FREE_MEMIF_FP, 0);
    ioctl(eac_fd, AUDDRV_FREE_DL1_BUF, 0);
    ioctl(eac_fd, FREE_MEMIF_BUF, 0);
    audio_clocks(0);
    close(eac_fd);
    eac_fd = -1;

    programmed_sampr = 0;
    current_hp_idx_l = -1;
    current_hp_idx_r = -1;
    dma_is_running = false;
}

bool y1_audio_set_sample_rate(unsigned long sampr)
{
    current_sampr = sampr;
    if (eac_fd < 0)
        return true;
    if (sampr == programmed_sampr)
        return true;

    uint32_t dac_con1;
    uint32_t rate_code;
    uint32_t pmic_newif_rate;
    calc_rate_params(sampr, &dac_con1, &rate_code, &pmic_newif_rate);

    y1_logf("[liby1-audio] program_hw_sampr: %lu Hz\n", sampr);

    /* 1. Soft-mute PMIC digital downlink and pause AFE DL1 DMA, SRC2, and I2S Out */
    set_anareg(eac_fd, 0x4006, 0xffff, 0xffff);
    set_anareg(eac_fd, 0x4008, 0xffff, 0xffff);
    usleep(1500);
    set_afereg(eac_fd, 0x10, 0, 1 << 1);
    set_afereg(eac_fd, 0x108, 0, ~0U);
    set_afereg(eac_fd, 0x34, 0, 1);

    /* 2. Gate PMIC digital audio paths and latch via top_ctl_trigger() BEFORE
     * reconfiguring clock dividers. MT6323 internal digital FIR interpolation
     * filters corrupt their history state and lose I2S phase sync if sample
     * rate dividers change before the disable is latched into the PMIC clock domain. */
    set_anareg(eac_fd, 0x4014, 0, 1);  /* Disable PMIC downlink channel */
    set_anareg(eac_fd, 0x4000, 0, 1);  /* Disable PMIC Audio Digital Master */
    top_ctl_trigger(eac_fd);
    usleep(2000);

    /* 3. Reset DL1 DMA ring buffer pointers to byte 0 and clear residual samples
     * without freeing/shrinking the 65536-byte kernel DMA buffer. */
    ioctl(eac_fd, FREE_MEMIF_FP, 0);
    ioctl(eac_fd, SET_MEMIF_FP, 0);
    {
        static const uint8_t zero_buf[4096] = { 0 };
        (void)write(eac_fd, zero_buf, sizeof(zero_buf));
    }

    /* 4. Configure AFE digital interconnections: DL1 -> I2S Out */
    set_afereg(eac_fd, 0x24, 1 << 21, 1 << 21);
    set_afereg(eac_fd, 0x28, 1 << 6,  1 << 6);

    bool ok = true;
    if (set_afereg(eac_fd, 0x14, rate_code, 0x0f) < 0) ok = false;
    if (set_afereg(eac_fd, 0x3a0, (rate_code << 4) | 1, 0xf1) < 0) ok = false;
    if (set_afereg(eac_fd, 0x3ac, 1024, ~0U) < 0) ok = false;
    if (set_anareg(eac_fd, 0x4024, (pmic_newif_rate << 12) | 0x330, 0xffff) < 0) ok = false;
    if (set_anareg(eac_fd, 0x4002, rate_code, 0x0f) < 0) ok = false;
    if (set_afereg(eac_fd, 0x34, (rate_code << 8) | 0x09, ~0U) < 0) ok = false;
    if (set_afereg(eac_fd, 0x108, dac_con1, ~0U) < 0) ok = false;

    set_anareg(eac_fd, 0x4014, 1, 1);
    top_ctl_trigger(eac_fd);
    set_anareg(eac_fd, 0x4000, 1, 1);

    usleep(3000);

    /* Resume DL1 DMA if active, then unmute PMIC digital downlink */
    if (dma_is_running) {
        set_afereg(eac_fd, 0x10, (1 << 0) | (1 << 1), (1 << 0) | (1 << 1));
    }
    set_anareg(eac_fd, 0x4006, 0, 0xffff);
    set_anareg(eac_fd, 0x4008, 0, 0xffff);

    if (ok)
        programmed_sampr = sampr;
    return ok;
}

unsigned long y1_audio_get_sample_rate(void)
{
    return current_sampr;
}

void y1_audio_set_hp_analog_gain(int idx_l, int idx_r)
{
    if (idx_l < 0) idx_l = 0;
    if (idx_l > 7) idx_l = 7;
    if (idx_r < 0) idx_r = 0;
    if (idx_r > 7) idx_r = 7;

    saved_hp_idx_l = idx_l;
    saved_hp_idx_r = idx_r;

    if (eac_fd < 0)
        return;

    if (idx_l == current_hp_idx_l && idx_r == current_hp_idx_r)
        return;

    if (current_hp_idx_l < 0 || current_hp_idx_r < 0) {
        current_hp_idx_l = idx_l;
        current_hp_idx_r = idx_r;
        uint32_t con5_gain = ((uint32_t)(idx_l & 0x7) << 12) |
                             ((uint32_t)(idx_r & 0x7) << 8);
        set_anareg(eac_fd, MT6323_AUDTOP_CON5, con5_gain, 0x7700);
        return;
    }

    /* Step 1 PGA index at a time to avoid analog DC step pops.
     * Bits [14:12] = HPL Gain (0..7), Bits [10:8] = HPR Gain (0..7).
     * Mask 0x7700 preserves HP mux/power bits [4:0] (0x0014) without pop. */
    while (current_hp_idx_l != idx_l || current_hp_idx_r != idx_r) {
        if (current_hp_idx_l < idx_l) current_hp_idx_l++;
        else if (current_hp_idx_l > idx_l) current_hp_idx_l--;
        if (current_hp_idx_r < idx_r) current_hp_idx_r++;
        else if (current_hp_idx_r > idx_r) current_hp_idx_r--;

        uint32_t con5_gain = ((uint32_t)(current_hp_idx_l & 0x7) << 12) |
                             ((uint32_t)(current_hp_idx_r & 0x7) << 8);
        set_anareg(eac_fd, MT6323_AUDTOP_CON5, con5_gain, 0x7700);
        if (current_hp_idx_l != idx_l || current_hp_idx_r != idx_r)
            usleep(500);
    }
}

void y1_audio_set_dma_running(bool running)
{
    dma_is_running = running;
    if (eac_fd < 0)
        return;
    if (running)
        set_afereg(eac_fd, 0x10, (1 << 0) | (1 << 1), (1 << 0) | (1 << 1));
    else
        set_afereg(eac_fd, 0x10, 0, 1 << 1);
}

ssize_t y1_audio_write_pcm(const void *pcm_stereo_s16, size_t bytes)
{
    if (eac_fd < 0 || !pcm_stereo_s16 || bytes == 0)
        return -1;

    ssize_t written = write(eac_fd, pcm_stereo_s16, bytes);
    if (written > 0) {
        size_t unaligned = (size_t)written & 3;
        if (unaligned != 0) {
            static const uint8_t pad[4] = {0, 0, 0, 0};
            size_t pad_len = 4 - unaligned;
            (void)!write(eac_fd, pad, pad_len);
        }
    }
    return written;
}

bool y1_audio_set_deep_idle(bool allow_cpu_powerdown)
{
    const char *cpu_powerdown = allow_cpu_powerdown ? "cpupdn 1\n" : "cpupdn 0\n";
    bool success = true;

    if (cpu_powerdown_enabled != allow_cpu_powerdown) {
        if (y1_sysfs_write("/sys/power/dpidle_state", cpu_powerdown))
            cpu_powerdown_enabled = allow_cpu_powerdown;
        else
            success = false;
    }

    if (!deep_idle_enabled) {
        if (y1_sysfs_write("/sys/power/dpidle_state", "1\n"))
            deep_idle_enabled = true;
        else
            success = false;
    }

    return success;
}

void y1_audio_wake_lock(bool hold)
{
    if (wakelock_fd < 0)
        wakelock_fd = open("/sys/power/wake_lock", O_WRONLY | O_CLOEXEC);
    if (wakelock_fd < 0)
        return;

    if (hold) {
        (void)!write(wakelock_fd, "y1play\n", 7);
    } else {
        int ufd = open("/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC);
        if (ufd >= 0) {
            (void)!write(ufd, "y1play\n", 7);
            close(ufd);
        }
    }
}

void y1_audio_enable_recording_path(enum y1_audio_rec_source source)
{
    if (eac_fd < 0)
        return;

    set_anareg(eac_fd, 0x106, 1, 0xffff);

    if (source == Y1_REC_SOURCE_LINE_FM) {
        set_anareg(eac_fd, 0x700, 0x0000, 0x0006); /* Mic biases off */
        set_anareg(eac_fd, 0x704, 0x0010, 0x0030); /* Line-in ADC mux */
        set_anareg(eac_fd, 0x710, 0x0008, 0x0008);
        set_anareg(eac_fd, 0x4014, 1, 1);
        top_ctl_trigger(eac_fd);
        return;
    }

    if (source == Y1_REC_SOURCE_HEADPHONE_MIC) {
        /* Headphone TRRS Mic: ACC_MIC input, MICBIAS1 enable */
        set_anareg(eac_fd, 0x700, 0x0004, 0x0006);
        set_anareg(eac_fd, 0x704, 0x0005, 0x000f);
    } else {
        /* Built-in Main Mic: MIC_P1 input, MICBIAS0 enable */
        set_anareg(eac_fd, 0x700, 0x0002, 0x0006);
        set_anareg(eac_fd, 0x704, 0x0000, 0x000f);
    }
    set_anareg(eac_fd, 0x700, 0x0100, 0x0100);
    set_anareg(eac_fd, 0x702, 0x1000, 0x1000);
    set_anareg(eac_fd, 0x710, 0x0008, 0x0008);
    set_anareg(eac_fd, 0x4014, 1, 1);
    top_ctl_trigger(eac_fd);
}

void y1_audio_disable_recording_path(void)
{
    if (eac_fd < 0)
        return;
    set_anareg(eac_fd, 0x700, 0x0000, 0x0106);
    set_anareg(eac_fd, 0x702, 0x0000, 0x1000);
    top_ctl_trigger(eac_fd);
}

void y1_audio_set_recording_pga_db(int gain_db)
{
    if (eac_fd < 0)
        return;
    uint32_t pga = (uint32_t)((gain_db / 6) & 0x7);
    set_anareg(eac_fd, 0x702, pga << 8, 0x0700);
    set_anareg(eac_fd, 0x700, pga << 4, 0x0070);
}

static bool rec_active = false;
static int  rec_mem_type = -1;
static int  rec_fd = -1;

static uint32_t calc_ul_rate_code(unsigned long sampr)
{
    switch (sampr) {
        case 8000:  return 0;
        case 16000: return 1;
        case 32000: return 2;
        case 48000: return 3;
        default:    return 3; /* 48 kHz */
    }
}

static uint32_t calc_irq_rate_code(unsigned long sampr)
{
    switch (sampr) {
        case 8000:  return 0;
        case 11025: return 1;
        case 12000: return 2;
        case 16000: return 4;
        case 22050: return 5;
        case 24000: return 6;
        case 32000: return 8;
        case 44100: return 9;
        case 48000: return 10;
        default:    return 10; /* 48 kHz */
    }
}

bool y1_audio_rec_start(enum y1_audio_rec_source source, unsigned long sample_rate)
{
    if (eac_fd < 0 && y1_audio_open(sample_rate) < 0)
        return false;

    if (rec_active)
        y1_audio_rec_stop();

    ensure_eac_node();
    rec_fd = open("/dev/eac", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (rec_fd < 0) {
        y1_logf("[liby1-audio] rec open(/dev/eac) failed: %s\n", strerror(errno));
        return false;
    }

    audio_clocks(1);
    set_afereg(eac_fd, 0x10, 1, 1); /* AFE_ON */

    if (source == Y1_REC_SOURCE_LINE_FM) {
        rec_mem_type = MEM_TYPE_AWB;
        ioctl(rec_fd, AUDDRV_ALLOCATE_AWB_BUF, 0x8000);
        set_afereg(eac_fd, 0x38, 1 << 6, 1 << 6);   /* AFE_CONN5: I2S_IN_L -> AWB_L */
        set_afereg(eac_fd, 0x3c, 1 << 7, 1 << 7);   /* AFE_CONN6: I2S_IN_R -> AWB_R */
        set_afereg(eac_fd, 0x14, 1 << 6, 1 << 6);   /* AFE_DAC_CON0: AWB_ON */
        y1_audio_enable_recording_path(source);
    } else {
        uint32_t ul_code = calc_ul_rate_code(sample_rate);
        rec_mem_type = MEM_TYPE_VUL;
        ioctl(rec_fd, AUDDRV_ALLOCATE_VUL_BUF, 0x8000);

        /* Enable AFE Uplink ADC & route ADDA UL L/R -> VUL (I03->O09, I04->O10 via 0x02c) */
        set_afereg(eac_fd, 0x120, 0, 1);            /* AFE_ADDA_TOP_CON0: internal ADC */
        set_afereg(eac_fd, 0x138, 0x03f87201U, ~0U);/* AFE_ADDA_NEWIF_CFG0 */
        set_afereg(eac_fd, 0x13c, 0x400U, 0xc00U);  /* AFE_ADDA_NEWIF_CFG1 */
        set_afereg(eac_fd, 0x114, (ul_code << 17) | 1U, 0x1e0001U); /* AFE_ADDA_UL_SRC_CON0: UL_SRC_ON */
        set_afereg(eac_fd, 0x124, 1, 1);            /* AFE_ADDA_UL_DL_CON0: ADDA_AFE_ON */
        set_afereg(eac_fd, 0x02c, (1U << 0) | (1U << 3), (1U << 0) | (1U << 3)); /* ADDA_UL L/R -> VUL L/R */
        set_afereg(eac_fd, 0x14, (ul_code << 16) | (1 << 3), (0x3 << 16) | (1 << 3)); /* VUL_MODE + VUL_ON */
        y1_audio_enable_recording_path(source);
    }

    /* Configure IRQ2 (0x3b0 counter, 0x3a0 enable + rate) so AudDrv_read wakes cleanly */
    uint32_t irq_rate = calc_irq_rate_code(sample_rate);
    set_afereg(eac_fd, 0x3b0, 1024, 0xffff);
    set_afereg(eac_fd, 0x3a0, (irq_rate << 8) | (1 << 1), (0xf << 8) | (1 << 1));

    if (ioctl(rec_fd, SET_MEMIF_FP, rec_mem_type) < 0) {
        y1_logf("[liby1-audio] rec SET_MEMIF_FP(%d) failed: %s\n", rec_mem_type, strerror(errno));
    }
    rec_active = true;
    y1_logf("[liby1-audio] Recording started (src=%d, mem=%d, rate=%lu, rec_fd=%d)\n",
            (int)source, rec_mem_type, sample_rate, rec_fd);
    return true;
}

ssize_t y1_audio_rec_read(void *pcm_stereo_s16, size_t bytes)
{
    if (!rec_active || rec_fd < 0 || !pcm_stereo_s16 || bytes == 0)
        return -1;
    struct pollfd pfd = { .fd = rec_fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 50);
    if (pr <= 0)
        return -1;
    return read(rec_fd, pcm_stereo_s16, bytes);
}

void y1_audio_rec_stop(void)
{
    if (!rec_active)
        return;

    if (eac_fd >= 0) {
        set_afereg(eac_fd, 0x3a0, 0, 1 << 1); /* Disable IRQ2 */
        if (rec_mem_type == MEM_TYPE_AWB) {
            set_afereg(eac_fd, 0x14, 0, 1 << 6);
        } else if (rec_mem_type == MEM_TYPE_VUL) {
            set_afereg(eac_fd, 0x14, 0, 1 << 3);
            set_afereg(eac_fd, 0x02c, 0, (1U << 0) | (1U << 3));
            set_afereg(eac_fd, 0x114, 0, 1);
        }
    }
    if (rec_fd >= 0) {
        ioctl(rec_fd, FREE_MEMIF_FP, rec_mem_type);
        if (rec_mem_type == MEM_TYPE_AWB)
            ioctl(rec_fd, AUDDRV_FREE_AWB_BUF, 0);
        else if (rec_mem_type == MEM_TYPE_VUL)
            ioctl(rec_fd, AUDDRV_FREE_VUL_BUF, 0);
        close(rec_fd);
        rec_fd = -1;
    }
    y1_audio_disable_recording_path();
    rec_active = false;
    rec_mem_type = -1;
    y1_log("[liby1-audio] Recording stopped\n");
}

void y1_audio_set_fm_route(bool enable)
{
    if (eac_fd < 0 && y1_audio_open(44100) < 0)
        return;

    const uint32_t conn_mask = (1U << 16) | (1U << 8) | (1U << 23) | (1U << 10);

    if (enable) {
        ioctl(eac_fd, AUD_DRV_FM_I2S_ON, 0);

        /* Configure 2nd I2S In (MT6627 FM slave, 32-bit I2S) */
        set_afereg(eac_fd, 0x18, 0x8000000cU, ~1U);
        set_afereg(eac_fd, 0x4c4, 0, 0x100U);

        /* Route I00/I01 -> HW Digital Gain 2 (O15/O16) -> I2S Out (I12/I13 -> O03/O04) */
        set_afereg(eac_fd, 0x438, conn_mask, conn_mask);
        set_afereg(eac_fd, 0x428, (200U << 8) | (9U << 4), 0xfff0U);
        set_afereg(eac_fd, 0x42c, 0x00080000U, ~0U); /* 0 dB FM digital gain */
        set_afereg(eac_fd, 0x43c, 0, ~0U);
        set_afereg(eac_fd, 0x428, 1, 1);             /* Enable HW Digital Gain 2 */

        /* Enable I2S DAC out, 2nd I2S In, and AFE master switch */
        set_afereg(eac_fd, 0x108, 1, 1);
        set_afereg(eac_fd, 0x34, 1, 1);
        set_afereg(eac_fd, 0x124, 1, 1);
        set_afereg(eac_fd, 0x4c4, 0, 0x10U);
        set_afereg(eac_fd, 0x18, 1, 1);
        set_afereg(eac_fd, 0x10, 1, 1);
        y1_log("[liby1-audio] FM I2S audio route enabled\n");
    } else {
        set_afereg(eac_fd, 0x438, 0, conn_mask);
        set_afereg(eac_fd, 0x428, 0, 1);
        set_afereg(eac_fd, 0x18, 0, 1);
        ioctl(eac_fd, AUD_DRV_FM_I2S_OFF, 0);
        y1_log("[liby1-audio] FM I2S audio route disabled\n");
    }
}

