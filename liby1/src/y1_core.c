/***************************************************************************
 * liby1 - Core, Sysfs, Logging, Battery & Power Control
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/reboot.h>

#include "liby1.h"

static bool g_log_enabled = false;
static int  g_kmsg_fd = -1;
static int  g_dbg_fd = -1;

int y1_init(void)
{
    mkdir(Y1_SYSTEM_DIR, 0777);
    y1_fb_init();
    y1_backlight_set_percent(50);
    y1_input_set_touch_scanning(true);
    y1_input_init();
    return 0;
}

void y1_shutdown(void)
{
    y1_audio_close();
    y1_fm_power(false);
    y1_input_close();
    y1_fb_close();
    if (g_dbg_fd >= 0) {
        close(g_dbg_fd);
        g_dbg_fd = -1;
    }
    if (g_kmsg_fd >= 0) {
        close(g_kmsg_fd);
        g_kmsg_fd = -1;
    }
}

bool y1_sysfs_write(const char *path, const char *val)
{
    if (!path || !val)
        return false;
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t len = strlen(val);
    ssize_t w = write(fd, val, len);
    close(fd);
    return (w == (ssize_t)len);
}

bool y1_sysfs_write_int(const char *path, int val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d\n", val);
    return y1_sysfs_write(path, buf);
}

bool y1_sysfs_read(const char *path, char *buf, size_t size)
{
    if (!path || !buf || size == 0)
        return false;
    buf[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t r = read(fd, buf, size - 1);
    close(fd);
    if (r < 0)
        return false;
    buf[r] = '\0';
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r')) {
        buf[--r] = '\0';
    }
    return true;
}

bool y1_sysfs_read_int(const char *path, int *out_val)
{
    char buf[64];
    if (!y1_sysfs_read(path, buf, sizeof(buf)))
        return false;
    if (out_val)
        *out_val = atoi(buf);
    return true;
}

void y1_log_set_enabled(bool enabled)
{
    g_log_enabled = enabled;
    if (!enabled && g_dbg_fd >= 0) {
        close(g_dbg_fd);
        g_dbg_fd = -1;
    }
}

bool y1_log_is_enabled(void)
{
    return g_log_enabled;
}

void y1_log_raw(const char *msg, size_t len)
{
    if (!g_log_enabled) {
        if (g_dbg_fd >= 0) {
            close(g_dbg_fd);
            g_dbg_fd = -1;
        }
        return;
    }
    if (!msg || len == 0)
        return;

    (void)!write(1, msg, len);

    if (g_kmsg_fd < 0)
        g_kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (g_kmsg_fd >= 0)
        (void)!write(g_kmsg_fd, msg, len);

    if (g_dbg_fd < 0) {
        mkdir(Y1_SYSTEM_DIR, 0777);
        g_dbg_fd = open(Y1_DEBUG_LOG_PATH,
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    }
    if (g_dbg_fd >= 0) {
        if (write(g_dbg_fd, msg, len) < 0) {
            close(g_dbg_fd);
            g_dbg_fd = -1;
        }
    }
}

void y1_log(const char *msg)
{
    if (!g_log_enabled || !msg)
        return;
    y1_log_raw(msg, strlen(msg));
}

void y1_logf(const char *fmt, ...)
{
    if (!g_log_enabled || !fmt)
        return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len > 0) {
        if ((size_t)len >= sizeof(buf))
            len = (int)(sizeof(buf) - 1);
        y1_log_raw(buf, (size_t)len);
    }
}

int y1_battery_percent(void)
{
    int level = 0;
    y1_sysfs_read_int("/sys/class/power_supply/battery/capacity", &level);
    return level;
}

int y1_battery_voltage_mv(void)
{
    int uv = 0;
    if (y1_sysfs_read_int("/sys/class/power_supply/battery/voltage_now", &uv)) {
        /* MT6572 battery driver reports either millivolts or microvolts depending on node */
        return (uv > 10000) ? (uv / 1000) : uv;
    }
    return 0;
}

int y1_battery_current_ma(void)
{
    int ua = 0;
    if (y1_sysfs_read_int("/sys/class/power_supply/battery/current_now", &ua)) {
        return ua / 1000;
    }
    return 0;
}

bool y1_battery_is_charging(void)
{
    char buf[32];
    if (y1_sysfs_read("/sys/class/power_supply/battery/status", buf, sizeof(buf))) {
        return strncmp(buf, "Charging", 8) == 0;
    }
    return false;
}

bool y1_power_usb_online(void)
{
    int present = 0;
    if (y1_sysfs_read_int("/sys/class/power_supply/ac/online", &present) && present > 0)
        return true;
    if (y1_sysfs_read_int("/sys/class/power_supply/usb/online", &present) && present > 0)
        return true;
    return false;
}

void y1_sys_poweroff(void)
{
    y1_backlight_set_raw(0);
    y1_shutdown();
    sync();
    reboot(RB_POWER_OFF);
    (void)!system("/bin/busybox poweroff -f");
    (void)!system("/sbin/poweroff");
    while (1) {
        usleep(10000);
    }
}

void y1_sys_reboot(void)
{
    y1_backlight_set_raw(0);
    y1_shutdown();
    sync();
    reboot(RB_AUTOBOOT);
    (void)!system("/bin/busybox reboot -f");
    (void)!system("/sbin/reboot");
    while (1) {
        usleep(10000);
    }
}

