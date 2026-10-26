/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Innioasis Y1 CPU Clock and Governor Control (MediaTek MT6572)
 * Supports dynamic scaling, power saver underclock (598MHz),
 * balanced normal (1.0GHz), stock (1.2GHz), and turbo overclock (1.3GHz).
 ****************************************************************************/

#ifndef _CPUFREQ_Y1_H_
#define _CPUFREQ_Y1_H_

#include <stdbool.h>

enum y1_cpu_modes
{
    Y1_CPU_DYNAMIC = 0,     /* Ondemand: 598MHz - 1300MHz dynamic scaling */
    Y1_CPU_POWERSAVE,       /* 598MHz: fixed minimum, maximum battery saving (underclock) */
    Y1_CPU_BALANCED,        /* 1001MHz: fixed 1.0GHz, smooth UI and audio */
    Y1_CPU_PERFORMANCE,     /* 1196MHz: fixed 1.2GHz stock peak */
    Y1_CPU_TURBO,           /* 1300MHz: fixed 1.3GHz maximum overclock */
    Y1_CPU_MODE_COUNT
};

/* Apply CPU scaling mode (0..4) to both MT6572 Cortex-A7 cores */
void y1_set_cpu_mode(int mode);

/* CPU core control: 0 = Dual-Core, 1 = Single-Core (disable CPU 1) */
void y1_set_cpu_cores_setting(int mode);

/* Get current CPU frequency in MHz */
int y1_get_current_freq(void);

/* Screen off power throttle: drop to 598MHz while screen is off, restore on wake */
void y1_set_screen_off_power_mode(bool screen_off);

/* Transition from boot-time maximum clock boost to user-configured CPU settings */
void y1_boot_boost_finish(void);

#endif /* _CPUFREQ_Y1_H_ */
