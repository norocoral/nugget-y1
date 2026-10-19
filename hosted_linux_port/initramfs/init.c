/*
 * init.c - Production PID 1 Init for Innioasis Y1 Hosted Rockbox Linux
 *
 * Provides:
 *  - Devtmpfs and device node auto-discovery (/dev/graphics/fb0 <-> /dev/fb0)
 *  - Dynamic Physical MicroSD discovery via /proc/partitions (waiting up to 5s for SDXC)
 *  - Exclusive Physical SD card primary mounting (eMMC virtual SD is excluded per design)
 *  - Multi-partition MicroSD & USB storage automounting
 *  - USB Mass Storage & LUN initialization for PC music transfer (Windows/Mac/Linux)
 *  - MT6323 Headphone Amplifier hardware unmute & MTK AFE/AIF audio clocks
 *  - Click wheel touch scanning activation (/proc/tpd_keys_enable)
 *  - Persistent early boot logging to /mnt/sdcard/boot_log.txt and /dev/kmsg
 *  - Hardware Framebuffer unblanking & visual splash verification
 *  - Background hotplug automounter daemon
 *  - Execution of /rockbox with error recovery
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <linux/fb.h>
#include <dirent.h>
#include <sys/wait.h>

#define SET_SPEAKER_OFF     0x400443a2
#define SET_HEADPHONE_ON    0x400443a4
#define SET_EARPIECE_OFF    0x400443a7
#define AUDDRV_SET_ANAREG   0xc0044302
#define AUDDRV_GET_ANAREG   0xc0044303
#define AUD_DRV_AFE_CLK     0xc0044355
#define AUD_DRV_AIF_CLK     0xc0044351

struct Register_Control {
    uint32_t offset;
    uint32_t value;
    uint32_t mask;
};

static void set_anareg(int fd, uint32_t offset, uint32_t val, uint32_t mask)
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

static inline void top_ctl_trigger(int fd)
{
    uint32_t val = get_anareg(fd, 0x4016);
    uint32_t toggle = ((~val) << 8) & 0x300;
    set_anareg(fd, 0x4016, toggle, 0x300);
}

#ifndef VESA_NO_BLANKING
#define VESA_NO_BLANKING 0
#endif

static int log_fd = -1;
static char early_log_buf[8192];
static size_t early_log_len = 0;

static char active_sd_dev[64] = {0};
static char active_ext_dev[64] = {0};

static void log_line(const char *msg)
{
    size_t len = strlen(msg);

    /* 1. Write to stdout */
    (void)!write(1, msg, len);

    /* 2. Buffer early log before persistent storage is mounted */
    if (log_fd < 0 && early_log_len + len < sizeof(early_log_buf)) {
        memcpy(early_log_buf + early_log_len, msg, len);
        early_log_len += len;
        early_log_buf[early_log_len] = '\0';
    }

    /* 3. Write to persistent boot log */
    if (log_fd >= 0) {
        (void)!write(log_fd, msg, len);
        fsync(log_fd);
    }

    /* 4. Write to kernel ring buffer */
    int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0) {
        (void)!write(kfd, "<6>[init] ", 10);
        (void)!write(kfd, msg, len);
        close(kfd);
    }
}

static void flush_early_log(void)
{
    if (log_fd >= 0 && early_log_len > 0) {
        (void)!write(log_fd, early_log_buf, early_log_len);
        fsync(log_fd);
        early_log_len = 0;
    }
}

static void write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)!write(fd, val, strlen(val));
        close(fd);
    }
}

/* Dynamically parse /proc/partitions and generate corresponding /dev and /dev/block nodes */
static void scan_proc_partitions_and_create_nodes(void)
{
    mkdir("/dev/block", 0755);

    FILE *fp = fopen("/proc/partitions", "r");
    if (!fp) return;

    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        int major, minor;
        unsigned long long blocks;
        char name[64];
        if (sscanf(line, "%d %d %llu %63s", &major, &minor, &blocks, name) == 4) {
            char p1[128], p2[128];
            snprintf(p1, sizeof(p1), "/dev/%s", name);
            snprintf(p2, sizeof(p2), "/dev/block/%s", name);

            if (access(p1, F_OK) != 0) {
                mknod(p1, S_IFBLK | 0660, makedev(major, minor));
            }
            if (access(p2, F_OK) != 0) {
                mknod(p2, S_IFBLK | 0660, makedev(major, minor));
            }
        }
    }
    fclose(fp);
}

/* Check if physical MicroSD (mmcblk1) has been discovered by kernel MMC subsystem */
static int is_mmcblk1_in_proc_partitions(void)
{
    FILE *fp = fopen("/proc/partitions", "r");
    if (!fp) return 0;

    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "mmcblk1")) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

static void make_device_nodes(void)
{
    mkdir("/dev/graphics", 0755);
    mkdir("/dev/block", 0755);
    mkdir("/dev/input", 0755);

    /* Framebuffer nodes */
    if (access("/dev/graphics/fb0", F_OK) != 0 && access("/dev/fb0", F_OK) != 0) {
        mknod("/dev/fb0", S_IFCHR | 0660, makedev(29, 0));
    }
    if (access("/dev/graphics/fb0", F_OK) == 0 && access("/dev/fb0", F_OK) != 0) {
        (void)!symlink("/dev/graphics/fb0", "/dev/fb0");
    } else if (access("/dev/fb0", F_OK) == 0 && access("/dev/graphics/fb0", F_OK) != 0) {
        (void)!symlink("/dev/fb0", "/dev/graphics/fb0");
    }

    /* Standard character nodes */
    if (access("/dev/console", F_OK) != 0) mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
    if (access("/dev/null", F_OK) != 0) mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));

    /* Input event nodes (support up to 16 devices) */
    for (int i = 0; i < 16; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/input/event%d", i);
        if (access(p, F_OK) != 0) {
            mknod(p, S_IFCHR | 0660, makedev(13, 64 + i));
        }
    }

    /* Note: mmcblk1 and partition nodes are created dynamically via /proc/partitions */
}

static int try_mount_storage(const char *devpath, const char *target)
{
    mkdir(target, 0755);

    /* 1. Try VFAT (UTF-8, mixed shortnames, full executable permissions for plugins, and sync flush) */
    if (mount(devpath, target, "vfat", MS_NOATIME | MS_NODIRATIME, "utf8,shortname=mixed,umask=0000,fmask=0000,dmask=0000,flush") == 0) {
        return 0;
    }
    if (mount(devpath, target, "vfat", MS_NOATIME | MS_NODIRATIME, "codepage=437,iocharset=utf8,shortname=mixed,umask=0000,fmask=0000,dmask=0000,flush") == 0) {
        return 0;
    }
    if (mount(devpath, target, "vfat", MS_NOATIME | MS_NODIRATIME, "umask=0000,fmask=0000,dmask=0000,flush") == 0) {
        return 0;
    }
    if (mount(devpath, target, "vfat", MS_NOATIME | MS_NODIRATIME, "") == 0) {
        return 0;
    }

    /* 2. Try exFAT (full permissions for dlopen) */
    if (mount(devpath, target, "exfat", MS_NOATIME | MS_NODIRATIME, "utf8,umask=0000,fmask=0000,dmask=0000") == 0) {
        return 0;
    }
    if (mount(devpath, target, "exfat", MS_NOATIME | MS_NODIRATIME, "iocharset=utf8,umask=0000,fmask=0000,dmask=0000") == 0) {
        return 0;
    }
    if (mount(devpath, target, "exfat", MS_NOATIME | MS_NODIRATIME, "iocharset=utf8,codepage=437,umask=0000,fmask=0000,dmask=0000") == 0) {
        return 0;
    }
    if (mount(devpath, target, "exfat", MS_NOATIME | MS_NODIRATIME, "umask=0000,fmask=0000,dmask=0000") == 0) {
        return 0;
    }
    if (mount(devpath, target, "exfat", MS_NOATIME | MS_NODIRATIME, NULL) == 0) {
        return 0;
    }

    /* 3. Try NTFS */
    if (mount(devpath, target, "ntfs", MS_NOATIME | MS_NODIRATIME, "utf8,umask=0000,fmask=0000,dmask=0000") == 0) {
        return 0;
    }
    if (mount(devpath, target, "ntfs", MS_NOATIME | MS_NODIRATIME, "") == 0) {
        return 0;
    }

    /* 4. Try ext4 */
    if (mount(devpath, target, "ext4", MS_NOATIME | MS_NODIRATIME, "") == 0) {
        return 0;
    }

    return -1;
}

static int is_mounted(const char *target)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char dev[64], mnt[64];
        if (sscanf(line, "%63s %63s", dev, mnt) == 2) {
            if (strcmp(mnt, target) == 0) {
                found = 1;
                break;
            }
        }
    }
    fclose(fp);
    return found;
}

/* Link all user folders and files from /mnt/sdcard into / so plugins and apps see / as SD root */
static void link_sdcard_to_root(void)
{
    DIR *d = opendir("/mnt/sdcard");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        /* Do not overwrite critical rootfs directories */
        if (strcmp(de->d_name, "bin") == 0 || strcmp(de->d_name, "sbin") == 0 ||
            strcmp(de->d_name, "dev") == 0 || strcmp(de->d_name, "proc") == 0 ||
            strcmp(de->d_name, "sys") == 0 || strcmp(de->d_name, "etc") == 0 ||
            strcmp(de->d_name, "mnt") == 0 || strcmp(de->d_name, "init") == 0 ||
            strcmp(de->d_name, "rootfs") == 0 || strcmp(de->d_name, "usr") == 0 ||
            strcmp(de->d_name, "storage") == 0 || strcmp(de->d_name, "rockbox") == 0 ||
            strcmp(de->d_name, "lib") == 0)
            continue;

        char target[512], linkname[512];
        snprintf(target, sizeof(target), "/mnt/sdcard/%s", de->d_name);
        snprintf(linkname, sizeof(linkname), "/%s", de->d_name);

        struct stat st;
        if (lstat(linkname, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                unlink(linkname);
            } else {
                continue; /* don't remove existing real directory */
            }
        }
        (void)!symlink(target, linkname);
    }
    closedir(d);
}

/* Full storage scanner & automounter (Physical MicroSD only, eMMC excluded) */
static void scan_and_mount_storage(int verbose)
{
    scan_proc_partitions_and_create_nodes();

    /* 1. Mount physical MicroSD card to /mnt/sdcard (Primary Storage / Drive 0) */
    if (!is_mounted("/mnt/sdcard")) {
        /* Check partition 1 first (e.g. mmcblk1p1) */
        const char *part1_candidates[] = {
            "/dev/block/mmcblk1p1",
            "/dev/mmcblk1p1",
            NULL
        };
        for (int i = 0; part1_candidates[i]; i++) {
            if (access(part1_candidates[i], F_OK) == 0 &&
                try_mount_storage(part1_candidates[i], "/mnt/sdcard") == 0) {
                snprintf(active_sd_dev, sizeof(active_sd_dev), "%s", part1_candidates[i]);
                if (verbose) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Mounted physical MicroSD partition 1 (%s) on /mnt/sdcard.\n", active_sd_dev);
                    log_line(msg);
                }
                break;
            }
        }

        /* If partition 1 was not found or failed, try unpartitioned superfloppy mmcblk1 */
        if (!active_sd_dev[0]) {
            const char *raw_candidates[] = {
                "/dev/block/mmcblk1",
                "/dev/mmcblk1",
                NULL
            };
            for (int i = 0; raw_candidates[i]; i++) {
                if (access(raw_candidates[i], F_OK) == 0 &&
                    try_mount_storage(raw_candidates[i], "/mnt/sdcard") == 0) {
                    snprintf(active_sd_dev, sizeof(active_sd_dev), "%s", raw_candidates[i]);
                    if (verbose) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Mounted unpartitioned physical MicroSD (%s) on /mnt/sdcard.\n", active_sd_dev);
                        log_line(msg);
                    }
                    break;
                }
            }
        }

        /* If mounted, setup standard compatibility symlinks & open boot log */
        if (is_mounted("/mnt/sdcard")) {
            mkdir("/storage", 0755);
            unlink("/storage/sdcard0");
            (void)!symlink("/mnt/sdcard", "/storage/sdcard0");
            unlink("/sdcard");
            (void)!symlink("/mnt/sdcard", "/sdcard");

            if (log_fd < 0) {
                log_fd = open("/mnt/sdcard/boot_log.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
                if (log_fd >= 0) {
                    flush_early_log();
                    dup2(log_fd, 1);
                    dup2(log_fd, 2);
                }
            }

            link_sdcard_to_root();
        }
    }

    /* 2. Scan and mount secondary partitions on physical MicroSD (mmcblk1p2..p8) */
    int ext_part_count = 0;
    char found_ext_dev[64] = {0};

    for (int p = 2; p <= 8; p++) {
        char devpath[64], mntpath[64], linkpath[64];
        snprintf(devpath, sizeof(devpath), "/dev/block/mmcblk1p%d", p);
        if (access(devpath, F_OK) != 0) {
            snprintf(devpath, sizeof(devpath), "/dev/mmcblk1p%d", p);
        }

        if (access(devpath, F_OK) == 0) {
            if (ext_part_count == 0) {
                snprintf(mntpath, sizeof(mntpath), "/mnt/extsd");
                snprintf(linkpath, sizeof(linkpath), "/mnt/sdcard/MicroSD_Part%d", p);
                snprintf(found_ext_dev, sizeof(found_ext_dev), "%s", devpath);
            } else {
                snprintf(mntpath, sizeof(mntpath), "/mnt/extsd%d", ext_part_count + 1);
                snprintf(linkpath, sizeof(linkpath), "/mnt/sdcard/MicroSD_Part%d", p);
            }

            if (is_mounted(mntpath) || try_mount_storage(devpath, mntpath) == 0) {
                ext_part_count++;
                unlink(linkpath);
                (void)!symlink(mntpath, linkpath);

                if (verbose) {
                    char logmsg[256];
                    snprintf(logmsg, sizeof(logmsg), "Mounted MicroSD partition %d (%s) on %s, linked %s\n",
                             p, devpath, mntpath, linkpath);
                    log_line(logmsg);
                }
            }
        }
    }

    if (ext_part_count > 0 && is_mounted("/mnt/extsd")) {
        snprintf(active_ext_dev, sizeof(active_ext_dev), "%s", found_ext_dev[0] ? found_ext_dev : "/dev/block/mmcblk1p2");
        unlink("/storage/sdcard1");
        (void)!symlink("/mnt/extsd", "/storage/sdcard1");
        unlink("/mnt/sdcard2");
        (void)!symlink("/mnt/extsd", "/mnt/sdcard2");
        unlink("/mnt/external_sd");
        (void)!symlink("/mnt/extsd", "/mnt/external_sd");
    }

    /* 3. Scan and mount USB OTG drives (sda1..p8 or raw sda) */
    int usb_count = 0;
    for (int p = 1; p <= 8; p++) {
        char devpath[64], mntpath[64], linkpath[64];
        snprintf(devpath, sizeof(devpath), "/dev/block/sda%d", p);
        if (access(devpath, F_OK) != 0) {
            snprintf(devpath, sizeof(devpath), "/dev/sda%d", p);
        }
        if (access(devpath, F_OK) == 0) {
            if (usb_count == 0) {
                snprintf(mntpath, sizeof(mntpath), "/mnt/usb");
                snprintf(linkpath, sizeof(linkpath), "/mnt/sdcard/USB");
            } else {
                snprintf(mntpath, sizeof(mntpath), "/mnt/usb%d", usb_count + 1);
                snprintf(linkpath, sizeof(linkpath), "/mnt/sdcard/USB%d", usb_count + 1);
            }
            if (is_mounted(mntpath) || try_mount_storage(devpath, mntpath) == 0) {
                usb_count++;
                unlink(linkpath);
                (void)!symlink(mntpath, linkpath);
                if (verbose) {
                    char logmsg[256];
                    snprintf(logmsg, sizeof(logmsg), "Mounted USB storage (%s) on %s, linked %s\n",
                             devpath, mntpath, linkpath);
                    log_line(logmsg);
                }
            }
        }
    }

    /* 4. Update USB gadget LUN0 backing file dynamically */
    if (access("/sys/class/android_usb/android0/f_mass_storage/lun0/file", F_OK) == 0) {
        const char *lun_target = NULL;
        if (access("/dev/block/mmcblk1", F_OK) == 0) {
            lun_target = "/dev/block/mmcblk1";
        } else if (active_sd_dev[0]) {
            lun_target = active_sd_dev;
        }

        if (lun_target) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s\n", lun_target);
            write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun0/file", buf);
            write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun/file", buf);
        }
    }
}

static void optimize_power_and_battery(void)
{
    log_line("Applying aggressive OS battery and power optimizations...\n");

    /* 1. MT6572 SPM (System Power Manager) Deep Idle & Regular Idle
     * Enables hardware SPM clock gating and CPU power-down when in WFI idle. */
    write_sysfs("/sys/power/idle_state", "1\n");
    write_sysfs("/sys/power/dpidle_state", "1\n");
    write_sysfs("/sys/power/dpidle_state", "cpupdn 1\n");
    write_sysfs("/sys/power/rgidle_state", "1\n");

    /* 2. CPU Frequency Scaling & Governor (ondemand with aggressive powersave bias) */
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "ondemand\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", "598000\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "1001000\n");

    /* Tune ondemand governor parameters for maximum power saving */
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/up_threshold", "98\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/sampling_down_factor", "1\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/sampling_rate", "100000\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/io_is_busy", "0\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/powersave_bias", "200\n");

    /* 3. Power-gate secondary CPU core (CPU1) for pure single-core audio playback */
    write_sysfs("/sys/devices/system/cpu/cpu1/online", "0\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_base", "1\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_limit", "1\n");

    /* 4. Linux Kernel VM dirty page flush tuning (avoid waking up storage bus) */
    write_sysfs("/proc/sys/vm/dirty_writeback_centisecs", "12000\n");
    write_sysfs("/proc/sys/vm/dirty_expire_centisecs", "12000\n");
    write_sysfs("/proc/sys/vm/laptop_mode", "5\n");

    /* 5. Storage queue schedulers & runtime power management */
    write_sysfs("/sys/block/mmcblk1/queue/scheduler", "noop\n");
    write_sysfs("/sys/block/mmcblk1/queue/read_ahead_kb", "512\n");
    write_sysfs("/sys/block/mmcblk0/queue/scheduler", "noop\n");
    write_sysfs("/sys/block/mmcblk0/queue/read_ahead_kb", "512\n");
    write_sysfs("/sys/devices/platform/mtk-msdc.0/power/control", "auto\n");
    write_sysfs("/sys/devices/platform/mtk-msdc.1/power/control", "auto\n");
    write_sysfs("/sys/block/mmcblk0/power/control", "auto\n");
    write_sysfs("/sys/block/mmcblk1/power/control", "auto\n");
    write_sysfs("/sys/devices/platform/mt_usb/power/control", "auto\n");

    /* 6. Suppress kernel console wakeups / printk spam */
    write_sysfs("/proc/sys/kernel/printk", "1 1 1 1\n");

    /* 7. Ensure unused wireless/connectivity chips remain unpowered */
    write_sysfs("/sys/class/rfkill/rfkill0/state", "0\n");
    write_sysfs("/sys/class/rfkill/rfkill1/state", "0\n");

    log_line("Battery optimizations active: CPU 598MHz/single-core, SPM Deep Idle enabled.\n");
}

static void display_screen(const char *name)
{
    char buf[128];
    const char *devs[] = {"/dev/graphics/fb0", "/dev/fb0", NULL};
    int fd = -1;
    const char *used_dev = NULL;

    for (int i = 0; devs[i]; i++) {
        fd = open(devs[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            used_dev = devs[i];
            break;
        }
    }

    if (fd < 0) {
        log_line("ERROR: Failed to open any framebuffer device!\n");
        return;
    }

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    memset(&finfo, 0, sizeof(finfo));
    memset(&vinfo, 0, sizeof(vinfo));

    ioctl(fd, FBIOGET_FSCREENINFO, &finfo);
    ioctl(fd, FBIOGET_VSCREENINFO, &vinfo);

    /* Unblank the hardware LCD */
    ioctl(fd, FBIOBLANK, VESA_NO_BLANKING);

    size_t map_len = finfo.smem_len ? finfo.smem_len : (vinfo.xres * vinfo.yres * 4);
    uint8_t *fb = (uint8_t *)mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (fb != MAP_FAILED && fb != NULL) {
        int stride = finfo.line_length ? finfo.line_length : (vinfo.xres * (vinfo.bits_per_pixel / 8));
        int w = vinfo.xres;
        int h = vinfo.yres;

        char path[128];
        int img_fd = -1;

        /* 1. Try initramfs /etc/<name>.raw first */
        snprintf(path, sizeof(path), "/etc/%s.raw", name);
        img_fd = open(path, O_RDONLY | O_CLOEXEC);

        /* 2. Try SD card fallbacks */
        if (img_fd < 0) {
            snprintf(path, sizeof(path), "/.rockbox/%s.raw", name);
            img_fd = open(path, O_RDONLY | O_CLOEXEC);
        }
        if (img_fd < 0) {
            snprintf(path, sizeof(path), "/mnt/sdcard/%s.raw", name);
            img_fd = open(path, O_RDONLY | O_CLOEXEC);
        }
        if (img_fd < 0) {
            snprintf(path, sizeof(path), "/mnt/sdcard/.rockbox/%s.raw", name);
            img_fd = open(path, O_RDONLY | O_CLOEXEC);
        }

        if (img_fd >= 0) {
            if (vinfo.bits_per_pixel == 32) {
                if (stride == w * 4) {
                    /* Direct full-frame read */
                    ssize_t rb = read(img_fd, fb, w * h * 4);
                    (void)rb;
                } else {
                    /* Row by row in case framebuffer stride exceeds width */
                    for (int r = 0; r < h; r++) {
                        ssize_t rb = read(img_fd, fb + r * stride, w * 4);
                        if (rb <= 0) break;
                    }
                }
            } else {
                /* 16-bit RGB565 fallback: convert each 32-bit pixel */
                uint32_t line_buf[480];
                for (int r = 0; r < h; r++) {
                    ssize_t rb = read(img_fd, line_buf, sizeof(line_buf));
                    if (rb <= 0) break;
                    uint16_t *row16 = (uint16_t *)(fb + r * stride);
                    for (int c = 0; c < w; c++) {
                        uint32_t px = line_buf[c];
                        uint16_t r5 = (px >> 19) & 0x1F;
                        uint16_t g6 = (px >> 10) & 0x3F;
                        uint16_t b5 = (px >> 3) & 0x1F;
                        row16[c] = (r5 << 11) | (g6 << 5) | b5;
                    }
                }
            }
            close(img_fd);
            snprintf(buf, sizeof(buf), "Displayed hardware screen '%s.raw' (%s).\n", name, used_dev ? used_dev : "fb0");
            log_line(buf);
        } else {
            /* Fallback to clean black screen */
            memset(fb, 0, map_len);
            snprintf(buf, sizeof(buf), "Notice: Screen asset '%s.raw' not found, cleared screen to black.\n", name);
            log_line(buf);
        }

        /* Signal display controller to refresh */
        vinfo.yoffset = 0;
        vinfo.xoffset = 0;
        ioctl(fd, FBIOPAN_DISPLAY, &vinfo);
        munmap(fb, map_len);
    } else {
        log_line("ERROR: mmap framebuffer failed during display screen!\n");
    }

    close(fd);
}

static int setup_eac_node(void)
{
    int maj = 10, min = -1;
    FILE *fp = fopen("/sys/class/misc/eac/dev", "r");
    if (fp) {
        if (fscanf(fp, "%d:%d", &maj, &min) == 2 && min >= 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Discovered /dev/eac from sysfs: major %d, minor %d\n", maj, min);
            log_line(msg);
        }
        fclose(fp);
    }
    if (min < 0) {
        log_line("Notice: /sys/class/misc/eac/dev not available, using fallback minor 255\n");
        min = 255;
    }
    unlink("/dev/eac");
    mknod("/dev/eac", S_IFCHR | 0666, makedev(maj, min));
    return min;
}

static void unmute_audio(void)
{
    log_line("Initializing MTK Audio hardware & clocks...\n");
    setup_eac_node();
    int fd = open("/dev/eac", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "ERROR: open(/dev/eac) failed: %s (errno=%d)\n", strerror(errno), errno);
        log_line(errbuf);
        return;
    }
    log_line("Successfully opened /dev/eac for audio unmute.\n");
    /* Turn on hardware audio clocks on SoC */
    ioctl(fd, AUD_DRV_AFE_CLK, 1);
    ioctl(fd, AUD_DRV_AIF_CLK, 1);

    /* Route to Headphone amplifier and mute unused */
    ioctl(fd, SET_SPEAKER_OFF, 0);
    ioctl(fd, SET_EARPIECE_OFF, 0);

    /* Power up MT6323 PMIC Audio Clocks */
    set_anareg(fd, 0x106, 1, 0xffff);        /* PMIC TOP_CKPDN_CON0: Power on audio clock */
    set_anareg(fd, 0x10c, 0x100, 0x100);    /* PMIC TOP_CKG_CON1 bit 8: Audio 13M clock gating */

    /* Power up MT6323 PMIC Audio Downlink DAC & Clocks */
    set_anareg(fd, 0x700, 0x7800, 0x7f80);
    set_anareg(fd, 0x704, 0x00f0, 0x00ff);
    set_anareg(fd, 0x702, 0x0500, 0x0700); /* Maximum preamp buffer gain Right (5 << 8) */
    set_anareg(fd, 0x700, 0x0050, 0x0070); /* Maximum preamp buffer gain Left (5 << 4) */
    set_anareg(fd, 0x710, 0x0008, 0x0008);
    set_anareg(fd, 0x700, 0x0180, 0x0180);
    set_anareg(fd, 0x704, 0x0003, 0x0003);

    /* Power up MT6323 PMIC Headphone Amplifier */
    set_anareg(fd, 0x70c, 0xf7f2, 0xffff);
    set_anareg(fd, 0x700, 0x7000, 0xf000);
    set_anareg(fd, 0x70a, 0x0014, 0xffff);
    set_anareg(fd, 0x708, 0x007c, 0xffff);
    usleep(10000);
    set_anareg(fd, 0x70c, 0xf5ba, 0xffff);
    set_anareg(fd, 0x70a, 0x7714, 0xffff); /* Max +14dB analog gain Left & Right, Headphone powered */

    /* Configure MT6323 PMIC Digital Audio Downlink (NewIF & Codec) */
    set_anareg(fd, 0x4024, (7 << 12) | 0x330, 0xffff); /* PMIC NewIF DL 44.1kHz (0x7330) */
    set_anareg(fd, 0x4002, 9, 0x0f);                   /* PMIC Codec DL 44.1kHz */
    set_anareg(fd, 0x4006, 0, 0xffff);                 /* PMIC DL Left digital gain: 0dB */
    set_anareg(fd, 0x4008, 0, 0xffff);                 /* PMIC DL Right digital gain: 0dB */
    set_anareg(fd, 0x4014, 1, 1);                      /* PMIC Downlink channel enable */

    /* PMIC CDC Clock Synchronization & Master Enable */
    top_ctl_trigger(fd);
    set_anareg(fd, 0x4000, 1, 1);                      /* PMIC TOP_CON0: Audio Digital Master ON */

    if (ioctl(fd, SET_HEADPHONE_ON, 3) == 0) {
        log_line("MT6323 Headphone amplifier powered ON (Stereo L+R).\n");
    } else {
        log_line("Warning: SET_HEADPHONE_ON ioctl failed.\n");
    }
    close(fd);
}

static void enable_touch_keys(void)
{
    int fd = open("/proc/tpd_keys_enable", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)!write(fd, "1\n", 2);
        close(fd);
        log_line("Touch keypad scanning enabled (/proc/tpd_keys_enable).\n");
    } else {
        log_line("Notice: /proc/tpd_keys_enable not present.\n");
    }
}

static void setup_usb_gadget(const char *sd_dev, const char *ext_dev)
{
    log_line("Configuring USB Mass Storage gadget for PC access...\n");

    /* 1. Disable gadget while reconfiguring */
    write_sysfs("/sys/class/android_usb/android0/enable", "0\n");

    /* 2. Configure USB descriptors */
    write_sysfs("/sys/class/android_usb/android0/idVendor", "0e8d\n");
    write_sysfs("/sys/class/android_usb/android0/idProduct", "0001\n");
    write_sysfs("/sys/class/android_usb/android0/iManufacturer", "Innioasis\n");
    write_sysfs("/sys/class/android_usb/android0/iProduct", "Y1 Rockbox Player\n");
    write_sysfs("/sys/class/android_usb/android0/functions", "mass_storage\n");

    /* 3. Determine primary backing device for LUN0 (Physical MicroSD) */
    const char *target_lun0 = sd_dev;
    if (!target_lun0 || access(target_lun0, F_OK) != 0) {
        if (access("/dev/block/mmcblk1", F_OK) == 0) {
            target_lun0 = "/dev/block/mmcblk1";
        } else if (access("/dev/block/mmcblk1p1", F_OK) == 0) {
            target_lun0 = "/dev/block/mmcblk1p1";
        }
    }

    /* 4. Configure LUN0 backing file BEFORE enabling gadget */
    if (target_lun0 && access(target_lun0, F_OK) == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s\n", target_lun0);

        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun0/file", buf);
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun/file", buf);

        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun0/nofua", "1\n");
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun/nofua", "1\n");

        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun0/ro", "0\n");
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun/ro", "0\n");

        char logmsg[128];
        snprintf(logmsg, sizeof(logmsg), "Attached %s to USB LUN0 (Physical MicroSD).\n", target_lun0);
        log_line(logmsg);
    } else {
        log_line("Notice: Waiting for MicroSD insertion to bind USB LUN0.\n");
    }

    /* 5. Configure LUN1 for secondary partition if present */
    if (ext_dev && access(ext_dev, F_OK) == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s\n", ext_dev);

        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/file", buf);
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/nofua", "1\n");
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/ro", "0\n");

        char logmsg[128];
        snprintf(logmsg, sizeof(logmsg), "Attached %s to USB LUN1 (Secondary Partition).\n", ext_dev);
        log_line(logmsg);
    } else {
        /* Clear LUN1 if no secondary storage */
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "\n");
    }

    /* 6. Enable gadget after LUNs are assigned */
    write_sysfs("/sys/class/android_usb/android0/enable", "1\n");
    log_line("USB Mass Storage activated (PC music transfer ready).\n");
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* 1. Mount essential virtual filesystems */
    mkdir("/proc", 0755);
    mount("proc", "/proc", "proc", 0, NULL);

    mkdir("/sys", 0755);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    mkdir("/dev", 0755);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) < 0) {
        mount("tmpfs", "/dev", "tmpfs", 0, NULL);
    }

    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);

    /* 2. Create device nodes */
    make_device_nodes();

    /* 3. Display hardware boot splash from boot.png immediately */
    display_screen("boot");

    /* 4. Prepare mount points */
    mkdir("/mnt", 0755);
    mkdir("/mnt/sdcard", 0755);
    mkdir("/mnt/extsd", 0755);
    mkdir("/mnt/usb", 0755);

    log_line("=================================================\n");
    log_line("   Innioasis Y1 Hosted Rockbox Linux Boot       \n");
    log_line("=================================================\n");

    /* 5. Wait up to 5 seconds for physical MicroSD card (mmcblk1) detection */
    log_line("Waiting up to 5s for physical MicroSD card (mmcblk1)...\n");
    int card_found = 0;
    for (int i = 0; i < 50; i++) {
        scan_proc_partitions_and_create_nodes();
        if (is_mmcblk1_in_proc_partitions()) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Physical MicroSD detected after %d ms!\n", (i + 1) * 100);
            log_line(msg);
            /* Wait 200ms for kernel partition table scanner to register mmcblk1p1 */
            usleep(200000);
            scan_proc_partitions_and_create_nodes();
            card_found = 1;
            break;
        }
        usleep(100000); /* 100ms */
    }

    if (!card_found) {
        log_line("Notice: MicroSD card not detected, displaying sdcard.png...\n");
        display_screen("sdcard");
    }

    /* 6. Mount physical MicroSD, secondary partitions, USB storage, and setup symlinks */
    scan_and_mount_storage(1);

    /* 7. Apply aggressive battery and power saving optimizations */
    optimize_power_and_battery();

    /* 8. Hardware initialization & USB Gadget */
    enable_touch_keys();
    unmute_audio();
    setup_usb_gadget(active_sd_dev[0] ? active_sd_dev : NULL,
                     active_ext_dev[0] ? active_ext_dev : NULL);

    /* 8. Setup /.rockbox directory: strictly require .rockbox on MicroSD */
    int rockbox_found = 0;
    const char *last_screen = card_found ? "boot" : "sdcard";

    /* Start background automounter daemon for hotplugging */
    pid_t am_pid = fork();
    if (am_pid == 0) {
        while (1) {
            sleep(2);
            scan_and_mount_storage(0);
        }
        _exit(0);
    }

    while (1) {
        while (!rockbox_found) {
            write_sysfs("/proc/sys/vm/drop_caches", "3\n");
            scan_and_mount_storage(0);

            struct stat st;
            if (stat("/mnt/sdcard/.rockbox", &st) == 0) {
                unlink("/.rockbox");
                if (symlink("/mnt/sdcard/.rockbox", "/.rockbox") == 0) {
                    log_line("Linked /.rockbox -> /mnt/sdcard/.rockbox\n");
                }
                rockbox_found = 1;
                break;
            }
            if (stat("/mnt/extsd/.rockbox", &st) == 0) {
                unlink("/.rockbox");
                if (symlink("/mnt/extsd/.rockbox", "/.rockbox") == 0) {
                    log_line("Linked /.rockbox -> /mnt/extsd/.rockbox\n");
                }
                rockbox_found = 1;
                break;
            }

            /* If no SD card detected or .rockbox missing: strictly show sdcard.png */
            if (strcmp(last_screen, "sdcard") != 0) {
                log_line("WARNING: Physical MicroSD /.rockbox directory not found!\n");
                log_line("Displaying sdcard.png. Insert card or copy .rockbox.\n");
                display_screen("sdcard");
                last_screen = "sdcard";
            }
            sleep(1);
        }

        /* Once issues are resolved, continue operation */
        log_line("Rockbox system files present. Continuing operation...\n");
        display_screen("boot");
        last_screen = "boot";

        /* Link all MicroSD user files/directories to root / for plugins */
        link_sdcard_to_root();

        /* Launch Rockbox executable */
        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
        setenv("HOME", "/mnt/sdcard", 1);
        setenv("TERM", "linux", 1);

        log_line("Launching Rockbox application (/rockbox)...\n");
        if (log_fd >= 0) fsync(log_fd);

        pid_t rb_pid = fork();
        if (rb_pid == 0) {
            execl("/rockbox", "/rockbox", NULL);
            execl("/bin/rockbox", "/bin/rockbox", NULL);
            _exit(127);
        }

        int crashed = 0;
        if (rb_pid > 0) {
            int status = 0;
            waitpid(rb_pid, &status, 0);

            char crash_msg[128];
            if (WIFSIGNALED(status)) {
                crashed = 1;
                snprintf(crash_msg, sizeof(crash_msg), "CRITICAL: Rockbox crashed with signal %d!\n", WTERMSIG(status));
            } else if (WEXITSTATUS(status) != 0) {
                crashed = 1;
                snprintf(crash_msg, sizeof(crash_msg), "CRITICAL: Rockbox exited with status %d!\n", WEXITSTATUS(status));
            } else {
                log_line("Rockbox exited normally.\n");
            }
            log_line(crash_msg);
            if (log_fd >= 0) fsync(log_fd);
        } else {
            crashed = 1;
            log_line("CRITICAL: Failed to fork Rockbox process!\n");
        }

        /* Outside of Rockbox:
         * Re-bind USB gadget so user can recover via PC */
        setup_usb_gadget(active_sd_dev[0] ? active_sd_dev : NULL,
                         active_ext_dev[0] ? active_ext_dev : NULL);

        /* Recovery wait loop:
         * - If critical error & USB connected to PC: errconnect.png
         * - If plugged into PC outside Rockbox: connect.png
         * - If general error without PC: error.png
         * - If .rockbox missing: sdcard.png
         * - When issues are resolved, seamlessly continue operation! */
        while (1) {
            write_sysfs("/proc/sys/vm/drop_caches", "3\n");
            scan_and_mount_storage(0);

            int usb_to_pc = 0;
            int ufd = open("/sys/class/android_usb/android0/state", O_RDONLY);
            if (ufd >= 0) {
                char usb_state[32] = {0};
                ssize_t rb = read(ufd, usb_state, sizeof(usb_state) - 1);
                close(ufd);
                if (rb > 0) {
                    usb_state[rb] = '\0';
                    if (strstr(usb_state, "CONFIGURED") || strstr(usb_state, "CONNECTED")) {
                        usb_to_pc = 1;
                    }
                }
            }

            struct stat st;
            int rb_avail = (stat("/mnt/sdcard/.rockbox", &st) == 0 ||
                            stat("/mnt/extsd/.rockbox", &st) == 0);

            if (crashed) {
                if (usb_to_pc) {
                    if (strcmp(last_screen, "errconnect") != 0) {
                        display_screen("errconnect");
                        last_screen = "errconnect";
                    }
                } else {
                    if (strcmp(last_screen, "error") != 0) {
                        display_screen("error");
                        last_screen = "error";
                    }
                }
            } else if (usb_to_pc) {
                if (strcmp(last_screen, "connect") != 0) {
                    display_screen("connect");
                    last_screen = "connect";
                }
            } else if (!rb_avail) {
                if (strcmp(last_screen, "sdcard") != 0) {
                    display_screen("sdcard");
                    last_screen = "sdcard";
                }
            }

            /* If issues resolved and not currently connected to PC, continue operation! */
            if (rb_avail && !usb_to_pc) {
                log_line("Issues resolved! Continuing operation...\n");
                rockbox_found = 1;
                crashed = 0;
                sleep(1);
                break;
            }

            sleep(2);
        }
    }

    return 0;
}
