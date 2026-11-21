/***************************************************************************
 * liby1 - Android USB Mass Storage Gadget Driver (MT6572)
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "liby1.h"

static bool storage_attached = false;
static bool usb_perf_boosted = false;
static char saved_scheduler[32] = "cfq";
static char saved_read_ahead[16] = "128";
static bool saved_queue_params = false;
static uint64_t g_pre_usb_sys_hash = 0;
static bool g_pre_usb_sys_valid = false;

static uint64_t fnv1a_update(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t hash_single_file(const char *path, uint64_t h)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return h;

    h = fnv1a_update(h, path, strlen(path) + 1);
    uint64_t meta[3] = {
        (uint64_t)st.st_mode,
        (uint64_t)st.st_size,
        (uint64_t)st.st_mtime
    };
    return fnv1a_update(h, meta, sizeof(meta));
}

static uint64_t hash_dir_tree(const char *path, uint64_t h)
{
    DIR *d = opendir(path);
    if (!d)
        return h;

    struct dirent *ent;
    char child[512];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (strcmp(ent->d_name, "y1_debug.txt") == 0)
            continue;

        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) != 0)
            continue;

        h = fnv1a_update(h, child, strlen(child) + 1);
        uint64_t meta[3] = {
            (uint64_t)st.st_mode,
            (uint64_t)st.st_size,
            (uint64_t)st.st_mtime
        };
        h = fnv1a_update(h, meta, sizeof(meta));

        if (S_ISDIR(st.st_mode))
            h = hash_dir_tree(child, h);
    }
    closedir(d);
    return h;
}

static uint64_t compute_sys_tree_hash(void)
{
    uint64_t h = 14695981039346656037ULL;
    h = hash_dir_tree("/mnt/sdcard/.system", h);
    h = hash_single_file("/mnt/sdcard/.rockbox/rockbox", h);
    h = hash_single_file("/mnt/sdcard/.meui/meui", h);
    h = hash_single_file("/mnt/sdcard/.custom/launch", h);
    return h;
}

static void check_sys_tree_modified_and_reboot(void)
{
    if (!g_pre_usb_sys_valid)
        return;

    g_pre_usb_sys_valid = false;
    uint64_t post_hash = compute_sys_tree_hash();
    if (post_hash != g_pre_usb_sys_hash) {
        y1_log("[liby1-usb] .system or app binary modified over USB; rebooting...\n");
        sync();
        y1_sys_reboot();
    }
}

static void write_usb_line(const char *path, const char *value)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s\n", value ? value : "");
    y1_sysfs_write(path, buf);
}

static void read_active_scheduler(const char *path, char *out, size_t out_sz)
{
    char buf[256];
    if (!y1_sysfs_read(path, buf, sizeof(buf)))
        return;

    char *lb = strchr(buf, '[');
    char *rb = lb ? strchr(lb + 1, ']') : NULL;
    if (lb && rb && (size_t)(rb - lb - 1) < out_sz) {
        size_t n = (size_t)(rb - lb - 1);
        memcpy(out, lb + 1, n);
        out[n] = '\0';
    }
}

void y1_usb_tune_performance(bool enable)
{
    static const char * const sched_path = "/sys/block/mmcblk1/queue/scheduler";
    static const char * const ra_path    = "/sys/block/mmcblk1/queue/read_ahead_kb";
    static const char * const bdi_ra     = "/sys/block/mmcblk1/bdi/read_ahead_kb";

    if (enable) {
        if (!saved_queue_params) {
            read_active_scheduler(sched_path, saved_scheduler, sizeof(saved_scheduler));
            char ra_buf[16];
            if (y1_sysfs_read(ra_path, ra_buf, sizeof(ra_buf))) {
                int v = atoi(ra_buf);
                if (v > 0)
                    snprintf(saved_read_ahead, sizeof(saved_read_ahead), "%d", v);
            }
            saved_queue_params = true;
        }

        if (!y1_sysfs_write(sched_path, "noop\n"))
            y1_sysfs_write(sched_path, "deadline\n");

        y1_sysfs_write(ra_path, "2048\n");
        y1_sysfs_write(bdi_ra, "2048\n");
        if (!y1_sysfs_write("/sys/block/mmcblk1/queue/nr_requests", "512\n"))
            y1_sysfs_write("/sys/block/mmcblk1/queue/nr_requests", "256\n");
        y1_sysfs_write("/sys/block/mmcblk1/queue/iostats", "0\n");
        y1_sysfs_write("/sys/block/mmcblk1/queue/add_random", "0\n");
        y1_sysfs_write("/sys/block/mmcblk1/queue/nomerges", "0\n");

        int hw_max = 0;
        if (y1_sysfs_read_int("/sys/block/mmcblk1/queue/max_hw_sectors_kb", &hw_max) && hw_max > 128) {
            int target_kb = (hw_max > 512) ? 512 : hw_max;
            char kb_str[16];
            snprintf(kb_str, sizeof(kb_str), "%d\n", target_kb);
            y1_sysfs_write("/sys/block/mmcblk1/queue/max_sectors_kb", kb_str);
        }

        if (!usb_perf_boosted) {
            y1_sysfs_write("/proc/cpufreq/usb_raise_freq", "1\n");
            y1_cpu_boost_for_conversion(true);
            usb_perf_boosted = true;
        }
        y1_log("[liby1-usb] USB 2.0 mass-storage performance tuning enabled (noop, 2MB RA, 1.3GHz dual-core)\n");
    } else {
        if (saved_queue_params) {
            write_usb_line(sched_path, saved_scheduler);
            write_usb_line(ra_path, saved_read_ahead);
            write_usb_line(bdi_ra, saved_read_ahead);
        } else {
            y1_sysfs_write(sched_path, "cfq\n");
            y1_sysfs_write(ra_path, "128\n");
            y1_sysfs_write(bdi_ra, "128\n");
        }
        y1_sysfs_write("/sys/block/mmcblk1/queue/nr_requests", "128\n");
        y1_sysfs_write("/sys/block/mmcblk1/queue/iostats", "1\n");

        if (usb_perf_boosted) {
            y1_sysfs_write("/proc/cpufreq/usb_raise_freq", "0\n");
            y1_cpu_boost_for_conversion(false);
            usb_perf_boosted = false;
        }
    }
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
        y1_sysfs_write(cdrom_paths[i], "0");
    }
    if (!inq)
        return;
    size_t len = strlen(inq);
    if (len > 28)
        len = 28;
    for (int i = 0; inq_paths[i]; i++) {
        int fd = open(inq_paths[i], O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t w = write(fd, inq, len);
            (void)w;
            close(fd);
        }
    }
}

static const char *find_sd_device(void)
{
    if (access("/dev/block/mmcblk1", F_OK) == 0)
        return "/dev/block/mmcblk1";
    if (access("/dev/mmcblk1", F_OK) == 0)
        return "/dev/mmcblk1";
    if (access("/dev/block/mmcblk1p1", F_OK) == 0)
        return "/dev/block/mmcblk1p1";
    if (access("/dev/mmcblk1p1", F_OK) == 0)
        return "/dev/mmcblk1p1";
    return NULL;
}

static const char *find_ext_device(void)
{
    if (access("/dev/block/mmcblk1p2", F_OK) == 0)
        return "/dev/block/mmcblk1p2";
    if (access("/dev/mmcblk1p2", F_OK) == 0)
        return "/dev/mmcblk1p2";
    return NULL;
}

bool y1_usb_is_serial_active(void)
{
    char buf[64];
    if (y1_sysfs_read("/sys/class/android_usb/android0/functions", buf, sizeof(buf))) {
        if (strstr(buf, "acm") != NULL)
            return true;
    }
    return false;
}

#define Y1_IPOD_MODE_FLAG_PATH "/mnt/sdcard/.system/ipod_usb_mode"
#define Y1_ROCKBOX_CFG_PATH    "/mnt/sdcard/.rockbox/config.cfg"
#define IPOD_5G_VID            "05ac"
#define IPOD_5G_PID            "1209"
#define IPOD_5G_BCD_DEVICE     "0002"
#define IPOD_5G_MANUFACTURER   "Apple"
#define IPOD_5G_PRODUCT        "iPod"
#define IPOD_5G_DEFAULT_GUID   "000A270019B48C1F"
#define IPOD_5G_INQUIRY        "Apple___iPod____________1.62"

static int g_ipod_usb_mode = -1;

void y1_usb_set_ipod_mode(bool enabled)
{
    g_ipod_usb_mode = enabled ? 1 : 0;
    FILE *fp = fopen(Y1_IPOD_MODE_FLAG_PATH, "w");
    if (fp) {
        fprintf(fp, "%d\n", g_ipod_usb_mode);
        fclose(fp);
    }
    y1_logf("[liby1-usb] iPod 5.5G USB mode set to %s\n", enabled ? "ON" : "OFF");
}

bool y1_usb_get_ipod_mode(void)
{
    if (g_ipod_usb_mode >= 0)
        return g_ipod_usb_mode != 0;

    char buf[32];
    if (y1_sysfs_read(Y1_IPOD_MODE_FLAG_PATH, buf, sizeof(buf))) {
        g_ipod_usb_mode = (atoi(buf) != 0) ? 1 : 0;
        return g_ipod_usb_mode != 0;
    }

    FILE *cfg = fopen(Y1_ROCKBOX_CFG_PATH, "r");
    if (cfg) {
        char line[256];
        while (fgets(line, sizeof(line), cfg)) {
            if (strncmp(line, "y1 ipod usb mode:", 17) == 0) {
                const char *val = line + 17;
                while (*val == ' ' || *val == '\t') val++;
                g_ipod_usb_mode = (strncmp(val, "on", 2) == 0 || *val == '1') ? 1 : 0;
                fclose(cfg);
                return g_ipod_usb_mode != 0;
            }
        }
        fclose(cfg);
    }

    g_ipod_usb_mode = 0;
    return false;
}

static void read_ipod_usb_serial(char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s", IPOD_5G_DEFAULT_GUID);
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
            size_t n = 0;
            while (n + 1 < out_sz &&
                   ((p[n] >= '0' && p[n] <= '9') ||
                    (p[n] >= 'a' && p[n] <= 'f') ||
                    (p[n] >= 'A' && p[n] <= 'F'))) {
                char c = p[n];
                if (c >= 'a' && c <= 'f')
                    c = (char)(c - 'a' + 'A');
                out[n++] = c;
            }
            if (n >= 8)
                out[n] = '\0';
            else
                snprintf(out, out_sz, "%s", IPOD_5G_DEFAULT_GUID);
            break;
        }
    }
    fclose(fp);
}

bool y1_usb_attach_mass_storage(void)
{
    if (y1_usb_is_serial_active())
        return false;

    const char *target = find_sd_device();
    if (!target)
        return false;

    bool ipod_mode = y1_usb_get_ipod_mode();
    if (ipod_mode)
        y1_ipod_ensure_device_files();

    sync();

    write_usb_line("/sys/class/android_usb/android0/enable", "0");
    write_usb_line("/sys/class/android_usb/android0/functions", "mass_storage");
    write_usb_line("/sys/class/android_usb/android0/bDeviceClass", "0");
    write_usb_line("/sys/class/android_usb/android0/bDeviceSubClass", "0");
    write_usb_line("/sys/class/android_usb/android0/bDeviceProtocol", "0");
    if (ipod_mode) {
        char serial[32];
        read_ipod_usb_serial(serial, sizeof(serial));

        if (access("/lib/modules/ipod_vpd.ko", F_OK) == 0 &&
            access("/sys/module/ipod_vpd", F_OK) != 0) {
            char dev_serial[32] = "8K7289LAV9K";
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
                     dev_serial, serial);
            (void)!system(cmd);
        }

        write_usb_line("/sys/class/android_usb/android0/idVendor", IPOD_5G_VID);
        write_usb_line("/sys/class/android_usb/android0/idProduct", IPOD_5G_PID);
        write_usb_line("/sys/class/android_usb/android0/bcdDevice", IPOD_5G_BCD_DEVICE);
        write_usb_line("/sys/class/android_usb/android0/iManufacturer", IPOD_5G_MANUFACTURER);
        write_usb_line("/sys/class/android_usb/android0/iProduct", IPOD_5G_PRODUCT);
        write_usb_line("/sys/class/android_usb/android0/iSerial", serial);
        write_inquiry_string(IPOD_5G_INQUIRY);
        /* Disable MediaTek BICR (Built-in CD-ROM) so the device presents as a
         * Direct Access Disk (SCSI TYPE_DISK 0x00), NOT a virtual CD-ROM ("File-CD Gadget").
         */
        write_usb_line("/sys/class/android_usb/android0/f_mass_storage/bicr", "0");
        write_usb_line("/sys/class/android_usb/android0/f_mass_storage/nluns", "1");
        write_usb_line("/sys/class/android_usb/android0/f_mass_storage/luns", "1");
        y1_logf("[liby1-usb] Spoofing iPod 5.5G (VID=%s PID=%s Serial=%s, Single LUN)\n",
                IPOD_5G_VID, IPOD_5G_PID, serial);
    } else {
        if (access("/sys/module/ipod_vpd", F_OK) == 0)
            (void)!system("rmmod ipod_vpd");
        write_usb_line("/sys/class/android_usb/android0/idVendor", "0e8d");
        write_usb_line("/sys/class/android_usb/android0/idProduct", "0001");
        write_usb_line("/sys/class/android_usb/android0/bcdDevice", "0100");
        write_usb_line("/sys/class/android_usb/android0/iManufacturer", "Innioasis");
        write_usb_line("/sys/class/android_usb/android0/iProduct", "Y1 Player");
        write_usb_line("/sys/class/android_usb/android0/iSerial", "0123456789ABCDEF");
        write_inquiry_string("Innioasis_Y1_Player_____0100");
        write_usb_line("/sys/class/android_usb/android0/f_mass_storage/bicr", "0");
    }

    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun0/file", target);
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun/file", target);
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun0/nofua", "1");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun/nofua", "1");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun0/ro", "0");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun/ro", "0");

    /* iTunes / Apple iPod requires strictly 1 LUN. Secondary LUN causes iTunes detection to fail. */
    if (!ipod_mode) {
        const char *ext = find_ext_device();
        if (ext) {
            write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", ext);
            write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/nofua", "1");
            write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/ro", "0");
        } else {
            write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "");
        }
    } else {
        write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "");
    }

    g_pre_usb_sys_hash = compute_sys_tree_hash();
    g_pre_usb_sys_valid = true;

    y1_usb_tune_performance(true);
    write_usb_line("/sys/class/android_usb/android0/enable", "1");
    storage_attached = true;
    return true;
}

void y1_usb_detach_mass_storage(void)
{
    if (y1_usb_is_serial_active())
        return;

    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun0/file", "");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun/file", "");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/bicr", "0");
    write_usb_line("/sys/class/android_usb/android0/enable", "0");
    write_usb_line("/proc/sys/vm/drop_caches", "3");
    y1_usb_tune_performance(false);
    storage_attached = false;
    check_sys_tree_modified_and_reboot();
}

bool y1_usb_is_storage_attached(void)
{
    return storage_attached;
}

bool y1_usb_check_host_ejected(void)
{
    if (!storage_attached)
        return false;

    char file[128];
    bool read_ok = y1_sysfs_read("/sys/class/android_usb/android0/f_mass_storage/lun0/file",
                                 file, sizeof(file));
    if (!read_ok)
        read_ok = y1_sysfs_read("/sys/class/android_usb/android0/f_mass_storage/lun/file",
                                file, sizeof(file));

    if (read_ok && file[0] == '\0') {
        y1_usb_tune_performance(false);
        storage_attached = false;
        check_sys_tree_modified_and_reboot();
        return true;
    }
    return false;
}

