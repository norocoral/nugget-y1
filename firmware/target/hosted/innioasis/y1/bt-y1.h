/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox Innioasis Y1 Port
 * Native MediaTek MT6572 Bluetooth HCI + L2CAP + SDP + A2DP SBC + AVRCP Driver
 ****************************************************************************/

#ifndef _BT_Y1_H_
#define _BT_Y1_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define Y1_BT_MAX_DEVICES 16

enum y1_bt_state {
    Y1_BT_STATE_OFF = 0,
    Y1_BT_STATE_INIT,
    Y1_BT_STATE_IDLE,
    Y1_BT_STATE_SCANNING,
    Y1_BT_STATE_CONNECTING,
    Y1_BT_STATE_CONNECTED,
    Y1_BT_STATE_STREAMING,
    Y1_BT_STATE_ERROR
};

struct y1_bt_device {
    uint8_t bdaddr[6]; /* Little-endian HCI order */
    char    name[64];
    int8_t  rssi;
    bool    paired;
    bool    connected;
    uint8_t link_key[16];
    uint8_t key_type;
};

void y1_bt_init(void);
void y1_bt_set_enabled(bool enable);
bool y1_bt_is_enabled(void);
enum y1_bt_state y1_bt_get_state(void);
const char *y1_bt_get_status_str(void);
const char *y1_bt_get_connected_name(void);

int  y1_bt_start_scan(void);
bool y1_bt_is_scanning(void);
int  y1_bt_get_devices(struct y1_bt_device *out_devs, int max_devs);

int  y1_bt_connect_device(const uint8_t bdaddr[6]);
void y1_bt_disconnect(void);
void y1_bt_forget_device(const uint8_t bdaddr[6]);

bool y1_bt_a2dp_is_streaming(void);
void y1_bt_a2dp_write_pcm(const void *buf, size_t bytes, unsigned long sampr);

#endif /* _BT_Y1_H_ */

