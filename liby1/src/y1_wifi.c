/***************************************************************************
 * liby1 - MediaTek MT6572 CONNSYS Wi-Fi Driver (/dev/wmtWifi & wlan0)
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/wireless.h>

#ifndef makedev
#define makedev(maj, min) (((maj) << 8) | (min))
#endif

#include "liby1.h"

#define WIFI_IFNAME "wlan0"
#define WIFI_WPA_CONF "/tmp/y1_wpa.conf"

static pthread_mutex_t wifi_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wifi_cond = PTHREAD_COND_INITIALIZER;
static pthread_t       wifi_thread;
static bool            wifi_thread_started = false;

static volatile bool wifi_target_enabled = false;
static volatile enum y1_wifi_state wifi_state = Y1_WIFI_STATE_OFF;
static volatile bool wifi_req_scan = false;
static volatile bool wifi_req_connect = false;
static volatile bool wifi_req_disconnect = false;

static char req_ssid[64] = "";
static char req_psk[128] = "";
static char connected_ssid[64] = "";
static char connected_ip[32] = "";
static int  connected_rssi = 0;
static char wifi_last_err[64] = "";

static struct y1_wifi_network g_nets[Y1_WIFI_MAX_NETWORKS];
static int g_net_count = 0;
static bool g_db_loaded = false;

static int wifi_load_ko_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }
    void *img = malloc((size_t)st.st_size);
    if (!img) {
        close(fd);
        return -1;
    }
    ssize_t rb = read(fd, img, (size_t)st.st_size);
    close(fd);
    if (rb != st.st_size) {
        free(img);
        return -1;
    }
    long ret = syscall(__NR_init_module, img, (unsigned long)st.st_size, "");
    free(img);
    return (ret == 0 || errno == EEXIST) ? 0 : -1;
}

extern void y1_bt_ensure_wmt_ready(void);

static int find_proc_device_major(const char *name)
{
    FILE *f = fopen("/proc/devices", "r");
    if (!f) return -1;
    char line[128];
    int maj = -1;
    while (fgets(line, sizeof(line), f)) {
        int m = 0;
        char dname[64] = "";
        if (sscanf(line, " %d %63s", &m, dname) == 2 && strcmp(dname, name) == 0) {
            maj = m;
            break;
        }
    }
    fclose(f);
    return maj;
}

static void ensure_wifi_firmware_links(void)
{
    const char *fw_files[] = { "WIFI_RAM_CODE_MT6582", "WIFI_RAM_CODE", NULL };
    const char *dirs[] = { "/mnt/sdcard/.system/bt", "/.system/bt", "/mnt/sdcard/.rockbox/bt", NULL };
    for (int i = 0; fw_files[i]; i++) {
        char dst[128];
        snprintf(dst, sizeof(dst), "/etc/firmware/%s", fw_files[i]);
        if (access(dst, F_OK) == 0)
            continue;
        for (int d = 0; dirs[d]; d++) {
            char src[128];
            snprintf(src, sizeof(src), "%s/%s", dirs[d], fw_files[i]);
            if (access(src, F_OK) == 0) {
                unlink(dst);
                (void)!symlink(src, dst);
                break;
            }
        }
    }
}

static void ensure_wmt_wifi_node(void)
{
    /* Ensure MT6572 CONNSYS WMT driver + firmware patch daemon are running */
    y1_bt_ensure_wmt_ready();
    ensure_wifi_firmware_links();
    usleep(150000);

    /* Load WLAN NIC driver (wlan_mt6582.ko) so mtk_wcn_wlan_probe is registered with WMT */
    if (access("/sys/module/wlan_mt6582", F_OK) != 0 && access("/sys/module/wlan", F_OK) != 0) {
        const char *wlan_paths[] = {
            "/lib/modules/wlan_mt6582.ko",
            "/mnt/sdcard/.system/bt/wlan_mt6582.ko",
            "/mnt/sdcard/.rockbox/bt/wlan_mt6582.ko",
            NULL
        };
        for (int i = 0; wlan_paths[i]; i++) {
            if (access(wlan_paths[i], F_OK) != 0)
                continue;
            if (wifi_load_ko_file(wlan_paths[i]) == 0)
                break;
        }
    }

    int maj = find_proc_device_major("mtk_wmt_WIFI_chrdev");
    if (maj < 0) {
        const char *wifi_paths[] = {
            "/lib/modules/mtk_wmt_wifi.ko",
            "/mnt/sdcard/.system/bt/mtk_wmt_wifi.ko",
            "/mnt/sdcard/.rockbox/bt/mtk_wmt_wifi.ko",
            NULL
        };
        int last_err = ENOENT;
        for (int i = 0; wifi_paths[i]; i++) {
            if (access(wifi_paths[i], F_OK) != 0)
                continue;
            if (wifi_load_ko_file(wifi_paths[i]) == 0) {
                last_err = 0;
                break;
            }
            last_err = errno;
        }
        if (last_err != 0) {
            snprintf(wifi_last_err, sizeof(wifi_last_err), "insmod wmt_wifi: %s", strerror(last_err));
        }
        maj = find_proc_device_major("mtk_wmt_WIFI_chrdev");
    }

    if (maj > 0) {
        unlink("/dev/wmtWifi");
        mknod("/dev/wmtWifi", S_IFCHR | 0666, makedev(maj, 0));
    } else if (access("/dev/wmtWifi", F_OK) != 0) {
        mknod("/dev/wmtWifi", S_IFCHR | 0666, makedev(153, 0));
    }
}

static bool write_wmt_wifi(const char *val)
{
    int fd = open("/dev/wmtWifi", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(wifi_last_err, sizeof(wifi_last_err), "open wmtWifi: %s", strerror(errno));
        return false;
    }
    ssize_t w = write(fd, val, strlen(val));
    if (w <= 0) {
        snprintf(wifi_last_err, sizeof(wifi_last_err), "write wmtWifi: %s",
                 w < 0 ? strerror(errno) : "0 bytes");
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

static bool set_iface_up(bool up)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", WIFI_IFNAME);
    bool ok = false;
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
        if (up)
            ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
        else
            ifr.ifr_flags &= ~IFF_UP;
        if (ioctl(sock, SIOCSIFFLAGS, &ifr) == 0)
            ok = true;
    }
    close(sock);
    return ok;
}

static void save_wifi_db_locked(void)
{
    mkdir(Y1_SYSTEM_DIR, 0777);
    FILE *f = fopen(Y1_WIFI_DB_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_net_count; i++) {
        if (g_nets[i].saved && g_nets[i].ssid[0]) {
            fprintf(f, "%s\t%d\t%s\n",
                    g_nets[i].ssid,
                    g_nets[i].secured ? 1 : 0,
                    g_nets[i].psk);
        }
    }
    fclose(f);
}

static void load_wifi_db_locked(void)
{
    if (g_db_loaded)
        return;
    g_db_loaded = true;
    FILE *f = fopen(Y1_WIFI_DB_PATH, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f) && g_net_count < Y1_WIFI_MAX_NETWORKS) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        char *tab2 = strchr(tab1 + 1, '\t');
        int sec = atoi(tab1 + 1);
        const char *psk = tab2 ? (tab2 + 1) : "";

        struct y1_wifi_network *n = &g_nets[g_net_count++];
        memset(n, 0, sizeof(*n));
        snprintf(n->ssid, sizeof(n->ssid), "%s", line);
        n->secured = (sec != 0);
        n->saved = true;
        snprintf(n->psk, sizeof(n->psk), "%s", psk);
    }
    fclose(f);
}

static struct y1_wifi_network *find_or_add_net_locked(const char *ssid)
{
    if (!ssid || !ssid[0])
        return NULL;
    for (int i = 0; i < g_net_count; i++) {
        if (strcmp(g_nets[i].ssid, ssid) == 0)
            return &g_nets[i];
    }
    if (g_net_count >= Y1_WIFI_MAX_NETWORKS)
        return NULL;
    struct y1_wifi_network *n = &g_nets[g_net_count++];
    memset(n, 0, sizeof(*n));
    snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
    return n;
}

static void poll_iface_telemetry(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", WIFI_IFNAME);
    char ip_buf[32] = "";
    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf));
    }

    char essid[64] = "";
    struct iwreq wrq;
    memset(&wrq, 0, sizeof(wrq));
    snprintf(wrq.ifr_name, sizeof(wrq.ifr_name), "%s", WIFI_IFNAME);
    wrq.u.essid.pointer = essid;
    wrq.u.essid.length = sizeof(essid) - 1;
    wrq.u.essid.flags = 0;
    if (ioctl(sock, SIOCGIWESSID, &wrq) == 0 && wrq.u.essid.length > 0) {
        size_t elen = wrq.u.essid.length < sizeof(essid) - 1 ? wrq.u.essid.length : sizeof(essid) - 1;
        essid[elen] = '\0';
    } else {
        essid[0] = '\0';
    }

    struct iw_statistics stats;
    memset(&stats, 0, sizeof(stats));
    memset(&wrq, 0, sizeof(wrq));
    snprintf(wrq.ifr_name, sizeof(wrq.ifr_name), "%s", WIFI_IFNAME);
    wrq.u.data.pointer = &stats;
    wrq.u.data.length = sizeof(stats);
    wrq.u.data.flags = 1;
    int rssi = 0;
    if (ioctl(sock, SIOCGIWSTATS, &wrq) == 0) {
        rssi = (int)(int8_t)stats.qual.level;
    }
    close(sock);

    pthread_mutex_lock(&wifi_mtx);
    snprintf(connected_ip, sizeof(connected_ip), "%s", ip_buf);
    snprintf(connected_ssid, sizeof(connected_ssid), "%s", essid);
    connected_rssi = rssi;
    if (wifi_target_enabled && wifi_state != Y1_WIFI_STATE_SCANNING && wifi_state != Y1_WIFI_STATE_CONNECTING) {
        if (essid[0] && ip_buf[0])
            wifi_state = Y1_WIFI_STATE_CONNECTED;
        else if (wifi_state == Y1_WIFI_STATE_CONNECTED && !essid[0])
            wifi_state = Y1_WIFI_STATE_DISCONNECTED;
    }
    for (int i = 0; i < g_net_count; i++) {
        g_nets[i].connected = (essid[0] && strcmp(g_nets[i].ssid, essid) == 0);
    }
    pthread_mutex_unlock(&wifi_mtx);
}

static void perform_wext_scan(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct iwreq wrq;
    memset(&wrq, 0, sizeof(wrq));
    snprintf(wrq.ifr_name, sizeof(wrq.ifr_name), "%s", WIFI_IFNAME);
    if (ioctl(sock, SIOCSIWSCAN, &wrq) < 0 && errno != EBUSY) {
        close(sock);
        return;
    }

    size_t buf_sz = 16384;
    uint8_t *buf = malloc(buf_sz);
    if (!buf) {
        close(sock);
        return;
    }

    int got_len = -1;
    for (int retry = 0; retry < 40; retry++) {
        usleep(150000);
        memset(&wrq, 0, sizeof(wrq));
        snprintf(wrq.ifr_name, sizeof(wrq.ifr_name), "%s", WIFI_IFNAME);
        wrq.u.data.pointer = buf;
        wrq.u.data.length = (uint16_t)buf_sz;
        wrq.u.data.flags = 0;
        if (ioctl(sock, SIOCGIWSCAN, &wrq) == 0) {
            got_len = wrq.u.data.length;
            break;
        }
        if (errno == E2BIG && buf_sz < 65535) {
            buf_sz = 65535;
            uint8_t *nb = realloc(buf, buf_sz);
            if (!nb) break;
            buf = nb;
        } else if (errno != EAGAIN) {
            break;
        }
    }
    close(sock);

    if (got_len > 0) {
        pthread_mutex_lock(&wifi_mtx);
        load_wifi_db_locked();

        uint8_t cur_bssid[6] = {0};
        char cur_ssid[64] = "";
        int cur_rssi = -80;
        bool cur_sec = false;
        bool have_entry = false;

        size_t pos = 0;
        while (pos + IW_EV_LCP_LEN <= (size_t)got_len) {
            uint16_t ev_len = 0, ev_cmd = 0;
            memcpy(&ev_len, buf + pos, 2);
            memcpy(&ev_cmd, buf + pos + 2, 2);
            if (ev_len < IW_EV_LCP_LEN || pos + ev_len > (size_t)got_len)
                break;

            if (ev_cmd == SIOCGIWAP && ev_len >= IW_EV_LCP_LEN + sizeof(struct sockaddr)) {
                if (have_entry && cur_ssid[0]) {
                    struct y1_wifi_network *n = find_or_add_net_locked(cur_ssid);
                    if (n) {
                        if (n->rssi == 0 || cur_rssi > n->rssi) {
                            memcpy(n->bssid, cur_bssid, 6);
                            n->rssi = cur_rssi;
                        }
                        if (cur_sec) n->secured = true;
                    }
                }
                struct sockaddr sa;
                memcpy(&sa, buf + pos + IW_EV_LCP_LEN, sizeof(sa));
                memcpy(cur_bssid, sa.sa_data, 6);
                cur_ssid[0] = '\0';
                cur_rssi = -80;
                cur_sec = false;
                have_entry = true;
            } else if (ev_cmd == SIOCGIWESSID && ev_len >= IW_EV_POINT_LEN) {
                /* Kernel WEXT stream strips pointer: length & flags sit at +IW_EV_LCP_LEN */
                uint16_t slen = 0;
                memcpy(&slen, buf + pos + IW_EV_LCP_LEN, 2);
                const char *sptr = (const char *)(buf + pos + IW_EV_POINT_LEN);
                size_t avail = (size_t)ev_len - IW_EV_POINT_LEN;
                if (slen > avail) slen = (uint16_t)avail;
                if (slen > sizeof(cur_ssid) - 1) slen = sizeof(cur_ssid) - 1;
                memcpy(cur_ssid, sptr, slen);
                cur_ssid[slen] = '\0';
            } else if (ev_cmd == IWEVQUAL && ev_len >= IW_EV_LCP_LEN + sizeof(struct iw_quality)) {
                struct iw_quality q;
                memcpy(&q, buf + pos + IW_EV_LCP_LEN, sizeof(q));
                cur_rssi = (int)(int8_t)q.level;
            } else if (ev_cmd == SIOCGIWENCODE && ev_len >= IW_EV_LCP_LEN + 4) {
                uint16_t eflags = 0;
                memcpy(&eflags, buf + pos + IW_EV_LCP_LEN + 2, 2);
                if (!(eflags & IW_ENCODE_DISABLED))
                    cur_sec = true;
            } else if (ev_cmd == IWEVGENIE && ev_len > IW_EV_POINT_LEN) {
                uint16_t ielen = 0;
                memcpy(&ielen, buf + pos + IW_EV_LCP_LEN, 2);
                const uint8_t *ie = buf + pos + IW_EV_POINT_LEN;
                size_t avail = (size_t)ev_len - IW_EV_POINT_LEN;
                if (ielen > avail) ielen = (uint16_t)avail;
                size_t ipos = 0;
                while (ipos + 2 <= ielen) {
                    uint8_t tag = ie[ipos];
                    uint8_t tlen = ie[ipos + 1];
                    if (tag == 0x30 || (tag == 0xDD && tlen >= 4 && ie[ipos+2] == 0x00 && ie[ipos+3] == 0x50 && ie[ipos+4] == 0xF2 && ie[ipos+5] == 0x01)) {
                        cur_sec = true;
                    }
                    ipos += 2 + tlen;
                }
            }
            pos += ev_len;
        }
        if (have_entry && cur_ssid[0]) {
            struct y1_wifi_network *n = find_or_add_net_locked(cur_ssid);
            if (n) {
                if (n->rssi == 0 || cur_rssi > n->rssi) {
                    memcpy(n->bssid, cur_bssid, 6);
                    n->rssi = cur_rssi;
                }
                if (cur_sec) n->secured = true;
            }
        }
        pthread_mutex_unlock(&wifi_mtx);
    }
    free(buf);
}

static const char *find_wpa_supplicant(void)
{
    const char *cands[] = {
        "/mnt/sdcard/.system/bin/wpa_supplicant",
        "/.system/bin/wpa_supplicant",
        "/bin/wpa_supplicant",
        "/sbin/wpa_supplicant",
        "/usr/sbin/wpa_supplicant",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        if (access(cands[i], X_OK) == 0)
            return cands[i];
    }
    return NULL;
}

static void ensure_udhcpc_script(void)
{
    const char *path = "/tmp/y1_udhcpc.sh";
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "#!/bin/sh\n"
        "case \"$1\" in\n"
        "  deconfig)\n"
        "    /bin/busybox ifconfig \"$interface\" 0.0.0.0 2>/dev/null\n"
        "    ;;\n"
        "  bound|renew)\n"
        "    /bin/busybox ifconfig \"$interface\" \"$ip\" ${subnet:+netmask \"$subnet\"} up\n"
        "    while /bin/busybox route del default gw 0.0.0.0 dev \"$interface\" 2>/dev/null; do :; done\n"
        "    for r in $router; do\n"
        "      /bin/busybox route add default gw \"$r\" dev \"$interface\" 2>/dev/null\n"
        "    done\n"
        "    : > /etc/resolv.conf\n"
        "    for d in $dns; do\n"
        "      echo \"nameserver $d\" >> /etc/resolv.conf\n"
        "    done\n"
        "    ;;\n"
        "esac\n");
    fclose(f);
    chmod(path, 0755);
}

static void perform_connect(const char *ssid, const char *psk)
{
    (void)!system("/bin/busybox killall wpa_supplicant udhcpc 2>/dev/null");
    ensure_udhcpc_script();

    const char *wpa = find_wpa_supplicant();
    if (wpa) {
        mkdir("/tmp/wpa_ctrl", 0777);
        FILE *f = fopen(WIFI_WPA_CONF, "w");
        if (f) {
            fprintf(f, "ctrl_interface=/tmp/wpa_ctrl\nap_scan=1\nnetwork={\n");
            fprintf(f, "    ssid=\"%s\"\n", ssid);
            fprintf(f, "    scan_ssid=1\n");
            if (psk && psk[0]) {
                fprintf(f, "    psk=\"%s\"\n", psk);
            } else {
                fprintf(f, "    key_mgmt=NONE\n");
            }
            fprintf(f, "}\n");
            fclose(f);
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s -B -Dwext -i%s -c%s >/dev/null 2>&1",
                 wpa, WIFI_IFNAME, WIFI_WPA_CONF);
        if (system(cmd) != 0) {
            snprintf(cmd, sizeof(cmd), "%s -B -Dnl80211 -i%s -c%s >/dev/null 2>&1",
                     wpa, WIFI_IFNAME, WIFI_WPA_CONF);
            (void)!system(cmd);
        }
        /* Wait for WEXT association before running DHCP */
        for (int w = 0; w < 20; w++) {
            usleep(500000);
            poll_iface_telemetry();
            if (connected_ssid[0])
                break;
        }
    } else {
        /* Fallback WEXT open/WEP association when wpa_supplicant is absent */
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct iwreq wrq;
            memset(&wrq, 0, sizeof(wrq));
            snprintf(wrq.ifr_name, sizeof(wrq.ifr_name), "%s", WIFI_IFNAME);
            wrq.u.essid.pointer = (void *)ssid;
            wrq.u.essid.length = (uint16_t)strlen(ssid);
            wrq.u.essid.flags = 1;
            ioctl(sock, SIOCSIWESSID, &wrq);
            close(sock);
        }
    }

    char dhcp_cmd[160];
    snprintf(dhcp_cmd, sizeof(dhcp_cmd),
             "/bin/busybox udhcpc -i %s -s /tmp/y1_udhcpc.sh -n -q -t 8 >/dev/null 2>&1",
             WIFI_IFNAME);
    (void)!system(dhcp_cmd);
    poll_iface_telemetry();
}

static void *wifi_worker_thread(void *arg)
{
    (void)arg;
    bool hw_on = false;

    while (1) {
        pthread_mutex_lock(&wifi_mtx);
        while (!wifi_target_enabled && !hw_on) {
            if (wifi_state != Y1_WIFI_STATE_ERROR)
                wifi_state = Y1_WIFI_STATE_OFF;
            pthread_cond_wait(&wifi_cond, &wifi_mtx);
        }
        bool want_on = wifi_target_enabled;
        bool do_scan = wifi_req_scan;
        bool do_conn = wifi_req_connect;
        bool do_disc = wifi_req_disconnect;
        char c_ssid[64], c_psk[128];
        snprintf(c_ssid, sizeof(c_ssid), "%s", req_ssid);
        snprintf(c_psk, sizeof(c_psk), "%s", req_psk);
        wifi_req_scan = false;
        wifi_req_connect = false;
        wifi_req_disconnect = false;
        pthread_mutex_unlock(&wifi_mtx);

        if (!want_on && hw_on) {
            (void)!system("killall wpa_supplicant udhcpc 2>/dev/null");
            set_iface_up(false);
            write_wmt_wifi("0");
            hw_on = false;
            pthread_mutex_lock(&wifi_mtx);
            connected_ssid[0] = '\0';
            connected_ip[0] = '\0';
            connected_rssi = 0;
            wifi_state = Y1_WIFI_STATE_OFF;
            pthread_mutex_unlock(&wifi_mtx);
            continue;
        }

        if (want_on && !hw_on) {
            pthread_mutex_lock(&wifi_mtx);
            wifi_state = Y1_WIFI_STATE_INIT;
            load_wifi_db_locked();
            pthread_mutex_unlock(&wifi_mtx);

            ensure_wmt_wifi_node();
            bool wmt_ok = false;
            for (int retry = 0; retry < 5; retry++) {
                if (write_wmt_wifi("1")) {
                    wmt_ok = true;
                    break;
                }
                usleep(250000);
            }
            if (!wmt_ok) {
                pthread_mutex_lock(&wifi_mtx);
                wifi_state = Y1_WIFI_STATE_ERROR;
                wifi_target_enabled = false;
                pthread_mutex_unlock(&wifi_mtx);
                continue;
            }
            for (int w = 0; w < 20; w++) {
                if (access("/sys/class/net/wlan0", F_OK) == 0)
                    break;
                usleep(100000);
            }
            set_iface_up(true);
            usleep(600000);
            hw_on = true;
            pthread_mutex_lock(&wifi_mtx);
            wifi_state = Y1_WIFI_STATE_DISCONNECTED;
            pthread_mutex_unlock(&wifi_mtx);
            do_scan = true;
        }

        if (do_disc) {
            (void)!system("killall wpa_supplicant udhcpc 2>/dev/null");
            set_iface_up(false);
            usleep(50000);
            set_iface_up(true);
            pthread_mutex_lock(&wifi_mtx);
            connected_ssid[0] = '\0';
            connected_ip[0] = '\0';
            wifi_state = Y1_WIFI_STATE_DISCONNECTED;
            pthread_mutex_unlock(&wifi_mtx);
        }

        if (do_scan) {
            pthread_mutex_lock(&wifi_mtx);
            wifi_state = Y1_WIFI_STATE_SCANNING;
            pthread_mutex_unlock(&wifi_mtx);
            perform_wext_scan();
            pthread_mutex_lock(&wifi_mtx);
            wifi_state = connected_ip[0] ? Y1_WIFI_STATE_CONNECTED : Y1_WIFI_STATE_DISCONNECTED;
            pthread_mutex_unlock(&wifi_mtx);
        }

        if (do_conn && c_ssid[0]) {
            pthread_mutex_lock(&wifi_mtx);
            wifi_state = Y1_WIFI_STATE_CONNECTING;
            pthread_mutex_unlock(&wifi_mtx);
            perform_connect(c_ssid, c_psk);
            pthread_mutex_lock(&wifi_mtx);
            if (!connected_ip[0])
                wifi_state = Y1_WIFI_STATE_DISCONNECTED;
            pthread_mutex_unlock(&wifi_mtx);
        }

        poll_iface_telemetry();

        pthread_mutex_lock(&wifi_mtx);
        if (wifi_target_enabled && !wifi_req_scan && !wifi_req_connect && !wifi_req_disconnect) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2;
            pthread_cond_timedwait(&wifi_cond, &wifi_mtx, &ts);
        }
        pthread_mutex_unlock(&wifi_mtx);
    }
    return NULL;
}

void y1_wifi_init(void)
{
    pthread_mutex_lock(&wifi_mtx);
    load_wifi_db_locked();
    if (!wifi_thread_started) {
        wifi_thread_started = true;
        pthread_create(&wifi_thread, NULL, wifi_worker_thread, NULL);
    }
    pthread_mutex_unlock(&wifi_mtx);
}

void y1_wifi_set_enabled(bool enable)
{
    y1_wifi_init();
    pthread_mutex_lock(&wifi_mtx);
    wifi_target_enabled = enable;
    pthread_cond_broadcast(&wifi_cond);
    pthread_mutex_unlock(&wifi_mtx);
}

bool y1_wifi_is_enabled(void)
{
    return wifi_target_enabled;
}

enum y1_wifi_state y1_wifi_get_state(void)
{
    return wifi_state;
}

const char *y1_wifi_get_status_str(void)
{
    static char sbuf[96];
    switch (wifi_state) {
        case Y1_WIFI_STATE_OFF:          return "Off";
        case Y1_WIFI_STATE_INIT:         return "Initializing...";
        case Y1_WIFI_STATE_DISCONNECTED: return "Disconnected";
        case Y1_WIFI_STATE_SCANNING:     return "Scanning...";
        case Y1_WIFI_STATE_CONNECTING:   return "Connecting...";
        case Y1_WIFI_STATE_CONNECTED:
            snprintf(sbuf, sizeof(sbuf), "%s (%s)",
                     connected_ssid[0] ? connected_ssid : "Connected",
                     connected_ip[0] ? connected_ip : "No IP");
            return sbuf;
        case Y1_WIFI_STATE_ERROR:
            if (wifi_last_err[0]) {
                snprintf(sbuf, sizeof(sbuf), "Err: %s", wifi_last_err);
                return sbuf;
            }
            return "Hardware Error";
        default:                         return "Unknown";
    }
}

void y1_wifi_get_connected_ssid(char *buf, size_t size)
{
    if (!buf || size == 0) return;
    pthread_mutex_lock(&wifi_mtx);
    snprintf(buf, size, "%s", connected_ssid);
    pthread_mutex_unlock(&wifi_mtx);
}

void y1_wifi_get_ip(char *buf, size_t size)
{
    if (!buf || size == 0) return;
    pthread_mutex_lock(&wifi_mtx);
    snprintf(buf, size, "%s", connected_ip[0] ? connected_ip : "0.0.0.0");
    pthread_mutex_unlock(&wifi_mtx);
}

int y1_wifi_get_rssi(void)
{
    return connected_rssi;
}

int y1_wifi_start_scan(void)
{
    if (!wifi_target_enabled)
        y1_wifi_set_enabled(true);
    pthread_mutex_lock(&wifi_mtx);
    wifi_req_scan = true;
    pthread_cond_broadcast(&wifi_cond);
    pthread_mutex_unlock(&wifi_mtx);
    return 0;
}

bool y1_wifi_is_scanning(void)
{
    return wifi_state == Y1_WIFI_STATE_SCANNING || wifi_req_scan;
}

int y1_wifi_get_networks(struct y1_wifi_network *out_nets, int max_nets)
{
    pthread_mutex_lock(&wifi_mtx);
    load_wifi_db_locked();
    int n = g_net_count < max_nets ? g_net_count : max_nets;
    if (n > 0 && out_nets)
        memcpy(out_nets, g_nets, (size_t)n * sizeof(struct y1_wifi_network));
    pthread_mutex_unlock(&wifi_mtx);
    return n;
}

int y1_wifi_connect(const char *ssid, const char *psk)
{
    if (!ssid || !ssid[0]) return -1;
    if (!wifi_target_enabled)
        y1_wifi_set_enabled(true);

    pthread_mutex_lock(&wifi_mtx);
    load_wifi_db_locked();
    struct y1_wifi_network *n = find_or_add_net_locked(ssid);
    if (n) {
        n->saved = true;
        if (psk && psk[0]) {
            n->secured = true;
            snprintf(n->psk, sizeof(n->psk), "%s", psk);
        }
        save_wifi_db_locked();
    }
    snprintf(req_ssid, sizeof(req_ssid), "%s", ssid);
    snprintf(req_psk, sizeof(req_psk), "%s", (psk && psk[0]) ? psk : (n ? n->psk : ""));
    wifi_req_connect = true;
    pthread_cond_broadcast(&wifi_cond);
    pthread_mutex_unlock(&wifi_mtx);
    return 0;
}

void y1_wifi_disconnect(void)
{
    pthread_mutex_lock(&wifi_mtx);
    wifi_req_disconnect = true;
    pthread_cond_broadcast(&wifi_cond);
    pthread_mutex_unlock(&wifi_mtx);
}

void y1_wifi_forget(const char *ssid)
{
    if (!ssid || !ssid[0]) return;
    pthread_mutex_lock(&wifi_mtx);
    for (int i = 0; i < g_net_count; i++) {
        if (strcmp(g_nets[i].ssid, ssid) == 0) {
            if (g_nets[i].connected)
                wifi_req_disconnect = true;
            g_nets[i].saved = false;
            g_nets[i].psk[0] = '\0';
        }
    }
    save_wifi_db_locked();
    pthread_cond_broadcast(&wifi_cond);
    pthread_mutex_unlock(&wifi_mtx);
}
