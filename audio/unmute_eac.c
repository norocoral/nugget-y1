/*
 * unmute_eac.c - Innioasis Y1 / MediaTek MT6572 Headphone Amplifier Unmute Utility
 *
 * Direct hardware control via /dev/eac ioctl interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <errno.h>

#ifndef makedev
#define makedev(maj, min) (((maj) << 8) | (min))
#endif

#define SET_SPEAKER_ON      0x400443a1
#define SET_SPEAKER_OFF     0x400443a2
#define SET_HEADPHONE_ON    0x400443a4
#define SET_HEADPHONE_OFF   0x400443a5
#define SET_EARPIECE_ON     0x400443a6
#define SET_EARPIECE_OFF    0x400443a7
#define SET_HEADSET_STATE   0x400443a3
#define SET_AUDIO_STATE     0xc0044342
#define AUDDRV_SET_ANAREG   0xc0044302
#define AUDDRV_GET_ANAREG   0xc0044303
#define FREE_MEMIF_BUF      0xc004431f

struct Register_Control {
    unsigned int offset;
    unsigned int value;
    unsigned int mask;
};

static void set_anareg(int fd, unsigned int offset, unsigned int val, unsigned int mask)
{
    struct Register_Control reg = { offset, val, mask };
    ioctl(fd, AUDDRV_SET_ANAREG, &reg);
}

static inline unsigned int get_anareg(int fd, unsigned int offset)
{
    struct Register_Control reg = { offset, 0, ~0U };
    if (ioctl(fd, AUDDRV_GET_ANAREG, &reg) == 0)
        return reg.value;
    return 0;
}

static inline void top_ctl_trigger(int fd)
{
    unsigned int val = get_anareg(fd, 0x4016);
    unsigned int toggle = (val ^ 0x300) & 0x300;
    set_anareg(fd, 0x4016, toggle, 0x300);
}

static int ensure_eac_node(void)
{
    /* Try to discover minor from sysfs */
    int maj = 10, minor = 255;
    FILE *f = fopen("/sys/class/misc/eac/dev", "r");
    if (f) {
        int min = -1;
        if (fscanf(f, "%d:%d", &maj, &min) == 2 && min >= 0) {
            minor = min;
            printf("[unmute_eac] Discovered /sys/class/misc/eac/dev: major %d, minor %d\n", maj, minor);
        }
        fclose(f);
    }

    unlink("/dev/eac");
    printf("[unmute_eac] Creating /dev/eac node (major %d, minor %d)...\n", maj, minor);
    dev_t dev = makedev(maj, minor);
    if (mknod("/dev/eac", S_IFCHR | 0666, dev) < 0 && errno != EEXIST) {
        perror("[unmute_eac] mknod failed");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    printf("[unmute_eac] Innioasis Y1 audio unmute starting...\n");
    ensure_eac_node();

    int fd = open("/dev/eac", O_RDWR);
    if (fd < 0) {
        perror("[unmute_eac] Failed to open /dev/eac");
        return 1;
    }

    /* 1. Turn on Audio Clocks (Clock 4: AFE, Clock 1: AIF) */
    ioctl(fd, 0xc0044355, 1);
    ioctl(fd, 0xc0044351, 1);

    /* 2. Mute unused endpoints */
    ioctl(fd, SET_SPEAKER_OFF, 0);
    ioctl(fd, SET_EARPIECE_OFF, 0);

    /* 3. Power up MT6323 PMIC Audio Clocks */
    set_anareg(fd, 0x106, 1, 0xffff);        /* PMIC TOP_CKPDN_CON0: Power on audio clock */
    set_anareg(fd, 0x10c, 0x100, 0x100);    /* PMIC TOP_CKG_CON1 bit 8: Audio 13M clock gating */

    /* 4. Power up MT6323 PMIC Audio Downlink DAC & Clocks */
    set_anareg(fd, 0x700, 0x7800, 0x7f80);
    set_anareg(fd, 0x704, 0x00f0, 0x00ff);
    set_anareg(fd, 0x702, 0x0500, 0x0700); /* Maximum preamp buffer gain Right (5 << 8) */
    set_anareg(fd, 0x700, 0x0050, 0x0070); /* Maximum preamp buffer gain Left (5 << 4) */
    set_anareg(fd, 0x710, 0x0008, 0x0008);
    set_anareg(fd, 0x700, 0x0180, 0x0180);
    set_anareg(fd, 0x704, 0x0003, 0x0003);

    /* 5. Power up MT6323 PMIC Headphone Amplifier */
    set_anareg(fd, 0x70c, 0xf7f2, 0xffff);
    set_anareg(fd, 0x700, 0x7000, 0xf000);
    set_anareg(fd, 0x70a, 0x0014, 0xffff);
    set_anareg(fd, 0x708, 0x007c, 0xffff);
    usleep(10000);
    set_anareg(fd, 0x70c, 0xf5ba, 0xffff);
    set_anareg(fd, 0x70a, 0x7714, 0xffff); /* Max +14dB analog gain Left & Right, Headphone powered */

    /* 6. Configure MT6323 PMIC Digital Audio Downlink (NewIF & Codec) */
    set_anareg(fd, 0x4024, (7 << 12) | 0x330, 0xffff); /* PMIC NewIF DL 44.1kHz (0x7330) */
    set_anareg(fd, 0x4002, 9, 0x0f);                   /* PMIC Codec DL 44.1kHz */
    set_anareg(fd, 0x4006, 0, 0xffff);                 /* PMIC DL Left digital gain: 0dB */
    set_anareg(fd, 0x4008, 0, 0xffff);                 /* PMIC DL Right digital gain: 0dB */
    set_anareg(fd, 0x4014, 1, 1);                      /* PMIC Downlink channel enable */

    /* 7. PMIC CDC Clock Synchronization & Master Enable */
    top_ctl_trigger(fd);
    set_anareg(fd, 0x4000, 1, 1);                      /* PMIC TOP_CON0: Audio Digital Master ON */

    /* 8. Power up and unmute headphone amplifier */
    if (ioctl(fd, SET_HEADPHONE_ON, 3) < 0) {
        perror("[unmute_eac] SET_HEADPHONE_ON ioctl failed");
        close(fd);
        return 1;
    }

    printf("[unmute_eac] MT6323 Headphone amplifier powered ON (Stereo L+R).\n");
    close(fd);
    return 0;
}

