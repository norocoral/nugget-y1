/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox Innioasis Y1 Port
 * USB management for minimal Linux on Innioasis Y1 (MT6572)
 ****************************************************************************/

#include <stdlib.h>
#include <stdbool.h>
#include "config.h"
#include "disk.h"
#include "usb.h"
#include "sysfs.h"
#include "power.h"

int usb_detect(void)
{
    int present = 0;
    if (!sysfs_get_int("/sys/class/power_supply/usb/online", &present))
        sysfs_get_int("/sys/class/power_supply/ac/online", &present);

    return present ? USB_INSERTED : USB_EXTRACTED;
}

void usb_enable(bool on)
{
    (void)on;
}

void usb_init_device(void)
{
}

int disk_mount_all(void)
{
    return 1;
}

int disk_unmount_all(void)
{
    return 1;
}

