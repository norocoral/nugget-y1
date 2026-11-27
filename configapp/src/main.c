#include "ui_gfx.h"
#include "cfg_registry.h"
#include "hangul_ime.h"
#include "lang.h"
#include "zmodem.h"
#include "liby1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <strings.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/klog.h>
#include <linux/rtc.h>
#include <linux/fb.h>

#define SETTINGS_CONF_PATH "/mnt/sdcard/.system/configapp/settings.conf"
#define SETTINGS_CONF_FALLBACK "/.system/configapp/settings.conf"

/* =========================================================================
 * Persistent Settings State
 * ========================================================================= */
static struct {
    int  lang;             /* 0 = English, 1 = Korean */
    int  dark_mode;        /* 0 = Light, 1 = Dark */
    int  brightness_pct;
    int  eco_mode;
    int  touch_scan;
    int  vibrate_ms;
    int  cpu_mode;
    int  dual_core;
    int  screen_off_clamp;
    int  usb_ipod_mode;
    int  usb_perf_tune;
    int  debug_log;
    int  bt_codec;
    int  screenshot;       /* 0 = Off (default), 1 = MENU + Play -> /Screenshots */
} g_cfg = {
    .lang             = 0,
    .dark_mode        = 0,
    .brightness_pct   = 65,
    .eco_mode         = 0,
    .touch_scan       = 1,
    .vibrate_ms       = 20,
    .cpu_mode         = Y1_CPU_MODE_DYNAMIC,
    .dual_core        = 1,
    .screen_off_clamp = 1,
    .usb_ipod_mode    = 0,
    .usb_perf_tune    = 1,
    .debug_log        = 0,
    .bt_codec         = Y1_BT_CODEC_SBC,
    .screenshot       = 0
};

static bool g_should_exit = false;
static bool g_boot_mode = false;
static char g_toast_msg[96] = "";
static struct timespec g_toast_until = {0, 0};

/* Editable Date/Time staging (synced from system/RTC) */
static int dt_year = 2026;
static int dt_month = 11;
static int dt_day = 22;
static int dt_hour12 = 12; /* 1..12 */
static int dt_minute = 0;
static int dt_ampm = 0;    /* 0 = AM, 1 = PM */
static bool dt_loaded = false;

static void show_toast(const char *msg, int ms)
{
    snprintf(g_toast_msg, sizeof(g_toast_msg), "%s", msg ? msg : "");
    clock_gettime(CLOCK_MONOTONIC, &g_toast_until);
    g_toast_until.tv_sec += ms / 1000;
    g_toast_until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (g_toast_until.tv_nsec >= 1000000000L) {
        g_toast_until.tv_sec += 1;
        g_toast_until.tv_nsec -= 1000000000L;
    }
}

static bool toast_active(void)
{
    if (!g_toast_msg[0])
        return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > g_toast_until.tv_sec ||
        (now.tv_sec == g_toast_until.tv_sec && now.tv_nsec >= g_toast_until.tv_nsec)) {
        g_toast_msg[0] = '\0';
        return false;
    }
    return true;
}

static void save_settings_conf(void)
{
    mkdir("/mnt/sdcard/.system", 0777);
    mkdir("/mnt/sdcard/.system/configapp", 0777);
    FILE *f = fopen(SETTINGS_CONF_PATH, "w");
    if (!f)
        f = fopen(SETTINGS_CONF_FALLBACK, "w");
    if (!f)
        return;

    fprintf(f, "lang=%d\n", g_cfg.lang);
    fprintf(f, "dark_mode=%d\n", g_cfg.dark_mode);
    fprintf(f, "brightness_pct=%d\n", g_cfg.brightness_pct);
    fprintf(f, "eco_mode=%d\n", g_cfg.eco_mode);
    fprintf(f, "touch_scan=%d\n", g_cfg.touch_scan);
    fprintf(f, "vibrate_ms=%d\n", g_cfg.vibrate_ms);
    fprintf(f, "cpu_mode=%d\n", g_cfg.cpu_mode);
    fprintf(f, "dual_core=%d\n", g_cfg.dual_core);
    fprintf(f, "screen_off_clamp=%d\n", g_cfg.screen_off_clamp);
    fprintf(f, "usb_ipod_mode=%d\n", g_cfg.usb_ipod_mode);
    fprintf(f, "usb_perf_tune=%d\n", g_cfg.usb_perf_tune);
    fprintf(f, "debug_log=%d\n", g_cfg.debug_log);
    fprintf(f, "bt_codec=%d\n", g_cfg.bt_codec);
    fprintf(f, "screenshot=%d\n", g_cfg.screenshot);
    fclose(f);
}

static void apply_all_settings(void)
{
    gfx_set_dark_mode(g_cfg.dark_mode != 0);
    y1_backlight_set_percent(g_cfg.brightness_pct);
    if (g_cfg.eco_mode != 1)
        y1_fb_set_eco_mode(g_cfg.eco_mode == 2);
    if (g_cfg.touch_scan != 1)
        y1_input_set_touch_scanning(g_cfg.touch_scan == 2);
    if (g_cfg.dual_core != 1)
        y1_cpu_set_dual_core(g_cfg.dual_core == 2);
    if (g_cfg.cpu_mode >= 0)
        y1_cpu_set_mode(g_cfg.cpu_mode);
    if (g_cfg.usb_ipod_mode != 1)
        y1_usb_set_ipod_mode(g_cfg.usb_ipod_mode == 2);
    if (y1_usb_is_storage_attached())
        y1_usb_tune_performance(g_cfg.usb_perf_tune != 0);
    y1_log_set_enabled(g_cfg.debug_log != 0);
}

static void load_settings_conf(void)
{
    FILE *f = fopen(SETTINGS_CONF_PATH, "r");
    if (!f)
        f = fopen(SETTINGS_CONF_FALLBACK, "r");
    if (!f)
        return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int val = 0;
        if (sscanf(line, "%63[^=]=%d", key, &val) == 2) {
            if (strcmp(key, "lang") == 0) g_cfg.lang = (val == 1) ? 1 : 0;
            else if (strcmp(key, "dark_mode") == 0) g_cfg.dark_mode = val ? 1 : 0;
            else if (strcmp(key, "brightness_pct") == 0) g_cfg.brightness_pct = val;
            else if (strcmp(key, "eco_mode") == 0) g_cfg.eco_mode = val;
            else if (strcmp(key, "touch_scan") == 0) g_cfg.touch_scan = val;
            else if (strcmp(key, "vibrate_ms") == 0) g_cfg.vibrate_ms = val;
            else if (strcmp(key, "cpu_mode") == 0) g_cfg.cpu_mode = val;
            else if (strcmp(key, "dual_core") == 0) g_cfg.dual_core = val;
            else if (strcmp(key, "screen_off_clamp") == 0) g_cfg.screen_off_clamp = val;
            else if (strcmp(key, "usb_ipod_mode") == 0) g_cfg.usb_ipod_mode = val;
            else if (strcmp(key, "usb_perf_tune") == 0) g_cfg.usb_perf_tune = val;
            else if (strcmp(key, "debug_log") == 0) g_cfg.debug_log = val;
            else if (strcmp(key, "bt_codec") == 0) g_cfg.bt_codec = val;
            else if (strcmp(key, "screenshot") == 0) g_cfg.screenshot = val ? 1 : 0;
        }
    }
    fclose(f);
}

/* =========================================================================
 * RTC & Date/Time Helpers
 * ========================================================================= */
static void load_datetime_from_sys(void)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        dt_year = tm->tm_year + 1900;
        dt_month = tm->tm_mon + 1;
        dt_day = tm->tm_mday;
        dt_ampm = (tm->tm_hour >= 12) ? 1 : 0;
        dt_hour12 = tm->tm_hour % 12;
        if (dt_hour12 == 0) dt_hour12 = 12;
        dt_minute = tm->tm_min;
    }
    dt_loaded = true;
}

static int days_in_month(int y, int m)
{
    static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 31;
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)))
        return 29;
    return dm[m - 1];
}

static void clamp_day(void)
{
    int md = days_in_month(dt_year, dt_month);
    if (dt_day > md) dt_day = md;
    if (dt_day < 1) dt_day = 1;
}

static void apply_datetime_to_rtc(void)
{
    int hour24 = (dt_hour12 % 12) + (dt_ampm ? 12 : 0);
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = dt_year - 1900;
    t.tm_mon = dt_month - 1;
    t.tm_mday = dt_day;
    t.tm_hour = hour24;
    t.tm_min = dt_minute;
    t.tm_sec = 0;

    time_t epoch = mktime(&t);
    if (epoch != (time_t)-1)
        y1_rtc_set_time(epoch);
    show_toast(TR_ID(STR_DATE_TIME_SAVED_TO_RTC), 1400);
}

/* =========================================================================
 * Setting Callbacks (Extendible Registry)
 * ========================================================================= */
static void info_battery(char *buf, size_t len)
{
    snprintf(buf, len, "%d%% (%d mV, %s)",
             y1_battery_percent(), y1_battery_voltage_mv(),
             y1_battery_is_charging() ? TR_ID(STR_CHARGING) : TR_ID(STR_DISCHARGING));
}

static void info_current(char *buf, size_t len)
{
    snprintf(buf, len, "%d mA (USB: %s)",
             y1_battery_current_ma(),
             y1_power_usb_online() ? TR_ID(STR_ONLINE) : TR_ID(STR_OFFLINE));
}

static void info_cpu(char *buf, size_t len)
{
    snprintf(buf, len, "%d MHz (%s)",
             y1_cpu_get_freq_mhz(),
             y1_cpu_is_dual_core() ? TR_ID(STR_DUAL_CORE) : TR_ID(STR_SINGLE_CORE));
}

static void info_memory(char *buf, size_t len)
{
    FILE *f = fopen("/proc/meminfo", "r");
    long total_kb = 0, avail_kb = 0, free_kb = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %ld kB", &total_kb) == 1) continue;
            if (sscanf(line, "MemAvailable: %ld kB", &avail_kb) == 1) continue;
            if (sscanf(line, "MemFree: %ld kB", &free_kb) == 1) continue;
        }
        fclose(f);
    }
    long free_mb = (avail_kb > 0 ? avail_kb : free_kb) / 1024;
    long total_mb = total_kb / 1024;
    snprintf(buf, len, "%ld MB / %ld MB", free_mb, total_mb);
}

static void info_storage(char *buf, size_t len)
{
    struct statvfs sv;
    if (statvfs("/mnt/sdcard", &sv) == 0) {
        unsigned long long free_mb = ((unsigned long long)sv.f_bavail * sv.f_frsize) >> 20;
        unsigned long long total_mb = ((unsigned long long)sv.f_blocks * sv.f_frsize) >> 20;
        snprintf(buf, len, "%llu.%llu / %llu.%llu GB",
                 free_mb >> 10, ((free_mb % 1024) * 10) >> 10,
                 total_mb >> 10, ((total_mb % 1024) * 10) >> 10);
    } else {
        snprintf(buf, len, "%s", TR_ID(STR_NOT_MOUNTED));
    }
}

static void info_kernel(char *buf, size_t len)
{
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(buf, len, "%s (%s)", u.release, u.machine);
    else
        snprintf(buf, len, "Linux ARMv7");
}

static void info_uptime(char *buf, size_t len)
{
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0.0;
    if (f) {
        if (fscanf(f, "%lf", &up) != 1)
            up = 0.0;
        fclose(f);
    }
    int sec = (int)up;
    snprintf(buf, len, "%dh %02dm %02ds", sec / 3600, (sec / 60) % 60, sec % 60);
}

static int get_dark_mode(void) { return g_cfg.dark_mode; }
static void set_dark_mode(int v)
{
    g_cfg.dark_mode = v ? 1 : 0;
    gfx_set_dark_mode(g_cfg.dark_mode != 0);
    save_settings_conf();
}

static int get_brightness(void) { return g_cfg.brightness_pct; }
static void set_brightness(int v)
{
    g_cfg.brightness_pct = v;
    y1_backlight_set_percent(v);
    save_settings_conf();
}

static int get_eco_mode(void) { return g_cfg.eco_mode; }
static void set_eco_mode(int v)
{
    g_cfg.eco_mode = (v < 0) ? 0 : (v > 2 ? 2 : v);
    save_settings_conf();
    if (g_cfg.eco_mode != 1)
        y1_fb_set_eco_mode(g_cfg.eco_mode == 2);
}

static int get_touch_scan(void) { return g_cfg.touch_scan; }
static void set_touch_scan(int v)
{
    g_cfg.touch_scan = (v < 0) ? 0 : (v > 2 ? 2 : v);
    save_settings_conf();
    if (g_cfg.touch_scan != 1)
        y1_input_set_touch_scanning(g_cfg.touch_scan == 2);
}

static int get_vibrate_ms(void) { return g_cfg.vibrate_ms; }
static void set_vibrate_ms(int v)
{
    g_cfg.vibrate_ms = v;
    y1_vibrate(v);
    save_settings_conf();
}

static void act_vibrate_test(void)
{
    y1_vibrate(g_cfg.vibrate_ms);
    show_toast(TR_ID(STR_HAPTIC_PULSE_SENT), 1000);
}

#define CFG_WP_MAX 256
struct cfg_wp_entry {
    char name[128];
    char path[384];
    bool is_dir;
    bool is_clear;
};
static bool                g_wp_open = false;
static bool                g_picker_is_font = false;
static char                g_wp_dir[256] = "/mnt/sdcard";
static struct cfg_wp_entry g_wp_items[CFG_WP_MAX];
static int                 g_wp_count = 0;
static int                 g_wp_sel = 0;
static int                 g_wp_scroll = 0;
static float               g_wp_scroll_anim = 0.0f;

static int                 g_cached_cpu_mhz = 390;
static int                 g_cached_bat_pct = 100;
static bool                g_cached_bat_chg = false;

#define CFG_MAX_ITEM_PILLS 24
struct cfg_item_pill {
    int      scope;
    int      idx;
    float    cur;
    float    start;
    float    target;
    uint64_t start_ms;
    bool     active;
    bool     valid;
};
static struct cfg_item_pill g_item_pills[CFG_MAX_ITEM_PILLS];
static int g_item_pill_next = 0;

static void cfg_item_pill_clear_scope(int scope)
{
    for (int i = 0; i < CFG_MAX_ITEM_PILLS; i++) {
        if (g_item_pills[i].valid && g_item_pills[i].scope == scope)
            g_item_pills[i].valid = false;
    }
}

static float cfg_item_pill_get(int scope, int idx, bool sel, uint64_t tnow)
{
    float want = sel ? 1.0f : 0.0f;
    struct cfg_item_pill *a = NULL;
    for (int i = 0; i < CFG_MAX_ITEM_PILLS; i++) {
        if (g_item_pills[i].valid && g_item_pills[i].scope == scope && g_item_pills[i].idx == idx) {
            a = &g_item_pills[i];
            break;
        }
    }
    if (!a) {
        a = &g_item_pills[g_item_pill_next];
        g_item_pill_next = (g_item_pill_next + 1) % CFG_MAX_ITEM_PILLS;
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

static bool cfg_item_pill_update_all(uint64_t tnow)
{
    bool any = false;
    for (int i = 0; i < CFG_MAX_ITEM_PILLS; i++) {
        struct cfg_item_pill *a = &g_item_pills[i];
        if (!a->valid || !a->active) continue;
        uint64_t el = (tnow >= a->start_ms) ? (tnow - a->start_ms) : 0;
        if (el >= 400) {
            a->cur = a->target;
            a->active = false;
            any = true;
        } else {
            float t = (float)el / 400.0f;
            float inv = 1.0f - t;
            float ease = 1.0f - inv * inv * inv;
            a->cur = a->start + (a->target - a->start) * ease;
            any = true;
        }
    }
    return any;
}

static bool cfg_item_pill_any_active(void)
{
    for (int i = 0; i < CFG_MAX_ITEM_PILLS; i++) {
        if (g_item_pills[i].valid && g_item_pills[i].active) return true;
    }
    return false;
}

#define CFG_MAX_SW_ANIMS 16
struct cfg_sw_anim {
    int      page;
    int      idx;
    float    cur;
    float    start;
    float    target;
    uint64_t start_ms;
    bool     active;
    bool     valid;
};
static struct cfg_sw_anim g_sw_anims[CFG_MAX_SW_ANIMS];
static int g_sw_anim_next = 0;

static float cfg_sw_get(int page, int idx, float want, uint64_t tnow)
{
    struct cfg_sw_anim *sw = NULL;
    for (int i = 0; i < CFG_MAX_SW_ANIMS; i++) {
        if (g_sw_anims[i].valid && g_sw_anims[i].page == page && g_sw_anims[i].idx == idx) {
            sw = &g_sw_anims[i];
            break;
        }
    }
    if (!sw) {
        sw = &g_sw_anims[g_sw_anim_next];
        g_sw_anim_next = (g_sw_anim_next + 1) % CFG_MAX_SW_ANIMS;
        sw->page = page;
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

static bool cfg_sw_update_all(uint64_t tnow)
{
    bool any = false;
    for (int i = 0; i < CFG_MAX_SW_ANIMS; i++) {
        struct cfg_sw_anim *sw = &g_sw_anims[i];
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

static bool cfg_sw_any_active(void)
{
    for (int i = 0; i < CFG_MAX_SW_ANIMS; i++) {
        if (g_sw_anims[i].valid && g_sw_anims[i].active) return true;
    }
    return false;
}

static uint32_t cfg_lerp_argb(uint32_t c0, uint32_t c1, float t)
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

static bool cfg_has_image_ext(const char *name)
{
    size_t n = strlen(name);
    if (n >= 4 && (strcasecmp(name + n - 4, ".png") == 0 || strcasecmp(name + n - 4, ".jpg") == 0))
        return true;
    if (n >= 5 && strcasecmp(name + n - 5, ".jpeg") == 0)
        return true;
    return false;
}

static bool cfg_has_font_ext(const char *name)
{
    size_t n = strlen(name);
    if (n >= 4 && (strcasecmp(name + n - 4, ".ttf") == 0 || strcasecmp(name + n - 4, ".otf") == 0))
        return true;
    return false;
}

static int cfg_cmp_wp(const void *a, const void *b)
{
    const struct cfg_wp_entry *ea = (const struct cfg_wp_entry *)a;
    const struct cfg_wp_entry *eb = (const struct cfg_wp_entry *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcasecmp(ea->name, eb->name);
}

static void cfg_scan_wp_dir(const char *dir)
{
    snprintf(g_wp_dir, sizeof(g_wp_dir), "%s", dir);
    g_wp_count = 0;
    if (!g_picker_is_font && gfx_has_wallpaper() && g_wp_count < CFG_WP_MAX) {
        snprintf(g_wp_items[g_wp_count].name, sizeof(g_wp_items[0].name), "%s",
                 TR_ID(STR_CLEAR_WALLPAPER));
        g_wp_items[g_wp_count].path[0] = '\0';
        g_wp_items[g_wp_count].is_dir = false;
        g_wp_items[g_wp_count].is_clear = true;
        g_wp_count++;
    } else if (g_picker_is_font && gfx_has_custom_font() && g_wp_count < CFG_WP_MAX) {
        snprintf(g_wp_items[g_wp_count].name, sizeof(g_wp_items[0].name), "%s",
                 TR_ID(STR_RESET_DEFAULT_FONT));
        g_wp_items[g_wp_count].path[0] = '\0';
        g_wp_items[g_wp_count].is_dir = false;
        g_wp_items[g_wp_count].is_clear = true;
        g_wp_count++;
    }
    if (strcmp(g_wp_dir, "/mnt/sdcard") != 0 && g_wp_count < CFG_WP_MAX) {
        snprintf(g_wp_items[g_wp_count].name, sizeof(g_wp_items[0].name), "..");
        snprintf(g_wp_items[g_wp_count].path, sizeof(g_wp_items[0].path), "%s", g_wp_dir);
        char *sl = strrchr(g_wp_items[g_wp_count].path, '/');
        if (sl && sl != g_wp_items[g_wp_count].path) *sl = '\0';
        else snprintf(g_wp_items[g_wp_count].path, sizeof(g_wp_items[0].path), "/mnt/sdcard");
        g_wp_items[g_wp_count].is_dir = true;
        g_wp_items[g_wp_count].is_clear = false;
        g_wp_count++;
    }
    int sort_start = g_wp_count;
    DIR *d = opendir(g_wp_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_wp_count < CFG_WP_MAX) {
        if (de->d_name[0] == '.') continue;
        char full[384];
        snprintf(full, sizeof(full), "%s/%s", g_wp_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        bool match = g_picker_is_font ? cfg_has_font_ext(de->d_name) : cfg_has_image_ext(de->d_name);
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
        qsort(&g_wp_items[sort_start], (size_t)(g_wp_count - sort_start), sizeof(struct cfg_wp_entry), cfg_cmp_wp);
    g_wp_sel = 0; g_wp_scroll = 0; g_wp_scroll_anim = 0.0f;
    cfg_item_pill_clear_scope(100);
}

static void act_open_wallpaper(void)
{
    g_picker_is_font = false;
    cfg_scan_wp_dir("/mnt/sdcard");
    g_wp_open = true;
}

static void act_open_font(void)
{
    g_picker_is_font = true;
    cfg_scan_wp_dir("/mnt/sdcard");
    g_wp_open = true;
}

static int get_cpu_mode(void)
{
    int idx = g_cfg.cpu_mode + 1;
    return (idx < 0 || idx > 5) ? 0 : idx;
}
static void set_cpu_mode(int v)
{
    g_cfg.cpu_mode = (v <= 0) ? -1 : ((v > 5) ? 4 : (v - 1));
    save_settings_conf();
    if (g_cfg.cpu_mode >= 0)
        y1_cpu_set_mode(g_cfg.cpu_mode);
}

static int get_dual_core(void) { return g_cfg.dual_core; }
static void set_dual_core(int v)
{
    g_cfg.dual_core = (v < 0) ? 0 : (v > 2 ? 2 : v);
    save_settings_conf();
    if (g_cfg.dual_core != 1)
        y1_cpu_set_dual_core(g_cfg.dual_core == 2);
}

static int get_screen_clamp(void) { return g_cfg.screen_off_clamp; }
static void set_screen_clamp(int v)
{
    g_cfg.screen_off_clamp = (v < 0) ? 0 : (v > 2 ? 2 : v);
    save_settings_conf();
    if (g_cfg.screen_off_clamp != 1)
        y1_cpu_set_screen_off_clamp(g_cfg.screen_off_clamp == 2);
}

static int get_dt_year(void) { if (!dt_loaded) load_datetime_from_sys(); return dt_year; }
static void set_dt_year(int v) { dt_year = v; clamp_day(); }

static int get_dt_month(void) { if (!dt_loaded) load_datetime_from_sys(); return dt_month; }
static void set_dt_month(int v) { dt_month = v; clamp_day(); }

static int get_dt_day(void) { if (!dt_loaded) load_datetime_from_sys(); return dt_day; }
static void set_dt_day(int v) { dt_day = v; clamp_day(); }

static int get_dt_hour12(void) { if (!dt_loaded) load_datetime_from_sys(); return dt_hour12; }
static void set_dt_hour12(int v) { dt_hour12 = v; }

static int get_dt_minute(void) { if (!dt_loaded) load_datetime_from_sys(); return dt_minute; }
static void set_dt_minute(int v) { dt_minute = v; }

static int get_dt_ampm(void) { if (!dt_loaded) load_datetime_from_sys(); return dt_ampm; }
static void set_dt_ampm(int v) { dt_ampm = v ? 1 : 0; }

static void info_dt_preview(char *buf, size_t len)
{
    if (!dt_loaded) load_datetime_from_sys();
    if (g_cfg.lang == 1) {
        snprintf(buf, len, "%04d-%02d-%02d %s %d:%02d",
                 dt_year, dt_month, dt_day,
                 ampm_choices_ko[dt_ampm ? 1 : 0], dt_hour12, dt_minute);
    } else {
        snprintf(buf, len, "%04d-%02d-%02d %d:%02d %s",
                 dt_year, dt_month, dt_day,
                 dt_hour12, dt_minute, dt_ampm ? "PM" : "AM");
    }
}

static int get_usb_ipod(void) { return g_cfg.usb_ipod_mode; }
static void set_usb_ipod(int v)
{
    g_cfg.usb_ipod_mode = (v < 0) ? 0 : (v > 2 ? 2 : v);
    save_settings_conf();
    if (g_cfg.usb_ipod_mode != 1)
        y1_usb_set_ipod_mode(g_cfg.usb_ipod_mode == 2);
}

static int get_usb_perf(void) { return g_cfg.usb_perf_tune; }
static void set_usb_perf(int v)
{
    g_cfg.usb_perf_tune = v ? 1 : 0;
    if (y1_usb_is_storage_attached())
        y1_usb_tune_performance(g_cfg.usb_perf_tune != 0);
    save_settings_conf();
}

static int get_usb_ums(void) { return y1_usb_is_storage_attached() ? 1 : 0; }
static void set_usb_ums(int v)
{
    if (v) {
        if (y1_usb_attach_mass_storage())
            show_toast(TR_ID(STR_USB_STORAGE_ATTACHED), 1400);
        else
            show_toast(TR_ID(STR_USB_ATTACH_FAILED), 1400);
    } else {
        y1_usb_detach_mass_storage();
        show_toast(TR_ID(STR_USB_STORAGE_DETACHED), 1400);
    }
}

static void act_sync_disks(void)
{
    sync();
    show_toast(TR_ID(STR_FILESYSTEMS_SYNCED), 1200);
}

static void act_drop_caches(void)
{
    sync();
    y1_sysfs_write("/proc/sys/vm/drop_caches", "3\n");
    show_toast(TR_ID(STR_PAGE_CACHE_CLEARED), 1200);
}

static int get_lang(void) { return g_cfg.lang; }
static void set_lang(int v)
{
    g_cfg.lang = (v == 1) ? 1 : 0;
    save_settings_conf();
    show_toast(TR_ID(STR_LANG_CHANGED), 1000);
}

static int get_debug_log(void) { return g_cfg.debug_log; }
static void set_debug_log(int v)
{
    g_cfg.debug_log = v ? 1 : 0;
    y1_log_set_enabled(g_cfg.debug_log != 0);
    save_settings_conf();
}

static int get_screenshot(void) { return g_cfg.screenshot; }
static void set_screenshot(int v)
{
    g_cfg.screenshot = v ? 1 : 0;
    save_settings_conf();
    show_toast(g_cfg.screenshot ? TR_ID(STR_SCREENSHOT_ON_MENU_PLAY)
                                : TR_ID(STR_SCREENSHOT_OFF), 1400);
}

static void act_clear_log(void)
{
    unlink(Y1_DEBUG_LOG_PATH);
    show_toast(TR_ID(STR_DEBUG_LOG_CLEARED), 1200);
}

static int get_bt_enabled(void) { return y1_bt_is_enabled() ? 1 : 0; }
static void set_bt_enabled(int v)
{
    y1_bt_set_enabled(v != 0);
    show_toast(v ? TR_ID(STR_BLUETOOTH_ENABLED) : TR_ID(STR_BLUETOOTH_DISABLED), 1200);
}

static int get_bt_receiver(void) { return y1_bt_is_receiver_mode() ? 1 : 0; }
static void set_bt_receiver(int v)
{
    y1_bt_set_receiver_mode(v != 0);
    show_toast(v ? TR_ID(STR_BT_RECEIVER_MODE_ON)
                 : TR_ID(STR_BT_RECEIVER_MODE_OFF), 1200);
}

static int get_bt_codec(void) { return g_cfg.bt_codec; }
static void set_bt_codec(int v)
{
    g_cfg.bt_codec = v;
    y1_bt_set_active_device_codec(v);
    save_settings_conf();
}

static void act_bt_scan(void)
{
    y1_bt_start_scan();
    show_toast(TR_ID(STR_SCANNING_FOR_BLUETOOTH_DEVICES), 1500);
}

static void act_bt_disconnect(void)
{
    y1_bt_disconnect();
    show_toast(TR_ID(STR_DISCONNECTED), 1200);
}

static void info_bt_status(char *buf, size_t len)
{
    snprintf(buf, len, "%s", y1_bt_get_status_str());
}

static void act_return_bootmenu(void) { g_should_exit = true; }
static void act_reboot(void) { save_settings_conf(); y1_sys_reboot(); }
static void act_poweroff(void) { save_settings_conf(); y1_sys_poweroff(); }

static int get_default_player(void)
{
    FILE *f = fopen("/mnt/sdcard/.system/default_player", "r");
    if (!f) return 2; /* Default to ConfigApp */
    char buf[64] = "";
    if (fgets(buf, sizeof(buf), f)) {
        for (char *p = buf; *p; p++) {
            if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') { *p = '\0'; break; }
        }
    }
    fclose(f);
    if (strcasecmp(buf, "rockbox") == 0) return 0;
    if (strcasecmp(buf, "meui") == 0) return 1;
    if (strcasecmp(buf, "configapp") == 0) return 2;
    return 2;
}

static void set_default_player(int v)
{
    mkdir("/mnt/sdcard/.system", 0777);
    const char *val = (v == 1) ? "meui" : (v == 2) ? "configapp" : "rockbox";
    FILE *f = fopen("/mnt/sdcard/.system/default_player", "w");
    if (f) {
        fprintf(f, "%s\n", val);
        fclose(f);
        sync();
    }
    show_toast(v == 1 ? TR_ID(STR_DEFAULT_MEUI)
                      : (v == 2 ? TR_ID(STR_DEFAULT_CONFIGAPP)
                                : TR_ID(STR_DEFAULT_ROCKBOX)), 1200);
}

static void act_reset_settings(void)
{
    unlink(SETTINGS_CONF_PATH);
    unlink(SETTINGS_CONF_FALLBACK);
    g_cfg.lang = 0;
    g_cfg.dark_mode = 0;
    g_cfg.brightness_pct = 65;
    g_cfg.eco_mode = 1;
    g_cfg.touch_scan = 1;
    g_cfg.vibrate_ms = 20;
    g_cfg.cpu_mode = -1;
    g_cfg.dual_core = 1;
    g_cfg.screen_off_clamp = 1;
    g_cfg.usb_ipod_mode = 1;
    g_cfg.usb_perf_tune = 1;
    g_cfg.debug_log = 0;
    g_cfg.bt_codec = Y1_BT_CODEC_SBC;
    g_cfg.screenshot = 0;
    apply_all_settings();
    save_settings_conf();
    show_toast(TR_ID(STR_SETTINGS_RESET_TO_DEFAULTS), 1400);
}

/* =========================================================================
 * Launchable Hidden Software Scanner
 * ========================================================================= */
#define MAX_LAUNCH_APPS 24
struct launch_entry {
    char name[64];
    char folder[64];
    char exec_path[256];
};
static struct launch_entry g_launch_apps[MAX_LAUNCH_APPS];
static int g_launch_count = 0;

static bool is_excluded_hidden_dir(const char *dname)
{
    if (strcmp(dname, ".") == 0 || strcmp(dname, "..") == 0)
        return true;
    if (strcmp(dname, ".system") == 0 || strcmp(dname, ".fseventsd") == 0 ||
        strcmp(dname, ".Spotlight-V100") == 0 || strncmp(dname, ".Trash", 6) == 0)
        return true;
    return false;
}

static bool find_exec_in_dir(const char *dir_path, const char *dir_name, char *out_exec, size_t out_sz)
{
    const char *base = (dir_name[0] == '.') ? dir_name + 1 : dir_name;
    const char *preferred[] = { "launch", base, "rockbox", "meui", NULL };
    for (int i = 0; preferred[i]; i++) {
        if (!preferred[i][0]) continue;
        snprintf(out_exec, out_sz, "%s/%s", dir_path, preferred[i]);
        struct stat st;
        if (stat(out_exec, &st) == 0 && S_ISREG(st.st_mode) && access(out_exec, X_OK) == 0)
            return true;
    }

    DIR *d = opendir(dir_path);
    if (!d) return false;
    struct dirent *de;
    bool found = false;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(out_exec, out_sz, "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (stat(out_exec, &st) == 0 && S_ISREG(st.st_mode) && access(out_exec, X_OK) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void scan_launch_apps(void)
{
    g_launch_count = 0;
    const char *roots[] = { "/mnt/sdcard", "/", NULL };
    for (int r = 0; roots[r]; r++) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL && g_launch_count < MAX_LAUNCH_APPS - 1) {
            if (!strchr(de->d_name, '.') || is_excluded_hidden_dir(de->d_name))
                continue;
            char dir_path[256];
            if (strcmp(roots[r], "/") == 0)
                snprintf(dir_path, sizeof(dir_path), "/%s", de->d_name);
            else
                snprintf(dir_path, sizeof(dir_path), "%s/%s", roots[r], de->d_name);

            struct stat st;
            if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;

            char exec_path[256];
            if (!find_exec_in_dir(dir_path, de->d_name, exec_path, sizeof(exec_path)))
                continue;

            bool dup = false;
            for (int k = 0; k < g_launch_count; k++) {
                if (strcmp(g_launch_apps[k].folder, de->d_name) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            const char *disp = (de->d_name[0] == '.') ? de->d_name + 1 : de->d_name;
            snprintf(g_launch_apps[g_launch_count].name, sizeof(g_launch_apps[0].name),
                     "%s", disp[0] ? disp : de->d_name);
            if (g_launch_apps[g_launch_count].name[0] >= 'a' &&
                g_launch_apps[g_launch_count].name[0] <= 'z') {
                g_launch_apps[g_launch_count].name[0] -= 32;
            }
            snprintf(g_launch_apps[g_launch_count].folder, sizeof(g_launch_apps[0].folder),
                     "%s", de->d_name);
            snprintf(g_launch_apps[g_launch_count].exec_path, sizeof(g_launch_apps[0].exec_path),
                     "%s", exec_path);
            g_launch_count++;
        }
        closedir(d);
    }

    if (g_launch_count < MAX_LAUNCH_APPS) {
        snprintf(g_launch_apps[g_launch_count].name, sizeof(g_launch_apps[0].name), "Android");
        snprintf(g_launch_apps[g_launch_count].folder, sizeof(g_launch_apps[0].folder), "Stock ROM");
        snprintf(g_launch_apps[g_launch_count].exec_path, sizeof(g_launch_apps[0].exec_path), "__ANDROID__");
        g_launch_count++;
    }
}

/* =========================================================================
 * Serial Terminal Controller (Dual USB ACM ttyGS0 / ttyGS1)
 * ========================================================================= */
static pid_t g_serial_pid1 = -1;
static pid_t g_serial_pid2 = -1;
static bool  g_serial_active = false;

static void serial_shell_worker(const char *tty_name, int tab_num)
{
    char sys_path[64];
    snprintf(sys_path, sizeof(sys_path), "/sys/class/tty/%s/dev", tty_name);
    char dev_str[32] = {0};
    for (int r = 0; r < 50; r++) {
        if (y1_sysfs_read(sys_path, dev_str, sizeof(dev_str)) && dev_str[0])
            break;
        usleep(100000);
    }
    char dev_path[64];
    snprintf(dev_path, sizeof(dev_path), "/dev/%s", tty_name);
    int maj = 0, min = 0;
    if (sscanf(dev_str, "%d:%d", &maj, &min) == 2) {
        unlink(dev_path);
        mknod(dev_path, S_IFCHR | 0666, makedev(maj, min));
    }

    while (access("/tmp/exit_terminal", F_OK) != 0) {
        int fd = open(dev_path, O_RDWR);
        if (fd < 0) {
            usleep(250000);
            continue;
        }
        struct termios tio;
        if (tcgetattr(fd, &tio) == 0) {
            tio.c_iflag |= (ICRNL | IXON);
            tio.c_oflag |= (OPOST | ONLCR);
            tio.c_lflag |= (ISIG | ICANON | ECHO | ECHOE | ECHOK);
            tcsetattr(fd, TCSANOW, &tio);
        }
        char banner[256];
        snprintf(banner, sizeof(banner),
                 "\r\nNugget Y1 Serial Console [Tab %d: /dev/%s]\r\nType 'exit' to close.\r\n\r\n",
                 tab_num, tty_name);
        (void)!write(fd, banner, strlen(banner));

        pid_t sh_pid = fork();
        if (sh_pid == 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
            setsid();
            ioctl(0, TIOCSCTTY, 1);
            setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/mnt/sdcard/.system/bin", 1);
            setenv("LD_LIBRARY_PATH", "/mnt/sdcard/.system/lib:/.system/lib:/lib", 1);
            setenv("HOME", "/mnt/sdcard", 1);
            setenv("TERM", "vt100", 1);
            (void)!chdir("/mnt/sdcard");
            char *const argv[] = { "/bin/sh", "-i", NULL };
            execv("/bin/sh", argv);
            execv("/bin/busybox", argv);
            _exit(127);
        }
        close(fd);
        if (sh_pid > 0) {
            int st = 0;
            waitpid(sh_pid, &st, 0);
        }
        usleep(100000);
    }
}

static void stop_serial_terminal(void)
{
    if (!g_serial_active)
        return;
    int ef = open("/tmp/exit_terminal", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ef >= 0) close(ef);
    if (g_serial_pid1 > 0) { kill(g_serial_pid1, SIGTERM); waitpid(g_serial_pid1, NULL, WNOHANG); g_serial_pid1 = -1; }
    if (g_serial_pid2 > 0) { kill(g_serial_pid2, SIGTERM); waitpid(g_serial_pid2, NULL, WNOHANG); g_serial_pid2 = -1; }
    unlink("/tmp/exit_terminal");
    y1_sysfs_write("/sys/class/android_usb/android0/enable", "0\n");
    y1_sysfs_write("/sys/class/android_usb/android0/bDeviceClass", "0\n");
    g_serial_active = false;
}

static int get_serial_active(void) { return g_serial_active ? 1 : 0; }
static void set_serial_active(int v)
{
    if (v && !g_serial_active) {
        unlink("/tmp/exit_terminal");
        y1_sysfs_write("/sys/class/android_usb/android0/enable", "0\n");
        y1_sysfs_write("/sys/class/android_usb/android0/idVendor", "0e8d\n");
        y1_sysfs_write("/sys/class/android_usb/android0/idProduct", "2007\n");
        y1_sysfs_write("/sys/class/android_usb/android0/f_acm/instances", "2\n");
        y1_sysfs_write("/sys/class/android_usb/android0/functions", "acm\n");
        y1_sysfs_write("/sys/class/android_usb/android0/bDeviceClass", "0\n");
        y1_sysfs_write("/sys/class/android_usb/android0/bDeviceSubClass", "0\n");
        y1_sysfs_write("/sys/class/android_usb/android0/bDeviceProtocol", "0\n");
        y1_sysfs_write("/sys/class/android_usb/android0/enable", "1\n");

        g_serial_pid2 = fork();
        if (g_serial_pid2 == 0) { setpgid(0, 0); serial_shell_worker("ttyGS1", 2); _exit(0); }
        g_serial_pid1 = fork();
        if (g_serial_pid1 == 0) { setpgid(0, 0); serial_shell_worker("ttyGS0", 1); _exit(0); }
        g_serial_active = true;
        show_toast(TR_ID(STR_SERIAL_CONSOLE_STARTED_TTYGS0_1), 1400);
    } else if (!v && g_serial_active) {
        stop_serial_terminal();
        show_toast(TR_ID(STR_SERIAL_CONSOLE_STOPPED), 1200);
    }
}

static void info_serial_ports(char *buf, size_t len)
{
    snprintf(buf, len, "%s",
             g_serial_active ? TR_ID(STR_ACTIVE_DEV_TTYGS0_TTYGS1)
                             : TR_ID(STR_IDLE));
}

static pid_t g_adb_pid = -1;
static bool  g_adb_active = false;

static const char *find_or_extract_adbd(void)
{
    static const char *cands[] = {
        "/sbin/adbd",
        "/mnt/sdcard/.system/bin/adbd",
        "/mnt/sdcard/.system/adbd",
        "/tmp/adbd",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        if (access(cands[i], F_OK) == 0) {
            chmod(cands[i], 0755);
            return cands[i];
        }
    }
    if (access("/mnt/sdcard/.system/ramdisk_android.cpio.gz", R_OK) == 0) {
        (void)!system("mkdir -p /tmp/adb_ext && (cd /tmp/adb_ext && gzip -dc /mnt/sdcard/.system/ramdisk_android.cpio.gz | cpio -idmu sbin/adbd 2>/dev/null) && mv /tmp/adb_ext/sbin/adbd /tmp/adbd 2>/dev/null && chmod 755 /tmp/adbd; rm -rf /tmp/adb_ext");
        if (access("/tmp/adbd", X_OK) == 0)
            return "/tmp/adbd";
    }
    return NULL;
}

static int get_adb_active(void) { return g_adb_active ? 1 : 0; }
static void set_adb_active(int v)
{
    if (v && !g_adb_active) {
        const char *adbd_bin = find_or_extract_adbd();
        if (!adbd_bin) {
            show_toast(TR_ID(STR_ADB_NOT_FOUND), 1400);
            return;
        }
        mkdir("/dev/pts", 0755);
        mount("devpts", "/dev/pts", "devpts", 0, NULL);
        if (access("/dev/ptmx", F_OK) != 0)
            mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2));
        chmod("/dev/ptmx", 0666);

        mkdir("/system", 0777);
        mkdir("/system/bin", 0777);
        if (access("/system/bin/sh", F_OK) != 0)
            (void)!symlink("/bin/sh", "/system/bin/sh");

        char dev_str[32] = {0};
        if (y1_sysfs_read("/sys/class/misc/android_adb/dev", dev_str, sizeof(dev_str))) {
            int maj = 0, min = 0;
            if (sscanf(dev_str, "%d:%d", &maj, &min) == 2) {
                unlink("/dev/android_adb");
                mknod("/dev/android_adb", S_IFCHR | 0666, makedev(maj, min));
            }
        }

        y1_sysfs_write("/sys/class/android_usb/android0/enable", "0\n");
        if (g_serial_active) {
            y1_sysfs_write("/sys/class/android_usb/android0/idVendor", "0e8d\n");
            y1_sysfs_write("/sys/class/android_usb/android0/idProduct", "201c\n");
            y1_sysfs_write("/sys/class/android_usb/android0/f_acm/instances", "2\n");
            y1_sysfs_write("/sys/class/android_usb/android0/functions", "acm,adb\n");
        } else {
            y1_sysfs_write("/sys/class/android_usb/android0/idVendor", "18d1\n");
            y1_sysfs_write("/sys/class/android_usb/android0/idProduct", "4e11\n");
            y1_sysfs_write("/sys/class/android_usb/android0/functions", "adb\n");
        }
        y1_sysfs_write("/sys/class/android_usb/android0/bDeviceClass", "0\n");
        y1_sysfs_write("/sys/class/android_usb/android0/bDeviceSubClass", "0\n");
        y1_sysfs_write("/sys/class/android_usb/android0/bDeviceProtocol", "0\n");
        y1_sysfs_write("/sys/class/android_usb/android0/enable", "1\n");

        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
            execl(adbd_bin, "adbd", (char *)NULL);
            _exit(127);
        }
        g_adb_pid = pid;
        g_adb_active = true;
        show_toast(TR_ID(STR_ADB_STARTED), 1400);
    } else if (!v && g_adb_active) {
        if (g_adb_pid > 0) {
            kill(g_adb_pid, SIGTERM);
            usleep(100000);
            kill(g_adb_pid, SIGKILL);
            waitpid(g_adb_pid, NULL, WNOHANG);
            g_adb_pid = -1;
        }
        (void)!system("killall adbd 2>/dev/null");
        y1_sysfs_write("/sys/class/android_usb/android0/enable", "0\n");
        if (g_serial_active) {
            y1_sysfs_write("/sys/class/android_usb/android0/idVendor", "0e8d\n");
            y1_sysfs_write("/sys/class/android_usb/android0/idProduct", "2007\n");
            y1_sysfs_write("/sys/class/android_usb/android0/f_acm/instances", "2\n");
            y1_sysfs_write("/sys/class/android_usb/android0/functions", "acm\n");
            y1_sysfs_write("/sys/class/android_usb/android0/enable", "1\n");
        }
        g_adb_active = false;
        show_toast(TR_ID(STR_ADB_STOPPED), 1200);
    }
}

/* =========================================================================
 * Debug Info Collector (ConfigApp Native UI + Full SD Export)
 * ========================================================================= */
#define MAX_DBG_LINES 512
#define MAX_DBG_LINE_LEN 256
static char g_dbg_lines[MAX_DBG_LINES][MAX_DBG_LINE_LEN];
static int  g_dbg_count = 0;

static void dbg_add_line(const char *s)
{
    if (!s || g_dbg_count >= MAX_DBG_LINES)
        return;
    char *dst = g_dbg_lines[g_dbg_count++];
    size_t i = 0;
    while (s[i] && s[i] != '\r' && s[i] != '\n' && i < MAX_DBG_LINE_LEN - 1) {
        dst[i] = (s[i] == '\t') ? ' ' : s[i];
        i++;
    }
    dst[i] = '\0';
}

static void dbg_add_linef(const char *fmt, ...)
{
    if (g_dbg_count >= MAX_DBG_LINES)
        return;
    char tmp[MAX_DBG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    dbg_add_line(tmp);
}

static void dbg_dump_mmc_host(const char *host)
{
    char host_dir[96];
    snprintf(host_dir, sizeof(host_dir), "/sys/class/mmc_host/%s", host);
    DIR *d = opendir(host_dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, host, strlen(host)) == 0 && strchr(de->d_name, ':')) {
            char p[160];
            char name[32] = "?", manf[16] = "?", oem[16] = "?", prv[16] = "?",
                 hw[16] = "?", fw[16] = "?", type[16] = "?";
            snprintf(p, sizeof(p), "%s/%s/name", host_dir, de->d_name);
            y1_sysfs_read(p, name, sizeof(name));
            snprintf(p, sizeof(p), "%s/%s/manfid", host_dir, de->d_name);
            y1_sysfs_read(p, manf, sizeof(manf));
            snprintf(p, sizeof(p), "%s/%s/oemid", host_dir, de->d_name);
            y1_sysfs_read(p, oem, sizeof(oem));
            snprintf(p, sizeof(p), "%s/%s/prv", host_dir, de->d_name);
            y1_sysfs_read(p, prv, sizeof(prv));
            snprintf(p, sizeof(p), "%s/%s/hwrev", host_dir, de->d_name);
            y1_sysfs_read(p, hw, sizeof(hw));
            snprintf(p, sizeof(p), "%s/%s/fwrev", host_dir, de->d_name);
            y1_sysfs_read(p, fw, sizeof(fw));
            snprintf(p, sizeof(p), "%s/%s/type", host_dir, de->d_name);
            y1_sysfs_read(p, type, sizeof(type));
            dbg_add_linef("%s(%s): type=%s name=%s manf=%s oem=%s prv=%s hw=%s fw=%s",
                          host, de->d_name, type, name, manf, oem, prv, hw, fw);
        }
    }
    closedir(d);
}

static void dbg_dump_dir_names(const char *label, const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (!d)
        return;
    char line[MAX_DBG_LINE_LEN];
    int pos = snprintf(line, sizeof(line), "%s=", label);
    struct dirent *de;
    int first = 1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (pos < (int)sizeof(line) - 2)
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%s%s",
                            first ? "" : ",", de->d_name);
        first = 0;
    }
    if (first && pos < (int)sizeof(line) - 8)
        snprintf(line + pos, sizeof(line) - (size_t)pos, "(empty)");
    closedir(d);
    dbg_add_line(line);
}

static long dbg_file_kb(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)((st.st_size + 1023) / 1024);
}

static void dbg_append_file_tail(const char *label, const char *path, size_t max_bytes)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return;
    }
    size_t read_sz = (size_t)sz;
    if (read_sz > max_bytes) {
        read_sz = max_bytes;
        fseek(fp, sz - (long)max_bytes, SEEK_SET);
    } else {
        fseek(fp, 0, SEEK_SET);
    }
    char *buf = malloc(read_sz + 1);
    if (buf) {
        size_t n = fread(buf, 1, read_sz, fp);
        buf[n] = '\0';
        dbg_add_linef("[%s (last %zu/%ld bytes)]", label, n, sz);
        char *p = buf;
        while (p && *p && g_dbg_count < MAX_DBG_LINES) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (*p) dbg_add_line(p);
            p = nl ? (nl + 1) : NULL;
        }
        free(buf);
    }
    fclose(fp);
}

static void refresh_debug_info(void)
{
    g_dbg_count = 0;
    time_t now = time(NULL);
    struct tm tm_now;
    char date_iso[32] = "unknown";
    if (localtime_r(&now, &tm_now))
        strftime(date_iso, sizeof(date_iso), "%Y-%m-%d %H:%M:%S", &tm_now);

    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    dbg_add_line("[build]");
    dbg_add_linef("date=%s", date_iso);
    dbg_add_linef("configapp_compiled=%s %s", __DATE__, __TIME__);
    dbg_add_linef("kernel=%s %s %s %s", uts.sysname, uts.release, uts.version, uts.machine);

    char buf[512];
    if (y1_sysfs_read("/proc/cmdline", buf, sizeof(buf)))
        dbg_add_linef("cmdline=%s", buf);

    /* Hardware */
    dbg_add_line("[hardware]");
    FILE *cpufp = fopen("/proc/cpuinfo", "r");
    if (cpufp) {
        char line[160];
        while (fgets(line, sizeof(line), cpufp)) {
            if (strncmp(line, "Processor", 9) == 0 ||
                strncmp(line, "Hardware", 8) == 0 ||
                strncmp(line, "Revision", 8) == 0 ||
                strncmp(line, "Serial", 6) == 0 ||
                strncmp(line, "BogoMIPS", 8) == 0) {
                dbg_add_line(line);
            }
        }
        fclose(cpufp);
    }

    int fbfd = open("/dev/graphics/fb0", O_RDONLY | O_CLOEXEC);
    if (fbfd >= 0) {
        struct fb_var_screeninfo vinfo;
        struct fb_fix_screeninfo finfo;
        if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
            ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == 0) {
            dbg_add_linef("fb0=%s %ux%u@%ubpp stride=%u smem=%u",
                          finfo.id, vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
                          finfo.line_length, finfo.smem_len);
        }
        close(fbfd);
    }
    if (y1_sysfs_read("/proc/lcm", buf, sizeof(buf)))
        dbg_add_linef("lcm=%s", buf);

    dbg_dump_mmc_host("mmc0");
    dbg_dump_mmc_host("mmc1");

    FILE *ifp = fopen("/proc/bus/input/devices", "r");
    if (ifp) {
        char line[256];
        while (fgets(line, sizeof(line), ifp)) {
            if (strncmp(line, "I: ", 3) == 0 ||
                strncmp(line, "N: ", 3) == 0 ||
                strncmp(line, "P: ", 3) == 0 ||
                strncmp(line, "H: ", 3) == 0) {
                dbg_add_line(line);
            }
        }
        fclose(ifp);
    }

    /* System State */
    dbg_add_line("[system_state]");
    char uptime[64] = "?", cpu0_freq[32] = "?", gov[32] = "?", cpu1_on[16] = "?";
    y1_sysfs_read("/proc/uptime", uptime, sizeof(uptime));
    y1_sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", cpu0_freq, sizeof(cpu0_freq));
    y1_sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov, sizeof(gov));
    y1_sysfs_read("/sys/devices/system/cpu/cpu1/online", cpu1_on, sizeof(cpu1_on));
    dbg_add_linef("uptime=%s cpu0_khz=%s gov=%s cpu1_online=%s", uptime, cpu0_freq, gov, cpu1_on);

    FILE *mfp = fopen("/proc/meminfo", "r");
    if (mfp) {
        char line[128];
        while (fgets(line, sizeof(line), mfp)) {
            if (strncmp(line, "MemTotal:", 9) == 0 ||
                strncmp(line, "MemFree:", 8) == 0 ||
                strncmp(line, "MemAvailable:", 13) == 0) {
                dbg_add_line(line);
            }
        }
        fclose(mfp);
    }

    char bstat[32] = "?", usb_on[16] = "?";
    y1_sysfs_read("/sys/class/power_supply/battery/status", bstat, sizeof(bstat));
    y1_sysfs_read("/sys/class/power_supply/usb/online", usb_on, sizeof(usb_on));
    dbg_add_linef("battery=%d%% status=%s volt=%dmV curr=%dmA usb_online=%s",
                  y1_battery_percent(), bstat, y1_battery_voltage_mv(),
                  y1_battery_current_ma(), usb_on);

    char wlock[128] = "?", h2w[16] = "?", tpd[16] = "?";
    y1_sysfs_read("/sys/power/wake_lock", wlock, sizeof(wlock));
    y1_sysfs_read("/sys/class/switch/h2w/state", h2w, sizeof(h2w));
    y1_sysfs_read("/proc/tpd_keys_enable", tpd, sizeof(tpd));
    dbg_add_linef("wake_lock=[%s] h2w=%s tpd_keys=%s", wlock, h2w, tpd);

    char usb_en[16] = "?", usb_fn[64] = "?", usb_vid[16] = "?", usb_pid[16] = "?";
    y1_sysfs_read("/sys/class/android_usb/android0/enable", usb_en, sizeof(usb_en));
    y1_sysfs_read("/sys/class/android_usb/android0/functions", usb_fn, sizeof(usb_fn));
    y1_sysfs_read("/sys/class/android_usb/android0/idVendor", usb_vid, sizeof(usb_vid));
    y1_sysfs_read("/sys/class/android_usb/android0/idProduct", usb_pid, sizeof(usb_pid));
    dbg_add_linef("usb_en=%s usb_fn=%s vid=%s pid=%s ipod_cfg=%d dbg_cfg=%d",
                  usb_en, usb_fn, usb_vid, usb_pid, g_cfg.usb_ipod_mode, g_cfg.debug_log);

    /* Available Components */
    dbg_add_line("[available_components]");
    dbg_add_linef("dev: fb0=%d eac=%d fm=%d stpwmt=%d stpbt=%d ttyGS0=%d",
                  access("/dev/graphics/fb0", F_OK) == 0,
                  access("/dev/eac", F_OK) == 0,
                  access("/dev/fm", F_OK) == 0,
                  access("/dev/stpwmt", F_OK) == 0,
                  access("/dev/stpbt", F_OK) == 0,
                  access("/dev/ttyGS0", F_OK) == 0);
    dbg_add_linef("bin_kb: rockbox=%ld meui=%ld custom=%ld sys_app=%ld ipod_sync=%ld",
                  dbg_file_kb("/mnt/sdcard/.rockbox/rockbox"),
                  dbg_file_kb("/mnt/sdcard/.meui/meui"),
                  dbg_file_kb("/mnt/sdcard/.custom/launch"),
                  dbg_file_kb("/mnt/sdcard/.system/app"),
                  dbg_file_kb("/bin/ipod_sync"));
    dbg_add_linef("sys_kb: liby1=%ld liby1video=%ld android_ramdisk=%ld ipod_vpd_ko=%ld",
                  dbg_file_kb("/mnt/sdcard/.system/lib/liby1.so"),
                  dbg_file_kb("/mnt/sdcard/.system/lib/liby1video.so"),
                  dbg_file_kb("/mnt/sdcard/.system/ramdisk_android.cpio.gz"),
                  dbg_file_kb("/lib/modules/ipod_vpd.ko"));
    dbg_add_linef("itunes_kb: iTunesDB=%ld ArtworkDB=%ld PlayCounts=%ld",
                  dbg_file_kb("/mnt/sdcard/iPod_Control/iTunes/iTunesDB"),
                  dbg_file_kb("/mnt/sdcard/iPod_Control/Artwork/ArtworkDB"),
                  dbg_file_kb("/mnt/sdcard/iPod_Control/iTunes/Play Counts"));
    dbg_dump_dir_names("sys_bin", "/mnt/sdcard/.system/bin");
    dbg_dump_dir_names("bt_blobs", "/mnt/sdcard/.system/bt");

    /* Storage & Partitions */
    struct statvfs vfs;
    unsigned long long total_mb = 0, free_mb = 0;
    if (statvfs("/mnt/sdcard", &vfs) == 0) {
        total_mb = ((unsigned long long)vfs.f_blocks * vfs.f_frsize) >> 20;
        free_mb = ((unsigned long long)vfs.f_bavail * vfs.f_frsize) >> 20;
    }
    dbg_add_line("[storage]");
    dbg_add_linef("sd_total_mb=%llu sd_free_mb=%llu", total_mb, free_mb);

    FILE *pfp = fopen("/proc/partitions", "r");
    if (pfp) {
        char line[128];
        while (fgets(line, sizeof(line), pfp)) {
            if (line[0] != '\n' && line[0] != '\r')
                dbg_add_line(line);
        }
        fclose(pfp);
    }

    FILE *mntfp = fopen("/proc/mounts", "r");
    if (mntfp) {
        char line[256];
        dbg_add_line("[mounts]");
        while (fgets(line, sizeof(line), mntfp)) {
            if (strstr(line, "/mnt/sdcard") || strstr(line, "mmcblk"))
                dbg_add_line(line);
        }
        fclose(mntfp);
    }

    FILE *modfp = fopen("/proc/modules", "r");
    if (modfp) {
        char line[256];
        dbg_add_line("[modules]");
        while (fgets(line, sizeof(line), modfp)) {
            char name[64];
            unsigned long sz = 0;
            if (sscanf(line, "%63s %lu", name, &sz) == 2)
                dbg_add_linef("%s (%lu)", name, sz);
        }
        fclose(modfp);
    }

    /* Kernel dmesg (hardware probes + tail) */
    char *kbuf = malloc(16384 + 1);
    if (kbuf) {
        int kn = klogctl(3, kbuf, 16384);
        if (kn > 0) {
            kbuf[kn] = '\0';

            dbg_add_line("[hw_probe_dmesg]");
            int probe_lines = 0;
            char *p = kbuf;
            while (p && *p && probe_lines < 25) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                if (strstr(p, "lcm") || strstr(p, "LCM") ||
                    strstr(p, "tpd") || strstr(p, "accdet") ||
                    strstr(p, "msdc") || strstr(p, "mt6323") ||
                    strstr(p, "mt6627") || strstr(p, "eac")) {
                    dbg_add_line(p);
                    probe_lines++;
                }
                if (nl) *nl = '\n';
                p = nl ? (nl + 1) : NULL;
            }

            const char *tail = (kn > 3072) ? (kbuf + kn - 3072) : kbuf;
            if (kn > 3072) {
                const char *nl = strchr(tail, '\n');
                if (nl) tail = nl + 1;
            }
            dbg_add_line("[dmesg_tail]");
            p = (char *)tail;
            while (p && *p && g_dbg_count < MAX_DBG_LINES) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                if (*p) dbg_add_line(p);
                p = nl ? (nl + 1) : NULL;
            }
        }
        free(kbuf);
    }

    dbg_append_file_tail("y1_debug.txt", "/mnt/sdcard/.system/y1_debug.txt", 2048);
}

static void act_save_debug_info(void)
{
    refresh_debug_info();
    time_t now = time(NULL);
    struct tm tm_now;
    char tag[32] = "00000000_000000";
    if (localtime_r(&now, &tm_now))
        strftime(tag, sizeof(tag), "%Y%m%d_%H%M%S", &tm_now);

    char path[128];
    snprintf(path, sizeof(path), "/mnt/sdcard/debuginfo%s.txt", tag);
    FILE *out = fopen(path, "w");
    if (!out) {
        show_toast(TR_ID(STR_SD_NOT_WRITABLE), 1400);
        return;
    }

    fprintf(out, "=== Nugget Y1 Debug Info ===\n");
    for (int i = 0; i < g_dbg_count; i++) {
        if (i > 0 && g_dbg_lines[i][0] == '[')
            fputc('\n', out);
        fprintf(out, "%s\n", g_dbg_lines[i]);
    }

    fflush(out);
    fsync(fileno(out));
    fclose(out);
    sync();
    show_toast(TR_ID(STR_SAVED_DEBUGINFO_TO_SD), 1500);
}

/* =========================================================================
 * Declarative Extendible Settings Tables
 * ========================================================================= */
static struct cfg_item status_items[] = {
    { NULL, STR_BATTERY_EN,    STR_BATTERY_KO,    STR_CHARGE_LEVEL_VOLTAGE_EN, STR_CHARGE_LEVEL_VOLTAGE_KO,     "battery-charging", CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_battery, NULL },    { NULL, STR_POWER_DRAW_EN, STR_POWER_DRAW_KO,STR_CURRENT_USB_POWER_EN,    STR_CURRENT_USB_POWER_KO, "activity",         CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_current, NULL },    { NULL, STR_PROCESSOR_EN,  STR_PROCESSOR_KO,  STR_LIVE_CLOCK_CORE_COUNT_EN,STR_LIVE_CLOCK_CORE_COUNT_KO,   "cpu",              CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_cpu,     NULL },    { NULL, STR_MEMORY_EN,     STR_MEMORY_KO,    STR_SYSTEM_RAM_USAGE_EN,       STR_SYSTEM_RAM_USAGE_KO,  "sliders",          CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_memory,  NULL },    { NULL, STR_SD_STORAGE_EN, STR_SD_STORAGE_KO, STR_MICROSD_FREE_SPACE_EN,     STR_MICROSD_FREE_SPACE_KO,     "hard-drive",       CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_storage, NULL },    { NULL, STR_KERNEL_EN,     STR_KERNEL_KO,      STR_LINUX_KERNEL_VERSION_EN,   STR_LINUX_KERNEL_VERSION_KO,      "info",             CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_kernel,  NULL },    { NULL, STR_UPTIME_EN,     STR_UPTIME_KO, STR_TIME_SINCE_BOOT_EN,        STR_TIME_SINCE_BOOT_KO,     "refresh-cw",       CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_uptime,  NULL },};

static struct cfg_item display_items[] = {
    { "dark_mode",      STR_DARK_MODE_EN,        STR_DARK_MODE_KO,       STR_DARK_MEUI_COLOR_THEME_EN,     STR_DARK_MEUI_COLOR_THEME_KO,  "sliders", CFG_TYPE_TOGGLE, 0, 1,   1, NULL, NULL, NULL, 0, get_dark_mode,  set_dark_mode,  NULL, NULL },    { NULL,             STR_WALLPAPER_EN,        STR_WALLPAPER_KO,        STR_PNG_JPG_BACKGROUND_IMAGE_EN,STR_PNG_JPG_BACKGROUND_IMAGE_KO,  "image",   CFG_TYPE_ACTION, 0, 0,   0, NULL, NULL, NULL, 0, NULL,           NULL,           NULL, act_open_wallpaper },    { NULL,             STR_FONT_EN,             STR_FONT_KO,            STR_CUSTOM_TTF_OTF_UI_FONT_EN,  STR_CUSTOM_TTF_OTF_UI_FONT_KO,  "sliders", CFG_TYPE_ACTION, 0, 0,   0, NULL, NULL, NULL, 0, NULL,           NULL,           NULL, act_open_font },    { "brightness_pct", STR_BRIGHTNESS_EN,       STR_BRIGHTNESS_KO,       STR_BACKLIGHT_INTENSITY_EN,       STR_BACKLIGHT_INTENSITY_KO,     "sun",     CFG_TYPE_INT,    5, 100, 5, "%",  NULL, NULL, 0, get_brightness, set_brightness, NULL, NULL },    { "eco_mode",       STR_DISPLAY_ECO_MODE_EN, STR_DISPLAY_ECO_MODE_KO, STR_PANEL_LOW_POWER_MODE_EN,      STR_PANEL_LOW_POWER_MODE_KO,       "sliders", CFG_TYPE_TRISTATE, 0, 2,  1, NULL, NULL, NULL, 0, get_eco_mode,   set_eco_mode,   NULL, NULL },    { "touch_scan",     STR_TOUCH_WHEEL_EN,      STR_TOUCH_WHEEL_KO,         STR_CAPACITIVE_WHEEL_SCANNING_EN, STR_CAPACITIVE_WHEEL_SCANNING_KO,  "activity",CFG_TYPE_TRISTATE, 0, 2,  1, NULL, NULL, NULL, 0, get_touch_scan, set_touch_scan, NULL, NULL },    { "vibrate_ms",     STR_HAPTIC_DURATION_EN,  STR_HAPTIC_DURATION_KO,       STR_MOTOR_PULSE_LENGTH_EN,        STR_MOTOR_PULSE_LENGTH_KO,    "vibrate", CFG_TYPE_INT,    5, 100, 5, "ms", NULL, NULL, 0, get_vibrate_ms, set_vibrate_ms, NULL, NULL },    { NULL,             STR_TEST_HAPTICS_EN,     STR_TEST_HAPTICS_KO,     STR_TRIGGER_VIBRATION_PULSE_EN,   STR_TRIGGER_VIBRATION_PULSE_KO,"vibrate", CFG_TYPE_ACTION, 0, 0,   0, NULL, NULL, NULL, 0, NULL,           NULL,           NULL, act_vibrate_test },};

static struct cfg_item cpu_items[] = {
    { "cpu_mode",         STR_CPU_GOVERNOR_EN,     STR_CPU_GOVERNOR_KO,      STR_FREQUENCY_SCALING_POLICY_EN, STR_FREQUENCY_SCALING_POLICY_KO,   "cpu",     CFG_TYPE_CHOICE,   0, 5, 1, NULL, cpu_mode_choices, cpu_mode_choices_ko, 6, get_cpu_mode,     set_cpu_mode,     NULL, NULL },    { "dual_core",        STR_DUAL_CORE_EN,        STR_DUAL_CORE_KO,       STR_ENABLE_SECOND_CORTEX_A7_EN,  STR_ENABLE_SECOND_CORTEX_A7_KO, "cpu",     CFG_TYPE_TRISTATE, 0, 2, 1, NULL, NULL,             NULL,                0, get_dual_core,    set_dual_core,    NULL, NULL },    { "screen_off_clamp", STR_SCREEN_OFF_CLAMP_EN, STR_SCREEN_OFF_CLAMP_KO,  STR_CLAMP_TO_390MHZ_OFF_SCREEN_EN,STR_CLAMP_TO_390MHZ_OFF_SCREEN_KO,"sliders",CFG_TYPE_TRISTATE, 0, 2, 1, NULL, NULL,             NULL,                0, get_screen_clamp, set_screen_clamp, NULL, NULL },    { NULL,               STR_LIVE_CLOCK_EN,       STR_LIVE_CLOCK_KO,       STR_CURRENT_CPU_FREQUENCY_EN,    STR_CURRENT_CPU_FREQUENCY_KO,      "activity",CFG_TYPE_INFO,   0, 0, 0, NULL, NULL,             NULL,                0, NULL,             NULL,             info_cpu, NULL },};

static struct cfg_item datetime_items[] = {
    { NULL, STR_PREVIEW_EN,      STR_PREVIEW_KO, STR_PENDING_DATE_TIME_EN,   STR_PENDING_DATE_TIME_KO, "clock", CFG_TYPE_INFO,   0,    0,    0, NULL, NULL,         NULL,            0, NULL,           NULL,           info_dt_preview, NULL },    { NULL, STR_YEAR_EN,         STR_YEAR_KO,      STR_CALENDAR_YEAR_EN,         STR_CALENDAR_YEAR_KO,           "clock", CFG_TYPE_INT,    2020, 2040, 1, NULL, NULL,         NULL,            0, get_dt_year,    set_dt_year,    NULL, NULL },    { NULL, STR_MONTH_EN,        STR_MONTH_KO,        STR_MONTH_1_12_EN,        STR_MONTH_1_12_KO,         "clock", CFG_TYPE_INT,    1,    12,   1, NULL, NULL,         NULL,            0, get_dt_month,   set_dt_month,   NULL, NULL },    { NULL, STR_DAY_EN,          STR_DAY_KO,        STR_DAY_OF_MONTH_EN,          STR_DAY_OF_MONTH_KO,             "clock", CFG_TYPE_INT,    1,    31,   1, NULL, NULL,         NULL,            0, get_dt_day,     set_dt_day,     NULL, NULL },    { NULL, STR_AM_PM_EN,      STR_AM_PM_KO,STR_S_12_HOUR_PERIOD_EN,       STR_S_12_HOUR_PERIOD_KO,    "clock", CFG_TYPE_CHOICE, 0,    1,    1, NULL, ampm_choices, ampm_choices_ko, 2, get_dt_ampm,    set_dt_ampm,    NULL, NULL },    { NULL, STR_HOUR_EN,         STR_HOUR_KO,        STR_HOUR_1_12_EN,         STR_HOUR_1_12_KO,         "clock", CFG_TYPE_INT,    1,    12,   1, NULL, NULL,         NULL,            0, get_dt_hour12,  set_dt_hour12,  NULL, NULL },    { NULL, STR_MINUTE_EN,       STR_MINUTE_KO,        STR_MINUTE_0_59_EN,       STR_MINUTE_0_59_KO,         "clock", CFG_TYPE_INT,    0,    59,   1, NULL, NULL,         NULL,            0, get_dt_minute,  set_dt_minute,  NULL, NULL },    { NULL, STR_APPLY_TO_RTC_EN, STR_APPLY_TO_RTC_KO,STR_SAVE_TO_HARDWARE_CLOCK_EN,STR_SAVE_TO_HARDWARE_CLOCK_KO,"check", CFG_TYPE_ACTION, 0,    0,    0, NULL, NULL,         NULL,            0, NULL,           NULL,           NULL, apply_datetime_to_rtc },};

static struct cfg_item bt_items[] = {
    { NULL,       STR_BLUETOOTH_RADIO_EN, STR_BLUETOOTH_RADIO_KO,  STR_POWER_MT6572_CONNSYS_RADIO_EN, STR_POWER_MT6572_CONNSYS_RADIO_KO,   "bluetooth",  CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,             NULL,             0, get_bt_enabled,  set_bt_enabled,  NULL, NULL },    { NULL,       STR_STATUS_EN,          STR_STATUS_KO,      STR_CURRENT_CONNECTION_STATE_EN,   STR_CURRENT_CONNECTION_STATE_KO,      "info",       CFG_TYPE_INFO,   0, 0, 0, NULL, NULL,             NULL,             0, NULL,            NULL,            info_bt_status, NULL },    { NULL,       STR_RECEIVER_MODE_EN,   STR_RECEIVER_MODE_KO,    STR_ACT_AS_BT_AUDIO_SINK_EN,       STR_ACT_AS_BT_AUDIO_SINK_KO,"radio",      CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,             NULL,             0, get_bt_receiver, set_bt_receiver, NULL, NULL },    { "bt_codec", STR_A2DP_CODEC_EN,      STR_A2DP_CODEC_KO,      STR_PREFERRED_AUDIO_CODEC_EN,      STR_PREFERRED_AUDIO_CODEC_KO,        "sliders",    CFG_TYPE_CHOICE, 0, 2, 1, NULL, bt_codec_choices, bt_codec_choices, 3, get_bt_codec,    set_bt_codec,    NULL, NULL },    { NULL,       STR_SCAN_DEVICES_EN,    STR_SCAN_DEVICES_KO,      STR_DISCOVER_NEARBY_AUDIO_SINKS_EN,STR_DISCOVER_NEARBY_AUDIO_SINKS_KO, "refresh-cw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,             NULL,             0, NULL,            NULL,            NULL, act_bt_scan },    { NULL,       STR_DISCONNECT_EN,      STR_DISCONNECT_KO,      STR_CLOSE_ACTIVE_BT_LINK_EN,       STR_CLOSE_ACTIVE_BT_LINK_KO,     "power",      CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,             NULL,             0, NULL,            NULL,            NULL, act_bt_disconnect },};

static int get_wifi_enable(void) { return y1_wifi_is_enabled() ? 1 : 0; }
static void set_wifi_enable(int v)
{
    y1_wifi_init();
    y1_wifi_set_enabled(v != 0);
    show_toast(v ? TR_ID(STR_WI_FI_ENABLED) : TR_ID(STR_WI_FI_DISABLED), 1200);
}
static void info_wifi_status(char *buf, size_t len)
{
    snprintf(buf, len, "%s", y1_wifi_get_status_str());
}
static void info_wifi_ip(char *buf, size_t len)
{
    char ip[32] = "";
    y1_wifi_get_ip(ip, sizeof(ip));
    snprintf(buf, len, "%s", ip[0] ? ip : TR_ID(STR_NONE));
}
static void act_wifi_scan(void)
{
    y1_wifi_init();
    if (!y1_wifi_is_enabled())
        y1_wifi_set_enabled(true);
    y1_wifi_start_scan();
    show_toast(TR_ID(STR_SCANNING_WI_FI), 1500);
}
static void act_wifi_disconnect(void)
{
    y1_wifi_disconnect();
    show_toast(TR_ID(STR_WI_FI_DISCONNECTED), 1200);
}

static struct cfg_item wifi_items[] = {
    { NULL, STR_WI_FI_POWER_EN,  STR_WI_FI_POWER_KO,  STR_ENABLE_MT6572_WI_FI_RADIO_EN, STR_ENABLE_MT6572_WI_FI_RADIO_KO,   "wifi",       CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_wifi_enable, set_wifi_enable, NULL, NULL },    { NULL, STR_STATUS_EN,       STR_STATUS_KO,   STR_CURRENT_WI_FI_STATE_EN,       STR_CURRENT_WI_FI_STATE_KO,          "info",       CFG_TYPE_INFO,   0, 0, 0, NULL, NULL, NULL, 0, NULL,            NULL,            info_wifi_status, NULL },    { NULL, STR_IP_ADDRESS_EN,   STR_IP_ADDRESS_KO,     STR_ASSIGNED_DHCP_IPV4_EN,        STR_ASSIGNED_DHCP_IPV4_KO,         "globe",      CFG_TYPE_INFO,   0, 0, 0, NULL, NULL, NULL, 0, NULL,            NULL,            info_wifi_ip, NULL },    { NULL, STR_SCAN_NETWORKS_EN,STR_SCAN_NETWORKS_KO,STR_FIND_NEARBY_ACCESS_POINTS_EN,STR_FIND_NEARBY_ACCESS_POINTS_KO,   "refresh-cw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL,            NULL,            NULL, act_wifi_scan },    { NULL, STR_DISCONNECT_EN,   STR_DISCONNECT_KO,   STR_LEAVE_CURRENT_NETWORK_EN,     STR_LEAVE_CURRENT_NETWORK_KO,  "power",      CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL,            NULL,            NULL, act_wifi_disconnect },};

static struct cfg_item usb_items[] = {
    { "usb_ipod_mode", STR_IPOD_USB_MODE_EN,    STR_IPOD_USB_MODE_KO,  STR_SPOOF_IPOD_5_5G_OVER_USB_EN,  STR_SPOOF_IPOD_5_5G_OVER_USB_KO,   "usb",        CFG_TYPE_TRISTATE, 0, 2, 1, NULL, NULL, NULL, 0, get_usb_ipod, set_usb_ipod, NULL, NULL },    { "usb_perf_tune", STR_USB_PERFORMANCE_EN,  STR_USB_PERFORMANCE_KO,  STR_BOOST_USB_GADGET_THROUGHPUT_EN,STR_BOOST_USB_GADGET_THROUGHPUT_KO,    "sliders",    CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_usb_perf, set_usb_perf, NULL, NULL },    { NULL,            STR_USB_MASS_STORAGE_EN, STR_USB_MASS_STORAGE_KO,STR_MOUNT_SD_CARD_TO_PC_EN,       STR_MOUNT_SD_CARD_TO_PC_KO,     "hard-drive", CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_usb_ums,  set_usb_ums,  NULL, NULL },    { NULL,            STR_SYNC_STORAGE_EN,     STR_SYNC_STORAGE_KO,  STR_FLUSH_SD_CARD_WRITE_CACHE_EN, STR_FLUSH_SD_CARD_WRITE_CACHE_KO,     "refresh-cw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL,         NULL,         NULL, act_sync_disks },    { NULL,            STR_DROP_CACHES_EN,      STR_DROP_CACHES_KO,    STR_FREE_LINUX_PAGE_CACHE_EN,     STR_FREE_LINUX_PAGE_CACHE_KO, "trash-2",    CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL,         NULL,         NULL, act_drop_caches },};

static struct cfg_item power_items[] = {
    { "lang",       STR_LANGUAGE_EN,      STR_LANGUAGE_KO,  STR_SWITCH_ENGLISH_EN,        STR_SWITCH_ENGLISH_KO,           "globe",      CFG_TYPE_CHOICE, 0, 1, 1, NULL, lang_choices, lang_choices, 2, get_lang,       set_lang,       NULL, NULL },    { "screenshot", STR_SCREENSHOT_EN,    STR_SCREENSHOT_KO,         STR_MENU_PLAY_SAVES_TO_SCREENSHOTS_EN, STR_MENU_PLAY_SAVES_TO_SCREENSHOTS_KO, "camera",     CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,         NULL,         0, get_screenshot, set_screenshot, NULL, NULL },    { "debug_log",  STR_Y1_DEBUG_LOG_EN,  STR_Y1_DEBUG_LOG_KO,   STR_WRITE_SYSTEM_Y1_DEBUG_TXT_EN,     STR_WRITE_SYSTEM_Y1_DEBUG_TXT_KO,    "info",       CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,         NULL,         0, get_debug_log,  set_debug_log,  NULL, NULL },    { NULL,        STR_CLEAR_LOG_EN,     STR_CLEAR_LOG_KO,        STR_DELETE_Y1_DEBUG_TXT_EN,        STR_DELETE_Y1_DEBUG_TXT_KO,  "trash-2",    CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_clear_log },    { NULL,        STR_RESET_SETTINGS_EN,STR_RESET_SETTINGS_KO,      STR_RESTORE_DEFAULT_SETTINGS_EN,   STR_RESTORE_DEFAULT_SETTINGS_KO,  "rotate-ccw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_reset_settings },    { NULL,        STR_EXIT_CONFIGAPP_EN,STR_EXIT_CONFIGAPP_KO,     STR_RETURN_TO_CALLER_EN,           STR_RETURN_TO_CALLER_KO, "arrow-left", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_return_bootmenu },    { NULL,        STR_REBOOT_DEVICE_EN, STR_REBOOT_DEVICE_KO,      STR_RESTART_THE_Y1_EN,             STR_RESTART_THE_Y1_KO,           "rotate-ccw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_reboot },    { NULL,        STR_POWER_OFF_EN,     STR_POWER_OFF_KO,        STR_SHUT_DOWN_SAFELY_EN,           STR_SHUT_DOWN_SAFELY_KO,     "power",      CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_poweroff },};

static struct cfg_item launch_items[] = {
    { NULL, STR_DEFAULT_LAUNCH_APP_EN,     STR_DEFAULT_LAUNCH_APP_KO,          STR_APP_STARTED_ON_BOOT_EN,           STR_APP_STARTED_ON_BOOT_KO,       "play",       CFG_TYPE_CHOICE, 0, 2, 1, NULL, default_player_choices, default_player_choices, 3, get_default_player, set_default_player, NULL, NULL },
    { NULL, STR_RESCAN_SOFTWARE_EN,        STR_RESCAN_SOFTWARE_KO,         STR_SCAN_HIDDEN_FOLDERS_ON_SD_EN,     STR_SCAN_HIDDEN_FOLDERS_ON_SD_KO,  "refresh-cw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,                   NULL,                   0, NULL,               NULL,               NULL, scan_launch_apps },
};

static struct cfg_item serial_items[] = {
    { NULL, STR_SERIAL_CONSOLE_EN, STR_SERIAL_CONSOLE_KO,   STR_ENABLE_DUAL_USB_ACM_SHELL_EN, STR_ENABLE_DUAL_USB_ACM_SHELL_KO, "terminal", CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_serial_active, set_serial_active, NULL, NULL },
    { NULL, STR_USB_ACM_PORTS_EN,  STR_USB_ACM_PORTS_KO,  STR_TTYGS0_TTYGS1_STATUS_EN,    STR_TTYGS0_TTYGS1_STATUS_KO,   "info",     CFG_TYPE_INFO,   0, 0, 0, NULL, NULL, NULL, 0, NULL,              NULL,              info_serial_ports, NULL },
    { NULL, STR_ADB_DAEMON_EN,     STR_ADB_DAEMON_KO,     STR_ADB_DESC_EN,                STR_ADB_DESC_KO,               "terminal", CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_adb_active,    set_adb_active,    NULL, NULL },
};

static struct cfg_item debug_items[] = {
    { NULL, STR_SAVE_DEBUG_INFO_EN, STR_SAVE_DEBUG_INFO_KO, STR_WRITE_DEBUGINFO_TXT_TO_SD_EN, STR_WRITE_DEBUGINFO_TXT_TO_SD_KO, "hard-drive", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, NULL, act_save_debug_info },};

static struct cfg_item exit_power_items[] = {
    { NULL, STR_EXIT_CONFIGAPP_EN, STR_EXIT_CONFIGAPP_KO, STR_RETURN_TO_CALLER_EN, STR_RETURN_TO_CALLER_KO, "arrow-left", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, NULL, act_return_bootmenu },    { NULL, STR_REBOOT_DEVICE_EN,  STR_REBOOT_DEVICE_KO,  STR_RESTART_THE_Y1_EN,   STR_RESTART_THE_Y1_KO,           "rotate-ccw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, NULL, act_reboot },    { NULL, STR_SHUT_DOWN_EN,      STR_SHUT_DOWN_KO,    STR_POWER_OFF_SAFELY_EN, STR_POWER_OFF_SAFELY_KO,     "power",      CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, NULL, act_poweroff },};

static struct cfg_page g_pages[] = {
    { "launch",   STR_LAUNCH_EN,            STR_LAUNCH_KO,             STR_START_INSTALLED_SOFTWARE_EN,   STR_START_INSTALLED_SOFTWARE_KO,     "play",      0xFF10B981U, launch_items,   (int)(sizeof(launch_items) / sizeof(launch_items[0])),     false, false, true,  false },    { "status",   STR_SYSTEM_STATUS_EN,     STR_SYSTEM_STATUS_KO,      STR_BATTERY_CPU_RAM_DISK_EN,   STR_BATTERY_CPU_RAM_DISK_KO, "activity",  0xFF16A34AU, status_items,   (int)(sizeof(status_items) / sizeof(status_items[0])),     false, false, false, false },    { "display",  STR_DISPLAY_HAPTICS_EN, STR_DISPLAY_HAPTICS_KO,     STR_BRIGHTNESS_WHEEL_MOTOR_EN,  STR_BRIGHTNESS_WHEEL_MOTOR_KO,      "sun",       0xFFD97706U, display_items,  (int)(sizeof(display_items) / sizeof(display_items[0])),   false, false, false, false },    { "cpu",      STR_CPU_POWER_EN,       STR_CPU_POWER_KO,      STR_GOVERNOR_CORE_HOTPLUG_EN,    STR_GOVERNOR_CORE_HOTPLUG_KO,        "cpu",       0xFF2563EBU, cpu_items,      (int)(sizeof(cpu_items) / sizeof(cpu_items[0])),           false, false, false, false },    { "datetime", STR_DATE_TIME_EN,       STR_DATE_TIME_KO,     STR_S_12_HOUR_CLOCK_RTC_EN,        STR_S_12_HOUR_CLOCK_RTC_KO,       "clock",     0xFF0EA5E9U, datetime_items, (int)(sizeof(datetime_items) / sizeof(datetime_items[0])), false, false, false, false },    { "bt",       STR_BLUETOOTH_EN,         STR_BLUETOOTH_KO,         STR_SCAN_PAIR_A2DP_CODEC_EN,    STR_SCAN_PAIR_A2DP_CODEC_KO,    "bluetooth", 0xFF0284C7U, bt_items,       (int)(sizeof(bt_items) / sizeof(bt_items[0])),             true,  false, false, false },    { "wifi",     STR_WI_FI_EN,             STR_WI_FI_KO,            STR_SCAN_CONNECT_WIRELESS_EN,    STR_SCAN_CONNECT_WIRELESS_KO, "wifi",      0xFF0284C7U, wifi_items,     (int)(sizeof(wifi_items) / sizeof(wifi_items[0])),           false, true,  false, false },    { "usb",      STR_USB_STORAGE_EN,     STR_USB_STORAGE_KO,    STR_IPOD_MODE_DISK_SYNC_EN,      STR_IPOD_MODE_DISK_SYNC_KO,        "usb",       0xFF7C3AEDU, usb_items,      (int)(sizeof(usb_items) / sizeof(usb_items[0])),           false, false, false, false },    { "serial",   STR_SERIAL_TERMINAL_EN,   STR_SERIAL_TERMINAL_KO,    STR_USB_ACM_ROOT_CONSOLE_EN,       STR_USB_ACM_ROOT_CONSOLE_KO,            "terminal",  0xFF0F766EU, serial_items,   (int)(sizeof(serial_items) / sizeof(serial_items[0])),     false, false, false, false },    { "debug",    STR_DEBUG_INFO_EN,        STR_DEBUG_INFO_KO,      STR_HARDWARE_SYSTEM_DUMP_EN,     STR_HARDWARE_SYSTEM_DUMP_KO,    "bug",       0xFF6366F1U, debug_items,    (int)(sizeof(debug_items) / sizeof(debug_items[0])),      false, false, false, true  },    { "system",   STR_SYSTEM_POWER_EN,    STR_SYSTEM_POWER_KO,   STR_LANGUAGE_DEBUG_LOGS_EN,      STR_LANGUAGE_DEBUG_LOGS_KO,        "settings",  0xFF64748BU, power_items,      (int)(sizeof(power_items) / sizeof(power_items[0])),           false, false, false, false },    { "exit",     STR_EXIT_SHUT_DOWN_EN,  STR_EXIT_SHUT_DOWN_KO, STR_EXIT_APP_OR_POWER_OFF_EN,      STR_EXIT_APP_OR_POWER_OFF_KO,     "power",     0xFFDC2626U, exit_power_items, (int)(sizeof(exit_power_items) / sizeof(exit_power_items[0])), false, false, false, false },};

#define NUM_PAGES ((int)(sizeof(g_pages) / sizeof(g_pages[0])))

/* =========================================================================
 * UI Navigation & Rendering State
 * ========================================================================= */
static int   g_home_sel = 0;
static int   g_home_row_scroll = 0;
static float g_home_row_scroll_anim = 0.0f;
static int   g_active_page = -1; /* -1 = Home Grid, >= 0 = Page index */
static int   g_item_sel = 0;
static int   g_item_scroll = 0;
static float g_item_scroll_anim = 0.0f;
static float g_trans_x = 0.0f;
static float g_bg_blur_anim = 0.0f;
static bool  g_editing_slider = false;
static bool  g_editing_choice = false;
static int   g_choice_sel = 0;

/* App Switch Confirmation Modal (when launched from MeUI/Rockbox) */
static bool  g_switch_open = false;
static int   g_switch_sel = 1;
static int   g_switch_target_idx = -1;
static pid_t g_switch_pid = 0;
static char  g_switch_cur_name[32] = "";

/* ABC Fast Scroll state */
static uint64_t g_scroll_last_ms = 0;
static int      g_scroll_fast_streak = 0;
static int      g_scroll_last_dir = 0;
static bool     g_abc_active = false;
static uint64_t g_abc_until_ms = 0;
static char     g_abc_label[8] = "";

static uint64_t cfg_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static int cfg_abc_bucket(const char *s, char *out_lbl, size_t lbl_sz)
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

static void draw_abc_badge(void)
{
    if (!g_abc_active || cfg_now_ms() >= g_abc_until_ms || !g_abc_label[0]) {
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

static struct y1_bt_device g_bt_devs[Y1_BT_MAX_DEVICES];
static int g_bt_dev_count = 0;

static void refresh_bt_list(void)
{
    g_bt_dev_count = y1_bt_get_devices(g_bt_devs, Y1_BT_MAX_DEVICES);
}

static struct y1_wifi_network g_wifi_nets[Y1_WIFI_MAX_NETWORKS];
static int g_wifi_net_count = 0;

static void refresh_wifi_list(void)
{
    g_wifi_net_count = y1_wifi_get_networks(g_wifi_nets, Y1_WIFI_MAX_NETWORKS);
}

static const char *cfg_row_label(const struct cfg_page *pg, int idx)
{
    if (idx < pg->num_items) return TR(pg->items[idx].label, pg->items[idx].label_ko);
    int extra = idx - pg->num_items;
    if (pg->is_launch_page && extra < g_launch_count) return g_launch_apps[extra].name;
    if (pg->is_wifi_page && extra < g_wifi_net_count) return g_wifi_nets[extra].ssid;
    if (pg->is_bt_page && extra < g_bt_dev_count) return g_bt_devs[extra].name;
    return "";
}

static void format_12h_clock(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) {
        snprintf(buf, len, "--:--");
        return;
    }
    int h12 = tm->tm_hour % 12;
    if (h12 == 0) h12 = 12;
    bool is_pm = (tm->tm_hour >= 12);
    if (g_cfg.lang == 1)
        snprintf(buf, len, "%s %d:%02d", ampm_choices_ko[is_pm ? 1 : 0], h12, tm->tm_min);
    else
        snprintf(buf, len, "%d:%02d %s", h12, tm->tm_min, is_pm ? "PM" : "AM");
}

static void draw_header(const char *title, const char *icon_name)
{
    gfx_set_text_shadow(false);
    gfx_fill_round_rect(8, 6, UI_W - 16, 48, 24, COL_SURFACE);

    char cpu_str[24];
    snprintf(cpu_str, sizeof(cpu_str), "%dM", g_cached_cpu_mhz);
    int cpu_w = gfx_text_width(cpu_str, 18.0f, UI_FONT_SEMIBOLD);

    int bat_pct = g_cached_bat_pct;
    bool bat_chg = g_cached_bat_chg;

    char time_str[24];
    format_12h_clock(time_str, sizeof(time_str));
    int time_w = gfx_text_width(time_str, 22.0f, UI_FONT_SEMIBOLD);

    int rx = UI_W - 22 - time_w;
    gfx_draw_text(rx, 17, time_str, 22.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    rx -= 58 + 10;
    gfx_draw_battery_icon(rx, 14, bat_pct, bat_chg, 0);

    int cpu_pill_w = 26 + cpu_w + 10;
    rx -= cpu_pill_w + 6;
    gfx_fill_round_rect(rx, 13, cpu_pill_w, 34, 17, COL_SURFACE_HI);
    gfx_draw_icon("cpu", rx + 7, 20, 19, COL_TEXT);
    gfx_draw_text(rx + 28, 19, cpu_str, 18.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    int max_pill_w = rx - 18;
    int title_w = gfx_text_width(title ? title : "", 23.5f, UI_FONT_SEMIBOLD);
    int pill_w = 40 + title_w + 16;
    if (pill_w > max_pill_w) pill_w = max_pill_w;
    if (pill_w < 80) pill_w = 80;
    gfx_fill_round_rect(12, 10, pill_w, 40, 20, COL_ACCENT);
    gfx_draw_icon(icon_name ? icon_name : "settings", 21, 18, 24, COL_ACCENT_TEXT);
    gfx_draw_text_ellipsize(52, 17, pill_w - 52, title ? title : "", 23.5f, UI_FONT_SEMIBOLD, COL_ACCENT_TEXT);
}

static void draw_toast_overlay(void)
{
    if (!toast_active())
        return;
    gfx_set_text_shadow(false);
    int tw = gfx_text_width(g_toast_msg, 19.5f, UI_FONT_SEMIBOLD);
    int pw = tw + 40;
    if (pw > UI_W - 24) pw = UI_W - 24;
    int px = (UI_W - pw) / 2;
    int py = UI_H - 58;
    gfx_fill_round_rect(px, py, pw, 44, 22, COL_MODAL_BG);
    gfx_stroke_round_rect(px, py, pw, 44, 22, 2, COL_MODAL_BORDER);
    gfx_draw_text_ellipsize(px + 20, py + 11, pw - 40, g_toast_msg,
                            19.5f, UI_FONT_SEMIBOLD, COL_TEXT);
}

static void render_home(int ox)
{
    gfx_reset_clip();
    draw_header(TR_ID(STR_SETTINGS), "settings");

    int cols = 2;
    int pad_x = 12;
    int gap_x = 10;
    int gap_y = 12;
    int top_y = 62;
    int card_w = (UI_W - pad_x * 2 - 12 - gap_x) / cols; /* Room for scrollbar */
    int card_h = 138;
    int step_y = card_h + gap_y;
    int visible_rows = 2;

    int sel_row = g_home_sel / cols;
    int total_rows = (NUM_PAGES + cols - 1) / cols;
    if (sel_row < g_home_row_scroll)
        g_home_row_scroll = sel_row;
    if (sel_row >= g_home_row_scroll + visible_rows)
        g_home_row_scroll = sel_row - visible_rows + 1;
    if (g_home_row_scroll > total_rows - visible_rows && total_rows >= visible_rows)
        g_home_row_scroll = total_rows - visible_rows;
    if (g_home_row_scroll < 0)
        g_home_row_scroll = 0;

    gfx_set_clip_y(60, UI_H - 4);
    uint64_t tnow = cfg_now_ms();
    for (int i = 0; i < NUM_PAGES; i++) {
        int col = i % cols;
        int row = i / cols;
        int x = pad_x + ox + col * (card_w + gap_x);
        int y = top_y + (int)roundf(((float)row - g_home_row_scroll_anim) * (float)step_y);
        if (y + card_h < 60 || y > UI_H) continue;
        bool sel = (i == g_home_sel);
        float t = cfg_item_pill_get(-1, i, sel, tnow);
        /* Grid card: slightly round more on selection (24 -> 36), not a pill */
        int r = (int)roundf(24.0f + t * 12.0f);
        uint32_t bg = cfg_lerp_argb(COL_SURFACE, COL_SEL_BG, t);

        gfx_fill_round_rect(x, y, card_w, card_h, r, bg);
        if (sel)
            gfx_stroke_round_rect(x, y, card_w, card_h, r, 2, COL_BORDER_HI);

        uint32_t badge_bg = (sel && !gfx_has_wallpaper()) ? COL_ACCENT : COL_SURFACE_HI;
        uint32_t icon_col = (sel && !gfx_has_wallpaper()) ? COL_ACCENT_TEXT : COL_TEXT;
        gfx_fill_round_rect(x + 14, y + 14, 54, 54, 22, badge_bg);
        gfx_draw_icon(g_pages[i].icon, x + 26, y + 26, 30, icon_col);

        gfx_set_text_shadow(false);
        const char *ptitle = TR(g_pages[i].title, g_pages[i].title_ko);
        const char *psub = TR(g_pages[i].subtitle, g_pages[i].subtitle_ko);
        gfx_draw_text_ellipsize(x + 14, y + 73, card_w - 28,
                                ptitle, 26.0f, UI_FONT_SEMIBOLD, sel ? COL_SEL_TEXT : COL_TEXT);
        gfx_draw_text_ellipsize(x + 14, y + 105, card_w - 28,
                                psub, 21.5f, UI_FONT_REGULAR, COL_TEXT_DIM);
        gfx_set_text_shadow(false);

        if (sel) {
            gfx_draw_icon("chevron-right", x + card_w - 32, y + 26, 24, COL_SEL_TEXT);
        }
    }
    gfx_reset_clip();
    gfx_draw_scrollbar(UI_W - 12, top_y, visible_rows * step_y - gap_y,
                       g_home_row_scroll_anim, visible_rows, total_rows);
}

static void render_slider_modal(const struct cfg_item *it)
{
    gfx_set_text_shadow(false);
    int mw = 420, mh = 160;
    int mx = (UI_W - mw) / 2;
    int my = (UI_H - mh) / 2;

    gfx_fill_round_rect(mx, my, mw, mh, 36, COL_MODAL_BG);
    gfx_stroke_round_rect(mx, my, mw, mh, 36, 2, COL_BORDER_HI);

    gfx_draw_text(mx + 24, my + 20, TR(it->label, it->label_ko),
                  24.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    int val = it->get_int ? it->get_int() : it->min_val;
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "%d%s", val, it->unit ? it->unit : "");
    int vw = gfx_text_width(vbuf, 24.0f, UI_FONT_SEMIBOLD);
    gfx_draw_text(mx + mw - 24 - vw, my + 20, vbuf, 24.0f, UI_FONT_SEMIBOLD, COL_ACCENT_TEXT);

    int tx = mx + 24, ty = my + 90, tw = mw - 48, th = 14;
    gfx_fill_round_rect(tx, ty, tw, th, 7, COL_SURFACE_HI);
    int range = it->max_val - it->min_val;
    int fill_w = (range > 0) ? ((val - it->min_val) * tw) / range : 0;
    if (fill_w < 10 && val > it->min_val) fill_w = 10;
    if (fill_w > tw) fill_w = tw;
    if (fill_w > 0)
        gfx_fill_round_rect(tx, ty, fill_w, th, 7, COL_ACCENT);

    int kx = tx + fill_w - 10;
    if (kx < tx) kx = tx;
    if (kx > tx + tw - 20) kx = tx + tw - 20;
    gfx_fill_round_rect(kx, ty - 5, 24, 24, 10, COL_SUCCESS);
    gfx_stroke_round_rect(kx, ty - 5, 24, 24, 10, 1, COL_BORDER_HI);
}

static void render_choice_modal(const struct cfg_item *it)
{
    if (!it || it->num_choices <= 0) return;
    gfx_set_text_shadow(false);
    int n = it->num_choices;
    int vis = (n > 4) ? 4 : n;
    int row_h = 46, gap = 6;
    int mw = 380;
    int mh = 72 + vis * (row_h + gap) + 12;
    int mx = (UI_W - mw) / 2;
    int my = (UI_H - mh) / 2;

    gfx_fill_round_rect(mx, my, mw, mh, 30, COL_MODAL_BG);
    gfx_stroke_round_rect(mx, my, mw, mh, 30, 2, COL_BORDER_HI);
    gfx_draw_text_ellipsize(mx + 22, my + 18, mw - 44, TR(it->label, it->label_ko),
                            23.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    int cur = it->get_int ? it->get_int() : 0;
    int scroll = 0;
    if (g_choice_sel >= vis) scroll = g_choice_sel - vis + 1;
    if (scroll > n - vis) scroll = n - vis;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < vis; i++) {
        int ci = scroll + i;
        int ry = my + 58 + i * (row_h + gap);
        bool sel = (ci == g_choice_sel);
        gfx_fill_round_rect(mx + 16, ry, mw - 32, row_h, row_h / 2,
                            sel ? COL_SEL_BG : COL_MODAL_HI);
        if (sel) gfx_stroke_round_rect(mx + 16, ry, mw - 32, row_h, row_h / 2, 2, COL_BORDER_HI);
        const char *lbl = (g_cfg.lang == 1 && it->choices_ko) ? it->choices_ko[ci] : it->choices[ci];
        gfx_draw_text_ellipsize(mx + 34, ry + 11, mw - 92, lbl,
                                21.5f, UI_FONT_SEMIBOLD, sel ? COL_SEL_TEXT : COL_TEXT);
        if (ci == cur) {
            gfx_draw_icon("check", mx + mw - 50, ry + 13, 20,
                          sel ? COL_SEL_TEXT : COL_ACCENT);
        }
    }
}

static void render_switch_modal(void)
{
    if (!g_switch_open || g_switch_target_idx < 0 || g_switch_target_idx >= g_launch_count)
        return;
    gfx_set_text_shadow(false);
    int mw = 410, mh = 196;
    int mx = (UI_W - mw) / 2;
    int my = (UI_H - mh) / 2;

    gfx_fill_round_rect(mx, my, mw, mh, 30, COL_MODAL_BG);
    gfx_stroke_round_rect(mx, my, mw, mh, 30, 2, COL_BORDER_HI);
    gfx_draw_text_ellipsize(mx + 24, my + 22, mw - 48, TR_ID(STR_CLOSE_APP_TITLE),
                            24.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    char body[160];
    snprintf(body, sizeof(body), TR_ID(STR_CLOSE_APP_BODY),
             g_switch_cur_name[0] ? g_switch_cur_name : "App",
             g_launch_apps[g_switch_target_idx].name);
    gfx_draw_text_ellipsize(mx + 24, my + 66, mw - 48, body,
                            21.0f, UI_FONT_REGULAR, COL_TEXT_DIM);

    int bw = (mw - 60) / 2, bh = 50;
    int by = my + mh - bh - 20;
    int bx0 = mx + 22, bx1 = bx0 + bw + 16;

    gfx_fill_round_rect(bx0, by, bw, bh, bh / 2, g_switch_sel == 0 ? COL_SEL_BG : COL_MODAL_HI);
    if (g_switch_sel == 0) gfx_stroke_round_rect(bx0, by, bw, bh, bh / 2, 2, COL_BORDER_HI);
    const char *no_lbl = TR_ID(STR_NO);
    int nw = gfx_text_width(no_lbl, 22.0f, UI_FONT_SEMIBOLD);
    gfx_draw_text(bx0 + (bw - nw) / 2, by + 13, no_lbl, 22.0f, UI_FONT_SEMIBOLD,
                  g_switch_sel == 0 ? COL_SEL_TEXT : COL_TEXT);

    gfx_fill_round_rect(bx1, by, bw, bh, bh / 2, g_switch_sel == 1 ? COL_ACCENT : COL_MODAL_HI);
    if (g_switch_sel == 1) gfx_stroke_round_rect(bx1, by, bw, bh, bh / 2, 2, COL_BORDER_HI);
    const char *yes_lbl = TR_ID(STR_YES);
    int yw = gfx_text_width(yes_lbl, 22.0f, UI_FONT_SEMIBOLD);
    gfx_draw_text(bx1 + (bw - yw) / 2, by + 13, yes_lbl, 22.0f, UI_FONT_SEMIBOLD,
                  g_switch_sel == 1 ? COL_ACCENT_TEXT : COL_TEXT);
}

static pid_t find_running_app_pid(char *out_name, size_t out_sz)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    pid_t self = getpid();
    pid_t found = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid <= 1 || pid == self) continue;
        char cpath[64], comm[64] = "";
        snprintf(cpath, sizeof(cpath), "/proc/%d/comm", (int)pid);
        FILE *f = fopen(cpath, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) {
            char *nl = strchr(comm, '\n');
            if (nl) *nl = '\0';
            if (strcmp(comm, "meui") == 0 || strcmp(comm, "rockbox") == 0) {
                found = pid;
                if (out_name && out_sz > 0) {
                    snprintf(out_name, out_sz, "%s",
                             strcmp(comm, "meui") == 0 ? "MeUI" : "Rockbox");
                }
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

static void execute_app_switch(const char *exec_path, pid_t kill_pid)
{
    if (strcmp(exec_path, "__ANDROID__") == 0) {
        if (kill_pid > 0) {
            kill(kill_pid, SIGTERM);
            usleep(200000);
        }
        save_settings_conf();
        y1_sys_reboot();
        return;
    }

    mkdir("/tmp", 0777);
    FILE *lf = fopen("/tmp/configapp_launch", "w");
    if (lf) {
        fprintf(lf, "%s\n", exec_path);
        fclose(lf);
    }
    FILE *sf = fopen("/tmp/configapp_switching", "w");
    if (sf) {
        fprintf(sf, "1\n");
        fclose(sf);
    }
    sync();

    save_settings_conf();
    y1_input_close();
    gfx_shutdown();

    if (kill_pid > 0) {
        kill(kill_pid, SIGTERM);
        char ppath[64];
        snprintf(ppath, sizeof(ppath), "/proc/%d", (int)kill_pid);
        for (int i = 0; i < 30; i++) {
            if (access(ppath, F_OK) != 0) break;
            usleep(50000);
        }
        if (access(ppath, F_OK) == 0)
            kill(kill_pid, SIGKILL);
    }
    _exit(0);
}

static void render_page(int ox)
{
    gfx_reset_clip();
    const struct cfg_page *pg = &g_pages[g_active_page];
    draw_header(TR(pg->title, pg->title_ko), pg->icon);

    if (pg->is_bt_page)
        refresh_bt_list();
    if (pg->is_wifi_page)
        refresh_wifi_list();
    if (pg->is_debug_page && g_dbg_count == 0)
        refresh_debug_info();

    int extra_rows = 0;
    if (pg->is_bt_page) extra_rows = g_bt_dev_count;
    else if (pg->is_wifi_page) extra_rows = g_wifi_net_count;
    else if (pg->is_launch_page) extra_rows = g_launch_count;
    else if (pg->is_debug_page) extra_rows = g_dbg_count;
    int total_rows = pg->num_items + extra_rows;
    int list_top = 60;
    int row_h = 68;
    int gap = 6;
    int step = row_h + gap;
    int visible_rows = 4;

    if (g_item_sel < g_item_scroll)
        g_item_scroll = g_item_sel;
    if (g_item_sel >= g_item_scroll + visible_rows)
        g_item_scroll = g_item_sel - visible_rows + 1;
    if (g_item_scroll < 0)
        g_item_scroll = 0;

    gfx_set_clip_y(58, UI_H - 4);
    int first_idx = (int)floorf(g_item_scroll_anim) - 1;
    if (first_idx < 0) first_idx = 0;
    int last_idx = first_idx + visible_rows + 3;
    if (last_idx > total_rows) last_idx = total_rows;

    int x = 10 + ox;
    int w = UI_W - 30; /* Room for scrollbar */
    uint64_t tnow = cfg_now_ms();
    for (int idx = first_idx; idx < last_idx; idx++) {
        int y = list_top + (int)roundf(((float)idx - g_item_scroll_anim) * (float)step);
        if (y + row_h < 58 || y > UI_H) continue;
        bool sel = (idx == g_item_sel);
        float t = cfg_item_pill_get(g_active_page, idx, sel, tnow);
        int r = (int)roundf(18.0f + t * (float)(row_h / 2 - 18));
        uint32_t bg = cfg_lerp_argb(COL_SURFACE, COL_SEL_BG, t);
        gfx_fill_round_rect(x, y, w, row_h, r, bg);
        if (sel)
            gfx_stroke_round_rect(x, y, w, row_h, r, 2, COL_BORDER_HI);

        if (idx < pg->num_items) {
            const struct cfg_item *it = &pg->items[idx];
            gfx_draw_icon(it->icon ? it->icon : "sliders", x + 14, y + 20, 28, COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 7, 232, TR(it->label, it->label_ko), 24.0f,
                                    UI_FONT_SEMIBOLD, COL_TEXT);
            if (it->subtitle) {
                gfx_draw_text_ellipsize(x + 52, y + 38, 232, TR(it->subtitle, it->subtitle_ko),
                                        18.5f, UI_FONT_REGULAR, COL_TEXT_MUTED);
            }

            if (it->type == CFG_TYPE_TOGGLE && it->get_int) {
                int on = it->get_int();
                float t = cfg_sw_get(g_active_page, idx, on ? 1.0f : 0.0f, cfg_now_ms());
                int pw = 58, ph = 34;
                int px = x + w - 16 - pw;
                int py = y + (row_h - ph) / 2;
                gfx_fill_round_rect(px, py, pw, ph, 17, sel ? COL_SURFACE : COL_SURFACE_HI);
                int tx = px + (int)roundf(t * (float)(pw - 34));
                uint32_t thumb_col = cfg_lerp_argb(COL_WARN, COL_SUCCESS, t);
                gfx_fill_round_rect(tx, py, 34, 34, 14, thumb_col);
                gfx_draw_icon(t >= 0.5f ? "check" : "x", tx + 7, py + 7, 20, COL_SWITCH_ICON);
            } else if (it->type == CFG_TYPE_TRISTATE && it->get_int) {
                int st = it->get_int();
                if (st < 0) st = 0;
                if (st > 2) st = 2;
                float want = (st == 0) ? 0.0f : ((st == 1) ? 0.5f : 1.0f);
                float t = cfg_sw_get(g_active_page, idx, want, cfg_now_ms());
                int pw = 82, ph = 34;
                int px = x + w - 16 - pw;
                int py = y + (row_h - ph) / 2;
                gfx_fill_round_rect(px, py, pw, ph, 17, sel ? COL_SURFACE : COL_SURFACE_HI);
                int tx = px + (int)roundf(t * (float)(pw - 34));
                uint32_t thumb_col = (t < 0.5f)
                    ? cfg_lerp_argb(COL_WARN, COL_ACCENT, t * 2.0f)
                    : cfg_lerp_argb(COL_ACCENT, COL_SUCCESS, (t - 0.5f) * 2.0f);
                gfx_fill_round_rect(tx, py, 34, 34, 14, thumb_col);
                if (t < 0.25f) {
                    gfx_draw_icon("x", tx + 7, py + 7, 20, COL_SWITCH_ICON);
                } else if (t > 0.75f) {
                    gfx_draw_icon("check", tx + 7, py + 7, 20, COL_SWITCH_ICON);
                } else {
                    int sw = gfx_text_width("/", 20.0f, UI_FONT_SEMIBOLD);
                    gfx_draw_text(tx + (34 - sw) / 2, py + 5, "/", 20.0f, UI_FONT_SEMIBOLD, COL_SWITCH_ICON);
                }
            } else if (it->type == CFG_TYPE_CHOICE && it->get_int && it->choices) {
                int c = it->get_int();
                if (c < 0 || c >= it->num_choices) c = 0;
                const char *val_str = (g_cfg.lang == 1 && it->choices_ko) ? it->choices_ko[c] : it->choices[c];
                int vw = gfx_text_width(val_str, 20.0f, UI_FONT_SEMIBOLD);
                int bw = vw + 26;
                int bx = x + w - 16 - bw;
                gfx_fill_round_rect(bx, y + 15, bw, 38, 19, COL_ACCENT);
                gfx_draw_text(bx + 13, y + 21, val_str, 20.0f, UI_FONT_SEMIBOLD, COL_ACCENT_TEXT);
            } else if (it->type == CFG_TYPE_INT && it->get_int) {
                char vbuf[32];
                snprintf(vbuf, sizeof(vbuf), "%d%s", it->get_int(), it->unit ? it->unit : "");
                int vw = gfx_text_width(vbuf, 20.0f, UI_FONT_SEMIBOLD);
                int bw = vw + 26;
                int bx = x + w - 16 - bw;
                gfx_fill_round_rect(bx, y + 15, bw, 38, 16, sel ? COL_SURFACE : COL_SURFACE_HI);
                gfx_draw_text(bx + 13, y + 21, vbuf, 20.0f, UI_FONT_SEMIBOLD, COL_TEXT);
            } else if (it->type == CFG_TYPE_INFO && it->get_str) {
                char ibuf[96];
                it->get_str(ibuf, sizeof(ibuf));
                int max_iw = 190;
                int iw = gfx_text_width(ibuf, 19.5f, UI_FONT_SEMIBOLD);
                if (iw > max_iw) iw = max_iw;
                gfx_draw_text_ellipsize(x + w - 16 - iw, y + 22, max_iw, ibuf,
                                        19.5f, UI_FONT_SEMIBOLD, COL_TEXT_DIM);
            } else if (it->type == CFG_TYPE_ACTION) {
                gfx_draw_icon("chevron-right", x + w - 36, y + 22, 24, COL_TEXT);
            }
        } else if (pg->is_launch_page) {
            int l_idx = idx - pg->num_items;
            const struct launch_entry *le = &g_launch_apps[l_idx];
            gfx_draw_icon("play", x + 14, y + 20, 28, COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 7, 238, le->name, 24.0f,
                                    UI_FONT_SEMIBOLD, COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 38, 238, le->exec_path, 18.5f,
                                    UI_FONT_REGULAR, COL_TEXT_MUTED);
            const char *badge = TR_ID(STR_LAUNCH);
            int bw = gfx_text_width(badge, 19.0f, UI_FONT_SEMIBOLD) + 26;
            gfx_fill_round_rect(x + w - 14 - bw, y + 15, bw, 38, 19, COL_ACCENT);
            gfx_draw_text(x + w - 14 - bw + 13, y + 21, badge, 19.0f, UI_FONT_SEMIBOLD, COL_ACCENT_TEXT);
        } else if (pg->is_debug_page) {
            int d_idx = idx - pg->num_items;
            const char *line = g_dbg_lines[d_idx];
            if (line[0] == '[') {
                gfx_draw_icon("info", x + 14, y + 21, 22, COL_TEXT);
                gfx_draw_text_ellipsize(x + 46, y + 22, w - 60, line, 19.0f,
                                        UI_FONT_SEMIBOLD, COL_TEXT);
            } else {
                gfx_draw_text_ellipsize(x + 16, y + 24, w - 32, line, 16.0f,
                                        UI_FONT_REGULAR, COL_TEXT);
            }
        } else if (pg->is_wifi_page) {
            int w_idx = idx - pg->num_items;
            const struct y1_wifi_network *net = &g_wifi_nets[w_idx];
            gfx_draw_icon(net->secured ? "lock" : "wifi", x + 14, y + 20, 28, COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 7, 220, net->ssid, 24.0f,
                                    UI_FONT_SEMIBOLD, COL_TEXT);
            char sub[64];
            snprintf(sub, sizeof(sub), "%d dBm  •  %s  •  %s",
                     net->rssi,
                     net->secured ? "WPA" : TR_ID(STR_OPEN),
                     net->connected ? TR_ID(STR_CONNECTED)
                                    : (net->saved ? TR_ID(STR_SAVED_RIGHT_FORGET)
                                                  : TR_ID(STR_AVAILABLE)));
            gfx_draw_text_ellipsize(x + 52, y + 38, 230, sub, 18.0f,
                                    UI_FONT_REGULAR, COL_TEXT_MUTED);

            const char *badge = net->connected ? TR_ID(STR_DISCONNECT) : TR_ID(STR_CONNECT);
            int bw = gfx_text_width(badge, 18.5f, UI_FONT_SEMIBOLD) + 24;
            gfx_fill_round_rect(x + w - 14 - bw, y + 15, bw, 38, 19,
                                net->connected ? COL_WARN : COL_ACCENT);
            gfx_draw_text(x + w - 14 - bw + 12, y + 21, badge, 18.5f, UI_FONT_SEMIBOLD,
                          net->connected ? COL_TEXT : COL_ACCENT_TEXT);
        } else {
            int d_idx = idx - pg->num_items;
            const struct y1_bt_device *dev = &g_bt_devs[d_idx];
            const char *name = dev->name[0] ? dev->name : TR_ID(STR_UNKNOWN_DEVICE);
            gfx_draw_icon("bluetooth", x + 14, y + 20, 28, COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 7, 220, name, 24.0f,
                                    UI_FONT_SEMIBOLD, COL_TEXT);
            char sub[56];
            snprintf(sub, sizeof(sub), "%02X:%02X:%02X:%02X:%02X:%02X  •  %s",
                     dev->bdaddr[5], dev->bdaddr[4], dev->bdaddr[3],
                     dev->bdaddr[2], dev->bdaddr[1], dev->bdaddr[0],
                     dev->connected ? TR_ID(STR_CONNECTED)
                                    : (dev->paired ? TR_ID(STR_PAIRED_RIGHT_FORGET)
                                                   : TR_ID(STR_DISCOVERED)));
            gfx_draw_text_ellipsize(x + 52, y + 38, 230, sub, 18.0f,
                                    UI_FONT_REGULAR, COL_TEXT_MUTED);

            const char *badge = dev->connected ? TR_ID(STR_DISCONNECT) : TR_ID(STR_CONNECT);
            int bw = gfx_text_width(badge, 18.5f, UI_FONT_SEMIBOLD) + 24;
            gfx_fill_round_rect(x + w - 14 - bw, y + 15, bw, 38, 19,
                                dev->connected ? COL_WARN : COL_ACCENT);
            gfx_draw_text(x + w - 14 - bw + 12, y + 21, badge, 18.5f, UI_FONT_SEMIBOLD,
                          dev->connected ? COL_TEXT : COL_ACCENT_TEXT);
        }
    }

    gfx_reset_clip();
    gfx_draw_scrollbar(UI_W - 12, list_top, visible_rows * step - gap,
                       g_item_scroll_anim, visible_rows, total_rows);

    if (g_editing_slider && g_item_sel < pg->num_items)
        render_slider_modal(&pg->items[g_item_sel]);
    if (g_editing_choice && g_item_sel < pg->num_items)
        render_choice_modal(&pg->items[g_item_sel]);
    if (g_switch_open)
        render_switch_modal();
}

/* =========================================================================
 * On-Screen Keyboard (Wi-Fi Passphrase Entry)
 * ========================================================================= */
static bool g_osk_active = false;
static char g_osk_ssid[64] = "";
static char g_osk_buf[96] = "";
static int  g_osk_len = 0;
static int  g_osk_mode = 0; /* 0 = lower, 1 = upper, 2 = symbols, 3 = hangul, 4 = hangul shift */
static int  g_osk_sel = 0;  /* 0..39 (4 rows x 10 cols) */
static struct hangul_ime g_osk_ime = { -1, -1, -1, 0 };

static const char *const osk_keys[5][40] = {
    {
        "1","2","3","4","5","6","7","8","9","0",
        "q","w","e","r","t","y","u","i","o","p",
        "a","s","d","f","g","h","j","k","l","DEL",
        "ABC","z","x","c","v","b","n","m","SPC","OK"
    },
    {
        "!","@","#","$","%","^","&","*","(",")",
        "Q","W","E","R","T","Y","U","I","O","P",
        "A","S","D","F","G","H","J","K","L","DEL",
        "SYM","Z","X","C","V","B","N","M","SPC","OK"
    },
    {
        "-","_","=","+","[","]","{","}","\\","|",
        ";",":","'","\"",",",".","<",">","/","?",
        "`","~","!","@","#","$","%","^","&","DEL",
        "한글","(",")","*","+","-","_",".","SPC","OK"
    },
    {
        "1","2","3","4","5","6","7","8","9","0",
        "ㅂ","ㅈ","ㄷ","ㄱ","ㅅ","ㅛ","ㅕ","ㅑ","ㅐ","ㅔ",
        "ㅁ","ㄴ","ㅇ","ㄹ","ㅎ","ㅗ","ㅓ","ㅏ","ㅣ","DEL",
        "쌍자","ㅋ","ㅌ","ㅊ","ㅍ","ㅠ","ㅜ","ㅡ","SPC","OK"
    },
    {
        "1","2","3","4","5","6","7","8","9","0",
        "ㅃ","ㅉ","ㄸ","ㄲ","ㅆ","ㅛ","ㅕ","ㅑ","ㅒ","ㅖ",
        "ㅁ","ㄴ","ㅇ","ㄹ","ㅎ","ㅗ","ㅓ","ㅏ","ㅣ","DEL",
        "abc","ㅋ","ㅌ","ㅊ","ㅍ","ㅠ","ㅜ","ㅡ","SPC","OK"
    }
};

static bool cfg_is_osk_fn(const char *lbl)
{
    return (strcmp(lbl, "DEL") == 0 || strcmp(lbl, "SPC") == 0 || strcmp(lbl, "OK") == 0 ||
            strcmp(lbl, "ABC") == 0 || strcmp(lbl, "SYM") == 0 || strcmp(lbl, "abc") == 0 ||
            strcmp(lbl, "한글") == 0 || strcmp(lbl, "쌍자") == 0);
}

static void open_wifi_osk(const char *ssid)
{
    snprintf(g_osk_ssid, sizeof(g_osk_ssid), "%s", ssid);
    g_osk_buf[0] = '\0';
    g_osk_len = 0;
    g_osk_mode = 0;
    g_osk_sel = 10; /* 'q' */
    hime_reset(&g_osk_ime);
    g_osk_active = true;
}

static void render_osk_modal(void)
{
    gfx_set_text_shadow(false);
    int mx = 12, my = 14, mw = UI_W - 24, mh = UI_H - 28;
    gfx_fill_round_rect(mx, my, mw, mh, 36, COL_MODAL_BG);
    gfx_stroke_round_rect(mx, my, mw, mh, 36, 2, COL_BORDER_HI);

    char title[84];
    snprintf(title, sizeof(title), "%s: %s", TR_ID(STR_PASSWORD), g_osk_ssid);
    gfx_draw_text_ellipsize(mx + 18, my + 12, mw - 36, title, 21.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    int bx = mx + 16, by = my + 46, bw = mw - 32, bh = 44;
    gfx_fill_round_rect(bx, by, bw, bh, 22, COL_MODAL_HI);
    const char *disp = g_osk_len > 0 ? g_osk_buf : TR_ID(STR_ENTER_WI_FI_PASSWORD);
    uint32_t dcol = g_osk_len > 0 ? COL_TEXT : COL_TEXT_MUTED;
    gfx_draw_text_ellipsize(bx + 16, by + 10, bw - 32, disp, 21.0f, UI_FONT_SEMIBOLD, dcol);

    int gx = mx + 14, gy = my + 102;
    int key_w = (mw - 28 - 9 * 4) / 10;
    int key_h = 48;
    int gap = 4;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            int idx = r * 10 + c;
            int kx = gx + c * (key_w + gap);
            int ky = gy + r * (key_h + gap);
            bool sel = (idx == g_osk_sel);
            const char *lbl = osk_keys[g_osk_mode][idx];
            bool is_ok = (strcmp(lbl, "OK") == 0);
            bool is_fn = cfg_is_osk_fn(lbl);

            uint32_t bg = sel ? COL_SEL_BG : (is_ok ? COL_SUCCESS : (is_fn ? COL_MODAL_HI : COL_KEY_BG));
            uint32_t fg = sel ? COL_SEL_TEXT : COL_TEXT;
            int rad = (sel || is_ok) ? 18 : 14;
            gfx_fill_round_rect(kx, ky, key_w, key_h, rad, bg);
            if (sel)
                gfx_stroke_round_rect(kx, ky, key_w, key_h, rad, 2, COL_BORDER_HI);

            float fsz = (is_fn && !is_ok) ? 14.5f : 21.0f;
            int tw = gfx_text_width(lbl, fsz, UI_FONT_SEMIBOLD);
            int tx = kx + (key_w - tw) / 2;
            int ty = ky + ((is_fn && !is_ok) ? 16 : 12);
            gfx_draw_text(tx, ty, lbl, fsz, UI_FONT_SEMIBOLD, fg);
        }
    }
}

static void handle_osk_button(uint32_t btn)
{
    int row = g_osk_sel / 10;
    int col = g_osk_sel % 10;

    if (btn == Y1_BTN_MENU) {
        g_osk_active = false;
        return;
    }
    if (btn == Y1_BTN_PLAY) {
        hime_commit(&g_osk_ime, g_osk_buf);
        g_osk_len = (int)strlen(g_osk_buf);
        g_osk_active = false;
        y1_wifi_connect(g_osk_ssid, g_osk_buf);
        show_toast(TR_ID(STR_CONNECTING), 1500);
        return;
    }
    if (btn == Y1_BTN_SCROLL_FWD) {
        row = (row + 1) % 4;
        g_osk_sel = row * 10 + col;
    } else if (btn == Y1_BTN_SCROLL_BACK) {
        row = (row + 3) % 4;
        g_osk_sel = row * 10 + col;
    } else if (btn == Y1_BTN_RIGHT) {
        col = (col + 1) % 10;
        g_osk_sel = row * 10 + col;
    } else if (btn == Y1_BTN_LEFT) {
        col = (col + 9) % 10;
        g_osk_sel = row * 10 + col;
    } else if (btn == Y1_BTN_SELECT) {
        const char *lbl = osk_keys[g_osk_mode][g_osk_sel];
        if (strcmp(lbl, "ABC") == 0 || strcmp(lbl, "SYM") == 0 || strcmp(lbl, "abc") == 0 ||
            strcmp(lbl, "한글") == 0 || strcmp(lbl, "쌍자") == 0) {
            hime_commit(&g_osk_ime, g_osk_buf);
            g_osk_mode = (g_osk_mode + 1) % 5;
        } else if (strcmp(lbl, "DEL") == 0) {
            hime_backspace(&g_osk_ime, g_osk_buf);
            g_osk_len = (int)strlen(g_osk_buf);
        } else if (strcmp(lbl, "SPC") == 0) {
            hime_commit(&g_osk_ime, g_osk_buf);
            if (g_osk_len < (int)sizeof(g_osk_buf) - 1) {
                g_osk_buf[g_osk_len++] = ' ';
                g_osk_buf[g_osk_len] = '\0';
                hime_commit(&g_osk_ime, g_osk_buf);
            }
        } else if (strcmp(lbl, "OK") == 0) {
            hime_commit(&g_osk_ime, g_osk_buf);
            g_osk_len = (int)strlen(g_osk_buf);
            g_osk_active = false;
            y1_wifi_connect(g_osk_ssid, g_osk_buf);
            show_toast(TR_ID(STR_CONNECTING), 1500);
        } else {
            hime_input_key(&g_osk_ime, lbl, g_osk_buf, sizeof(g_osk_buf));
            g_osk_len = (int)strlen(g_osk_buf);
        }
    }
}

static void render_wp_picker(void)
{
    draw_header(g_picker_is_font ? TR_ID(STR_FONT) : TR_ID(STR_WALLPAPER),
                g_picker_is_font ? "sliders" : "image");
    int list_top = 60, row_h = 68, gap = 6, step = row_h + gap, visible = 4;
    if (g_wp_sel < g_wp_scroll) g_wp_scroll = g_wp_sel;
    if (g_wp_sel >= g_wp_scroll + visible) g_wp_scroll = g_wp_sel - visible + 1;
    if (g_wp_scroll < 0) g_wp_scroll = 0;

    int x = 10, w = UI_W - 30;
    int first_idx = (int)floorf(g_wp_scroll_anim) - 1;
    if (first_idx < 0) first_idx = 0;
    int last_idx = first_idx + visible + 3;
    if (last_idx > g_wp_count) last_idx = g_wp_count;

    gfx_set_clip_y(58, UI_H - 4);
    uint64_t tnow = cfg_now_ms();
    for (int idx = first_idx; idx < last_idx; idx++) {
        int y = list_top + (int)roundf(((float)idx - g_wp_scroll_anim) * (float)step);
        if (y + row_h < 58 || y > UI_H) continue;
        bool sel = (idx == g_wp_sel);
        float t = cfg_item_pill_get(100, idx, sel, tnow);
        int r = (int)roundf(18.0f + t * (float)(row_h / 2 - 18));
        uint32_t bg = cfg_lerp_argb(COL_SURFACE, COL_SEL_BG, t);
        gfx_fill_round_rect(x, y, w, row_h, r, bg);
        if (sel)
            gfx_stroke_round_rect(x, y, w, row_h, r, 2, COL_BORDER_HI);

        const struct cfg_wp_entry *we = &g_wp_items[idx];
        const char *wic = we->is_clear ? "trash-2" : (we->is_dir ? "folder" : (g_picker_is_font ? "sliders" : "image"));
        uint32_t wcol = we->is_clear ? COL_DANGER : COL_TEXT;
        gfx_draw_icon(wic, x + 14, y + 20, 28, wcol);
        gfx_draw_text_ellipsize(x + 52, y + 19, w - 98, we->name, 24.0f, UI_FONT_SEMIBOLD, wcol);
        if (we->is_dir) gfx_draw_icon("chevron-right", x + w - 36, y + 22, 24, COL_TEXT);
    }
    gfx_reset_clip();
    gfx_draw_scrollbar(UI_W - 12, list_top, visible * step - gap, g_wp_scroll_anim, visible, g_wp_count);
}

static void render_frame(void)
{
    int ox = (int)roundf(g_trans_x);
    gfx_draw_wallpaper(g_bg_blur_anim);
    if (g_wp_open)
        render_wp_picker();
    else if (g_active_page < 0)
        render_home(ox);
    else
        render_page(ox);
    if (g_osk_active)
        render_osk_modal();
    draw_abc_badge();
    draw_toast_overlay();
    gfx_present();
}

static void handle_button(uint32_t btn)
{
    if (g_wp_open) {
        if (btn == Y1_BTN_MENU) { g_wp_open = false; return; }
        if (g_wp_count <= 0) return;
        if (btn == Y1_BTN_SCROLL_FWD) g_wp_sel = (g_wp_sel + 1) % g_wp_count;
        else if (btn == Y1_BTN_SCROLL_BACK) g_wp_sel = (g_wp_sel + g_wp_count - 1) % g_wp_count;
        else if (btn == Y1_BTN_SELECT) {
            const struct cfg_wp_entry *we = &g_wp_items[g_wp_sel];
            if (we->is_clear) {
                if (g_picker_is_font) {
                    gfx_reset_default_font();
                    show_toast(TR_ID(STR_DEFAULT_FONT_RESTORED), 1200);
                } else {
                    gfx_clear_wallpaper();
                    show_toast(TR_ID(STR_WALLPAPER_CLEARED), 1200);
                }
                g_wp_open = false;
            } else if (we->is_dir) {
                cfg_scan_wp_dir(we->path);
            } else if (g_picker_is_font) {
                if (gfx_set_custom_font(we->path)) {
                    g_wp_open = false;
                    show_toast(TR_ID(STR_FONT_APPLIED), 1400);
                } else {
                    show_toast(TR_ID(STR_FAILED_TO_LOAD_FONT), 1400);
                }
            } else {
                show_toast(TR_ID(STR_APPLYING_WALLPAPER), 1000);
                render_frame();
                if (gfx_set_wallpaper_from_image(we->path)) {
                    g_wp_open = false;
                    show_toast(TR_ID(STR_WALLPAPER_APPLIED), 1400);
                } else {
                    show_toast(TR_ID(STR_FAILED_TO_LOAD_IMAGE), 1400);
                }
            }
        }
        return;
    }
    if (g_osk_active) {
        handle_osk_button(btn);
        return;
    }
    if (g_active_page < 0) {
        if (btn == Y1_BTN_SCROLL_FWD || btn == Y1_BTN_RIGHT) {
            g_home_sel = (g_home_sel + 1) % NUM_PAGES;
        } else if (btn == Y1_BTN_SCROLL_BACK || btn == Y1_BTN_LEFT) {
            g_home_sel = (g_home_sel + NUM_PAGES - 1) % NUM_PAGES;
        } else if (btn == Y1_BTN_SELECT || btn == Y1_BTN_PLAY) {
            g_active_page = g_home_sel;
            g_trans_x = 90.0f;
            g_item_sel = 0;
            g_item_scroll = 0;
            g_item_scroll_anim = 0.0f;
            cfg_item_pill_clear_scope(g_active_page);
            g_editing_slider = false;
            g_editing_choice = false;
            g_switch_open = false;
            if (strcmp(g_pages[g_active_page].id, "datetime") == 0)
                load_datetime_from_sys();
            if (g_pages[g_active_page].is_bt_page)
                y1_bt_init();
            if (g_pages[g_active_page].is_wifi_page)
                y1_wifi_init();
            if (g_pages[g_active_page].is_launch_page)
                scan_launch_apps();
            if (g_pages[g_active_page].is_debug_page)
                refresh_debug_info();
        } else if (btn == Y1_BTN_MENU) {
            g_should_exit = true;
        }
        return;
    }

    const struct cfg_page *pg = &g_pages[g_active_page];

    if (g_switch_open) {
        if (btn == Y1_BTN_MENU) {
            g_switch_open = false;
        } else if (btn == Y1_BTN_SCROLL_FWD || btn == Y1_BTN_SCROLL_BACK ||
                   btn == Y1_BTN_LEFT || btn == Y1_BTN_RIGHT) {
            g_switch_sel = g_switch_sel ? 0 : 1;
        } else if (btn == Y1_BTN_SELECT) {
            if (g_switch_sel == 1 && g_switch_target_idx >= 0 && g_switch_target_idx < g_launch_count) {
                execute_app_switch(g_launch_apps[g_switch_target_idx].exec_path, g_switch_pid);
            }
            g_switch_open = false;
        }
        return;
    }

    if (g_editing_choice && g_item_sel < pg->num_items) {
        const struct cfg_item *it = &pg->items[g_item_sel];
        int n = it->num_choices;
        if (n > 0) {
            if (btn == Y1_BTN_SCROLL_FWD || btn == Y1_BTN_RIGHT) {
                g_choice_sel = (g_choice_sel + 1) % n;
            } else if (btn == Y1_BTN_SCROLL_BACK || btn == Y1_BTN_LEFT) {
                g_choice_sel = (g_choice_sel + n - 1) % n;
            } else if (btn == Y1_BTN_SELECT) {
                if (it->set_int) it->set_int(g_choice_sel);
                g_editing_choice = false;
            } else if (btn == Y1_BTN_MENU) {
                g_editing_choice = false;
            }
        } else {
            g_editing_choice = false;
        }
        return;
    }

    if (g_editing_slider && g_item_sel < pg->num_items) {
        const struct cfg_item *it = &pg->items[g_item_sel];
        int val = it->get_int ? it->get_int() : it->min_val;
        if (btn == Y1_BTN_SCROLL_FWD || btn == Y1_BTN_RIGHT) {
            val += it->step;
            if (val > it->max_val) val = it->max_val;
            if (it->set_int) it->set_int(val);
        } else if (btn == Y1_BTN_SCROLL_BACK || btn == Y1_BTN_LEFT) {
            val -= it->step;
            if (val < it->min_val) val = it->min_val;
            if (it->set_int) it->set_int(val);
        } else if (btn == Y1_BTN_SELECT || btn == Y1_BTN_MENU) {
            g_editing_slider = false;
            g_editing_choice = false;
            g_switch_open = false;
        }
        return;
    }

    int extra_rows = 0;
    if (pg->is_bt_page) extra_rows = g_bt_dev_count;
    else if (pg->is_wifi_page) extra_rows = g_wifi_net_count;
    else if (pg->is_launch_page) extra_rows = g_launch_count;
    else if (pg->is_debug_page) extra_rows = g_dbg_count;
    int total_rows = pg->num_items + extra_rows;
    if (btn == Y1_BTN_SCROLL_FWD || btn == Y1_BTN_SCROLL_BACK) {
        if (total_rows > 0) {
            int dir = (btn == Y1_BTN_SCROLL_FWD) ? 1 : -1;
            uint64_t tnow = cfg_now_ms();
            if (dir == g_scroll_last_dir && (tnow - g_scroll_last_ms) <= 95)
                g_scroll_fast_streak++;
            else
                g_scroll_fast_streak = 1;
            g_scroll_last_dir = dir;
            g_scroll_last_ms = tnow;

            if (total_rows >= 8 && (g_abc_active || g_scroll_fast_streak >= 20)) {
                g_abc_active = true;
                g_abc_until_ms = tnow + 1100;
                char cur_l[8] = "";
                int cur_b = cfg_abc_bucket(cfg_row_label(pg, g_item_sel), cur_l, sizeof(cur_l));
                int next_idx = (g_item_sel + total_rows + dir) % total_rows;
                for (int step = 1; step < total_rows; step++) {
                    int cand = (g_item_sel + total_rows + dir * step) % total_rows;
                    char cl[8] = "";
                    int cb = cfg_abc_bucket(cfg_row_label(pg, cand), cl, sizeof(cl));
                    if (cb != cur_b) {
                        next_idx = cand;
                        snprintf(g_abc_label, sizeof(g_abc_label), "%s", cl);
                        break;
                    }
                }
                g_item_sel = next_idx;
                if (!g_abc_label[0])
                    cfg_abc_bucket(cfg_row_label(pg, g_item_sel), g_abc_label, sizeof(g_abc_label));
            } else {
                g_item_sel = (g_item_sel + total_rows + dir) % total_rows;
            }
        }
    } else if (btn == Y1_BTN_MENU) {
        g_active_page = -1;
        g_trans_x = -90.0f;
    } else if (btn == Y1_BTN_SELECT || btn == Y1_BTN_RIGHT || btn == Y1_BTN_LEFT) {
        if (g_item_sel < pg->num_items) {
            const struct cfg_item *it = &pg->items[g_item_sel];
            if (it->type == CFG_TYPE_TOGGLE && it->get_int && it->set_int) {
                it->set_int(!it->get_int());
            } else if (it->type == CFG_TYPE_TRISTATE && it->get_int && it->set_int) {
                int st = it->get_int();
                if (st < 0 || st > 2) st = 1;
                if (btn == Y1_BTN_LEFT) {
                    if (st > 0) it->set_int(st - 1);
                } else if (btn == Y1_BTN_RIGHT) {
                    if (st < 2) it->set_int(st + 1);
                } else {
                    it->set_int((st + 1) % 3);
                }
            } else if (it->type == CFG_TYPE_CHOICE && it->get_int && it->set_int && it->num_choices > 0) {
                if (btn == Y1_BTN_SELECT && it->num_choices >= 2) {
                    g_choice_sel = it->get_int();
                    if (g_choice_sel < 0 || g_choice_sel >= it->num_choices) g_choice_sel = 0;
                    g_editing_choice = true;
                } else {
                    int c = it->get_int();
                    int dir = (btn == Y1_BTN_LEFT) ? -1 : 1;
                    c = (c + it->num_choices + dir) % it->num_choices;
                    it->set_int(c);
                }
            } else if (it->type == CFG_TYPE_INT && btn == Y1_BTN_SELECT) {
                g_editing_slider = true;
            } else if (it->type == CFG_TYPE_INT && (btn == Y1_BTN_RIGHT || btn == Y1_BTN_LEFT)) {
                int v = it->get_int ? it->get_int() : it->min_val;
                v += (btn == Y1_BTN_RIGHT ? it->step : -it->step);
                if (v < it->min_val) v = it->min_val;
                if (v > it->max_val) v = it->max_val;
                if (it->set_int) it->set_int(v);
            } else if (it->type == CFG_TYPE_ACTION && btn == Y1_BTN_SELECT && it->on_action) {
                it->on_action();
            }
        } else if (pg->is_launch_page && btn == Y1_BTN_SELECT) {
            int l_idx = g_item_sel - pg->num_items;
            if (l_idx >= 0 && l_idx < g_launch_count) {
                pid_t run_pid = !g_boot_mode ? find_running_app_pid(g_switch_cur_name, sizeof(g_switch_cur_name)) : 0;
                if (run_pid > 0) {
                    g_switch_pid = run_pid;
                    g_switch_target_idx = l_idx;
                    g_switch_sel = 1;
                    g_switch_open = true;
                } else {
                    FILE *lf = fopen("/tmp/configapp_launch", "w");
                    if (lf) {
                        fprintf(lf, "%s\n", g_launch_apps[l_idx].exec_path);
                        fclose(lf);
                    }
                    if (g_boot_mode) {
                        g_should_exit = true;
                    } else if (strcmp(g_launch_apps[l_idx].exec_path, "__ANDROID__") == 0) {
                        show_toast(TR_ID(STR_REBOOT_TO_ANDROID_FROM_BOOT), 1500);
                    } else {
                        show_toast(TR_ID(STR_LAUNCHING), 1000);
                        g_should_exit = true;
                    }
                }
            }
        } else if (pg->is_wifi_page) {
            int w_idx = g_item_sel - pg->num_items;
            if (w_idx >= 0 && w_idx < g_wifi_net_count) {
                if (btn == Y1_BTN_RIGHT && g_wifi_nets[w_idx].saved) {
                    y1_wifi_forget(g_wifi_nets[w_idx].ssid);
                    show_toast(TR_ID(STR_NETWORK_FORGOTTEN), 1200);
                } else if (btn == Y1_BTN_LEFT && g_wifi_nets[w_idx].secured) {
                    open_wifi_osk(g_wifi_nets[w_idx].ssid);
                } else if (btn == Y1_BTN_SELECT) {
                    if (g_wifi_nets[w_idx].connected) {
                        y1_wifi_disconnect();
                        show_toast(TR_ID(STR_DISCONNECTING), 1200);
                    } else if (!g_wifi_nets[w_idx].secured || g_wifi_nets[w_idx].saved) {
                        y1_wifi_connect(g_wifi_nets[w_idx].ssid, NULL);
                        show_toast(TR_ID(STR_CONNECTING), 1500);
                    } else {
                        open_wifi_osk(g_wifi_nets[w_idx].ssid);
                    }
                }
            }
        } else if (pg->is_bt_page) {
            int d_idx = g_item_sel - pg->num_items;
            if (d_idx >= 0 && d_idx < g_bt_dev_count) {
                if (btn == Y1_BTN_RIGHT && g_bt_devs[d_idx].paired) {
                    y1_bt_forget_device(g_bt_devs[d_idx].bdaddr);
                    show_toast(TR_ID(STR_DEVICE_FORGOTTEN), 1200);
                } else if (btn == Y1_BTN_SELECT) {
                    if (g_bt_devs[d_idx].connected) {
                        y1_bt_disconnect();
                        show_toast(TR_ID(STR_DISCONNECTING), 1200);
                    } else {
                        y1_bt_connect_device(g_bt_devs[d_idx].bdaddr);
                        show_toast(TR_ID(STR_CONNECTING), 1500);
                    }
                }
            }
        }
    }
}

static int cli_launch_bg(const char *exec_path, const char *log_name)
{
    if (!exec_path || !exec_path[0]) return 1;
    if (strcmp(exec_path, "__ANDROID__") == 0) {
        printf("Rebooting to Android...\n");
        save_settings_conf();
        y1_sys_reboot();
        return 0;
    }
    if (access(exec_path, X_OK) != 0) {
        fprintf(stderr, "Not executable: %s\n", exec_path);
        return 1;
    }

    /* If ConfigApp GUI is currently running in the player, ask init to switch cleanly
     * without tearing down active Serial/ADB sessions. */
    DIR *d = opendir("/proc");
    pid_t gui_pid = 0;
    if (d) {
        pid_t self = getpid();
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            pid_t pid = (pid_t)atoi(de->d_name);
            if (pid <= 1 || pid == self) continue;
            char cpath[64], comm[64] = "";
            snprintf(cpath, sizeof(cpath), "/proc/%d/comm", (int)pid);
            FILE *f = fopen(cpath, "r");
            if (!f) continue;
            if (fgets(comm, sizeof(comm), f)) {
                char *nl = strchr(comm, '\n');
                if (nl) *nl = '\0';
                if (strcmp(comm, "configapp") == 0) {
                    gui_pid = pid;
                    fclose(f);
                    break;
                }
            }
            fclose(f);
        }
        closedir(d);
    }
    if (gui_pid > 0) {
        FILE *lf = fopen("/tmp/configapp_launch", "w");
        if (lf) { fprintf(lf, "%s\n", exec_path); fclose(lf); }
        FILE *sf = fopen("/tmp/configapp_switching", "w");
        if (sf) { fprintf(sf, "1\n"); fclose(sf); }
        kill(gui_pid, SIGTERM);
        printf("Switching from ConfigApp GUI (PID %d) to %s (serial/ADB kept active)\n",
               (int)gui_pid, exec_path);
        return 0;
    }

    char log_path[128];
    snprintf(log_path, sizeof(log_path), "/tmp/%s.log", log_name && log_name[0] ? log_name : "app");
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, 0); if (devnull > 2) close(devnull); }
        int lfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lfd >= 0) { dup2(lfd, 1); dup2(lfd, 2); if (lfd > 2) close(lfd); }
        execl(exec_path, exec_path, (char *)NULL);
        _exit(127);
    }
    printf("Started %s in background (PID %d, log: %s)\n", exec_path, (int)pid, log_path);
    return 0;
}

static void fb_read_rgb(const uint8_t *fb, int bpp, int stride, int xoff, int yoff,
                        int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const uint8_t *src = fb + (size_t)(y + yoff) * (size_t)stride;
    if (bpp == 32) {
        uint32_t c = ((const uint32_t *)src + xoff)[x];
        *b = (uint8_t)(c & 0xFF);
        *g = (uint8_t)((c >> 8) & 0xFF);
        *r = (uint8_t)((c >> 16) & 0xFF);
    } else if (bpp == 16) {
        uint16_t c = ((const uint16_t *)src + xoff)[x];
        *b = (uint8_t)((c & 0x1F) << 3);
        *g = (uint8_t)(((c >> 5) & 0x3F) << 2);
        *r = (uint8_t)(((c >> 11) & 0x1F) << 3);
    } else {
        *r = *g = *b = 0;
    }
}

static void write_base64_ascii(const uint8_t *data, size_t len)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int col = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16);
        if (i + 1 < len) v |= ((uint32_t)data[i + 1] << 8);
        if (i + 2 < len) v |= (uint32_t)data[i + 2];

        putchar(b64[(v >> 18) & 0x3F]);
        putchar(b64[(v >> 12) & 0x3F]);
        putchar((i + 1 < len) ? b64[(v >> 6) & 0x3F] : '=');
        putchar((i + 2 < len) ? b64[v & 0x3F] : '=');
        col += 4;
        if (col >= 76) {
            putchar('\n');
            col = 0;
        }
    }
    if (col > 0) putchar('\n');
}

static int cli_take_screenshot(const char *out_path)
{
    int fd = open("/dev/graphics/fb0", O_RDONLY);
    if (fd < 0) fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) {
        perror("open fb0");
        return 1;
    }
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("ioctl fb");
        close(fd);
        return 1;
    }

    int w = (int)vinfo.xres;
    int h = (int)vinfo.yres;
    int bpp = (int)vinfo.bits_per_pixel;
    int stride = (int)finfo.line_length;
    int xoff = (int)vinfo.xoffset;
    int yoff = (int)vinfo.yoffset;
    size_t map_sz = (size_t)finfo.smem_len;
    if (map_sz == 0) map_sz = (size_t)vinfo.yres_virtual * (size_t)stride;

    const uint8_t *fb = (const uint8_t *)mmap(NULL, map_sz, PROT_READ, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap fb");
        close(fd);
        return 1;
    }

    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    uint32_t img_sz = (uint32_t)((row_bytes + pad) * h);
    uint32_t file_sz = 54U + img_sz;

    uint8_t *bmp = (uint8_t *)calloc(1, file_sz);
    if (!bmp) {
        fprintf(stderr, "Out of memory\n");
        munmap((void *)fb, map_sz);
        close(fd);
        return 1;
    }

    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = (uint8_t)(file_sz);       bmp[3] = (uint8_t)(file_sz >> 8);
    bmp[4] = (uint8_t)(file_sz >> 16); bmp[5] = (uint8_t)(file_sz >> 24);
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = (uint8_t)(w);       bmp[19] = (uint8_t)(w >> 8);
    bmp[20] = (uint8_t)(w >> 16); bmp[21] = (uint8_t)(w >> 24);
    bmp[22] = (uint8_t)(h);       bmp[23] = (uint8_t)(h >> 8);
    bmp[24] = (uint8_t)(h >> 16); bmp[25] = (uint8_t)(h >> 24);
    bmp[26] = 1;
    bmp[28] = 24;
    bmp[34] = (uint8_t)(img_sz);       bmp[35] = (uint8_t)(img_sz >> 8);
    bmp[36] = (uint8_t)(img_sz >> 16); bmp[37] = (uint8_t)(img_sz >> 24);

    uint8_t *dst = bmp + 54;
    int row_stride = row_bytes + pad;
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            fb_read_rgb(fb, bpp, stride, xoff, yoff, x, y, &r, &g, &b);
            dst[x * 3 + 0] = b;
            dst[x * 3 + 1] = g;
            dst[x * 3 + 2] = r;
        }
        dst += row_stride;
    }

    munmap((void *)fb, map_sz);
    close(fd);

    if (!out_path || strcmp(out_path, "-") == 0) {
        printf("-----BEGIN BMP BASE64 (%dx%d)-----\n", w, h);
        write_base64_ascii(bmp, file_sz);
        printf("-----END BMP BASE64-----\n");
        fflush(stdout);
    } else if (strcmp(out_path, "--zmodem") == 0 || strcmp(out_path, "-z") == 0) {
        char zname[64];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        if (tm) {
            snprintf(zname, sizeof(zname), "shot_%04d%02d%02d_%02d%02d%02d.bmp",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
        } else {
            snprintf(zname, sizeof(zname), "screenshot.bmp");
        }
        int zrc = zmodem_send_buf(zname, bmp, file_sz);
        free(bmp);
        return zrc;
    } else {
        FILE *f = fopen(out_path, "wb");
        if (!f) {
            perror("fopen screenshot");
            free(bmp);
            return 1;
        }
        fwrite(bmp, 1, file_sz, f);
        fclose(f);
        printf("Saved screenshot to %s (%dx%d %dbpp)\n", out_path, w, h, bpp);
    }

    free(bmp);
    return 0;
}

static void cli_print_help(void)
{
    printf(
        "Usage: configapp <command> [args]\n"
        "  status                       Show battery, CPU, RAM, storage, kernel, uptime\n"
        "  list                         List all settings and launchable apps\n"
        "  get <key>                    Get a setting value (or default_player)\n"
        "  set <key> <val>              Set a setting value and apply immediately\n"
        "  launch [app|path]            Launch app in background (terminal stays free)\n"
        "  gui                          Launch ConfigApp GUI in background\n"
        "  stop <proc_name>             Stop a running app (e.g. rockbox, meui)\n"
        "  log <name>                   Tail /tmp/<name>.log\n"
        "  screenshot [--zmodem|out.bmp] Capture framebuffer (Base64 / ZMODEM / BMP)\n"
        "  sz <file>                    Send file over ZMODEM\n"
        "  rz [dir]                     Receive file(s) over ZMODEM\n"
        "  adb <on|off|status>          Enable/disable USB ADB daemon\n"
        "  bt <on|off|rx|rx_off|scan|status|disconnect>\n"
        "  wifi <on|off|scan|status|disconnect|connect <ssid> [pass]>\n"
        "  action <sync|drop_caches|clear_log|reset|debuginfo|vibrate|reboot|poweroff>\n"
    );
}

static int run_configapp_cli(int argc, char **argv)
{
    load_settings_conf();
    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        cli_print_help();
        return 0;
    }
    if (strcmp(cmd, "status") == 0 || strcmp(cmd, "info") == 0) {
        char buf[128];
        info_battery(buf, sizeof(buf)); printf("Battery:  %s\n", buf);
        info_current(buf, sizeof(buf)); printf("Power:    %s\n", buf);
        info_cpu(buf, sizeof(buf));     printf("CPU:      %s\n", buf);
        info_memory(buf, sizeof(buf));  printf("Memory:   %s\n", buf);
        info_storage(buf, sizeof(buf)); printf("Storage:  %s\n", buf);
        info_kernel(buf, sizeof(buf));  printf("Kernel:   %s\n", buf);
        info_uptime(buf, sizeof(buf));  printf("Uptime:   %s\n", buf);
        printf("Default:  %s\n", default_player_choices[get_default_player()]);
        return 0;
    }
    if (strcmp(cmd, "list") == 0) {
        printf("Settings:\n");
        printf("  lang=%d dark_mode=%d brightness_pct=%d eco_mode=%d touch_scan=%d vibrate_ms=%d\n",
               g_cfg.lang, g_cfg.dark_mode, g_cfg.brightness_pct, g_cfg.eco_mode,
               g_cfg.touch_scan, g_cfg.vibrate_ms);
        printf("  cpu_mode=%d dual_core=%d screen_off_clamp=%d usb_ipod_mode=%d usb_perf_tune=%d\n",
               g_cfg.cpu_mode, g_cfg.dual_core, g_cfg.screen_off_clamp,
               g_cfg.usb_ipod_mode, g_cfg.usb_perf_tune);
        printf("  debug_log=%d bt_codec=%d screenshot=%d default_player=%s\n",
               g_cfg.debug_log, g_cfg.bt_codec, g_cfg.screenshot,
               default_player_choices[get_default_player()]);
        scan_launch_apps();
        printf("Launchable apps (%d):\n", g_launch_count);
        for (int i = 0; i < g_launch_count; i++)
            printf("  %-14s -> %s\n", g_launch_apps[i].name, g_launch_apps[i].exec_path);
        return 0;
    }
    if (strcmp(cmd, "get") == 0 && argc >= 3) {
        const char *k = argv[2];
        if (strcmp(k, "default_player") == 0) printf("%s\n", default_player_choices[get_default_player()]);
        else if (strcmp(k, "lang") == 0) printf("%d\n", g_cfg.lang);
        else if (strcmp(k, "dark_mode") == 0) printf("%d\n", g_cfg.dark_mode);
        else if (strcmp(k, "brightness_pct") == 0) printf("%d\n", g_cfg.brightness_pct);
        else if (strcmp(k, "eco_mode") == 0) printf("%d\n", g_cfg.eco_mode);
        else if (strcmp(k, "touch_scan") == 0) printf("%d\n", g_cfg.touch_scan);
        else if (strcmp(k, "vibrate_ms") == 0) printf("%d\n", g_cfg.vibrate_ms);
        else if (strcmp(k, "cpu_mode") == 0) printf("%d\n", g_cfg.cpu_mode);
        else if (strcmp(k, "dual_core") == 0) printf("%d\n", g_cfg.dual_core);
        else if (strcmp(k, "screen_off_clamp") == 0) printf("%d\n", g_cfg.screen_off_clamp);
        else if (strcmp(k, "usb_ipod_mode") == 0) printf("%d\n", g_cfg.usb_ipod_mode);
        else if (strcmp(k, "usb_perf_tune") == 0) printf("%d\n", g_cfg.usb_perf_tune);
        else if (strcmp(k, "debug_log") == 0) printf("%d\n", g_cfg.debug_log);
        else if (strcmp(k, "bt_codec") == 0) printf("%d\n", g_cfg.bt_codec);
        else if (strcmp(k, "screenshot") == 0) printf("%d\n", g_cfg.screenshot);
        else { fprintf(stderr, "Unknown key: %s\n", k); return 1; }
        return 0;
    }
    if (strcmp(cmd, "set") == 0 && argc >= 4) {
        const char *k = argv[2];
        const char *vs = argv[3];
        int v = atoi(vs);
        if (strcmp(k, "default_player") == 0) {
            int idx = (strcasecmp(vs, "meui") == 0) ? 1 : (strcasecmp(vs, "configapp") == 0) ? 2 : 0;
            set_default_player(idx);
            return 0;
        }
        if (strcmp(k, "lang") == 0) g_cfg.lang = (v == 1) ? 1 : 0;
        else if (strcmp(k, "dark_mode") == 0) g_cfg.dark_mode = v ? 1 : 0;
        else if (strcmp(k, "brightness_pct") == 0) { g_cfg.brightness_pct = (v < 5) ? 5 : (v > 100 ? 100 : v); y1_backlight_set_percent(g_cfg.brightness_pct); }
        else if (strcmp(k, "eco_mode") == 0) { g_cfg.eco_mode = v ? 1 : 0; y1_fb_set_eco_mode(g_cfg.eco_mode != 0); }
        else if (strcmp(k, "touch_scan") == 0) { g_cfg.touch_scan = v ? 1 : 0; y1_input_set_touch_scanning(g_cfg.touch_scan != 0); }
        else if (strcmp(k, "vibrate_ms") == 0) g_cfg.vibrate_ms = (v < 5) ? 5 : (v > 100 ? 100 : v);
        else if (strcmp(k, "cpu_mode") == 0) { g_cfg.cpu_mode = (v < -1) ? -1 : (v > 4 ? 4 : v); if (g_cfg.cpu_mode >= 0) y1_cpu_set_mode(g_cfg.cpu_mode); }
        else if (strcmp(k, "dual_core") == 0) { g_cfg.dual_core = (v < 0) ? 0 : (v > 2 ? 2 : v); if (g_cfg.dual_core != 1) y1_cpu_set_dual_core(g_cfg.dual_core == 2); }
        else if (strcmp(k, "screen_off_clamp") == 0) { g_cfg.screen_off_clamp = (v < 0) ? 0 : (v > 2 ? 2 : v); if (g_cfg.screen_off_clamp != 1) y1_cpu_set_screen_off_clamp(g_cfg.screen_off_clamp == 2); }
        else if (strcmp(k, "usb_ipod_mode") == 0) { g_cfg.usb_ipod_mode = (v < 0) ? 0 : (v > 2 ? 2 : v); if (g_cfg.usb_ipod_mode != 1) y1_usb_set_ipod_mode(g_cfg.usb_ipod_mode == 2); }
        else if (strcmp(k, "usb_perf_tune") == 0) { g_cfg.usb_perf_tune = v ? 1 : 0; if (y1_usb_is_storage_attached()) y1_usb_tune_performance(g_cfg.usb_perf_tune != 0); }
        else if (strcmp(k, "debug_log") == 0) { g_cfg.debug_log = v ? 1 : 0; y1_log_set_enabled(g_cfg.debug_log != 0); }
        else if (strcmp(k, "bt_codec") == 0) { g_cfg.bt_codec = (v < 0) ? 0 : (v > 2 ? 2 : v); y1_bt_set_active_device_codec(g_cfg.bt_codec); }
        else if (strcmp(k, "screenshot") == 0) g_cfg.screenshot = v ? 1 : 0;
        else { fprintf(stderr, "Unknown key: %s\n", k); return 1; }
        save_settings_conf();
        printf("Updated %s=%s\n", k, vs);
        return 0;
    }
    if (strcmp(cmd, "gui") == 0) {
        const char *exe = access("/mnt/sdcard/.system/configapp/configapp", X_OK) == 0
                        ? "/mnt/sdcard/.system/configapp/configapp"
                        : "/.system/configapp/configapp";
        return cli_launch_bg(exe, "configapp");
    }
    if (strcmp(cmd, "launch") == 0) {
        scan_launch_apps();
        if (argc < 3) {
            for (int i = 0; i < g_launch_count; i++)
                printf("%s (%s)\n", g_launch_apps[i].name, g_launch_apps[i].exec_path);
            return 0;
        }
        const char *target = argv[2];
        for (int i = 0; i < g_launch_count; i++) {
            if (strcasecmp(target, g_launch_apps[i].name) == 0 ||
                strcasecmp(target, g_launch_apps[i].folder) == 0 ||
                strcmp(target, g_launch_apps[i].exec_path) == 0) {
                return cli_launch_bg(g_launch_apps[i].exec_path, g_launch_apps[i].name);
            }
        }
        if (target[0] == '/')
            return cli_launch_bg(target, "custom");
        fprintf(stderr, "App not found: %s\n", target);
        return 1;
    }
    if (strcmp(cmd, "stop") == 0 && argc >= 3) {
        char pcmd[128];
        snprintf(pcmd, sizeof(pcmd), "killall '%s' 2>/dev/null; sleep 1; killall -9 '%s' 2>/dev/null", argv[2], argv[2]);
        (void)!system(pcmd);
        return 0;
    }
    if (strcmp(cmd, "log") == 0 && argc >= 3) {
        char pcmd[128];
        snprintf(pcmd, sizeof(pcmd), "tail -f '/tmp/%s.log'", argv[2]);
        return system(pcmd);
    }
    if (strcmp(cmd, "screenshot") == 0) {
        return cli_take_screenshot(argc >= 3 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "sz") == 0 && argc >= 3) {
        return zmodem_send_file(argv[2]);
    }
    if (strcmp(cmd, "rz") == 0) {
        return zmodem_recv_dir(argc >= 3 ? argv[2] : ".");
    }
    if (strcmp(cmd, "adb") == 0 && argc >= 3) {
        const char *sub = argv[2];
        char cur_fn[64] = "";
        if (y1_sysfs_read("/sys/class/android_usb/android0/functions", cur_fn, sizeof(cur_fn))) {
            g_serial_active = (strstr(cur_fn, "acm") != NULL);
            g_adb_active = (strstr(cur_fn, "adb") != NULL);
        }
        if (strcmp(sub, "on") == 0) { set_adb_active(1); printf("ADB enabled\n"); }
        else if (strcmp(sub, "off") == 0) { set_adb_active(0); printf("ADB disabled\n"); }
        else if (strcmp(sub, "status") == 0) { printf("ADB: %s (functions: %s)\n", g_adb_active ? "active" : "off", cur_fn); }
        else { fprintf(stderr, "Unknown adb subcommand: %s\n", sub); return 1; }
        return 0;
    }
    if (strcmp(cmd, "bt") == 0 && argc >= 3) {
        const char *sub = argv[2];
        y1_bt_init();
        if (strcmp(sub, "on") == 0) y1_bt_set_enabled(true);
        else if (strcmp(sub, "off") == 0) y1_bt_set_enabled(false);
        else if (strcmp(sub, "rx") == 0) y1_bt_set_receiver_mode(true);
        else if (strcmp(sub, "rx_off") == 0) y1_bt_set_receiver_mode(false);
        else if (strcmp(sub, "scan") == 0) y1_bt_start_scan();
        else if (strcmp(sub, "disconnect") == 0) y1_bt_disconnect();
        else if (strcmp(sub, "status") == 0) printf("%s\n", y1_bt_get_status_str());
        else { fprintf(stderr, "Unknown bt subcommand: %s\n", sub); return 1; }
        return 0;
    }
    if (strcmp(cmd, "wifi") == 0 && argc >= 3) {
        const char *sub = argv[2];
        y1_wifi_init();
        if (strcmp(sub, "on") == 0) y1_wifi_set_enabled(true);
        else if (strcmp(sub, "off") == 0) y1_wifi_set_enabled(false);
        else if (strcmp(sub, "scan") == 0) y1_wifi_start_scan();
        else if (strcmp(sub, "disconnect") == 0) y1_wifi_disconnect();
        else if (strcmp(sub, "connect") == 0 && argc >= 4) y1_wifi_connect(argv[3], argc >= 5 ? argv[4] : NULL);
        else if (strcmp(sub, "status") == 0) {
            char ip[32] = ""; y1_wifi_get_ip(ip, sizeof(ip));
            printf("%s (IP: %s)\n", y1_wifi_get_status_str(), ip[0] ? ip : "None");
        } else { fprintf(stderr, "Unknown wifi subcommand: %s\n", sub); return 1; }
        return 0;
    }
    if (strcmp(cmd, "action") == 0 && argc >= 3) {
        const char *sub = argv[2];
        if (strcmp(sub, "sync") == 0) act_sync_disks();
        else if (strcmp(sub, "drop_caches") == 0) act_drop_caches();
        else if (strcmp(sub, "clear_log") == 0) act_clear_log();
        else if (strcmp(sub, "reset") == 0) act_reset_settings();
        else if (strcmp(sub, "debuginfo") == 0) act_save_debug_info();
        else if (strcmp(sub, "vibrate") == 0) act_vibrate_test();
        else if (strcmp(sub, "reboot") == 0) act_reboot();
        else if (strcmp(sub, "poweroff") == 0) act_poweroff();
        else { fprintf(stderr, "Unknown action: %s\n", sub); return 1; }
        return 0;
    }

    cli_print_help();
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bootmenu") == 0)
            g_boot_mode = true;
    }
    if (argc > 1 && !g_boot_mode)
        return run_configapp_cli(argc, argv);

    y1_fb_init();
    y1_input_set_touch_scanning(true);
    y1_input_init();
    {
        /* Flush any residual key events from parent caller (e.g. MeUI) */
        struct y1_input_event flush_ev;
        while (y1_input_poll(&flush_ev, 0) > 0) {}
    }

    load_settings_conf();
    apply_all_settings();

    gfx_init();
    g_cached_cpu_mhz = y1_cpu_get_freq_mhz();
    g_cached_bat_pct = y1_battery_percent();
    g_cached_bat_chg = y1_battery_is_charging();
    render_frame();

    struct timespec last_refresh;
    clock_gettime(CLOCK_MONOTONIC, &last_refresh);
    uint64_t last_hw_ms = cfg_now_ms();
    uint64_t last_render_dur_ms = 0;
    bool dirty = true;

    while (!g_should_exit) {
        uint64_t tnow = cfg_now_ms();
        if (tnow - last_hw_ms >= 5000) {
            last_hw_ms = tnow;
            int nc = y1_cpu_get_freq_mhz();
            int nb = y1_battery_percent();
            bool nchg = y1_battery_is_charging();
            if (nc != g_cached_cpu_mhz || nb != g_cached_bat_pct || nchg != g_cached_bat_chg) {
                g_cached_cpu_mhz = nc;
                g_cached_bat_pct = nb;
                g_cached_bat_chg = nchg;
                dirty = true;
            }
        }

        float target_blur = (g_active_page < 0 && !g_wp_open) ? 0.0f : 1.0f;
        bool anim_pending = (fabsf(g_trans_x) > 0.5f ||
                             fabsf(g_bg_blur_anim - target_blur) > 0.02f ||
                             fabsf(g_wp_scroll_anim - (float)g_wp_scroll) > 0.02f ||
                             fabsf(g_home_row_scroll_anim - (float)g_home_row_scroll) > 0.02f ||
                             fabsf(g_item_scroll_anim - (float)g_item_scroll) > 0.02f ||
                             cfg_item_pill_any_active() ||
                             cfg_sw_any_active());
        int wait_ms = (dirty || anim_pending)
            ? ((last_render_dur_ms < 15) ? (16 - (int)last_render_dur_ms) : 1)
            : 2000;

        struct y1_input_event ev;
        if (y1_input_poll(&ev, wait_ms) > 0) {
            do {
                if ((ev.value == 1 || ev.value == 2) && ev.button != Y1_BTN_NONE) {
                    handle_button(ev.button);
                    dirty = true;
                }
            } while (y1_input_poll(&ev, 0) > 0);
        }

        tnow = cfg_now_ms();
        bool animating = false;
        if (cfg_item_pill_update_all(tnow)) animating = true;
        if (cfg_sw_update_all(tnow)) animating = true;
        if (fabsf(g_trans_x) > 0.5f) {
            g_trans_x *= 0.62f;
            if (fabsf(g_trans_x) < 1.0f) g_trans_x = 0.0f;
            animating = true;
        }
        float hdiff = (float)g_home_row_scroll - g_home_row_scroll_anim;
        if (fabsf(hdiff) > 0.015f) {
            g_home_row_scroll_anim += hdiff * 0.38f;
            animating = true;
        } else {
            g_home_row_scroll_anim = (float)g_home_row_scroll;
        }
        float bdiff = target_blur - g_bg_blur_anim;
        if (fabsf(bdiff) > 0.02f) {
            g_bg_blur_anim += bdiff * 0.30f;
            animating = true;
        } else {
            g_bg_blur_anim = target_blur;
        }
        float wdiff = (float)g_wp_scroll - g_wp_scroll_anim;
        if (fabsf(wdiff) > 0.015f) {
            g_wp_scroll_anim += wdiff * 0.38f;
            animating = true;
        } else {
            g_wp_scroll_anim = (float)g_wp_scroll;
        }
        float idiff = (float)g_item_scroll - g_item_scroll_anim;
        if (fabsf(idiff) > 0.015f) {
            g_item_scroll_anim += idiff * 0.38f;
            animating = true;
        } else {
            g_item_scroll_anim = (float)g_item_scroll;
        }

        if (dirty || animating) {
            uint64_t rf0 = cfg_now_ms();
            render_frame();
            last_render_dur_ms = cfg_now_ms() - rf0;
            clock_gettime(CLOCK_MONOTONIC, &last_refresh);
            dirty = false;
        } else {
            last_render_dur_ms = 0;
        }
    }

    stop_serial_terminal();
    save_settings_conf();
    gfx_shutdown();
    y1_input_close();
    y1_fb_close();
    return 0;
}
