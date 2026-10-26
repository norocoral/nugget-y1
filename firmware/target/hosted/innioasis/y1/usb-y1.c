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
#include "usb-y1.h"

static bool y1_charge_only = false;

bool y1_is_charge_only_mode(void)
{
    return y1_charge_only;
}

void y1_usb_eject_to_charge_only(void)
{
    y1_charge_only = true;

    /* Detach storage backing files from Linux USB gadget and disable gadget */
    sysfs_set_string("/sys/class/android_usb/android0/f_mass_storage/lun0/file", "");
    sysfs_set_string("/sys/class/android_usb/android0/f_mass_storage/lun/file", "");
    sysfs_set_string("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "");
    sysfs_set_string("/sys/class/android_usb/android0/enable", "0");

    /* Release exclusive storage to remount storage in Rockbox and broadcast disconnect */
    usb_release_exclusive_storage();
}

int usb_detect(void)
{
    int present = 0;
    if (!sysfs_get_int("/sys/class/power_supply/usb/online", &present))
        sysfs_get_int("/sys/class/power_supply/ac/online", &present);

    if (!present)
    {
        /* Physical cable unplugged: reset charge-only mode for next connection */
        y1_charge_only = false;
        return USB_EXTRACTED;
    }

    if (y1_charge_only)
        return USB_EXTRACTED;

    return USB_INSERTED;
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

