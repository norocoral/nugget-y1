/***************************************************************************
 * liby1 - CPU Frequency Scaling & Dual-Core Hotplug Control (MT6572)
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "liby1.h"

static int current_cpu_mode = -1;
static int saved_cpu_mode = -1;
static int saved_cpu1_online = -1;

static void write_cpu_node(int cpu, const char *node, const char *val)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, node);
    y1_sysfs_write(path, val);
}

static void write_cpu_freq_bounds(int cpu, const char *min_f, const char *max_f)
{
    write_cpu_node(cpu, "scaling_min_freq", "598000\n");
    write_cpu_node(cpu, "scaling_max_freq", max_f);
    write_cpu_node(cpu, "scaling_min_freq", min_f);
}

void y1_cpu_set_mode(int mode)
{
    if (current_cpu_mode == mode)
        return;

    current_cpu_mode = mode;

    for (int cpu = 0; cpu < 2; cpu++) {
        switch (mode) {
        case Y1_CPU_MODE_POWERSAVE:
            /* Fixed 598 MHz (Lowest VCORE 1.05V) */
            y1_sysfs_write("/proc/cpufreq/usb_raise_freq", "0\n");
            y1_sysfs_write("/proc/cpufreq/MTK_PPM_POLICY", "DISABLE\n");
            y1_sysfs_write("/proc/cpufreq/MTK_PPM_POLICY", "0\n");
            write_cpu_freq_bounds(cpu, "598000\n", "598000\n");
            write_cpu_node(cpu, "scaling_governor", "userspace\n");
            write_cpu_node(cpu, "scaling_setspeed", "598000\n");
            y1_sysfs_write("/proc/cpufreq/cpufreq_cur_freq", "598000\n");
            break;

        case Y1_CPU_MODE_BALANCED:
            /* Fixed 1.0 GHz */
            y1_sysfs_write("/proc/cpufreq/MTK_PPM_POLICY", "DISABLE\n");
            y1_sysfs_write("/proc/cpufreq/MTK_PPM_POLICY", "0\n");
            write_cpu_freq_bounds(cpu, "1001000\n", "1001000\n");
            write_cpu_node(cpu, "scaling_governor", "userspace\n");
            write_cpu_node(cpu, "scaling_setspeed", "1001000\n");
            break;

        case Y1_CPU_MODE_PERFORMANCE:
            /* Fixed 1.2 GHz */
            write_cpu_freq_bounds(cpu, "1196000\n", "1196000\n");
            write_cpu_node(cpu, "scaling_governor", "userspace\n");
            write_cpu_node(cpu, "scaling_setspeed", "1196000\n");
            break;

        case Y1_CPU_MODE_TURBO:
            /* Fixed 1.3 GHz */
            write_cpu_freq_bounds(cpu, "1300000\n", "1300000\n");
            write_cpu_node(cpu, "scaling_governor", "performance\n");
            write_cpu_node(cpu, "scaling_setspeed", "1300000\n");
            break;

        case Y1_CPU_MODE_DYNAMIC:
        default:
            /* Ondemand 598 MHz - 1001 MHz */
            y1_sysfs_write("/proc/cpufreq/usb_raise_freq", "0\n");
            y1_sysfs_write("/proc/cpufreq/MTK_PPM_POLICY", "DISABLE\n");
            y1_sysfs_write("/proc/cpufreq/MTK_PPM_POLICY", "0\n");
            write_cpu_freq_bounds(cpu, "598000\n", "1001000\n");
            write_cpu_node(cpu, "scaling_governor", "ondemand\n");
            break;
        }
    }

    if (mode == Y1_CPU_MODE_DYNAMIC) {
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/up_threshold", "98\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/sampling_down_factor", "1\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/sampling_rate", "100000\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/io_is_busy", "0\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/powersave_bias", "200\n");
    }
}

int y1_cpu_get_mode(void)
{
    return current_cpu_mode >= 0 ? current_cpu_mode : Y1_CPU_MODE_DYNAMIC;
}

void y1_cpu_set_dual_core(bool dual_core)
{
    if (!dual_core) {
        /* Disarm MediaTek hotplug drivers and power down Core 1 */
        y1_sysfs_write("/proc/mtk_hotplug/enable", "0\n");
        y1_sysfs_write("/sys/module/mt_hotplug_mechanism/parameters/g_enable", "0\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/hotplug/cpu_hotplug_disable", "1\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_base", "1\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_limit", "1\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpu1/online", "0\n");
    } else {
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/hotplug/cpu_hotplug_disable", "0\n");
        y1_sysfs_write("/proc/mtk_hotplug/enable", "1\n");
        y1_sysfs_write("/sys/module/mt_hotplug_mechanism/parameters/g_enable", "1\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_base", "1\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpufreq/hotplug/cpu_num_limit", "2\n");
        y1_sysfs_write("/sys/devices/system/cpu/cpu1/online", "1\n");
    }
}

bool y1_cpu_is_dual_core(void)
{
    int online = 1;
    if (y1_sysfs_read_int("/sys/devices/system/cpu/cpu1/online", &online))
        return online != 0;
    return true;
}

int y1_cpu_get_freq_mhz(void)
{
    int freq = 0;
    if (y1_sysfs_read_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &freq)) {
        return freq / 1000;
    }
    return 1001;
}

void y1_cpu_set_screen_off_clamp(bool screen_off)
{
    if (screen_off) {
        if (saved_cpu_mode == -1) {
            saved_cpu_mode = current_cpu_mode >= 0 ? current_cpu_mode : Y1_CPU_MODE_DYNAMIC;
        }
        if (saved_cpu1_online == -1) {
            saved_cpu1_online = y1_cpu_is_dual_core() ? 1 : 0;
        }
        y1_log("[liby1-cpu] screen-off: clamping strictly to 598 MHz single-core (lowest VCORE)\n");
        y1_sysfs_write("/proc/cpufreq/usb_raise_freq", "0\n");
        y1_sysfs_write("/proc/cpufreq/MTK_PPM_POLICY", "DISABLE\n");
        y1_sysfs_write("/proc/cpufreq/MTK_PPM_POLICY", "0\n");
        current_cpu_mode = -1;
        y1_cpu_set_mode(Y1_CPU_MODE_POWERSAVE);
        y1_cpu_set_dual_core(false);
        y1_sysfs_write("/proc/cpufreq/cpufreq_cur_freq", "598000\n");
    } else {
        if (saved_cpu1_online != -1) {
            y1_cpu_set_dual_core(saved_cpu1_online != 0);
            saved_cpu1_online = -1;
        }
        if (saved_cpu_mode != -1) {
            int restore = saved_cpu_mode;
            saved_cpu_mode = -1;
            current_cpu_mode = -1;
            y1_logf("[liby1-cpu] screen awake: restoring CPU mode %d\n", restore);
            y1_cpu_set_mode(restore);
        }
    }
}

