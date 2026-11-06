/*
 * init.c - Production PID 1 Init for Innioasis Y1 Hosted Rockbox Linux
 *
 * Provides:
 *  - Devtmpfs and device node auto-discovery (/dev/graphics/fb0 <-> /dev/fb0)
 *  - Dynamic Physical MicroSD discovery via /proc/partitions (waiting up to 5s for SDXC)
 *  - Exclusive Physical SD card primary mounting (eMMC virtual SD is excluded per design)
 *  - Multi-partition physical MicroSD mounting at boot
 *  - USB Mass Storage & LUN initialization for PC music transfer (Windows/Mac/Linux)
 *  - Click wheel touch scanning activation (/proc/tpd_keys_enable)
 *  - Optional event, CPU-process, and battery diagnostics to /mnt/sdcard/y1_debug.txt
 *    and /dev/kmsg
 *  - Hardware Framebuffer unblanking & visual splash verification
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
#include <sys/reboot.h>
#include <sys/sysmacros.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <dirent.h>
#include <sys/wait.h>

#ifndef VESA_NO_BLANKING
#define VESA_NO_BLANKING 0
#endif

static char active_sd_dev[64] = {0};
static char active_ext_dev[64] = {0};
static char early_log_buf[8192];
static size_t early_log_len;
static void log_line(const char *msg);
static int is_mounted(const char *target);

static int logging_enabled_from_config(void)
{
    FILE *fp = fopen("/mnt/sdcard/.rockbox/config.cfg", "r");
    if (!fp)
        return 0;

    char line[128];
    char value[16];
    int enabled = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "y1 logging: %15s", value) == 1) {
            enabled = strcmp(value, "on") == 0;
            break;
        }
    }
    fclose(fp);
    return enabled;
}

static int read_text_file(const char *path, char *buffer, size_t size)
{
    if (size == 0)
        return 0;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        buffer[0] = '\0';
        return 0;
    }

    int ok = fgets(buffer, size, fp) != NULL;
    fclose(fp);
    if (!ok) {
        buffer[0] = '\0';
        return 0;
    }

    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 1;
}

struct process_cpu_sample {
    int pid;
    unsigned long long ticks;
    char name[32];
};

#define MAX_DIAGNOSTIC_PROCESSES 512
#define TOP_DIAGNOSTIC_PROCESSES 5

static size_t sample_process_cpu(struct process_cpu_sample *samples,
                                size_t max_samples)
{
    DIR *proc = opendir("/proc");
    if (!proc)
        return 0;

    size_t count = 0;
    struct dirent *entry;
    while (count < max_samples && (entry = readdir(proc)) != NULL) {
        char *end = NULL;
        long pid = strtol(entry->d_name, &end, 10);
        if (!entry->d_name[0] || !end || *end || pid <= 0)
            continue;

        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;

        char line[1024];
        if (!fgets(line, sizeof(line), fp)) {
            fclose(fp);
            continue;
        }
        fclose(fp);

        char *name_start = strchr(line, '(');
        char *name_end = strrchr(line, ')');
        if (!name_start || !name_end || name_end <= name_start)
            continue;

        size_t name_len = (size_t)(name_end - name_start - 1);
        if (name_len >= sizeof(samples[count].name))
            name_len = sizeof(samples[count].name) - 1;
        memcpy(samples[count].name, name_start + 1, name_len);
        samples[count].name[name_len] = '\0';

        char *cursor = name_end + 1;
        while (*cursor == ' ')
            cursor++;

        unsigned long long user_ticks = 0;
        unsigned long long system_ticks = 0;
        int field = 3;
        char *saveptr = NULL;
        char *token = strtok_r(cursor, " ", &saveptr);
        while (token && field <= 15) {
            if (field == 14)
                user_ticks = strtoull(token, NULL, 10);
            else if (field == 15)
                system_ticks = strtoull(token, NULL, 10);
            token = strtok_r(NULL, " ", &saveptr);
            field++;
        }
        if (field <= 15)
            continue;

        samples[count].pid = (int)pid;
        samples[count].ticks = user_ticks + system_ticks;
        count++;
    }

    closedir(proc);
    return count;
}

static int previous_process_sample(const struct process_cpu_sample *samples,
                                   size_t count, int pid,
                                   unsigned long long *ticks)
{
    for (size_t i = 0; i < count; i++) {
        if (samples[i].pid == pid) {
            *ticks = samples[i].ticks;
            return 1;
        }
    }
    return 0;
}

static void log_battery_diagnostics(void)
{
    DIR *supplies = opendir("/sys/class/power_supply");
    if (!supplies) {
        log_line("[diag] Battery telemetry unavailable: no power_supply sysfs\n");
        return;
    }

    struct dirent *entry;
    int found_battery = 0;
    while ((entry = readdir(supplies)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        char path[320];
        char type[32];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type",
                 entry->d_name);
        if (!read_text_file(path, type, sizeof(type)) ||
            strcmp(type, "Battery") != 0)
            continue;

        char capacity[32] = "n/a";
        char voltage[32] = "n/a";
        char current[32] = "n/a";
        char temperature[32] = "n/a";
        char status[32] = "n/a";
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity",
                 entry->d_name);
        read_text_file(path, capacity, sizeof(capacity));
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/voltage_now",
                 entry->d_name);
        read_text_file(path, voltage, sizeof(voltage));
        if (voltage[0] == '\0') {
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/batt_vol",
                     entry->d_name);
            read_text_file(path, voltage, sizeof(voltage));
        }
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/current_now",
                 entry->d_name);
        read_text_file(path, current, sizeof(current));
        if (current[0] == '\0') {
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/batt_current",
                     entry->d_name);
            read_text_file(path, current, sizeof(current));
        }
        if (current[0] == '\0') {
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/BatteryAverageCurrent",
                     entry->d_name);
            read_text_file(path, current, sizeof(current));
        }
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/temp",
                 entry->d_name);
        read_text_file(path, temperature, sizeof(temperature));
        if (temperature[0] == '\0') {
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/batt_temp",
                     entry->d_name);
            read_text_file(path, temperature, sizeof(temperature));
        }
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status",
                 entry->d_name);
        read_text_file(path, status, sizeof(status));

        char message[256];
        snprintf(message, sizeof(message),
                 "[diag] battery=%s%% voltage_now=%s_uV current_now=%s_uA "
                 "temp_raw=%s status=%s\n",
                 capacity, voltage, current, temperature, status);
        log_line(message);
        found_battery = 1;
    }
    closedir(supplies);

    if (!found_battery)
        log_line("[diag] Battery telemetry unavailable: battery supply not found\n");

    char cpu_freq[32] = "n/a";
    char load[96] = "n/a";
    read_text_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        cpu_freq, sizeof(cpu_freq));
    read_text_file("/proc/loadavg", load, sizeof(load));
    char message[192];
    snprintf(message, sizeof(message),
             "[diag] cpu0_freq=%s_kHz loadavg=%s\n", cpu_freq, load);
    log_line(message);
}

static void diagnostic_monitor(void)
{
    struct process_cpu_sample previous[MAX_DIAGNOSTIC_PROCESSES];
    struct process_cpu_sample current[MAX_DIAGNOSTIC_PROCESSES];
    size_t previous_count = sample_process_cpu(previous,
                                                MAX_DIAGNOSTIC_PROCESSES);
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    if (ticks_per_second <= 0)
        ticks_per_second = 100;

    while (1) {
        if (!logging_enabled_from_config()) {
            /* Logging disabled: exit telemetry monitor process to eliminate any MicroSD wakeups */
            break;
        }

        sleep(60);
        if (!logging_enabled_from_config())
            break;

        size_t current_count = sample_process_cpu(current,
                                                  MAX_DIAGNOSTIC_PROCESSES);
        log_battery_diagnostics();

        struct process_cpu_sample top[TOP_DIAGNOSTIC_PROCESSES] = {0};
        unsigned long long top_delta[TOP_DIAGNOSTIC_PROCESSES] = {0};
        for (size_t i = 0; i < current_count; i++) {
            unsigned long long old_ticks;
            if (!previous_process_sample(previous, previous_count,
                                         current[i].pid, &old_ticks) ||
                current[i].ticks < old_ticks)
                continue;

            unsigned long long delta = current[i].ticks - old_ticks;
            for (size_t rank = 0; rank < TOP_DIAGNOSTIC_PROCESSES; rank++) {
                if (delta <= top_delta[rank])
                    continue;
                for (size_t move = TOP_DIAGNOSTIC_PROCESSES - 1;
                     move > rank; move--) {
                    top_delta[move] = top_delta[move - 1];
                    top[move] = top[move - 1];
                }
                top_delta[rank] = delta;
                top[rank] = current[i];
                break;
            }
        }

        for (size_t i = 0; i < TOP_DIAGNOSTIC_PROCESSES; i++) {
            if (top_delta[i] == 0)
                continue;
            unsigned long long cpu_hundredths =
                top_delta[i] * 10000ULL /
                ((unsigned long long)ticks_per_second * 60ULL);
            char message[160];
            snprintf(message, sizeof(message),
                     "[diag] top_cpu_%zu pid=%d name=%s usage=%llu.%02llu%% "
                     "over_60s\n",
                     i + 1, top[i].pid, top[i].name,
                     cpu_hundredths / 100, cpu_hundredths % 100);
            log_line(message);
        }

        memcpy(previous, current,
               current_count * sizeof(struct process_cpu_sample));
        previous_count = current_count;
    }
}

static void start_diagnostic_monitor(void)
{
    if (!logging_enabled_from_config())
        return;

    pid_t monitor_pid = fork();
    if (monitor_pid == 0) {
        diagnostic_monitor();
        _exit(0);
    }
    if (monitor_pid < 0)
        log_line("[diag] Failed to start telemetry monitor\n");
    else
        log_line("[diag] CPU and battery telemetry enabled (60-second interval)\n");
}

static void log_line(const char *msg)
{
    if (!logging_enabled_from_config()) {
        size_t len = strlen(msg);
        if (early_log_len + len < sizeof(early_log_buf)) {
            memcpy(early_log_buf + early_log_len, msg, len);
            early_log_len += len;
        }
        return;
    }

    size_t len = strlen(msg);

    if (is_mounted("/mnt/sdcard")) {
        int fd = open("/mnt/sdcard/y1_debug.txt",
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        if (fd >= 0) {
            if (early_log_len > 0) {
                (void)!write(fd, early_log_buf, early_log_len);
                early_log_len = 0;
            }
            (void)!write(fd, msg, len);
            close(fd);
        }
    }

    (void)!write(1, msg, len);
    int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0) {
        (void)!write(kfd, "<6>[init] ", 10);
        (void)!write(kfd, msg, len);
        close(kfd);
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

/* Check if physical MicroSD partition 1 (mmcblk1p1) is registered in /proc/partitions */
static int is_mmcblk1p1_in_proc_partitions(void)
{
    FILE *fp = fopen("/proc/partitions", "r");
    if (!fp) return 0;

    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "mmcblk1p1")) {
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

    /* MediaTek WMT / STP / Bluetooth / FM character device nodes */
    if (access("/dev/stpwmt", F_OK) != 0)  mknod("/dev/stpwmt",  S_IFCHR | 0666, makedev(190, 0));
    if (access("/dev/stpgps", F_OK) != 0)  mknod("/dev/stpgps",  S_IFCHR | 0666, makedev(191, 0));
    if (access("/dev/stpbt",  F_OK) != 0)  mknod("/dev/stpbt",   S_IFCHR | 0666, makedev(192, 0));
    if (access("/dev/fm",     F_OK) != 0)  mknod("/dev/fm",      S_IFCHR | 0666, makedev(193, 0));
    if (access("/dev/stpfm",  F_OK) != 0)  mknod("/dev/stpfm",   S_IFCHR | 0666, makedev(193, 0));
    if (access("/dev/wmtWifi", F_OK) != 0) mknod("/dev/wmtWifi", S_IFCHR | 0666, makedev(153, 0));
    if (access("/dev/urandom", F_OK) != 0) mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));

    /* Note: mmcblk1 and partition nodes are created dynamically via /proc/partitions */
}

static int try_mount_storage(const char *devpath, const char *target)
{
    mkdir(target, 0755);

    /* 1. Try VFAT (UTF-8, mixed shortnames, full executable permissions for plugins) */
    if (mount(devpath, target, "vfat", MS_NOATIME | MS_NODIRATIME, "utf8,shortname=mixed,umask=0000,fmask=0000,dmask=0000") == 0) {
        return 0;
    }
    if (mount(devpath, target, "vfat", MS_NOATIME | MS_NODIRATIME, "codepage=437,iocharset=utf8,shortname=mixed,umask=0000,fmask=0000,dmask=0000") == 0) {
        return 0;
    }
    if (mount(devpath, target, "vfat", MS_NOATIME | MS_NODIRATIME, "umask=0000,fmask=0000,dmask=0000") == 0) {
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

static int has_sdcard_rockbox(void)
{
    struct stat st;
    return is_mounted("/mnt/sdcard") &&
           stat("/mnt/sdcard/.rockbox", &st) == 0 &&
           S_ISDIR(st.st_mode) &&
           access("/mnt/sdcard/.rockbox/rockbox", X_OK) == 0;
}

/* Link all user folders and files from /mnt/sdcard into / so plugins and apps see / as SD root */
static void link_sdcard_to_root(void)
{
    static int sdcard_linked = 0;
    if (sdcard_linked)
        return;

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
            strcmp(de->d_name, "lib") == 0 || strcmp(de->d_name, "system") == 0 ||
            strcmp(de->d_name, "data") == 0)
            continue;

        char target[512], linkname[512];
        snprintf(target, sizeof(target), "/mnt/sdcard/%s", de->d_name);
        snprintf(linkname, sizeof(linkname), "/%s", de->d_name);

        char existing[512];
        ssize_t len = readlink(linkname, existing, sizeof(existing) - 1);
        if (len > 0) {
            existing[len] = '\0';
            if (strcmp(existing, target) == 0)
                continue; /* Link already points to target, avoid redundant write */
            unlink(linkname);
        } else {
            struct stat st;
            if (lstat(linkname, &st) == 0) {
                if (S_ISLNK(st.st_mode)) {
                    unlink(linkname);
                } else {
                    continue; /* don't remove existing real directory */
                }
            }
        }
        (void)!symlink(target, linkname);
    }
    closedir(d);
    sdcard_linked = 1;
}

/* Mount physical MicroSD storage once during boot (eMMC excluded). */
static void mount_storage_at_boot(int verbose)
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

        /* If mounted, setup standard compatibility symlinks. */
        if (is_mounted("/mnt/sdcard")) {
            mkdir("/storage", 0755);
            unlink("/storage/sdcard0");
            (void)!symlink("/mnt/sdcard", "/storage/sdcard0");
            unlink("/sdcard");
            (void)!symlink("/mnt/sdcard", "/sdcard");

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
}

static void boost_cpu_for_boot(void)
{
    /* 1. Power on secondary CPU core (CPU1) for fast multi-threaded boot and module loading */
    write_sysfs("/sys/devices/system/cpu/cpu1/online", "1\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_base", "2\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_limit", "2\n");

    /* 2. Boost both MT6572 Cortex-A7 cores to performance governor at maximum 1.196 GHz (1.2 GHz) */
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", "598000\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "1196000\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "performance\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed", "1196000\n");

    write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_min_freq", "598000\n");
    write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq", "1196000\n");
    write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_governor", "performance\n");
    write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_setspeed", "1196000\n");

    log_line("Dynamic boot-time CPU clock boosted to 1.2 GHz dual-core.\n");
}

static void apply_powersave_cpu(void)
{
    /* Used when Rockbox exits or crashes to conserve battery while waiting */
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "ondemand\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", "598000\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "1001000\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/up_threshold", "98\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/sampling_down_factor", "1\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/sampling_rate", "100000\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/io_is_busy", "0\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/ondemand/powersave_bias", "200\n");

    write_sysfs("/sys/devices/system/cpu/cpu1/online", "0\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_base", "1\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_limit", "1\n");
}

static void optimize_power_and_battery(void)
{
    log_line("Applying OS power and storage optimizations...\n");

    /* 1. Eliminate MediaTek proprietary USB clock override and PPM policies */
    write_sysfs("/proc/cpufreq/usb_raise_freq", "0\n");
    write_sysfs("/proc/cpufreq/MTK_PPM_POLICY", "DISABLE\n");
    write_sysfs("/proc/cpufreq/MTK_PPM_POLICY", "0\n");

    /* 2. Prevent MediaTek hotplug mechanisms from reviving Core 1 against user preference */
    write_sysfs("/proc/mtk_hotplug/enable", "0\n");
    write_sysfs("/sys/module/mt_hotplug_mechanism/parameters/g_enable", "0\n");
    write_sysfs("/sys/devices/system/cpu/cpufreq/hotplug/cpu_hotplug_disable", "1\n");

    /* 3. Enable MT6572 SPM idle modes at boot & configure dpidle power collapse */
    write_sysfs("/proc/spm_fs/dpidle", "1 cpu_pdn\n");
    write_sysfs("/proc/spm_fs/dpidle", "1 infra_pdn\n");
    write_sysfs("/sys/power/idle_state", "1\n");
    write_sysfs("/sys/power/dpidle_state", "cpupdn 1\n");
    /* Clear non-display peripheral clock bits from dpidle_condition_mask via
     * enable_dpidle_by_bit ('enable <clock_id>') so USB gadget / BTIF / PWM / APDMA
     * do not block SPM Deep Idle when the screen is off:
     * 34=APDMA, 48=PWM, 49=UART0, 50=UART1, 51=BTIF, 52=USB, 53=FHCTL */
    write_sysfs("/sys/power/dpidle_state", "enable 34\n");
    write_sysfs("/sys/power/dpidle_state", "enable 48\n");
    write_sysfs("/sys/power/dpidle_state", "enable 49\n");
    write_sysfs("/sys/power/dpidle_state", "enable 50\n");
    write_sysfs("/sys/power/dpidle_state", "enable 51\n");
    write_sysfs("/sys/power/dpidle_state", "enable 52\n");
    write_sysfs("/sys/power/dpidle_state", "enable 53\n");
    write_sysfs("/sys/power/dpidle_state", "1\n");
    write_sysfs("/sys/power/rgidle_state", "1\n");
    /* Hold a userspace wakelock so kernel autosleep does not freeze userspace
     * threads when we trigger early_suspend ('mem' -> /sys/power/state) */
    write_sysfs("/sys/power/wake_lock", "rockbox_active\n");

    /* 4. Linux Kernel Scheduler & VM dirty page flush tuning */
    write_sysfs("/proc/sys/kernel/sched_tunable_scaling", "0\n");
    write_sysfs("/proc/sys/kernel/sched_child_runs_first", "1\n");
    write_sysfs("/proc/sys/vm/swappiness", "0\n");
    write_sysfs("/proc/sys/vm/page-cluster", "0\n");
    write_sysfs("/proc/sys/vm/vfs_cache_pressure", "50\n");
    write_sysfs("/proc/sys/vm/min_free_kbytes", "1024\n");
    write_sysfs("/proc/sys/vm/dirty_background_ratio", "10\n");
    write_sysfs("/proc/sys/vm/dirty_ratio", "20\n");
    write_sysfs("/proc/sys/vm/dirty_writeback_centisecs", "60000\n");
    write_sysfs("/proc/sys/vm/dirty_expire_centisecs", "60000\n");
    write_sysfs("/proc/sys/vm/laptop_mode", "0\n");

    /* 5. Storage queue schedulers, burst-read ahead, and stop card polling wakeups */
    write_sysfs("/sys/block/mmcblk0/events_dfl_poll_msecs", "0\n");
    write_sysfs("/sys/block/mmcblk1/events_dfl_poll_msecs", "0\n");
    write_sysfs("/sys/block/mmcblk1/queue/scheduler", "noop\n");
    write_sysfs("/sys/block/mmcblk1/queue/read_ahead_kb", "2048\n");
    write_sysfs("/sys/block/mmcblk0/queue/scheduler", "noop\n");
    write_sysfs("/sys/block/mmcblk0/queue/read_ahead_kb", "2048\n");
    write_sysfs("/sys/devices/platform/mtk-msdc.0/power/control", "auto\n");
    write_sysfs("/sys/devices/platform/mtk-msdc.1/power/control", "auto\n");
    write_sysfs("/sys/devices/platform/mtk-msdc.0/power/autosuspend_delay_ms", "100\n");
    write_sysfs("/sys/devices/platform/mtk-msdc.1/power/autosuspend_delay_ms", "100\n");
    write_sysfs("/sys/block/mmcblk0/power/control", "auto\n");
    write_sysfs("/sys/block/mmcblk1/power/control", "auto\n");
    write_sysfs("/sys/block/mmcblk0/power/autosuspend_delay_ms", "100\n");
    write_sysfs("/sys/block/mmcblk1/power/autosuspend_delay_ms", "100\n");
    write_sysfs("/sys/devices/platform/mt_usb/power/control", "auto\n");

    /* 6. RAM & Peripheral Bus: Force MediaTek SMI Eco Mode & DRAM Dynamic Frequency Scaling */
    write_sysfs("/proc/mtk_smi/eco_mode", "1\n");
    write_sysfs("/proc/dispsys/eco_mode", "1\n");
    write_sysfs("/sys/module/mtk_smi/parameters/eco_mode", "1\n");
    write_sysfs("/sys/module/mtk_smi/parameters/bwc_enable", "1\n");
    write_sysfs("/sys/devices/platform/mt_emi_ctrl/con_emi_clk_drop", "1\n");
    write_sysfs("/sys/devices/platform/mt_emi_ctrl/force_dram_dfs", "1\n");
    write_sysfs("/proc/cpufreq/MTK_VCORE_DVFS", "0\n");
    write_sysfs("/sys/kernel/debug/vcore_dvfs/vcore_dvfs_force_opp", "0\n");

    /* 7. Power-gate unused Mali-400 GPU & lock clocks to lowest step */
    write_sysfs("/sys/devices/platform/mt_gpu/power/control", "auto\n");
    write_sysfs("/sys/devices/platform/mali.0/power/control", "auto\n");
    write_sysfs("/sys/devices/platform/mt_gpu/power/autosuspend_delay_ms", "50\n");
    write_sysfs("/sys/devices/platform/mali.0/power/autosuspend_delay_ms", "50\n");
    write_sysfs("/sys/class/misc/mali0/device/power_policy", "coarse_demand\n");
    write_sysfs("/sys/module/mali/parameters/mali_core_power_tweak", "1\n");
    write_sysfs("/sys/module/mali/parameters/mali_dvfs_control", "0\n");
    write_sysfs("/sys/class/misc/mali0/device/clock", "104\n");
    write_sysfs("/sys/class/misc/mali0/device/max_freq", "104000\n");

    /* 8. MediaTek Dynamic Clock Management (DCM) & Low Power Mode (LPM) */
    write_sysfs("/proc/dcm/dcm_enable", "1\n");
    write_sysfs("/proc/lpm/lpm_enable", "1\n");
    write_sysfs("/proc/spm_fs/dpidle", "1 cpu_pdn\n");
    write_sysfs("/proc/spm_fs/dpidle", "1 infra_pdn\n");
    write_sysfs("/proc/spm_fs/mcdi", "1\n");

    /* 9. Suppress kernel console wakeups, printk spam & watchdog timers */
    write_sysfs("/proc/sys/kernel/printk", "1 1 1 1\n");
    write_sysfs("/proc/sys/kernel/watchdog", "0\n");
    write_sysfs("/proc/sys/kernel/nmi_watchdog", "0\n");
    write_sysfs("/proc/timer_stats", "0\n");
    write_sysfs("/proc/mtk_printk_ctrl", "0\n");
    write_sysfs("/sys/module/printk/parameters/always_kmsg_dump", "0\n");
    write_sysfs("/sys/devices/platform/mtk-uart.0/power/control", "auto\n");

    /* 10. Ensure unused wireless/connectivity chips remain unpowered */
    write_sysfs("/sys/class/rfkill/rfkill0/state", "0\n");
    write_sysfs("/sys/class/rfkill/rfkill1/state", "0\n");

    /* 11. Disable haptic vibrator wakeups & enable hardware keypad wakeup */
    write_sysfs("/sys/devices/platform/mtk-kpd/power/wakeup", "enabled\n");

    log_line("Battery optimizations active: CPU 598MHz/single-core, SPM idle modes enabled.\n");
}

static char current_displayed_screen[32] = {0};

static void display_screen(const char *name)
{
    if (name && current_displayed_screen[0] && strcmp(current_displayed_screen, name) == 0) {
        return; /* Already displaying this exact splash screen, skip redundant FB write! */
    }

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
    if (name)
        snprintf(current_displayed_screen, sizeof(current_displayed_screen), "%s", name);
}

#include <sys/syscall.h>
#include <poll.h>

#ifndef LINUX_REBOOT_MAGIC1
#define LINUX_REBOOT_MAGIC1 0xfee1dead
#endif
#ifndef LINUX_REBOOT_MAGIC2
#define LINUX_REBOOT_MAGIC2 672274793
#endif
#ifndef LINUX_REBOOT_CMD_RESTART2
#define LINUX_REBOOT_CMD_RESTART2 0xa1b2c3d4
#endif

static void clear_android_bcb(void);
static void setup_usb_gadget(const char *sd_dev, const char *ext_dev);

static void clear_android_bcb(void)
{
    const char *misc_paths[] = {
        "/dev/block/platform/mtk-msdc.0/by-name/para",
        "/dev/block/platform/mtk-msdc.0/by-name/misc",
        "/dev/misc",
        "/dev/para",
        NULL
    };
    for (int i = 0; misc_paths[i]; i++) {
        int fd = open(misc_paths[i], O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            char bcb[2048];
            memset(bcb, 0, sizeof(bcb));
            (void)!write(fd, bcb, sizeof(bcb));
            close(fd);
            sync();
        }
    }
}

static void boot_into_android(void)
{
    log_line("Bootloader Menu: Booting Android...\n");
    clear_android_bcb();

    /* Check for Android stock ramdisk archive */
    const char *ramdisk_paths[] = {
        "/mnt/sdcard/.rockbox/ramdisk_android.cpio.gz",
        "/mnt/sdcard/.rockbox/ramdisk.cpio.gz",
        "/etc/ramdisk_android.cpio.gz",
        NULL
    };
    const char *ramdisk_archive = NULL;
    for (int i = 0; ramdisk_paths[i]; i++) {
        if (access(ramdisk_paths[i], R_OK) == 0) {
            ramdisk_archive = ramdisk_paths[i];
            break;
        }
    }

    if (ramdisk_archive) {
        /* Copy archive to /tmp so we can safely unmount /mnt/sdcard before extracting */
        mkdir("/tmp", 0755);
        int sfd = open(ramdisk_archive, O_RDONLY | O_CLOEXEC);
        int dfd = open("/tmp/android_ramdisk.cpio.gz", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (sfd >= 0 && dfd >= 0) {
            char cpbuf[16384];
            ssize_t r;
            while ((r = read(sfd, cpbuf, sizeof(cpbuf))) > 0) {
                (void)!write(dfd, cpbuf, r);
            }
            ramdisk_archive = "/tmp/android_ramdisk.cpio.gz";
        }
        if (sfd >= 0) close(sfd);
        if (dfd >= 0) close(dfd);
    } else {
        log_line("Notice: Android ramdisk archive not found.\n");
    }

    log_line("Extracting Android ramdisk and handing over to Android /init...\n");
    sync();

    /* 1. Detach SD card and secondary partitions so Android vold can claim them cleanly */
    umount2("/mnt/extsd", MNT_DETACH);
    umount2("/mnt/sdcard", MNT_DETACH);

    /* 2. Remove all user-space symlinks in / created by link_sdcard_to_root() */
    DIR *d = opendir("/");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char path[512];
            snprintf(path, sizeof(path), "/%s", de->d_name);
            struct stat st;
            if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
                char target[512];
                ssize_t len = readlink(path, target, sizeof(target) - 1);
                if (len > 0) {
                    target[len] = '\0';
                    /* Remove symlinks pointing into /mnt or /storage, or specific SD card paths */
                    if (strncmp(target, "/mnt/", 5) == 0 ||
                        strncmp(target, "/storage/", 9) == 0 ||
                        strcmp(de->d_name, "sdcard") == 0 ||
                        strcmp(de->d_name, ".rockbox") == 0) {
                        unlink(path);
                    }
                }
            }
        }
        closedir(d);
    }
    unlink("/sdcard");
    unlink("/storage/sdcard0");
    unlink("/storage/sdcard1");
    unlink("/.rockbox");

    /* 3. Clean up conflicting root directories before extracting stock Android ramdisk.
     * /etc MUST NOT exist as a directory, otherwise Android's init.rc 'symlink /system/etc /etc'
     * fails with EEXIST, causing vold to fail loading /etc/vold.fstab and crashing SystemServer! */
    (void)!system("/bin/busybox rm -rf /etc");

    /* Remove sbin so Android's sbin (adbd, e2fsck, meta_tst, tune2fs, ueventd, watchdogd) extracts cleanly */
    (void)!system("/bin/busybox rm -rf /sbin");

    /* Clean /mnt and /storage so Android init's 'symlink /storage/sdcard0 /mnt/sdcard' doesn't collide
     * with the real /mnt/sdcard directory created during Nugget Linux boot */
    (void)!system("/bin/busybox rm -rf /mnt /storage /sdcard");
    mkdir("/mnt", 0775);
    mkdir("/storage", 0055);

    /* Replace our PID 1 /init binary prior to extraction so busybox cpio can write Android's /init */
    rename("/init", "/init.rockbox");

    /* 4. Extract stock Android ramdisk unconditionally (-i -d -u) */
    if (ramdisk_archive) {
        char cmd[512];
        /* Note: -u forces unconditional overwrite; preserving kernel-created /emmc@* symlinks */
        snprintf(cmd, sizeof(cmd),
                 "/bin/busybox gzip -dc '%s' | (cd / && /bin/busybox cpio -idu)",
                 ramdisk_archive);
        (void)!system(cmd);
        unlink("/tmp/android_ramdisk.cpio.gz");
    }
    unlink("/init.rockbox");

    /* Remove lingering temporary files & unused Nugget rootfs directories */
    (void)!system("/bin/busybox rm -rf /tmp /usr /var");

    /* Guarantee /etc is NOT a directory (Stock ramdisk creates /etc via symlink to /system/etc) */
    struct stat etc_st;
    if (lstat("/etc", &etc_st) == 0 && S_ISDIR(etc_st.st_mode)) {
        (void)!system("/bin/busybox rm -rf /etc");
    }

    /* 5. Unmount virtual filesystems that Android's /init mounts itself in early-init */
    umount2("/dev/pts", MNT_DETACH);
    umount2("/dev", MNT_DETACH);
    umount2("/sys", MNT_DETACH);
    umount2("/proc", MNT_DETACH);

    /* Close all open file descriptors (except stdio) */
    for (int fd = 3; fd < 256; fd++) {
        close(fd);
    }

    /* Hand off PID 1 execution directly to stock Android /init */
    (void)!chdir("/");
    char *const init_argv[] = { "/init", NULL };
    char *const init_envp[] = { "PATH=/sbin:/vendor/bin:/system/sbin:/system/bin:/system/xbin", NULL };
    execve("/init", init_argv, init_envp);

    sleep(2);
}

static void reboot_to_android(void)
{
    boot_into_android();
}

static int is_menu_button_held_at_boot(void)
{
    int held = 0;
    uint8_t key_bits[KEY_MAX / 8 + 1];

    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        memset(key_bits, 0, sizeof(key_bits));
        if (ioctl(fd, EVIOCGKEY(sizeof(key_bits)), key_bits) >= 0) {
            /* KEY_BACK = 158, KEY_MENU = 139 */
            if ((key_bits[158 / 8] & (1 << (158 % 8))) ||
                (key_bits[139 / 8] & (1 << (139 % 8)))) {
                held = 1;
            }
        }

        struct input_event ev;
        while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY && (ev.code == 158 || ev.code == 139) && ev.value != 0) {
                held = 1;
            }
        }
        close(fd);
    }
    return held;
}

static int bootloader_menu_enabled_from_config(void)
{
    FILE *fp = fopen("/mnt/sdcard/.rockbox/config.cfg", "r");
    if (!fp)
        return 0;

    char line[128];
    char value[16];
    int enabled = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "bootloader menu: %15s", value) == 1) {
            enabled = (strcmp(value, "on") == 0);
            break;
        }
    }
    fclose(fp);
    return enabled;
}

static void run_bootloader_menu(void)
{
    log_line("Entering Bootloader Menu...\n");

    int event_fds[16];
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        event_fds[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }

    int selection = 0; /* 0 = Nugget, 1 = Android, 2 = Shut down, 3 = USB */
    const char *screens[4] = {"bootl_pick1", "bootl_pick2", "bootl_pick3", "bootl_pick4"};

    display_screen(screens[selection]);

    /* Debounce wait: wait 200ms before accepting confirmation */
    usleep(200000);

    /* Flush any pending press events before interactive loop */
    for (int i = 0; i < 16; i++) {
        if (event_fds[i] >= 0) {
            struct input_event ev;
            while (read(event_fds[i], &ev, sizeof(ev)) == sizeof(ev));
        }
    }

    while (1) {
        struct pollfd pfd[16];
        int map_idx[16];
        nfds_t n = 0;
        for (int i = 0; i < 16; i++) {
            if (event_fds[i] >= 0) {
                pfd[n].fd = event_fds[i];
                pfd[n].events = POLLIN;
                pfd[n].revents = 0;
                map_idx[n] = i;
                n++;
            }
        }

        if (poll(pfd, n, 100) > 0) {
            for (nfds_t i = 0; i < n; i++) {
                if (!(pfd[i].revents & POLLIN))
                    continue;

                struct input_event ev;
                while (read(event_fds[map_idx[i]], &ev, sizeof(ev)) == sizeof(ev)) {
                    if (ev.type != EV_KEY || ev.value != 1)
                        continue;

                    /* Previous / Left / Wheel CCW */
                    if (ev.code == 105 || ev.code == 165 || ev.code == 103) {
                        selection = (selection + 3) % 4;
                        display_screen(screens[selection]);
                    }
                    /* Next / Right / Wheel CW */
                    else if (ev.code == 106 || ev.code == 163 || ev.code == 108) {
                        selection = (selection + 1) % 4;
                        display_screen(screens[selection]);
                    }
                    /* Confirm: Center button or Play button */
                    else if (ev.code == 232 || ev.code == 28 || ev.code == 96 ||
                             ev.code == 352 || ev.code == 353 || ev.code == 314 ||
                             ev.code == 315 || ev.code == 304 || ev.code == 164 ||
                             ev.code == 207 || ev.code == 119) {

                        if (selection == 0) {
                            /* Pick 1: Nugget */
                            log_line("Bootloader Menu: Nugget selected.\n");
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) close(event_fds[k]);
                            }
                            display_screen("boot");
                            return;
                        } else if (selection == 1) {
                            /* Pick 2: Android */
                            log_line("Bootloader Menu: Android selected.\n");
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) close(event_fds[k]);
                            }
                            boot_into_android();
                        } else if (selection == 2) {
                            /* Pick 3: Shut down */
                            log_line("Bootloader Menu: Shut down selected.\n");
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) close(event_fds[k]);
                            }
                            sync();
                            reboot(RB_POWER_OFF);
                            while (1) sleep(1);
                        } else if (selection == 3) {
                            /* Pick 4: USB */
                            log_line("Bootloader Menu: USB mode selected.\n");
                            setup_usb_gadget(active_sd_dev[0] ? active_sd_dev : NULL,
                                             active_ext_dev[0] ? active_ext_dev : NULL);
                            display_screen("bootl_usb");

                            /* Flush input events and debounce */
                            usleep(300000);
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) {
                                    struct input_event flush_ev;
                                    while (read(event_fds[k], &flush_ev, sizeof(flush_ev)) == sizeof(flush_ev));
                                }
                            }

                            /* Wait for any button press to exit USB mode */
                            int exit_usb = 0;
                            while (!exit_usb) {
                                struct pollfd usb_pfd[16];
                                nfds_t un = 0;
                                int usb_map[16];
                                for (int k = 0; k < 16; k++) {
                                    if (event_fds[k] >= 0) {
                                        usb_pfd[un].fd = event_fds[k];
                                        usb_pfd[un].events = POLLIN;
                                        usb_pfd[un].revents = 0;
                                        usb_map[un] = k;
                                        un++;
                                    }
                                }
                                if (poll(usb_pfd, un, 100) > 0) {
                                    for (nfds_t k = 0; k < un; k++) {
                                        if (usb_pfd[k].revents & POLLIN) {
                                            struct input_event uev;
                                            while (read(event_fds[usb_map[k]], &uev, sizeof(uev)) == sizeof(uev)) {
                                                if (uev.type == EV_KEY && uev.value == 1) {
                                                    exit_usb = 1;
                                                    break;
                                                }
                                            }
                                        }
                                        if (exit_usb) break;
                                    }
                                }
                            }

                            /* Disable USB gadget */
                            log_line("Exiting USB mode, returning to Bootloader Menu...\n");
                            write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun0/file", "\n");
                            write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun/file", "\n");
                            write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "\n");
                            write_sysfs("/sys/class/android_usb/android0/enable", "0\n");
                            sync();

                            /* Debounce before returning to menu */
                            usleep(200000);
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) {
                                    struct input_event flush_ev;
                                    while (read(event_fds[k], &flush_ev, sizeof(flush_ev)) == sizeof(flush_ev));
                                }
                            }

                            display_screen(screens[selection]);
                        }
                    }
                }
            }
        }
    }
}

static int error_screen_center_pressed(void)
{
    static int event_fds[16];
    static int initialized = 0;
    struct pollfd poll_fds[16];
    int event_indices[16];
    nfds_t count = 0;

    if (!initialized) {
        for (int i = 0; i < 16; i++)
            event_fds[i] = -1;
        initialized = 1;
    }

    for (int i = 0; i < 16; i++) {
        if (event_fds[i] < 0) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            event_fds[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
        if (event_fds[i] >= 0) {
            poll_fds[count].fd = event_fds[i];
            poll_fds[count].events = POLLIN;
            poll_fds[count].revents = 0;
            event_indices[count] = i;
            count++;
        }
    }

    if (poll(poll_fds, count, 0) <= 0)
        return 0;

    for (nfds_t i = 0; i < count; i++) {
        if (!(poll_fds[i].revents & POLLIN))
            continue;

        struct input_event event;
        while (read(event_fds[event_indices[i]], &event, sizeof(event)) == sizeof(event)) {
            if (event.type == EV_KEY && event.value == 1 &&
                (event.code == 232 || event.code == KEY_ENTER ||
                 event.code == KEY_KPENTER || event.code == 352 ||
                 event.code == 353 || event.code == 314 ||
                 event.code == 315 || event.code == 304))
                return 1;
        }
    }

    return 0;
}

static int is_error_status_screen(const char *screen)
{
    return strcmp(screen, "sdcard") == 0 ||
           strcmp(screen, "error") == 0 ||
           strcmp(screen, "errconnect") == 0;
}

static void power_off_from_error_screen(void)
{
    log_line("Center button pressed on error screen; powering off.\n");
    sync();
    reboot(RB_POWER_OFF);
    log_line("ERROR: Device power-off failed.\n");
}

static int load_kernel_module(const char *path, const char *params)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }
    void *image = malloc(st.st_size);
    if (!image) {
        close(fd);
        return -1;
    }
    ssize_t rb = read(fd, image, st.st_size);
    close(fd);
    if (rb != st.st_size) {
        free(image);
        return -1;
    }
    long ret = syscall(__NR_init_module, image, (unsigned long)st.st_size, params ? params : "");
    free(image);
    return (ret == 0 || errno == EEXIST) ? 0 : -1;
}

#define WMT_IOCTL_SET_PATCH_NAME  0x4004a004
#define WMT_IOCTL_SET_STP_MODE    0x4004a005
#define WMT_IOCTL_PORT_NAME       0x4004a00d
#define WMT_IOCTL_SET_PATCH_NUM   0x4004a00e
#define WMT_IOCTL_SET_PATCH_INFO  0x4004a00f
#define WMT_IOCTL_GET_CHIP_INFO   0x8004a004
#define WMT_IOCTL_QUERY_CHIP_ID   0x8004a00c

struct wmt_patch_info {
    int download_seq;
    char addresse[4];
    char patch_name[256];
};

static void init_bluetooth_subsystem(void)
{
    /* Ensure /system/etc/firmware symlink points to /etc/firmware for mtk_stp_wmt.ko */
    mkdir("/system", 0755);
    mkdir("/system/etc", 0755);
    unlink("/system/etc/firmware");
    (void)!symlink("/etc/firmware", "/system/etc/firmware");
    mkdir("/data", 0777);

    if (load_kernel_module("/lib/modules/mtk_stp_wmt.ko", "") == 0) {
        log_line("Loaded MT6572 WMT combo driver (mtk_stp_wmt.ko).\n");
    } else if (load_kernel_module("/mnt/sdcard/.rockbox/bt/mtk_stp_wmt.ko", "") == 0) {
        log_line("Loaded MT6572 WMT combo driver from SD card.\n");
    }

    if (load_kernel_module("/lib/modules/mtk_stp_bt.ko", "") == 0) {
        log_line("Loaded MT6572 STP Bluetooth driver (mtk_stp_bt.ko).\n");
    } else if (load_kernel_module("/mnt/sdcard/.rockbox/bt/mtk_stp_bt.ko", "") == 0) {
        log_line("Loaded MT6572 STP Bluetooth driver from SD card.\n");
    }

    if (access("/dev/stpwmt", F_OK) != 0) mknod("/dev/stpwmt", S_IFCHR | 0666, makedev(190, 0));
    if (access("/dev/stpbt",  F_OK) != 0) mknod("/dev/stpbt",  S_IFCHR | 0666, makedev(192, 0));

    /* Fork background WMT firmware launcher daemon (replaces 6620_launcher -m 3 -p /etc/firmware) */
    pid_t wmt_pid = fork();
    if (wmt_pid == 0) {
        int wmt_fd = -1;
        for (int retry = 0; retry < 20; retry++) {
            wmt_fd = open("/dev/stpwmt", O_RDWR | O_NOCTTY);
            if (wmt_fd >= 0) break;
            usleep(200000);
        }
        if (wmt_fd < 0) _exit(1);

        ioctl(wmt_fd, WMT_IOCTL_SET_PATCH_NAME, "/etc/firmware");
        ioctl(wmt_fd, WMT_IOCTL_SET_STP_MODE, (3 & 0x0F) | 0x20); /* Mode 3: BTIF */
        ioctl(wmt_fd, WMT_IOCTL_PORT_NAME, 0);

        struct pollfd pfd;
        pfd.fd = wmt_fd;
        pfd.events = POLLIN;

        while (1) {
            int pr = poll(&pfd, 1, -1);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                char cmd[256] = {0};
                ssize_t rb = read(wmt_fd, cmd, 255);
                if (rb > 0 && strncmp(cmd, "srh_patch", 9) == 0) {
                    (void)ioctl(wmt_fd, WMT_IOCTL_QUERY_CHIP_ID, 0);
                    (void)ioctl(wmt_fd, WMT_IOCTL_GET_CHIP_INFO, 2);

                    const char *patch_path = "/etc/firmware/mt6572_82_patch_e1_0_hdr.bin";
                    int pfd_bin = open(patch_path, O_RDONLY);
                    if (pfd_bin >= 0) {
                        unsigned char hdr[28] = {0};
                        if (read(pfd_bin, hdr, sizeof(hdr)) == sizeof(hdr)) {
                            int patch_num = (hdr[24] >> 4) & 0x0F;
                            int dl_seq    = hdr[24] & 0x0F;
                            if (patch_num <= 0) patch_num = 1;
                            if (dl_seq <= 0) dl_seq = 1;

                            ioctl(wmt_fd, WMT_IOCTL_SET_PATCH_NUM, patch_num);

                            struct wmt_patch_info pinfo;
                            memset(&pinfo, 0, sizeof(pinfo));
                            pinfo.download_seq = dl_seq;
                            pinfo.addresse[0] = 0;
                            pinfo.addresse[1] = (char)hdr[25];
                            pinfo.addresse[2] = (char)hdr[26];
                            pinfo.addresse[3] = (char)hdr[27];
                            strncpy(pinfo.patch_name, patch_path, sizeof(pinfo.patch_name) - 1);

                            ioctl(wmt_fd, WMT_IOCTL_SET_PATCH_INFO, &pinfo);
                        }
                        close(pfd_bin);
                    }
                    (void)!write(wmt_fd, "ok", 2);
                }
            }
        }
        _exit(0);
    }
}

static void enable_touch_keys(void)
{
    int fd = open("/proc/tpd_keys_enable", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)!write(fd, "1\n", 2);
        close(fd);
        log_line("Touch keypad scanning enabled (/proc/tpd_keys_enable).\n");
    }
}

static int is_usb_power_online(void)
{
    char buf[16];
    if (read_text_file("/sys/class/power_supply/usb/online", buf, sizeof(buf)) && atoi(buf) > 0)
        return 1;
    if (read_text_file("/sys/class/power_supply/ac/online", buf, sizeof(buf)) && atoi(buf) > 0)
        return 1;
    return 0;
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
    /* Suppress MediaTek USB clock boost that locks CPU to 1.0GHz */
    write_sysfs("/proc/cpufreq/usb_raise_freq", "0\n");
    write_sysfs("/proc/cpufreq/MTK_PPM_POLICY", "DISABLE\n");
    write_sysfs("/proc/cpufreq/MTK_PPM_POLICY", "0\n");
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

    /* Boost CPU to 1.2 GHz Dual-Core immediately for maximum boot performance */
    boost_cpu_for_boot();

    mkdir("/dev", 0755);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) < 0) {
        mount("tmpfs", "/dev", "tmpfs", 0, NULL);
    }

    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);

    /* 2. Create device nodes & enable touch keys early */
    make_device_nodes();
    clear_android_bcb();
    enable_touch_keys();

    int menu_held = is_menu_button_held_at_boot();

    /* 3. Display hardware boot splash from boot.png immediately */
    display_screen("boot");

    /* 4. Prepare mount points */
    mkdir("/mnt", 0755);
    mkdir("/mnt/sdcard", 0755);
    mkdir("/mnt/extsd", 0755);

    log_line("=================================================\n");
    log_line("   Innioasis Y1 Hosted Rockbox Linux Boot       \n");
    log_line("=================================================\n");

    /* 5. Wait up to 5 seconds for physical MicroSD card (mmcblk1) detection with 10ms fast polling */
    log_line("Waiting up to 5s for physical MicroSD card (mmcblk1)...\n");
    int card_found = 0;
    for (int i = 0; i < 500; i++) {
        if (!menu_held && is_menu_button_held_at_boot()) {
            menu_held = 1;
        }
        scan_proc_partitions_and_create_nodes();
        if (is_mmcblk1_in_proc_partitions()) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Physical MicroSD detected after %d ms!\n", (i + 1) * 10);
            log_line(msg);

            /* Fast poll for kernel partition table scanner to register mmcblk1p1 */
            if (!is_mmcblk1p1_in_proc_partitions()) {
                for (int p = 0; p < 20; p++) {
                    usleep(10000); /* 10ms */
                    scan_proc_partitions_and_create_nodes();
                    if (is_mmcblk1p1_in_proc_partitions())
                        break;
                }
            } else {
                scan_proc_partitions_and_create_nodes();
            }
            card_found = 1;
            break;
        }
        usleep(10000); /* 10ms fast poll */
    }

    /* If MENU was held and card not found, allow user to boot Android or shut down */
    if (!card_found && menu_held) {
        run_bootloader_menu();
    }

    if (!card_found) {
        log_line("Notice: MicroSD card not detected, displaying sdcard.raw...\n");
        display_screen("sdcard");
    }

    /* 6. Mount physical MicroSD and its secondary partitions. */
    mount_storage_at_boot(1);

    /* Check for Rockbox Reboot to Android flag */
    if (access("/mnt/sdcard/.boot_android", F_OK) == 0) {
        unlink("/mnt/sdcard/.boot_android");
        sync();
        reboot_to_android();
    }

    /* Bootloader Menu check:
     * Holding MENU while booting will always show this menu, regardless of disablement.
     * Otherwise, shown if enabled in settings (bootloader menu: on). */
    if (menu_held || bootloader_menu_enabled_from_config()) {
        run_bootloader_menu();
    }

    /* 7. Apply aggressive battery and power saving optimizations */
    optimize_power_and_battery();
    start_diagnostic_monitor();

    /* 8. Hardware initialization, Bluetooth WMT/STP subsystem, & USB Gadget */
    enable_touch_keys();
    init_bluetooth_subsystem();
    if (is_usb_power_online() || !has_sdcard_rockbox()) {
        setup_usb_gadget(active_sd_dev[0] ? active_sd_dev : NULL,
                         active_ext_dev[0] ? active_ext_dev : NULL);
    } else {
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun0/file", "\n");
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun/file", "\n");
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "\n");
        write_sysfs("/sys/class/android_usb/android0/enable", "0\n");
    }

    /* 8. Setup /.rockbox directory: strictly require .rockbox on MicroSD */
    int rockbox_found = 0;
    int waited_for_rockbox = 0;
    const char *last_screen = card_found ? "boot" : "sdcard";

    while (1) {
        while (!rockbox_found) {
            if (has_sdcard_rockbox()) {
                char rb_link[256];
                ssize_t rblen = readlink("/.rockbox", rb_link, sizeof(rb_link) - 1);
                if (rblen <= 0 || strncmp(rb_link, "/mnt/sdcard/.rockbox", rblen) != 0) {
                    unlink("/.rockbox");
                    if (symlink("/mnt/sdcard/.rockbox", "/.rockbox") == 0) {
                        log_line("Linked /.rockbox -> /mnt/sdcard/.rockbox\n");
                    }
                }
                if (waited_for_rockbox) {
                    log_line("Rockbox installation detected. Rebooting device...\n");
                    sync();
                    reboot(RB_AUTOBOOT);
                    log_line("ERROR: Device reboot failed; remaining in recovery wait.\n");
                    while (1) sleep(1);
                }
                rockbox_found = 1;
                break;
            }

            waited_for_rockbox = 1;
            /* Require .rockbox on the mounted primary MicroSD before launching. */
            if (strcmp(last_screen, "sdcard") != 0) {
                log_line("WARNING: Primary MicroSD or its /.rockbox directory is missing!\n");
                log_line("Displaying sdcard.raw. Insert card or copy .rockbox.\n");
                display_screen("sdcard");
                last_screen = "sdcard";
            }
            if (is_error_status_screen(last_screen) && error_screen_center_pressed())
                power_off_from_error_screen();
            sleep(1);
        }

        /* Once issues are resolved, continue operation */
        log_line("Rockbox SD installation present. Continuing operation...\n");
        display_screen("boot");
        last_screen = "boot";

        /* Ensure default Videos directory exists on MicroSD card */
        mkdir("/mnt/sdcard/Videos", 0777);

        /* Link all MicroSD user files/directories to root / for plugins */
        link_sdcard_to_root();

        /* Launch Rockbox executable */
        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
        setenv("LD_LIBRARY_PATH", "/mnt/sdcard/.rockbox/lib:/lib:/usr/lib", 1);
        setenv("HOME", "/mnt/sdcard", 1);
        setenv("TERM", "linux", 1);

        log_line("Launching Rockbox application from the MicroSD card...\n");

        pid_t rb_pid = fork();
        if (rb_pid == 0) {
            execl("/mnt/sdcard/.rockbox/rockbox", "/mnt/sdcard/.rockbox/rockbox", NULL);
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
            if (crashed)
                log_line(crash_msg);
        } else {
            crashed = 1;
            log_line("CRITICAL: Failed to fork Rockbox process!\n");
        }

        /* Outside of Rockbox:
         * Drop CPU to powersave while waiting in recovery, re-bind USB gadget */
        apply_powersave_cpu();
        setup_usb_gadget(active_sd_dev[0] ? active_sd_dev : NULL,
                         active_ext_dev[0] ? active_ext_dev : NULL);

        /* Recovery wait loop:
         * - If critical error & USB connected to PC: errconnect.png
         * - If plugged into PC outside Rockbox: connect.png
         * - If general error without PC: error.png
         * - If .rockbox missing: sdcard.raw
         * - When issues are resolved, seamlessly continue operation! */
        while (1) {
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

            int rb_avail = has_sdcard_rockbox();

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
                boost_cpu_for_boot();
                rockbox_found = 1;
                crashed = 0;
                sleep(1);
                break;
            }

            if (is_error_status_screen(last_screen) && error_screen_center_pressed())
                power_off_from_error_screen();

            sleep(2);
        }
    }

    return 0;
}
