/***************************************************************************
 * liby1 - Click-Wheel, Buttons, Touch Scan & Headphone Jack Input (MT6572)
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "liby1.h"

#ifndef KEY_HEADSETHOOK
#define KEY_HEADSETHOOK 226
#endif
#ifndef KEY_SEND
#define KEY_SEND        231
#endif
#ifndef KEY_HANGUEL
#define KEY_HANGUEL     122
#endif
#ifndef KEY_STOPCD
#define KEY_STOPCD      166
#endif

#define Y1_MAX_EVENT_DEVS 16

static int  g_event_fds[Y1_MAX_EVENT_DEVS];
static bool g_event_is_accdet[Y1_MAX_EVENT_DEVS];
static bool g_input_inited = false;
static int  g_h2w_fd = -1;
static int  g_h2w_raw_state = -1;
static int  g_tpd_state = -1;

int y1_input_init(void)
{
    if (g_input_inited)
        return 0;

    int count = 0;
    for (int i = 0; i < Y1_MAX_EVENT_DEVS; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        g_event_fds[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        g_event_is_accdet[i] = false;
        if (g_event_fds[i] >= 0) {
            char name[64] = {0};
            if (ioctl(g_event_fds[i], EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
                if (strstr(name, "ACCDET") != NULL || strstr(name, "accdet") != NULL)
                    g_event_is_accdet[i] = true;
            }
            count++;
        }
    }
    g_input_inited = true;
    return count > 0 ? 0 : -1;
}

void y1_input_close(void)
{
    for (int i = 0; i < Y1_MAX_EVENT_DEVS; i++) {
        if (g_event_fds[i] >= 0) {
            close(g_event_fds[i]);
            g_event_fds[i] = -1;
        }
    }
    if (g_h2w_fd >= 0) {
        close(g_h2w_fd);
        g_h2w_fd = -1;
    }
    g_input_inited = false;
}

uint32_t y1_input_map_keycode(int keycode)
{
    switch (keycode) {
    case KEY_UP:            /* 103: Click wheel counter-clockwise */
        return Y1_BTN_SCROLL_BACK;

    case KEY_DOWN:          /* 108: Click wheel clockwise */
        return Y1_BTN_SCROLL_FWD;

    case 232:               /* MT6572 hardware matrix keypad center switch */
    case KEY_ENTER:         /* 28 */
    case KEY_KPENTER:       /* 96 */
    case 352:               /* KEY_OK */
    case 353:               /* KEY_SELECT */
    case 314:               /* BTN_SELECT */
    case 315:               /* BTN_START */
    case 304:               /* BTN_A */
        return Y1_BTN_SELECT;

    case KEY_BACK:          /* 158: Top Menu / Back button */
    case KEY_MENU:          /* 139 */
        return Y1_BTN_MENU;

    case KEY_PLAYPAUSE:     /* 164: Bottom Play / Pause button */
    case KEY_PLAY:          /* 207 */
    case 119:               /* KEY_PAUSE */
        return Y1_BTN_PLAY;

    case KEY_LEFT:          /* 105: Left / Previous */
    case 165:               /* KEY_PREVIOUSSONG */
        return Y1_BTN_LEFT;

    case KEY_RIGHT:         /* 106: Right / Next */
    case 163:               /* KEY_NEXTSONG */
        return Y1_BTN_RIGHT;

    case KEY_HEADSETHOOK:   /* 226 */
    case KEY_SEND:          /* 231 */
    case 169:               /* KEY_PHONE */
        return Y1_BTN_HEADSET_MFB;

    case KEY_STOPCD:        /* 166: ACCDET long-press MFB */
    case KEY_HANGUEL:       /* 122 */
        return Y1_BTN_HEADSET_LONG;

    default:
        return Y1_BTN_NONE;
    }
}

int y1_input_poll(struct y1_input_event *out_ev, int timeout_ms)
{
    if (!g_input_inited)
        y1_input_init();

    struct pollfd pfds[Y1_MAX_EVENT_DEVS];
    int idx_map[Y1_MAX_EVENT_DEVS];
    nfds_t nfds = 0;

    for (int i = 0; i < Y1_MAX_EVENT_DEVS; i++) {
        if (g_event_fds[i] >= 0) {
            pfds[nfds].fd = g_event_fds[i];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            idx_map[nfds] = i;
            nfds++;
        }
    }

    if (nfds == 0)
        return -1;

    int rc = poll(pfds, nfds, timeout_ms);
    if (rc <= 0)
        return 0;

    for (nfds_t k = 0; k < nfds; k++) {
        if (pfds[k].revents & POLLIN) {
            struct input_event ev;
            int dev_idx = idx_map[k];
            while (read(g_event_fds[dev_idx], &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    if (out_ev) {
                        out_ev->keycode = ev.code;
                        out_ev->value = ev.value;
                        out_ev->is_accdet = g_event_is_accdet[dev_idx];
                        out_ev->button = y1_input_map_keycode(ev.code);
                        if (out_ev->is_accdet && out_ev->button == Y1_BTN_PLAY)
                            out_ev->button = Y1_BTN_HEADSET_MFB;
                    }
                    return 1;
                }
            }
        }
    }
    return 0;
}

void y1_input_set_touch_scanning(bool enabled)
{
    int target = enabled ? 1 : 0;
    if (!enabled && g_tpd_state == target)
        return;
    g_tpd_state = target;

    int fd = open("/proc/tpd_keys_enable", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        char val = enabled ? '1' : '0';
        (void)!write(fd, &val, 1);
        (void)!write(fd, "\n", 1);
        close(fd);
        y1_logf("[liby1-input] tpd_keys_enable=%d\n", target);
    }
}

int y1_jack_get_raw_state(void)
{
    if (g_h2w_fd < 0) {
        g_h2w_fd = open("/sys/class/switch/h2w/state", O_RDONLY | O_CLOEXEC);
    }
    if (g_h2w_fd >= 0) {
        char buf[16];
        lseek(g_h2w_fd, 0, SEEK_SET);
        ssize_t len = read(g_h2w_fd, buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            int state = 0;
            if (sscanf(buf, "%d", &state) == 1) {
                if (state != g_h2w_raw_state) {
                    g_h2w_raw_state = state;
                    if (state == 1) {
                        /* 4-pole headset inserted: configure 11.7ms press/release debounce */
                        y1_pmic_tune_accdet_debounce();
                    }
                    y1_logf("[liby1-input] h2w state=%d\n", state);
                }
            }
        } else {
            close(g_h2w_fd);
            g_h2w_fd = -1;
        }
    }
    return g_h2w_raw_state;
}

bool y1_jack_is_inserted(void)
{
    int st = y1_jack_get_raw_state();
    if (st < 0)
        return true; /* Default to inserted if switch driver not yet read or sysfs node unavailable */
    return st > 0;
}

bool y1_jack_has_mic(void)
{
    int st = y1_jack_get_raw_state();
    return st == 1;
}

