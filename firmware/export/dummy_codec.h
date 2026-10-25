/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 *
 * Copyright (c) 2011 Andrew Ryabinin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#ifndef __DUMMY_CODEC_H_
#define __DUMMY_CODEC_H_

#define DUMMY_VOLUME_MIN    -74
#define DUMMY_VOLUME_MAX    6
AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -74, 6, 0)

#ifdef HAVE_RECORDING
AUDIOHW_SETTING(MIC_GAIN,   "dB", 0, 1, 0, 30, 12)
AUDIOHW_SETTING(LEFT_GAIN,  "dB", 0, 1, 0, 30, 12)
AUDIOHW_SETTING(RIGHT_GAIN, "dB", 0, 1, 0, 30, 12)
#endif

#endif /* __DUMMY_CODEC_H_ */
