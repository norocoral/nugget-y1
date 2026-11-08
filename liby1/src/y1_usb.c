/***************************************************************************
 * liby1 - Android USB Mass Storage Gadget Driver (MT6572)
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "liby1.h"

static bool storage_attached = false;

static void write_usb_line(const char *path, const char *value)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s\n", value ? value : "");
    y1_sysfs_write(path, buf);
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

bool y1_usb_attach_mass_storage(void)
{
    const char *target = find_sd_device();
    if (!target)
        return false;

    write_usb_line("/sys/class/android_usb/android0/enable", "0");

    write_usb_line("/sys/class/android_usb/android0/functions", "mass_storage");
    write_usb_line("/sys/class/android_usb/android0/idVendor", "0e8d");
    write_usb_line("/sys/class/android_usb/android0/idProduct", "0001");
    write_usb_line("/sys/class/android_usb/android0/iManufacturer", "Innioasis");
    write_usb_line("/sys/class/android_usb/android0/iProduct", "Y1 Player");

    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun0/file", target);
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun/file", target);
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun0/nofua", "1");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun/nofua", "1");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun0/ro", "0");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun/ro", "0");

    const char *ext = find_ext_device();
    if (ext) {
        write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", ext);
        write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/nofua", "1");
        write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/ro", "0");
    } else {
        write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "");
    }

    write_usb_line("/sys/class/android_usb/android0/enable", "1");
    storage_attached = true;
    return true;
}

void y1_usb_detach_mass_storage(void)
{
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun0/file", "");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun/file", "");
    write_usb_line("/sys/class/android_usb/android0/f_mass_storage/lun1/file", "");
    write_usb_line("/sys/class/android_usb/android0/enable", "0");
    storage_attached = false;
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
        storage_attached = false;
        return true;
    }
    return false;
}

