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
 *  - Optional event, CPU-process, and battery diagnostics to /mnt/sdcard/.system/y1_debug.txt
 *    and /dev/kmsg
 *  - Hardware Framebuffer unblanking & visual splash verification
 *  - Execution of /mnt/sdcard/.system/app or /mnt/sdcard/.rockbox/rockbox with error recovery
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/klog.h>

#ifndef VESA_NO_BLANKING
#define VESA_NO_BLANKING 0
#endif

#ifndef NUGGET_BUILD_DATE
#define NUGGET_BUILD_DATE __DATE__ " " __TIME__
#endif

#ifndef NUGGET_GIT_REV
#define NUGGET_GIT_REV "unknown"
#endif

static char active_sd_dev[64] = {0};
static char active_ext_dev[64] = {0};
static char early_log_buf[8192];
static size_t early_log_len;
static void log_line(const char *msg);
static int is_mounted(const char *target);

static int logging_enabled_from_config(void)
{
    const char *cfg_paths[] = {
        "/mnt/sdcard/.system/system.cfg",
        "/mnt/sdcard/.rockbox/config.cfg",
        NULL
    };

    for (int i = 0; cfg_paths[i]; i++) {
        FILE *fp = fopen(cfg_paths[i], "r");
        if (!fp)
            continue;

        char line[128];
        char value[16];
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "y1 logging: %15s", value) == 1) {
                fclose(fp);
                return strcmp(value, "on") == 0;
            }
        }
        fclose(fp);
    }
    return 0;
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

static int is_ipod_usb_mode_enabled(void);

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
        mkdir("/mnt/sdcard/.system", 0777);
        int fd = open("/mnt/sdcard/.system/y1_debug.txt",
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

static void write_sysfs_exact(const char *path, const void *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)!write(fd, buf, len);
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
    if (access("/dev/ttyMT0", F_OK) != 0) mknod("/dev/ttyMT0", S_IFCHR | 0600, makedev(204, 0));

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

static char custom_app_override[256] = {0};

static const char *get_sdcard_app_binary(void)
{
    if (!is_mounted("/mnt/sdcard"))
        return NULL;

    if (custom_app_override[0]) {
        static char sd_path[300];
        if (strncmp(custom_app_override, "/mnt/sdcard/", 12) == 0) {
            snprintf(sd_path, sizeof(sd_path), "%s", custom_app_override);
        } else if (custom_app_override[0] == '/') {
            snprintf(sd_path, sizeof(sd_path), "/mnt/sdcard%s", custom_app_override);
        } else {
            snprintf(sd_path, sizeof(sd_path), "/mnt/sdcard/%s", custom_app_override);
        }
        if (access(sd_path, F_OK) == 0) {
            chmod(sd_path, 0755);
            return sd_path;
        }
        /* Fallback to root symlink if pointing into SD card */
        if (access(custom_app_override, F_OK) == 0) {
            chmod(custom_app_override, 0755);
            return custom_app_override;
        }
        return NULL;
    }

    if (access("/mnt/sdcard/.system/app", X_OK) == 0)
        return "/mnt/sdcard/.system/app";

    /* Check configured default player if present */
    FILE *dp_fp = fopen("/mnt/sdcard/.system/default_player", "r");
    if (dp_fp) {
        char player[64] = {0};
        if (fgets(player, sizeof(player), dp_fp)) {
            char *p = player;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char *e = p + strlen(p) - 1;
            while (e >= p && (*e == '\r' || *e == '\n' || *e == ' ' || *e == '\t')) {
                *e = '\0';
                e--;
            }
            if (strcmp(p, "rockbox") == 0) {
                if (access("/mnt/sdcard/.rockbox/rockbox", F_OK) == 0) {
                    chmod("/mnt/sdcard/.rockbox/rockbox", 0755);
                    fclose(dp_fp);
                    return "/mnt/sdcard/.rockbox/rockbox";
                }
            }
        }
        fclose(dp_fp);
    }

    if (access("/mnt/sdcard/.rockbox/rockbox", X_OK) == 0 ||
        access("/mnt/sdcard/.rockbox/rockbox", F_OK) == 0) {
        chmod("/mnt/sdcard/.rockbox/rockbox", 0755);
        return "/mnt/sdcard/.rockbox/rockbox";
    }
    return NULL;
}

static int has_sdcard_rockbox(void)
{
    if (get_sdcard_app_binary() == NULL)
        return 0;

    struct stat st;
    if (stat("/mnt/sdcard/.system", &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;

    if (access("/mnt/sdcard/.system/lib/liby1.so", F_OK) != 0 &&
        access("/mnt/sdcard/.system/bt", F_OK) != 0)
        return 0;

    return 1;
}

/* Link all user folders and files from /mnt/sdcard into / so plugins and apps see / as SD root */
static void link_sdcard_to_root(void)
{
    static int sdcard_linked = 0;
    if (sdcard_linked)
        return;

    mkdir("/mnt/sdcard/.system", 0777);
    unlink("/.system");
    (void)!symlink("/mnt/sdcard/.system", "/.system");

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

    /* 2. Boost both MT6572 Cortex-A7 cores to performance governor at 1.209 GHz (1.2 GHz OPP) */
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", "390000\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "1209000\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "performance\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed", "1209000\n");

    write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_min_freq", "390000\n");
    write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq", "1209000\n");
    write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_governor", "performance\n");
    write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_setspeed", "1209000\n");

    log_line("Dynamic boot-time CPU clock boosted to 1.2 GHz dual-core.\n");
}

static void apply_powersave_cpu(void)
{
    /* Used when Rockbox exits or crashes to conserve battery while waiting */
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "ondemand\n");
    write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", "390000\n");
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
    write_sysfs("/proc/spm_fs/dpidle", "1 pwrlevel\n");
    write_sysfs("/proc/spm_fs/mcdi", "1\n");
    write_sysfs("/sys/power/dpidle_state", "time 0\n");

    /* 9. Suppress kernel console wakeups, printk spam & watchdog timers */
    write_sysfs("/proc/sys/kernel/printk", "1 1 1 1\n");
    write_sysfs("/proc/sys/kernel/watchdog", "0\n");
    write_sysfs("/proc/sys/kernel/nmi_watchdog", "0\n");
    write_sysfs("/proc/timer_stats", "0\n");
    write_sysfs("/proc/mtk_printk_ctrl", "0\n");
    write_sysfs("/proc/batdrv_log", "0\n");
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

        /* 2. Try SD card .system and .rockbox fallbacks */
        if (img_fd < 0) {
            snprintf(path, sizeof(path), "/.system/%s.raw", name);
            img_fd = open(path, O_RDONLY | O_CLOEXEC);
        }
        if (img_fd < 0) {
            snprintf(path, sizeof(path), "/mnt/sdcard/.system/%s.raw", name);
            img_fd = open(path, O_RDONLY | O_CLOEXEC);
        }
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
static void setup_usb_gadget(const char *sd_dev, const char *ext_dev, int allow_ipod);

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

    /* Check for Android stock ramdisk archive in .system first, then fallbacks */
    const char *ramdisk_paths[] = {
        "/mnt/sdcard/.system/ramdisk_android.cpio.gz",
        "/mnt/sdcard/.system/ramdisk.cpio.gz",
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
                        strcmp(de->d_name, ".system") == 0 ||
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
    unlink("/.system");
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
    const char *cfg_paths[] = {
        "/mnt/sdcard/.system/system.cfg",
        "/mnt/sdcard/.rockbox/config.cfg",
        NULL
    };

    for (int i = 0; cfg_paths[i]; i++) {
        FILE *fp = fopen(cfg_paths[i], "r");
        if (!fp)
            continue;

        char line[128];
        char value[16];
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "bootloader menu: %15s", value) == 1) {
                fclose(fp);
                return (strcmp(value, "on") == 0);
            }
        }
        fclose(fp);
    }
    return 0;
}

static void run_usb_serial_shell(const char *tty_name, int tab_num)
{
    char sys_path[64];
    snprintf(sys_path, sizeof(sys_path), "/sys/class/tty/%s/dev", tty_name);

    char dev_str[32] = {0};
    for (int r = 0; r < 50; r++) {
        if (read_text_file(sys_path, dev_str, sizeof(dev_str)) && dev_str[0])
            break;
        usleep(100000);
    }

    char dev_path[64];
    snprintf(dev_path, sizeof(dev_path), "/dev/%s", tty_name);

    int maj = 0, min = 0;
    if (sscanf(dev_str, "%d:%d", &maj, &min) == 2) {
        unlink(dev_path);
        mknod(dev_path, S_IFCHR | 0666, makedev(maj, min));
    } else {
        (void)!system("/bin/busybox mdev -s");
    }

    while (access("/tmp/exit_terminal", F_OK) != 0) {
        int fd = open(dev_path, O_RDWR);
        if (fd < 0) {
            usleep(250000);
            continue;
        }

        /* Configure line discipline for serial console terminal */
        struct termios tio;
        if (tcgetattr(fd, &tio) == 0) {
            tio.c_iflag |= (ICRNL | IXON);
            tio.c_oflag |= (OPOST | ONLCR);
            tio.c_lflag |= (ISIG | ICANON | ECHO | ECHOE | ECHOK);
            tcsetattr(fd, TCSANOW, &tio);
        }

        /* Immediate greeting banner upon COM port open */
        char banner[2048];
        snprintf(banner, sizeof(banner),
            "Nugget for Y1 terminal [Tab %d: /dev/%s]\r\n"
            "\r\n"
            "            ################################\r\n"
            "           ## ######################### ## ##\r\n"
            "           ## #                       # ## ##\r\n"
            "           ## #                       # ## ##\r\n"
            "           ## #      ##       ##      # ## ##\r\n"
            "           ## #                       # ## ##\r\n"
            "           ## #      ##       ##      # ## ##\r\n"
            "           ## #       #########       # ## ##\r\n"
            "           ## #                       # ## ##\r\n"
            "           ## ######################### ## ##\r\n"
            "           ##                           ## ##\r\n"
            "           ##         #########         ## ##\r\n"
            "           ##       ##         ##       ## ##\r\n"
            "           ##      ##   #####   ##      ## ##\r\n"
            "           ##     ##   #     #   ##     ## ##\r\n"
            "           ##     ##   #     #   ##     ## ##\r\n"
            "           ##      ##   #####   ##      ## ##\r\n"
            "           ##       ##         ##       ## ##\r\n"
            "           ##         #########         ## ##\r\n"
            "           ##                           ## ##\r\n"
            "            ################################\r\n"
            "                    #           #\r\n"
            "                    #           #\r\n"
            "                #####       #####\r\n"
            "\r\n"
            "         https://github.com/norocoral/nugget-y1\r\n"
            "       Welcome! Root shell active on /dev/%s [Tab %d].\r\n"
            "       * Tip: Open Tab 1 (/dev/ttyGS0) & Tab 2 (/dev/ttyGS1) concurrently.\r\n"
            "       * Type 'menu' to return to the Bootloader Menu.\r\n\r\n",
            tab_num, tty_name, tty_name, tab_num);

        (void)!write(fd, banner, strlen(banner));

        pid_t sh_pid = fork();
        if (sh_pid == 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
            setsid();
            ioctl(0, TIOCSCTTY, 1);

            setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/mnt/sdcard/.system/bin:/mnt/sdcard/.rockbox/bin:/mnt/sdcard/.meui/bin:/mnt/sdcard/.custom/bin", 1);
            setenv("LD_LIBRARY_PATH", "/mnt/sdcard/.meui/lib:/mnt/sdcard/.custom/lib:/mnt/sdcard/.system/lib:/mnt/sdcard/.rockbox/lib:/lib:/usr/lib", 1);
            setenv("HOME", "/mnt/sdcard", 1);
            setenv("TERM", "vt100", 1);
            setenv("USER", "root", 1);

            char ps1[32];
            if (tab_num > 1)
                snprintf(ps1, sizeof(ps1), "nugget-y1[tab%d]:# ", tab_num);
            else
                snprintf(ps1, sizeof(ps1), "nugget-y1:# ");
            setenv("PS1", ps1, 1);

            (void)!chdir("/mnt/sdcard");

            execl("/bin/sh", "sh", "-i", NULL);
            execl("/bin/busybox", "busybox", "sh", "-i", NULL);
            _exit(1);
        }

        close(fd);
        if (sh_pid > 0) {
            int status = 0;
            waitpid(sh_pid, &status, 0);
        }

        if (access("/tmp/exit_terminal", F_OK) == 0)
            break;

        /* If tab 1 exited normally via 'exit', request return to bootloader */
        if (tab_num == 1) {
            int ef = open("/tmp/exit_terminal", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (ef >= 0) close(ef);
            break;
        }

        /* For secondary tab, wait briefly before accepting next connection */
        usleep(300000);
    }
}

static int read_trimmed_line(const char *path, char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return -1;
    out[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, out, out_sz - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = '\0';
    return 0;
}

static void append_file_tail(FILE *out, const char *label, const char *path, size_t max_bytes)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        off_t offset = 0;
        size_t to_read = (size_t)st.st_size;
        if (to_read > max_bytes) {
            offset = st.st_size - (off_t)max_bytes;
            to_read = max_bytes;
        }
        if (offset > 0)
            lseek(fd, offset, SEEK_SET);
        char *buf = malloc(to_read + 1);
        if (buf) {
            ssize_t r = read(fd, buf, to_read);
            if (r > 0) {
                buf[r] = '\0';
                fprintf(out, "\n[%s (last %zd/%lld bytes)]\n%s%s",
                        label, r, (long long)st.st_size, buf,
                        (buf[r - 1] == '\n') ? "" : "\n");
            }
            free(buf);
        }
    }
    close(fd);
}

static void dump_mmc_host_info(FILE *out, const char *host)
{
    char host_dir[128];
    snprintf(host_dir, sizeof(host_dir), "/sys/class/mmc_host/%s", host);
    DIR *d = opendir(host_dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, host, strlen(host)) == 0 && strchr(de->d_name, ':')) {
            char p[256];
            char name[32] = "?", manf[16] = "?", oem[16] = "?", prv[16] = "?",
                 hw[16] = "?", fw[16] = "?", type[16] = "?";
            snprintf(p, sizeof(p), "%s/%s/name", host_dir, de->d_name);
            read_trimmed_line(p, name, sizeof(name));
            snprintf(p, sizeof(p), "%s/%s/manfid", host_dir, de->d_name);
            read_trimmed_line(p, manf, sizeof(manf));
            snprintf(p, sizeof(p), "%s/%s/oemid", host_dir, de->d_name);
            read_trimmed_line(p, oem, sizeof(oem));
            snprintf(p, sizeof(p), "%s/%s/prv", host_dir, de->d_name);
            read_trimmed_line(p, prv, sizeof(prv));
            snprintf(p, sizeof(p), "%s/%s/hwrev", host_dir, de->d_name);
            read_trimmed_line(p, hw, sizeof(hw));
            snprintf(p, sizeof(p), "%s/%s/fwrev", host_dir, de->d_name);
            read_trimmed_line(p, fw, sizeof(fw));
            snprintf(p, sizeof(p), "%s/%s/type", host_dir, de->d_name);
            read_trimmed_line(p, type, sizeof(type));
            fprintf(out, "%s(%s): type=%s name=%s manf=%s oem=%s prv=%s hw=%s fw=%s\n",
                    host, de->d_name, type, name, manf, oem, prv, hw, fw);
        }
    }
    closedir(d);
}

static void dump_dir_names(FILE *out, const char *label, const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (!d)
        return;
    fprintf(out, "%s=", label);
    struct dirent *de;
    int first = 1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        fprintf(out, "%s%s", first ? "" : ",", de->d_name);
        first = 0;
    }
    if (first)
        fprintf(out, "(empty)");
    fputc('\n', out);
    closedir(d);
}

static long file_kb(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)((st.st_size + 1023) / 1024);
}

static int write_compact_debug_info(char *saved_path, size_t saved_path_sz)
{
    if (saved_path && saved_path_sz > 0)
        saved_path[0] = '\0';

    time_t now = time(NULL);
    struct tm tm_now;
    char date_tag[32] = "unknown";
    char date_iso[32] = "unknown";
    if (localtime_r(&now, &tm_now)) {
        strftime(date_tag, sizeof(date_tag), "%Y%m%d_%H%M%S", &tm_now);
        strftime(date_iso, sizeof(date_iso), "%Y-%m-%d %H:%M:%S", &tm_now);
    }

    char out_path[128];
    snprintf(out_path, sizeof(out_path), "/mnt/sdcard/debuginfo%s.txt", date_tag);
    if (saved_path && saved_path_sz > 0)
        snprintf(saved_path, saved_path_sz, "%s", out_path);

    FILE *out = fopen(out_path, "w");
    if (!out) {
        log_line("Bootloader Menu: Failed to create debuginfo file on /mnt/sdcard.\n");
        return -1;
    }

    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    fprintf(out, "=== Nugget Y1 Debug Info ===\n");
    fprintf(out, "[build]\n");
    fprintf(out, "date=%s\n", date_iso);
    fprintf(out, "build_date=%s\n", NUGGET_BUILD_DATE);
    fprintf(out, "git_rev=%s\n", NUGGET_GIT_REV);
    fprintf(out, "init_compiled=%s %s\n", __DATE__, __TIME__);
    fprintf(out, "kernel=%s %s %s %s\n", uts.sysname, uts.release, uts.version, uts.machine);

    char buf[512];
    if (read_trimmed_line("/proc/cmdline", buf, sizeof(buf)) == 0)
        fprintf(out, "cmdline=%s\n", buf);

    /* Hardware variations (board/CPU/FB/MMC/input) */
    fprintf(out, "\n[hardware]\n");
    FILE *cpufp = fopen("/proc/cpuinfo", "r");
    if (cpufp) {
        char line[160];
        while (fgets(line, sizeof(line), cpufp)) {
            if (strncmp(line, "Processor", 9) == 0 ||
                strncmp(line, "Hardware", 8) == 0 ||
                strncmp(line, "Revision", 8) == 0 ||
                strncmp(line, "Serial", 6) == 0 ||
                strncmp(line, "BogoMIPS", 8) == 0) {
                fputs(line, out);
            }
        }
        fclose(cpufp);
    }

    int fbfd = open("/dev/graphics/fb0", O_RDONLY | O_CLOEXEC);
    if (fbfd >= 0) {
        struct fb_var_screeninfo vinfo;
        struct fb_fix_screeninfo finfo;
        if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
            ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == 0) {
            fprintf(out, "fb0=%s %ux%u@%ubpp stride=%u smem=%u\n",
                    finfo.id, vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
                    finfo.line_length, finfo.smem_len);
        }
        close(fbfd);
    }
    if (read_trimmed_line("/proc/lcm", buf, sizeof(buf)) == 0)
        fprintf(out, "lcm=%s\n", buf);

    dump_mmc_host_info(out, "mmc0");
    dump_mmc_host_info(out, "mmc1");

    FILE *ifp = fopen("/proc/bus/input/devices", "r");
    if (ifp) {
        char line[256];
        while (fgets(line, sizeof(line), ifp)) {
            if (strncmp(line, "I: ", 3) == 0 ||
                strncmp(line, "N: ", 3) == 0 ||
                strncmp(line, "P: ", 3) == 0 ||
                strncmp(line, "H: ", 3) == 0) {
                fputs(line, out);
            }
        }
        fclose(ifp);
    }

    /* System State */
    fprintf(out, "\n[system_state]\n");
    char uptime[64] = "?";
    read_trimmed_line("/proc/uptime", uptime, sizeof(uptime));
    char cpu0_freq[32] = "?", gov[32] = "?", cpu1_on[16] = "?";
    read_trimmed_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", cpu0_freq, sizeof(cpu0_freq));
    read_trimmed_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov, sizeof(gov));
    read_trimmed_line("/sys/devices/system/cpu/cpu1/online", cpu1_on, sizeof(cpu1_on));
    fprintf(out, "uptime=%s cpu0_khz=%s gov=%s cpu1_online=%s\n", uptime, cpu0_freq, gov, cpu1_on);

    FILE *mfp = fopen("/proc/meminfo", "r");
    if (mfp) {
        char line[128];
        while (fgets(line, sizeof(line), mfp)) {
            if (strncmp(line, "MemTotal:", 9) == 0 ||
                strncmp(line, "MemFree:", 8) == 0 ||
                strncmp(line, "MemAvailable:", 13) == 0) {
                fputs(line, out);
            }
        }
        fclose(mfp);
    }

    char cap[16] = "?", volt[32] = "?", curr[32] = "?", bstat[32] = "?", usb_on[16] = "?";
    read_trimmed_line("/sys/class/power_supply/battery/capacity", cap, sizeof(cap));
    read_trimmed_line("/sys/class/power_supply/battery/voltage_now", volt, sizeof(volt));
    read_trimmed_line("/sys/class/power_supply/battery/current_now", curr, sizeof(curr));
    read_trimmed_line("/sys/class/power_supply/battery/status", bstat, sizeof(bstat));
    read_trimmed_line("/sys/class/power_supply/usb/online", usb_on, sizeof(usb_on));
    fprintf(out, "battery=%s%% status=%s volt=%s curr=%s usb_online=%s\n",
            cap, bstat, volt, curr, usb_on);

    char wlock[128] = "?", h2w[16] = "?", tpd[16] = "?";
    read_trimmed_line("/sys/power/wake_lock", wlock, sizeof(wlock));
    read_trimmed_line("/sys/class/switch/h2w/state", h2w, sizeof(h2w));
    read_trimmed_line("/proc/tpd_keys_enable", tpd, sizeof(tpd));
    fprintf(out, "wake_lock=[%s] h2w=%s tpd_keys=%s\n", wlock, h2w, tpd);

    char usb_en[16] = "?", usb_fn[64] = "?", usb_vid[16] = "?", usb_pid[16] = "?";
    read_trimmed_line("/sys/class/android_usb/android0/enable", usb_en, sizeof(usb_en));
    read_trimmed_line("/sys/class/android_usb/android0/functions", usb_fn, sizeof(usb_fn));
    read_trimmed_line("/sys/class/android_usb/android0/idVendor", usb_vid, sizeof(usb_vid));
    read_trimmed_line("/sys/class/android_usb/android0/idProduct", usb_pid, sizeof(usb_pid));
    fprintf(out, "usb_en=%s usb_fn=%s vid=%s pid=%s ipod_cfg=%d dbg_cfg=%d\n",
            usb_en, usb_fn, usb_vid, usb_pid,
            is_ipod_usb_mode_enabled(), logging_enabled_from_config());

    /* Available Components */
    fprintf(out, "\n[available_components]\n");
    fprintf(out, "dev: fb0=%d eac=%d fm=%d stpwmt=%d stpbt=%d ttyGS0=%d\n",
            access("/dev/graphics/fb0", F_OK) == 0,
            access("/dev/eac", F_OK) == 0,
            access("/dev/fm", F_OK) == 0,
            access("/dev/stpwmt", F_OK) == 0,
            access("/dev/stpbt", F_OK) == 0,
            access("/dev/ttyGS0", F_OK) == 0);
    fprintf(out, "bin_kb: rockbox=%ld meui=%ld custom=%ld sys_app=%ld ipod_sync=%ld\n",
            file_kb("/mnt/sdcard/.rockbox/rockbox"),
            file_kb("/mnt/sdcard/.meui/meui"),
            file_kb("/mnt/sdcard/.custom/launch"),
            file_kb("/mnt/sdcard/.system/app"),
            file_kb("/bin/ipod_sync"));
    fprintf(out, "sys_kb: liby1=%ld liby1video=%ld android_ramdisk=%ld ipod_vpd_ko=%ld\n",
            file_kb("/mnt/sdcard/.system/lib/liby1.so"),
            file_kb("/mnt/sdcard/.system/lib/liby1video.so"),
            file_kb("/mnt/sdcard/.system/ramdisk_android.cpio.gz"),
            file_kb("/lib/modules/ipod_vpd.ko"));
    fprintf(out, "itunes_kb: iTunesDB=%ld ArtworkDB=%ld PlayCounts=%ld\n",
            file_kb("/mnt/sdcard/iPod_Control/iTunes/iTunesDB"),
            file_kb("/mnt/sdcard/iPod_Control/Artwork/ArtworkDB"),
            file_kb("/mnt/sdcard/iPod_Control/iTunes/Play Counts"));
    dump_dir_names(out, "sys_bin", "/mnt/sdcard/.system/bin");
    dump_dir_names(out, "bt_blobs", "/mnt/sdcard/.system/bt");

    /* Storage & Partitions */
    struct statvfs vfs;
    unsigned long long total_mb = 0, free_mb = 0;
    if (statvfs("/mnt/sdcard", &vfs) == 0) {
        total_mb = ((unsigned long long)vfs.f_blocks * vfs.f_frsize) >> 20;
        free_mb = ((unsigned long long)vfs.f_bavail * vfs.f_frsize) >> 20;
    }
    fprintf(out, "\n[storage]\n");
    fprintf(out, "sd_dev=%s ext_dev=%s sd_total_mb=%llu sd_free_mb=%llu\n",
            active_sd_dev[0] ? active_sd_dev : "none",
            active_ext_dev[0] ? active_ext_dev : "none",
            total_mb, free_mb);

    FILE *pfp = fopen("/proc/partitions", "r");
    if (pfp) {
        char line[128];
        while (fgets(line, sizeof(line), pfp))
            fputs(line, out);
        fclose(pfp);
    }

    FILE *mntfp = fopen("/proc/mounts", "r");
    if (mntfp) {
        char line[256];
        fprintf(out, "\n[mounts]\n");
        while (fgets(line, sizeof(line), mntfp)) {
            if (strstr(line, "/mnt/sdcard") || strstr(line, "mmcblk"))
                fputs(line, out);
        }
        fclose(mntfp);
    }

    FILE *modfp = fopen("/proc/modules", "r");
    if (modfp) {
        char line[256];
        fprintf(out, "\n[modules]\n");
        while (fgets(line, sizeof(line), modfp)) {
            char name[64];
            unsigned long sz = 0;
            if (sscanf(line, "%63s %lu", name, &sz) == 2)
                fprintf(out, "%s (%lu)\n", name, sz);
        }
        fclose(modfp);
    }

    /* Compact Logs */
    if (early_log_len > 0) {
        fprintf(out, "\n[init_log]\n");
        fwrite(early_log_buf, 1, early_log_len, out);
        if (early_log_buf[early_log_len - 1] != '\n')
            fputc('\n', out);
    }

    char *kbuf = malloc(16384 + 1);
    if (kbuf) {
        int kn = klogctl(3, kbuf, 16384);
        if (kn > 0) {
            kbuf[kn] = '\0';

            /* Compact hardware-probe lines from dmesg */
            fprintf(out, "\n[hw_probe_dmesg]\n");
            int probe_lines = 0;
            char *p = kbuf;
            while (p && *p && probe_lines < 25) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                if (strstr(p, "lcm") || strstr(p, "LCM") ||
                    strstr(p, "tpd") || strstr(p, "accdet") ||
                    strstr(p, "msdc") || strstr(p, "mt6323") ||
                    strstr(p, "mt6627") || strstr(p, "eac")) {
                    fprintf(out, "%s\n", p);
                    probe_lines++;
                }
                if (nl) *nl = '\n';
                p = nl ? (nl + 1) : NULL;
            }

            const char *tail = (kn > 3072) ? (kbuf + kn - 3072) : kbuf;
            if (kn > 3072) {
                const char *nl = strchr(tail, '\n');
                if (nl) tail = nl + 1;
            }
            fprintf(out, "\n[dmesg_tail]\n%s%s", tail,
                    (kbuf[kn - 1] == '\n') ? "" : "\n");
        }
        free(kbuf);
    }

    append_file_tail(out, "y1_debug.txt", "/mnt/sdcard/.system/y1_debug.txt", 2048);

    fflush(out);
    fsync(fileno(out));
    fclose(out);
    sync();

    char msg[192];
    snprintf(msg, sizeof(msg), "Bootloader Menu: Debug info saved to %s\n", out_path);
    log_line(msg);
    return 0;
}

static const unsigned char dbg_font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x6C,0x6C,0x48,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* # */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* $ */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* & */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* / */
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
    {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00}, /* 2 */
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* 3 */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 4 */
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, /* 5 */
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* 6 */
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 7 */
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* 8 */
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* < */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* = */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* > */
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* ? */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* @ */
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* A */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* B */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* C */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* D */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* E */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* F */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* G */
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* H */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* I */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* J */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* K */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* L */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* M */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* N */
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* O */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* P */
    {0x7C,0xC6,0xC6,0xC6,0xD6,0x7C,0x0E,0x00}, /* Q */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* R */
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, /* S */
    {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00}, /* T */
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* U */
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, /* V */
    {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00}, /* W */
    {0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0x00}, /* X */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, /* Y */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* Z */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* [ */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* \ */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ] */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* a */
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, /* b */
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, /* c */
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, /* d */
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, /* e */
    {0x38,0x6C,0x64,0xF0,0x60,0x60,0xF0,0x00}, /* f */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* g */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* i */
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, /* j */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* l */
    {0x00,0x00,0xCC,0xFE,0xFE,0xD6,0xC6,0x00}, /* m */
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /* n */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, /* o */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* p */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* q */
    {0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00}, /* r */
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, /* s */
    {0x30,0x30,0xFC,0x30,0x30,0x34,0x18,0x00}, /* t */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* u */
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* v */
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, /* w */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* x */
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC}, /* y */
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00}, /* z */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* } */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

static void dbg_draw_char(uint8_t *fb, int stride, int w, int h, int bpp,
                          int px, int py, char ch, uint32_t fg)
{
    unsigned char uc = (unsigned char)ch;
    if (uc < 0x20 || uc > 0x7E) uc = '?';
    const unsigned char *glyph = dbg_font8x8[uc - 0x20];
    uint16_t fg16 = (uint16_t)((((fg >> 19) & 0x1F) << 11) |
                               (((fg >> 10) & 0x3F) << 5) |
                               ((fg >> 3) & 0x1F));

    for (int row = 0; row < 8; row++) {
        int y = py + row;
        if (y < 0 || y >= h) continue;
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int x = px + col;
            if (x < 0 || x >= w) continue;
            if (bits & (0x80 >> col)) {
                if (bpp == 32) {
                    uint32_t *p = (uint32_t *)(fb + y * stride);
                    p[x] = fg;
                } else {
                    uint16_t *p = (uint16_t *)(fb + y * stride);
                    p[x] = fg16;
                }
            }
        }
    }
}

static void dbg_draw_text(uint8_t *fb, int stride, int w, int h, int bpp,
                          int px, int py, const char *s, uint32_t fg)
{
    while (*s && px + 8 <= w) {
        dbg_draw_char(fb, stride, w, h, bpp, px, py, *s, fg);
        px += 8;
        s++;
    }
}

static void show_debug_info_screen(const char *saved_path, int saved_ok, int *event_fds)
{
    const char *devs[] = {"/dev/graphics/fb0", "/dev/fb0", NULL};
    int fbfd = -1;
    for (int i = 0; devs[i]; i++) {
        fbfd = open(devs[i], O_RDWR | O_CLOEXEC);
        if (fbfd >= 0) break;
    }
    if (fbfd < 0) return;

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    memset(&finfo, 0, sizeof(finfo));
    memset(&vinfo, 0, sizeof(vinfo));
    ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);
    ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fbfd, FBIOBLANK, VESA_NO_BLANKING);

    int w = vinfo.xres ? (int)vinfo.xres : 480;
    int h = vinfo.yres ? (int)vinfo.yres : 360;
    int bpp = vinfo.bits_per_pixel ? (int)vinfo.bits_per_pixel : 32;
    int stride = finfo.line_length ? (int)finfo.line_length : (w * (bpp / 8));
    size_t map_len = finfo.smem_len ? finfo.smem_len : (size_t)(stride * h);
    uint8_t *fb = (uint8_t *)mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED || !fb) {
        close(fbfd);
        return;
    }

    char lines[64][64];
    int nlines = 0;
    if (saved_ok == 0 && saved_path && saved_path[0]) {
        snprintf(lines[nlines++], 64, "Saved: %s", saved_path);
    } else {
        snprintf(lines[nlines++], 64, "SD not writable (showing live info)");
    }

    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);
    snprintf(lines[nlines++], 64, "Build: %s (%s)", NUGGET_BUILD_DATE, NUGGET_GIT_REV);
    snprintf(lines[nlines++], 64, "Kernel: %s %s", uts.release, uts.machine);

    char tmp[128];
    if (read_trimmed_line("/proc/lcm", tmp, sizeof(tmp)) == 0)
        snprintf(lines[nlines++], 64, "LCM: %s", tmp);
    snprintf(lines[nlines++], 64, "FB0: %dx%d @ %dbpp", w, h, bpp);

    char cap[16] = "?", bstat[32] = "?", usb_on[16] = "?";
    read_trimmed_line("/sys/class/power_supply/battery/capacity", cap, sizeof(cap));
    read_trimmed_line("/sys/class/power_supply/battery/status", bstat, sizeof(bstat));
    read_trimmed_line("/sys/class/power_supply/usb/online", usb_on, sizeof(usb_on));
    snprintf(lines[nlines++], 64, "Batt: %s%% (%s) USB:%s", cap, bstat, usb_on);

    snprintf(lines[nlines++], 64, "SD: %s  Ext: %s",
             active_sd_dev[0] ? active_sd_dev : "none",
             active_ext_dev[0] ? active_ext_dev : "none");

    if (saved_ok == 0 && saved_path && saved_path[0]) {
        FILE *fp = fopen(saved_path, "r");
        if (fp) {
            char fline[256];
            while (nlines < 64 && fgets(fline, sizeof(fline), fp)) {
                size_t len = strlen(fline);
                while (len > 0 && (fline[len - 1] == '\n' || fline[len - 1] == '\r'))
                    fline[--len] = '\0';
                if (len == 0) continue;
                snprintf(lines[nlines++], 64, "%.57s", fline);
            }
            fclose(fp);
        }
    }

    int max_vis = (h - 48) / 10;
    if (max_vis < 1) max_vis = 1;
    int scroll = 0;

    usleep(200000);
    for (int k = 0; k < 16; k++) {
        if (event_fds[k] >= 0) {
            struct input_event fev;
            while (read(event_fds[k], &fev, sizeof(fev)) == sizeof(fev));
        }
    }

    int done = 0;
    while (!done) {
        if (bpp == 32) {
            for (int y = 0; y < h; y++) {
                uint32_t *row = (uint32_t *)(fb + y * stride);
                for (int x = 0; x < w; x++) row[x] = 0xFF10141C;
            }
        } else {
            memset(fb, 0, map_len);
        }

        dbg_draw_text(fb, stride, w, h, bpp, 8, 8, "=== Nugget Y1 Debug Info ===", 0xFF55CCFF);
        for (int i = 0; i < max_vis && (scroll + i) < nlines; i++) {
            uint32_t col = (scroll + i == 0) ? (saved_ok == 0 ? 0xFF55FF88 : 0xFFFF6666) : 0xFFE6E6E6;
            dbg_draw_text(fb, stride, w, h, bpp, 8, 24 + i * 10, lines[scroll + i], col);
        }
        dbg_draw_text(fb, stride, w, h, bpp, 8, h - 14,
                      "Press Menu/Center to go back", 0xFFAAAAAA);

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
        if (poll(pfd, n, 200) > 0) {
            for (nfds_t i = 0; i < n; i++) {
                if (!(pfd[i].revents & POLLIN)) continue;
                struct input_event ev;
                while (read(event_fds[map_idx[i]], &ev, sizeof(ev)) == sizeof(ev)) {
                    if (ev.type != EV_KEY || ev.value != 1) continue;
                    if (ev.code == 105 || ev.code == 165 || ev.code == 103) {
                        if (scroll > 0) scroll--;
                    } else if (ev.code == 106 || ev.code == 163 || ev.code == 108) {
                        if (scroll + max_vis < nlines) scroll++;
                    } else {
                        done = 1;
                        break;
                    }
                }
                if (done) break;
            }
        }
    }

    munmap(fb, map_len);
    close(fbfd);
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

    struct bootl_entry {
        int id; /* 0=Rockbox, 1=MeUI, 2=Custom, 3=Android, 4=Shutdown, 5=USB, 6=Terminal, 7=Debug */
        const char *screen;
    } entries[8];
    int num_entries = 0;

    /* Pick 1: Rockbox (always present) */
    entries[num_entries].id = 0;
    entries[num_entries].screen = "bootl_pick1";
    num_entries++;

    /* Pick 2: MeUI (only if /.meui/meui or /mnt/sdcard/.meui/meui exists) */
    if (access("/mnt/sdcard/.meui/meui", F_OK) == 0 || access("/.meui/meui", F_OK) == 0) {
        entries[num_entries].id = 1;
        entries[num_entries].screen = "bootl_pick2";
        num_entries++;
    }

    /* Pick 3: User defined software (only if /.custom/launch or /mnt/sdcard/.custom/launch exists) */
    if (access("/mnt/sdcard/.custom/launch", F_OK) == 0 || access("/.custom/launch", F_OK) == 0) {
        entries[num_entries].id = 2;
        entries[num_entries].screen = "bootl_pick3";
        num_entries++;
    }

    /* Pick 4: Android (always present) */
    entries[num_entries].id = 3;
    entries[num_entries].screen = "bootl_pick4";
    num_entries++;

    /* Pick 5: Shut down (always present) */
    entries[num_entries].id = 4;
    entries[num_entries].screen = "bootl_pick5";
    num_entries++;

    /* Pick 6: USB Mass Storage (always present) */
    entries[num_entries].id = 5;
    entries[num_entries].screen = "bootl_pick6";
    num_entries++;

    /* Pick 7: Linux terminal (USB) (always present) */
    entries[num_entries].id = 6;
    entries[num_entries].screen = "bootl_pick7";
    num_entries++;

    /* Pick 8: Debug info log (always present) */
    entries[num_entries].id = 7;
    entries[num_entries].screen = "bootl_pick8";
    num_entries++;

    int selection = 0;
    display_screen(entries[selection].screen);

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
                        selection = (selection + num_entries - 1) % num_entries;
                        display_screen(entries[selection].screen);
                    }
                    /* Next / Right / Wheel CW */
                    else if (ev.code == 106 || ev.code == 163 || ev.code == 108) {
                        selection = (selection + 1) % num_entries;
                        display_screen(entries[selection].screen);
                    }
                    /* Confirm: Center button */
                    else if (ev.code == 232 || ev.code == 28 || ev.code == 96 ||
                             ev.code == 352 || ev.code == 353 || ev.code == 314 ||
                             ev.code == 315 || ev.code == 304) {

                        int action_id = entries[selection].id;
                        if (action_id == 0) {
                            /* Pick 1: Rockbox */
                            log_line("Bootloader Menu: Rockbox selected.\n");
                            if (access("/mnt/sdcard/.rockbox/rockbox", F_OK) == 0) {
                                strncpy(custom_app_override, "/mnt/sdcard/.rockbox/rockbox", sizeof(custom_app_override) - 1);
                            } else {
                                strncpy(custom_app_override, "/.rockbox/rockbox", sizeof(custom_app_override) - 1);
                            }
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) close(event_fds[k]);
                            }
                            display_screen("boot");
                            return;
                        } else if (action_id == 1) {
                            /* Pick 2: MeUI */
                            log_line("Bootloader Menu: MeUI selected.\n");
                            if (access("/mnt/sdcard/.meui/meui", F_OK) == 0) {
                                strncpy(custom_app_override, "/mnt/sdcard/.meui/meui", sizeof(custom_app_override) - 1);
                            } else {
                                strncpy(custom_app_override, "/.meui/meui", sizeof(custom_app_override) - 1);
                            }
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) close(event_fds[k]);
                            }
                            display_screen("boot");
                            return;
                        } else if (action_id == 2) {
                            /* Pick 3: User defined software */
                            log_line("Bootloader Menu: User defined software selected.\n");
                            if (access("/mnt/sdcard/.custom/launch", F_OK) == 0) {
                                strncpy(custom_app_override, "/mnt/sdcard/.custom/launch", sizeof(custom_app_override) - 1);
                            } else {
                                strncpy(custom_app_override, "/.custom/launch", sizeof(custom_app_override) - 1);
                            }
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) close(event_fds[k]);
                            }
                            display_screen("boot");
                            return;
                        } else if (action_id == 3) {
                            /* Pick 4: Android */
                            log_line("Bootloader Menu: Android selected.\n");
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) close(event_fds[k]);
                            }
                            boot_into_android();
                        } else if (action_id == 4) {
                            /* Pick 5: Shut down */
                            log_line("Bootloader Menu: Shut down selected.\n");
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) close(event_fds[k]);
                            }
                            sync();
                            reboot(RB_POWER_OFF);
                            while (1) sleep(1);
                        } else if (action_id == 5) {
                            /* Pick 6: USB Mass Storage (standard non-iPod mode, no spoofing) */
                            log_line("Bootloader Menu: USB mode selected.\n");
                            setup_usb_gadget(active_sd_dev[0] ? active_sd_dev : NULL,
                                             active_ext_dev[0] ? active_ext_dev : NULL, 0);
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
                            write_sysfs("/sys/class/android_usb/android0/f_mass_storage/bicr", "0\n");
                            write_sysfs("/sys/class/android_usb/android0/enable", "0\n");
                            sync();
                            write_sysfs("/proc/sys/vm/drop_caches", "3\n");

                            /* Debounce before returning to menu */
                            usleep(200000);
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) {
                                    struct input_event flush_ev;
                                    while (read(event_fds[k], &flush_ev, sizeof(flush_ev)) == sizeof(flush_ev));
                                }
                            }

                            display_screen(entries[selection].screen);
                        } else if (action_id == 6) {
                            /* Pick 7: Linux terminal (USB) -> Launch multi-tab terminal shells on /dev/ttyGS0 and /dev/ttyGS1.
                             * Disconnect ONLY when the user turns off the device or types 'exit'/'menu'.
                             * Button presses on the device will NOT disconnect the terminal. */
                            log_line("Bootloader Menu: Linux terminal (USB) mode selected (Dual ACM tabs).\n");

                            unlink("/tmp/exit_terminal");

                            write_sysfs("/sys/class/android_usb/android0/enable", "0\n");
                            write_sysfs("/sys/class/android_usb/android0/idVendor", "0e8d\n");
                            write_sysfs("/sys/class/android_usb/android0/idProduct", "2007\n");
                            write_sysfs("/sys/class/android_usb/android0/f_acm/instances", "2\n");
                            write_sysfs("/sys/class/android_usb/android0/functions", "acm\n");
                            /* Use bDeviceClass 0 (Interface-defined) so Windows Composite Device Enumerator
                             * (usbccgp.sys) creates independent COM ports for each ACM interface (e.g. COM5 & COM6). */
                            write_sysfs("/sys/class/android_usb/android0/bDeviceClass", "0\n");
                            write_sysfs("/sys/class/android_usb/android0/bDeviceSubClass", "0\n");
                            write_sysfs("/sys/class/android_usb/android0/bDeviceProtocol", "0\n");
                            write_sysfs("/sys/class/android_usb/android0/enable", "1\n");

                            /* Spawn Tab 2 worker in background (/dev/ttyGS1) */
                            pid_t term_pid2 = fork();
                            if (term_pid2 == 0) {
                                setpgid(0, 0);
                                run_usb_serial_shell("ttyGS1", 2);
                                _exit(0);
                            }

                            /* Spawn Tab 1 worker (/dev/ttyGS0) */
                            pid_t term_pid1 = fork();
                            if (term_pid1 == 0) {
                                setpgid(0, 0);
                                run_usb_serial_shell("ttyGS0", 1);
                                _exit(0);
                            }

                            display_screen("bootl_terminal");

                            /* Wait for user to exit via terminal shell ('exit' command on primary tab or 'menu') */
                            int status = 0;
                            waitpid(term_pid1, &status, 0);

                            int ef = open("/tmp/exit_terminal", O_WRONLY | O_CREAT | O_TRUNC, 0666);
                            if (ef >= 0) close(ef);

                            if (term_pid2 > 0) {
                                kill(term_pid2, SIGTERM);
                                waitpid(term_pid2, NULL, WNOHANG);
                            }
                            unlink("/tmp/exit_terminal");

                            /* Disable USB terminal */
                            log_line("Terminal shell exited. Disabling USB terminal and returning to Bootloader Menu...\n");
                            write_sysfs("/sys/class/android_usb/android0/enable", "0\n");
                            write_sysfs("/sys/class/android_usb/android0/bDeviceClass", "0\n");

                            /* Debounce before returning to menu */
                            usleep(200000);
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) {
                                    struct input_event flush_ev;
                                    while (read(event_fds[k], &flush_ev, sizeof(flush_ev)) == sizeof(flush_ev));
                                }
                            }

                            display_screen(entries[selection].screen);
                        } else if (action_id == 7) {
                            /* Pick 8: Write compact debug info log and display debug info on screen */
                            log_line("Bootloader Menu: Debug info selected.\n");
                            char saved_path[128];
                            int saved_ok = write_compact_debug_info(saved_path, sizeof(saved_path));
                            show_debug_info_screen(saved_path, saved_ok, event_fds);

                            usleep(200000);
                            for (int k = 0; k < 16; k++) {
                                if (event_fds[k] >= 0) {
                                    struct input_event flush_ev;
                                    while (read(event_fds[k], &flush_ev, sizeof(flush_ev)) == sizeof(flush_ev));
                                }
                            }
                            current_displayed_screen[0] = '\0';
                            display_screen(entries[selection].screen);
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

static void init_bluetooth_subsystem(void)
{
    /* Ensure /system/etc/firmware and /system/etc/fmr symlinks point to /.system/bt
     * for mtk_stp_wmt.ko and mtk_fm_drv.ko. */
    mkdir("/system", 0755);
    mkdir("/system/etc", 0755);
    unlink("/system/etc/firmware");
    (void)!symlink("/etc/firmware", "/system/etc/firmware");
    unlink("/etc/firmware/mt6627");
    (void)!symlink("/.system/bt/mt6627", "/etc/firmware/mt6627");
    unlink("/etc/fmr");
    (void)!symlink("/.system/bt/fmr", "/etc/fmr");
    unlink("/system/etc/fmr");
    (void)!symlink("/.system/bt/fmr", "/system/etc/fmr");
    mkdir("/data", 0777);

    if (access("/dev/stpwmt", F_OK) != 0) mknod("/dev/stpwmt", S_IFCHR | 0666, makedev(190, 0));
    if (access("/dev/stpbt",  F_OK) != 0) mknod("/dev/stpbt",  S_IFCHR | 0666, makedev(192, 0));
    if (access("/dev/fm",     F_OK) != 0) mknod("/dev/fm",     S_IFCHR | 0666, makedev(193, 0));
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

static void force_hardware_shutdown(void)
{
    int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0) {
        const char *msg = "<0>[watchdog] Play/Pause held for 10s: Forceful hardware shutdown triggered!\n";
        (void)!write(kfd, msg, strlen(msg));
        close(kfd);
    }
    sync();
    reboot(RB_POWER_OFF);
    (void)!system("/bin/busybox poweroff -f");
    (void)!system("/sbin/poweroff");
    while (1) {
        sleep(1);
    }
}

static void play_shutdown_watchdog_loop(void)
{
    int event_fds[16];
    int num_fds = 0;

    while (1) {
        num_fds = 0;
        for (int i = 0; i < 16; i++) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                char name[64] = {0};
                if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
                    if (strstr(name, "ACCDET") != NULL || strstr(name, "accdet") != NULL) {
                        close(fd);
                        continue;
                    }
                }
                event_fds[num_fds++] = fd;
            }
        }

        if (num_fds == 0) {
            usleep(250000);
            continue;
        }

        struct pollfd pfds[16];
        int play_held = 0;
        struct timespec press_start_ts;
        memset(&press_start_ts, 0, sizeof(press_start_ts));

        while (1) {
            for (int i = 0; i < num_fds; i++) {
                pfds[i].fd = event_fds[i];
                pfds[i].events = POLLIN;
                pfds[i].revents = 0;
            }

            int timeout_ms = play_held ? 100 : 500;
            int pr = poll(pfds, num_fds, timeout_ms);
            if (pr < 0 && errno == EINTR) {
                continue;
            }

            int dev_error = 0;
            if (pr > 0) {
                for (int i = 0; i < num_fds; i++) {
                    if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                        dev_error = 1;
                        break;
                    }
                    if (pfds[i].revents & POLLIN) {
                        struct input_event ev;
                        while (read(pfds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                            if (ev.type == EV_KEY &&
                                (ev.code == 164 /* KEY_PLAYPAUSE */ ||
                                 ev.code == 207 /* KEY_PLAY */ ||
                                 ev.code == 119 /* KEY_PAUSE */ ||
                                 ev.code == 200 /* KEY_PLAYCD */)) {
                                if (ev.value == 1) {
                                    if (!play_held) {
                                        play_held = 1;
                                        clock_gettime(CLOCK_MONOTONIC, &press_start_ts);
                                    }
                                } else if (ev.value == 0) {
                                    play_held = 0;
                                }
                            }
                        }
                    }
                }
            }

            if (dev_error) {
                for (int i = 0; i < num_fds; i++) {
                    if (event_fds[i] >= 0) close(event_fds[i]);
                }
                break;
            }

            /* Query hardware key state via EVIOCGKEY to prevent missed releases or start while already pressed */
            int key_pressed_now = 0;
            uint8_t key_bits[KEY_MAX / 8 + 1];
            for (int i = 0; i < num_fds; i++) {
                memset(key_bits, 0, sizeof(key_bits));
                if (ioctl(event_fds[i], EVIOCGKEY(sizeof(key_bits)), key_bits) >= 0) {
                    if ((key_bits[164 / 8] & (1 << (164 % 8))) ||
                        (key_bits[207 / 8] & (1 << (207 % 8))) ||
                        (key_bits[119 / 8] & (1 << (119 % 8))) ||
                        (key_bits[200 / 8] & (1 << (200 % 8)))) {
                        key_pressed_now = 1;
                        break;
                    }
                }
            }

            if (!key_pressed_now) {
                play_held = 0;
            } else {
                if (!play_held) {
                    play_held = 1;
                    clock_gettime(CLOCK_MONOTONIC, &press_start_ts);
                } else {
                    struct timespec now_ts;
                    clock_gettime(CLOCK_MONOTONIC, &now_ts);
                    long elapsed_ms = (now_ts.tv_sec - press_start_ts.tv_sec) * 1000 +
                                      (now_ts.tv_nsec - press_start_ts.tv_nsec) / 1000000;
                    if (elapsed_ms >= 10000) {
                        force_hardware_shutdown();
                    }
                }
            }
        }
    }
}

static void start_play_shutdown_watchdog(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        log_line("WARNING: Failed to fork Play/Pause shutdown watchdog!\n");
        return;
    }
    if (pid == 0) {
        play_shutdown_watchdog_loop();
        _exit(0);
    }
    log_line("Hardware 10s Play/Pause power-off watchdog started.\n");
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

static int is_ipod_usb_mode_enabled(void)
{
    char buf[64];
    if (read_text_file("/mnt/sdcard/.system/ipod_usb_mode", buf, sizeof(buf))) {
        return atoi(buf) != 0 ? 1 : 0;
    }

    FILE *cfg = fopen("/mnt/sdcard/.rockbox/config.cfg", "r");
    if (cfg) {
        char line[256];
        while (fgets(line, sizeof(line), cfg)) {
            if (strncmp(line, "y1 ipod usb mode:", 17) == 0) {
                const char *val = line + 17;
                while (*val == ' ' || *val == '\t') val++;
                fclose(cfg);
                return (strncmp(val, "on", 2) == 0 || *val == '1') ? 1 : 0;
            }
        }
        fclose(cfg);
    }
    return 1;
}

static void read_ipod_usb_serial(char *out, size_t out_sz)
{
    snprintf(out, out_sz, "000A270019B48C1F\n");
    FILE *fp = fopen("/mnt/sdcard/iPod_Control/Device/SysInfo", "r");
    if (!fp)
        return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "FirewireGuid:", 13) == 0) {
            const char *p = line + 13;
            while (*p == ' ' || *p == '\t') p++;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
                p += 2;
            char hex[32];
            size_t n = 0;
            while (n + 1 < sizeof(hex) &&
                   ((p[n] >= '0' && p[n] <= '9') ||
                    (p[n] >= 'a' && p[n] <= 'f') ||
                    (p[n] >= 'A' && p[n] <= 'F'))) {
                char c = p[n];
                if (c >= 'a' && c <= 'f')
                    c = (char)(c - 'a' + 'A');
                hex[n++] = c;
            }
            hex[n] = '\0';
            if (n >= 8)
                snprintf(out, out_sz, "%s\n", hex);
            break;
        }
    }
    fclose(fp);
}

/* Linux f_mass_storage enforces: if (count > 28) return -EINVAL;
 * Appending a newline causes 28-char inquiry strings to be 29 bytes and silently fail.
 * Write exact raw bytes without newline across all possible lun inquiry sysfs nodes. */
static void write_inquiry_string(const char *inq)
{
    static const char * const inq_paths[] = {
        "/sys/class/android_usb/android0/f_mass_storage/lun0/inquiry_string",
        "/sys/class/android_usb/android0/f_mass_storage/lun1/inquiry_string",
        "/sys/class/android_usb/android0/f_mass_storage/lun/inquiry_string",
        "/sys/class/android_usb/android0/f_mass_storage/inquiry_string",
        "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun0/inquiry_string",
        "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun1/inquiry_string",
        "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun/inquiry_string",
        "/sys/devices/virtual/android_usb/android0/f_mass_storage/inquiry_string",
        NULL
    };
    static const char * const cdrom_paths[] = {
        "/sys/class/android_usb/android0/f_mass_storage/lun0/cdrom",
        "/sys/class/android_usb/android0/f_mass_storage/lun1/cdrom",
        "/sys/class/android_usb/android0/f_mass_storage/lun/cdrom",
        "/sys/class/android_usb/android0/f_mass_storage/cdrom",
        "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun0/cdrom",
        "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun1/cdrom",
        "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun/cdrom",
        "/sys/devices/virtual/android_usb/android0/f_mass_storage/cdrom",
        NULL
    };
    for (int i = 0; cdrom_paths[i]; i++) {
        write_sysfs(cdrom_paths[i], "0\n");
    }
    if (!inq)
        return;
    size_t len = strlen(inq);
    if (len > 28)
        len = 28;
    for (int i = 0; inq_paths[i]; i++) {
        write_sysfs_exact(inq_paths[i], inq, len);
    }
}

static void ensure_ipod_device_files(void)
{
    mkdir("/mnt/sdcard/iPod_Control", 0777);
    mkdir("/mnt/sdcard/iPod_Control/Device", 0777);
    mkdir("/mnt/sdcard/iPod_Control/iTunes", 0777);
    mkdir("/mnt/sdcard/iPod_Control/Music", 0777);
    mkdir("/mnt/sdcard/iPod_Control/Artwork", 0777);
    mkdir("/mnt/sdcard/Playlists", 0777);
    mkdir("/mnt/sdcard/Videos", 0777);
    mkdir("/mnt/sdcard/.system", 0777);
    mkdir("/mnt/sdcard/.rockbox", 0777);
    mkdir("/mnt/sdcard/.rockbox/albumart", 0777);
    mkdir("/mnt/sdcard/.rockbox/video_thumbs", 0777);

    for (int i = 0; i < 20; i++) {
        char fdir[128];
        snprintf(fdir, sizeof(fdir), "/mnt/sdcard/iPod_Control/Music/F%02d", i);
        mkdir(fdir, 0777);
    }

    if (access("/mnt/sdcard/iPod_Control", F_OK) == 0 && access("/iPod_Control", F_OK) != 0)
        (void)!symlink("/mnt/sdcard/iPod_Control", "/iPod_Control");
    if (access("/mnt/sdcard/Playlists", F_OK) == 0 && access("/Playlists", F_OK) != 0)
        (void)!symlink("/mnt/sdcard/Playlists", "/Playlists");
    if (access("/mnt/sdcard/Videos", F_OK) == 0 && access("/Videos", F_OK) != 0)
        (void)!symlink("/mnt/sdcard/Videos", "/Videos");

    char serial[48] = {0};
    char guid[32] = {0};
    snprintf(serial, sizeof(serial), "8K7289LAV9K");
    snprintf(guid, sizeof(guid), "000A270019B48C1F");

    FILE *rfp = fopen("/mnt/sdcard/iPod_Control/Device/SysInfo", "r");
    if (rfp) {
        char line[256];
        while (fgets(line, sizeof(line), rfp)) {
            if (strncmp(line, "pszSerialNumber:", 16) == 0) {
                const char *p = line + 16;
                while (*p == ' ' || *p == '\t') p++;
                size_t l = strlen(p);
                while (l > 0 && (p[l-1] == '\r' || p[l-1] == '\n' || p[l-1] == ' ')) l--;
                if (l > 0 && l < sizeof(serial)) {
                    memcpy(serial, p, l);
                    serial[l] = '\0';
                }
            } else if (strncmp(line, "FirewireGuid:", 13) == 0) {
                const char *p = line + 13;
                while (*p == ' ' || *p == '\t') p++;
                if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
                char hex[32]; size_t n = 0;
                while (n + 1 < sizeof(hex) && isxdigit((unsigned char)p[n])) {
                    hex[n] = (char)toupper((unsigned char)p[n]);
                    n++;
                }
                hex[n] = '\0';
                if (n >= 8) snprintf(guid, sizeof(guid), "%s", hex);
            }
        }
        fclose(rfp);
    }

    struct stat st;
    if (stat("/mnt/sdcard/iPod_Control/Device/SysInfo", &st) != 0 || st.st_size < 100) {
        FILE *wfp = fopen("/mnt/sdcard/iPod_Control/Device/SysInfo", "w");
        if (wfp) {
            fprintf(wfp,
                "BoardHwName: iPod M25\r\n"
                "pszSerialNumber: %s\r\n"
                "ModelNumStr: MA450\r\n"
                "FirewireGuid: 0x%s\r\n"
                "HddFirmwareRev: BU011A\r\n"
                "RegionCode: FB(0x001A)\r\n"
                "PolicyFlags: 0x00000000\r\n"
                "buildID: 0x06118000 (6.1.1)\r\n"
                "visibleBuildID: 0x01128000 (1.2.1)\r\n"
                "boardHwRev: 0x00000000 (0.0 0)\r\n"
                "boardHwSwInterfaceRev: 0x000C0005 (0.0.12 5)\r\n"
                "bootLoaderImageRev: 0x00000000 (0.0 0)\r\n"
                "diskModeImageRev: 0x00000000 (0.0 0)\r\n"
                "diagImageRev: 0x00000000 (0.0 0)\r\n"
                "osImageRev: 0x00000000 (0.0 0)\r\n"
                "iPodFamily: 0x00000006\r\n"
                "updaterFamily: 0x00000006\r\n",
                serial, guid);
            fflush(wfp);
            fclose(wfp);
        }
    }
}

static void setup_usb_gadget(const char *sd_dev, const char *ext_dev, int allow_ipod)
{
    log_line("Configuring USB Mass Storage gadget for PC access...\n");

    int ipod_mode = allow_ipod ? is_ipod_usb_mode_enabled() : 0;
    if (ipod_mode) {
        ensure_ipod_device_files();
        if (access("/bin/ipod_sync", X_OK) == 0) {
            (void)!system("/bin/ipod_sync init");
        }
    }
    sync();

    /* 1. Disable gadget while reconfiguring */
    write_sysfs("/sys/class/android_usb/android0/enable", "0\n");

    /* 2. Configure USB descriptors */
    write_sysfs("/sys/class/android_usb/android0/functions", "mass_storage\n");
    write_sysfs("/sys/class/android_usb/android0/bDeviceClass", "0\n");
    write_sysfs("/sys/class/android_usb/android0/bDeviceSubClass", "0\n");
    write_sysfs("/sys/class/android_usb/android0/bDeviceProtocol", "0\n");

    if (ipod_mode) {
        char serial_buf[48];
        read_ipod_usb_serial(serial_buf, sizeof(serial_buf));

        if (access("/lib/modules/ipod_vpd.ko", F_OK) == 0 &&
            access("/sys/module/ipod_vpd", F_OK) != 0) {
            char dev_serial[32] = "8K7289LAV9K";
            char dev_guid[48] = "000A270019B48C1F";
            snprintf(dev_guid, sizeof(dev_guid), "%s", serial_buf);
            size_t gl = strlen(dev_guid);
            while (gl > 0 && (dev_guid[gl - 1] == '\n' || dev_guid[gl - 1] == '\r'))
                dev_guid[--gl] = '\0';

            FILE *sfp = fopen("/mnt/sdcard/iPod_Control/Device/SysInfo", "r");
            if (sfp) {
                char sline[256];
                while (fgets(sline, sizeof(sline), sfp)) {
                    if (strncmp(sline, "pszSerialNumber:", 16) == 0) {
                        const char *p = sline + 16;
                        while (*p == ' ' || *p == '\t') p++;
                        size_t n = 0;
                        while (n + 1 < sizeof(dev_serial) &&
                               ((p[n] >= '0' && p[n] <= '9') ||
                                (p[n] >= 'A' && p[n] <= 'Z') ||
                                (p[n] >= 'a' && p[n] <= 'z'))) {
                            dev_serial[n] = p[n];
                            n++;
                        }
                        if (n > 0)
                            dev_serial[n] = '\0';
                        break;
                    }
                }
                fclose(sfp);
            }

            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                     "insmod /lib/modules/ipod_vpd.ko serial=%s guid=%s",
                     dev_serial, dev_guid);
            (void)!system(cmd);
        }

        write_sysfs("/sys/class/android_usb/android0/idVendor", "05ac\n");
        write_sysfs("/sys/class/android_usb/android0/idProduct", "1209\n");
        write_sysfs("/sys/class/android_usb/android0/bcdDevice", "0002\n");
        write_sysfs("/sys/class/android_usb/android0/iManufacturer", "Apple\n");
        write_sysfs("/sys/class/android_usb/android0/iProduct", "iPod\n");
        write_sysfs("/sys/class/android_usb/android0/iSerial", serial_buf);
        write_inquiry_string("Apple___iPod____________1.62");
        /* Disable MediaTek BICR (Built-in CD-ROM) so the device presents the
         * physical MicroSD card directly as a Direct Access Disk (SCSI TYPE_DISK 0x00).
         * Do NOT enable bicr (Built-In CD-ROM) as it overrides LUN0 with internal storage/ISO.
         */
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/bicr", "0\n");
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/nluns", "1\n");
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/luns", "1\n");
    } else {
        if (access("/sys/module/ipod_vpd", F_OK) == 0)
            (void)!system("rmmod ipod_vpd");
        write_sysfs("/sys/class/android_usb/android0/idVendor", "0e8d\n");
        write_sysfs("/sys/class/android_usb/android0/idProduct", "0001\n");
        write_sysfs("/sys/class/android_usb/android0/bcdDevice", "0100\n");
        write_sysfs("/sys/class/android_usb/android0/iManufacturer", "Innioasis\n");
        write_sysfs("/sys/class/android_usb/android0/iProduct", "Y1 Rockbox Player\n");
        write_sysfs("/sys/class/android_usb/android0/iSerial", "0123456789ABCDEF\n");
        write_inquiry_string("Innioasis_Y1_Player_____0100");
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/bicr", "0\n");
    }

    /* 3. Determine primary backing device for LUN0 (prefer whole raw MicroSD disk) */
    const char *target_lun0 = NULL;
    if (access("/dev/block/mmcblk1", F_OK) == 0) {
        target_lun0 = "/dev/block/mmcblk1";
    } else if (access("/dev/mmcblk1", F_OK) == 0) {
        target_lun0 = "/dev/mmcblk1";
    } else if (sd_dev && access(sd_dev, F_OK) == 0) {
        target_lun0 = sd_dev;
    } else if (access("/dev/block/mmcblk1p1", F_OK) == 0) {
        target_lun0 = "/dev/block/mmcblk1p1";
    } else if (access("/dev/mmcblk1p1", F_OK) == 0) {
        target_lun0 = "/dev/mmcblk1p1";
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

    /* 5. Configure LUN1 for secondary partition if present (strictly disabled in iPod mode) */
    if (!ipod_mode && ext_dev && access(ext_dev, F_OK) == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s\n", ext_dev);

        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/file", buf);
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/nofua", "1\n");
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/ro", "0\n");

        char logmsg[128];
        snprintf(logmsg, sizeof(logmsg), "Attached %s to USB LUN1 (Secondary Partition).\n", ext_dev);
        log_line(logmsg);
    } else {
        /* Clear LUN1 if no secondary storage or in iPod mode */
        write_sysfs("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "\n");
    }

    /* 6. Tune MMC block queue for maximum USB 2.0 sequential throughput */
    write_sysfs("/sys/block/mmcblk1/queue/scheduler", "deadline\n");
    write_sysfs("/sys/block/mmcblk1/queue/scheduler", "noop\n");
    write_sysfs("/sys/block/mmcblk1/queue/read_ahead_kb", "2048\n");
    write_sysfs("/sys/block/mmcblk1/bdi/read_ahead_kb", "2048\n");
    write_sysfs("/sys/block/mmcblk1/queue/nr_requests", "256\n");
    write_sysfs("/sys/block/mmcblk1/queue/nr_requests", "512\n");
    write_sysfs("/sys/block/mmcblk1/queue/iostats", "0\n");
    write_sysfs("/sys/block/mmcblk1/queue/add_random", "0\n");
    write_sysfs("/sys/block/mmcblk1/queue/nomerges", "0\n");
    {
        int hwfd = open("/sys/block/mmcblk1/queue/max_hw_sectors_kb", O_RDONLY);
        if (hwfd >= 0) {
            char hw_buf[32] = {0};
            ssize_t r = read(hwfd, hw_buf, sizeof(hw_buf) - 1);
            close(hwfd);
            if (r > 0) {
                int hw_max = atoi(hw_buf);
                if (hw_max > 128) {
                    char kb_str[16];
                    snprintf(kb_str, sizeof(kb_str), "%d\n", hw_max > 512 ? 512 : hw_max);
                    write_sysfs("/sys/block/mmcblk1/queue/max_sectors_kb", kb_str);
                }
            }
        }
    }

    /* 7. Enable gadget after LUNs and queue tuning are assigned */
    write_sysfs("/sys/class/android_usb/android0/enable", "1\n");
    write_sysfs("/proc/cpufreq/usb_raise_freq", "1\n");
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
    write_sysfs("/sys/class/android_usb/android0/f_mass_storage/bicr", "0\n");

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
    start_play_shutdown_watchdog();

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
                         active_ext_dev[0] ? active_ext_dev : NULL,
                         has_sdcard_rockbox());
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

        /* Ensure default Videos and .system directories exist on MicroSD card */
        mkdir("/mnt/sdcard/.system", 0777);
        mkdir("/mnt/sdcard/Videos", 0777);

        /* Link all MicroSD user files/directories to root / for plugins */
        link_sdcard_to_root();

        /* Launch target application from MicroSD card */
        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/mnt/sdcard/.system/bin:/mnt/sdcard/.rockbox/bin:/mnt/sdcard/.meui/bin:/mnt/sdcard/.custom/bin", 1);
        setenv("LD_LIBRARY_PATH", "/mnt/sdcard/.meui/lib:/mnt/sdcard/.custom/lib:/mnt/sdcard/.system/lib:/mnt/sdcard/.rockbox/lib:/lib:/usr/lib", 1);
        setenv("HOME", "/mnt/sdcard", 1);
        setenv("TERM", "linux", 1);

        const char *app_bin = get_sdcard_app_binary();
        if (!app_bin)
            app_bin = "/mnt/sdcard/.rockbox/rockbox";

        log_line("Launching application from the MicroSD card...\n");

        pid_t rb_pid = fork();
        if (rb_pid == 0) {
            execl(app_bin, app_bin, NULL);
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
         * Drop CPU to powersave while idle in recovery, re-bind USB gadget (iPod/spoofing OFF) */
        apply_powersave_cpu();
        int recovery_allow_ipod = (!crashed && has_sdcard_rockbox());
        if (recovery_allow_ipod && is_ipod_usb_mode_enabled() && access("/bin/ipod_sync", X_OK) == 0) {
            (void)!system("/bin/ipod_sync init");
        }
        setup_usb_gadget(active_sd_dev[0] ? active_sd_dev : NULL,
                         active_ext_dev[0] ? active_ext_dev : NULL,
                         recovery_allow_ipod);
        int saw_usb_pc = 0;

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

            if (usb_to_pc && !saw_usb_pc) {
                saw_usb_pc = 1;
                boost_cpu_for_boot();
            } else if (!usb_to_pc && saw_usb_pc) {
                apply_powersave_cpu();
            }

            /* If issues resolved and not currently connected to PC, continue operation! */
            if (rb_avail && !usb_to_pc) {
                log_line("Issues resolved! Continuing operation...\n");
                write_sysfs("/sys/class/android_usb/android0/enable", "0\n");
                sync();
                write_sysfs("/proc/sys/vm/drop_caches", "3\n");
                if (!crashed && saw_usb_pc && is_ipod_usb_mode_enabled() && access("/bin/ipod_sync", X_OK) == 0) {
                    boost_cpu_for_boot();
                    (void)!system("/bin/ipod_sync post");
                }
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
