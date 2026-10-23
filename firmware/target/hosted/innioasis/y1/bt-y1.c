/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox Innioasis Y1 Port
 * Native MediaTek MT6572 Bluetooth HCI + L2CAP + SDP + A2DP SBC + AVRCP Metadata
 * Reverse-engineered from MT6572 Android 4.2.2 CONNSYS (/dev/stpwmt & /dev/stpbt)
 ****************************************************************************/

#define RB_FILESYSTEM_OS
#include "autoconf.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>

#ifndef makedev
#define makedev(maj, min) (((maj) << 8) | (min))
#endif

#include "system.h"
#include "kernel.h"
#include "button.h"
#include "settings.h"
#include "y1-log.h"
#include "bt-y1.h"
#include "pcm-eac.h"
#include "sbc/sbc.h"

#define PAIRED_DB_PATH   "/mnt/sdcard/.rockbox/bt_paired.dat"
#define PAIRED_MAGIC_V2  0x59314202U /* 'Y1B2' (v2 with pin and volume) */
#define PAIRED_MAGIC_V1  0x59314254U /* 'Y1BT' (legacy v1) */

/* HCI H4 Packet Types */
#define HCI_PKT_CMD  0x01
#define HCI_PKT_ACL  0x02
#define HCI_PKT_SCO  0x03
#define HCI_PKT_EVT  0x04

/* L2CAP PSMs */
#define PSM_SDP      0x0001
#define PSM_AVCTP    0x0017
#define PSM_AVDTP    0x0019
#define PSM_BIP      0x001B

/* L2CAP Signaling Commands */
#define L2CAP_CONN_REQ       0x02
#define L2CAP_CONN_RSP       0x03
#define L2CAP_CONF_REQ       0x04
#define L2CAP_CONF_RSP       0x05
#define L2CAP_DISCONN_REQ    0x06
#define L2CAP_DISCONN_RSP    0x07
#define L2CAP_ECHO_REQ       0x08
#define L2CAP_ECHO_RSP       0x09
#define L2CAP_INFO_REQ       0x0A
#define L2CAP_INFO_RSP       0x0B

/* AVDTP Signal IDs */
#define AVDTP_DISCOVER             0x01
#define AVDTP_GET_CAPABILITIES     0x02
#define AVDTP_SET_CONFIGURATION    0x03
#define AVDTP_GET_CONFIGURATION    0x04
#define AVDTP_RECONFIGURE          0x05
#define AVDTP_OPEN                 0x06
#define AVDTP_START                0x07
#define AVDTP_CLOSE                0x08
#define AVDTP_SUSPEND              0x09
#define AVDTP_ABORT                0x0A
#define AVDTP_SECURITY_CONTROL     0x0B
#define AVDTP_GET_ALL_CAPABILITIES 0x0C
#define AVDTP_DELAYREPORT          0x0D

/* Local Fixed L2CAP CIDs */
#define CID_SDP_LOCAL        0x0040
#define CID_AVDTP_SIG_LOCAL  0x0041
#define CID_AVDTP_MED_LOCAL  0x0042
#define CID_AVCTP_LOCAL      0x0043
#define CID_BIP_LOCAL        0x0044

static pthread_mutex_t bt_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bt_enable_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bt_enable_cond = PTHREAD_COND_INITIALIZER;
static pthread_t bt_thread;
static bool bt_thread_started = false;

static bool bt_target_enabled = false;
static volatile bool bt_receiver_mode = false;
static bool receiver_previous_bt_enabled;
static volatile enum y1_bt_state bt_state = Y1_BT_STATE_OFF;
static int stpbt_fd = -1;

static struct y1_bt_device bt_devices[Y1_BT_MAX_DEVICES];
static int bt_num_devices = 0;
static bool bt_paired_devices_loaded = false;
static bool bt_autoconnect_attempted = false;

/* Pending command requests from UI thread */
static volatile bool req_scan = false;
static volatile bool req_connect = false;
static uint8_t req_connect_bdaddr[6];
static volatile bool req_disconnect = false;
static bool pending_switch_connect;
static uint8_t pending_switch_bdaddr[6];

static bool bt_is_target_enabled(void)
{
    pthread_mutex_lock(&bt_enable_mtx);
    bool enabled = bt_target_enabled;
    pthread_mutex_unlock(&bt_enable_mtx);
    return enabled;
}

static void bt_set_target_enabled(bool enabled)
{
    pthread_mutex_lock(&bt_enable_mtx);
    bt_target_enabled = enabled;
    pthread_cond_broadcast(&bt_enable_cond);
    pthread_mutex_unlock(&bt_enable_mtx);
}

static void bt_wait_until_enabled(void)
{
    pthread_mutex_lock(&bt_enable_mtx);
    while (!bt_target_enabled)
        pthread_cond_wait(&bt_enable_cond, &bt_enable_mtx);
    pthread_mutex_unlock(&bt_enable_mtx);
}

/* Active ACL + L2CAP + AVDTP state */
static uint16_t acl_handle = 0xFFFF;
static uint8_t  active_bdaddr[6];
static char     connected_name[64] = "";
static uint8_t  l2cap_ident = 1;
static uint8_t  avdtp_tx_label = 1;

static uint16_t cid_sdp_remote = 0;
static uint16_t cid_avdtp_sig_remote = 0;
static uint16_t cid_avdtp_med_remote = 0;
static uint16_t cid_avctp_remote = 0;
static uint16_t cid_bip_remote = 0;

static bool avdtp_sig_local_conf_done = false;
static bool avdtp_sig_remote_conf_done = false;
static bool avdtp_med_local_conf_done = false;
static bool avdtp_med_remote_conf_done = false;
static bool avdtp_initiator = false;
static bool avdtp_discover_sent = false;
static bool avdtp_opening_media = false;
static volatile bool a2dp_streaming = false;
static volatile bool in_sniff_mode = false;
static uint32_t last_pcm_time_ms = 0;
static bool avrcp_play_notify_pending = false;
static uint8_t avrcp_play_notify_label = 0;
static uint8_t avrcp_play_notify_subunit = 0x48;
static bool avrcp_track_notify_pending = false;
static uint8_t avrcp_track_notify_label = 0;
static uint8_t avrcp_track_notify_subunit = 0x48;
static uint64_t avrcp_track_uid;
static uint8_t avrcp_tx_label = 1;
static char avrcp_title[128];
static char avrcp_artist[128];
static char avrcp_album[128];
static char receiver_title[128];
static char receiver_artist[128];
static char receiver_album[128];
static bool receiver_metadata_request_pending;
static uint8_t receiver_metadata_request_label;
static bool receiver_play_status_request_pending;
static uint8_t receiver_play_status_request_label;
static uint32_t last_receiver_metadata_query_ms;
static uint32_t last_receiver_play_status_query_ms;
static uint32_t receiver_art_generation;
static char receiver_art_handle[96];
static uint32_t receiver_art_connection_id;
static bool receiver_art_has_connection_id;
static bool receiver_art_connect_pending;
static uint8_t receiver_art_data[65536];
static size_t receiver_art_data_size;
static uint8_t receiver_play_status;
static bool bip_local_conf_done;
static bool bip_remote_conf_done;
static bool bip_obex_connected;
static uint32_t avrcp_duration_ms;
static uint32_t avrcp_position_ms;
static uint8_t avrcp_play_status;

static uint8_t remote_seid = 1;
static uint8_t remote_sink_seids[32];
static uint8_t remote_sink_count;
static uint8_t remote_sink_index;
static uint8_t sbc_remote_octet0;
static uint8_t sbc_remote_octet1;
static uint8_t sbc_cfg_octet0 = 0x21; /* 44.1kHz, Joint Stereo */
static uint8_t sbc_cfg_octet1 = 0x15; /* 16 blocks, 8 subbands, Loudness */
static uint8_t sbc_bitpool    = 53;
static uint8_t sbc_remote_min_bitpool = 2;
static uint8_t sbc_remote_max_bitpool = 53;
static bool    sbc_is_xq      = false;

/* Remote Name Resolution queue after Inquiry */
static bool resolving_names = false;
static int  resolve_idx = 0;

/* Logging helper */
static void bt_log(const char *msg)
{
    y1_log(msg);
}

static void format_bdaddr(const uint8_t bd[6], char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             bd[5], bd[4], bd[3], bd[2], bd[1], bd[0]);
}

/* ============================================================================
 * Persistent Paired Device Database (/mnt/sdcard/.rockbox/bt_paired.dat)
 * ============================================================================ */
struct paired_db_entry {
    uint8_t bdaddr[6];
    char    name[64];
    uint8_t link_key[16];
    uint8_t key_type;
    char    pin[16];
    int32_t volume;
};

static void load_paired_devices(void)
{
    int fd = open(PAIRED_DB_PATH, O_RDONLY);
    if (fd < 0)
        return;

    uint32_t magic = 0, count = 0;
    if (read(fd, &magic, 4) == 4 && read(fd, &count, 4) == 4)
    {
        if (magic == PAIRED_MAGIC_V2)
        {
            for (uint32_t i = 0; i < count && bt_num_devices < Y1_BT_MAX_DEVICES; i++)
            {
                struct paired_db_entry e;
                if (read(fd, &e, sizeof(e)) != sizeof(e))
                    break;
                struct y1_bt_device *d = &bt_devices[bt_num_devices++];
                memset(d, 0, sizeof(*d));
                memcpy(d->bdaddr, e.bdaddr, 6);
                strncpy(d->name, e.name, sizeof(d->name) - 1);
                memcpy(d->link_key, e.link_key, 16);
                d->key_type = e.key_type;
                strncpy(d->pin, e.pin, sizeof(d->pin) - 1);
                d->volume = e.volume;
                d->paired = true;
                d->rssi = -60;
            }
        }
        else if (magic == PAIRED_MAGIC_V1)
        {
            struct legacy_paired_entry {
                uint8_t bdaddr[6];
                char    name[64];
                uint8_t link_key[16];
                uint8_t key_type;
            };
            for (uint32_t i = 0; i < count && bt_num_devices < Y1_BT_MAX_DEVICES; i++)
            {
                struct legacy_paired_entry e;
                if (read(fd, &e, sizeof(e)) != sizeof(e))
                    break;
                struct y1_bt_device *d = &bt_devices[bt_num_devices++];
                memset(d, 0, sizeof(*d));
                memcpy(d->bdaddr, e.bdaddr, 6);
                strncpy(d->name, e.name, sizeof(d->name) - 1);
                memcpy(d->link_key, e.link_key, 16);
                d->key_type = e.key_type;
                d->volume = -15;
                d->paired = true;
                d->rssi = -60;
            }
        }
    }
    close(fd);
}

static void save_paired_devices(void)
{
    int fd = open(PAIRED_DB_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;

    uint32_t magic = PAIRED_MAGIC_V2;
    uint32_t count = 0;
    for (int i = 0; i < bt_num_devices; i++) {
        if (bt_devices[i].paired)
            count++;
    }
    (void)!write(fd, &magic, 4);
    (void)!write(fd, &count, 4);
    for (int i = 0; i < bt_num_devices; i++) {
        if (bt_devices[i].paired) {
            struct paired_db_entry e;
            memset(&e, 0, sizeof(e));
            memcpy(e.bdaddr, bt_devices[i].bdaddr, 6);
            strncpy(e.name, bt_devices[i].name, sizeof(e.name) - 1);
            memcpy(e.link_key, bt_devices[i].link_key, 16);
            e.key_type = bt_devices[i].key_type;
            strncpy(e.pin, bt_devices[i].pin, sizeof(e.pin) - 1);
            e.volume = bt_devices[i].volume;
            (void)!write(fd, &e, sizeof(e));
        }
    }
    close(fd);
}

static struct y1_bt_device *find_or_create_device(const uint8_t bd[6])
{
    for (int i = 0; i < bt_num_devices; i++) {
        if (memcmp(bt_devices[i].bdaddr, bd, 6) == 0)
            return &bt_devices[i];
    }
    if (bt_num_devices >= Y1_BT_MAX_DEVICES) {
        /* Evict oldest unpaired & unconnected entry */
        for (int i = 0; i < bt_num_devices; i++) {
            if (!bt_devices[i].paired && !bt_devices[i].connected) {
                memset(&bt_devices[i], 0, sizeof(bt_devices[i]));
                memcpy(bt_devices[i].bdaddr, bd, 6);
                format_bdaddr(bd, bt_devices[i].name, sizeof(bt_devices[i].name));
                bt_devices[i].volume = -15;
                return &bt_devices[i];
            }
        }
        return NULL;
    }
    struct y1_bt_device *d = &bt_devices[bt_num_devices++];
    memset(d, 0, sizeof(*d));
    memcpy(d->bdaddr, bd, 6);
    format_bdaddr(bd, d->name, sizeof(d->name));
    d->volume = -15;
    return d;
}

static void move_paired_device_to_end(struct y1_bt_device *device)
{
    int index = (int)(device - bt_devices);
    if (index < 0 || index >= bt_num_devices || !device->paired ||
        index == bt_num_devices - 1)
        return;

    struct y1_bt_device last_used = *device;
    for (int i = index; i + 1 < bt_num_devices; i++)
        bt_devices[i] = bt_devices[i + 1];
    bt_devices[bt_num_devices - 1] = last_used;
    save_paired_devices();
}

static bool is_placeholder_name(const char *name)
{
    /* True if empty or "XX:XX:XX:XX:XX:XX" */
    if (!name || !name[0])
        return true;
    if (strlen(name) == 17 && name[2] == ':' && name[5] == ':' && name[8] == ':')
        return true;
    return false;
}

/* ============================================================================
 * Standard A2DP 8-Subband SBC Encoder + PCM Ringbuffer
 * ============================================================================ */
#define PCM_RING_SAMPLES 16384 /* Stereo int16 sample pairs (64 KB) */
static int16_t pcm_ring[PCM_RING_SAMPLES * 2];
static volatile size_t pcm_ring_r = 0;
static volatile size_t pcm_ring_w = 0;
static uint16_t rtp_seq_num = 1;
static uint32_t rtp_timestamp = 0;
static uint64_t pcm_resample_phase_fp;
static unsigned long pcm_resample_source_rate;
static unsigned long pcm_resample_target_rate;

static void reset_bt_session(void)
{
    acl_handle = 0xFFFF;
    memset(active_bdaddr, 0, sizeof(active_bdaddr));
    connected_name[0] = '\0';
    cid_sdp_remote = 0;
    cid_avdtp_sig_remote = 0;
    cid_avdtp_med_remote = 0;
    cid_avctp_remote = 0;
    cid_bip_remote = 0;
    avdtp_sig_local_conf_done = false;
    avdtp_sig_remote_conf_done = false;
    avdtp_med_local_conf_done = false;
    avdtp_med_remote_conf_done = false;
    avdtp_initiator = false;
    avdtp_discover_sent = false;
    avdtp_opening_media = false;
    remote_seid = 1;
    memset(remote_sink_seids, 0, sizeof(remote_sink_seids));
    remote_sink_count = 0;
    remote_sink_index = 0;
    sbc_remote_octet0 = 0;
    sbc_remote_octet1 = 0;
    sbc_cfg_octet0 = 0x21;
    sbc_cfg_octet1 = 0x15;
    sbc_bitpool = 53;
    sbc_remote_min_bitpool = 2;
    sbc_remote_max_bitpool = 53;
    sbc_is_xq = false;
    a2dp_streaming = false;
    in_sniff_mode = false;
    avrcp_play_notify_pending = false;
    avrcp_track_notify_pending = false;
    receiver_metadata_request_pending = false;
    receiver_play_status_request_pending = false;
    receiver_art_connect_pending = false;
    receiver_art_handle[0] = '\0';
    receiver_art_data_size = 0;
    receiver_art_has_connection_id = false;
    receiver_art_connection_id = 0;
    bip_local_conf_done = false;
    bip_remote_conf_done = false;
    bip_obex_connected = false;
    unlink(Y1_BT_RECEIVER_ART_PATH);
    pthread_mutex_lock(&bt_mtx);
    receiver_play_status = 0;
    receiver_art_generation++;
    receiver_title[0] = '\0';
    receiver_artist[0] = '\0';
    receiver_album[0] = '\0';
    pthread_mutex_unlock(&bt_mtx);
    last_receiver_metadata_query_ms = 0;
    last_receiver_play_status_query_ms = 0;
    avrcp_tx_label = 1;
    pcm_ring_r = 0;
    pcm_ring_w = 0;
    rtp_seq_num = 1;
    rtp_timestamp = 0;
    pcm_resample_phase_fp = 0;
    pcm_resample_source_rate = 0;
    pcm_resample_target_rate = 0;
}

static sbc_t a2dp_sbc;
static bool  a2dp_sbc_ready = false;

static void reinit_a2dp_sbc(void)
{
    uint8_t conf[4] = { sbc_cfg_octet0, sbc_cfg_octet1, 2, sbc_bitpool };
    if (a2dp_sbc_ready) {
        sbc_reinit_a2dp(&a2dp_sbc, 0, conf, sizeof(conf));
    } else {
        if (sbc_init_a2dp(&a2dp_sbc, 0, conf, sizeof(conf)) == 0)
            a2dp_sbc_ready = true;
    }
    if (a2dp_sbc_ready) {
        a2dp_sbc.bitpool = sbc_bitpool;
    }
}

static unsigned long a2dp_sample_rate(void)
{
    switch (sbc_cfg_octet0 & 0xF0) {
        case 0x10: return 48000;
        case 0x20: return 44100;
        case 0x40: return 32000;
        case 0x80: return 16000;
        default:   return 44100;
    }
}

/* ============================================================================
 * HCI + L2CAP Transmission Helpers
 * ============================================================================ */
static int hci_send_cmd(uint16_t opcode, const void *param, uint8_t plen)
{
    if (stpbt_fd < 0)
        return -1;
    uint8_t pkt[260];
    pkt[0] = HCI_PKT_CMD;
    pkt[1] = (uint8_t)(opcode & 0xFF);
    pkt[2] = (uint8_t)((opcode >> 8) & 0xFF);
    pkt[3] = plen;
    if (plen > 0 && param)
        memcpy(&pkt[4], param, plen);

    ssize_t total = 4 + plen;
    ssize_t w = write(stpbt_fd, pkt, total);
    return (w == total) ? 0 : -1;
}

static int l2cap_send_pdu(uint16_t dcid, const void *payload, uint16_t len)
{
    if (stpbt_fd < 0 || acl_handle == 0xFFFF || dcid == 0)
        return -1;
    if (len > 1012)
        return -1;

    uint8_t pkt[1024];
    uint16_t acl_len = len + 4;
    uint16_t hdr_handle = (acl_handle & 0x0FFF) | (0x02 << 12); /* PB = 10b (First automatically flushable) */

    pkt[0] = HCI_PKT_ACL;
    pkt[1] = (uint8_t)(hdr_handle & 0xFF);
    pkt[2] = (uint8_t)((hdr_handle >> 8) & 0xFF);
    pkt[3] = (uint8_t)(acl_len & 0xFF);
    pkt[4] = (uint8_t)((acl_len >> 8) & 0xFF);
    pkt[5] = (uint8_t)(len & 0xFF);
    pkt[6] = (uint8_t)((len >> 8) & 0xFF);
    pkt[7] = (uint8_t)(dcid & 0xFF);
    pkt[8] = (uint8_t)((dcid >> 8) & 0xFF);
    if (len > 0 && payload)
        memcpy(&pkt[9], payload, len);

    ssize_t w = write(stpbt_fd, pkt, 9 + len);
    if (w == (ssize_t)(9 + len)) {
        return 0;
    }
    return -1;
}

static void l2cap_send_sig(uint8_t code, uint8_t ident, const void *data, uint16_t dlen)
{
    uint8_t buf[128];
    if (dlen > sizeof(buf) - 4)
        return;
    buf[0] = code;
    buf[1] = ident;
    buf[2] = (uint8_t)(dlen & 0xFF);
    buf[3] = (uint8_t)((dlen >> 8) & 0xFF);
    if (dlen > 0 && data)
        memcpy(&buf[4], data, dlen);
    l2cap_send_pdu(0x0001, buf, 4 + dlen);
}

static void l2cap_send_conn_req(uint16_t psm, uint16_t scid)
{
    uint8_t req[4];
    req[0] = (uint8_t)(psm & 0xFF);
    req[1] = (uint8_t)((psm >> 8) & 0xFF);
    req[2] = (uint8_t)(scid & 0xFF);
    req[3] = (uint8_t)((scid >> 8) & 0xFF);
    l2cap_send_sig(L2CAP_CONN_REQ, l2cap_ident++, req, 4);
}

static void l2cap_send_conf_req(uint16_t dcid, uint16_t mtu)
{
    uint8_t req[8];
    req[0] = (uint8_t)(dcid & 0xFF);
    req[1] = (uint8_t)((dcid >> 8) & 0xFF);
    req[2] = 0x00;
    req[3] = 0x00;
    req[4] = 0x01; /* Option: MTU */
    req[5] = 0x02; /* Length: 2 */
    req[6] = (uint8_t)(mtu & 0xFF);
    req[7] = (uint8_t)((mtu >> 8) & 0xFF);
    l2cap_send_sig(L2CAP_CONF_REQ, l2cap_ident++, req, 8);
}

/* ============================================================================
 * SDP Server (PSM 0x0001) - Advertises A2DP Source (0x110A) & AVRCP (0x110E)
 * ============================================================================ */
/* Pre-encoded SDP attribute list for A2DP Audio Source (Record 0x00010001) */
static const uint8_t sdp_attr_a2dp_source[] = {
    0x35, 0x38, /* Sequence (56 bytes) */
    /* Attr 0x0000: ServiceRecordHandle = 0x00010001 */
    0x09, 0x00, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x01,
    /* Attr 0x0001: ServiceClassIDList = { 0x110A (AudioSource) } */
    0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0A,
    /* Attr 0x0004: ProtocolDescriptorList = { { L2CAP (0x0100), PSM=0x0019 }, { AVDTP (0x0019), v1.2 (0x0102) } } */
    0x09, 0x00, 0x04, 0x35, 0x10,
        0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,
        0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x02,
    /* Attr 0x0009: BluetoothProfileDescriptorList = { { AdvancedAudioDistribution (0x110D), v1.2 (0x0102) } } */
    0x09, 0x00, 0x09, 0x35, 0x08,
        0x35, 0x06, 0x19, 0x11, 0x0D, 0x09, 0x01, 0x02,
    /* Attr 0x0311: SupportedFeatures = 0x0001 (Player) */
    0x09, 0x03, 0x11, 0x09, 0x00, 0x01
};

static const uint8_t sdp_attr_a2dp_sink[] = {
    0x35, 0x38,
    0x09, 0x00, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x01,
    0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0B,
    0x09, 0x00, 0x04, 0x35, 0x10,
        0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,
        0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x02,
    0x09, 0x00, 0x09, 0x35, 0x08,
        0x35, 0x06, 0x19, 0x11, 0x0D, 0x09, 0x01, 0x02,
    0x09, 0x03, 0x11, 0x09, 0x00, 0x01
};

static const uint8_t sdp_attr_avrcp_target[] = {
    0x35, 0x38,
    0x09, 0x00, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x02,
    0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0C,
    0x09, 0x00, 0x04, 0x35, 0x10,
        0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x17,
        0x35, 0x06, 0x19, 0x00, 0x17, 0x09, 0x01, 0x04,
    0x09, 0x00, 0x09, 0x35, 0x08,
        0x35, 0x06, 0x19, 0x11, 0x0E, 0x09, 0x01, 0x06,
    0x09, 0x03, 0x11, 0x09, 0x00, 0x01
};

static const uint8_t sdp_attr_avrcp_controller[] = {
    0x35, 0x38,
    0x09, 0x00, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x02,
    0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0F,
    0x09, 0x00, 0x04, 0x35, 0x10,
        0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x17,
        0x35, 0x06, 0x19, 0x00, 0x17, 0x09, 0x01, 0x04,
    0x09, 0x00, 0x09, 0x35, 0x08,
        0x35, 0x06, 0x19, 0x11, 0x0E, 0x09, 0x01, 0x06,
    0x09, 0x03, 0x11, 0x09, 0x00, 0x02
};

static bool sdp_request_is_avrcp(const uint8_t *pdu, uint16_t len)
{
    for (uint16_t i = 5; i + 2 < len; i++) {
        if (pdu[i] == 0x19 && pdu[i + 1] == 0x11 &&
            (pdu[i + 2] == 0x0C || pdu[i + 2] == 0x0E))
            return true;
    }
    return false;
}

static void handle_sdp_pdu(const uint8_t *pdu, uint16_t len)
{
    if (len < 5 || cid_sdp_remote == 0)
        return;

    uint8_t pdu_id = pdu[0];
    uint16_t tid = ((uint16_t)pdu[1] << 8) | pdu[2];
    uint8_t rsp[128];
    bool avrcp = sdp_request_is_avrcp(pdu, len);
    const uint8_t *record = avrcp ?
                            (bt_receiver_mode ? sdp_attr_avrcp_controller :
                                                sdp_attr_avrcp_target) :
                            (bt_receiver_mode ? sdp_attr_a2dp_sink :
                                                sdp_attr_a2dp_source);
    uint16_t record_len = avrcp ? sizeof(sdp_attr_avrcp_target) :
                                  sizeof(sdp_attr_a2dp_source);

    if (pdu_id == 0x02) /* SDP_ServiceSearchRequest */
    {
        uint32_t handle = avrcp ? 0x00010002 : 0x00010001;
        rsp[0] = 0x03; /* SDP_ServiceSearchResponse */
        rsp[1] = (uint8_t)(tid >> 8);
        rsp[2] = (uint8_t)(tid & 0xFF);
        rsp[3] = 0x00;
        rsp[4] = 0x09; /* Param length = 9 */
        rsp[5] = 0x00;
        rsp[6] = 0x01; /* TotalServiceRecordCount = 1 */
        rsp[7] = 0x00;
        rsp[8] = 0x01; /* CurrentServiceRecordCount = 1 */
        rsp[9] = (uint8_t)(handle >> 24);
        rsp[10] = (uint8_t)(handle >> 16);
        rsp[11] = (uint8_t)(handle >> 8);
        rsp[12] = (uint8_t)handle;
        rsp[13] = 0x00; /* ContinuationState = 0 */
        l2cap_send_pdu(cid_sdp_remote, rsp, 14);
    }
    else if (pdu_id == 0x04) /* SDP_ServiceAttributeRequest */
    {
        if (len >= 9) {
            uint32_t handle = ((uint32_t)pdu[5] << 24) |
                              ((uint32_t)pdu[6] << 16) |
                              ((uint32_t)pdu[7] << 8) | pdu[8];
            avrcp = handle == 0x00010002;
            record = avrcp ?
                     (bt_receiver_mode ? sdp_attr_avrcp_controller :
                                         sdp_attr_avrcp_target) :
                     (bt_receiver_mode ? sdp_attr_a2dp_sink :
                                         sdp_attr_a2dp_source);
            record_len = avrcp ? sizeof(sdp_attr_avrcp_target) :
                                 sizeof(sdp_attr_a2dp_source);
        }
        uint16_t attr_len = record_len;
        uint16_t param_len = 2 + attr_len + 1;
        rsp[0] = 0x05; /* SDP_ServiceAttributeResponse */
        rsp[1] = (uint8_t)(tid >> 8);
        rsp[2] = (uint8_t)(tid & 0xFF);
        rsp[3] = (uint8_t)(param_len >> 8);
        rsp[4] = (uint8_t)(param_len & 0xFF);
        rsp[5] = (uint8_t)(attr_len >> 8);
        rsp[6] = (uint8_t)(attr_len & 0xFF);
        memcpy(&rsp[7], record, attr_len);
        rsp[7 + attr_len] = 0x00; /* ContinuationState = 0 */
        l2cap_send_pdu(cid_sdp_remote, rsp, 5 + param_len);
    }
    else if (pdu_id == 0x06) /* SDP_ServiceSearchAttributeRequest */
    {
        uint16_t seq_len = 2 + record_len;
        uint16_t param_len = 2 + seq_len + 1;
        rsp[0] = 0x07; /* SDP_ServiceSearchAttributeResponse */
        rsp[1] = (uint8_t)(tid >> 8);
        rsp[2] = (uint8_t)(tid & 0xFF);
        rsp[3] = (uint8_t)(param_len >> 8);
        rsp[4] = (uint8_t)(param_len & 0xFF);
        rsp[5] = (uint8_t)(seq_len >> 8);
        rsp[6] = (uint8_t)(seq_len & 0xFF);
        rsp[7] = 0x35; /* Data Element Sequence */
        rsp[8] = (uint8_t)record_len;
        memcpy(&rsp[9], record, record_len);
        rsp[9 + record_len] = 0x00; /* ContinuationState = 0 */
        l2cap_send_pdu(cid_sdp_remote, rsp, 5 + param_len);
    }
}

/* ============================================================================
 * AVDTP Signaling + Media Setup (PSM 0x0019)
 * ============================================================================ */
static void avdtp_send_cmd(uint8_t sig_id, const void *params, uint16_t plen)
{
    if (cid_avdtp_sig_remote == 0)
        return;
    uint8_t buf[64];
    buf[0] = (uint8_t)(((avdtp_tx_label++ & 0x0F) << 4) | 0x00); /* Single packet, Command (0x00) */
    buf[1] = sig_id & 0x3F;
    if (plen > 0 && params)
        memcpy(&buf[2], params, plen);
    l2cap_send_pdu(cid_avdtp_sig_remote, buf, 2 + plen);
}

static void avdtp_send_accept(uint8_t rx_hdr, uint8_t sig_id, const void *params, uint16_t plen)
{
    if (cid_avdtp_sig_remote == 0)
        return;
    uint8_t buf[64];
    buf[0] = (uint8_t)((rx_hdr & 0xF0) | 0x02); /* Single packet, Response Accept (0x02) */
    buf[1] = sig_id & 0x3F;
    if (plen > 0 && params)
        memcpy(&buf[2], params, plen);
    l2cap_send_pdu(cid_avdtp_sig_remote, buf, 2 + plen);
}

static bool avdtp_request_next_sink_capabilities(void)
{
    if (remote_sink_index >= remote_sink_count)
    {
        bt_log("No remote audio sink endpoint supports SBC\n");
        return false;
    }

    remote_seid = remote_sink_seids[remote_sink_index++];
    uint8_t param[1] = { (uint8_t)(remote_seid << 2) };
    avdtp_send_cmd(AVDTP_GET_CAPABILITIES, param, sizeof(param));
    return true;
}

static void handle_avdtp_sig_pdu(const uint8_t *pdu, uint16_t len)
{
    if (len < 2)
        return;

    uint8_t hdr = pdu[0];
    uint8_t msg_type = hdr & 0x03;
    uint8_t sig_id = pdu[1] & 0x3F;

    if (msg_type == 0x00) /* Command from remote headphone (Acceptor role) */
    {
        switch (sig_id)
        {
            case AVDTP_DISCOVER: {
                uint8_t seps[2] = { (1 << 2),
                                    bt_receiver_mode ? 0x08 : 0x00 };
                avdtp_send_accept(hdr, sig_id, seps, 2);
                break;
            }
            case AVDTP_GET_CAPABILITIES:
            case AVDTP_GET_ALL_CAPABILITIES: {
                /* Media Transport (0x01, len 0) + Media Codec SBC (0x07, len 6: Audio, SBC, 44.1/48kHz all modes, 4..16 blocks, bitpool 2..53) */
                uint8_t caps[10] = {
                    0x01, 0x00,
                    0x07, 0x06, 0x00, 0x00, 0x3F, 0xFF, 0x02, 0x35
                };
                avdtp_send_accept(hdr, sig_id, caps, sizeof(caps));
                break;
            }
            case AVDTP_SET_CONFIGURATION: {
                /* Parse negotiated SBC configuration if present */
                for (int i = 4; i + 7 < len; i++) {
                    if (pdu[i] == 0x07 && pdu[i + 1] == 0x06 && pdu[i + 2] == 0x00 && pdu[i + 3] == 0x00) {
                        sbc_cfg_octet0 = pdu[i + 4];
                        sbc_cfg_octet1 = pdu[i + 5];
                        sbc_remote_min_bitpool = pdu[i + 6];
                        uint8_t max_bp = pdu[i + 7];
                        sbc_remote_max_bitpool = max_bp;
                        sbc_bitpool = max_bp;
                        sbc_is_xq = ((sbc_cfg_octet0 & 0x04) != 0 && sbc_bitpool >= 38);
                    }
                }
                reinit_a2dp_sbc();
                avdtp_send_accept(hdr, sig_id, NULL, 0);
                break;
            }
            case AVDTP_OPEN:
                avdtp_send_accept(hdr, sig_id, NULL, 0);
                break;
            case AVDTP_START: {
                bool was_streaming = a2dp_streaming;
                reinit_a2dp_sbc();
                avdtp_send_accept(hdr, sig_id, NULL, 0);
                a2dp_streaming = true;
                bt_state = Y1_BT_STATE_STREAMING;
                bt_log("A2DP STREAMING STARTED (remote initiated)\n");
                if (!was_streaming && !bt_receiver_mode) {
                    y1_bt_audio_connect_hook();
                }
                if (bt_receiver_mode)
                    y1_audio_receiver_set_active(true, a2dp_sample_rate());
                break;
            }
            case AVDTP_SUSPEND:
                avdtp_send_accept(hdr, sig_id, NULL, 0);
                a2dp_streaming = false;
                bt_state = Y1_BT_STATE_CONNECTED;
                if (bt_receiver_mode)
                    y1_audio_receiver_set_active(false, a2dp_sample_rate());
                break;
            case AVDTP_CLOSE:
            case AVDTP_ABORT:
                avdtp_send_accept(hdr, sig_id, NULL, 0);
                a2dp_streaming = false;
                cid_avdtp_med_remote = 0;
                if (bt_receiver_mode)
                    y1_audio_receiver_set_active(false, a2dp_sample_rate());
                break;
            case AVDTP_DELAYREPORT:
                avdtp_send_accept(hdr, sig_id, NULL, 0);
                break;
            default: {
                /* Reject unsupported command (msg_type = 0x03, error = 0x19 NOT_SUPPORTED_COMMAND) */
                uint8_t rej[3] = { (uint8_t)((hdr & 0xF0) | 0x03), sig_id, 0x19 };
                l2cap_send_pdu(cid_avdtp_sig_remote, rej, 3);
                break;
            }
        }
    }
    else if (msg_type == 0x02) /* Response Accept to our Initiator command */
    {
        switch (sig_id)
        {
            case AVDTP_DISCOVER: {
                remote_sink_count = 0;
                remote_sink_index = 0;
                for (int i = 2; i + 1 < len; i += 2) {
                    uint8_t seid = pdu[i] >> 2;
                    bool in_use = (pdu[i] & 0x02) != 0;
                    bool is_sink = (pdu[i + 1] & 0x08) != 0;
                    uint8_t media_type = pdu[i + 1] >> 4;
                    if (!in_use && is_sink && media_type == 0 && seid > 0 &&
                        remote_sink_count < sizeof(remote_sink_seids))
                        remote_sink_seids[remote_sink_count++] = seid;
                }
                avdtp_request_next_sink_capabilities();
                break;
            }
            case AVDTP_GET_CAPABILITIES:
            case AVDTP_GET_ALL_CAPABILITIES: {
                bool found_sbc = false;
                uint8_t sup_o0 = 0, sup_o1 = 0, min_bp = 0, max_bp = 0;
                for (int i = 2; i + 7 < len; i++) {
                    if (pdu[i] == 0x07 && pdu[i + 1] == 0x06 &&
                        pdu[i + 2] == 0x00 && pdu[i + 3] == 0x00) {
                        sup_o0 = pdu[i + 4];
                        sup_o1 = pdu[i + 5];
                        min_bp = pdu[i + 6];
                        max_bp = pdu[i + 7];
                        found_sbc = true;
                        break;
                    }
                }
                if (!found_sbc || min_bp < 2 || max_bp < min_bp || max_bp > 250)
                {
                    if (!avdtp_request_next_sink_capabilities())
                        bt_log("Remote endpoints do not expose valid SBC capabilities\n");
                    break;
                }

                sbc_remote_min_bitpool = min_bp;
                sbc_remote_max_bitpool = max_bp;
                sbc_remote_octet0 = sup_o0;
                sbc_remote_octet1 = sup_o1;

                uint8_t freq = (sup_o0 & 0x10) ? 0x10 :
                               (sup_o0 & 0x20) ? 0x20 :
                               (sup_o0 & 0x40) ? 0x40 : 0x80;
                if (!(sup_o0 & 0xF0))
                {
                    if (!avdtp_request_next_sink_capabilities())
                        bt_log("Remote SBC endpoint has no common sample rate\n");
                    break;
                }

                uint8_t channel_mode = (sup_o0 & 0x01) ? 0x01 :
                                       (sup_o0 & 0x02) ? 0x02 :
                                       (sup_o0 & 0x04) ? 0x04 :
                                       (sup_o0 & 0x08) ? 0x08 : 0;
                if (channel_mode == 0)
                {
                    if (!avdtp_request_next_sink_capabilities())
                        bt_log("Remote SBC endpoint has no common channel mode\n");
                    break;
                }

                uint8_t blocks = (sup_o1 & 0x10) ? 0x10 :
                                 (sup_o1 & 0x20) ? 0x20 :
                                 (sup_o1 & 0x40) ? 0x40 :
                                 (sup_o1 & 0x80) ? 0x80 : 0;
                uint8_t subbands = (sup_o1 & 0x04) ? 0x04 :
                                   (sup_o1 & 0x08) ? 0x08 : 0;
                uint8_t allocation = (sup_o1 & 0x01) ? 0x01 :
                                     (sup_o1 & 0x02) ? 0x02 : 0;
                if (!blocks || !subbands || !allocation)
                {
                    if (!avdtp_request_next_sink_capabilities())
                        bt_log("Remote SBC endpoint has no compatible SBC parameters\n");
                    break;
                }

                sbc_is_xq = false;
                sbc_cfg_octet0 = freq | channel_mode;
                sbc_bitpool = max_bp > 53 ? 53 : max_bp;
                if (sbc_bitpool < min_bp)
                {
                    if (!avdtp_request_next_sink_capabilities())
                        bt_log("Remote SBC bitpool range is unsupported\n");
                    break;
                }

                /* Use standard SBC parameters first; these are widely supported by speakers. */
                sbc_cfg_octet1 = blocks | subbands | allocation;
                if ((sup_o0 & 0x04) && min_bp <= 38 && max_bp >= 38 &&
                    remote_sink_count == 1)
                {
                    /* Preserve the higher-quality mode for a single compatible endpoint. */
                    sbc_is_xq = true;
                    sbc_cfg_octet0 = freq | 0x04; /* Dual Channel */
                    sbc_bitpool = 38;
                    bt_log("Negotiating SBC XQ (Dual Channel, high-bitpool)\n");
                } else {
                    bt_log("Negotiating standard SBC\n");
                }

                reinit_a2dp_sbc();

                uint8_t set_cfg[12] = {
                    (uint8_t)(remote_seid << 2), /* ACP SEID */
                    (1 << 2),                    /* INT SEID = 1 */
                    0x01, 0x00,                  /* Media Transport */
                    0x07, 0x06, 0x00, 0x00,      /* Media Codec: Audio, SBC */
                    sbc_cfg_octet0,
                    sbc_cfg_octet1,
                    min_bp,
                    sbc_bitpool
                };
                avdtp_send_cmd(AVDTP_SET_CONFIGURATION, set_cfg, sizeof(set_cfg));
                break;
            }
            case AVDTP_SET_CONFIGURATION: {
                uint8_t open_param[1] = { (uint8_t)(remote_seid << 2) };
                avdtp_send_cmd(AVDTP_OPEN, open_param, 1);
                break;
            }
            case AVDTP_OPEN: {
                /* Open second L2CAP channel on PSM 0x0019 for AVDTP Media Transport */
                avdtp_opening_media = true;
                avdtp_med_local_conf_done = false;
                avdtp_med_remote_conf_done = false;
                l2cap_send_conn_req(PSM_AVDTP, CID_AVDTP_MED_LOCAL);
                break;
            }
            case AVDTP_START: {
                bool was_streaming = a2dp_streaming;
                reinit_a2dp_sbc();
                a2dp_streaming = true;
                bt_state = Y1_BT_STATE_STREAMING;
                bt_log("A2DP STREAMING STARTED (local initiated)\n");
                if (!was_streaming) {
                    y1_bt_audio_connect_hook();
                }
                break;
            }
            default:
                break;
        }
    }
    else if (msg_type == 0x03) /* Response Reject to our Initiator command */
    {
        if (sig_id == AVDTP_SET_CONFIGURATION) {
            bt_log("AVDTP_SET_CONFIGURATION rejected by remote\n");
            if (sbc_is_xq) {
                /* Remote rejected SBC XQ: fallback to standard SBC Joint Stereo */
                bt_log("Falling back from SBC XQ to standard SBC (Joint Stereo)\n");
                sbc_is_xq = false;
                uint8_t freq = sbc_cfg_octet0 & 0xF0;
                uint8_t channel_mode = (sbc_remote_octet0 & 0x01) ? 0x01 :
                                       (sbc_remote_octet0 & 0x02) ? 0x02 :
                                       (sbc_remote_octet0 & 0x04) ? 0x04 :
                                       (sbc_remote_octet0 & 0x08) ? 0x08 : 0;
                sbc_cfg_octet0 = freq | channel_mode;
                sbc_bitpool = sbc_remote_max_bitpool > 53 ?
                              53 : sbc_remote_max_bitpool;
                if (sbc_bitpool < sbc_remote_min_bitpool)
                    sbc_bitpool = sbc_remote_min_bitpool;
                sbc_cfg_octet1 = (sbc_remote_octet1 & 0x10) ? 0x10 :
                                 (sbc_remote_octet1 & 0x20) ? 0x20 :
                                 (sbc_remote_octet1 & 0x40) ? 0x40 : 0x80;
                sbc_cfg_octet1 |= (sbc_remote_octet1 & 0x04) ? 0x04 : 0x08;
                sbc_cfg_octet1 |= (sbc_remote_octet1 & 0x01) ? 0x01 : 0x02;
                reinit_a2dp_sbc();

                uint8_t set_cfg[12] = {
                    (uint8_t)(remote_seid << 2), /* ACP SEID */
                    (1 << 2),                    /* INT SEID = 1 */
                    0x01, 0x00,                  /* Media Transport */
                    0x07, 0x06, 0x00, 0x00,      /* Media Codec: Audio, SBC */
                    sbc_cfg_octet0,
                    sbc_cfg_octet1,
                    2,                           /* min_bp */
                    sbc_bitpool                  /* max_bp */
                };
                avdtp_send_cmd(AVDTP_SET_CONFIGURATION, set_cfg, sizeof(set_cfg));
            }
            else if (!sbc_is_xq && !avdtp_request_next_sink_capabilities())
            {
                bt_log("All available SBC endpoint configurations were rejected\n");
            }
        }
    }
}

static void send_avrcp_vendor_response(const uint8_t *request, uint8_t ctype,
                                      uint8_t pdu_id, const uint8_t *params,
                                      uint16_t params_len)
{
    uint8_t rsp[512];
    if (cid_avctp_remote == 0 || params_len > sizeof(rsp) - 13)
        return;

    rsp[0] = (uint8_t)((request[0] & 0xF0) | 0x02);
    rsp[1] = 0x11;
    rsp[2] = 0x0E;
    rsp[3] = ctype;
    rsp[4] = request[4];
    rsp[5] = 0x00;
    rsp[6] = 0x00;
    rsp[7] = 0x19;
    rsp[8] = 0x58;
    rsp[9] = pdu_id;
    rsp[10] = 0x00;
    rsp[11] = (uint8_t)(params_len >> 8);
    rsp[12] = (uint8_t)params_len;
    if (params_len > 0 && params)
        memcpy(&rsp[13], params, params_len);
    l2cap_send_pdu(cid_avctp_remote, rsp, (uint16_t)(13 + params_len));
}

static void request_receiver_metadata(void)
{
    if (!bt_receiver_mode || cid_avctp_remote == 0 ||
        receiver_metadata_request_pending)
        return;

    uint8_t request[38] = { 0 };
    receiver_metadata_request_label = avrcp_tx_label++ & 0x0F;
    request[0] = (uint8_t)(receiver_metadata_request_label << 4);
    request[1] = 0x11;
    request[2] = 0x0E;
    request[3] = 0x00;
    request[4] = 0x48;
    request[5] = 0x00;
    request[6] = 0x00;
    request[7] = 0x19;
    request[8] = 0x58;
    request[9] = 0x20;
    request[10] = 0x00;
    request[11] = 0x00;
    request[12] = 0x19;
    memset(&request[13], 0xFF, 8);
    request[21] = 4;
    request[22] = 0x00; request[23] = 0x00;
    request[24] = 0x00; request[25] = 0x01;
    request[26] = 0x00; request[27] = 0x00;
    request[28] = 0x00; request[29] = 0x02;
    request[30] = 0x00; request[31] = 0x00;
    request[32] = 0x00; request[33] = 0x03;
    request[34] = 0x00; request[35] = 0x00;
    request[36] = 0x00; request[37] = 0x08;

    if (l2cap_send_pdu(cid_avctp_remote, request, sizeof(request)) >= 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        receiver_metadata_request_pending = true;
        last_receiver_metadata_query_ms = (uint32_t)(now.tv_sec * 1000 +
                                                     now.tv_nsec / 1000000);
    }
}

static void request_receiver_play_status(void)
{
    if (!bt_receiver_mode || cid_avctp_remote == 0 ||
        receiver_play_status_request_pending)
        return;

    uint8_t request[13] = {
        0, 0x11, 0x0E, 0x00, 0x48, 0x00, 0x00, 0x19, 0x58,
        0x30, 0x00, 0x00, 0x00
    };
    receiver_play_status_request_label = avrcp_tx_label++ & 0x0F;
    request[0] = (uint8_t)(receiver_play_status_request_label << 4);
    if (l2cap_send_pdu(cid_avctp_remote, request, sizeof(request)) >= 0) {
        receiver_play_status_request_pending = true;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        last_receiver_play_status_query_ms =
            (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
    }
}

static void receiver_send_obex_connect(void)
{
    static const uint8_t cover_art_target[16] = {
        0x71, 0x63, 0xDD, 0x54, 0x4A, 0x7E, 0x11, 0xE2,
        0xB4, 0x7C, 0x00, 0x25, 0x00, 0x9A, 0xA3, 0x26
    };
    uint8_t request[26] = {
        0x80, 0x00, 0x1A, 0x10, 0x00, 0x04, 0x00,
        0x46, 0x00, 0x13
    };
    memcpy(&request[10], cover_art_target, sizeof(cover_art_target));
    if (l2cap_send_pdu(cid_bip_remote, request, sizeof(request)) < 0)
        bt_log("Failed to send AVRCP cover-art OBEX connect\n");
    else
        receiver_art_connect_pending = true;
}

static void receiver_bip_try_connect(void)
{
    if (cid_bip_remote != 0 && bip_local_conf_done &&
        bip_remote_conf_done && !bip_obex_connected &&
        !receiver_art_connect_pending)
        receiver_send_obex_connect();
}

static void receiver_send_obex_get_thumbnail(void)
{
    static const char type[] = "x-bt/img-thm";
    uint8_t request[256];
    size_t offset = 3;

    request[0] = 0x83;
    if (receiver_art_has_connection_id) {
        request[offset++] = 0xCB;
        request[offset++] = (uint8_t)(receiver_art_connection_id >> 24);
        request[offset++] = (uint8_t)(receiver_art_connection_id >> 16);
        request[offset++] = (uint8_t)(receiver_art_connection_id >> 8);
        request[offset++] = (uint8_t)receiver_art_connection_id;
    }

    size_t type_len = sizeof(type);
    request[offset++] = 0x42;
    request[offset++] = (uint8_t)((type_len + 3) >> 8);
    request[offset++] = (uint8_t)(type_len + 3);
    memcpy(&request[offset], type, type_len);
    offset += type_len;

    size_t name_len = strlen(receiver_art_handle);
    size_t name_header_len = 3 + (name_len + 1) * 2;
    request[offset++] = 0x01;
    request[offset++] = (uint8_t)(name_header_len >> 8);
    request[offset++] = (uint8_t)name_header_len;
    for (size_t i = 0; i < name_len; i++) {
        request[offset++] = 0;
        request[offset++] = (uint8_t)receiver_art_handle[i];
    }
    request[offset++] = 0;
    request[offset++] = 0;

    request[1] = (uint8_t)(offset >> 8);
    request[2] = (uint8_t)offset;
    if (offset > sizeof(request) ||
        l2cap_send_pdu(cid_bip_remote, request, (uint16_t)offset) < 0)
        bt_log("Failed to request AVRCP cover-art thumbnail\n");
}

static void receiver_request_cover_art(const char *handle)
{
    if (!bt_receiver_mode || !handle || !handle[0] ||
        strlen(handle) >= sizeof(receiver_art_handle) ||
        strcmp(handle, receiver_art_handle) == 0)
        return;

    strcpy(receiver_art_handle, handle);
    receiver_art_data_size = 0;
    receiver_art_has_connection_id = false;
    receiver_art_connect_pending = false;

    if (cid_bip_remote != 0 && bip_obex_connected)
        receiver_send_obex_get_thumbnail();
    else if (cid_bip_remote == 0)
        l2cap_send_conn_req(PSM_BIP, CID_BIP_LOCAL);
}

static void receiver_save_cover_art(void)
{
    static const char temp_path[] =
        "/mnt/sdcard/.rockbox/albumart/bt-receiver.jpg.tmp";
    if (receiver_art_data_size < 4 ||
        receiver_art_data[0] != 0xFF || receiver_art_data[1] != 0xD8) {
        bt_log("AVRCP cover-art thumbnail is not a JPEG image\n");
        receiver_art_data_size = 0;
        return;
    }

    if (mkdir("/mnt/sdcard/.rockbox/albumart", 0777) < 0 && errno != EEXIST) {
        bt_log("Failed to create AVRCP cover-art cache directory\n");
        receiver_art_data_size = 0;
        return;
    }

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        bt_log("Failed to open AVRCP cover-art cache file\n");
        receiver_art_data_size = 0;
        return;
    }

    size_t written = 0;
    while (written < receiver_art_data_size) {
        ssize_t result = write(fd, &receiver_art_data[written],
                               receiver_art_data_size - written);
        if (result > 0)
            written += (size_t)result;
        else if (result < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(fd);

    if (written != receiver_art_data_size ||
        rename(temp_path, Y1_BT_RECEIVER_ART_PATH) < 0) {
        unlink(temp_path);
        bt_log("Failed to save AVRCP cover-art thumbnail\n");
        receiver_art_data_size = 0;
        return;
    }

    receiver_art_data_size = 0;
    pthread_mutex_lock(&bt_mtx);
    receiver_art_generation++;
    pthread_mutex_unlock(&bt_mtx);
}

static void handle_receiver_bip_pdu(const uint8_t *pdu, uint16_t len)
{
    if (len < 3)
        return;
    uint16_t packet_len = ((uint16_t)pdu[1] << 8) | pdu[2];
    if (packet_len < 3 || packet_len > len)
        return;

    uint8_t response = pdu[0];
    size_t offset = 3;
    if (response == 0xA0 && receiver_art_connect_pending) {
        if (packet_len < 7)
            return;
        receiver_art_connect_pending = false;
        offset = 7;
        while (offset < packet_len) {
            uint8_t id = pdu[offset];
            uint8_t kind = id & 0xC0;
            if (kind == 0x80) {
                if (packet_len - offset < 2)
                    break;
                offset += 2;
            } else if (kind == 0xC0) {
                if (packet_len - offset < 5)
                    break;
                if (id == 0xCB) {
                    receiver_art_connection_id =
                        ((uint32_t)pdu[offset + 1] << 24) |
                        ((uint32_t)pdu[offset + 2] << 16) |
                        ((uint32_t)pdu[offset + 3] << 8) |
                        pdu[offset + 4];
                    receiver_art_has_connection_id = true;
                }
                offset += 5;
            } else {
                if (packet_len - offset < 3)
                    break;
                uint16_t header_len = ((uint16_t)pdu[offset + 1] << 8) |
                                      pdu[offset + 2];
                if (header_len < 3 || header_len > packet_len - offset)
                    break;
                offset += header_len;
            }
        }
        bip_obex_connected = true;
        receiver_send_obex_get_thumbnail();
        return;
    }

    if (response != 0x90 && response != 0xA0) {
        bt_log("Phone rejected AVRCP cover-art request\n");
        receiver_art_data_size = 0;
        return;
    }

    while (offset < packet_len) {
        uint8_t id = pdu[offset];
        uint8_t kind = id & 0xC0;
        if (kind == 0x80) {
            if (packet_len - offset < 2)
                break;
            offset += 2;
        } else if (kind == 0x40 || kind == 0x00) {
            if (packet_len - offset < 3)
                break;
            uint16_t header_len = ((uint16_t)pdu[offset + 1] << 8) |
                                  pdu[offset + 2];
            if (header_len < 3 || header_len > packet_len - offset)
                break;
            size_t body_len = header_len - 3;
            if ((id == 0x48 || id == 0x49) && body_len > 0) {
                if (body_len > sizeof(receiver_art_data) -
                               receiver_art_data_size) {
                    bt_log("AVRCP cover-art thumbnail exceeds cache limit\n");
                    receiver_art_data_size = 0;
                    return;
                }
                memcpy(&receiver_art_data[receiver_art_data_size],
                       &pdu[offset + 3], body_len);
                receiver_art_data_size += body_len;
            }
            offset += header_len;
        } else {
            break;
        }
    }

    if (response == 0x90)
        receiver_send_obex_get_thumbnail();
    else if (receiver_art_data_size > 0)
        receiver_save_cover_art();
}

bool y1_bt_receiver_send_control(enum y1_bt_receiver_command command)
{
    if (!bt_receiver_mode || cid_avctp_remote == 0)
        return false;

    uint8_t op_id;
    switch (command)
    {
        case Y1_BT_RECEIVER_PLAY_PAUSE:
            op_id = y1_bt_a2dp_is_streaming() ? 0x46 : 0x44;
            break;
        case Y1_BT_RECEIVER_NEXT:
            op_id = 0x4B;
            break;
        case Y1_BT_RECEIVER_PREVIOUS:
            op_id = 0x4C;
            break;
        case Y1_BT_RECEIVER_SEEK_FORWARD:
            op_id = 0x49;
            break;
        case Y1_BT_RECEIVER_SEEK_BACKWARD:
            op_id = 0x48;
            break;
        default:
            return false;
    }

    uint8_t request[8] = {
        (uint8_t)((avrcp_tx_label++ & 0x0F) << 4),
        0x11, 0x0E, 0x00, 0x48, 0x7C, op_id, 0x00
    };
    if (l2cap_send_pdu(cid_avctp_remote, request, sizeof(request)) < 0)
    {
        bt_log("Failed to send AVRCP receiver control\n");
        return false;
    }

    request[0] = (uint8_t)((avrcp_tx_label++ & 0x0F) << 4);
    request[6] |= 0x80;
    if (l2cap_send_pdu(cid_avctp_remote, request, sizeof(request)) < 0)
    {
        bt_log("Failed to release AVRCP receiver control\n");
        return false;
    }

    return true;
}

static void copy_receiver_attribute(char *out, size_t out_size,
                                    const uint8_t *text, size_t text_size)
{
    if (out_size == 0)
        return;
    if (text_size >= out_size)
        text_size = out_size - 1;
    memcpy(out, text, text_size);
    out[text_size] = '\0';
}

int y1_bt_get_active_device_volume(void)
{
    pthread_mutex_lock(&bt_mtx);
    int vol = -15;
    for (int i = 0; i < bt_num_devices; i++) {
        if (memcmp(bt_devices[i].bdaddr, active_bdaddr, 6) == 0) {
            if (bt_devices[i].volume != 0)
                vol = bt_devices[i].volume;
            break;
        }
    }
    pthread_mutex_unlock(&bt_mtx);
    return vol;
}

void y1_bt_store_active_volume(int vol)
{
    if (vol > 0)
        vol = 0;
    pthread_mutex_lock(&bt_mtx);
    for (int i = 0; i < bt_num_devices; i++) {
        if (memcmp(bt_devices[i].bdaddr, active_bdaddr, 6) == 0) {
            bt_devices[i].volume = vol;
            save_paired_devices();
            break;
        }
    }
    pthread_mutex_unlock(&bt_mtx);
}

/* ============================================================================
 * AVRCP / AVCTP Handler (PSM 0x0017) - Headphone Media Buttons -> Rockbox
 * ============================================================================ */
static void handle_avctp_pdu(const uint8_t *pdu, uint16_t len)
{
    /* AVCTP header (3 bytes: tx_label|cr|pid_hi, pid_lo) + AVRCP Ctype, Subunit, Opcode(0x7C PassThrough), OpID, StateLen */
    if (len < 8 || cid_avctp_remote == 0)
        return;

    uint8_t avctp_hdr = pdu[0];
    if ((avctp_hdr & 0x02) != 0) {
        if (bt_receiver_mode && receiver_play_status_request_pending &&
            (avctp_hdr >> 4) == receiver_play_status_request_label &&
            len >= 22 && pdu[5] == 0x00 && pdu[9] == 0x30) {
            uint16_t params_len = ((uint16_t)pdu[11] << 8) | pdu[12];
            if (params_len >= 9 && len >= 22) {
                pthread_mutex_lock(&bt_mtx);
                receiver_play_status = pdu[21];
                pthread_mutex_unlock(&bt_mtx);
            }
            receiver_play_status_request_pending = false;
        } else if (bt_receiver_mode && receiver_metadata_request_pending &&
            (avctp_hdr >> 4) == receiver_metadata_request_label &&
            len >= 14 && pdu[5] == 0x00 && pdu[9] == 0x20) {
            uint16_t params_len = ((uint16_t)pdu[11] << 8) | pdu[12];
            size_t pos = 13;
            size_t end = pos + params_len;
            char cover_handle[sizeof(receiver_art_handle)] = "";
            if (end > len)
                end = len;
            if (pos < end) {
                uint8_t attr_count = pdu[pos++];
                pthread_mutex_lock(&bt_mtx);
                for (uint8_t i = 0; i < attr_count && pos + 8 <= end; i++) {
                    uint32_t attr_id = ((uint32_t)pdu[pos] << 24) |
                                       ((uint32_t)pdu[pos + 1] << 16) |
                                       ((uint32_t)pdu[pos + 2] << 8) |
                                       pdu[pos + 3];
                    uint16_t charset = ((uint16_t)pdu[pos + 4] << 8) |
                                       pdu[pos + 5];
                    uint16_t text_len = ((uint16_t)pdu[pos + 6] << 8) |
                                        pdu[pos + 7];
                    pos += 8;
                    if (text_len > end - pos)
                        break;
                    if (charset == 0x006A) {
                        if (attr_id == 1)
                            copy_receiver_attribute(receiver_title,
                                                    sizeof(receiver_title),
                                                    &pdu[pos], text_len);
                        else if (attr_id == 2)
                            copy_receiver_attribute(receiver_artist,
                                                    sizeof(receiver_artist),
                                                    &pdu[pos], text_len);
                        else if (attr_id == 3)
                            copy_receiver_attribute(receiver_album,
                                                    sizeof(receiver_album),
                                                    &pdu[pos], text_len);
                        else if (attr_id == 8)
                            copy_receiver_attribute(cover_handle,
                                                    sizeof(cover_handle),
                                                    &pdu[pos], text_len);
                    }
                    pos += text_len;
                }
                pthread_mutex_unlock(&bt_mtx);
            }
            receiver_metadata_request_pending = false;
            if (cover_handle[0])
                receiver_request_cover_art(cover_handle);
        }
        return; /* Ignore responses, only handle commands (C/R bit = 0) */
    }

    uint8_t opcode = pdu[5];
    if (opcode == 0x7C) /* PASS THROUGH */
    {
        uint8_t op_id = pdu[6] & 0x7F;
        bool released = (pdu[6] & 0x80) != 0;

        /* Send AVRCP Accepted (0x09) response */
        uint8_t rsp[8];
        memcpy(rsp, pdu, 8);
        rsp[0] = (uint8_t)(avctp_hdr | 0x02); /* AVCTP Response bit */
        rsp[3] = 0x09;                        /* AVRCP Response: ACCEPTED */
        l2cap_send_pdu(cid_avctp_remote, rsp, 8);

        if (!released && !bt_receiver_mode) {
            int btn = 0;
            switch (op_id) {
                case 0x44: /* PLAY */
                case 0x46: /* PAUSE */
                case 0x45: /* STOP */
                    btn = BUTTON_MULTIMEDIA_PLAYPAUSE;
                    break;
                case 0x4B: /* FORWARD / NEXT */
                    btn = BUTTON_MULTIMEDIA_NEXT;
                    break;
                case 0x4C: /* BACKWARD / PREV */
                    btn = BUTTON_MULTIMEDIA_PREV;
                    break;
                case 0x41: /* VOLUME UP */
                    btn = BUTTON_MULTIMEDIA_VOLUME_UP;
                    break;
                case 0x42: /* VOLUME DOWN */
                    btn = BUTTON_MULTIMEDIA_VOLUME_DOWN;
                    break;
                default:
                    break;
            }
            if (btn != 0) {
                button_queue_post(btn, 0);
            }
        }
    }
    else if (opcode == 0x00 && len >= 10) /* AVRCP vendor-dependent metadata */
    {
        uint8_t pdu_id = pdu[9];
        if (pdu_id == 0x10 && len >= 14) /* GetCapabilities */
        {
            uint8_t params[5];
            uint16_t params_len;
            if (pdu[13] == 0x03) {
                params[0] = 0x02;
                params[1] = 0x03;
                params[2] = 0x01; /* Playback status changed */
                params[3] = 0x02; /* Track changed */
                params_len = 3;
            } else {
                params[0] = 0x03;
                params[1] = 0x00;
                params[2] = 0x19;
                params[3] = 0x58;
                params_len = 4;
            }
            send_avrcp_vendor_response(pdu, 0x0C, pdu_id, params,
                                       params_len);
        }
        else if (pdu_id == 0x31 && len >= 14) /* RegisterNotification */
        {
            uint8_t event_id = pdu[13];
            if (event_id == 0x01) /* EVENT_PLAYBACK_STATUS_CHANGED */
            {
                avrcp_play_notify_label = avctp_hdr >> 4;
                avrcp_play_notify_subunit = pdu[4];
                avrcp_play_notify_pending = true;

                uint8_t rsp[15] = {
                    (uint8_t)(avctp_hdr | 0x02),
                    0x11, 0x0E, 0x0F, pdu[4], 0x00,
                    0x00, 0x19, 0x58, 0x31, 0x00,
                    0x00, 0x02, 0x01, avrcp_play_status
                };
                l2cap_send_pdu(cid_avctp_remote, rsp, sizeof(rsp));
            }
            else if (event_id == 0x02) /* EVENT_TRACK_CHANGED */
            {
                avrcp_track_notify_label = avctp_hdr >> 4;
                avrcp_track_notify_subunit = pdu[4];
                avrcp_track_notify_pending = true;

                uint8_t params[9] = { 0x02 };
                uint8_t request[5] = { 0 };
                request[0] = avctp_hdr;
                request[4] = pdu[4];
                pthread_mutex_lock(&bt_mtx);
                uint64_t uid = avrcp_track_uid;
                pthread_mutex_unlock(&bt_mtx);
                for (int i = 0; i < 8; i++)
                    params[1 + i] = (uint8_t)(uid >> (56 - i * 8));
                send_avrcp_vendor_response(request, 0x0F, 0x31, params,
                                           sizeof(params));
            }
        }
        else if (pdu_id == 0x20 && len >= 22) /* GetElementAttributes */
        {
            char title[sizeof(avrcp_title)];
            char artist[sizeof(avrcp_artist)];
            char album[sizeof(avrcp_album)];
            pthread_mutex_lock(&bt_mtx);
            memcpy(title, avrcp_title, sizeof(title));
            memcpy(artist, avrcp_artist, sizeof(artist));
            memcpy(album, avrcp_album, sizeof(album));
            pthread_mutex_unlock(&bt_mtx);

            static const uint32_t all_attrs[] = { 1, 2, 3 };
            const uint8_t requested = pdu[21];
            size_t attr_count = requested ? requested :
                                sizeof(all_attrs) / sizeof(all_attrs[0]);
            size_t available = (len - 22) / 4;
            if (requested && attr_count > available)
                attr_count = available;

            uint8_t params[480];
            size_t params_len = 1;
            uint8_t returned = 0;
            for (size_t i = 0; i < attr_count; i++)
            {
                uint32_t attr = requested ?
                    ((uint32_t)pdu[22 + i * 4] << 24) |
                    ((uint32_t)pdu[23 + i * 4] << 16) |
                    ((uint32_t)pdu[24 + i * 4] << 8) |
                    pdu[25 + i * 4] : all_attrs[i];
                const char *text = attr == 1 ? title :
                                   attr == 2 ? artist :
                                   attr == 3 ? album : NULL;
                if (!text || params_len + 8 + strlen(text) > sizeof(params))
                    continue;

                size_t text_len = strlen(text);
                params[params_len++] = (uint8_t)(attr >> 24);
                params[params_len++] = (uint8_t)(attr >> 16);
                params[params_len++] = (uint8_t)(attr >> 8);
                params[params_len++] = (uint8_t)attr;
                params[params_len++] = 0x00;
                params[params_len++] = 0x6A; /* UTF-8 */
                params[params_len++] = (uint8_t)(text_len >> 8);
                params[params_len++] = (uint8_t)text_len;
                memcpy(&params[params_len], text, text_len);
                params_len += text_len;
                returned++;
            }
            params[0] = returned;
            send_avrcp_vendor_response(pdu, 0x0C, pdu_id, params,
                                       (uint16_t)params_len);
        }
        else if (pdu_id == 0x30 && len >= 13) /* GetPlayStatus */
        {
            uint8_t params[9];
            pthread_mutex_lock(&bt_mtx);
            uint32_t duration = avrcp_duration_ms;
            uint32_t position = avrcp_position_ms;
            uint8_t status = avrcp_play_status;
            pthread_mutex_unlock(&bt_mtx);
            params[0] = (uint8_t)(duration >> 24);
            params[1] = (uint8_t)(duration >> 16);
            params[2] = (uint8_t)(duration >> 8);
            params[3] = (uint8_t)duration;
            params[4] = (uint8_t)(position >> 24);
            params[5] = (uint8_t)(position >> 16);
            params[6] = (uint8_t)(position >> 8);
            params[7] = (uint8_t)position;
            params[8] = status;
            send_avrcp_vendor_response(pdu, 0x0C, pdu_id, params,
                                       sizeof(params));
        }
    }
}

/* ============================================================================
 * L2CAP Signaling & Channel Multiplexer
 * ============================================================================ */
static void check_avdtp_channels_ready(void)
{
    if (avdtp_sig_local_conf_done && avdtp_sig_remote_conf_done && !avdtp_discover_sent) {
        if (avdtp_initiator) {
            avdtp_discover_sent = true;
            avdtp_send_cmd(AVDTP_DISCOVER, NULL, 0);
        }
    }
    if (avdtp_med_local_conf_done && avdtp_med_remote_conf_done && !a2dp_streaming) {
        if (avdtp_initiator || avdtp_opening_media) {
            avdtp_opening_media = false;
            uint8_t start_param[1] = { (uint8_t)(remote_seid << 2) };
            avdtp_send_cmd(AVDTP_START, start_param, 1);
        }
    }
}

static void handle_l2cap_sig(const uint8_t *sig, uint16_t len)
{
    uint16_t off = 0;
    while (off + 4 <= len)
    {
        uint8_t code = sig[off];
        uint8_t ident = sig[off + 1];
        uint16_t clen = (uint16_t)sig[off + 2] | ((uint16_t)sig[off + 3] << 8);
        const uint8_t *d = &sig[off + 4];
        if (off + 4 + clen > len)
            break;

        switch (code)
        {
            case L2CAP_CONN_REQ: {
                if (clen >= 4) {
                    uint16_t psm  = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
                    uint16_t scid = (uint16_t)d[2] | ((uint16_t)d[3] << 8);
                    uint16_t dcid = 0;
                    uint16_t result = 0x0002; /* PSM not supported */

                    if (psm == PSM_SDP) {
                        dcid = CID_SDP_LOCAL;
                        cid_sdp_remote = scid;
                        result = 0;
                    } else if (psm == PSM_AVDTP) {
                        if (cid_avdtp_sig_remote == 0) {
                            dcid = CID_AVDTP_SIG_LOCAL;
                            cid_avdtp_sig_remote = scid;
                            avdtp_sig_local_conf_done = false;
                            avdtp_sig_remote_conf_done = false;
                            result = 0;
                        } else {
                            dcid = CID_AVDTP_MED_LOCAL;
                            cid_avdtp_med_remote = scid;
                            avdtp_med_local_conf_done = false;
                            avdtp_med_remote_conf_done = false;
                            result = 0;
                        }
                    } else if (psm == PSM_AVCTP) {
                        dcid = CID_AVCTP_LOCAL;
                        cid_avctp_remote = scid;
                        result = 0;
                    } else if (psm == PSM_BIP && bt_receiver_mode) {
                        dcid = CID_BIP_LOCAL;
                        cid_bip_remote = scid;
                        bip_local_conf_done = false;
                        bip_remote_conf_done = false;
                        result = 0;
                    }

                    uint8_t rsp[8] = {
                        (uint8_t)(dcid & 0xFF), (uint8_t)(dcid >> 8),
                        (uint8_t)(scid & 0xFF), (uint8_t)(scid >> 8),
                        (uint8_t)(result & 0xFF), (uint8_t)(result >> 8),
                        0x00, 0x00
                    };
                    l2cap_send_sig(L2CAP_CONN_RSP, ident, rsp, 8);
                    if (result == 0) {
                        l2cap_send_conf_req(scid, 895);
                    }
                }
                break;
            }
            case L2CAP_CONN_RSP: {
                if (clen >= 8) {
                    uint16_t dcid   = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
                    uint16_t scid   = (uint16_t)d[2] | ((uint16_t)d[3] << 8);
                    uint16_t result = (uint16_t)d[4] | ((uint16_t)d[5] << 8);
                    if (result == 0x0000) {
                        if (scid == CID_AVDTP_SIG_LOCAL) {
                            cid_avdtp_sig_remote = dcid;
                            l2cap_send_conf_req(dcid, 895);
                        } else if (scid == CID_AVDTP_MED_LOCAL) {
                            cid_avdtp_med_remote = dcid;
                            l2cap_send_conf_req(dcid, 895);
                        } else if (scid == CID_AVCTP_LOCAL) {
                            cid_avctp_remote = dcid;
                            l2cap_send_conf_req(dcid, 512);
                        } else if (scid == CID_BIP_LOCAL) {
                            cid_bip_remote = dcid;
                            bip_local_conf_done = false;
                            bip_remote_conf_done = false;
                            l2cap_send_conf_req(dcid, 512);
                        }
                    }
                }
                break;
            }
            case L2CAP_CONF_REQ: {
                if (clen >= 4) {
                    uint16_t dcid = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
                    uint16_t rcid = 0;
                    if (dcid == CID_SDP_LOCAL) rcid = cid_sdp_remote;
                    else if (dcid == CID_AVDTP_SIG_LOCAL) { rcid = cid_avdtp_sig_remote; avdtp_sig_remote_conf_done = true; }
                    else if (dcid == CID_AVDTP_MED_LOCAL) { rcid = cid_avdtp_med_remote; avdtp_med_remote_conf_done = true; }
                    else if (dcid == CID_AVCTP_LOCAL) rcid = cid_avctp_remote;
                    else if (dcid == CID_BIP_LOCAL) {
                        rcid = cid_bip_remote;
                        bip_remote_conf_done = true;
                    }

                    uint8_t rsp[6] = {
                        (uint8_t)(rcid & 0xFF), (uint8_t)(rcid >> 8),
                        0x00, 0x00, /* Flags */
                        0x00, 0x00  /* Result: Success */
                    };
                    l2cap_send_sig(L2CAP_CONF_RSP, ident, rsp, 6);
                    check_avdtp_channels_ready();
                    receiver_bip_try_connect();
                }
                break;
            }
            case L2CAP_CONF_RSP: {
                if (clen >= 6) {
                    uint16_t scid   = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
                    uint16_t result = (uint16_t)d[4] | ((uint16_t)d[5] << 8);
                    if (result == 0x0000) {
                        if (scid == CID_AVDTP_SIG_LOCAL) avdtp_sig_local_conf_done = true;
                        else if (scid == CID_AVDTP_MED_LOCAL) avdtp_med_local_conf_done = true;
                        else if (scid == CID_AVCTP_LOCAL)
                            request_receiver_metadata();
                        else if (scid == CID_BIP_LOCAL)
                            bip_local_conf_done = true;
                        check_avdtp_channels_ready();
                        receiver_bip_try_connect();
                    }
                }
                break;
            }
            case L2CAP_DISCONN_REQ: {
                if (clen >= 4) {
                    uint16_t dcid = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
                    uint16_t scid = (uint16_t)d[2] | ((uint16_t)d[3] << 8);
                    uint8_t rsp[4] = { d[0], d[1], d[2], d[3] };
                    l2cap_send_sig(L2CAP_DISCONN_RSP, ident, rsp, 4);
                    if (dcid == CID_SDP_LOCAL || scid == cid_sdp_remote) cid_sdp_remote = 0;
                    if (dcid == CID_AVDTP_MED_LOCAL || scid == cid_avdtp_med_remote) {
                        cid_avdtp_med_remote = 0;
                        a2dp_streaming = false;
                        if (bt_receiver_mode)
                            y1_audio_receiver_set_active(false,
                                                          a2dp_sample_rate());
                    }
                    if (dcid == CID_AVDTP_SIG_LOCAL || scid == cid_avdtp_sig_remote) {
                        cid_avdtp_sig_remote = 0;
                        a2dp_streaming = false;
                        if (bt_receiver_mode)
                            y1_audio_receiver_set_active(false,
                                                          a2dp_sample_rate());
                    }
                    if (dcid == CID_BIP_LOCAL || scid == cid_bip_remote) {
                        cid_bip_remote = 0;
                        bip_local_conf_done = false;
                        bip_remote_conf_done = false;
                        bip_obex_connected = false;
                        receiver_art_connect_pending = false;
                    }
                }
                break;
            }
            case L2CAP_ECHO_REQ:
                l2cap_send_sig(L2CAP_ECHO_RSP, ident, d, clen);
                break;
            case L2CAP_INFO_REQ: {
                if (clen >= 2) {
                    uint16_t info_type = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
                    if (info_type == 0x0002) { /* Extended Features Mask */
                        uint8_t rsp[8] = { 0x02, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00 };
                        l2cap_send_sig(L2CAP_INFO_RSP, ident, rsp, 8);
                    } else {
                        uint8_t rsp[4] = { d[0], d[1], 0x01, 0x00 }; /* Not supported */
                        l2cap_send_sig(L2CAP_INFO_RSP, ident, rsp, 4);
                    }
                }
                break;
            }
            default:
                break;
        }
        off += 4 + clen;
    }
}

static void handle_acl_packet(const uint8_t *pkt, size_t len)
{
    /* pkt[0] == 0x02, pkt[1..2] == handle|flags, pkt[3..4] == acl_len, pkt[5..6] == l2cap_len, pkt[7..8] == cid */
    if (len < 9)
        return;
    uint16_t l2cap_len = (uint16_t)pkt[5] | ((uint16_t)pkt[6] << 8);
    uint16_t cid       = (uint16_t)pkt[7] | ((uint16_t)pkt[8] << 8);
    if (len < (size_t)(9 + l2cap_len))
        l2cap_len = (uint16_t)(len - 9);

    const uint8_t *payload = &pkt[9];
    if (cid == 0x0001) {
        handle_l2cap_sig(payload, l2cap_len);
    } else if (cid == CID_SDP_LOCAL) {
        handle_sdp_pdu(payload, l2cap_len);
    } else if (cid == CID_AVDTP_SIG_LOCAL) {
        handle_avdtp_sig_pdu(payload, l2cap_len);
    } else if (cid == CID_AVCTP_LOCAL) {
        handle_avctp_pdu(payload, l2cap_len);
    } else if (cid == CID_BIP_LOCAL && bt_receiver_mode) {
        handle_receiver_bip_pdu(payload, l2cap_len);
    } else if (cid == CID_AVDTP_MED_LOCAL && bt_receiver_mode &&
               a2dp_streaming && a2dp_sbc_ready && l2cap_len > 13) {
        const uint8_t *rtp = payload;
        uint8_t frame_count = rtp[12] & 0x3F;
        size_t offset = 13;
        for (uint8_t i = 0; i < frame_count && offset < l2cap_len; i++) {
            int16_t decoded[512];
            size_t decoded_bytes = 0;
            ssize_t consumed = sbc_decode(&a2dp_sbc, &rtp[offset],
                                          l2cap_len - offset, decoded,
                                          sizeof(decoded), &decoded_bytes);
            if (consumed <= 0 || decoded_bytes == 0)
                break;
            offset += (size_t)consumed;
            size_t samples = decoded_bytes / sizeof(int16_t);
            if (a2dp_sbc.mode == SBC_MODE_MONO) {
                for (size_t sample = samples; sample > 0; sample--) {
                    int16_t value = decoded[sample - 1];
                    decoded[(sample - 1) * 2] = value;
                    decoded[(sample - 1) * 2 + 1] = value;
                }
                samples *= 2;
            }
            y1_audio_receiver_write(decoded, samples / 2);
        }
    }
}

/* ============================================================================
 * HCI Event Handler
 * ============================================================================ */
static void step_remote_name_resolution(void)
{
    pthread_mutex_lock(&bt_mtx);
    while (resolve_idx < bt_num_devices) {
        struct y1_bt_device *d = &bt_devices[resolve_idx];
        if (!d->paired && is_placeholder_name(d->name)) {
            uint8_t cmd[10];
            memcpy(&cmd[0], d->bdaddr, 6);
            cmd[6] = 0x01; /* Page Scan Repetition Mode R1 */
            cmd[7] = 0x00; /* Reserved */
            cmd[8] = 0x00; /* Clock offset lo */
            cmd[9] = 0x00; /* Clock offset hi */
            pthread_mutex_unlock(&bt_mtx);
            hci_send_cmd(0x0419, cmd, 10); /* HCI_Remote_Name_Request */
            return;
        }
        resolve_idx++;
    }
    resolving_names = false;
    if (bt_state == Y1_BT_STATE_SCANNING)
        bt_state = (acl_handle != 0xFFFF) ? (a2dp_streaming ? Y1_BT_STATE_STREAMING : Y1_BT_STATE_CONNECTED) : Y1_BT_STATE_IDLE;
    pthread_mutex_unlock(&bt_mtx);
}

static void parse_eir_name(const uint8_t *eir, size_t max_len, char *out_name, size_t name_sz)
{
    size_t pos = 0;
    while (pos < max_len) {
        uint8_t elen = eir[pos];
        if (elen == 0 || pos + 1 + elen > max_len)
            break;
        uint8_t etype = eir[pos + 1];
        if ((etype == 0x09 || etype == 0x08) && elen > 1) { /* Complete or Shortened Local Name */
            size_t copy_len = elen - 1;
            if (copy_len >= name_sz)
                copy_len = name_sz - 1;
            memcpy(out_name, &eir[pos + 2], copy_len);
            out_name[copy_len] = '\0';
            return;
        }
        pos += 1 + elen;
    }
}

static void handle_hci_event(const uint8_t *pkt, size_t len)
{
    /* pkt[0] == 0x04, pkt[1] == evt_code, pkt[2] == plen, pkt[3..] == params */
    if (len < 3)
        return;
    uint8_t evt  = pkt[1];
    uint8_t plen = pkt[2];
    const uint8_t *p = &pkt[3];
    if (len < (size_t)(3 + plen))
        return;

    switch (evt)
    {
        case 0x01: /* Inquiry Complete */
            resolving_names = true;
            resolve_idx = 0;
            step_remote_name_resolution();
            break;

        case 0x02: /* Standard Inquiry Result */ {
            if (plen >= 1) {
                uint8_t num = p[0];
                pthread_mutex_lock(&bt_mtx);
                for (uint8_t i = 0; i < num && (1 + (i + 1) * 14) <= plen; i++) {
                    const uint8_t *entry = &p[1 + i * 14];
                    find_or_create_device(entry);
                }
                pthread_mutex_unlock(&bt_mtx);
            }
            break;
        }

        case 0x22: /* Inquiry Result with RSSI */ {
            if (plen >= 1) {
                uint8_t num = p[0];
                pthread_mutex_lock(&bt_mtx);
                for (uint8_t i = 0; i < num && (1 + (i + 1) * 14) <= plen; i++) {
                    const uint8_t *entry = &p[1 + i * 14];
                    struct y1_bt_device *d = find_or_create_device(entry);
                    if (d) d->rssi = (int8_t)entry[13];
                }
                pthread_mutex_unlock(&bt_mtx);
            }
            break;
        }

        case 0x2F: /* Extended Inquiry Result */ {
            if (plen >= 15) {
                pthread_mutex_lock(&bt_mtx);
                struct y1_bt_device *d = find_or_create_device(&p[1]);
                if (d) {
                    d->rssi = (int8_t)p[14];
                    if (plen > 15) {
                        parse_eir_name(&p[15], plen - 15, d->name, sizeof(d->name));
                    }
                }
                pthread_mutex_unlock(&bt_mtx);
            }
            break;
        }

        case 0x07: /* Remote Name Request Complete */ {
            if (plen >= 7) {
                uint8_t status = p[0];
                const uint8_t *bd = &p[1];
                if (status == 0x00 && plen > 7) {
                    pthread_mutex_lock(&bt_mtx);
                    struct y1_bt_device *d = find_or_create_device(bd);
                    if (d && p[7] != '\0') {
                        strncpy(d->name, (const char *)&p[7], sizeof(d->name) - 1);
                        d->name[sizeof(d->name) - 1] = '\0';
                        if (d->paired)
                            save_paired_devices();
                        if (d->connected)
                            strncpy(connected_name, d->name, sizeof(connected_name) - 1);
                    }
                    pthread_mutex_unlock(&bt_mtx);
                }
            }
            if (resolving_names) {
                resolve_idx++;
                step_remote_name_resolution();
            }
            break;
        }

        case 0x04: /* Connection Request (Incoming from Headphone) */ {
            if (plen >= 10) {
                uint8_t accept[7];
                memcpy(&accept[0], &p[0], 6);
                accept[6] = 0x00; /* Become Master (0x00) for clean A2DP streaming */
                avdtp_initiator = false;
                hci_send_cmd(0x0409, accept, 7); /* HCI_Accept_Connection_Request */
            }
            break;
        }

        case 0x03: /* Connection Complete */ {
            if (plen >= 11) {
                uint8_t status = p[0];
                uint16_t handle = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
                const uint8_t *bd = &p[3];
                if (status == 0x00) {
                    bool initiated = avdtp_initiator;
                    reset_bt_session();
                    avdtp_initiator = initiated;
                    acl_handle = handle & 0x0FFF;
                    sbc_is_xq = false;
                    memcpy(active_bdaddr, bd, 6);

                    pthread_mutex_lock(&bt_mtx);
                    for (int i = 0; i < bt_num_devices; i++)
                        bt_devices[i].connected = false;
                    struct y1_bt_device *d = find_or_create_device(bd);
                    if (d) {
                        d->connected = true;
                        strncpy(connected_name, d->name, sizeof(connected_name) - 1);
                        move_paired_device_to_end(d);
                    }
                    bt_state = Y1_BT_STATE_CONNECTED;
                    pthread_mutex_unlock(&bt_mtx);

                    char msg[96];
                    snprintf(msg, sizeof(msg), "ACL Connected handle=0x%04X (%s)\n", acl_handle, connected_name);
                    bt_log(msg);

                    /* Request Remote Name if unknown */
                    if (is_placeholder_name(connected_name)) {
                        uint8_t rnr[10] = { bd[0], bd[1], bd[2], bd[3], bd[4], bd[5], 0x01, 0x00, 0x00, 0x00 };
                        hci_send_cmd(0x0419, rnr, 10);
                    }

                    /* If we initiated the connection, request authentication -> then open L2CAP AVDTP */
                    if (avdtp_initiator || bt_receiver_mode) {
                        uint8_t auth[2] = { (uint8_t)(acl_handle & 0xFF), (uint8_t)(acl_handle >> 8) };
                        hci_send_cmd(0x0411, auth, 2); /* HCI_Authentication_Requested */
                    }
                } else {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "ACL Connection failed status=0x%02X\n", status);
                    bt_log(msg);
                    acl_handle = 0xFFFF;
                    bt_state = Y1_BT_STATE_IDLE;
                }
            }
            break;
        }

        case 0x05: /* Disconnection Complete */ {
            reset_bt_session();
            sbc_is_xq = false;
            pthread_mutex_lock(&bt_mtx);
            for (int i = 0; i < bt_num_devices; i++)
                bt_devices[i].connected = false;
            if (bt_is_target_enabled())
                bt_state = Y1_BT_STATE_IDLE;
            pthread_mutex_unlock(&bt_mtx);
            bt_log("ACL Disconnected\n");
            if (bt_receiver_mode)
                y1_audio_receiver_set_active(false, a2dp_sample_rate());
            else
                y1_bt_audio_disconnect_hook();
            if (pending_switch_connect && bt_is_target_enabled()) {
                memcpy(req_connect_bdaddr, pending_switch_bdaddr,
                       sizeof(req_connect_bdaddr));
                pending_switch_connect = false;
                req_connect = true;
                bt_log("Starting connection to selected Bluetooth device\n");
            }
            break;
        }

        case 0x06: /* Authentication Complete */ {
            if (plen >= 3) {
                uint8_t status = p[0];
                if (status == 0x00 && acl_handle != 0xFFFF) {
                    /* Enable link encryption, and if Initiator, open L2CAP AVDTP signaling */
                    uint8_t enc[3] = { (uint8_t)(acl_handle & 0xFF), (uint8_t)(acl_handle >> 8), 0x01 };
                    hci_send_cmd(0x0413, enc, 3); /* HCI_Set_Connection_Encryption */
                    if (bt_receiver_mode && cid_avctp_remote == 0) {
                        l2cap_send_conn_req(PSM_AVCTP, CID_AVCTP_LOCAL);
                    } else if (avdtp_initiator && cid_avdtp_sig_remote == 0) {
                        l2cap_send_conn_req(PSM_AVDTP, CID_AVDTP_SIG_LOCAL);
                    }
                } else if (avdtp_initiator && acl_handle != 0xFFFF && cid_avdtp_sig_remote == 0) {
                    /* Even if headset didn't require auth, proceed to L2CAP AVDTP */
                    l2cap_send_conn_req(PSM_AVDTP, CID_AVDTP_SIG_LOCAL);
                }
            }
            break;
        }

        case 0x14: /* Mode Change (Sniff / Active) */ {
            if (plen >= 6) {
                uint8_t mode = p[3]; /* 0x00 = Active, 0x02 = Sniff */
                in_sniff_mode = (mode == 0x02);
            }
            break;
        }

        case 0x16: /* PIN Code Request (Legacy BT 2.0 devices) */ {
            if (plen >= 6) {
                const uint8_t *bd = &p[0];
                pthread_mutex_lock(&bt_mtx);
                struct y1_bt_device *d = find_or_create_device(bd);
                const char *pin = (d && d->pin[0]) ? d->pin : "0000";
                uint8_t pin_len = (uint8_t)strlen(pin);
                if (pin_len > 16) pin_len = 16;
                uint8_t reply[23];
                memset(reply, 0, sizeof(reply));
                memcpy(&reply[0], bd, 6);
                reply[6] = pin_len;
                memcpy(&reply[7], pin, pin_len);
                pthread_mutex_unlock(&bt_mtx);
                hci_send_cmd(0x040D, reply, 23); /* HCI_PIN_Code_Request_Reply */
            }
            break;
        }

        case 0x17: /* Link Key Request */ {
            if (plen >= 6) {
                const uint8_t *bd = &p[0];
                pthread_mutex_lock(&bt_mtx);
                struct y1_bt_device *d = find_or_create_device(bd);
                if (d && d->paired) {
                    uint8_t reply[22];
                    memcpy(&reply[0], bd, 6);
                    memcpy(&reply[6], d->link_key, 16);
                    pthread_mutex_unlock(&bt_mtx);
                    hci_send_cmd(0x040B, reply, 22); /* HCI_Link_Key_Request_Reply */
                } else {
                    pthread_mutex_unlock(&bt_mtx);
                    hci_send_cmd(0x040C, bd, 6); /* HCI_Link_Key_Request_Negative_Reply */
                }
            }
            break;
        }

        case 0x18: /* Link Key Notification (New pairing completed!) */ {
            if (plen >= 23) {
                const uint8_t *bd = &p[0];
                pthread_mutex_lock(&bt_mtx);
                struct y1_bt_device *d = find_or_create_device(bd);
                if (d) {
                    memcpy(d->link_key, &p[6], 16);
                    d->key_type = p[22];
                    d->paired = true;
                    save_paired_devices();
                }
                pthread_mutex_unlock(&bt_mtx);
                bt_log("Saved new Link Key to bt_paired.dat\n");
            }
            break;
        }

        case 0x31: /* IO Capability Request (Secure Simple Pairing) */ {
            if (plen >= 6) {
                uint8_t reply[9];
                memcpy(&reply[0], &p[0], 6);
                reply[6] = 0x03; /* IO_Capability: NoInputNoOutput ("Just Works") */
                reply[7] = 0x00; /* OOB_Data_Present: No */
                reply[8] = 0x04; /* Authentication_Requirements: General Bonding, No MITM */
                hci_send_cmd(0x042B, reply, 9); /* HCI_IO_Capability_Request_Reply */
            }
            break;
        }

        case 0x33: /* User Confirmation Request (SSP Just Works) */ {
            if (plen >= 6) {
                hci_send_cmd(0x042C, &p[0], 6); /* HCI_User_Confirmation_Request_Reply */
            }
            break;
        }

        case 0x13: /* Number Of Completed Packets */
            break;

        default:
            break;
    }
}

/* ============================================================================
 * MT6572 Bluetooth Radio Initialization Sequence
 * Reverse-engineered from libbluetooth_mtk.so (bt_init_script_6572)
 * ============================================================================ */
static int wait_cmd_complete(uint16_t expected_opcode, int timeout_ms)
{
    uint8_t buf[256];
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        struct pollfd pfd = { .fd = stpbt_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 50);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t r = read(stpbt_fd, buf, sizeof(buf));
            if (r >= 6 && buf[0] == HCI_PKT_EVT && buf[1] == 0x0E) {
                uint16_t op = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
                if (op == expected_opcode)
                    return 0;
            }
        }
        elapsed += 50;
    }
    return -1;
}

static int run_cmd_sync(uint16_t opcode, const void *param, uint8_t plen)
{
    if (hci_send_cmd(opcode, param, plen) < 0)
        return -1;
    return wait_cmd_complete(opcode, 1500);
}

static bool mt6572_bt_hw_open_and_init(void)
{
    reset_bt_session();
    mknod("/dev/stpwmt", S_IFCHR | 0666, makedev(190, 0));
    mknod("/dev/stpbt",  S_IFCHR | 0666, makedev(192, 0));

    bt_log("Opening /dev/stpbt (triggers MT6572 CONNSYS WMT power-on)...\n");
    stpbt_fd = open("/dev/stpbt", O_RDWR | O_NOCTTY);
    if (stpbt_fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ERROR: open(/dev/stpbt) failed: %s (errno=%d)\n", strerror(errno), errno);
        bt_log(msg);
        return false;
    }

    int flags = fcntl(stpbt_fd, F_GETFL, 0);
    fcntl(stpbt_fd, F_SETFL, flags | O_NONBLOCK);

    /* 1. MT6572 Vendor GORM Init Script (from libbluetooth_mtk.so @ 0x4024) */
    static const uint8_t bdaddr_y1[6]   = { 0x59, 0x31, 0x72, 0x65, 0x46, 0x00 }; /* 00:46:65:72:31:59 */
    static const uint8_t radio_6572[6]  = { 0x07, 0x80, 0x00, 0x06, 0x05, 0x07 };
    static const uint8_t txpwr_6572[3]  = { 0xFF, 0xFF, 0xFF };
    static const uint8_t sleep_6572[7]  = { 0x03, 0x40, 0x1F, 0x40, 0x1F, 0x00, 0x04 };

    run_cmd_sync(0xFC1A, bdaddr_y1,  6); /* GORMcmd_HCC_Set_Local_BD_Addr */
    run_cmd_sync(0xFC79, radio_6572, 6); /* GORMcmd_HCC_Set_Radio */
    run_cmd_sync(0xFC93, txpwr_6572, 3); /* GORMcmd_HCC_Set_TX_Power_Offset */
    run_cmd_sync(0xFC7A, sleep_6572, 7); /* GORMcmd_HCC_Set_Sleep_Timeout */
    if (run_cmd_sync(0x0C03, NULL, 0) < 0) { /* HCI_Reset */
        bt_log("Warning: HCI_Reset timed out on /dev/stpbt\n");
    }

    /* 2. Standard Bluetooth 2.1 + EDR Host Controller Setup */
    uint8_t ssp_enable = 0x01;
    run_cmd_sync(0x0C56, &ssp_enable, 1); /* HCI_Write_Simple_Pairing_Mode = Enabled */

    static const uint8_t evt_mask[8] = { 0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D };
    run_cmd_sync(0x0C01, evt_mask, 8);    /* HCI_Set_Event_Mask */

    uint8_t local_name[248];
    memset(local_name, 0, sizeof(local_name));
    strcpy((char *)local_name, "Innioasis Y1");
    run_cmd_sync(0x0C13, local_name, 248); /* HCI_Write_Local_Name */

    static const uint8_t cod[3] = { 0x1C, 0x04, 0x2C }; /* Portable Audio / Rendering / Capturing */
    run_cmd_sync(0x0C24, cod, 3);          /* HCI_Write_Class_Of_Device */

    uint8_t inq_mode = 0x02;               /* Extended Inquiry Result format */
    run_cmd_sync(0x0C45, &inq_mode, 1);    /* HCI_Write_Inquiry_Mode */

    uint8_t page_to[2] = { 0x00, 0x40 };   /* 0x4000 slots (~10.24s) */
    run_cmd_sync(0x0C18, page_to, 2);      /* HCI_Write_Page_Timeout */

    uint8_t link_pol[2] = { 0x0F, 0x00 };  /* Enable Role Switch, Hold, Sniff, Park */
    run_cmd_sync(0x080F, link_pol, 2);     /* HCI_Write_Default_Link_Policy_Settings */

    uint8_t scan_en = 0x03;                /* Inquiry + Page Scan Enabled */
    run_cmd_sync(0x0C1A, &scan_en, 1);     /* HCI_Write_Scan_Enable */

    bt_log("MT6572 Bluetooth Radio initialized and discoverable as 'Innioasis Y1'!\n");
    return true;
}

/* ============================================================================
 * Background Bluetooth Worker Thread (HCI Rx + A2DP RTP Media Tx)
 * ============================================================================ */
static void drain_a2dp_pcm_and_transmit(void)
{
    if (bt_receiver_mode || !a2dp_streaming ||
        cid_avdtp_med_remote == 0 || acl_handle == 0xFFFF ||
        !a2dp_sbc_ready)
        return;

    size_t codesize = sbc_get_codesize(&a2dp_sbc);
    if (codesize == 0 || codesize > 1024)
        codesize = 512;

    size_t samples_per_frame = codesize >> 2; /* 16-bit stereo = 4 bytes per sample */

    size_t framelen = sbc_get_frame_length(&a2dp_sbc);
    int frames_per_pkt = (framelen > 0) ? (int)((895 - 13) / framelen) : 5;
    if (frames_per_pkt > 5) frames_per_pkt = 5;
    if (frames_per_pkt < 1) frames_per_pkt = 1;

    size_t samples_per_pkt = samples_per_frame * frames_per_pkt;

    int pkts_sent = 0;
    while (pkts_sent < 4)
    {
        size_t r = pcm_ring_r;
        size_t w = pcm_ring_w;
        size_t avail = (w - r) & (PCM_RING_SAMPLES - 1);
        if (avail < samples_per_pkt)
            break;

        uint8_t rtp_pkt[1024];
        /* 12-byte RTP Header + 1-byte A2DP SBC Payload Header */
        rtp_pkt[0]  = 0x80; /* RTP v2 */
        rtp_pkt[1]  = 0x60; /* Payload type 96 (Dynamic) */
        rtp_pkt[2]  = (uint8_t)(rtp_seq_num >> 8);
        rtp_pkt[3]  = (uint8_t)(rtp_seq_num & 0xFF);
        rtp_seq_num++;
        rtp_pkt[4]  = (uint8_t)(rtp_timestamp >> 24);
        rtp_pkt[5]  = (uint8_t)(rtp_timestamp >> 16);
        rtp_pkt[6]  = (uint8_t)(rtp_timestamp >> 8);
        rtp_pkt[7]  = (uint8_t)(rtp_timestamp & 0xFF);
        rtp_pkt[8]  = 0x00;
        rtp_pkt[9]  = 0x00;
        rtp_pkt[10] = 0x00;
        rtp_pkt[11] = 0x01; /* SSRC = 1 */
        rtp_pkt[12] = 0; /* Updated with the number of successfully encoded frames */

        size_t pkt_len = 13;
        int encoded_frames = 0;
        for (int f = 0; f < frames_per_pkt; f++) {
            int16_t block_pcm[512];
            for (size_t s = 0; s < samples_per_frame; s++) {
                size_t idx = ((r + s) & (PCM_RING_SAMPLES - 1)) * 2;
                block_pcm[s * 2 + 0] = pcm_ring[idx + 0];
                block_pcm[s * 2 + 1] = pcm_ring[idx + 1];
            }
            r = (r + samples_per_frame) & (PCM_RING_SAMPLES - 1);

            ssize_t written = 0;
            ssize_t res = sbc_encode(&a2dp_sbc, block_pcm, codesize,
                                     &rtp_pkt[pkt_len], sizeof(rtp_pkt) - pkt_len, &written);
            if (res > 0 && written > 0) {
                pkt_len += (size_t)written;
                encoded_frames++;
            } else {
                bt_log("SBC encoder failed to produce a complete frame\n");
                break;
            }
        }

        if (encoded_frames == 0)
            break;

        rtp_pkt[12] = (uint8_t)encoded_frames;
        if (l2cap_send_pdu(cid_avdtp_med_remote, rtp_pkt, (uint16_t)pkt_len) < 0)
            break;

        pcm_ring_r = r;
        rtp_timestamp += (uint32_t)(samples_per_frame * encoded_frames);
        pkts_sent++;
    }
}

static void *bt_worker_thread(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&bt_mtx);
    load_paired_devices();
    bt_paired_devices_loaded = true;
    pthread_mutex_unlock(&bt_mtx);

    uint8_t rx_buf[2048];
    size_t rx_len = 0;

    while (1)
    {
        if (!bt_is_target_enabled())
        {
            if (stpbt_fd >= 0) {
                if (acl_handle != 0xFFFF) {
                    uint8_t disc[3] = { (uint8_t)(acl_handle & 0xFF), (uint8_t)(acl_handle >> 8), 0x13 };
                    hci_send_cmd(0x0406, disc, 3);
                    usleep(50000);
                }
                close(stpbt_fd);
                stpbt_fd = -1;
                reset_bt_session();
                pending_switch_connect = false;
                pthread_mutex_lock(&bt_mtx);
                for (int i = 0; i < bt_num_devices; i++)
                    bt_devices[i].connected = false;
                pthread_mutex_unlock(&bt_mtx);
                if (bt_receiver_mode)
                    y1_audio_receiver_set_active(false, a2dp_sample_rate());
                else
                    y1_bt_audio_disconnect_hook();
                bt_state = Y1_BT_STATE_OFF;
                bt_log("Closed /dev/stpbt (MT6572 BT Radio powered down)\n");
            }
            bt_autoconnect_attempted = false;
            bt_wait_until_enabled();
            continue;
        }

        if (stpbt_fd < 0)
        {
            bt_state = Y1_BT_STATE_INIT;
            rx_len = 0;
            if (!mt6572_bt_hw_open_and_init()) {
                bt_state = Y1_BT_STATE_ERROR;
                bt_set_target_enabled(false);
                continue;
            }
            bt_state = Y1_BT_STATE_IDLE;
        }

        bool receiver_autoconnect = bt_receiver_mode &&
                                    global_settings.bt_receiver_auto_connect;
        if (bt_paired_devices_loaded &&
            !bt_autoconnect_attempted &&
            !req_connect &&
            (!bt_receiver_mode || receiver_autoconnect))
        {
            uint8_t last_used[6];
            bool have_last_used = false;
            pthread_mutex_lock(&bt_mtx);
            for (int i = bt_num_devices - 1; i >= 0; i--) {
                if (bt_devices[i].paired) {
                    memcpy(last_used, bt_devices[i].bdaddr, sizeof(last_used));
                    have_last_used = true;
                    break;
                }
            }
            pthread_mutex_unlock(&bt_mtx);

            bt_autoconnect_attempted = true;
            if (have_last_used) {
                memcpy(req_connect_bdaddr, last_used, sizeof(last_used));
                req_connect = true;
                bt_log("Trying to reconnect the last-used paired device\n");
            }
        }

        /* Handle UI thread requests */
        if (req_disconnect) {
            req_disconnect = false;
            req_connect = false;
            pending_switch_connect = false;
            if (acl_handle != 0xFFFF) {
                uint8_t disc[3] = { (uint8_t)(acl_handle & 0xFF), (uint8_t)(acl_handle >> 8), 0x13 };
                hci_send_cmd(0x0406, disc, 3); /* HCI_Disconnect */
            }
        }

        if (req_scan) {
            req_scan = false;
            resolving_names = false;
            bt_state = Y1_BT_STATE_SCANNING;
            /* GIAC LAP = 0x9E8B33, Inquiry Length = 8 (10.24s), Num_Responses = 0 */
            uint8_t inq[5] = { 0x33, 0x8B, 0x9E, 0x08, 0x00 };
            hci_send_cmd(0x0401, inq, 5);
            bt_log("Started HCI Inquiry scan (10.24s)...\n");
        }

        if (req_connect) {
            req_connect = false;
            if (acl_handle != 0xFFFF &&
                !pending_switch_connect &&
                memcmp(active_bdaddr, req_connect_bdaddr, sizeof(active_bdaddr)) == 0)
                continue;

            if (acl_handle != 0xFFFF) {
                memcpy(pending_switch_bdaddr, req_connect_bdaddr,
                       sizeof(pending_switch_bdaddr));
                if (pending_switch_connect)
                    continue;

                pending_switch_connect = true;
                uint8_t disc[3] = {
                    (uint8_t)(acl_handle & 0xFF),
                    (uint8_t)(acl_handle >> 8),
                    0x13
                };
                if (hci_send_cmd(0x0406, disc, sizeof(disc)) < 0) {
                    pending_switch_connect = false;
                    bt_state = Y1_BT_STATE_ERROR;
                    bt_log("Failed to disconnect before switching Bluetooth device\n");
                } else {
                    bt_state = Y1_BT_STATE_CONNECTING;
                    bt_log("Disconnecting current device before switching Bluetooth device\n");
                }
                continue;
            }

            if (bt_state == Y1_BT_STATE_SCANNING) {
                hci_send_cmd(0x0402, NULL, 0); /* HCI_Inquiry_Cancel */
                usleep(20000);
            }
            resolving_names = false;
            bt_state = Y1_BT_STATE_CONNECTING;
            avdtp_initiator = !bt_receiver_mode;

            uint8_t cc[13];
            memcpy(&cc[0], req_connect_bdaddr, 6);
            cc[6]  = 0x18; /* Packet_Type lo (DM1/DH1/DM3/DH3/DM5/DH5) */
            cc[7]  = 0xCC; /* Packet_Type hi */
            cc[8]  = 0x01; /* Page_Scan_Repetition_Mode R1 */
            cc[9]  = 0x00; /* Reserved */
            cc[10] = 0x00; /* Clock_Offset lo */
            cc[11] = 0x00; /* Clock_Offset hi */
            cc[12] = 0x01; /* Allow_Role_Switch */
            hci_send_cmd(0x0405, cc, 13); /* HCI_Create_Connection */
        }

        /* Drain PCM ringbuffer into A2DP SBC packets when streaming */
        drain_a2dp_pcm_and_transmit();

        if (bt_receiver_mode && cid_avctp_remote != 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint32_t now_ms = (uint32_t)(now.tv_sec * 1000 +
                                         now.tv_nsec / 1000000);
            if (receiver_metadata_request_pending &&
                now_ms - last_receiver_metadata_query_ms >= 750)
                receiver_metadata_request_pending = false;
            if (receiver_play_status_request_pending &&
                now_ms - last_receiver_play_status_query_ms >= 750)
                receiver_play_status_request_pending = false;
            if (!receiver_metadata_request_pending &&
                now_ms - last_receiver_metadata_query_ms >= 250)
                request_receiver_metadata();
            if (!receiver_play_status_request_pending &&
                now_ms - last_receiver_play_status_query_ms >= 250)
                request_receiver_play_status();
        }

        /* Do not throttle receiver links or active media streams: incoming
         * audio does not update the source-side PCM activity timestamp. */
        if (acl_handle != 0xFFFF && !bt_receiver_mode && !a2dp_streaming &&
            !in_sniff_mode && !req_connect && !req_scan && !resolving_names) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint32_t now_ms = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
            if (now_ms - last_pcm_time_ms >= 2000 && pcm_ring_r == pcm_ring_w) {
                in_sniff_mode = true;
                uint8_t sniff_cmd[10];
                sniff_cmd[0] = (uint8_t)(acl_handle & 0xFF);
                sniff_cmd[1] = (uint8_t)(acl_handle >> 8);
                sniff_cmd[2] = 0x20; sniff_cmd[3] = 0x03; /* Max Interval = 800 slots = 500ms */
                sniff_cmd[4] = 0x90; sniff_cmd[5] = 0x01; /* Min Interval = 400 slots = 250ms */
                sniff_cmd[6] = 0x04; sniff_cmd[7] = 0x00; /* Attempt = 4 slots */
                sniff_cmd[8] = 0x01; sniff_cmd[9] = 0x00; /* Timeout = 1 slot */
                hci_send_cmd(0x0803, sniff_cmd, 10);      /* HCI_Sniff_Mode */
                bt_log("Bluetooth idle: entered low-power Sniff Mode\n");
            }
        }

        /* Poll /dev/stpbt for incoming HCI events and ACL packets */
        struct pollfd pfd = { .fd = stpbt_fd, .events = POLLIN };
        int timeout_ms = a2dp_streaming ? 4 : (in_sniff_mode ? 100 : 25);
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr > 0 && (pfd.revents & POLLIN))
        {
            ssize_t r = read(stpbt_fd, &rx_buf[rx_len], sizeof(rx_buf) - rx_len);
            if (r > 0) {
                rx_len += (size_t)r;
                size_t pos = 0;
                while (pos < rx_len) {
                    uint8_t ptype = rx_buf[pos];
                    if (ptype == HCI_PKT_EVT) {
                        if (pos + 3 > rx_len)
                            break;
                        size_t plen = 3 + rx_buf[pos + 2];
                        if (pos + plen > rx_len)
                            break;
                        handle_hci_event(&rx_buf[pos], plen);
                        pos += plen;
                    } else if (ptype == HCI_PKT_ACL) {
                        if (pos + 5 > rx_len)
                            break;
                        size_t dlen = (size_t)rx_buf[pos + 3] | ((size_t)rx_buf[pos + 4] << 8);
                        size_t plen = 5 + dlen;
                        if (pos + plen > rx_len)
                            break;
                        handle_acl_packet(&rx_buf[pos], plen);
                        pos += plen;
                    } else {
                        /* Resync to next valid packet header */
                        pos++;
                    }
                }
                if (pos > 0) {
                    if (pos < rx_len)
                        memmove(&rx_buf[0], &rx_buf[pos], rx_len - pos);
                    rx_len -= pos;
                }
            }
        }
    }
    return NULL;
}

/* ============================================================================
 * Public API for Rockbox Settings, UI Menus, and PCM Sink (pcm-eac.c)
 * ============================================================================ */
void y1_bt_init(void)
{
    pthread_mutex_lock(&bt_mtx);
    if (!bt_thread_started) {
        bt_set_target_enabled(global_settings.bt_enabled);
        bt_thread_started = true;
        pthread_create(&bt_thread, NULL, bt_worker_thread, NULL);
    }
    pthread_mutex_unlock(&bt_mtx);
}

void y1_bt_set_enabled(bool enable)
{
    y1_bt_init();
    bt_set_target_enabled(enable);
}

void y1_bt_set_receiver_mode(bool enable)
{
    y1_bt_init();
    if (enable == bt_receiver_mode)
        return;

    if (enable) {
        receiver_previous_bt_enabled = bt_is_target_enabled();
        bt_autoconnect_attempted = false;
        bt_receiver_mode = true;
        y1_audio_receiver_mode_set(true);
        y1_bt_set_enabled(true);
        if (acl_handle != 0xFFFF)
            y1_bt_disconnect();
    } else {
        bt_receiver_mode = false;
        bt_autoconnect_attempted = false;
        y1_audio_receiver_set_active(false, a2dp_sample_rate());
        y1_audio_receiver_mode_set(false);
        y1_bt_disconnect();
        y1_bt_set_enabled(receiver_previous_bt_enabled);
    }
}

bool y1_bt_is_receiver_mode(void)
{
    return bt_receiver_mode;
}

uint8_t y1_bt_receiver_get_play_status(void)
{
    pthread_mutex_lock(&bt_mtx);
    uint8_t status = receiver_play_status;
    pthread_mutex_unlock(&bt_mtx);
    return bt_receiver_mode ? status : 0;
}

uint32_t y1_bt_receiver_art_generation(void)
{
    pthread_mutex_lock(&bt_mtx);
    uint32_t generation = receiver_art_generation;
    pthread_mutex_unlock(&bt_mtx);
    return generation;
}

void y1_bt_get_receiver_metadata(char *title, size_t title_size,
                                 char *artist, size_t artist_size,
                                 char *album, size_t album_size)
{
    if (!bt_receiver_mode) {
        if (title && title_size > 0)
            title[0] = '\0';
        if (artist && artist_size > 0)
            artist[0] = '\0';
        if (album && album_size > 0)
            album[0] = '\0';
        return;
    }

    pthread_mutex_lock(&bt_mtx);
    copy_receiver_attribute(title, title_size, receiver_title,
                            strlen(receiver_title));
    copy_receiver_attribute(artist, artist_size, receiver_artist,
                            strlen(receiver_artist));
    copy_receiver_attribute(album, album_size, receiver_album,
                            strlen(receiver_album));
    pthread_mutex_unlock(&bt_mtx);
}

bool y1_bt_is_enabled(void)
{
    return bt_is_target_enabled();
}

enum y1_bt_state y1_bt_get_state(void)
{
    return bt_state;
}

const char *y1_bt_get_status_str(void)
{
    static char status_buf[80];
    switch (bt_state) {
        case Y1_BT_STATE_OFF:        return "Off";
        case Y1_BT_STATE_INIT:       return "Initializing...";
        case Y1_BT_STATE_IDLE:       return "Ready (Idle)";
        case Y1_BT_STATE_SCANNING:   return "Scanning...";
        case Y1_BT_STATE_CONNECTING: return "Connecting...";
        case Y1_BT_STATE_CONNECTED:
            snprintf(status_buf, sizeof(status_buf), "Connected: %s",
                     connected_name[0] ? connected_name : "Audio Device");
            return status_buf;
        case Y1_BT_STATE_STREAMING:
            snprintf(status_buf, sizeof(status_buf), "Streaming (%s): %s",
                     sbc_is_xq ? "SBC XQ" : "SBC",
                     connected_name[0] ? connected_name : "Audio Device");
            return status_buf;
        case Y1_BT_STATE_ERROR:      return "Hardware Error";
        default:                     return "Unknown";
    }
}

const char *y1_bt_get_connected_name(void)
{
    return connected_name;
}

int y1_bt_start_scan(void)
{
    if (!bt_is_target_enabled()) {
        y1_bt_set_enabled(true);
    }
    req_scan = true;
    return 0;
}

bool y1_bt_is_scanning(void)
{
    return (bt_state == Y1_BT_STATE_SCANNING) || req_scan || resolving_names;
}

int y1_bt_get_devices(struct y1_bt_device *out_devs, int max_devs)
{
    pthread_mutex_lock(&bt_mtx);
    int n = (bt_num_devices < max_devs) ? bt_num_devices : max_devs;
    if (n > 0 && out_devs)
        memcpy(out_devs, bt_devices, n * sizeof(struct y1_bt_device));
    pthread_mutex_unlock(&bt_mtx);
    return n;
}

int y1_bt_get_paired_devices(struct y1_bt_device *out_devs, int max_devs)
{
    pthread_mutex_lock(&bt_mtx);
    int count = 0;
    for (int i = 0; i < bt_num_devices && count < max_devs; i++) {
        if (bt_devices[i].paired) {
            if (out_devs)
                out_devs[count] = bt_devices[i];
            count++;
        }
    }
    pthread_mutex_unlock(&bt_mtx);
    return count;
}

int y1_bt_get_new_devices(struct y1_bt_device *out_devs, int max_devs)
{
    pthread_mutex_lock(&bt_mtx);
    int count = 0;
    for (int i = 0; i < bt_num_devices && count < max_devs; i++) {
        if (!bt_devices[i].paired) {
            if (out_devs)
                out_devs[count] = bt_devices[i];
            count++;
        }
    }
    pthread_mutex_unlock(&bt_mtx);
    return count;
}

int y1_bt_connect_device(const uint8_t bdaddr[6])
{
    if (!bt_is_target_enabled())
        y1_bt_set_enabled(true);
    memcpy(req_connect_bdaddr, bdaddr, 6);
    req_connect = true;
    return 0;
}

int y1_bt_connect_device_with_pin(const uint8_t bdaddr[6], const char *pin)
{
    if (!bt_is_target_enabled())
        y1_bt_set_enabled(true);

    pthread_mutex_lock(&bt_mtx);
    struct y1_bt_device *d = find_or_create_device(bdaddr);
    if (d && pin) {
        strncpy(d->pin, pin, sizeof(d->pin) - 1);
        d->pin[sizeof(d->pin) - 1] = '\0';
    }
    pthread_mutex_unlock(&bt_mtx);

    memcpy(req_connect_bdaddr, bdaddr, 6);
    req_connect = true;
    return 0;
}

void y1_bt_disconnect(void)
{
    req_disconnect = true;
    y1_bt_audio_disconnect_hook();
}

void y1_bt_forget_device(const uint8_t bdaddr[6])
{
    pthread_mutex_lock(&bt_mtx);
    for (int i = 0; i < bt_num_devices; i++) {
        if (memcmp(bt_devices[i].bdaddr, bdaddr, 6) == 0) {
            if (bt_devices[i].connected) {
                req_disconnect = true;
                y1_bt_audio_disconnect_hook();
            }
            for (int j = i; j + 1 < bt_num_devices; j++)
                bt_devices[j] = bt_devices[j + 1];
            bt_num_devices--;
            save_paired_devices();
            break;
        }
    }
    pthread_mutex_unlock(&bt_mtx);
}

void y1_bt_update_metadata(const char *title, const char *artist,
                       const char *album, uint32_t duration_ms,
                       uint32_t position_ms, uint8_t play_status)
{
    bool playback_changed;
    bool track_changed;
    pthread_mutex_lock(&bt_mtx);
    track_changed = strcmp(avrcp_title, title ? title : "") != 0 ||
                    strcmp(avrcp_artist, artist ? artist : "") != 0 ||
                    strcmp(avrcp_album, album ? album : "") != 0;
    strncpy(avrcp_title, title ? title : "", sizeof(avrcp_title) - 1);
    avrcp_title[sizeof(avrcp_title) - 1] = '\0';
    strncpy(avrcp_artist, artist ? artist : "", sizeof(avrcp_artist) - 1);
    avrcp_artist[sizeof(avrcp_artist) - 1] = '\0';
    strncpy(avrcp_album, album ? album : "", sizeof(avrcp_album) - 1);
    avrcp_album[sizeof(avrcp_album) - 1] = '\0';
    avrcp_duration_ms = duration_ms;
    avrcp_position_ms = position_ms;
    playback_changed = avrcp_play_status != play_status;
    avrcp_play_status = play_status;
    if (track_changed)
        avrcp_track_uid++;
    uint64_t track_uid = avrcp_track_uid;
    pthread_mutex_unlock(&bt_mtx);

    if (track_changed && avrcp_track_notify_pending &&
        cid_avctp_remote != 0)
    {
        uint8_t request[5] = { 0 };
        uint8_t params[9] = { 0x02 };
        request[0] = (uint8_t)((avrcp_track_notify_label & 0x0F) << 4);
        request[4] = avrcp_track_notify_subunit;
        for (int i = 0; i < 8; i++)
            params[1 + i] = (uint8_t)(track_uid >> (56 - i * 8));
        avrcp_track_notify_pending = false;
        send_avrcp_vendor_response(request, 0x0D, 0x31, params,
                                   sizeof(params));
    }

    if (playback_changed && avrcp_play_notify_pending &&
        cid_avctp_remote != 0)
    {
        uint8_t request[5] = { 0 };
        uint8_t params[2] = { 0x01, play_status };
        request[0] = (uint8_t)((avrcp_play_notify_label & 0x0F) << 4);
        request[4] = avrcp_play_notify_subunit;
        avrcp_play_notify_pending = false;
        send_avrcp_vendor_response(request, 0x0D, 0x31, params,
                                   sizeof(params));
    }
}

bool y1_bt_a2dp_is_streaming(void)
{
    return a2dp_streaming && (cid_avdtp_med_remote != 0);
}

static inline int16_t bt_soft_clamp(int16_t sample)
{
    if (sample > 31000) return 31000;
    if (sample < -31000) return -31000;
    return sample;
}

static int16_t bt_resample_linear(int16_t a, int16_t b, uint32_t fraction)
{
    int32_t value = (int32_t)a * (int32_t)(65536 - fraction) +
                    (int32_t)b * (int32_t)fraction;
    return bt_soft_clamp((int16_t)(value / 65536));
}

/* Called from pcm-eac.c worker thread with 16-bit signed stereo PCM */
void y1_bt_a2dp_write_pcm(const void *buf, size_t bytes, unsigned long sampr)
{
    if (!a2dp_streaming || !buf || bytes < 4)
        return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    last_pcm_time_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

    if (in_sniff_mode && acl_handle != 0xFFFF) {
        in_sniff_mode = false;
        uint8_t exit_sniff[2];
        exit_sniff[0] = (uint8_t)(acl_handle & 0xFF);
        exit_sniff[1] = (uint8_t)(acl_handle >> 8);
        hci_send_cmd(0x0804, exit_sniff, 2); /* HCI_Exit_Sniff_Mode */
    }

    const int16_t *src = (const int16_t *)buf;
    size_t in_frames = bytes >> 2;
    size_t w = pcm_ring_w;
    size_t r = pcm_ring_r;

    unsigned long target_rate = a2dp_sample_rate();

    if (sampr == target_rate || sampr == 0) {
        pcm_resample_phase_fp = 0;
        pcm_resample_source_rate = sampr;
        pcm_resample_target_rate = target_rate;
        for (size_t i = 0; i < in_frames; i++) {
            if (((w - r) & (PCM_RING_SAMPLES - 1)) >= PCM_RING_SAMPLES - 2)
                break;
            size_t idx = (w & (PCM_RING_SAMPLES - 1)) * 2;
            pcm_ring[idx + 0] = bt_soft_clamp(src[i * 2 + 0]);
            pcm_ring[idx + 1] = bt_soft_clamp(src[i * 2 + 1]);
            w = (w + 1) & (PCM_RING_SAMPLES - 1);
        }
    } else {
        /* Preserve fractional phase across PCM blocks to avoid sample-rate drift. */
        if (pcm_resample_source_rate != sampr ||
            pcm_resample_target_rate != target_rate) {
            pcm_resample_phase_fp = 0;
            pcm_resample_source_rate = sampr;
            pcm_resample_target_rate = target_rate;
        }
        uint32_t step_fp = (uint32_t)(((uint64_t)sampr << 16) / target_rate);
        uint64_t pos_fp = pcm_resample_phase_fp;
        uint64_t block_end_fp = (uint64_t)in_frames << 16;
        while (pos_fp < block_end_fp) {
            if (((w - r) & (PCM_RING_SAMPLES - 1)) >= PCM_RING_SAMPLES - 2)
                break;
            size_t s_idx = (pos_fp >> 16) * 2;
            size_t next_s_idx = s_idx + 2;
            if (next_s_idx >= in_frames * 2)
                next_s_idx = s_idx;
            uint32_t fraction = (uint32_t)(pos_fp & 0xFFFF);
            size_t d_idx = (w & (PCM_RING_SAMPLES - 1)) * 2;
            pcm_ring[d_idx + 0] = bt_resample_linear(src[s_idx + 0],
                                                     src[next_s_idx + 0],
                                                     fraction);
            pcm_ring[d_idx + 1] = bt_resample_linear(src[s_idx + 1],
                                                     src[next_s_idx + 1],
                                                     fraction);
            w = (w + 1) & (PCM_RING_SAMPLES - 1);
            pos_fp += step_fp;
        }
        pcm_resample_phase_fp = pos_fp >= block_end_fp ?
                                pos_fp - block_end_fp : 0;
    }
    pcm_ring_w = w;
}
