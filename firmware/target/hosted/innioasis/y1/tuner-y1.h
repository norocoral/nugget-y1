/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox Innioasis Y1 Port
 * Innioasis Y1 FM Radio Tuner Driver Header
 ****************************************************************************/

#ifndef _TUNER_Y1_H_
#define _TUNER_Y1_H_

#ifndef CONFIG_TUNER_MULTI
#define tuner_set y1_tuner_set
#define tuner_get y1_tuner_get
#endif

void y1_tuner_init(void);
int y1_tuner_set(int setting, int value);
int y1_tuner_get(int setting);

#endif /* _TUNER_Y1_H_ */

