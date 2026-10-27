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
#include <sys/types.h>

#define Y1_BT_MAX_DEVICES 16
#define Y1_BT_RECEIVER_ART_PATH "/mnt/sdcard/.rockbox/albumart/bt-receiver.jpg"

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
    char    pin[16];
    int     volume;    /* Remembered volume for this specific device */
};

enum y1_bt_receiver_command {
    Y1_BT_RECEIVER_PLAY_PAUSE,
    Y1_BT_RECEIVER_NEXT,
    Y1_BT_RECEIVER_PREVIOUS,
    Y1_BT_RECEIVER_SEEK_FORWARD,
    Y1_BT_RECEIVER_SEEK_BACKWARD
};

void y1_bt_init(void);
void y1_bt_set_enabled(bool enable);
bool y1_bt_is_enabled(void);
void y1_bt_set_receiver_mode(bool enable);
bool y1_bt_is_receiver_mode(void);
bool y1_bt_receiver_send_control(enum y1_bt_receiver_command command);
enum y1_bt_state y1_bt_get_state(void);
const char *y1_bt_get_status_str(void);
const char *y1_bt_get_connected_name(void);

int  y1_bt_start_scan(void);
bool y1_bt_is_scanning(void);
int  y1_bt_get_devices(struct y1_bt_device *out_devs, int max_devs);
int  y1_bt_get_paired_devices(struct y1_bt_device *out_devs, int max_devs);
int  y1_bt_get_new_devices(struct y1_bt_device *out_devs, int max_devs);

int  y1_bt_connect_device(const uint8_t bdaddr[6]);
int  y1_bt_connect_device_with_pin(const uint8_t bdaddr[6], const char *pin);
void y1_bt_disconnect(void);
void y1_bt_forget_device(const uint8_t bdaddr[6]);

bool y1_bt_a2dp_is_streaming(void);
uint8_t y1_bt_receiver_get_play_status(void);
uint32_t y1_bt_receiver_art_generation(void);
void y1_bt_a2dp_write_pcm(const void *buf, size_t bytes, unsigned long sampr);

void y1_bt_audio_disconnect_hook(void);
void y1_bt_audio_connect_hook(void);
void y1_bt_process_pending_audio_events(void);
void y1_bt_store_active_volume(int vol);
int  y1_bt_get_active_device_volume(void);
void y1_bt_update_metadata(const char *title, const char *artist,
                           const char *album, uint32_t duration_ms,
                           uint32_t position_ms, uint8_t play_status);
void y1_bt_get_receiver_metadata(char *title, size_t title_size,
                                 char *artist, size_t artist_size,
                                 char *album, size_t album_size);

#ifdef HAVE_RECORDING
bool y1_bt_is_connected(void);
void y1_bt_start_mic(void);
void y1_bt_stop_mic(void);
ssize_t y1_bt_read_mic_pcm(void *out_buf, size_t max_bytes);
#endif

#endif /* _BT_Y1_H_ */
