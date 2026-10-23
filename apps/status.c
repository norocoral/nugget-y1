/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
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
#include <string.h>
#include <stdbool.h>
#include "config.h"
#include "status.h"
#include "audio.h"
#if defined(INNIOASIS_Y1)
#include "bt-y1.h"
#endif
#if CONFIG_TUNER
#include "radio.h"
#endif

static enum playmode ff_mode = 0;

void status_set_ffmode(enum playmode mode)
{
    ff_mode = mode; /* Either STATUS_FASTFORWARD or STATUS_FASTBACKWARD */
}

enum playmode status_get_ffmode(void)
{
    /* only use this function for STATUS_FASTFORWARD or STATUS_FASTBACKWARD */
    /* use audio_status() for other modes */
    return ff_mode;
}

int current_playmode(void)
{
    int audio_stat = audio_status();

#if defined(INNIOASIS_Y1)
    if (y1_bt_is_receiver_mode()) {
        uint8_t play_status = y1_bt_receiver_get_play_status();
        if (play_status == 1 || play_status == 3 || play_status == 4)
            return STATUS_PLAY;
        if (play_status == 2)
            return STATUS_PAUSE;
        return STATUS_STOP;
    }
#endif

    /* ff_mode can be either STATUS_FASTFORWARD or STATUS_FASTBACKWARD
       and that supercedes the other modes */
    if(ff_mode)
        return ff_mode;
    
    if(audio_stat & AUDIO_STATUS_PLAY)
    {
        if(audio_stat & AUDIO_STATUS_PAUSE)
            return STATUS_PAUSE;
        else
            return STATUS_PLAY;
    }

#ifdef HAVE_RECORDING
    if(audio_stat & AUDIO_STATUS_RECORD)
    {
        if(audio_stat & AUDIO_STATUS_PAUSE)
            return STATUS_RECORD_PAUSE;
        else
            return STATUS_RECORD;
    }
#endif

#if CONFIG_TUNER
    audio_stat = get_radio_status();
    if(audio_stat & FMRADIO_PLAYING)
       return STATUS_RADIO;

    if(audio_stat & FMRADIO_PAUSED)
       return STATUS_RADIO_PAUSE;
#endif
    
    return STATUS_STOP;
}
