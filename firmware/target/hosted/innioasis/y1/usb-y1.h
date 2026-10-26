/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox Innioasis Y1 Port
 ****************************************************************************/

#ifndef _USB_Y1_H_
#define _USB_Y1_H_

#include <stdbool.h>

void y1_usb_eject_to_charge_only(void);
bool y1_is_charge_only_mode(void);

#endif /* _USB_Y1_H_ */

