/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Innioasis Y1 CPU Clock and Governor Control (MediaTek MT6572)
 * Writes to Linux sysfs cpufreq interface for cpu0 and cpu1.
 ****************************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "config.h"
#include "cpufreq-y1.h"
#include "sysfs.h"
#include "settings.h"

static int _current_cpu_mode = -1;
static int _saved_cpu_mode = -1;
static int _saved_cpu1_online = -1;

static int cpu1_is_online(void)
{
    int online = 1;
    int fd = open("/sys/devices/system/cpu/cpu1/online", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[4] = { 0 };
        if (read(fd, buf, sizeof(buf) - 1) > 0)
            online = (buf[0] != '0');
        close(fd);
    }
    return online;
}

void y1_set_screen_off_power_mode(bool screen_off)
{
    if (screen_off) {
        if (!global_settings.y1_screen_off_cpu_save)
            return;
        if (_saved_cpu_mode == -1) {
            _saved_cpu_mode = _current_cpu_mode >= 0 ? _current_cpu_mode : Y1_CPU_DYNAMIC;
        }
        if (_saved_cpu1_online == -1)
            _saved_cpu1_online = cpu1_is_online();
        /* 598 MHz + single core while the panel is off (play or idle). */
        y1_set_cpu_mode(Y1_CPU_POWERSAVE);
        y1_set_cpu_cores_setting(1);
    } else {
        if (_saved_cpu1_online != -1) {
            y1_set_cpu_cores_setting(_saved_cpu1_online ? 0 : 1);
            _saved_cpu1_online = -1;
        }
        if (_saved_cpu_mode != -1) {
            int restore = _saved_cpu_mode;
            _saved_cpu_mode = -1;
            _current_cpu_mode = -1;
            y1_set_cpu_mode(restore);
        }
    }
}

static void write_cpu_node(int cpu, const char *node, const char *val)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, node);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        write(fd, val, strlen(val));
        close(fd);
    }
}

void y1_set_cpu_mode(int mode)
{
    if (_current_cpu_mode == mode)
        return;

    _current_cpu_mode = mode;

    for (int cpu = 0; cpu < 2; cpu++) {
        switch (mode) {
        case Y1_CPU_POWERSAVE:
            /* Fixed 598 MHz (Underclock / Maximum Battery Life) */
            write_cpu_node(cpu, "scaling_governor", "powersave\n");
            write_cpu_node(cpu, "scaling_min_freq", "598000\n");
            write_cpu_node(cpu, "scaling_max_freq", "598000\n");
            write_cpu_node(cpu, "scaling_setspeed", "598000\n");
            break;

        case Y1_CPU_BALANCED:
            /* Fixed 1.0 GHz (Normal / Balanced) */
            write_cpu_node(cpu, "scaling_governor", "userspace\n");
            write_cpu_node(cpu, "scaling_min_freq", "1001000\n");
            write_cpu_node(cpu, "scaling_max_freq", "1001000\n");
            write_cpu_node(cpu, "scaling_setspeed", "1001000\n");
            break;

        case Y1_CPU_PERFORMANCE:
            /* Fixed 1.2 GHz (Stock High Performance) */
            write_cpu_node(cpu, "scaling_governor", "userspace\n");
            write_cpu_node(cpu, "scaling_min_freq", "1196000\n");
            write_cpu_node(cpu, "scaling_max_freq", "1196000\n");
            write_cpu_node(cpu, "scaling_setspeed", "1196000\n");
            break;

        case Y1_CPU_TURBO:
            /* Fixed 1.3 GHz (Turbo Overclock) */
            write_cpu_node(cpu, "scaling_governor", "performance\n");
            write_cpu_node(cpu, "scaling_min_freq", "1300000\n");
            write_cpu_node(cpu, "scaling_max_freq", "1300000\n");
            write_cpu_node(cpu, "scaling_setspeed", "1300000\n");
            break;

        case Y1_CPU_DYNAMIC:
        default:
            /* Dynamic Scaling (Ondemand 598MHz - 1300MHz) */
            write_cpu_node(cpu, "scaling_governor", "ondemand\n");
            write_cpu_node(cpu, "scaling_min_freq", "598000\n");
            write_cpu_node(cpu, "scaling_max_freq", "1300000\n");
            break;
        }
    }
}

int y1_get_current_freq(void)
{
    int freq = 0;
    if (sysfs_get_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &freq)) {
        return freq / 1000; /* MHz */
    }
    return 1001;
}

void y1_set_cpu_cores_setting(int mode)
{
    /* mode 0: Dual-Core (online=1), mode 1: Single-Core (online=0) */
    int fd = open("/sys/devices/system/cpu/cpu1/online", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (mode == 1) {
            (void)!write(fd, "0\n", 2);
        } else {
            (void)!write(fd, "1\n", 2);
        }
        close(fd);
    }
}
