#include "ui_gfx.h"
#include "ipod_db.h"
#include "audio_player.h"
#include "album_art.h"
#include "photos.h"
#include "hangul_ime.h"
#include "liby1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define SHARED_CONF_PATH "/mnt/sdcard/.system/configapp/settings.conf"
#define MEUI_CONF_PATH   "/mnt/sdcard/.meui/meui.conf"
#define RESUME_CONF_PATH "/mnt/sdcard/.meui/resume.conf"
#define SAFE_VOL_LIMIT   60  /* 60 dB real in-ear safe threshold */
#define TR(en, ko) (g_lang == 1 ? (ko) : (en))

enum screen_id {
    SCR_HOME = 0,
    SCR_COVERFLOW,
    SCR_SONGS,
    SCR_ARTISTS,
    SCR_ALBUMS,
    SCR_PLAYLISTS,
    SCR_PHOTOS,
    SCR_PHOTO_VIEW,
    SCR_FILTERED_SONGS,
    SCR_QUEUE,
    SCR_NOW_PLAYING,
    SCR_SEARCH,
    SCR_SETTINGS,
    SCR_ADVANCED_SETTINGS,
    SCR_WALLPAPER,
    SCR_FONT_PICKER,
    SCR_BT_MENU,
    SCR_BT_PAIRED,
    SCR_BT_NEW,
    SCR_BT_FORGET
};

static int  g_lang = 0;
static int  g_dark_mode = 0;
static int  g_brightness = 65;
static int  g_vol_step = 5;            /* 1, 2, 5, 10 */
static int  g_seek_sec = 10;           /* 5, 10, 15, 30 */
static int  g_use_ipod_db = 1;         /* 1 = iPod DB, 0 = Normal Files */
static int  g_screen_timeout_sec = 30; /* 15, 30, 60, 120, 0 (Never) */
static int  g_safe_vol_enabled = 1;    /* 1 = Warn > 60 dB, 0 = Off */
static bool g_should_exit = false;

/* Power, CPU, Screen Timeout, Jack, BT & USB state */
static bool     g_screen_off = false;
static bool     g_wake_swallow_select = false;
static uint64_t g_last_activity_ms = 0;
static int      g_cur_cpu_mode = -1;
static bool     g_usb_was_online = false;
static bool     g_usb_active = false;
static bool     g_usb_charge_only = false;
static bool     g_jack_was_inserted = false;

/* MFB multi-click state */
static int      g_mfb_clicks = 0;
static uint64_t g_mfb_last_ms = 0;

/* Bluetooth callback flags & device list cache */
static volatile int     g_bt_ev_connected = 0;
static volatile int     g_bt_ev_disconnected = 0;
static volatile uint8_t g_bt_avrcp_op = 0;
static struct y1_bt_device g_bt_devs[Y1_BT_MAX_DEVICES];
static int                 g_bt_dev_count = 0;

/* ABC Fast Scroll state */
static uint64_t g_scroll_last_ms = 0;
static int      g_scroll_fast_streak = 0;
static int      g_scroll_last_dir = 0;
static bool     g_abc_active = false;
static uint64_t g_abc_until_ms = 0;
static char     g_abc_label[8] = "";

/* Safe Volume Warning Modal (> 60 dB) */
static bool     g_vol_warn_open = false;
static int      g_vol_warn_sel = 0; /* 0 = No, 1 = Yes */
static int      g_vol_warn_pending_delta = 0;
static bool     g_safe_vol_acked = false;
static uint64_t g_vol_up_last_ms = 0;
static int      g_vol_up_rapid_count = 0;

static struct ipod_db g_db;

/* Navigation Stack */
struct nav_frame {
    enum screen_id screen;
    int            sel;
    int            scroll;
};
#define NAV_STACK_MAX 12
static struct nav_frame g_nav_stack[NAV_STACK_MAX];
static int              g_nav_depth = 0;

static enum screen_id g_screen = SCR_HOME;
static int   g_home_sel = 0;
static int   g_home_scroll = 0;
static float g_home_scroll_anim = 0.0f;
static int   g_list_sel = 0;
static int   g_list_scroll = 0;
static float g_list_scroll_anim = 0.0f;
static float g_trans_x = 0.0f;
static float g_bg_blur_anim = 0.0f; /* 0.0 = sharp (Home/Now Playing), 1.0 = blurred (submenus) */

/* Right-pane iPod-style smooth corner pan state */
static float    g_art_pan_x0 = 0.5f, g_art_pan_y0 = 0.5f;
static float    g_art_pan_x1 = 0.0f, g_art_pan_y1 = 0.0f;
static uint64_t g_art_pan_start_ms = 0;
static uint64_t g_art_pan_dur_ms = 5500;
static bool     g_art_pan_to_corner = true;
static int      g_art_pan_last_corner = -1;
static float    g_art_pan_cur_x = 0.5f, g_art_pan_cur_y = 0.5f;

/* Wallpaper / Font file picker */
#define WP_MAX_ENTRIES 256
struct wp_entry {
    char name[128];
    char path[384];
    bool is_dir;
    bool is_clear;
    bool is_album_art;
};
static char            g_wp_dir[256] = "/mnt/sdcard";
static struct wp_entry g_wp_items[WP_MAX_ENTRIES];
static int             g_wp_count = 0;
static bool            g_picker_is_font = false;
static int             g_wp_album_art = 0;
static int             g_last_wp_art_track = -2;

/* Play/Pause 1s hold-to-stop */
static bool     g_play_held = false;
static bool     g_play_hold_fired = false;
static uint64_t g_play_press_ms = 0;

/* Filtered song list (dynamically allocated for 100k+ song libraries) */
static char  g_filter_title[96] = "";
static int  *g_filter_indices = NULL;
static int   g_filter_count = 0;
static int   g_filter_cap = 0;
static bool  g_filter_is_album = false;

static void reset_filter_list(void)
{
    g_filter_count = 0;
    g_filter_is_album = false;
}

static int cmp_filter_track_num(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    uint16_t ta = g_db.tracks[ia].track_num;
    uint16_t tb = g_db.tracks[ib].track_num;
    if (ta != tb) {
        if (ta == 0) return 1;
        if (tb == 0) return -1;
        return (int)ta - (int)tb;
    }
    return ia - ib;
}

static void format_meta_pair(char *out, size_t sz, const char *a, const char *b)
{
    bool ha = (a && a[0] != '\0');
    bool hb = (b && b[0] != '\0');
    if (ha && hb)
        snprintf(out, sz, "%s \xE2\x80\xA2 %s", a, b);
    else if (ha)
        snprintf(out, sz, "%s", a);
    else if (hb)
        snprintf(out, sz, "%s", b);
    else if (sz > 0)
        out[0] = '\0';
}

static void push_filter_index(int idx)
{
    if (g_filter_count >= g_filter_cap) {
        int nc = g_filter_cap > 0 ? g_filter_cap * 2 : 1024;
        int *ni = realloc(g_filter_indices, sizeof(int) * (size_t)nc);
        if (!ni) return;
        g_filter_indices = ni;
        g_filter_cap = nc;
    }
    g_filter_indices[g_filter_count++] = idx;
}

/* Context Menu overlay */
static bool g_ctx_open = false;
static int  g_ctx_track_idx = -1;
static int  g_ctx_sel = 0;
static int  g_ctx_scroll = 0;
static float g_ctx_scroll_anim = 0.0f;
static bool g_ctx_from_np = false;
static bool g_ctx_from_queue = false;
static int  g_ctx_queue_pos = -1;

/* Playlist Picker modal */
#define PL_PICK_MAX 32
static bool g_pl_pick_open = false;
static int  g_pl_pick_track_idx = -1;
static int  g_pl_pick_sel = 0;
static int  g_pl_pick_scroll = 0;
static int  g_pl_pick_count = 0;
static char g_pl_pick_names[PL_PICK_MAX][96];
static char g_pl_pick_paths[PL_PICK_MAX][256];

/* Photos full-view state */
static int      g_photo_idx = 0;
static float    g_photo_zoom = 1.0f;
static bool     g_photo_slideshow = false;
static uint64_t g_photo_slide_ms = 0;
static uint64_t g_photo_zoom_hud_ms = 0;

static int ctx_item_count(void) { return g_ctx_from_queue ? 6 : (g_ctx_from_np ? 8 : 6); }

/* Long-press detection for Center/Select & Prev/Next seek */
static bool     g_sel_held = false;
static bool     g_sel_long_fired = false;
static uint64_t g_sel_press_ms = 0;

static uint32_t g_media_held_btn = Y1_BTN_NONE;
static bool     g_media_seek_fired = false;
static uint64_t g_media_press_ms = 0;
static uint64_t g_media_last_seek_ms = 0;

/* Favorites bitset (dynamically sized to g_db.track_count) */
static bool *g_fav_flags = NULL;
static int   g_fav_cap = 0;

static bool is_track_fav(int ti)
{
    return (ti >= 0 && ti < g_fav_cap && g_fav_flags) ? g_fav_flags[ti] : false;
}

static void set_track_fav(int ti, bool v)
{
    if (ti >= 0 && ti < g_fav_cap && g_fav_flags) g_fav_flags[ti] = v;
}

static void play_all_tracks(int start_pos)
{
    if (g_db.track_count <= 0) return;
    int *all_idx = malloc(sizeof(int) * (size_t)g_db.track_count);
    if (!all_idx) return;
    for (int i = 0; i < g_db.track_count; i++) all_idx[i] = i;
    player_play_queue(all_idx, g_db.track_count, start_pos);
    free(all_idx);
}

/* Cached hardware telemetry (updated every 1s in poll loop; zero sysfs I/O in render!) */
static int  g_cached_bat_pct = 100;
static bool g_cached_bat_chg = false;
static bool g_cached_bt_conn = false;

/* Search OSK state */
static char g_search_buf[96] = "";
static int  g_search_len = 0;
static int  g_search_mode = 0; /* 0 = lower, 1 = upper, 2 = hangul, 3 = hangul shift */
static int  g_search_sel = 0;  /* 0..39 (4x10 grid) */
static struct hangul_ime g_search_ime = { -1, -1, -1, 0 };

/* Toast & Volume HUD */
static char     g_toast[96] = "";
static uint64_t g_toast_until_ms = 0;
static uint64_t g_vol_hud_until_ms = 0;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

#define MAX_ITEM_PILLS 24
struct item_pill_anim {
    int      scope;
    int      idx;
    float    cur;
    float    start;
    float    target;
    uint64_t start_ms;
    bool     active;
    bool     valid;
};
static struct item_pill_anim g_item_pills[MAX_ITEM_PILLS];
static int g_item_pill_next = 0;

static void item_pill_clear_scope(int scope)
{
    for (int i = 0; i < MAX_ITEM_PILLS; i++) {
        if (g_item_pills[i].valid && g_item_pills[i].scope == scope)
            g_item_pills[i].valid = false;
    }
}

static float item_pill_get(int scope, int idx, bool sel, uint64_t tnow)
{
    float want = sel ? 1.0f : 0.0f;
    struct item_pill_anim *a = NULL;
    for (int i = 0; i < MAX_ITEM_PILLS; i++) {
        if (g_item_pills[i].valid && g_item_pills[i].scope == scope && g_item_pills[i].idx == idx) {
            a = &g_item_pills[i];
            break;
        }
    }
    if (!a) {
        a = &g_item_pills[g_item_pill_next];
        g_item_pill_next = (g_item_pill_next + 1) % MAX_ITEM_PILLS;
        a->scope = scope;
        a->idx = idx;
        a->cur = want;
        a->start = want;
        a->target = want;
        a->active = false;
        a->valid = true;
        return a->cur;
    }
    if (a->target != want) {
        a->start = a->cur;
        a->target = want;
        a->start_ms = tnow;
        a->active = true;
    }
    return a->cur;
}

static bool item_pill_update_all(uint64_t tnow)
{
    bool any = false;
    for (int i = 0; i < MAX_ITEM_PILLS; i++) {
        struct item_pill_anim *a = &g_item_pills[i];
        if (!a->valid || !a->active) continue;
        uint64_t el = (tnow >= a->start_ms) ? (tnow - a->start_ms) : 0;
        if (el >= 400) {
            a->cur = a->target;
            a->active = false;
            any = true;
        } else {
            float t = (float)el / 400.0f;
            float inv = 1.0f - t;
            float ease = 1.0f - inv * inv * inv; /* Smooth 0.4s cubic ease-out */
            a->cur = a->start + (a->target - a->start) * ease;
            any = true;
        }
    }
    return any;
}

static bool item_pill_any_active(void)
{
    for (int i = 0; i < MAX_ITEM_PILLS; i++) {
        if (g_item_pills[i].valid && g_item_pills[i].active) return true;
    }
    return false;
}

#define MAX_SW_ANIMS 16
struct switch_anim {
    int      scope;
    int      idx;
    float    cur;
    float    start;
    float    target;
    uint64_t start_ms;
    bool     active;
    bool     valid;
};
static struct switch_anim g_sw_anims[MAX_SW_ANIMS];
static int g_sw_anim_next = 0;

static float sw_anim_get(int scope, int idx, bool on, uint64_t tnow)
{
    float want = on ? 1.0f : 0.0f;
    struct switch_anim *sw = NULL;
    for (int i = 0; i < MAX_SW_ANIMS; i++) {
        if (g_sw_anims[i].valid && g_sw_anims[i].scope == scope && g_sw_anims[i].idx == idx) {
            sw = &g_sw_anims[i];
            break;
        }
    }
    if (!sw) {
        sw = &g_sw_anims[g_sw_anim_next];
        g_sw_anim_next = (g_sw_anim_next + 1) % MAX_SW_ANIMS;
        sw->scope = scope;
        sw->idx = idx;
        sw->cur = want;
        sw->start = want;
        sw->target = want;
        sw->active = false;
        sw->valid = true;
        return sw->cur;
    }
    if (sw->target != want) {
        sw->start = sw->cur;
        sw->target = want;
        sw->start_ms = tnow;
        sw->active = true;
    }
    return sw->cur;
}

static bool sw_anim_update_all(uint64_t tnow)
{
    bool any = false;
    for (int i = 0; i < MAX_SW_ANIMS; i++) {
        struct switch_anim *sw = &g_sw_anims[i];
        if (!sw->valid || !sw->active) continue;
        uint64_t el = (tnow >= sw->start_ms) ? (tnow - sw->start_ms) : 0;
        if (el >= 260) {
            sw->cur = sw->target;
            sw->active = false;
            any = true;
        } else {
            float t = (float)el / 260.0f;
            float inv = 1.0f - t;
            float ease = 1.0f - inv * inv * inv;
            sw->cur = sw->start + (sw->target - sw->start) * ease;
            any = true;
        }
    }
    return any;
}

static bool sw_anim_any_active(void)
{
    for (int i = 0; i < MAX_SW_ANIMS; i++) {
        if (g_sw_anims[i].valid && g_sw_anims[i].active) return true;
    }
    return false;
}

static uint32_t lerp_argb(uint32_t c0, uint32_t c1, float t)
{
    if (t <= 0.0f) return c0;
    if (t >= 1.0f) return c1;
    uint32_t a0 = (c0 >> 24) & 0xFF, r0 = (c0 >> 16) & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = c0 & 0xFF;
    uint32_t a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint32_t a = (uint32_t)((float)a0 + ((float)a1 - (float)a0) * t + 0.5f);
    uint32_t r = (uint32_t)((float)r0 + ((float)r1 - (float)r0) * t + 0.5f);
    uint32_t g = (uint32_t)((float)g0 + ((float)g1 - (float)g0) * t + 0.5f);
    uint32_t b = (uint32_t)((float)b0 + ((float)b1 - (float)b0) * t + 0.5f);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void update_art_pan(uint64_t tnow)
{
    static const float corners[4][2] = {
        { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f }
    };
    if (g_art_pan_start_ms == 0) {
        g_art_pan_start_ms = tnow;
        g_art_pan_x0 = 0.5f; g_art_pan_y0 = 0.5f;
        int c = rand() % 4;
        g_art_pan_last_corner = c;
        g_art_pan_x1 = corners[c][0];
        g_art_pan_y1 = corners[c][1];
        g_art_pan_to_corner = true;
    }
    uint64_t el = (tnow >= g_art_pan_start_ms) ? (tnow - g_art_pan_start_ms) : 0;
    if (el >= g_art_pan_dur_ms) {
        g_art_pan_start_ms = tnow;
        g_art_pan_x0 = g_art_pan_x1;
        g_art_pan_y0 = g_art_pan_y1;
        g_art_pan_to_corner = !g_art_pan_to_corner;
        if (g_art_pan_to_corner) {
            int c = rand() % 4;
            if (c == g_art_pan_last_corner) c = (c + 1 + (rand() % 3)) % 4;
            g_art_pan_last_corner = c;
            g_art_pan_x1 = corners[c][0];
            g_art_pan_y1 = corners[c][1];
        } else {
            g_art_pan_x1 = 0.5f;
            g_art_pan_y1 = 0.5f;
        }
        el = 0;
    }
    float t = (float)el / (float)g_art_pan_dur_ms;
    float e = t * t * (3.0f - 2.0f * t); /* Smoothstep */
    g_art_pan_cur_x = g_art_pan_x0 + (g_art_pan_x1 - g_art_pan_x0) * e;
    g_art_pan_cur_y = g_art_pan_y0 + (g_art_pan_y1 - g_art_pan_y0) * e;
}

static void show_toast(const char *msg, int dur_ms)
{
    snprintf(g_toast, sizeof(g_toast), "%s", msg ? msg : "");
    g_toast_until_ms = now_ms() + (uint64_t)dur_ms;
}

static int g_cur_dual_core = -1;

static void update_cpu_power_state(bool ui_burst, bool bt_stream)
{
    /* 3-tier power policy:
     * - UI active/animating: Dynamic + Dual-Core for 60fps UI
     * - BT streaming at idle/screen-off: Dynamic + Single-Core (Core 1 OFF)
     * - Idle: Powersave (390MHz) + Single-Core */
    int target_mode = (ui_burst || bt_stream) ? Y1_CPU_MODE_DYNAMIC : Y1_CPU_MODE_POWERSAVE;
    int target_dual = ui_burst ? 1 : 0;
    if (target_dual != g_cur_dual_core) {
        g_cur_dual_core = target_dual;
        y1_cpu_set_dual_core(target_dual != 0);
    }
    if (target_mode != g_cur_cpu_mode) {
        g_cur_cpu_mode = target_mode;
        y1_cpu_set_mode(target_mode);
    }
}

static void apply_base_low_power(void)
{
    g_cur_cpu_mode = -1;
    g_cur_dual_core = -1;
    y1_fb_set_eco_mode(true);
    update_cpu_power_state(false, false);
}

static void fade_backlight(int from_pct, int to_pct)
{
    const int steps = 8;
    for (int i = 1; i <= steps; i++) {
        int val = from_pct + ((to_pct - from_pct) * i) / steps;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        y1_backlight_set_percent(val);
        usleep(18000);
    }
}

/* =========================================================================
 * Bluetooth Callbacks & Submenu Helpers (Auto Best Codec: SBC < SBC-XQ < AAC)
 * ========================================================================= */
static void bt_on_connect(void)
{
    g_bt_ev_connected = 1;
}

static void bt_on_disconnect(void)
{
    player_pause();
    g_bt_ev_disconnected = 1;
}

static void bt_on_avrcp(uint8_t op_id)
{
    g_bt_avrcp_op = op_id;
}

static void init_bluetooth_callbacks(void)
{
    static const struct y1_bt_callbacks cbs = {
        .on_audio_connect = bt_on_connect,
        .on_audio_disconnect = bt_on_disconnect,
        .on_avrcp_passthrough = bt_on_avrcp,
    };
    y1_bt_init();
    y1_bt_set_callbacks(&cbs);
}

static void refresh_bt_sublist(void)
{
    if (g_screen == SCR_BT_PAIRED || g_screen == SCR_BT_FORGET)
        g_bt_dev_count = y1_bt_get_paired_devices(g_bt_devs, Y1_BT_MAX_DEVICES);
    else if (g_screen == SCR_BT_NEW)
        g_bt_dev_count = y1_bt_get_new_devices(g_bt_devs, Y1_BT_MAX_DEVICES);
    else
        g_bt_dev_count = 0;
    if (g_list_sel >= g_bt_dev_count && g_bt_dev_count > 0)
        g_list_sel = g_bt_dev_count - 1;
}

static void bt_connect_best_codec(const uint8_t bdaddr[6])
{
    /* liby1 avdtp_configure_best_endpoint negotiates AAC > SBC-XQ > SBC when AAC is preferred */
    y1_bt_set_device_codec(bdaddr, Y1_BT_CODEC_AAC);
    y1_bt_connect_device(bdaddr);
}

/* =========================================================================
 * ABC Fast Scroll Helper (English A-Z + Korean Choseong ㄱ-ㅎ + #)
 * ========================================================================= */
static int meui_abc_bucket(const char *s, char *out_lbl, size_t lbl_sz)
{
    static const char *const k_cho[19] = {
        "ㄱ","ㄲ","ㄴ","ㄷ","ㄸ","ㄹ","ㅁ","ㅂ","ㅃ",
        "ㅅ","ㅆ","ㅇ","ㅈ","ㅉ","ㅊ","ㅋ","ㅌ","ㅍ","ㅎ"
    };
    if (!s || !*s) { snprintf(out_lbl, lbl_sz, "#"); return 0; }
    const unsigned char *u = (const unsigned char *)s;
    uint32_t cp = 0;
    if (u[0] < 0x80) cp = u[0];
    else if ((u[0] & 0xE0) == 0xC0 && u[1]) cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    else if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);

    if (cp >= 'a' && cp <= 'z') cp -= 32;
    if (cp >= 'A' && cp <= 'Z') {
        snprintf(out_lbl, lbl_sz, "%c", (char)cp);
        return (int)(cp - 'A' + 1);
    }
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
        int cho = (int)((cp - 0xAC00) / 588);
        snprintf(out_lbl, lbl_sz, "%s", k_cho[cho]);
        return 100 + cho;
    }
    snprintf(out_lbl, lbl_sz, "#");
    return 0;
}

static bool screen_is_abc_sortable(enum screen_id scr)
{
    return (scr == SCR_SONGS || scr == SCR_ARTISTS || scr == SCR_ALBUMS ||
            scr == SCR_COVERFLOW || scr == SCR_FILTERED_SONGS || scr == SCR_PLAYLISTS);
}

static const char *list_item_sort_name(int idx)
{
    if (g_screen == SCR_SONGS && idx < g_db.track_count)
        return g_db.tracks[idx].title;
    if (g_screen == SCR_FILTERED_SONGS && idx < g_filter_count)
        return g_db.tracks[g_filter_indices[idx]].title;
    if (g_screen == SCR_ARTISTS && idx < g_db.artist_count)
        return g_db.artists[idx].name;
    if ((g_screen == SCR_ALBUMS || g_screen == SCR_COVERFLOW) && idx < g_db.album_count)
        return g_db.albums[idx].name;
    if (g_screen == SCR_PLAYLISTS && idx < g_db.playlist_count)
        return g_db.playlists[idx].name;
    return "";
}

/* =========================================================================
 * Preferences & Instant Resume Persistence
 * ========================================================================= */
static void load_prefs(void)
{
    FILE *f = fopen(SHARED_CONF_PATH, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char k[64]; int v = 0;
            if (sscanf(line, "%63[^=]=%d", k, &v) == 2) {
                if (strcmp(k, "lang") == 0) g_lang = (v == 1) ? 1 : 0;
                else if (strcmp(k, "dark_mode") == 0) g_dark_mode = v ? 1 : 0;
                else if (strcmp(k, "brightness_pct") == 0 && v >= 5 && v <= 100) g_brightness = v;
            }
        }
        fclose(f);
    }
    f = fopen(MEUI_CONF_PATH, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char k[64]; int v = 0;
            if (sscanf(line, "%63[^=]=%d", k, &v) == 2) {
                if (strcmp(k, "vol_step") == 0 && v >= 1 && v <= 20) g_vol_step = v;
                else if (strcmp(k, "seek_sec") == 0 && v >= 5 && v <= 60) g_seek_sec = v;
                else if (strcmp(k, "use_ipod_db") == 0) g_use_ipod_db = v ? 1 : 0;
                else if (strcmp(k, "screen_timeout") == 0 && v >= 0 && v <= 600) g_screen_timeout_sec = v;
                else if (strcmp(k, "safe_vol") == 0) g_safe_vol_enabled = v ? 1 : 0;
                else if (strcmp(k, "wp_album_art") == 0) g_wp_album_art = v ? 1 : 0;
            }
        }
        fclose(f);
    }
    gfx_set_dark_mode(g_dark_mode != 0);
    y1_usb_set_ipod_mode(g_use_ipod_db != 0);
}

static void save_meui_prefs(void)
{
    mkdir("/mnt/sdcard/.meui", 0777);
    FILE *f = fopen(MEUI_CONF_PATH, "w");
    if (!f) return;
    fprintf(f, "vol_step=%d\nseek_sec=%d\nuse_ipod_db=%d\nscreen_timeout=%d\nsafe_vol=%d\nwp_album_art=%d\n",
            g_vol_step, g_seek_sec, g_use_ipod_db, g_screen_timeout_sec, g_safe_vol_enabled, g_wp_album_art);
    fclose(f);
}

static void save_resume_state(void)
{
    struct player_status st;
    player_get_status(&st);
    if (!st.has_track || st.track_idx < 0 || st.track_idx >= g_db.track_count)
        return;
    mkdir("/mnt/sdcard/.meui", 0777);
    FILE *f = fopen(RESUME_CONF_PATH, "w");
    if (!f) return;
    fprintf(f, "fs_path=%s\npos_ms=%u\nvol_pct=%d\n",
            g_db.tracks[st.track_idx].fs_path, st.pos_ms, st.volume_pct);
    fclose(f);
}

static void load_resume_state(void)
{
    FILE *f = fopen(RESUME_CONF_PATH, "r");
    if (!f) return;
    char saved_path[256] = "";
    uint32_t saved_pos = 0;
    int saved_vol = 50;
    char line[320];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (strncmp(line, "fs_path=", 8) == 0) {
            snprintf(saved_path, sizeof(saved_path), "%.255s", line + 8);
        } else if (strncmp(line, "pos_ms=", 7) == 0) {
            saved_pos = (uint32_t)strtoul(line + 7, NULL, 10);
        } else if (strncmp(line, "vol_pct=", 8) == 0) {
            saved_vol = atoi(line + 8);
        }
    }
    fclose(f);

    if (saved_vol < 0) saved_vol = 0;
    if (saved_vol > 100) saved_vol = 100;
    player_set_volume(saved_vol);
    if (saved_vol > SAFE_VOL_LIMIT) g_safe_vol_acked = true;

    if (!saved_path[0] || g_db.track_count <= 0) return;

    int found = -1;
    for (int i = 0; i < g_db.track_count; i++) {
        if (strcmp(g_db.tracks[i].fs_path, saved_path) == 0) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        int *all_idx = malloc(sizeof(int) * (size_t)g_db.track_count);
        if (all_idx) {
            for (int i = 0; i < g_db.track_count; i++) all_idx[i] = i;
            player_prepare_resume(all_idx, g_db.track_count, found, saved_pos, saved_vol);
            free(all_idx);
        }
    }
}

/* =========================================================================
 * Safe Volume Adjustment (> 60 dB Warning)
 * ========================================================================= */
static void request_volume_delta(int delta)
{
    struct player_status st;
    player_get_status(&st);

    if (delta < 0) {
        g_vol_up_rapid_count = 0;
        player_adjust_volume(delta);
        player_get_status(&st);
        if (st.volume_pct <= SAFE_VOL_LIMIT - 10)
            g_safe_vol_acked = false;
        g_vol_hud_until_ms = now_ms() + 1500;
        save_resume_state();
        return;
    }

    uint64_t tnow = now_ms();
    if (tnow - g_vol_up_last_ms <= 650) g_vol_up_rapid_count++;
    else g_vol_up_rapid_count = 1;
    g_vol_up_last_ms = tnow;

    int target = st.volume_pct + delta;
    if (target > 100) target = 100;

    if (g_safe_vol_enabled && !g_safe_vol_acked && target > SAFE_VOL_LIMIT &&
        (g_vol_up_rapid_count >= 2 || st.volume_pct >= SAFE_VOL_LIMIT)) {
        if (st.volume_pct < SAFE_VOL_LIMIT)
            player_set_volume(SAFE_VOL_LIMIT);
        g_vol_warn_open = true;
        g_vol_warn_sel = 0;
        g_vol_warn_pending_delta = delta;
        y1_vibrate(25);
        return;
    }

    player_adjust_volume(delta);
    g_vol_hud_until_ms = now_ms() + 1500;
    save_resume_state();
}

static bool has_image_ext(const char *name)
{
    size_t n = strlen(name);
    if (n >= 4 && (strcasecmp(name + n - 4, ".png") == 0 || strcasecmp(name + n - 4, ".jpg") == 0))
        return true;
    if (n >= 5 && strcasecmp(name + n - 5, ".jpeg") == 0)
        return true;
    return false;
}

static bool has_font_ext(const char *name)
{
    size_t n = strlen(name);
    if (n >= 4 && (strcasecmp(name + n - 4, ".ttf") == 0 || strcasecmp(name + n - 4, ".otf") == 0))
        return true;
    return false;
}

static int cmp_wp_entry(const void *a, const void *b)
{
    const struct wp_entry *ea = (const struct wp_entry *)a;
    const struct wp_entry *eb = (const struct wp_entry *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcasecmp(ea->name, eb->name);
}

static void sync_album_art_wallpaper(const struct player_status *st, bool crossfade_1s)
{
    if (!g_wp_album_art) return;
    int ti = (st && st->has_track && st->track_idx >= 0 && st->track_idx < g_db.track_count)
             ? st->track_idx : -1;
    if (ti == g_last_wp_art_track) return;
    g_last_wp_art_track = ti;
    const struct art_bitmap *art = (ti >= 0) ? album_art_get_for_track(&g_db.tracks[ti]) : NULL;
    gfx_set_wallpaper_rgb565(art ? art->rgb565 : NULL, art ? art->w : 0, art ? art->h : 0, crossfade_1s);
}

static void scan_picker_dir(const char *dir, bool is_font)
{
    g_picker_is_font = is_font;
    snprintf(g_wp_dir, sizeof(g_wp_dir), "%s", dir);
    g_wp_count = 0;
    if (!is_font && (gfx_has_wallpaper() || g_wp_album_art) && g_wp_count < WP_MAX_ENTRIES) {
        snprintf(g_wp_items[g_wp_count].name, sizeof(g_wp_items[0].name), "%s",
                 TR("Clear Wallpaper", "배경화면 지우기"));
        g_wp_items[g_wp_count].path[0] = '\0';
        g_wp_items[g_wp_count].is_dir = false;
        g_wp_items[g_wp_count].is_clear = true;
        g_wp_items[g_wp_count].is_album_art = false;
        g_wp_count++;
    } else if (is_font && gfx_has_custom_font() && g_wp_count < WP_MAX_ENTRIES) {
        snprintf(g_wp_items[g_wp_count].name, sizeof(g_wp_items[0].name), "%s",
                 TR("Reset Default Font", "기본 글꼴로 복원"));
        g_wp_items[g_wp_count].path[0] = '\0';
        g_wp_items[g_wp_count].is_dir = false;
        g_wp_items[g_wp_count].is_clear = true;
        g_wp_items[g_wp_count].is_album_art = false;
        g_wp_count++;
    }
    if (!is_font && strcmp(g_wp_dir, "/mnt/sdcard") == 0 && g_wp_count < WP_MAX_ENTRIES) {
        snprintf(g_wp_items[g_wp_count].name, sizeof(g_wp_items[0].name), "%s",
                 TR("Use Album Art", "앨범 아트를 배경으로 사용"));
        g_wp_items[g_wp_count].path[0] = '\0';
        g_wp_items[g_wp_count].is_dir = false;
        g_wp_items[g_wp_count].is_clear = false;
        g_wp_items[g_wp_count].is_album_art = true;
        g_wp_count++;
    }
    if (strcmp(g_wp_dir, "/mnt/sdcard") != 0 && g_wp_count < WP_MAX_ENTRIES) {
        snprintf(g_wp_items[g_wp_count].name, sizeof(g_wp_items[0].name), "..");
        snprintf(g_wp_items[g_wp_count].path, sizeof(g_wp_items[0].path), "%s", g_wp_dir);
        char *sl = strrchr(g_wp_items[g_wp_count].path, '/');
        if (sl && sl != g_wp_items[g_wp_count].path) *sl = '\0';
        else snprintf(g_wp_items[g_wp_count].path, sizeof(g_wp_items[0].path), "/mnt/sdcard");
        g_wp_items[g_wp_count].is_dir = true;
        g_wp_items[g_wp_count].is_clear = false;
        g_wp_items[g_wp_count].is_album_art = false;
        g_wp_count++;
    }
    int sort_start = g_wp_count;
    DIR *d = opendir(g_wp_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_wp_count < WP_MAX_ENTRIES) {
        if (de->d_name[0] == '.') continue;
        char full[384];
        snprintf(full, sizeof(full), "%s/%s", g_wp_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        bool match = is_font ? has_font_ext(de->d_name) : has_image_ext(de->d_name);
        if (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && match)) {
            snprintf(g_wp_items[g_wp_count].name, sizeof(g_wp_items[0].name), "%.127s", de->d_name);
            snprintf(g_wp_items[g_wp_count].path, sizeof(g_wp_items[0].path), "%.383s", full);
            g_wp_items[g_wp_count].is_dir = S_ISDIR(st.st_mode);
            g_wp_items[g_wp_count].is_clear = false;
            g_wp_count++;
        }
    }
    closedir(d);
    if (g_wp_count - sort_start > 1)
        qsort(&g_wp_items[sort_start], (size_t)(g_wp_count - sort_start), sizeof(struct wp_entry), cmp_wp_entry);
}

static void push_screen(enum screen_id next)
{
    if (next == g_screen) return;
    int existing = -1;
    for (int i = 0; i < g_nav_depth; i++) {
        if (g_nav_stack[i].screen == next) {
            existing = i;
            break;
        }
    }
    if (existing >= 0) {
        g_nav_depth = existing;
        g_trans_x = -90.0f;
    } else {
        if (g_nav_depth < NAV_STACK_MAX) {
            g_nav_stack[g_nav_depth].screen = g_screen;
            g_nav_stack[g_nav_depth].sel    = (g_screen == SCR_HOME) ? g_home_sel : g_list_sel;
            g_nav_stack[g_nav_depth].scroll = (g_screen == SCR_HOME) ? g_home_scroll : g_list_scroll;
            g_nav_depth++;
        }
        g_trans_x = 90.0f;
    }
    g_screen = next;
    g_abc_active = false;
    g_scroll_fast_streak = 0;
    if (next == SCR_QUEUE) {
        struct player_status st;
        player_get_status(&st);
        int qpos = (st.queue_len > 0 && st.queue_pos >= 0 && st.queue_pos < st.queue_len) ? st.queue_pos : 0;
        g_list_sel = qpos;
        g_list_scroll = qpos > 1 ? qpos - 1 : 0;
        g_list_scroll_anim = (float)g_list_scroll;
        item_pill_clear_scope((int)next);
    } else if (next != SCR_HOME && next != SCR_NOW_PLAYING) {
        g_list_sel = 0;
        g_list_scroll = 0;
        g_list_scroll_anim = 0.0f;
        item_pill_clear_scope((int)next);
    }
    if (next == SCR_BT_NEW) {
        if (!y1_bt_is_enabled()) y1_bt_set_enabled(true);
        y1_bt_start_scan();
    }
    if (next == SCR_BT_PAIRED || next == SCR_BT_NEW || next == SCR_BT_FORGET)
        refresh_bt_sublist();
}

static void pop_screen(void)
{
    g_abc_active = false;
    g_scroll_fast_streak = 0;
    if (g_nav_depth > 0) {
        g_nav_depth--;
        struct nav_frame prev = g_nav_stack[g_nav_depth];
        g_trans_x = -90.0f;
        g_screen = prev.screen;
        if (prev.screen == SCR_HOME) {
            g_home_sel = prev.sel;
            g_home_scroll = prev.scroll;
            g_home_scroll_anim = (float)prev.scroll;
            item_pill_clear_scope(SCR_HOME);
        } else {
            g_list_sel = prev.sel;
            g_list_scroll = prev.scroll;
            g_list_scroll_anim = (float)prev.scroll;
            item_pill_clear_scope((int)prev.screen);
        }
    } else if (g_screen != SCR_HOME) {
        g_trans_x = -90.0f;
        g_screen = SCR_HOME;
    }
}

static uint64_t g_scan_last_draw_ms = 0;

static void draw_header(const char *title, const char *icon_name, const struct player_status *st);

static bool db_scan_progress_cb(int tracks_found, int dirs_scanned, const char *cur_path, void *user)
{
    bool *cancelled = (bool *)user;
    struct y1_input_event ev;
    while (y1_input_poll(&ev, 0) > 0) {
        if (ev.button == Y1_BTN_MENU && ev.value == 1) {
            if (cancelled) *cancelled = true;
            return false;
        }
    }

    uint64_t tnow = now_ms();
    if (tnow - g_scan_last_draw_ms < 60)
        return true;
    g_scan_last_draw_ms = tnow;

    gfx_clear(COL_BG);
    draw_header(TR("Scanning Files", "파일 검색 중"), "hard-drive", NULL);

    int cw = UI_W - 40, ch = 220;
    int cx = 20, cy = 84;
    gfx_fill_round_rect(cx, cy, cw, ch, 34, COL_SURFACE);
    gfx_stroke_round_rect(cx, cy, cw, ch, 34, 2, COL_BORDER_HI);

    char msg[96];
    snprintf(msg, sizeof(msg),
             g_lang == 1 ? "%d곡 발견 (%d개 폴더)" : "Found %d songs (%d folders)",
             tracks_found, dirs_scanned);
    int mw = gfx_text_width(msg, 23.0f, UI_FONT_SEMIBOLD);
    gfx_draw_text(cx + (cw - mw) / 2, cy + 28, msg, 23.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    if (cur_path && cur_path[0]) {
        const char *short_p = cur_path;
        if (strncmp(short_p, "/mnt/sdcard", 11) == 0) short_p += 11;
        if (!short_p[0]) short_p = "/";
        gfx_draw_text_ellipsize(cx + 24, cy + 66, cw - 48, short_p, 17.0f, UI_FONT_REGULAR, COL_TEXT_DIM);
    }

    /* Animated indeterminate / track-fill progress bar */
    int bx = cx + 28, by = cy + 108, bw = cw - 56, bh = 18;
    gfx_fill_round_rect(bx, by, bw, bh, 9, COL_SURFACE_HI);
    int span = bw / 3;
    int pos = (int)((tnow / 8) % (uint64_t)(bw + span)) - span;
    int fx0 = pos < 0 ? 0 : pos;
    int fx1 = pos + span > bw ? bw : pos + span;
    if (fx1 > fx0)
        gfx_fill_round_rect(bx + fx0, by, fx1 - fx0, bh, 9, COL_ACCENT);

    const char *cancel_lbl = TR("Press MENU to Cancel", "취소하려면 MENU를 누르세요");
    int clw = gfx_text_width(cancel_lbl, 19.0f, UI_FONT_SEMIBOLD);
    int pw = clw + 36;
    if (pw > cw - 40) pw = cw - 40;
    int px = cx + (cw - pw) / 2;
    int py = cy + ch - 62;
    gfx_fill_round_rect(px, py, pw, 42, 21, COL_SURFACE_HI);
    gfx_draw_text_ellipsize(px + 18, py + 10, pw - 36, cancel_lbl, 19.0f, UI_FONT_SEMIBOLD, COL_DANGER);

    gfx_present();
    return true;
}

static void load_library_with_ui(bool initial_boot)
{
    if (!initial_boot) {
        save_resume_state();
        player_stop();
        album_art_shutdown();
    photos_shutdown();
        album_art_init();
    }
    y1_ipod_native_invalidate();

    bool cancelled = false;
    g_scan_last_draw_ms = 0;
    ipod_db_load(&g_db, g_use_ipod_db != 0,
                 g_use_ipod_db ? NULL : db_scan_progress_cb, &cancelled);
    free(g_fav_flags);
    g_fav_cap = g_db.track_count > 0 ? g_db.track_count : 1;
    g_fav_flags = calloc((size_t)g_fav_cap, sizeof(bool));
    if (cancelled)
        show_toast(TR("Scan cancelled", "검색 취소됨"), 1200);
    if (!initial_boot)
        load_resume_state();
}

static void reload_library(void)
{
    load_library_with_ui(false);
}

/* =========================================================================
 * Screen Power Management (Fade In/Out + Keep Audio Playing!) & USB Mode
 * ========================================================================= */
static void enter_screen_off(void)
{
    if (g_screen_off) return;
    save_resume_state();
    g_screen_off = true;
    g_sel_held = false;
    g_sel_long_fired = false;

    fade_backlight(g_brightness, 0);
    y1_display_enter_early_suspend();
    y1_input_set_touch_scanning(false);

    struct player_status st;
    player_get_status(&st);
    if (st.playing) {
        /* Keep audio playing smoothly when screen turns off! */
        y1_power_set_idle_sleep(false);
        y1_audio_wake_lock(true);
        y1_cpu_set_screen_off_clamp(false);
        update_cpu_power_state(false, y1_bt_a2dp_is_streaming());
    } else {
        y1_cpu_set_screen_off_clamp(true);
        y1_power_set_idle_sleep(true);
        update_cpu_power_state(false, false);
    }
}

static void render_current_frame(const struct player_status *st);

static void exit_screen_off(const struct player_status *st)
{
    if (!g_screen_off) return;
    g_screen_off = false;
    g_last_activity_ms = now_ms();
    y1_power_set_idle_sleep(false);
    y1_cpu_set_screen_off_clamp(false);
    update_cpu_power_state(true, y1_bt_a2dp_is_streaming());
    y1_display_exit_early_suspend();
    y1_input_set_touch_scanning(true);
    if (st) render_current_frame(st);
    fade_backlight(0, g_brightness);
}

static void enter_usb_mode(void)
{
    if (g_usb_active) return;
    exit_screen_off(NULL);
    save_resume_state();
    g_ctx_open = false;
    g_vol_warn_open = false;
    g_sel_held = false;
    player_stop();
    y1_usb_set_ipod_mode(g_use_ipod_db != 0);
    y1_usb_attach_mass_storage();
    g_usb_active = true;
    g_last_activity_ms = now_ms();
}

static void exit_usb_mode(bool to_charge_only)
{
    if (!g_usb_active) return;
    y1_usb_detach_mass_storage();
    g_usb_active = false;
    g_usb_charge_only = to_charge_only;
    y1_usb_sync_clock_from_sdcard();
    reload_library();
    apply_base_low_power();
    g_last_activity_ms = now_ms();
    if (to_charge_only)
        show_toast(TR("Charge Only Mode", "충전 전용 모드"), 1400);
    else
        show_toast(TR("USB Disconnected", "USB 연결 해제됨"), 1200);
}

static void format_clock(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) { snprintf(buf, len, "--:--"); return; }
    int h12 = tm->tm_hour % 12;
    if (h12 == 0) h12 = 12;
    bool pm = tm->tm_hour >= 12;
    if (g_lang == 1)
        snprintf(buf, len, "%s %02d:%02d", pm ? "오후" : "오전", h12, tm->tm_min);
    else
        snprintf(buf, len, "%02d:%02d %s", h12, tm->tm_min, pm ? "PM" : "AM");
}

static void format_mmss(uint32_t ms, char *buf, size_t len)
{
    uint32_t sec = ms / 1000;
    snprintf(buf, len, "%u:%02u", sec / 60, sec % 60);
}

static void open_album_tracks(const char *album_name)
{
    snprintf(g_filter_title, sizeof(g_filter_title), "%s", album_name);
    reset_filter_list();
    g_filter_is_album = true;
    for (int i = 0; i < g_db.track_count; i++) {
        const char *a = g_db.tracks[i].album[0] ? g_db.tracks[i].album : "Unknown Album";
        if (strcasecmp(a, album_name) == 0) push_filter_index(i);
    }
    if (g_filter_count > 1)
        qsort(g_filter_indices, (size_t)g_filter_count, sizeof(int), cmp_filter_track_num);
    push_screen(SCR_FILTERED_SONGS);
}

static void launch_system_settings(void)
{
    save_resume_state();
    const char *paths[] = {
        "/mnt/sdcard/.system/configapp/configapp",
        "/.system/configapp/configapp",
        NULL
    };
    const char *exe = NULL;
    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0) { exe = paths[i]; break; }
    }
    if (!exe) {
        show_toast(TR("ConfigApp not found", "시스템 설정 앱을 찾을 수 없음"), 1400);
        return;
    }

    /* Close MeUI's input fds BEFORE launching ConfigApp so events inside ConfigApp
     * don't buffer in MeUI and cause an instant re-launch soft-lock on exit! */
    y1_input_close();
    g_sel_held = false;
    g_sel_long_fired = false;

    pid_t pid = fork();
    if (pid == 0) {
        char *const argv[] = { (char *)exe, NULL };
        execv(exe, argv);
        _exit(127);
    } else if (pid > 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }

    /* Re-open input and flush any residual key events */
    y1_input_init();
    y1_input_set_touch_scanning(true);
    struct y1_input_event flush_ev;
    while (y1_input_poll(&flush_ev, 0) > 0) {}

    load_prefs();
    apply_base_low_power();
    y1_backlight_set_percent(g_brightness);
    g_last_activity_ms = now_ms();
}

static void trigger_play_pause(void)
{
    struct player_status st;
    player_get_status(&st);
    if (!st.has_track && g_db.track_count > 0) {
        play_all_tracks(0);
    } else {
        player_toggle_pause();
    }
    save_resume_state();
}

/* =========================================================================
 * UI Helpers
 * ========================================================================= */
static void draw_toggle_switch(int scope, int idx, int x, int y, int w, int row_h, bool on, bool sel)
{
    float t = sw_anim_get(scope, idx, on, now_ms());
    int pw = 58, ph = 34;
    int px = x + w - 16 - pw;
    int py = y + (row_h - ph) / 2;
    gfx_fill_round_rect(px, py, pw, ph, 17, sel ? COL_SURFACE : COL_SURFACE_HI);
    int tx = px + (int)roundf(t * (float)(pw - 34));
    uint32_t thumb_col = lerp_argb(COL_WARN, COL_SUCCESS, t);
    gfx_fill_round_rect(tx, py, 34, 34, 14, thumb_col);
    gfx_draw_icon(t >= 0.5f ? "check" : "x", tx + 7, py + 7, 20, COL_SWITCH_ICON);
}

static const char *active_status_icon(const struct player_status *st)
{
    if (g_media_seek_fired) {
        if (g_media_held_btn == Y1_BTN_RIGHT) return "fast-forward";
        if (g_media_held_btn == Y1_BTN_LEFT)  return "rewind";
    }
    return (st && st->playing) ? "play" : "pause";
}

static void draw_header(const char *title, const char *icon_name, const struct player_status *st)
{
    gfx_reset_clip();
    gfx_fill_round_rect(8, 6, UI_W - 16, 48, 24, COL_SURFACE);

    char time_str[24];
    format_clock(time_str, sizeof(time_str));
    int time_w = gfx_text_width(time_str, 19.5f, UI_FONT_SEMIBOLD);
    int rx = UI_W - 18 - time_w;
    gfx_draw_text(rx, 19, time_str, 19.5f, UI_FONT_SEMIBOLD, COL_TEXT);

    char bat_str[16];
    snprintf(bat_str, sizeof(bat_str), "%d%%", g_cached_bat_pct);
    int bat_w = gfx_text_width(bat_str, 19.0f, UI_FONT_SEMIBOLD);
    int bat_pill_w = 28 + bat_w + 12;
    rx -= bat_pill_w + 6;
    gfx_fill_round_rect(rx, 13, bat_pill_w, 34, 17, COL_SURFACE_HI);
    gfx_draw_battery_icon(rx + 7, 20, g_cached_bat_pct, g_cached_bat_chg,
                          (g_cached_bat_pct <= 20 && !g_cached_bat_chg) ? COL_DANGER : COL_TEXT);
    gfx_draw_text(rx + 31, 19, bat_str, 19.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    if (g_cached_bt_conn) {
        rx -= 38;
        gfx_fill_round_rect(rx, 13, 34, 34, 17, COL_ACCENT);
        gfx_draw_icon("bluetooth", rx + 7, 20, 20, COL_ACCENT_TEXT);
    }

    if (st && st->has_track && g_screen != SCR_NOW_PLAYING) {
        rx -= 38;
        gfx_fill_round_rect(rx, 13, 34, 34, 17, st->playing ? COL_SUCCESS : COL_WARN);
        gfx_draw_icon(active_status_icon(st), rx + 7, 20, 20, COL_SWITCH_ICON);
    }

    uint32_t pill_bg = COL_ACCENT;
    uint32_t pill_fg = COL_ACCENT_TEXT;
    if (g_screen == SCR_NOW_PLAYING && st && st->has_track) {
        bool seeking = g_media_seek_fired &&
                       (g_media_held_btn == Y1_BTN_LEFT || g_media_held_btn == Y1_BTN_RIGHT);
        if (seeking) {
            pill_bg = COL_ACCENT;
            pill_fg = COL_ACCENT_TEXT;
        } else if (st->playing) {
            pill_bg = COL_SUCCESS;
            pill_fg = COL_SWITCH_ICON;
        } else {
            pill_bg = COL_WARN;
            pill_fg = COL_SWITCH_ICON;
        }
    }

    int max_pill_w = rx - 20;
    int title_w = gfx_text_width(title ? title : "", 24.0f, UI_FONT_SEMIBOLD);
    int pill_w = icon_name ? (40 + title_w + 14) : (16 + title_w + 16);
    if (pill_w > max_pill_w) pill_w = max_pill_w;
    if (pill_w < 64) pill_w = 64;
    gfx_fill_round_rect(12, 10, pill_w, 40, 20, pill_bg);
    if (icon_name) {
        gfx_draw_icon(icon_name, 21, 18, 24, pill_fg);
        gfx_draw_text_ellipsize(52, 16, pill_w - 52, title ? title : "", 24.0f, UI_FONT_SEMIBOLD, pill_fg);
    } else {
        gfx_draw_text_ellipsize(28, 16, pill_w - 32, title ? title : "", 24.0f, UI_FONT_SEMIBOLD, pill_fg);
    }
}

static void render_abc_badge(void)
{
    if (!g_abc_active || now_ms() >= g_abc_until_ms || !g_abc_label[0]) {
        g_abc_active = false;
        return;
    }
    gfx_reset_clip();
    int sz = 88;
    int bx = (UI_W - sz) / 2, by = (UI_H - sz) / 2;
    gfx_fill_round_rect(bx, by, sz, sz, 28, COL_ACCENT);
    gfx_stroke_round_rect(bx, by, sz, sz, 28, 3, COL_BORDER_HI);
    int tw = gfx_text_width(g_abc_label, 42.0f, UI_FONT_SEMIBOLD);
    gfx_draw_text(bx + (sz - tw) / 2, by + 20, g_abc_label, 42.0f, UI_FONT_SEMIBOLD, COL_ACCENT_TEXT);
}

/* =========================================================================
 * USB Mass Storage / iPod Mode Blocking Screen
 * ========================================================================= */
static void render_usb_screen(void)
{
    draw_header(g_use_ipod_db ? TR("iPod USB Mode", "iPod USB 모드")
                              : TR("USB Storage", "USB 저장소"),
                "usb", NULL);

    gfx_fill_round_rect(18, 66, UI_W - 36, UI_H - 80, 36, COL_SURFACE);
    gfx_stroke_round_rect(18, 66, UI_W - 36, UI_H - 80, 36, 2, COL_BORDER_HI);

    int badge = 92;
    int bx = (UI_W - badge) / 2;
    int by = 92;
    gfx_fill_round_rect(bx, by, badge, badge, 30, COL_ACCENT);
    gfx_draw_icon("usb", bx + (badge - 50) / 2, by + (badge - 50) / 2, 50, COL_ACCENT_TEXT);

    const char *title = g_use_ipod_db ? TR("iPod Mode Connected", "iPod 모드 연결됨")
                                      : TR("USB Mass Storage", "USB 대용량 저장소");
    int tw = gfx_text_width(title, 26.0f, UI_FONT_SEMIBOLD);
    gfx_draw_text((UI_W - tw) / 2, by + badge + 18, title, 26.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    const char *sub = TR("Do not disconnect while syncing", "동기화 중 케이블을 분리하지 마세요");
    int sw = gfx_text_width(sub, 19.0f, UI_FONT_REGULAR);
    gfx_draw_text((UI_W - sw) / 2, by + badge + 54, sub, 19.0f, UI_FONT_REGULAR, COL_TEXT_DIM);

    const char *hint = TR("Press any button for Charge Only", "아무 버튼이나 누르면 충전 전용 모드");
    int hw = gfx_text_width(hint, 19.5f, UI_FONT_SEMIBOLD);
    int pw = hw + 36;
    if (pw > UI_W - 60) pw = UI_W - 60;
    int px = (UI_W - pw) / 2;
    int py = UI_H - 68;
    gfx_fill_round_rect(px, py, pw, 42, 21, COL_SURFACE_HI);
    gfx_draw_text_ellipsize(px + 18, py + 10, pw - 36, hint, 19.5f, UI_FONT_SEMIBOLD, COL_TEXT);
}

/* =========================================================================
 * Home Screen (iPod 6G Split-Screen)
 * ========================================================================= */
struct home_entry {
    const char *en;
    const char *ko;
    const char *icon;
};

static const struct home_entry k_home_items[] = {
    { "Now Playing", "지금 재생 중",   "play" },
    { "Cover Flow",  "커버 플로우",    "layout-grid" },
    { "Songs",       "모든 노래",      "music" },
    { "Artists",     "가수별",         "user" },
    { "Albums",      "앨범별",         "disc-3" },
    { "Playlists",   "재생목록",       "list-music" },
    { "Photos",      "사진",           "images" },
    { "Search",      "검색",           "search" },
    { "Shuffle All", "전체 랜덤 재생", "shuffle" },
    { "Settings",    "설정",           "settings" },
};
#define NUM_HOME_ITEMS ((int)(sizeof(k_home_items) / sizeof(k_home_items[0])))

static void render_home(const struct player_status *st, int ox)
{
    gfx_set_text_shadow(false);
    draw_header("MeUI", NULL, st);

    int lx = 10 + ox, ly = 64, lw = 230;
    int row_h = 68, gap = 7, visible = 4;
    int step = row_h + gap;

    if (g_home_sel < g_home_scroll) g_home_scroll = g_home_sel;
    if (g_home_sel >= g_home_scroll + visible) g_home_scroll = g_home_sel - visible + 1;
    if (g_home_scroll < 0) g_home_scroll = 0;

    gfx_set_clip_y(62, UI_H - 4);
    uint64_t tnow = now_ms();
    gfx_set_text_shadow(false);
    for (int i = 0; i < NUM_HOME_ITEMS; i++) {
        int y = ly + (int)roundf(((float)i - g_home_scroll_anim) * (float)step);
        if (y + row_h < 62 || y > UI_H) continue;
        bool sel = (i == g_home_sel);
        float t = item_pill_get(SCR_HOME, i, sel, tnow);
        int r = (int)roundf(18.0f + t * (float)(row_h / 2 - 18));
        uint32_t bg = lerp_argb(COL_SURFACE, COL_SEL_BG, t);

        gfx_fill_round_rect(lx, y, lw, row_h, r, bg);
        if (sel) gfx_stroke_round_rect(lx, y, lw, row_h, r, 2, COL_BORDER_HI);

        const char *ic = (i == 0 && st->has_track) ? active_status_icon(st) : k_home_items[i].icon;
        uint32_t fg = sel ? COL_SEL_TEXT : COL_TEXT;
        gfx_draw_icon(ic, lx + 14, y + 20, 28, fg);
        gfx_draw_text_ellipsize(lx + 50, y + 19, lw - 76,
                                TR(k_home_items[i].en, k_home_items[i].ko),
                                26.0f, UI_FONT_SEMIBOLD, fg);
        if (sel) gfx_draw_icon("chevron-right", lx + lw - 28, y + 22, 24, fg);
    }
    gfx_set_text_shadow(false);
    gfx_reset_clip();
    gfx_draw_scrollbar(lx + lw + 4, ly, visible * step - gap, g_home_scroll_anim, visible, NUM_HOME_ITEMS);

    /* Right Pane: shown when media is loaded (even when paused), hidden when album art is wallpaper */
    if (!g_wp_album_art && st->has_track && st->track_idx >= 0 && st->track_idx < g_db.track_count) {
        const struct art_bitmap *art = album_art_get_for_track(&g_db.tracks[st->track_idx]);
        if (art) {
            int rx = 254 + ox, ry = 64, rw = UI_W - 254 - 10, rh = 286;
            gfx_blit_rgb565_squircle_pan(rx, ry, rw, rh, 34, art->rgb565, art->w, art->h,
                                         1.5f, g_art_pan_cur_x, g_art_pan_cur_y);
            gfx_stroke_round_rect(rx, ry, rw, rh, 34, 2, COL_BORDER);
        }
    }
}

/* =========================================================================
 * Cover Flow (Basic 3x2 Album Art Grid)
 * ========================================================================= */
static void render_coverflow(const struct player_status *st, int ox)
{
    draw_header(TR("Cover Flow", "커버 플로우"), "layout-grid", st);

    if (g_db.album_count == 0) {
        gfx_fill_round_rect(24 + ox, 110, UI_W - 48, 120, 34, COL_SURFACE);
        const char *msg = TR("No albums found", "앨범이 없습니다");
        int mw = gfx_text_width(msg, 24.0f, UI_FONT_SEMIBOLD);
        gfx_draw_text((UI_W - mw) / 2 + ox, 154, msg, 24.0f, UI_FONT_SEMIBOLD, COL_TEXT_DIM);
        return;
    }

    int cols = 3, visible_rows = 2;
    int total_rows = (g_db.album_count + cols - 1) / cols;
    int sel_row = g_list_sel / cols;
    if (sel_row < g_list_scroll) g_list_scroll = sel_row;
    if (sel_row >= g_list_scroll + visible_rows) g_list_scroll = sel_row - visible_rows + 1;
    if (g_list_scroll < 0) g_list_scroll = 0;

    int cw = 142, ch = 138, gap_x = 8, gap_y = 8;
    int top_y = 62, left_x = 12 + ox;
    int step_y = ch + gap_y;

    gfx_set_clip_y(60, UI_H - 4);
    int first_row = (int)floorf(g_list_scroll_anim) - 1;
    if (first_row < 0) first_row = 0;
    int first_idx = first_row * cols;
    int last_idx = (first_row + visible_rows + 3) * cols;
    if (last_idx > g_db.album_count) last_idx = g_db.album_count;
    uint64_t tnow = now_ms();
    for (int i = first_idx; i < last_idx; i++) {
        int r = i / cols;
        int c = i % cols;
        int x = left_x + c * (cw + gap_x);
        int y = top_y + (int)roundf(((float)r - g_list_scroll_anim) * (float)step_y);
        if (y + ch < 60 || y > UI_H) continue;
        bool sel = (i == g_list_sel);
        float t = item_pill_get(SCR_COVERFLOW, i, sel, tnow);
        /* Grid card: slightly round more on selection (24 -> 34), not a pill */
        int cr = (int)roundf(24.0f + t * 10.0f);
        uint32_t bg = lerp_argb(COL_SURFACE, COL_SEL_BG, t);

        gfx_fill_round_rect(x, y, cw, ch, cr, bg);
        if (sel) gfx_stroke_round_rect(x, y, cw, ch, cr, 2, COL_BORDER_HI);

        int ti = g_db.albums[i].first_track_idx;
        const struct art_bitmap *art = (ti >= 0 && ti < g_db.track_count) ? album_art_get_for_track(&g_db.tracks[ti]) : NULL;
        int asz = 92;
        int ax = x + (cw - asz) / 2;
        int ay = y + 10;
        if (art) {
            gfx_blit_rgb565_squircle(ax, ay, asz, asz, 20, art->rgb565, art->w, art->h);
        } else {
            gfx_fill_round_rect(ax, ay, asz, asz, 22, COL_ACCENT_SOFT);
            gfx_draw_icon("disc-3", ax + (asz - 44) / 2, ay + (asz - 44) / 2, 44, COL_TEXT_DIM);
        }
        gfx_draw_text_ellipsize(x + 10, ay + asz + 6, cw - 20, g_db.albums[i].name,
                                21.5f, UI_FONT_SEMIBOLD, COL_TEXT);
    }
    gfx_reset_clip();
    gfx_draw_scrollbar(UI_W - 10, top_y, visible_rows * step_y - gap_y, g_list_scroll_anim, visible_rows, total_rows);
}

/* =========================================================================
 * Full-Width Lists (Songs / Artists / Albums / Playlists / Queue / Settings / BT Submenus)
 * ========================================================================= */
#define NUM_SETTINGS_ITEMS  9
#define NUM_ADVANCED_ITEMS  8
#define NUM_BT_MENU_ITEMS   5

static int current_list_count(void)
{
    if (g_screen == SCR_COVERFLOW) return g_db.album_count;
    if (g_screen == SCR_PHOTOS) return photos_count();
    if (g_screen == SCR_SONGS) return g_db.track_count;
    if (g_screen == SCR_ARTISTS) return g_db.artist_count;
    if (g_screen == SCR_ALBUMS) return g_db.album_count;
    if (g_screen == SCR_PLAYLISTS) return g_db.playlist_count;
    if (g_screen == SCR_FILTERED_SONGS) return g_filter_count;
    if (g_screen == SCR_QUEUE) {
        struct player_status st;
        player_get_status(&st);
        return st.queue_len;
    }
    if (g_screen == SCR_SETTINGS) return NUM_SETTINGS_ITEMS;
    if (g_screen == SCR_ADVANCED_SETTINGS) return NUM_ADVANCED_ITEMS;
    if (g_screen == SCR_WALLPAPER || g_screen == SCR_FONT_PICKER) return g_wp_count;
    if (g_screen == SCR_BT_MENU) return NUM_BT_MENU_ITEMS;
    if (g_screen == SCR_BT_PAIRED || g_screen == SCR_BT_NEW || g_screen == SCR_BT_FORGET)
        return g_bt_dev_count;
    return 0;
}

static void render_list_screen(const struct player_status *st, int ox)
{
    if (g_screen == SCR_BT_PAIRED || g_screen == SCR_BT_NEW || g_screen == SCR_BT_FORGET)
        refresh_bt_sublist();

    const char *hdr = TR("Songs", "모든 노래");
    const char *ico = "music";
    if (g_screen == SCR_ARTISTS) { hdr = TR("Artists", "가수별"); ico = "user"; }
    else if (g_screen == SCR_ALBUMS) { hdr = TR("Albums", "앨범별"); ico = "disc-3"; }
    else if (g_screen == SCR_PLAYLISTS) { hdr = TR("Playlists", "재생목록"); ico = "list-music"; }
    else if (g_screen == SCR_FILTERED_SONGS) { hdr = g_filter_title; ico = "music"; }
    else if (g_screen == SCR_QUEUE) { hdr = TR("Queue", "재생 대기열"); ico = "list-ordered"; }
    else if (g_screen == SCR_SETTINGS) { hdr = TR("Settings", "설정"); ico = "settings"; }
    else if (g_screen == SCR_ADVANCED_SETTINGS) { hdr = TR("Advanced", "고급 설정"); ico = "sliders"; }
    else if (g_screen == SCR_WALLPAPER) { hdr = TR("Wallpaper", "배경화면"); ico = "image"; }
    else if (g_screen == SCR_FONT_PICKER) { hdr = TR("Font", "글꼴"); ico = "type"; }
    else if (g_screen == SCR_BT_MENU) { hdr = TR("Bluetooth", "블루투스"); ico = "bluetooth"; }
    else if (g_screen == SCR_BT_PAIRED) { hdr = TR("Paired Devices", "등록된 기기"); ico = "bluetooth"; }
    else if (g_screen == SCR_BT_NEW) { hdr = TR("New Devices", "새 기기 검색"); ico = "bluetooth"; }
    else if (g_screen == SCR_BT_FORGET) { hdr = TR("Forget Devices", "기기 삭제"); ico = "trash-2"; }

    draw_header(hdr, ico, st);

    int total = current_list_count();
    if (total == 0) {
        gfx_fill_round_rect(24 + ox, 110, UI_W - 48, 120, 34, COL_SURFACE);
        const char *msg = (g_screen == SCR_BT_NEW && y1_bt_is_scanning())
                          ? TR("Scanning for Bluetooth devices...", "블루투스 기기 검색 중...")
                          : TR("Nothing here yet", "항목이 없습니다");
        int mw = gfx_text_width(msg, 23.0f, UI_FONT_SEMIBOLD);
        gfx_draw_text((UI_W - mw) / 2 + ox, 154, msg, 23.0f, UI_FONT_SEMIBOLD, COL_TEXT_DIM);
        return;
    }

    int *q_buf = NULL;
    int q_n = 0;
    if (g_screen == SCR_QUEUE && st->queue_len > 0) {
        q_buf = malloc(sizeof(int) * (size_t)st->queue_len);
        if (q_buf) q_n = player_get_queue(q_buf, st->queue_len);
    }

    int visible = 4, row_h = 68, gap = 6, top_y = 60;
    int step = row_h + gap;
    if (g_list_sel < g_list_scroll) g_list_scroll = g_list_sel;
    if (g_list_sel >= g_list_scroll + visible) g_list_scroll = g_list_sel - visible + 1;
    if (g_list_scroll < 0) g_list_scroll = 0;

    int x = 10 + ox;
    int w = UI_W - 30;

    /* Compute ONLY visible index range (O(visible) instead of O(100,000)!) */
    int first_idx = (int)floorf(g_list_scroll_anim) - 1;
    if (first_idx < 0) first_idx = 0;
    int last_idx = first_idx + visible + 3;
    if (last_idx > total) last_idx = total;

    gfx_set_clip_y(58, UI_H - 4);
    uint64_t tnow = now_ms();
    for (int idx = first_idx; idx < last_idx; idx++) {
        int y = top_y + (int)roundf(((float)idx - g_list_scroll_anim) * (float)step);
        if (y + row_h < 58 || y > UI_H) continue;
        bool sel = (idx == g_list_sel);
        float t = item_pill_get((int)g_screen, idx, sel, tnow);
        int r = (int)roundf(18.0f + t * (float)(row_h / 2 - 18));
        uint32_t bg = lerp_argb(COL_SURFACE, COL_SEL_BG, t);
        gfx_fill_round_rect(x, y, w, row_h, r, bg);
        if (sel) gfx_stroke_round_rect(x, y, w, row_h, r, 2, COL_BORDER_HI);

        if (g_screen == SCR_SONGS || g_screen == SCR_FILTERED_SONGS || g_screen == SCR_QUEUE) {
            int ti = (g_screen == SCR_SONGS) ? idx :
                     (g_screen == SCR_FILTERED_SONGS) ? g_filter_indices[idx] :
                     (idx < q_n ? q_buf[idx] : 0);
            const struct ipod_track *tr = &g_db.tracks[ti];
            bool is_cur = (g_screen == SCR_QUEUE) ? (idx == st->queue_pos)
                                                  : (st->has_track && st->track_idx == ti);
            int text_x = x + 66;
            if (g_screen == SCR_FILTERED_SONGS && g_filter_is_album) {
                char tnum[16];
                int n = tr->track_num > 0 ? (int)tr->track_num : (idx + 1);
                snprintf(tnum, sizeof(tnum), "%d", n);
                int nw = gfx_text_width(tnum, 21.0f, UI_FONT_SEMIBOLD);
                gfx_draw_text(x + 12 + (32 - nw) / 2, y + 21, tnum, 21.0f, UI_FONT_SEMIBOLD,
                              is_cur ? COL_SUCCESS : COL_TEXT_DIM);
                text_x = x + 52;
            } else {
                const struct art_bitmap *art = album_art_get_for_track(tr);
                if (art) {
                    gfx_blit_rgb565_squircle(x + 11, y + 11, 46, 46, 12, art->rgb565, art->w, art->h);
                } else {
                    const char *row_ico = is_cur ? ((g_screen == SCR_QUEUE || st->playing) ? "play" : "pause") : "music";
                    gfx_draw_icon(row_ico, x + 20, y + 20, 28, is_cur ? COL_SUCCESS : COL_TEXT);
                }
            }
            int text_w = w - (text_x - x) - 82;
            gfx_draw_text_ellipsize(text_x, y + 7, text_w, tr->title, 24.5f, UI_FONT_SEMIBOLD, COL_TEXT);
            gfx_draw_text_ellipsize(text_x, y + 39, text_w, tr->artist, 19.0f, UI_FONT_REGULAR, COL_TEXT_DIM);
            if (is_track_fav(ti))
                gfx_draw_icon("heart", x + w - 76, y + 23, 20, COL_DANGER);
            if (tr->duration_ms > 0) {
                char dstr[16];
                format_mmss(tr->duration_ms, dstr, sizeof(dstr));
                int dw = gfx_text_width(dstr, 19.0f, UI_FONT_SEMIBOLD);
                gfx_draw_text(x + w - 14 - dw, y + 23, dstr, 19.0f, UI_FONT_SEMIBOLD, COL_TEXT_MUTED);
            }
        } else if (g_screen == SCR_ARTISTS || g_screen == SCR_ALBUMS) {
            const struct ipod_group *g = (g_screen == SCR_ARTISTS) ? &g_db.artists[idx] : &g_db.albums[idx];
            int text_x = x + 52;
            char sub[96] = "";
            if (g_screen == SCR_ALBUMS && g->first_track_idx >= 0 && g->first_track_idx < g_db.track_count) {
                const struct ipod_track *ft = &g_db.tracks[g->first_track_idx];
                const struct art_bitmap *art = album_art_get_for_track(ft);
                if (art) {
                    gfx_blit_rgb565_squircle(x + 11, y + 11, 46, 46, 12, art->rgb565, art->w, art->h);
                } else {
                    gfx_draw_icon(ico, x + 20, y + 20, 28, COL_TEXT);
                }
                text_x = x + 66;
                char ystr[16] = "";
                if (ft->year > 0) snprintf(ystr, sizeof(ystr), "%u", ft->year);
                format_meta_pair(sub, sizeof(sub), ft->genre, ystr);
            } else {
                gfx_draw_icon(ico, x + 14, y + 20, 28, COL_TEXT);
            }
            int text_w = w - (text_x - x) - 46;
            gfx_draw_text_ellipsize(text_x, y + 7, text_w, g->name, 24.5f, UI_FONT_SEMIBOLD, COL_TEXT);
            if (!sub[0])
                snprintf(sub, sizeof(sub), g_lang == 1 ? "%d곡" : "%d songs", g->track_count);
            gfx_draw_text_ellipsize(text_x, y + 39, text_w, sub, 19.0f, UI_FONT_REGULAR, COL_TEXT_DIM);
            gfx_draw_icon("chevron-right", x + w - 36, y + 22, 24, COL_TEXT);
        } else if (g_screen == SCR_PLAYLISTS) {
            const struct ipod_playlist *pl = &g_db.playlists[idx];
            gfx_draw_icon("list-music", x + 14, y + 20, 28, COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 7, w - 98, pl->name, 24.5f, UI_FONT_SEMIBOLD, COL_TEXT);
            char sub[32];
            snprintf(sub, sizeof(sub), g_lang == 1 ? "%d곡" : "%d songs", pl->track_count);
            gfx_draw_text(x + 52, y + 39, sub, 19.0f, UI_FONT_REGULAR, COL_TEXT_DIM);
            gfx_draw_icon("chevron-right", x + w - 36, y + 22, 24, COL_TEXT);
        } else if (g_screen == SCR_BT_PAIRED || g_screen == SCR_BT_NEW || g_screen == SCR_BT_FORGET) {
            const struct y1_bt_device *dev = &g_bt_devs[idx];
            const char *name = dev->name[0] ? dev->name : TR("Unknown Device", "알 수 없는 기기");
            gfx_draw_icon(g_screen == SCR_BT_FORGET ? "trash-2" : "bluetooth",
                          x + 14, y + 20, 28, g_screen == SCR_BT_FORGET ? COL_DANGER : COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 7, w - 170, name, 24.0f, UI_FONT_SEMIBOLD, COL_TEXT);
            char sub[48];
            snprintf(sub, sizeof(sub), "%02X:%02X:%02X:%02X:%02X:%02X",
                     dev->bdaddr[5], dev->bdaddr[4], dev->bdaddr[3],
                     dev->bdaddr[2], dev->bdaddr[1], dev->bdaddr[0]);
            gfx_draw_text(x + 52, y + 39, sub, 18.5f, UI_FONT_REGULAR, COL_TEXT_DIM);

            const char *badge = (g_screen == SCR_BT_FORGET) ? TR("Forget", "삭제")
                              : (dev->connected ? TR("Disconnect", "연결 해제") : TR("Connect", "연결"));
            uint32_t bcol = (g_screen == SCR_BT_FORGET) ? COL_DANGER
                          : (dev->connected ? COL_WARN : COL_ACCENT);
            uint32_t bfg  = (g_screen == SCR_BT_FORGET) ? 0xFFFFFFFFU
                          : (dev->connected ? COL_TEXT : COL_ACCENT_TEXT);
            int bw = gfx_text_width(badge, 19.5f, UI_FONT_SEMIBOLD) + 26;
            gfx_fill_round_rect(x + w - 14 - bw, y + 15, bw, 38, 19, bcol);
            gfx_draw_text(x + w - 14 - bw + 13, y + 21, badge, 19.5f, UI_FONT_SEMIBOLD, bfg);
        } else if (g_screen == SCR_WALLPAPER || g_screen == SCR_FONT_PICKER) {
            const struct wp_entry *we = &g_wp_items[idx];
            const char *wic = we->is_clear ? "trash-2" :
                              (we->is_album_art ? "disc-3" :
                               (we->is_dir ? "folder" : (g_screen == SCR_FONT_PICKER ? "type" : "image")));
            uint32_t wcol = we->is_clear ? COL_DANGER : COL_TEXT;
            gfx_draw_icon(wic, x + 14, y + 20, 28, wcol);
            gfx_draw_text_ellipsize(x + 52, y + 19, w - 110, we->name, 24.5f, UI_FONT_SEMIBOLD, wcol);
            if (we->is_album_art)
                draw_toggle_switch((int)g_screen, idx, x, y, w, row_h, g_wp_album_art != 0, sel);
            else if (we->is_dir)
                gfx_draw_icon("chevron-right", x + w - 36, y + 22, 24, COL_TEXT);
        } else if (g_screen == SCR_SETTINGS || g_screen == SCR_ADVANCED_SETTINGS || g_screen == SCR_BT_MENU) {
            const char *lbl = "";
            char val[48] = "";
            const char *sico = "settings";
            bool is_toggle = false;
            bool toggle_val = false;
            bool is_danger = false;

            if (g_screen == SCR_SETTINGS) {
                switch (idx) {
                    case 0:
                        lbl = TR("Brightness", "화면 밝기");
                        snprintf(val, sizeof(val), "%d%%", g_brightness);
                        sico = "sun";
                        break;
                    case 1:
                        lbl = TR("Bluetooth", "블루투스");
                        sico = "bluetooth";
                        break;
                    case 2:
                        lbl = TR("Wallpaper", "배경화면");
                        sico = "image";
                        break;
                    case 3:
                        lbl = TR("Dark Mode", "다크 모드");
                        is_toggle = true; toggle_val = (g_dark_mode != 0);
                        sico = "moon";
                        break;
                    case 4:
                        lbl = TR("Shuffle", "랜덤 재생");
                        is_toggle = true; toggle_val = st->shuffle;
                        sico = "shuffle";
                        break;
                    case 5:
                        lbl = TR("Repeat", "반복 재생");
                        snprintf(val, sizeof(val), "%s",
                                 st->repeat == REPEAT_ONE ? TR("One", "한 곡")
                                 : (st->repeat == REPEAT_ALL ? TR("All", "전체") : TR("Off", "꺼짐")));
                        sico = st->repeat == REPEAT_ONE ? "repeat-1" : "repeat";
                        break;
                    case 6:
                        lbl = TR("Language", "언어");
                        snprintf(val, sizeof(val), "%s", g_lang == 1 ? "한국어" : "English");
                        sico = "globe";
                        break;
                    case 7:
                        lbl = TR("Advanced", "고급 설정");
                        sico = "sliders";
                        break;
                    case 8:
                        lbl = TR("Power Off", "전원 끄기");
                        sico = "power";
                        is_danger = true;
                        break;
                }
            } else if (g_screen == SCR_BT_MENU) {
                switch (idx) {
                    case 0:
                        lbl = TR("Bluetooth", "블루투스 전원");
                        is_toggle = true; toggle_val = y1_bt_is_enabled();
                        sico = "bluetooth";
                        break;
                    case 1:
                        lbl = TR("Receiver Mode", "블루투스 수신 모드");
                        is_toggle = true; toggle_val = y1_bt_is_receiver_mode();
                        sico = "radio";
                        break;
                    case 2:
                        lbl = TR("Paired Devices", "등록된 기기");
                        sico = "bluetooth-connected";
                        break;
                    case 3:
                        lbl = TR("New Devices", "새 기기 검색");
                        sico = "refresh-cw";
                        break;
                    case 4:
                        lbl = TR("Forget Devices", "기기 삭제");
                        sico = "trash-2";
                        is_danger = true;
                        break;
                }
            } else {
                switch (idx) {
                    case 0:
                        lbl = TR("Safe Volume", "안전 볼륨 (60dB)");
                        is_toggle = true; toggle_val = (g_safe_vol_enabled != 0);
                        sico = "volume-2";
                        break;
                    case 1:
                        lbl = TR("Music Source", "음악 소스");
                        snprintf(val, sizeof(val), "%s", g_use_ipod_db ? "iPod DB" : TR("Files", "일반 파일"));
                        sico = "hard-drive";
                        break;
                    case 2:
                        lbl = TR("Screen Timeout", "화면 꺼짐 시간");
                        if (g_screen_timeout_sec == 0) snprintf(val, sizeof(val), "%s", TR("Never", "안 함"));
                        else snprintf(val, sizeof(val), "%ds", g_screen_timeout_sec);
                        sico = "clock";
                        break;
                    case 3:
                        lbl = TR("Volume Step", "볼륨 단위");
                        snprintf(val, sizeof(val), "%d%%", g_vol_step);
                        sico = "volume-2";
                        break;
                    case 4:
                        lbl = TR("Seek Step", "탐색 단위");
                        snprintf(val, sizeof(val), "%ds", g_seek_sec);
                        sico = "skip-forward";
                        break;
                    case 5:
                        lbl = TR("Font", "글꼴");
                        sico = "type";
                        break;
                    case 6:
                        lbl = TR("Rescan Library", "음악 다시 불러오기");
                        sico = "refresh-cw";
                        break;
                    case 7:
                        lbl = TR("System Settings", "시스템 설정");
                        sico = "sliders";
                        break;
                }
            }

            gfx_draw_icon(sico, x + 14, y + 20, 28, is_danger ? COL_DANGER : COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 19, w - 170, lbl, 24.5f, UI_FONT_SEMIBOLD,
                                    is_danger ? COL_DANGER : COL_TEXT);
            if (is_toggle) {
                draw_toggle_switch((int)g_screen, idx, x, y, w, row_h, toggle_val, sel);
            } else if (val[0]) {
                int vw = gfx_text_width(val, 20.0f, UI_FONT_SEMIBOLD);
                int bw = vw + 26;
                gfx_fill_round_rect(x + w - 14 - bw, y + 15, bw, 38, 19, COL_ACCENT);
                gfx_draw_text(x + w - 14 - bw + 13, y + 21, val, 20.0f, UI_FONT_SEMIBOLD, COL_ACCENT_TEXT);
            } else {
                gfx_draw_icon("chevron-right", x + w - 36, y + 22, 24, is_danger ? COL_DANGER : COL_TEXT);
            }
        }
    }
    gfx_reset_clip();
    gfx_draw_scrollbar(UI_W - 12, top_y, visible * step - gap, g_list_scroll_anim, visible, total);
    free(q_buf);
}

/* =========================================================================
 * Now Playing Screen
 * ========================================================================= */
static void render_now_playing(const struct player_status *st, int ox)
{
    gfx_set_text_shadow(false);
    const char *hdr_title = TR("Now Playing", "지금 재생 중");
    const char *hdr_ico = active_status_icon(st);
    if (st && st->has_track) {
        if (g_media_seek_fired && (g_media_held_btn == Y1_BTN_LEFT || g_media_held_btn == Y1_BTN_RIGHT)) {
            hdr_title = TR("Seeking", "탐색 중");
            hdr_ico = (g_media_held_btn == Y1_BTN_RIGHT) ? "chevron-right" : "chevron-left";
        } else if (st->playing) {
            hdr_title = TR("Playing", "재생 중");
            hdr_ico = "play";
        } else {
            hdr_title = TR("Paused", "일시정지");
            hdr_ico = "pause";
        }
    }
    draw_header(hdr_title, hdr_ico, st);

    if (!st->has_track || st->track_idx < 0 || st->track_idx >= g_db.track_count) {
        gfx_set_text_shadow(gfx_has_wallpaper() && g_bg_blur_anim < 0.5f);
        const char *msg = TR("Pick a song to start listening", "노래를 선택해 재생하세요");
        int mw = gfx_text_width(msg, 24.0f, UI_FONT_SEMIBOLD);
        gfx_draw_text((UI_W - mw) / 2 + ox, 158, msg, 24.0f, UI_FONT_SEMIBOLD, COL_TEXT_DIM);
        gfx_set_text_shadow(false);
        return;
    }

    const struct ipod_track *tr = &g_db.tracks[st->track_idx];
    int tx = 26 + ox;
    int tw = UI_W - 52;
    if (!g_wp_album_art) {
        const struct art_bitmap *art = album_art_get_for_track(tr);
        int ax = 26 + ox, ay = 94, asz = 168;
        if (art) {
            gfx_blit_rgb565_squircle(ax, ay, asz, asz, 28, art->rgb565, art->w, art->h);
        } else {
            gfx_fill_round_rect(ax, ay, asz, asz, 32, COL_SURFACE_HI);
            gfx_draw_icon("music", ax + (asz - 64) / 2, ay + (asz - 64) / 2, 64, COL_ACCENT_TEXT);
        }
        tx = ax + asz + 16;
        tw = (12 + ox + UI_W - 24) - 16 - tx;
    }

    gfx_set_text_shadow(gfx_has_wallpaper() && g_bg_blur_anim < 0.5f);

    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d / %d", st->queue_pos + 1, st->queue_len);
    gfx_draw_text(tx, 112, cnt, 21.5f, UI_FONT_SEMIBOLD, COL_TEXT_MUTED);
    if (is_track_fav(st->track_idx))
        gfx_draw_icon("heart", tx + tw - 28, 110, 26, COL_DANGER);

    char ystr[16] = "", artist_line[160], album_line[128];
    if (tr->year > 0) snprintf(ystr, sizeof(ystr), "%u", tr->year);
    format_meta_pair(artist_line, sizeof(artist_line), tr->artist, tr->genre);
    format_meta_pair(album_line, sizeof(album_line), tr->album, ystr);

    gfx_draw_text_ellipsize(tx, 144, tw, tr->title, 30.0f, UI_FONT_SEMIBOLD, COL_TEXT);
    gfx_draw_text_ellipsize(tx, 184, tw, artist_line, 24.5f, UI_FONT_REGULAR, COL_TEXT_DIM);
    gfx_draw_text_ellipsize(tx, 218, tw, album_line, 21.5f, UI_FONT_REGULAR, COL_TEXT_MUTED);

    int bx = 26 + ox, by = 294, bw = UI_W - 52, bh = 18;
    if (now_ms() < g_vol_hud_until_ms) {
        gfx_fill_round_rect(bx, by, bw, bh, 9, COL_SURFACE_HI);
        int vw = (bw * st->volume_pct) / 100;
        uint32_t vcol = (st->volume_pct > SAFE_VOL_LIMIT) ? COL_DANGER : COL_SUCCESS;
        if (vw > 0) gfx_fill_round_rect(bx, by, vw < 18 ? 18 : vw, bh, 9, vcol);
        char vstr[32];
        snprintf(vstr, sizeof(vstr), "%s %d%%", TR("Volume", "볼륨"), st->volume_pct);
        gfx_draw_icon("volume-2", bx, by + 24, 24, vcol);
        gfx_draw_text(bx + 30, by + 23, vstr, 22.5f, UI_FONT_SEMIBOLD, COL_TEXT);
    } else {
        gfx_fill_round_rect(bx, by, bw, bh, 9, COL_SURFACE_HI);
        int fill_w = (st->dur_ms > 0) ? (int)(((uint64_t)st->pos_ms * (uint64_t)bw) / st->dur_ms) : 0;
        if (fill_w > bw) fill_w = bw;
        if (fill_w > 0) gfx_fill_round_rect(bx, by, fill_w < 18 ? 18 : fill_w, bh, 9, COL_ACCENT);

        char t_cur[16], t_rem[16], rem_lbl[20];
        format_mmss(st->pos_ms, t_cur, sizeof(t_cur));
        uint32_t rem = (st->dur_ms > st->pos_ms) ? (st->dur_ms - st->pos_ms) : 0;
        format_mmss(rem, t_rem, sizeof(t_rem));
        snprintf(rem_lbl, sizeof(rem_lbl), "-%s", t_rem);

        gfx_draw_text(bx, by + 24, t_cur, 22.5f, UI_FONT_SEMIBOLD, COL_TEXT);
        int rw = gfx_text_width(rem_lbl, 22.5f, UI_FONT_SEMIBOLD);
        gfx_draw_text(bx + bw - rw, by + 24, rem_lbl, 22.5f, UI_FONT_SEMIBOLD, COL_TEXT_DIM);
    }
    gfx_set_text_shadow(false);
}

/* =========================================================================
 * Modals & Full Frame Renderer
 * ========================================================================= */
static void render_vol_warn_modal(void)
{
    if (!g_vol_warn_open) return;
    gfx_reset_clip();
    gfx_set_text_shadow(false);

    int mw = 440, mh = 256;
    int mx = (UI_W - mw) / 2, my = (UI_H - mh) / 2;
    gfx_fill_round_rect(mx, my, mw, mh, 34, COL_MODAL_BG);
    gfx_stroke_round_rect(mx, my, mw, mh, 34, 2, COL_DANGER);

    gfx_fill_round_rect(mx + 20, my + 18, 52, 52, 18, COL_DANGER);
    gfx_draw_icon("volume-2", mx + 32, my + 30, 28, 0xFFFFFFFFU);

    gfx_draw_text_ellipsize(mx + 84, my + 20, mw - 104,
                            TR("High Volume Warning", "높은 볼륨 주의"),
                            26.0f, UI_FONT_SEMIBOLD, COL_DANGER);
    gfx_draw_text_ellipsize(mx + 84, my + 52, mw - 104,
                            TR("Above 60 dB safe limit", "안전 기준(60dB) 초과"),
                            21.0f, UI_FONT_REGULAR, COL_TEXT_DIM);

    gfx_draw_text_ellipsize(mx + 22, my + 92, mw - 44,
                            TR("Listening at high volume may harm your", "큰 볼륨으로 오래 들으면 청력이 손상될 수"),
                            22.0f, UI_FONT_REGULAR, COL_TEXT);
    gfx_draw_text_ellipsize(mx + 22, my + 122, mw - 44,
                            TR("hearing. Raise volume anyway?", "있습니다. 볼륨을 더 높이시겠습니까?"),
                            22.0f, UI_FONT_REGULAR, COL_TEXT);

    int bw = (mw - 56) / 2, bh = 56;
    int by = my + mh - bh - 20;
    int bx0 = mx + 20, bx1 = bx0 + bw + 16;

    gfx_fill_round_rect(bx0, by, bw, bh, bh / 2, g_vol_warn_sel == 0 ? COL_SEL_BG : COL_MODAL_HI);
    if (g_vol_warn_sel == 0) gfx_stroke_round_rect(bx0, by, bw, bh, bh / 2, 2, COL_BORDER_HI);
    const char *no_lbl = TR("No", "아니요");
    int nw = gfx_text_width(no_lbl, 24.0f, UI_FONT_SEMIBOLD);
    gfx_draw_text(bx0 + (bw - nw) / 2, by + 14, no_lbl, 24.0f, UI_FONT_SEMIBOLD,
                  g_vol_warn_sel == 0 ? COL_SEL_TEXT : COL_TEXT);

    gfx_fill_round_rect(bx1, by, bw, bh, bh / 2, g_vol_warn_sel == 1 ? COL_DANGER : COL_MODAL_HI);
    if (g_vol_warn_sel == 1) gfx_stroke_round_rect(bx1, by, bw, bh, bh / 2, 2, COL_BORDER_HI);
    const char *yes_lbl = TR("Yes", "예");
    int yw = gfx_text_width(yes_lbl, 24.0f, UI_FONT_SEMIBOLD);
    gfx_draw_text(bx1 + (bw - yw) / 2, by + 14, yes_lbl, 24.0f, UI_FONT_SEMIBOLD,
                  g_vol_warn_sel == 1 ? 0xFFFFFFFFU : COL_TEXT);
}

static void render_context_menu(const struct player_status *st)
{
    if (!g_ctx_open || g_ctx_track_idx < 0 || g_ctx_track_idx >= g_db.track_count) return;
    const struct ipod_track *tr = &g_db.tracks[g_ctx_track_idx];
    bool is_fav = is_track_fav(g_ctx_track_idx);
    int n_items = ctx_item_count();

    int mw = 432, mh = 344;
    int rh = 44, gap = 4, visible = 6;
    int step = rh + gap;
    int mx = (UI_W - mw) / 2, my = (UI_H - mh) / 2;
    gfx_reset_clip();
    gfx_set_text_shadow(false);
    gfx_fill_round_rect(mx, my, mw, mh, 34, COL_MODAL_BG);
    gfx_stroke_round_rect(mx, my, mw, mh, 34, 2, COL_BORDER_HI);

    gfx_draw_text_ellipsize(mx + 22, my + 12, mw - 44, tr->title, 25.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    if (g_ctx_sel < g_ctx_scroll) g_ctx_scroll = g_ctx_sel;
    if (g_ctx_sel >= g_ctx_scroll + visible) g_ctx_scroll = g_ctx_sel - visible + 1;
    if (g_ctx_scroll < 0) g_ctx_scroll = 0;
    int max_sc = n_items - visible;
    if (max_sc < 0) max_sc = 0;
    if (g_ctx_scroll > max_sc) g_ctx_scroll = max_sc;

    char shuf_en[32], shuf_ko[32], rep_en[32], rep_ko[32];
    bool shuf = st ? st->shuffle : false;
    enum repeat_mode rep = st ? st->repeat : REPEAT_ALL;
    snprintf(shuf_en, sizeof(shuf_en), "Shuffle: %s", shuf ? "ON" : "OFF");
    snprintf(shuf_ko, sizeof(shuf_ko), "셔플: %s", shuf ? "켜짐" : "꺼짐");
    const char *rn_en = (rep == REPEAT_OFF) ? "Off" : (rep == REPEAT_ONE) ? "One" : "All";
    const char *rn_ko = (rep == REPEAT_OFF) ? "끄기" : (rep == REPEAT_ONE) ? "한 곡" : "전체";
    snprintf(rep_en, sizeof(rep_en), "Repeat: %s", rn_en);
    snprintf(rep_ko, sizeof(rep_ko), "반복: %s", rn_ko);

    const char *labels_en[8] = {
        "Play Next", "Add to Queue", "View Queue", "Add to Playlist...",
        is_fav ? "Unfavorite Song" : "Favorite Song", "Go to Album",
        shuf_en, rep_en
    };
    const char *labels_ko[8] = {
        "다음에 재생", "대기열에 추가", "재생 대기열 보기", "재생목록에 추가...",
        is_fav ? "즐겨찾기 해제" : "즐겨찾기", "앨범으로 이동",
        shuf_ko, rep_ko
    };
    const char *icons[8] = {
        "skip-forward", "list-plus", "list-ordered", "list-music", "heart", "disc-3",
        "shuffle", (rep == REPEAT_ONE) ? "repeat-1" : "repeat"
    };
    const char *q_en[6] = {
        "Remove from Queue", "Move Up", "Move Down", "Play Now",
        is_fav ? "Unfavorite Song" : "Favorite Song", "Clear Queue"
    };
    const char *q_ko[6] = {
        "대기열에서 제거", "위로 이동", "아래로 이동", "지금 재생",
        is_fav ? "즐겨찾기 해제" : "즐겨찾기", "대기열 비우기"
    };
    const char *q_icons[6] = {
        "trash-2", "arrow-up", "arrow-down", "play", "heart", "x"
    };
    const char **use_en = g_ctx_from_queue ? q_en : labels_en;
    const char **use_ko = g_ctx_from_queue ? q_ko : labels_ko;
    const char **use_ic = g_ctx_from_queue ? q_icons : icons;

    int ry = my + 48;
    int list_h = visible * step - gap;
    int rw = (n_items > visible) ? (mw - 34) : (mw - 28);
    gfx_set_clip_y(ry, ry + list_h);
    for (int i = 0; i < n_items; i++) {
        int y = ry + (int)roundf(((float)i - g_ctx_scroll_anim) * (float)step);
        if (y + rh <= ry || y >= ry + list_h) continue;
        bool sel = (i == g_ctx_sel);
        gfx_fill_round_rect(mx + 14, y, rw, rh, sel ? rh / 2 : 18, sel ? COL_SEL_BG : COL_MODAL_HI);
        if (sel) gfx_stroke_round_rect(mx + 14, y, rw, rh, rh / 2, 2, COL_BORDER_HI);
        uint32_t fg = sel ? COL_SEL_TEXT : COL_TEXT;
        gfx_draw_icon(use_ic[i], mx + 26, y + 9, 26, fg);
        gfx_draw_text_ellipsize(mx + 60, y + 9, rw - 46, TR(use_en[i], use_ko[i]),
                                24.0f, UI_FONT_SEMIBOLD, fg);
    }
    gfx_reset_clip();
    if (n_items > visible)
        gfx_draw_scrollbar(mx + mw - 14, ry, list_h, g_ctx_scroll_anim, visible, n_items);
}

static void render_toast(void)
{
    if (!g_toast[0] || now_ms() >= g_toast_until_ms) return;
    gfx_reset_clip();
    gfx_set_text_shadow(false);
    int tw = gfx_text_width(g_toast, 23.0f, UI_FONT_SEMIBOLD);
    int pw = tw + 44;
    if (pw > UI_W - 24) pw = UI_W - 24;
    int px = (UI_W - pw) / 2, py = UI_H - 60;
    gfx_fill_round_rect(px, py, pw, 48, 24, COL_MODAL_BG);
    gfx_stroke_round_rect(px, py, pw, 48, 24, 2, COL_MODAL_BORDER);
    gfx_draw_text_ellipsize(px + 22, py + 11, pw - 44, g_toast, 23.0f, UI_FONT_SEMIBOLD, COL_TEXT);
}

static const char *const k_search_keys[4][40] = {
    {
        "1","2","3","4","5","6","7","8","9","0",
        "q","w","e","r","t","y","u","i","o","p",
        "a","s","d","f","g","h","j","k","l","DEL",
        "ABC","z","x","c","v","b","n","m","SPC","GO"
    },
    {
        "1","2","3","4","5","6","7","8","9","0",
        "Q","W","E","R","T","Y","U","I","O","P",
        "A","S","D","F","G","H","J","K","L","DEL",
        "한글","Z","X","C","V","B","N","M","SPC","GO"
    },
    {
        "1","2","3","4","5","6","7","8","9","0",
        "ㅂ","ㅈ","ㄷ","ㄱ","ㅅ","ㅛ","ㅕ","ㅑ","ㅐ","ㅔ",
        "ㅁ","ㄴ","ㅇ","ㄹ","ㅎ","ㅗ","ㅓ","ㅏ","ㅣ","DEL",
        "쌍자","ㅋ","ㅌ","ㅊ","ㅍ","ㅠ","ㅜ","ㅡ","SPC","GO"
    },
    {
        "1","2","3","4","5","6","7","8","9","0",
        "ㅃ","ㅉ","ㄸ","ㄲ","ㅆ","ㅛ","ㅕ","ㅑ","ㅒ","ㅖ",
        "ㅁ","ㄴ","ㅇ","ㄹ","ㅎ","ㅗ","ㅓ","ㅏ","ㅣ","DEL",
        "abc","ㅋ","ㅌ","ㅊ","ㅍ","ㅠ","ㅜ","ㅡ","SPC","GO"
    }
};

static bool is_search_fn_key(const char *lbl)
{
    return (strcmp(lbl, "DEL") == 0 || strcmp(lbl, "SPC") == 0 || strcmp(lbl, "GO") == 0 ||
            strcmp(lbl, "ABC") == 0 || strcmp(lbl, "abc") == 0 ||
            strcmp(lbl, "한글") == 0 || strcmp(lbl, "쌍자") == 0);
}

static bool ci_contains(const char *hay, const char *needle)
{
    if (!needle || !*needle) return true;
    if (!hay || !*hay) return false;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < nlen && p[k]) {
            char a = p[k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            k++;
        }
        if (k == nlen) return true;
    }
    return false;
}

static void execute_search_query(void)
{
    hime_commit(&g_search_ime, g_search_buf);
    g_search_len = (int)strlen(g_search_buf);
    if (g_search_len == 0) {
        show_toast(TR("Type a search term", "검색어를 입력하세요"), 1100);
        return;
    }
    snprintf(g_filter_title, sizeof(g_filter_title), "%s: %s", TR("Search", "검색"), g_search_buf);
    reset_filter_list();
    for (int i = 0; i < g_db.track_count; i++) {
        if (ci_contains(g_db.tracks[i].title, g_search_buf) ||
            ci_contains(g_db.tracks[i].artist, g_search_buf) ||
            ci_contains(g_db.tracks[i].album, g_search_buf)) {
            push_filter_index(i);
        }
    }
    if (g_filter_count == 0) {
        show_toast(TR("No matches found", "검색 결과 없음"), 1200);
        return;
    }
    push_screen(SCR_FILTERED_SONGS);
}

static void render_search_screen(const struct player_status *st, int ox)
{
    draw_header(TR("Search", "검색"), "search", st);

    gfx_fill_round_rect(12 + ox, 62, UI_W - 24, 288, 32, COL_MODAL_BG);
    gfx_stroke_round_rect(12 + ox, 62, UI_W - 24, 288, 32, 2, COL_MODAL_BORDER);

    gfx_fill_round_rect(24 + ox, 76, UI_W - 48, 48, 18, COL_MODAL_HI);
    if (g_search_len > 0) {
        gfx_draw_text_ellipsize(38 + ox, 86, UI_W - 76, g_search_buf, 24.0f, UI_FONT_SEMIBOLD, COL_TEXT);
    } else {
        gfx_draw_text_ellipsize(38 + ox, 88, UI_W - 76,
                                TR("Scroll/Arrows + Select, GO to search", "글자를 선택하고 GO를 누르세요"),
                                20.0f, UI_FONT_REGULAR, COL_TEXT_MUTED);
    }

    int gx = 22 + ox, gy = 136, key_w = 40, key_h = 46, gap = 4;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            int idx = r * 10 + c;
            int kx = gx + c * (key_w + gap);
            int ky = gy + r * (key_h + gap);
            bool sel = (idx == g_search_sel);
            const char *lbl = k_search_keys[g_search_mode][idx];
            bool is_go = (strcmp(lbl, "GO") == 0);
            bool is_fn = is_search_fn_key(lbl);

            uint32_t bg = sel ? COL_SEL_BG : (is_go ? COL_SUCCESS : (is_fn ? COL_MODAL_HI : COL_KEY_BG));
            uint32_t fg = sel ? COL_SEL_TEXT : (is_go ? COL_SWITCH_ICON : COL_TEXT);
            int rad = (sel || is_go) ? 16 : 12;
            gfx_fill_round_rect(kx, ky, key_w, key_h, rad, bg);
            if (sel) gfx_stroke_round_rect(kx, ky, key_w, key_h, rad, 2, COL_BORDER_HI);

            float fsz = (is_fn && !is_go) ? 14.5f : (is_go ? 17.5f : 22.0f);
            int tw = gfx_text_width(lbl, fsz, UI_FONT_SEMIBOLD);
            int tx = kx + (key_w - tw) / 2;
            int ty = ky + ((is_fn && !is_go) ? 15 : (is_go ? 13 : 11));
            gfx_draw_text(tx, ty, lbl, fsz, UI_FONT_SEMIBOLD, fg);
        }
    }
}

static void open_playlist_picker(int track_idx)
{
    g_pl_pick_track_idx = track_idx;
    g_pl_pick_sel = 0;
    g_pl_pick_scroll = 0;
    g_pl_pick_count = 0;

    bool has_fav = false;
    DIR *d = opendir("/mnt/sdcard/Playlists");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL && g_pl_pick_count < PL_PICK_MAX - 1) {
            if (de->d_name[0] == '.') continue;
            size_t nl = strlen(de->d_name);
            bool is_m3u = (nl >= 4 && strcasecmp(de->d_name + nl - 4, ".m3u") == 0);
            bool is_m3u8 = (nl >= 5 && strcasecmp(de->d_name + nl - 5, ".m3u8") == 0);
            if (!is_m3u && !is_m3u8) continue;
            snprintf(g_pl_pick_paths[g_pl_pick_count], sizeof(g_pl_pick_paths[0]),
                     "/mnt/sdcard/Playlists/%s", de->d_name);
            snprintf(g_pl_pick_names[g_pl_pick_count], sizeof(g_pl_pick_names[0]), "%s", de->d_name);
            char *dot = strrchr(g_pl_pick_names[g_pl_pick_count], '.');
            if (dot) *dot = '\0';
            if (strcasecmp(de->d_name, "MeUI_Favorites.m3u") == 0) has_fav = true;
            g_pl_pick_count++;
        }
        closedir(d);
    }
    if (!has_fav && g_pl_pick_count < PL_PICK_MAX) {
        snprintf(g_pl_pick_names[g_pl_pick_count], sizeof(g_pl_pick_names[0]), "MeUI_Favorites");
        snprintf(g_pl_pick_paths[g_pl_pick_count], sizeof(g_pl_pick_paths[0]),
                 "/mnt/sdcard/Playlists/MeUI_Favorites.m3u");
        g_pl_pick_count++;
    }
    g_pl_pick_open = (g_pl_pick_count > 0);
}

static void render_playlist_picker(void)
{
    if (!g_pl_pick_open || g_pl_pick_count <= 0) return;
    int mw = 420, mh = 304;
    int mx = (UI_W - mw) / 2, my = (UI_H - mh) / 2;
    gfx_reset_clip();
    gfx_set_text_shadow(false);
    gfx_fill_round_rect(mx, my, mw, mh, 34, COL_MODAL_BG);
    gfx_stroke_round_rect(mx, my, mw, mh, 34, 2, COL_BORDER_HI);
    gfx_draw_text_ellipsize(mx + 22, my + 12, mw - 44,
                            TR("Add to Playlist", "재생목록에 추가"), 25.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    int visible = 5, rh = 44, gap = 4;
    int step = rh + gap;
    if (g_pl_pick_sel < g_pl_pick_scroll) g_pl_pick_scroll = g_pl_pick_sel;
    if (g_pl_pick_sel >= g_pl_pick_scroll + visible) g_pl_pick_scroll = g_pl_pick_sel - visible + 1;
    if (g_pl_pick_scroll < 0) g_pl_pick_scroll = 0;

    int ry = my + 48;
    int list_h = visible * step - gap;
    int rw = (g_pl_pick_count > visible) ? (mw - 34) : (mw - 28);
    gfx_set_clip_y(ry, ry + list_h);
    for (int i = g_pl_pick_scroll; i < g_pl_pick_count && i < g_pl_pick_scroll + visible; i++) {
        int y = ry + (i - g_pl_pick_scroll) * step;
        bool sel = (i == g_pl_pick_sel);
        gfx_fill_round_rect(mx + 14, y, rw, rh, sel ? rh / 2 : 18, sel ? COL_SEL_BG : COL_MODAL_HI);
        if (sel) gfx_stroke_round_rect(mx + 14, y, rw, rh, rh / 2, 2, COL_BORDER_HI);
        uint32_t fg = sel ? COL_SEL_TEXT : COL_TEXT;
        gfx_draw_icon("list-music", mx + 26, y + 9, 26, fg);
        gfx_draw_text_ellipsize(mx + 60, y + 9, rw - 46, g_pl_pick_names[i], 23.0f, UI_FONT_SEMIBOLD, fg);
    }
    gfx_reset_clip();
    if (g_pl_pick_count > visible)
        gfx_draw_scrollbar(mx + mw - 14, ry, list_h, (float)g_pl_pick_scroll, visible, g_pl_pick_count);
}

static void execute_playlist_pick(void)
{
    int ti = g_pl_pick_track_idx;
    int sel = g_pl_pick_sel;
    g_pl_pick_open = false;
    if (ti < 0 || ti >= g_db.track_count || sel < 0 || sel >= g_pl_pick_count) return;
    mkdir("/mnt/sdcard/Playlists", 0777);
    FILE *f = fopen(g_pl_pick_paths[sel], "a");
    if (f) {
        fprintf(f, "%s\n", g_db.tracks[ti].fs_path);
        fclose(f);
        char msg[128];
        snprintf(msg, sizeof(msg), "%s %s", TR("Added to", "추가됨:"), g_pl_pick_names[sel]);
        show_toast(msg, 1300);
    } else {
        show_toast(TR("Failed to save playlist", "재생목록 저장 실패"), 1300);
    }
}

static void render_photos_grid(const struct player_status *st, int ox)
{
    draw_header(TR("Photos", "사진"), "images", st);
    int total = photos_count();
    if (total == 0) {
        gfx_fill_round_rect(24 + ox, 110, UI_W - 48, 120, 34, COL_SURFACE);
        const char *msg = TR("No photos in /Photos", "/Photos 폴더에 사진이 없습니다");
        int mw = gfx_text_width(msg, 23.0f, UI_FONT_SEMIBOLD);
        gfx_draw_text((UI_W - mw) / 2 + ox, 154, msg, 23.0f, UI_FONT_SEMIBOLD, COL_TEXT_DIM);
        return;
    }

    int cols = 3, visible_rows = 2;
    int total_rows = (total + cols - 1) / cols;
    int sel_row = g_list_sel / cols;
    if (sel_row < g_list_scroll) g_list_scroll = sel_row;
    if (sel_row >= g_list_scroll + visible_rows) g_list_scroll = sel_row - visible_rows + 1;
    if (g_list_scroll < 0) g_list_scroll = 0;

    int cw = 142, ch = 138, gap_x = 8, gap_y = 8;
    int top_y = 62, left_x = 12 + ox;
    int step_y = ch + gap_y;

    gfx_set_clip_y(60, UI_H - 4);
    int first_row = (int)floorf(g_list_scroll_anim) - 1;
    if (first_row < 0) first_row = 0;
    int first_idx = first_row * cols;
    int last_idx = (first_row + visible_rows + 3) * cols;
    if (last_idx > total) last_idx = total;
    uint64_t tnow = now_ms();
    for (int i = first_idx; i < last_idx; i++) {
        int r = i / cols;
        int c = i % cols;
        int x = left_x + c * (cw + gap_x);
        int y = top_y + (int)roundf(((float)r - g_list_scroll_anim) * (float)step_y);
        if (y + ch < 60 || y > UI_H) continue;
        bool sel = (i == g_list_sel);
        float t = item_pill_get(SCR_PHOTOS, i, sel, tnow);
        int cr = (int)roundf(24.0f + t * 10.0f);
        uint32_t bg = lerp_argb(COL_SURFACE, COL_SEL_BG, t);
        gfx_fill_round_rect(x, y, cw, ch, cr, bg);
        if (sel) gfx_stroke_round_rect(x, y, cw, ch, cr, 2, COL_BORDER_HI);

        const struct photo_bitmap *tb = photos_get_thumb(i);
        if (tb && tb->rgb565) {
            gfx_blit_rgb565_squircle(x + 5, y + 6, 132, 92, 18, tb->rgb565, tb->w, tb->h);
        } else {
            gfx_fill_round_rect(x + 5, y + 6, 132, 92, 18, COL_SURFACE_HI);
            gfx_draw_icon("images", x + 53, y + 34, 36, COL_TEXT_DIM);
        }
        const struct photo_item *pi = photos_get(i);
        uint32_t fg = sel ? COL_SEL_TEXT : COL_TEXT;
        gfx_draw_text_ellipsize(x + 8, y + 106, cw - 16, pi ? pi->name : "", 18.0f, UI_FONT_SEMIBOLD, fg);
    }
    gfx_reset_clip();
    gfx_draw_scrollbar(UI_W - 10, top_y, visible_rows * step_y - gap_y, g_list_scroll_anim, visible_rows, total_rows);
}

static void render_photo_view(void)
{
    gfx_reset_clip();
    const struct photo_bitmap *fbmp = photos_get_full(g_photo_idx);
    if (fbmp && fbmp->rgb565) {
        gfx_blit_rgb565_fit_zoom(fbmp->rgb565, fbmp->w, fbmp->h, g_photo_zoom);
    } else {
        gfx_clear(0xFF000000U);
        const char *msg = TR("Cannot load photo", "사진을 불러올 수 없음");
        int mw = gfx_text_width(msg, 24.0f, UI_FONT_SEMIBOLD);
        gfx_draw_text((UI_W - mw) / 2, 164, msg, 24.0f, UI_FONT_SEMIBOLD, 0xFFFFFFFFU);
    }
    if (now_ms() < g_photo_zoom_hud_ms) {
        char zbuf[24];
        snprintf(zbuf, sizeof(zbuf), "%.2fx", g_photo_zoom);
        int zw = gfx_text_width(zbuf, 20.0f, UI_FONT_SEMIBOLD) + 46;
        gfx_fill_round_rect(UI_W - zw - 12, 12, zw, 36, 18, 0xB018181BU);
        gfx_draw_icon(g_photo_zoom > 1.01f ? "zoom-in" : "zoom-out", UI_W - zw - 2, 20, 20, 0xFFFFFFFFU);
        gfx_draw_text(UI_W - zw + 24, 18, zbuf, 20.0f, UI_FONT_SEMIBOLD, 0xFFFFFFFFU);
    }
    if (g_photo_slideshow) {
        gfx_fill_round_rect(12, 12, 36, 36, 18, 0xB018181BU);
        gfx_draw_icon("play", 20, 20, 20, 0xFF69E484U);
    }
}

static void render_current_frame(const struct player_status *st)
{
    if (g_screen == SCR_PHOTO_VIEW && !g_usb_active) {
        render_photo_view();
        render_toast();
        gfx_present();
        return;
    }
    gfx_draw_wallpaper(g_bg_blur_anim);
    if (g_usb_active) {
        render_usb_screen();
    } else {
        int ox = (int)roundf(g_trans_x);
        if (g_screen == SCR_HOME) render_home(st, ox);
        else if (g_screen == SCR_COVERFLOW) render_coverflow(st, ox);
        else if (g_screen == SCR_PHOTOS) render_photos_grid(st, ox);
        else if (g_screen == SCR_NOW_PLAYING) render_now_playing(st, ox);
        else if (g_screen == SCR_SEARCH) render_search_screen(st, ox);
        else render_list_screen(st, ox);

        render_abc_badge();
        render_context_menu(st);
        render_playlist_picker();
        render_vol_warn_modal();
    }
    render_toast();
    gfx_present();
}

static void execute_context_action(const struct player_status *st)
{
    int ti = g_ctx_track_idx;
    bool from_q = g_ctx_from_queue;
    int qpos = g_ctx_queue_pos;
    g_ctx_open = false;
    if (ti < 0 || ti >= g_db.track_count) return;
    const struct ipod_track *tr = &g_db.tracks[ti];

    if (from_q) {
        if (g_ctx_sel == 0) {
            player_remove_queue_pos(qpos);
            int total = current_list_count();
            if (g_list_sel >= total) g_list_sel = total > 0 ? total - 1 : 0;
            show_toast(TR("Removed from Queue", "대기열에서 제거됨"), 1200);
        } else if (g_ctx_sel == 1) {
            if (qpos > 0) {
                player_move_queue_pos(qpos, qpos - 1);
                g_list_sel = qpos - 1;
            }
        } else if (g_ctx_sel == 2) {
            if (st && qpos + 1 < st->queue_len) {
                player_move_queue_pos(qpos, qpos + 1);
                g_list_sel = qpos + 1;
            }
        } else if (g_ctx_sel == 3) {
            player_jump_queue_pos(qpos);
        } else if (g_ctx_sel == 4) {
            bool nf = !is_track_fav(ti);
            set_track_fav(ti, nf);
            y1_ipod_native_record_favorite(tr->rb_path, nf);
            show_toast(nf ? TR("Favorited", "즐겨찾기에 추가됨") : TR("Removed Favorite", "즐겨찾기 해제됨"), 1200);
        } else if (g_ctx_sel == 5) {
            player_stop();
            g_list_sel = 0;
            g_list_scroll = 0;
            g_list_scroll_anim = 0.0f;
            show_toast(TR("Queue cleared", "대기열 비움"), 1200);
        }
        return;
    }

    if (g_ctx_sel == 0) {
        player_play_next(ti);
        show_toast(TR("Added to Play Next", "다음 곡으로 추가됨"), 1200);
    } else if (g_ctx_sel == 1) {
        player_add_to_queue(ti);
        show_toast(TR("Added to Queue", "재생 대기열에 추가됨"), 1200);
    } else if (g_ctx_sel == 2) {
        push_screen(SCR_QUEUE);
    } else if (g_ctx_sel == 3) {
        open_playlist_picker(ti);
    } else if (g_ctx_sel == 4) {
        bool nf = !is_track_fav(ti);
        set_track_fav(ti, nf);
        y1_ipod_native_record_favorite(tr->rb_path, nf);
        show_toast(nf ? TR("Favorited", "즐겨찾기에 추가됨") : TR("Removed Favorite", "즐겨찾기 해제됨"), 1200);
    } else if (g_ctx_sel == 5) {
        const char *alb = tr->album[0] ? tr->album : "Unknown Album";
        open_album_tracks(alb);
    } else if (g_ctx_sel == 6) {
        bool ns = st ? !st->shuffle : true;
        player_set_shuffle(ns);
        show_toast(ns ? TR("Shuffle ON", "셔플 켜짐") : TR("Shuffle OFF", "셔플 꺼짐"), 1200);
    } else if (g_ctx_sel == 7) {
        enum repeat_mode nr = (enum repeat_mode)(((st ? st->repeat : REPEAT_ALL) + 1) % 3);
        player_set_repeat(nr);
        show_toast(nr == REPEAT_OFF ? TR("Repeat: Off", "반복: 끄기")
                 : (nr == REPEAT_ONE ? TR("Repeat: One", "반복: 한 곡")
                                     : TR("Repeat: All", "반복: 전체")), 1200);
    }
}

/* =========================================================================
 * Input Handling
 * ========================================================================= */
static void start_shuffle_all(void)
{
    if (g_db.track_count <= 0) return;
    player_set_shuffle(true);
    play_all_tracks(rand() % g_db.track_count);
    save_resume_state();
    push_screen(SCR_NOW_PLAYING);
}

static void handle_short_select(const struct player_status *st)
{
    if (g_vol_warn_open) {
        g_vol_warn_open = false;
        if (g_vol_warn_sel == 1) {
            g_safe_vol_acked = true;
            player_adjust_volume(g_vol_warn_pending_delta > 0 ? g_vol_warn_pending_delta : g_vol_step);
            g_vol_hud_until_ms = now_ms() + 1500;
            save_resume_state();
        }
        return;
    }

    if (g_pl_pick_open) { execute_playlist_pick(); return; }
    if (g_ctx_open) { execute_context_action(st); return; }

    if (g_screen == SCR_NOW_PLAYING) {
        if (st->has_track && st->track_idx >= 0) {
            g_ctx_track_idx = st->track_idx;
            g_ctx_sel = 0;
            g_ctx_scroll = 0;
            g_ctx_scroll_anim = 0.0f;
            g_ctx_from_np = true;
            g_ctx_from_queue = false;
            g_ctx_queue_pos = -1;
            g_ctx_open = true;
        }
        return;
    }

    if (g_screen == SCR_SEARCH) {
        const char *lbl = k_search_keys[g_search_mode][g_search_sel];
        if (strcmp(lbl, "ABC") == 0 || strcmp(lbl, "abc") == 0 ||
            strcmp(lbl, "한글") == 0 || strcmp(lbl, "쌍자") == 0) {
            hime_commit(&g_search_ime, g_search_buf);
            g_search_mode = (g_search_mode + 1) % 4;
        } else if (strcmp(lbl, "DEL") == 0) {
            hime_backspace(&g_search_ime, g_search_buf);
            g_search_len = (int)strlen(g_search_buf);
        } else if (strcmp(lbl, "SPC") == 0) {
            hime_commit(&g_search_ime, g_search_buf);
            if (g_search_len < (int)sizeof(g_search_buf) - 1) {
                g_search_buf[g_search_len++] = ' ';
                g_search_buf[g_search_len] = '\0';
                hime_commit(&g_search_ime, g_search_buf);
            }
        } else if (strcmp(lbl, "GO") == 0) {
            execute_search_query();
        } else {
            hime_input_key(&g_search_ime, lbl, g_search_buf, sizeof(g_search_buf));
            g_search_len = (int)strlen(g_search_buf);
        }
        return;
    }

    if (g_screen == SCR_HOME) {
        if (g_home_sel == 0) push_screen(SCR_NOW_PLAYING);
        else if (g_home_sel == 1) push_screen(SCR_COVERFLOW);
        else if (g_home_sel == 2) push_screen(SCR_SONGS);
        else if (g_home_sel == 3) push_screen(SCR_ARTISTS);
        else if (g_home_sel == 4) push_screen(SCR_ALBUMS);
        else if (g_home_sel == 5) push_screen(SCR_PLAYLISTS);
        else if (g_home_sel == 6) {
            photos_scan();
            push_screen(SCR_PHOTOS);
        }
        else if (g_home_sel == 7) {
            g_search_len = 0; g_search_buf[0] = '\0'; g_search_sel = 10;
            hime_reset(&g_search_ime);
            push_screen(SCR_SEARCH);
        }
        else if (g_home_sel == 8) start_shuffle_all();
        else if (g_home_sel == 9) push_screen(SCR_SETTINGS);
        return;
    }

    int total = current_list_count();
    if (total <= 0) return;

    if (g_screen == SCR_PHOTOS) {
        g_photo_idx = g_list_sel;
        g_photo_zoom = 1.0f;
        g_photo_slideshow = false;
        push_screen(SCR_PHOTO_VIEW);
    } else if (g_screen == SCR_COVERFLOW || g_screen == SCR_ALBUMS) {
        open_album_tracks(g_db.albums[g_list_sel].name);
    } else if (g_screen == SCR_SONGS) {
        play_all_tracks(g_list_sel);
        save_resume_state();
        push_screen(SCR_NOW_PLAYING);
    } else if (g_screen == SCR_FILTERED_SONGS) {
        player_play_queue(g_filter_indices, g_filter_count, g_list_sel);
        save_resume_state();
        push_screen(SCR_NOW_PLAYING);
    } else if (g_screen == SCR_QUEUE) {
        player_jump_queue_pos(g_list_sel);
        save_resume_state();
        push_screen(SCR_NOW_PLAYING);
    } else if (g_screen == SCR_ARTISTS) {
        const char *name = g_db.artists[g_list_sel].name;
        snprintf(g_filter_title, sizeof(g_filter_title), "%s", name);
        reset_filter_list();
        for (int i = 0; i < g_db.track_count; i++) {
            const char *a = g_db.tracks[i].artist[0] ? g_db.tracks[i].artist : "Unknown Artist";
            if (strcasecmp(a, name) == 0) push_filter_index(i);
        }
        push_screen(SCR_FILTERED_SONGS);
    } else if (g_screen == SCR_PLAYLISTS) {
        const struct ipod_playlist *pl = &g_db.playlists[g_list_sel];
        snprintf(g_filter_title, sizeof(g_filter_title), "%s", pl->name);
        reset_filter_list();
        for (int i = 0; i < pl->track_count; i++)
            push_filter_index(pl->track_indices[i]);
        push_screen(SCR_FILTERED_SONGS);
    } else if (g_screen == SCR_WALLPAPER || g_screen == SCR_FONT_PICKER) {
        if (g_list_sel >= 0 && g_list_sel < g_wp_count) {
            const struct wp_entry *we = &g_wp_items[g_list_sel];
            if (we->is_clear) {
                if (g_screen == SCR_FONT_PICKER) {
                    gfx_reset_default_font();
                    show_toast(TR("Default font restored", "기본 글꼴로 복원됨"), 1200);
                } else {
                    g_wp_album_art = 0;
                    g_last_wp_art_track = -2;
                    save_meui_prefs();
                    gfx_clear_wallpaper();
                    show_toast(TR("Wallpaper cleared", "배경화면 삭제됨"), 1200);
                }
                pop_screen();
            } else if (we->is_album_art) {
                g_wp_album_art = g_wp_album_art ? 0 : 1;
                save_meui_prefs();
                if (g_wp_album_art) {
                    g_last_wp_art_track = -2;
                    sync_album_art_wallpaper(st, true);
                    show_toast(TR("Album art wallpaper ON", "앨범 아트 배경화면 켜짐"), 1400);
                    pop_screen();
                } else {
                    gfx_clear_wallpaper();
                    show_toast(TR("Album art wallpaper OFF", "앨범 아트 배경화면 꺼짐"), 1200);
                }
            } else if (we->is_dir) {
                scan_picker_dir(we->path, g_screen == SCR_FONT_PICKER);
                g_list_sel = 0; g_list_scroll = 0; g_list_scroll_anim = 0.0f;
                item_pill_clear_scope((int)g_screen);
            } else if (g_screen == SCR_FONT_PICKER) {
                if (gfx_set_custom_font(we->path)) {
                    show_toast(TR("Font applied", "글꼴 적용됨"), 1400);
                    pop_screen();
                } else {
                    show_toast(TR("Failed to load font", "글꼴 로드 실패"), 1400);
                }
            } else {
                show_toast(TR("Applying wallpaper...", "배경화면 적용 중..."), 1000);
                render_current_frame(st);
                if (gfx_set_wallpaper_from_image(we->path)) {
                    g_wp_album_art = 0;
                    save_meui_prefs();
                    show_toast(TR("Wallpaper applied", "배경화면 적용됨"), 1400);
                    pop_screen();
                } else {
                    show_toast(TR("Failed to load image", "이미지 로드 실패"), 1400);
                }
            }
        }
    } else if (g_screen == SCR_SETTINGS) {
        switch (g_list_sel) {
            case 0: g_brightness = (g_brightness >= 100) ? 10 : g_brightness + 15; y1_backlight_set_percent(g_brightness); break;
            case 1: push_screen(SCR_BT_MENU); break;
            case 2: scan_picker_dir("/mnt/sdcard", false); push_screen(SCR_WALLPAPER); break;
            case 3: g_dark_mode = !g_dark_mode; gfx_set_dark_mode(g_dark_mode != 0); break;
            case 4: player_set_shuffle(!st->shuffle); break;
            case 5: player_set_repeat((enum repeat_mode)((st->repeat + 1) % 3)); break;
            case 6: g_lang = g_lang ? 0 : 1; break;
            case 7: push_screen(SCR_ADVANCED_SETTINGS); break;
            case 8: save_resume_state(); y1_sys_poweroff(); break;
        }
    } else if (g_screen == SCR_BT_MENU) {
        switch (g_list_sel) {
            case 0:
                y1_bt_set_enabled(!y1_bt_is_enabled());
                show_toast(y1_bt_is_enabled() ? TR("Bluetooth ON", "블루투스 켜짐")
                                              : TR("Bluetooth OFF", "블루투스 꺼짐"), 1200);
                break;
            case 1: {
                bool next_rx = !y1_bt_is_receiver_mode();
                if (next_rx && st->playing) player_pause();
                y1_bt_set_receiver_mode(next_rx);
                show_toast(next_rx ? TR("BT Receiver Mode ON", "블루투스 수신 모드 켜짐")
                                   : TR("BT Receiver Mode OFF", "블루투스 수신 모드 꺼짐"), 1200);
                break;
            }
            case 2: push_screen(SCR_BT_PAIRED); break;
            case 3: push_screen(SCR_BT_NEW); break;
            case 4: push_screen(SCR_BT_FORGET); break;
        }
    } else if (g_screen == SCR_BT_PAIRED || g_screen == SCR_BT_NEW) {
        if (g_list_sel >= 0 && g_list_sel < g_bt_dev_count) {
            const struct y1_bt_device *dev = &g_bt_devs[g_list_sel];
            if (dev->connected) {
                y1_bt_disconnect();
                show_toast(TR("Disconnecting...", "연결 해제 중..."), 1200);
            } else {
                bt_connect_best_codec(dev->bdaddr);
                show_toast(TR("Connecting...", "연결 중..."), 1500);
            }
        }
    } else if (g_screen == SCR_BT_FORGET) {
        if (g_list_sel >= 0 && g_list_sel < g_bt_dev_count) {
            y1_bt_forget_device(g_bt_devs[g_list_sel].bdaddr);
            refresh_bt_sublist();
            show_toast(TR("Device forgotten", "기기 삭제됨"), 1200);
        }
    } else if (g_screen == SCR_ADVANCED_SETTINGS) {
        switch (g_list_sel) {
            case 0:
                g_safe_vol_enabled = g_safe_vol_enabled ? 0 : 1;
                g_safe_vol_acked = false;
                save_meui_prefs();
                break;
            case 1:
                g_use_ipod_db = g_use_ipod_db ? 0 : 1;
                y1_usb_set_ipod_mode(g_use_ipod_db != 0);
                save_meui_prefs();
                reload_library();
                show_toast(g_use_ipod_db ? TR("Switched to iPod DB", "iPod DB로 전환됨")
                                         : TR("Switched to Files", "일반 파일로 전환됨"), 1200);
                break;
            case 2: {
                static const int ts[] = { 15, 30, 60, 120, 0 };
                int ni = 1;
                for (int k = 0; k < 5; k++) if (ts[k] == g_screen_timeout_sec) ni = (k + 1) % 5;
                g_screen_timeout_sec = ts[ni];
                save_meui_prefs();
                break;
            }
            case 3: {
                static const int vs[] = { 1, 2, 5, 10 };
                int ni = 0; for (int k = 0; k < 4; k++) if (vs[k] == g_vol_step) ni = (k + 1) % 4;
                g_vol_step = vs[ni]; save_meui_prefs(); break;
            }
            case 4: {
                static const int ss[] = { 5, 10, 15, 30 };
                int ni = 0; for (int k = 0; k < 4; k++) if (ss[k] == g_seek_sec) ni = (k + 1) % 4;
                g_seek_sec = ss[ni]; save_meui_prefs(); break;
            }
            case 5: scan_picker_dir("/mnt/sdcard", true); push_screen(SCR_FONT_PICKER); break;
            case 6: reload_library(); show_toast(TR("Library refreshed", "음악 목록 새로고침됨"), 1200); break;
            case 7: launch_system_settings(); break;
        }
    }
}

static void handle_non_select_button(uint32_t btn, int ev_value)
{
    /* Photo Full View controls: clickwheel = zoom, Menu = back, Prev/Next = photo, Play = 1m slideshow */
    if (g_screen == SCR_PHOTO_VIEW && !g_ctx_open && !g_pl_pick_open && !g_vol_warn_open) {
        if (ev_value < 1) return;
        int pcnt = photos_count();
        if (btn == Y1_BTN_MENU) {
            g_photo_slideshow = false;
            pop_screen();
        } else if (btn == Y1_BTN_SCROLL_FWD) {
            g_photo_zoom += 0.25f;
            if (g_photo_zoom > 4.0f) g_photo_zoom = 4.0f;
            g_photo_zoom_hud_ms = now_ms() + 1200;
        } else if (btn == Y1_BTN_SCROLL_BACK) {
            g_photo_zoom -= 0.25f;
            if (g_photo_zoom < 1.0f) g_photo_zoom = 1.0f;
            g_photo_zoom_hud_ms = now_ms() + 1200;
        } else if (btn == Y1_BTN_RIGHT && pcnt > 0) {
            g_photo_idx = (g_photo_idx + 1) % pcnt;
            g_photo_zoom = 1.0f;
            g_photo_slide_ms = now_ms();
        } else if (btn == Y1_BTN_LEFT && pcnt > 0) {
            g_photo_idx = (g_photo_idx + pcnt - 1) % pcnt;
            g_photo_zoom = 1.0f;
            g_photo_slide_ms = now_ms();
        } else if (btn == Y1_BTN_PLAY && pcnt > 0) {
            g_photo_slideshow = !g_photo_slideshow;
            g_photo_slide_ms = now_ms();
            show_toast(g_photo_slideshow ? TR("Slideshow ON (60s)", "슬라이드쇼 켜짐 (60초)")
                                         : TR("Slideshow OFF", "슬라이드쇼 꺼짐"), 1200);
        }
        return;
    }

    /* Search Keyboard: follow ConfigApp OSK control scheme (arrows = col, scroll = row, Play = GO) */
    if (g_screen == SCR_SEARCH && !g_ctx_open && !g_vol_warn_open) {
        if (ev_value < 1) return;
        int row = g_search_sel / 10;
        int col = g_search_sel % 10;
        if (btn == Y1_BTN_MENU) {
            pop_screen();
        } else if (btn == Y1_BTN_PLAY) {
            execute_search_query();
        } else if (btn == Y1_BTN_SCROLL_FWD) {
            row = (row + 1) % 4;
            g_search_sel = row * 10 + col;
        } else if (btn == Y1_BTN_SCROLL_BACK) {
            row = (row + 3) % 4;
            g_search_sel = row * 10 + col;
        } else if (btn == Y1_BTN_RIGHT) {
            col = (col + 1) % 10;
            g_search_sel = row * 10 + col;
        } else if (btn == Y1_BTN_LEFT) {
            col = (col + 9) % 10;
            g_search_sel = row * 10 + col;
        }
        return;
    }

    /* Global Media Controls: Prev/Next & Play/Pause ALWAYS control media on every other page */
    if (btn == Y1_BTN_PLAY) {
        if (ev_value == 1) {
            g_play_held = true;
            g_play_hold_fired = false;
            g_play_press_ms = now_ms();
        } else if (ev_value == 0) {
            if (g_play_held && !g_play_hold_fired) {
                trigger_play_pause();
                if (g_screen == SCR_HOME && !g_ctx_open && !g_vol_warn_open)
                    push_screen(SCR_NOW_PLAYING);
            }
            g_play_held = false;
        }
        return;
    }
    if (btn == Y1_BTN_LEFT || btn == Y1_BTN_RIGHT) {
        if (ev_value == 1) {
            g_media_held_btn = btn;
            g_media_seek_fired = false;
            g_media_press_ms = now_ms();
            g_media_last_seek_ms = g_media_press_ms;
        } else if (ev_value == 0) {
            if (g_media_held_btn == btn && !g_media_seek_fired) {
                if (btn == Y1_BTN_RIGHT) player_next();
                else player_prev();
                save_resume_state();
            }
            g_media_held_btn = Y1_BTN_NONE;
            g_media_seek_fired = false;
        }
        return;
    }

    if (ev_value < 1)
        return;

    if (g_vol_warn_open) {
        if (btn == Y1_BTN_MENU) g_vol_warn_open = false;
        else if (btn == Y1_BTN_SCROLL_BACK) g_vol_warn_sel = 0;
        else if (btn == Y1_BTN_SCROLL_FWD) g_vol_warn_sel = 1;
        return;
    }

    if (g_pl_pick_open) {
        if (btn == Y1_BTN_MENU) g_pl_pick_open = false;
        else if (btn == Y1_BTN_SCROLL_FWD && g_pl_pick_count > 0) g_pl_pick_sel = (g_pl_pick_sel + 1) % g_pl_pick_count;
        else if (btn == Y1_BTN_SCROLL_BACK && g_pl_pick_count > 0) g_pl_pick_sel = (g_pl_pick_sel + g_pl_pick_count - 1) % g_pl_pick_count;
        return;
    }

    if (g_ctx_open) {
        int n_ctx = ctx_item_count();
        if (btn == Y1_BTN_MENU) g_ctx_open = false;
        else if (btn == Y1_BTN_SCROLL_FWD) g_ctx_sel = (g_ctx_sel + 1) % n_ctx;
        else if (btn == Y1_BTN_SCROLL_BACK) g_ctx_sel = (g_ctx_sel + n_ctx - 1) % n_ctx;
        return;
    }

    if (g_screen == SCR_NOW_PLAYING) {
        if (btn == Y1_BTN_MENU) pop_screen();
        else if (btn == Y1_BTN_SCROLL_FWD) request_volume_delta(g_vol_step);
        else if (btn == Y1_BTN_SCROLL_BACK) request_volume_delta(-g_vol_step);
        return;
    }

    if (g_screen == SCR_HOME) {
        if (btn == Y1_BTN_SCROLL_FWD) g_home_sel = (g_home_sel + 1) % NUM_HOME_ITEMS;
        else if (btn == Y1_BTN_SCROLL_BACK) g_home_sel = (g_home_sel + NUM_HOME_ITEMS - 1) % NUM_HOME_ITEMS;
        return;
    }

    if (btn == Y1_BTN_MENU) {
        pop_screen();
        return;
    }

    int total = current_list_count();
    if (total <= 0) return;

    if (btn == Y1_BTN_SCROLL_FWD || btn == Y1_BTN_SCROLL_BACK) {
        int dir = (btn == Y1_BTN_SCROLL_FWD) ? 1 : -1;
        uint64_t tnow = now_ms();
        if (dir == g_scroll_last_dir && (tnow - g_scroll_last_ms) <= 130)
            g_scroll_fast_streak++;
        else
            g_scroll_fast_streak = 1;
        g_scroll_last_dir = dir;
        g_scroll_last_ms = tnow;

        if (screen_is_abc_sortable(g_screen) && total >= 20 &&
            (g_abc_active || g_scroll_fast_streak >= 8)) {
            g_abc_active = true;
            g_abc_until_ms = tnow + 1100;
            char cur_l[8] = "";
            int cur_b = meui_abc_bucket(list_item_sort_name(g_list_sel), cur_l, sizeof(cur_l));
            int next_idx = (g_list_sel + total + dir) % total;
            for (int step = 1; step < total; step++) {
                int cand = (g_list_sel + total + dir * step) % total;
                char cl[8] = "";
                int cb = meui_abc_bucket(list_item_sort_name(cand), cl, sizeof(cl));
                if (cb != cur_b) {
                    next_idx = cand;
                    snprintf(g_abc_label, sizeof(g_abc_label), "%s", cl);
                    break;
                }
            }
            g_list_sel = next_idx;
            if (!g_abc_label[0])
                meui_abc_bucket(list_item_sort_name(g_list_sel), g_abc_label, sizeof(g_abc_label));
        } else {
            int accel = 1;
            if (total >= 12 && g_scroll_fast_streak >= 4) {
                if (g_scroll_fast_streak >= 12) accel = (total >= 200) ? 8 : 5;
                else if (g_scroll_fast_streak >= 8) accel = 3;
                else accel = 2;
            }
            if (accel == 1) {
                g_list_sel = (g_list_sel + total + dir) % total;
            } else {
                int next = g_list_sel + dir * accel;
                if (next < 0) next = 0;
                if (next >= total) next = total - 1;
                g_list_sel = next;
            }
        }
    }
}

static void on_signal(int sig) { (void)sig; g_should_exit = true; }

int main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    y1_init();
    y1_pmic_tune_accdet_debounce();
    apply_base_low_power();
    init_bluetooth_callbacks();
    gfx_init();
    album_art_init();
    photos_init();
    load_prefs();
    y1_backlight_set_percent(g_brightness);

    ipod_db_init(&g_db);
    load_library_with_ui(true);
    player_init(&g_db);
    load_resume_state();

    bool dirty = true;
    uint32_t last_sec = 0xFFFFFFFFU;
    int  last_min = -1;
    bool last_playing = false;
    bool last_has_track = false;
    int  last_track_idx = -1;
    int  last_bat_pct = -1;
    bool last_bat_chg = false;
    bool last_bt_conn = false;
    bool last_bt_en = false;
    int  last_bt_cnt = -1;

    uint64_t last_hw_poll_ms = 0;
    uint64_t last_resume_save_ms = now_ms();
    g_last_activity_ms = now_ms();
    g_usb_was_online = y1_power_usb_online();
    g_jack_was_inserted = y1_jack_is_inserted();
    g_cached_bat_pct = y1_battery_percent();
    g_cached_bat_chg = y1_battery_is_charging();
    g_cached_bt_conn = y1_bt_is_connected();

    while (!g_should_exit) {
        uint64_t tnow = now_ms();

        /* 0. Monitor USB, Headphone Jack, Battery & Bluetooth Status (every 1000ms for low power) */
        if (tnow - last_hw_poll_ms >= 1000) {
            last_hw_poll_ms = tnow;

            bool usb_now = y1_power_usb_online();
            if (usb_now && !g_usb_was_online) {
                g_usb_charge_only = false;
                enter_usb_mode();
                dirty = true;
            } else if (!usb_now) {
                g_usb_charge_only = false;
                if (g_usb_active) {
                    exit_usb_mode(false);
                    dirty = true;
                }
            } else if (g_usb_active && y1_usb_check_host_ejected()) {
                exit_usb_mode(true);
                dirty = true;
            }
            g_usb_was_online = usb_now;

            bool jack_now = y1_jack_is_inserted();
            if (g_jack_was_inserted && !jack_now && !y1_bt_is_connected()) {
                struct player_status pst;
                player_get_status(&pst);
                if (pst.playing) {
                    player_pause();
                    save_resume_state();
                    show_toast(TR("Headphones unplugged", "이어폰 연결 해제됨"), 1600);
                    dirty = true;
                }
                g_safe_vol_acked = false;
            }
            g_jack_was_inserted = jack_now;

            bool bt_conn = false;
            char bt_name[64] = "";
            if (y1_bt_poll_connection_notify(&bt_conn, bt_name, sizeof(bt_name))) {
                char msg[96];
                if (bt_conn) {
                    if (bt_name[0])
                        snprintf(msg, sizeof(msg), "%s: %s", TR("BT Connected", "블루투스 연결됨"), bt_name);
                    else
                        snprintf(msg, sizeof(msg), "%s", TR("Bluetooth Connected", "블루투스 연결됨"));
                    show_toast(msg, 2000);
                } else {
                    player_pause();
                    save_resume_state();
                    show_toast(TR("Bluetooth Disconnected", "블루투스 연결 해제됨"), 2000);
                }
                g_bt_ev_connected = 0;
                g_bt_ev_disconnected = 0;
                dirty = true;
            } else if (g_bt_ev_connected) {
                g_bt_ev_connected = 0;
                const char *bn = y1_bt_get_connected_name();
                char msg[96];
                if (bn && bn[0])
                    snprintf(msg, sizeof(msg), "%s: %s", TR("BT Connected", "블루투스 연결됨"), bn);
                else
                    snprintf(msg, sizeof(msg), "%s", TR("Bluetooth Connected", "블루투스 연결됨"));
                show_toast(msg, 2000);
                dirty = true;
            } else if (g_bt_ev_disconnected) {
                g_bt_ev_disconnected = 0;
                player_pause();
                save_resume_state();
                show_toast(TR("Bluetooth Disconnected", "블루투스 연결 해제됨"), 2000);
                dirty = true;
            }

            /* Live status change detection (cached for zero sysfs I/O during rendering) */
            int  bp = y1_battery_percent();
            bool bc = y1_battery_is_charging();
            bool btc = y1_bt_is_connected();
            bool bte = y1_bt_is_enabled();
            g_cached_bat_pct = bp;
            g_cached_bat_chg = bc;
            g_cached_bt_conn = btc;
            if (bp != last_bat_pct || bc != last_bat_chg || btc != last_bt_conn || bte != last_bt_en) {
                last_bat_pct = bp;
                last_bat_chg = bc;
                last_bt_conn = btc;
                last_bt_en = bte;
                dirty = true;
            }
            if (g_screen == SCR_BT_NEW || g_screen == SCR_BT_PAIRED || g_screen == SCR_BT_FORGET) {
                refresh_bt_sublist();
                if (g_bt_dev_count != last_bt_cnt) {
                    last_bt_cnt = g_bt_dev_count;
                    dirty = true;
                }
            }
        }

        /* Process Bluetooth AVRCP Controls */
        if (g_bt_avrcp_op != 0 && !g_usb_active) {
            uint8_t op = g_bt_avrcp_op;
            g_bt_avrcp_op = 0;
            if (op == 0x44) {
                struct player_status pst; player_get_status(&pst);
                if (!pst.playing) trigger_play_pause();
            } else if (op == 0x46 || op == 0x45) {
                player_pause(); save_resume_state();
            } else if (op == 0x4B) {
                player_next(); save_resume_state();
            } else if (op == 0x4C) {
                player_prev(); save_resume_state();
            } else if (op == 0x41) {
                request_volume_delta(g_vol_step);
            } else if (op == 0x42) {
                request_volume_delta(-g_vol_step);
            }
            dirty = true;
        }

        /* Resolve pending MFB multi-click */
        if (g_mfb_clicks > 0 && (now_ms() - g_mfb_last_ms >= 450)) {
            int clicks = g_mfb_clicks;
            g_mfb_clicks = 0;
            if (!g_usb_active) {
                if (clicks == 1) trigger_play_pause();
                else if (clicks == 2) { player_next(); save_resume_state(); }
                else if (clicks >= 3) { player_prev(); save_resume_state(); }
                dirty = true;
            }
        }

        struct player_status st;
        player_get_status(&st);

        /* Detect Play/Pause or Track changes immediately */
        if (st.playing != last_playing || st.has_track != last_has_track || st.track_idx != last_track_idx) {
            last_playing = st.playing;
            last_has_track = st.has_track;
            last_track_idx = st.track_idx;
            dirty = true;
            if (g_wp_album_art)
                sync_album_art_wallpaper(&st, true);
            if (g_screen_off) {
                y1_power_set_idle_sleep(!st.playing);
                y1_cpu_set_screen_off_clamp(!st.playing);
            }
        }
        if (g_wp_album_art && g_last_wp_art_track == -2) {
            sync_album_art_wallpaper(&st, false);
            dirty = true;
        }

        /* Periodic resume-position save while playing (every 30s to avoid SD card stuttering) */
        if (st.playing && (tnow - last_resume_save_ms >= 30000)) {
            last_resume_save_ms = tnow;
            save_resume_state();
        }

        float target_blur = (g_screen == SCR_HOME || g_screen == SCR_NOW_PLAYING) ? 0.0f : 1.0f;
        bool home_art_anim = (!g_screen_off && !g_usb_active && !g_wp_album_art &&
                              g_screen == SCR_HOME && st.playing && st.has_track);
        bool anim_pending = (fabsf(g_trans_x) > 0.5f ||
                             fabsf(g_bg_blur_anim - target_blur) > 0.02f ||
                             fabsf(g_home_scroll_anim - (float)g_home_scroll) > 0.02f ||
                             fabsf(g_list_scroll_anim - (float)g_list_scroll) > 0.02f ||
                             (g_ctx_open && fabsf(g_ctx_scroll_anim - (float)g_ctx_scroll) > 0.02f) ||
                             item_pill_any_active() || sw_anim_any_active() || gfx_wallpaper_is_fading());

        bool ui_burst = (!g_screen_off && (anim_pending || (tnow - g_last_activity_ms) < 1200));
        update_cpu_power_state(ui_burst, y1_bt_a2dp_is_streaming());

        static uint64_t last_render_dur_ms = 0;
        int wait_ms;
        if (g_mfb_clicks > 0)
            wait_ms = 40;
        else if (g_screen_off)
            wait_ms = st.playing ? 500 : 1000;
        else if (dirty || anim_pending)
            wait_ms = (last_render_dur_ms < 15) ? (16 - (int)last_render_dur_ms) : 1;
        else if (home_art_anim)
            wait_ms = (last_render_dur_ms < 49) ? (50 - (int)last_render_dur_ms) : 1;
        else if (st.playing && g_screen == SCR_NOW_PLAYING)
            wait_ms = 250;
        else
            wait_ms = 500;

        /* 1. Poll & drain ALL pending input events */
        struct y1_input_event ev;
        if (y1_input_poll(&ev, wait_ms) > 0) {
            do {
                if (ev.button == Y1_BTN_HEADSET_MFB) {
                    if (ev.value == 1 && !g_usb_active) {
                        g_mfb_clicks++;
                        g_mfb_last_ms = now_ms();
                    }
                    continue;
                }
                if (ev.button == Y1_BTN_HEADSET_LONG) {
                    if (ev.value == 1 && !g_usb_active) {
                        g_mfb_clicks = 0;
                        struct player_status pst;
                        player_get_status(&pst);
                        bool next_shuf = !pst.shuffle;
                        player_set_shuffle(next_shuf);
                        show_toast(next_shuf ? TR("Shuffle ON", "셔플 켜짐")
                                             : TR("Shuffle OFF", "셔플 꺼짐"), 1200);
                        save_resume_state();
                        dirty = true;
                    }
                    continue;
                }

                if (g_screen_off) {
                    if (ev.button == Y1_BTN_SELECT && ev.value == 1) {
                        player_get_status(&st);
                        exit_screen_off(&st);
                        g_wake_swallow_select = true;
                        dirty = false;
                    }
                    continue;
                }

                g_last_activity_ms = now_ms();

                if (g_wake_swallow_select && ev.button == Y1_BTN_SELECT) {
                    if (ev.value == 0) g_wake_swallow_select = false;
                    continue;
                }

                if (g_usb_active) {
                    if (ev.value == 1 && ev.button != Y1_BTN_NONE &&
                        ev.button != Y1_BTN_SCROLL_FWD && ev.button != Y1_BTN_SCROLL_BACK) {
                        exit_usb_mode(true);
                        dirty = true;
                    }
                    continue;
                }

                if (ev.button == Y1_BTN_SELECT) {
                    if (ev.value == 1) {
                        g_sel_held = true;
                        g_sel_long_fired = false;
                        g_sel_press_ms = now_ms();
                    } else if (ev.value == 0) {
                        if (g_sel_held && !g_sel_long_fired) {
                            handle_short_select(&st);
                            dirty = true;
                        }
                        g_sel_held = false;
                    }
                } else if (ev.button != Y1_BTN_NONE) {
                    handle_non_select_button(ev.button, ev.value);
                    dirty = true;
                }
            } while (y1_input_poll(&ev, 0) > 0);
        }

        /* Hold Play/Pause (>= 1000ms) -> Stop playback (refresh tnow AFTER poll to avoid uint64 underflow!) */
        tnow = now_ms();
        if (!g_screen_off && !g_usb_active && g_play_held && !g_play_hold_fired &&
            tnow >= g_play_press_ms && (tnow - g_play_press_ms >= 1000)) {
            player_stop();
            g_play_hold_fired = true;
            y1_vibrate(25);
            dirty = true;
        }

        /* Hold Prev/Next (>= 450ms) to seek -5s / +5s */
        if (!g_screen_off && !g_usb_active &&
            (g_media_held_btn == Y1_BTN_LEFT || g_media_held_btn == Y1_BTN_RIGHT) &&
            tnow >= g_media_press_ms && (tnow - g_media_press_ms >= 450)) {
            if (!g_media_seek_fired || (tnow >= g_media_last_seek_ms && tnow - g_media_last_seek_ms >= 300)) {
                int delta_ms = (g_media_held_btn == Y1_BTN_RIGHT) ? 5000 : -5000;
                player_seek_delta_ms(delta_ms);
                g_media_seek_fired = true;
                g_media_last_seek_ms = tnow;
                dirty = true;
            }
        }

        /* 2. Screen Timeout check */
        if (!g_screen_off && !g_usb_active && g_screen_timeout_sec > 0 &&
            tnow >= g_last_activity_ms &&
            (tnow - g_last_activity_ms >= (uint64_t)g_screen_timeout_sec * 1000ULL)) {
            enter_screen_off();
            continue;
        }

        if (g_screen_off)
            continue;

        /* Clear expired ABC badge */
        if (g_abc_active && tnow >= g_abc_until_ms) {
            g_abc_active = false;
            dirty = true;
        }

        /* 3. Press-and-hold (>= 450ms) on Select in Song Lists & Queue */
        if (!g_usb_active && !g_vol_warn_open && g_sel_held && !g_sel_long_fired && !g_ctx_open &&
            tnow >= g_sel_press_ms && (tnow - g_sel_press_ms >= 450)) {
            if (g_screen == SCR_SONGS || g_screen == SCR_FILTERED_SONGS || g_screen == SCR_QUEUE) {
                int total = current_list_count();
                if (g_list_sel >= 0 && g_list_sel < total) {
                    int ti = -1;
                    if (g_screen == SCR_SONGS) {
                        ti = g_list_sel;
                    } else if (g_screen == SCR_FILTERED_SONGS) {
                        ti = g_filter_indices[g_list_sel];
                    } else {
                        int *qb = malloc(sizeof(int) * (size_t)total);
                        if (qb) {
                            int qn = player_get_queue(qb, total);
                            if (g_list_sel < qn) ti = qb[g_list_sel];
                            free(qb);
                        }
                    }
                    if (ti >= 0) {
                        g_ctx_track_idx = ti;
                        g_ctx_sel = 0;
                        g_ctx_scroll = 0;
                        g_ctx_scroll_anim = 0.0f;
                        g_ctx_from_np = false;
                        g_ctx_from_queue = (g_screen == SCR_QUEUE);
                        g_ctx_queue_pos = g_ctx_from_queue ? g_list_sel : -1;
                        g_ctx_open = true;
                        g_sel_long_fired = true;
                        y1_vibrate(18);
                        dirty = true;
                    }
                }
            }
        }

        /* 4. Advance smooth scroll, 0.4s pill corner-radius morph & screen transition animations */
        bool animating = false;
        if (item_pill_update_all(tnow)) animating = true;
        if (sw_anim_update_all(tnow)) animating = true;
        if (home_art_anim) {
            update_art_pan(tnow);
            animating = true;
        }
        if (fabsf(g_trans_x) > 0.5f) {
            g_trans_x *= 0.62f;
            if (fabsf(g_trans_x) < 1.0f) g_trans_x = 0.0f;
            animating = true;
        }
        float hdiff = (float)g_home_scroll - g_home_scroll_anim;
        if (fabsf(hdiff) > 0.015f) {
            g_home_scroll_anim += hdiff * 0.38f;
            animating = true;
        } else {
            g_home_scroll_anim = (float)g_home_scroll;
        }
        float bdiff = target_blur - g_bg_blur_anim;
        if (fabsf(bdiff) > 0.02f) {
            g_bg_blur_anim += bdiff * 0.30f;
            animating = true;
        } else {
            g_bg_blur_anim = target_blur;
        }
        float ldiff = (float)g_list_scroll - g_list_scroll_anim;
        if (fabsf(ldiff) > 0.015f) {
            g_list_scroll_anim += ldiff * 0.38f;
            animating = true;
        } else {
            g_list_scroll_anim = (float)g_list_scroll;
        }
        float cdiff = (float)g_ctx_scroll - g_ctx_scroll_anim;
        if (g_ctx_open && fabsf(cdiff) > 0.015f) {
            g_ctx_scroll_anim += cdiff * 0.38f;
            animating = true;
        } else {
            g_ctx_scroll_anim = (float)g_ctx_scroll;
        }

        /* Photo Slideshow 1-minute (60000 ms) auto-advance */
        if (g_screen == SCR_PHOTO_VIEW && g_photo_slideshow && !g_screen_off && !g_usb_active) {
            int pcnt = photos_count();
            if (pcnt > 0 && tnow >= g_photo_slide_ms && (tnow - g_photo_slide_ms >= 60000ULL)) {
                g_photo_idx = (g_photo_idx + 1) % pcnt;
                g_photo_zoom = 1.0f;
                g_photo_slide_ms = tnow;
                dirty = true;
            }
        }

        /* 5. Live clock & playback progress updates */
        if (g_screen == SCR_NOW_PLAYING && st.playing) {
            uint32_t cur_sec = st.pos_ms / 1000;
            if (cur_sec != last_sec) {
                last_sec = cur_sec;
                dirty = true;
            }
        }
        time_t wt = time(NULL);
        struct tm *wtm = localtime(&wt);
        if (wtm && wtm->tm_min != last_min) {
            last_min = wtm->tm_min;
            dirty = true;
        }

        if (dirty || animating || tnow < g_vol_hud_until_ms || tnow < g_toast_until_ms ||
            (g_screen == SCR_PHOTO_VIEW && tnow < g_photo_zoom_hud_ms)) {
            uint64_t rf0 = now_ms();
            player_get_status(&st);
            render_current_frame(&st);
            last_render_dur_ms = now_ms() - rf0;
            dirty = false;
        } else {
            last_render_dur_ms = 0;
        }
    }

    save_resume_state();
    if (g_usb_active) y1_usb_detach_mass_storage();
    album_art_shutdown();
    photos_shutdown();
    player_shutdown();
    ipod_db_free(&g_db);
    free(g_filter_indices);
    free(g_fav_flags);
    gfx_shutdown();
    y1_shutdown();
    return 0;
}
