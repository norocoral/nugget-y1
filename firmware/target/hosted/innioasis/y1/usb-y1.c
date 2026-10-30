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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "disk.h"
#include "usb.h"
#include "sysfs.h"
#include "power.h"
#include "usb-y1.h"

static bool y1_charge_only = false;
static bool y1_storage_attached = false;

static void y1_sysfs_write_line(const char *path, const char *value)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s\n", value ? value : "");
    sysfs_set_string(path, buf);
}

static const char *y1_find_sd_device(void)
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

static const char *y1_find_ext_device(void)
{
    if (access("/dev/block/mmcblk1p2", F_OK) == 0)
        return "/dev/block/mmcblk1p2";
    if (access("/dev/mmcblk1p2", F_OK) == 0)
        return "/dev/mmcblk1p2";
    return NULL;
}

bool y1_is_charge_only_mode(void)
{
    return y1_charge_only;
}

void y1_usb_attach_storage(void)
{
    const char *target = y1_find_sd_device();
    if (!target)
        return;

    /* 1. Temporarily disable gadget to ensure clean LUN re-binding and host re-enumeration */
    y1_sysfs_write_line("/sys/class/android_usb/android0/enable", "0");

    /* 2. Configure gadget parameters */
    y1_sysfs_write_line("/sys/class/android_usb/android0/functions", "mass_storage");
    y1_sysfs_write_line("/sys/class/android_usb/android0/idVendor", "0e8d");
    y1_sysfs_write_line("/sys/class/android_usb/android0/idProduct", "0001");
    y1_sysfs_write_line("/sys/class/android_usb/android0/iManufacturer", "Innioasis");
    y1_sysfs_write_line("/sys/class/android_usb/android0/iProduct", "Y1 Rockbox Player");

    /* 3. Bind primary LUN0 (Physical MicroSD) */
    y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun0/file", target);
    y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun/file", target);
    y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun0/nofua", "1");
    y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun/nofua", "1");
    y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun0/ro", "0");
    y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun/ro", "0");

    /* 4. Bind secondary LUN1 if partition 2 exists */
    const char *ext = y1_find_ext_device();
    if (ext)
    {
        y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", ext);
        y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun1/nofua", "1");
        y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun1/ro", "0");
    }
    else
    {
        y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "");
    }

    /* 5. Enable gadget to present the media to the host PC */
    y1_sysfs_write_line("/sys/class/android_usb/android0/enable", "1");
    y1_storage_attached = true;
}

bool y1_usb_check_host_ejected(void)
{
    if (y1_charge_only || !y1_storage_attached)
        return false;

    char file[128];
    bool read_ok = sysfs_get_string("/sys/class/android_usb/android0/f_mass_storage/lun0/file",
                                    file, sizeof(file));
    if (!read_ok)
        read_ok = sysfs_get_string("/sys/class/android_usb/android0/f_mass_storage/lun/file",
                                   file, sizeof(file));

    if (read_ok && file[0] == '\0')
    {
        /* The kernel closed the backing file because the host PC sent SCSI EJECT */
        y1_storage_attached = false;
        return true;
    }

    return false;
}

void y1_usb_eject_to_charge_only(void)
{
    y1_charge_only = true;
    y1_storage_attached = false;

    /* Detach storage backing files from Linux USB gadget and disable gadget */
    y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun0/file", "");
    y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun/file", "");
    y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "");
    y1_sysfs_write_line("/sys/class/android_usb/android0/enable", "0");

    sync();

    /* Release exclusive storage to remount storage in Rockbox and broadcast disconnect */
    usb_release_exclusive_storage();
}

int usb_detect(void)
{
    static long last_usb_check_tick = 0;
    static int cached_present = 0;
    extern volatile long current_tick;
    extern bool y1_is_screen_sleeping(void);

    long now = current_tick;
    long interval = y1_is_screen_sleeping() ? (2 * HZ) : (HZ / 2);

    if (last_usb_check_tick == 0 || !TIME_BEFORE(now, last_usb_check_tick + interval))
    {
        last_usb_check_tick = now ? now : 1;
        int present = 0;
        if (!sysfs_get_int("/sys/class/power_supply/usb/online", &present))
            sysfs_get_int("/sys/class/power_supply/ac/online", &present);
        cached_present = present;
    }

    if (!cached_present)
    {
        /* Physical cable unplugged: reset charge-only mode for next connection */
        y1_charge_only = false;
        y1_storage_attached = false;
        return USB_EXTRACTED;
    }

    if (y1_charge_only)
        return USB_EXTRACTED;

    return USB_INSERTED;
}

void usb_enable(bool on)
{
    if (on)
    {
        y1_usb_attach_storage();
    }
    else
    {
        /* Detach storage backing file and disable gadget when USB is unplugged so usb_wakelock is released */
        y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun0/file", "");
        y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun/file", "");
        y1_sysfs_write_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "");
        y1_sysfs_write_line("/sys/class/android_usb/android0/enable", "0");
        y1_storage_attached = false;
    }
}

void usb_init_device(void)
{
    if (usb_detect() == USB_INSERTED)
        y1_usb_attach_storage();
    else
        usb_enable(false);
}

int disk_mount_all(void)
{
    sync();
    return 1;
}

int disk_unmount_all(void)
{
    y1_usb_attach_storage();
    return 1;
}
