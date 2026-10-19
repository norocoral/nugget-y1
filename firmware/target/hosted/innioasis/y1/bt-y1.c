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
#include "bt-y1.h"
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

static pthread_mutex_t bt_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t bt_thread;
static bool bt_thread_started = false;

static volatile bool bt_target_enabled = false;
static volatile enum y1_bt_state bt_state = Y1_BT_STATE_OFF;
static int stpbt_fd = -1;

static struct y1_bt_device bt_devices[Y1_BT_MAX_DEVICES];
static int bt_num_devices = 0;

/* Pending command requests from UI thread */
static volatile bool req_scan = false;
static volatile bool req_connect = false;
static uint8_t req_connect_bdaddr[6];
static volatile bool req_disconnect = false;

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
static bool avrcp_vol_notify_pending = false;
static uint8_t avrcp_vol_notify_label = 0;

static uint8_t remote_seid = 1;
static uint8_t sbc_cfg_octet0 = 0x21; /* 44.1kHz, Joint Stereo */
static uint8_t sbc_cfg_octet1 = 0x15; /* 16 blocks, 8 subbands, Loudness */
static uint8_t sbc_bitpool    = 53;

/* HCI ACL flow control */
static int acl_max_pkts = 4;
static int acl_in_flight = 0;

/* Remote Name Resolution queue after Inquiry */
static bool resolving_names = false;
static int  resolve_idx = 0;

/* Logging helper */
static void bt_log(const char *msg)
{
    int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0) {
        (void)!write(kfd, "<6>[bt-y1] ", 11);
        (void)!write(kfd, msg, strlen(msg));
        close(kfd);
    }
    int dfd = open("/mnt/sdcard/bt_debug.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (dfd >= 0) {
        (void)!write(dfd, msg, strlen(msg));
        close(dfd);
    }
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
                return &bt_devices[i];
            }
        }
        return NULL;
    }
    struct y1_bt_device *d = &bt_devices[bt_num_devices++];
    memset(d, 0, sizeof(*d));
    memcpy(d->bdaddr, bd, 6);
    format_bdaddr(bd, d->name, sizeof(d->name));
    return d;
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
        acl_in_flight++;
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
    0x35, 0x36, /* Sequence (54 bytes) */
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

static void handle_sdp_pdu(const uint8_t *pdu, uint16_t len)
{
    if (len < 5 || cid_sdp_remote == 0)
        return;

    uint8_t pdu_id = pdu[0];
    uint16_t tid = ((uint16_t)pdu[1] << 8) | pdu[2];
    uint8_t rsp[128];

    if (pdu_id == 0x02) /* SDP_ServiceSearchRequest */
    {
        /* Return 1 matching handle: 0x00010001 */
        rsp[0] = 0x03; /* SDP_ServiceSearchResponse */
        rsp[1] = (uint8_t)(tid >> 8);
        rsp[2] = (uint8_t)(tid & 0xFF);
        rsp[3] = 0x00;
        rsp[4] = 0x09; /* Param length = 9 */
        rsp[5] = 0x00;
        rsp[6] = 0x01; /* TotalServiceRecordCount = 1 */
        rsp[7] = 0x00;
        rsp[8] = 0x01; /* CurrentServiceRecordCount = 1 */
        rsp[9] = 0x00;
        rsp[10] = 0x01;
        rsp[11] = 0x00;
        rsp[12] = 0x01; /* Handle 0x00010001 */
        rsp[13] = 0x00; /* ContinuationState = 0 */
        l2cap_send_pdu(cid_sdp_remote, rsp, 14);
    }
    else if (pdu_id == 0x04) /* SDP_ServiceAttributeRequest */
    {
        uint16_t attr_len = sizeof(sdp_attr_a2dp_source);
        uint16_t param_len = 2 + attr_len + 1;
        rsp[0] = 0x05; /* SDP_ServiceAttributeResponse */
        rsp[1] = (uint8_t)(tid >> 8);
        rsp[2] = (uint8_t)(tid & 0xFF);
        rsp[3] = (uint8_t)(param_len >> 8);
        rsp[4] = (uint8_t)(param_len & 0xFF);
        rsp[5] = (uint8_t)(attr_len >> 8);
        rsp[6] = (uint8_t)(attr_len & 0xFF);
        memcpy(&rsp[7], sdp_attr_a2dp_source, attr_len);
        rsp[7 + attr_len] = 0x00; /* ContinuationState = 0 */
        l2cap_send_pdu(cid_sdp_remote, rsp, 5 + param_len);
    }
    else if (pdu_id == 0x06) /* SDP_ServiceSearchAttributeRequest */
    {
        /* Check if client is searching for A2DP (0x110A / 0x110B / 0x110D / 0x0019 / 0x0100) */
        uint16_t seq_len = 2 + sizeof(sdp_attr_a2dp_source);
        uint16_t param_len = 2 + seq_len + 1;
        rsp[0] = 0x07; /* SDP_ServiceSearchAttributeResponse */
        rsp[1] = (uint8_t)(tid >> 8);
        rsp[2] = (uint8_t)(tid & 0xFF);
        rsp[3] = (uint8_t)(param_len >> 8);
        rsp[4] = (uint8_t)(param_len & 0xFF);
        rsp[5] = (uint8_t)(seq_len >> 8);
        rsp[6] = (uint8_t)(seq_len & 0xFF);
        rsp[7] = 0x35; /* Data Element Sequence */
        rsp[8] = sizeof(sdp_attr_a2dp_source);
        memcpy(&rsp[9], sdp_attr_a2dp_source, sizeof(sdp_attr_a2dp_source));
        rsp[9 + sizeof(sdp_attr_a2dp_source)] = 0x00; /* ContinuationState = 0 */
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
                /* Advertise Local SEID 1: Audio Source (TSEP=0 Source -> bit 3 = 0) */
                uint8_t seps[2] = { (1 << 2), 0x00 }; /* SEID=1, InUse=0, MediaType=Audio(0), TSEP=Source(0) */
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
                        uint8_t max_bp = pdu[i + 7];
                        if (max_bp >= 2 && max_bp < 53)
                            sbc_bitpool = max_bp;
                        else
                            sbc_bitpool = 53;
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
                if (!was_streaming) {
                    y1_bt_audio_connect_hook();
                }
                break;
            }
            case AVDTP_SUSPEND:
                avdtp_send_accept(hdr, sig_id, NULL, 0);
                a2dp_streaming = false;
                bt_state = Y1_BT_STATE_CONNECTED;
                break;
            case AVDTP_CLOSE:
            case AVDTP_ABORT:
                avdtp_send_accept(hdr, sig_id, NULL, 0);
                a2dp_streaming = false;
                cid_avdtp_med_remote = 0;
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
                /* Find first Sink SEP (TSEP bit 3 == 1 -> Audio Sink) that is not in use */
                remote_seid = 1;
                for (int i = 2; i + 1 < len; i += 2) {
                    uint8_t seid = pdu[i] >> 2;
                    bool in_use = (pdu[i] & 0x02) != 0;
                    bool is_sink = (pdu[i + 1] & 0x08) != 0;
                    uint8_t media_type = pdu[i + 1] >> 4;
                    if (!in_use && is_sink && media_type == 0 && seid > 0) {
                        remote_seid = seid;
                        break;
                    }
                }
                uint8_t param[1] = { (uint8_t)(remote_seid << 2) };
                avdtp_send_cmd(AVDTP_GET_CAPABILITIES, param, 1);
                break;
            }
            case AVDTP_GET_CAPABILITIES:
            case AVDTP_GET_ALL_CAPABILITIES: {
                /* Parse remote Sink's SBC capabilities */
                uint8_t sup_o0 = 0x2F, sup_o1 = 0xFF, min_bp = 2, max_bp = 53;
                for (int i = 2; i + 7 < len; i++) {
                    if (pdu[i] == 0x07 && pdu[i + 1] >= 0x06 && pdu[i + 2] == 0x00 && pdu[i + 3] == 0x00) {
                        sup_o0 = pdu[i + 4];
                        sup_o1 = pdu[i + 5];
                        min_bp = pdu[i + 6];
                        max_bp = pdu[i + 7];
                        break;
                    }
                }
                /* Choose 44.1kHz (0x20), else 48kHz (0x10) */
                uint8_t freq = (sup_o0 & 0x20) ? 0x20 : 0x10;
                /* Choose Joint Stereo (0x01), else Stereo (0x02) */
                uint8_t chm  = (sup_o0 & 0x01) ? 0x01 : 0x02;
                sbc_cfg_octet0 = freq | chm;
                /* Choose 16 blocks (0x10), 8 subbands (0x04), Loudness (0x01) or SNR (0x02) */
                sbc_cfg_octet1 = 0x14 | ((sup_o1 & 0x01) ? 0x01 : 0x02);
                sbc_bitpool = (max_bp > 53) ? 53 : ((max_bp < 2) ? 35 : max_bp);

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
    if ((avctp_hdr & 0x02) != 0)
        return; /* Ignore responses, only handle commands (C/R bit = 0) */

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

        if (!released) {
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
    else if (opcode == 0x00 && len >= 10) /* VENDOR DEPENDENT (Metadata & Absolute Volume) */
    {
        uint8_t pdu_id = pdu[9];
        if (pdu_id == 0x50 && len >= 14) /* SetAbsoluteVolume */
        {
            uint8_t abs_vol = pdu[13] & 0x7F;
            y1_bt_on_set_absolute_volume((int)abs_vol);

            /* Send AVRCP Accepted Response */
            uint8_t rsp[14];
            rsp[0] = (uint8_t)(avctp_hdr | 0x02); /* Response bit */
            rsp[1] = 0x11; rsp[2] = 0x0E;
            rsp[3] = 0x09; /* ACCEPTED */
            rsp[4] = pdu[4];
            rsp[5] = 0x00; /* VENDOR DEPENDENT */
            rsp[6] = 0x00; rsp[7] = 0x19; rsp[8] = 0x58; /* BT SIG */
            rsp[9] = 0x50; /* SetAbsoluteVolume */
            rsp[10] = 0x00;
            rsp[11] = 0x00; rsp[12] = 0x01; /* Param len = 1 */
            rsp[13] = abs_vol;
            l2cap_send_pdu(cid_avctp_remote, rsp, 14);
        }
        else if (pdu_id == 0x31 && len >= 14) /* RegisterNotification */
        {
            uint8_t event_id = pdu[13];
            if (event_id == 0x0D) /* EVENT_VOLUME_CHANGED */
            {
                avrcp_vol_notify_label = avctp_hdr >> 4;
                avrcp_vol_notify_pending = true;

                int cur_vol = y1_bt_get_active_device_volume();
                int min_vol = -74;
                int abs_vol = 0;
                if (cur_vol >= 0) abs_vol = 127;
                else if (cur_vol > min_vol) abs_vol = ((cur_vol - min_vol) * 127) / (0 - min_vol);

                /* Send INTERIM Response */
                uint8_t rsp[15];
                rsp[0] = (uint8_t)(avctp_hdr | 0x02);
                rsp[1] = 0x11; rsp[2] = 0x0E;
                rsp[3] = 0x0F; /* INTERIM */
                rsp[4] = pdu[4];
                rsp[5] = 0x00;
                rsp[6] = 0x00; rsp[7] = 0x19; rsp[8] = 0x58;
                rsp[9] = 0x31;
                rsp[10] = 0x00;
                rsp[11] = 0x00; rsp[12] = 0x02;
                rsp[13] = 0x0D;
                rsp[14] = (uint8_t)(abs_vol & 0x7F);
                l2cap_send_pdu(cid_avctp_remote, rsp, 15);
            }
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

                    uint8_t rsp[6] = {
                        (uint8_t)(rcid & 0xFF), (uint8_t)(rcid >> 8),
                        0x00, 0x00, /* Flags */
                        0x00, 0x00  /* Result: Success */
                    };
                    l2cap_send_sig(L2CAP_CONF_RSP, ident, rsp, 6);
                    check_avdtp_channels_ready();
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
                        check_avdtp_channels_ready();
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
                    }
                    if (dcid == CID_AVDTP_SIG_LOCAL || scid == cid_avdtp_sig_remote) {
                        cid_avdtp_sig_remote = 0;
                        a2dp_streaming = false;
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
                    acl_handle = handle & 0x0FFF;
                    acl_in_flight = 0;
                    memcpy(active_bdaddr, bd, 6);
                    cid_sdp_remote = 0;
                    cid_avdtp_sig_remote = 0;
                    cid_avdtp_med_remote = 0;
                    cid_avctp_remote = 0;
                    avdtp_sig_local_conf_done = false;
                    avdtp_sig_remote_conf_done = false;
                    avdtp_med_local_conf_done = false;
                    avdtp_med_remote_conf_done = false;
                    avdtp_discover_sent = false;
                    avdtp_opening_media = false;

                    pthread_mutex_lock(&bt_mtx);
                    for (int i = 0; i < bt_num_devices; i++)
                        bt_devices[i].connected = false;
                    struct y1_bt_device *d = find_or_create_device(bd);
                    if (d) {
                        d->connected = true;
                        strncpy(connected_name, d->name, sizeof(connected_name) - 1);
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
                    if (avdtp_initiator) {
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
            acl_handle = 0xFFFF;
            a2dp_streaming = false;
            in_sniff_mode = false;
            avrcp_vol_notify_pending = false;
            memset(active_bdaddr, 0, 6);
            cid_sdp_remote = 0;
            cid_avdtp_sig_remote = 0;
            cid_avdtp_med_remote = 0;
            cid_avctp_remote = 0;
            pthread_mutex_lock(&bt_mtx);
            for (int i = 0; i < bt_num_devices; i++)
                bt_devices[i].connected = false;
            connected_name[0] = '\0';
            if (bt_target_enabled)
                bt_state = Y1_BT_STATE_IDLE;
            pthread_mutex_unlock(&bt_mtx);
            bt_log("ACL Disconnected\n");
            y1_bt_audio_disconnect_hook();
            break;
        }

        case 0x06: /* Authentication Complete */ {
            if (plen >= 3) {
                uint8_t status = p[0];
                if (status == 0x00 && acl_handle != 0xFFFF) {
                    /* Enable link encryption, and if Initiator, open L2CAP AVDTP signaling */
                    uint8_t enc[3] = { (uint8_t)(acl_handle & 0xFF), (uint8_t)(acl_handle >> 8), 0x01 };
                    hci_send_cmd(0x0413, enc, 3); /* HCI_Set_Connection_Encryption */
                    if (avdtp_initiator && cid_avdtp_sig_remote == 0) {
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

        case 0x13: /* Number Of Completed Packets (HCI ACL Flow Control) */ {
            if (plen >= 5) {
                uint8_t num_handles = p[0];
                for (uint8_t i = 0; i < num_handles && (1 + (i + 1) * 4) <= plen; i++) {
                    uint16_t completed = (uint16_t)p[3 + i * 4] | ((uint16_t)p[4 + i * 4] << 8);
                    acl_in_flight -= completed;
                    if (acl_in_flight < 0)
                        acl_in_flight = 0;
                }
            }
            break;
        }

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
    if (!a2dp_streaming || cid_avdtp_med_remote == 0 || acl_handle == 0xFFFF || !a2dp_sbc_ready)
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

    while (acl_in_flight < acl_max_pkts)
    {
        size_t r = pcm_ring_r;
        size_t w = pcm_ring_w;
        size_t avail = (w - r) & (PCM_RING_SAMPLES - 1);
        if (avail < samples_per_pkt)
            break;

        uint8_t rtp_pkt[896];
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
        rtp_timestamp += (uint32_t)samples_per_pkt;
        rtp_pkt[8]  = 0x00;
        rtp_pkt[9]  = 0x00;
        rtp_pkt[10] = 0x00;
        rtp_pkt[11] = 0x01; /* SSRC = 1 */
        rtp_pkt[12] = (uint8_t)frames_per_pkt; /* Unfragmented SBC payload header */

        size_t pkt_len = 13;
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
            }
        }
        pcm_ring_r = r;

        if (l2cap_send_pdu(cid_avdtp_med_remote, rtp_pkt, (uint16_t)pkt_len) < 0)
            break;
    }
}

static void *bt_worker_thread(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&bt_mtx);
    load_paired_devices();
    pthread_mutex_unlock(&bt_mtx);

    uint8_t rx_buf[2048];
    size_t rx_len = 0;

    while (1)
    {
        if (!bt_target_enabled)
        {
            if (stpbt_fd >= 0) {
                if (acl_handle != 0xFFFF) {
                    uint8_t disc[3] = { (uint8_t)(acl_handle & 0xFF), (uint8_t)(acl_handle >> 8), 0x13 };
                    hci_send_cmd(0x0406, disc, 3);
                    usleep(50000);
                }
                close(stpbt_fd);
                stpbt_fd = -1;
                acl_handle = 0xFFFF;
                a2dp_streaming = false;
                bt_state = Y1_BT_STATE_OFF;
                bt_log("Closed /dev/stpbt (MT6572 BT Radio powered down)\n");
            }
            usleep(100000);
            continue;
        }

        if (stpbt_fd < 0)
        {
            bt_state = Y1_BT_STATE_INIT;
            rx_len = 0;
            if (!mt6572_bt_hw_open_and_init()) {
                bt_state = Y1_BT_STATE_ERROR;
                bt_target_enabled = false;
                continue;
            }
            bt_state = Y1_BT_STATE_IDLE;
        }

        /* Handle UI thread requests */
        if (req_disconnect) {
            req_disconnect = false;
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
            if (bt_state == Y1_BT_STATE_SCANNING) {
                hci_send_cmd(0x0402, NULL, 0); /* HCI_Inquiry_Cancel */
                usleep(20000);
            }
            resolving_names = false;
            bt_state = Y1_BT_STATE_CONNECTING;
            avdtp_initiator = true;

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

        /* Low-Power Sniff Mode optimization: enter sniff mode if idle for >= 2.0s */
        if (acl_handle != 0xFFFF && !in_sniff_mode && !req_connect && !req_scan && !resolving_names) {
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
        bt_thread_started = true;
        pthread_create(&bt_thread, NULL, bt_worker_thread, NULL);
    }
    pthread_mutex_unlock(&bt_mtx);
}

void y1_bt_set_enabled(bool enable)
{
    y1_bt_init();
    bt_target_enabled = enable;
}

bool y1_bt_is_enabled(void)
{
    return bt_target_enabled;
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
            snprintf(status_buf, sizeof(status_buf), "Streaming: %s",
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
    if (!bt_target_enabled) {
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
    if (!bt_target_enabled)
        y1_bt_set_enabled(true);
    memcpy(req_connect_bdaddr, bdaddr, 6);
    req_connect = true;
    return 0;
}

int y1_bt_connect_device_with_pin(const uint8_t bdaddr[6], const char *pin)
{
    if (!bt_target_enabled)
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

void y1_bt_sync_volume_changed(int vol)
{
    if (vol > 0) vol = 0;
    pthread_mutex_lock(&bt_mtx);
    for (int i = 0; i < bt_num_devices; i++) {
        if (memcmp(bt_devices[i].bdaddr, active_bdaddr, 6) == 0) {
            bt_devices[i].volume = vol;
            save_paired_devices();
            break;
        }
    }
    pthread_mutex_unlock(&bt_mtx);

    if (avrcp_vol_notify_pending && cid_avctp_remote != 0) {
        avrcp_vol_notify_pending = false;
        int min_vol = -74;
        int abs_vol = 0;
        if (vol >= 0) abs_vol = 127;
        else if (vol > min_vol) abs_vol = ((vol - min_vol) * 127) / (0 - min_vol);

        uint8_t rsp[15];
        rsp[0] = (uint8_t)((avrcp_vol_notify_label << 4) | 0x02);
        rsp[1] = 0x11; rsp[2] = 0x0E;
        rsp[3] = 0x09; /* CHANGED */
        rsp[4] = 0x48;
        rsp[5] = 0x00;
        rsp[6] = 0x00; rsp[7] = 0x19; rsp[8] = 0x58;
        rsp[9] = 0x31;
        rsp[10] = 0x00;
        rsp[11] = 0x00; rsp[12] = 0x02;
        rsp[13] = 0x0D;
        rsp[14] = (uint8_t)(abs_vol & 0x7F);
        l2cap_send_pdu(cid_avctp_remote, rsp, 15);
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

    /* Negotiated SBC rate is 44100 Hz (or 48000 Hz if 0x10 bit set) */
    unsigned long target_rate = (sbc_cfg_octet0 & 0x10) ? 48000 : 44100;

    if (sampr == target_rate || sampr == 0) {
        for (size_t i = 0; i < in_frames; i++) {
            if (((w - r) & (PCM_RING_SAMPLES - 1)) >= PCM_RING_SAMPLES - 2)
                break;
            size_t idx = (w & (PCM_RING_SAMPLES - 1)) * 2;
            pcm_ring[idx + 0] = bt_soft_clamp(src[i * 2 + 0]);
            pcm_ring[idx + 1] = bt_soft_clamp(src[i * 2 + 1]);
            w = (w + 1) & (PCM_RING_SAMPLES - 1);
        }
    } else {
        /* Nearest/linear rate conversion from sampr -> target_rate */
        uint32_t step_fp = (uint32_t)(((uint64_t)sampr << 16) / target_rate);
        uint32_t pos_fp = 0;
        while ((pos_fp >> 16) < in_frames) {
            if (((w - r) & (PCM_RING_SAMPLES - 1)) >= PCM_RING_SAMPLES - 2)
                break;
            size_t s_idx = (pos_fp >> 16) * 2;
            size_t d_idx = (w & (PCM_RING_SAMPLES - 1)) * 2;
            pcm_ring[d_idx + 0] = bt_soft_clamp(src[s_idx + 0]);
            pcm_ring[d_idx + 1] = bt_soft_clamp(src[s_idx + 1]);
            w = (w + 1) & (PCM_RING_SAMPLES - 1);
            pos_fp += step_fp;
        }
    }
    pcm_ring_w = w;
}

