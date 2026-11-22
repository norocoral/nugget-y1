#include "ui_gfx.h"
#include "cfg_registry.h"
#include "hangul_ime.h"
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
#include <sys/sysmacros.h>
#include <linux/rtc.h>
#include <linux/fb.h>

#define SETTINGS_CONF_PATH "/mnt/sdcard/.system/configapp/settings.conf"
#define SETTINGS_CONF_FALLBACK "/.system/configapp/settings.conf"

#define TR(en, ko) (g_cfg.lang == 1 ? (ko) : (en))

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
    y1_fb_set_eco_mode(g_cfg.eco_mode != 0);
    y1_input_set_touch_scanning(g_cfg.touch_scan != 0);
    y1_cpu_set_mode(g_cfg.cpu_mode);
    y1_cpu_set_dual_core(g_cfg.dual_core != 0);
    y1_usb_set_ipod_mode(g_cfg.usb_ipod_mode != 0);
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
    show_toast(TR("Date & time saved to RTC", "날짜 및 시간이 RTC에 저장됨"), 1400);
}

/* =========================================================================
 * Setting Callbacks (Extendible Registry)
 * ========================================================================= */
static void info_battery(char *buf, size_t len)
{
    snprintf(buf, len, "%d%% (%d mV, %s)",
             y1_battery_percent(), y1_battery_voltage_mv(),
             y1_battery_is_charging() ? TR("Charging", "충전 중") : TR("Discharging", "방전 중"));
}

static void info_current(char *buf, size_t len)
{
    snprintf(buf, len, "%d mA (USB: %s)",
             y1_battery_current_ma(),
             y1_power_usb_online() ? TR("Online", "연결됨") : TR("Offline", "연결 안 됨"));
}

static void info_cpu(char *buf, size_t len)
{
    snprintf(buf, len, "%d MHz (%s)",
             y1_cpu_get_freq_mhz(),
             y1_cpu_is_dual_core() ? TR("Dual-Core", "듀얼 코어") : TR("Single-Core", "싱글 코어"));
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
        snprintf(buf, len, "%s", TR("Not mounted", "마운트 안 됨"));
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
    g_cfg.eco_mode = v ? 1 : 0;
    y1_fb_set_eco_mode(g_cfg.eco_mode != 0);
    save_settings_conf();
}

static int get_touch_scan(void) { return g_cfg.touch_scan; }
static void set_touch_scan(int v)
{
    g_cfg.touch_scan = v ? 1 : 0;
    y1_input_set_touch_scanning(g_cfg.touch_scan != 0);
    save_settings_conf();
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
    show_toast(TR("Haptic pulse sent", "진동 테스트 실행"), 1000);
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

static float cfg_sw_get(int page, int idx, bool on, uint64_t tnow)
{
    float want = on ? 1.0f : 0.0f;
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
                 TR("Clear Wallpaper", "배경화면 지우기"));
        g_wp_items[g_wp_count].path[0] = '\0';
        g_wp_items[g_wp_count].is_dir = false;
        g_wp_items[g_wp_count].is_clear = true;
        g_wp_count++;
    } else if (g_picker_is_font && gfx_has_custom_font() && g_wp_count < CFG_WP_MAX) {
        snprintf(g_wp_items[g_wp_count].name, sizeof(g_wp_items[0].name), "%s",
                 TR("Reset Default Font", "기본 글꼴로 복원"));
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

static int get_cpu_mode(void) { return g_cfg.cpu_mode; }
static void set_cpu_mode(int v)
{
    g_cfg.cpu_mode = v;
    y1_cpu_set_mode(v);
    save_settings_conf();
}

static int get_dual_core(void) { return g_cfg.dual_core; }
static void set_dual_core(int v)
{
    g_cfg.dual_core = v ? 1 : 0;
    y1_cpu_set_dual_core(g_cfg.dual_core != 0);
    save_settings_conf();
}

static int get_screen_clamp(void) { return g_cfg.screen_off_clamp; }
static void set_screen_clamp(int v)
{
    g_cfg.screen_off_clamp = v ? 1 : 0;
    y1_cpu_set_screen_off_clamp(g_cfg.screen_off_clamp != 0);
    save_settings_conf();
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
        snprintf(buf, len, "%04d-%02d-%02d %s %02d:%02d",
                 dt_year, dt_month, dt_day,
                 dt_ampm ? "오후" : "오전", dt_hour12, dt_minute);
    } else {
        snprintf(buf, len, "%04d-%02d-%02d %02d:%02d %s",
                 dt_year, dt_month, dt_day,
                 dt_hour12, dt_minute, dt_ampm ? "PM" : "AM");
    }
}

static int get_usb_ipod(void) { return g_cfg.usb_ipod_mode; }
static void set_usb_ipod(int v)
{
    g_cfg.usb_ipod_mode = v ? 1 : 0;
    y1_usb_set_ipod_mode(g_cfg.usb_ipod_mode != 0);
    save_settings_conf();
}

static int get_usb_perf(void) { return g_cfg.usb_perf_tune; }
static void set_usb_perf(int v)
{
    g_cfg.usb_perf_tune = v ? 1 : 0;
    y1_usb_tune_performance(g_cfg.usb_perf_tune != 0);
    save_settings_conf();
}

static int get_usb_ums(void) { return y1_usb_is_storage_attached() ? 1 : 0; }
static void set_usb_ums(int v)
{
    if (v) {
        if (y1_usb_attach_mass_storage())
            show_toast(TR("USB storage attached", "USB 저장장치 연결됨"), 1400);
        else
            show_toast(TR("USB attach failed", "USB 연결 실패"), 1400);
    } else {
        y1_usb_detach_mass_storage();
        show_toast(TR("USB storage detached", "USB 저장장치 해제됨"), 1400);
    }
}

static void act_sync_disks(void)
{
    sync();
    show_toast(TR("Filesystems synced", "파일시스템 동기화 완료"), 1200);
}

static void act_drop_caches(void)
{
    sync();
    y1_sysfs_write("/proc/sys/vm/drop_caches", "3\n");
    show_toast(TR("Page cache cleared", "페이지 캐시 정리 완료"), 1200);
}

static int get_lang(void) { return g_cfg.lang; }
static void set_lang(int v)
{
    g_cfg.lang = (v == 1) ? 1 : 0;
    save_settings_conf();
    show_toast(g_cfg.lang == 1 ? "언어: 한국어" : "Language: English", 1000);
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
    show_toast(g_cfg.screenshot ? TR("Screenshot ON (MENU + Play)", "스크린샷 켜짐 (MENU + 재생)")
                                : TR("Screenshot OFF", "스크린샷 꺼짐"), 1400);
}

static void act_clear_log(void)
{
    unlink(Y1_DEBUG_LOG_PATH);
    show_toast(TR("Debug log cleared", "디버그 로그 삭제됨"), 1200);
}

static int get_bt_enabled(void) { return y1_bt_is_enabled() ? 1 : 0; }
static void set_bt_enabled(int v)
{
    y1_bt_set_enabled(v != 0);
    show_toast(v ? TR("Bluetooth enabled", "블루투스 켜짐") : TR("Bluetooth disabled", "블루투스 꺼짐"), 1200);
}

static int get_bt_receiver(void) { return y1_bt_is_receiver_mode() ? 1 : 0; }
static void set_bt_receiver(int v)
{
    y1_bt_set_receiver_mode(v != 0);
    show_toast(v ? TR("BT Receiver Mode ON", "블루투스 수신 모드 켜짐")
                 : TR("BT Receiver Mode OFF", "블루투스 수신 모드 꺼짐"), 1200);
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
    show_toast(TR("Scanning for Bluetooth devices...", "블루투스 기기 검색 중..."), 1500);
}

static void act_bt_disconnect(void)
{
    y1_bt_disconnect();
    show_toast(TR("Disconnected", "연결 해제됨"), 1200);
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
    if (!f) return 0;
    char buf[64] = "";
    if (fgets(buf, sizeof(buf), f)) {
        for (char *p = buf; *p; p++) {
            if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') { *p = '\0'; break; }
        }
    }
    fclose(f);
    if (strcasecmp(buf, "meui") == 0) return 1;
    if (strcasecmp(buf, "configapp") == 0) return 2;
    return 0;
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
    show_toast(v == 1 ? TR("Default: MeUI", "기본 앱: MeUI")
                      : (v == 2 ? TR("Default: ConfigApp", "기본 앱: ConfigApp")
                                : TR("Default: Rockbox", "기본 앱: Rockbox")), 1200);
}

static int get_boot_configapp(void)
{
    const char *paths[] = { "/mnt/sdcard/.system/system.cfg", "/mnt/sdcard/.rockbox/config.cfg", NULL };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        char line[128], val[16];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "bootloader menu: %15s", val) == 1) {
                fclose(f);
                return (strcmp(val, "on") == 0) ? 1 : 0;
            }
        }
        fclose(f);
    }
    return 0;
}

static void set_boot_configapp(int v)
{
    mkdir("/mnt/sdcard/.system", 0777);
    const char *path = "/mnt/sdcard/.system/system.cfg";
    char lines[64][128];
    int n = 0;
    bool replaced = false;
    FILE *f = fopen(path, "r");
    if (f) {
        while (n < 63 && fgets(lines[n], sizeof(lines[n]), f)) {
            if (strncmp(lines[n], "bootloader menu:", 16) == 0) {
                snprintf(lines[n], sizeof(lines[n]), "bootloader menu: %s\n", v ? "on" : "off");
                replaced = true;
            }
            n++;
        }
        fclose(f);
    }
    if (!replaced && n < 64)
        snprintf(lines[n++], sizeof(lines[n]), "bootloader menu: %s\n", v ? "on" : "off");
    f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < n; i++) fputs(lines[i], f);
        fclose(f);
        sync();
    }
    show_toast(v ? TR("Always boot to ConfigApp: ON", "부팅 시 설정 앱 자동 실행: 켜짐")
                 : TR("Always boot to ConfigApp: OFF", "부팅 시 설정 앱 자동 실행: 꺼짐"), 1400);
}

static void act_reset_settings(void)
{
    unlink(SETTINGS_CONF_PATH);
    unlink(SETTINGS_CONF_FALLBACK);
    g_cfg.lang = 0;
    g_cfg.dark_mode = 0;
    g_cfg.brightness_pct = 65;
    g_cfg.eco_mode = 0;
    g_cfg.touch_scan = 1;
    g_cfg.vibrate_ms = 20;
    g_cfg.cpu_mode = Y1_CPU_MODE_DYNAMIC;
    g_cfg.dual_core = 1;
    g_cfg.screen_off_clamp = 1;
    g_cfg.usb_ipod_mode = 0;
    g_cfg.usb_perf_tune = 1;
    g_cfg.debug_log = 0;
    g_cfg.bt_codec = Y1_BT_CODEC_SBC;
    g_cfg.screenshot = 0;
    apply_all_settings();
    save_settings_conf();
    show_toast(TR("Settings reset to defaults", "설정이 기본값으로 초기화됨"), 1400);
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
        show_toast(TR("Serial Console started (ttyGS0/1)", "시리얼 콘솔 시작됨 (ttyGS0/1)"), 1400);
    } else if (!v && g_serial_active) {
        stop_serial_terminal();
        show_toast(TR("Serial Console stopped", "시리얼 콘솔 중지됨"), 1200);
    }
}

static void info_serial_ports(char *buf, size_t len)
{
    snprintf(buf, len, "%s",
             g_serial_active ? TR("Active (/dev/ttyGS0, ttyGS1)", "활성 (/dev/ttyGS0, ttyGS1)")
                             : TR("Idle", "대기 중"));
}

/* =========================================================================
 * Debug Info Collector (ConfigApp Native UI)
 * ========================================================================= */
#define MAX_DBG_LINES 32
static char g_dbg_lines[MAX_DBG_LINES][96];
static int  g_dbg_count = 0;

static void refresh_debug_info(void)
{
    g_dbg_count = 0;
    struct utsname u;
    memset(&u, 0, sizeof(u));
    uname(&u);
    snprintf(g_dbg_lines[g_dbg_count++], 96, "Kernel: %s (%s)", u.release, u.machine);

    char tmp[96] = "?";
    if (y1_sysfs_read("/proc/lcm", tmp, sizeof(tmp)))
        snprintf(g_dbg_lines[g_dbg_count++], 96, "LCM: %s", tmp);

    snprintf(g_dbg_lines[g_dbg_count++], 96, "FB0: %dx%d @ %dbpp (stride=%d)",
             y1_fb_width(), y1_fb_height(), y1_fb_bpp(), y1_fb_stride());
    snprintf(g_dbg_lines[g_dbg_count++], 96, "CPU: %d MHz (%s)",
             y1_cpu_get_freq_mhz(), y1_cpu_is_dual_core() ? "Dual" : "Single");
    snprintf(g_dbg_lines[g_dbg_count++], 96, "Battery: %d%% %dmV %dmA (%s)",
             y1_battery_percent(), y1_battery_voltage_mv(), y1_battery_current_ma(),
             y1_battery_is_charging() ? "Chg" : "Dischg");

    char mem[96];
    info_memory(mem, sizeof(mem));
    snprintf(g_dbg_lines[g_dbg_count++], 96, "RAM: %s", mem);

    char sd[96];
    info_storage(sd, sizeof(sd));
    snprintf(g_dbg_lines[g_dbg_count++], 96, "SD: %s", sd);

    snprintf(g_dbg_lines[g_dbg_count++], 96, "Dev: fb0=%d eac=%d fm=%d wmt=%d bt=%d",
             access("/dev/graphics/fb0", F_OK) == 0,
             access("/dev/eac", F_OK) == 0,
             access("/dev/fm", F_OK) == 0,
             access("/dev/stpwmt", F_OK) == 0,
             access("/dev/stpbt", F_OK) == 0);

    snprintf(g_dbg_lines[g_dbg_count++], 96, "Bin: rb=%d meui=%d liby1=%d",
             access("/mnt/sdcard/.rockbox/rockbox", X_OK) == 0,
             access("/mnt/sdcard/.meui/meui", X_OK) == 0,
             access("/mnt/sdcard/.system/lib/liby1.so", F_OK) == 0);

    snprintf(g_dbg_lines[g_dbg_count++], 96, "iTunesDB: %s  PlayCounts: %s",
             access("/mnt/sdcard/iPod_Control/iTunes/iTunesDB", F_OK) == 0 ? "Yes" : "No",
             access("/mnt/sdcard/iPod_Control/iTunes/Play Counts", F_OK) == 0 ? "Yes" : "No");
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
    FILE *f = fopen(path, "w");
    if (!f) {
        show_toast(TR("SD not writable", "SD 카드에 쓸 수 없음"), 1400);
        return;
    }
    fprintf(f, "=== Nugget Y1 Debug Info ===\n");
    for (int i = 0; i < g_dbg_count; i++)
        fprintf(f, "%s\n", g_dbg_lines[i]);
    fclose(f);
    show_toast(TR("Saved debuginfo to SD", "디버그 정보를 SD에 저장함"), 1500);
}

/* =========================================================================
 * Declarative Extendible Settings Tables
 * ========================================================================= */
static const char *cpu_mode_choices[] = {
    "Dynamic", "Powersave 390M", "Balanced 1.0G", "Perf 1.2G", "Turbo 1.3G"
};
static const char *cpu_mode_choices_ko[] = {
    "자동 (Dynamic)", "절전 390M", "균형 1.0G", "고성능 1.2G", "터보 1.3G"
};

static const char *bt_codec_choices[] = { "SBC", "SBC XQ", "AAC" };
static const char *default_player_choices[] = { "Rockbox", "MeUI", "ConfigApp" };
static const char *lang_choices[] = { "English", "한국어" };
static const char *ampm_choices[] = { "AM", "PM" };
static const char *ampm_choices_ko[] = { "오전", "오후" };

static struct cfg_item status_items[] = {
    { NULL, "Battery",    "배터리",    "Charge level & voltage", "충전 상태 및 전압",     "battery-charging", CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_battery, NULL },
    { NULL, "Power Draw", "전류 / USB","Current & USB power",    "소비 전류 및 USB 전원", "activity",         CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_current, NULL },
    { NULL, "Processor",  "프로세서",  "Live clock & core count","실시간 클럭 및 코어",   "cpu",              CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_cpu,     NULL },
    { NULL, "Memory",     "메모리",    "System RAM usage",       "시스템 RAM 여유 공간",  "sliders",          CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_memory,  NULL },
    { NULL, "SD Storage", "SD 저장소", "MicroSD free space",     "SD 카드 여유 공간",     "hard-drive",       CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_storage, NULL },
    { NULL, "Kernel",     "커널",      "Linux kernel version",   "리눅스 커널 버전",      "info",             CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_kernel,  NULL },
    { NULL, "Uptime",     "가동 시간", "Time since boot",        "부팅 후 경과 시간",     "refresh-cw",       CFG_TYPE_INFO, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, info_uptime,  NULL },
};

static struct cfg_item display_items[] = {
    { "dark_mode",      "Dark Mode",        "다크 모드",       "Dark MeUI color theme",     "어두운 MeUI 색상 테마",  "sliders", CFG_TYPE_TOGGLE, 0, 1,   1, NULL, NULL, NULL, 0, get_dark_mode,  set_dark_mode,  NULL, NULL },
    { NULL,             "Wallpaper",        "배경화면",        "PNG / JPG background image","PNG / JPG 배경 이미지",  "image",   CFG_TYPE_ACTION, 0, 0,   0, NULL, NULL, NULL, 0, NULL,           NULL,           NULL, act_open_wallpaper },
    { NULL,             "Font",             "글꼴",            "Custom TTF / OTF UI font",  "사용자 TTF / OTF 글꼴",  "sliders", CFG_TYPE_ACTION, 0, 0,   0, NULL, NULL, NULL, 0, NULL,           NULL,           NULL, act_open_font },
    { "brightness_pct", "Brightness",       "화면 밝기",       "Backlight intensity",       "백라이트 밝기 조절",     "sun",     CFG_TYPE_INT,    5, 100, 5, "%",  NULL, NULL, 0, get_brightness, set_brightness, NULL, NULL },
    { "eco_mode",       "Display Eco Mode", "디스플레이 에코", "Panel low-power mode",      "패널 저전력 모드",       "sliders", CFG_TYPE_TOGGLE, 0, 1,   1, NULL, NULL, NULL, 0, get_eco_mode,   set_eco_mode,   NULL, NULL },
    { "touch_scan",     "Touch Wheel",      "터치 휠",         "Capacitive wheel scanning", "정전식 휠 스캔 활성화",  "activity",CFG_TYPE_TOGGLE, 0, 1,   1, NULL, NULL, NULL, 0, get_touch_scan, set_touch_scan, NULL, NULL },
    { "vibrate_ms",     "Haptic Duration",  "진동 길이",       "Motor pulse length",        "진동 모터 작동 시간",    "vibrate", CFG_TYPE_INT,    5, 100, 5, "ms", NULL, NULL, 0, get_vibrate_ms, set_vibrate_ms, NULL, NULL },
    { NULL,             "Test Haptics",     "진동 테스트",     "Trigger vibration pulse",   "설정한 길이로 진동 실행","vibrate", CFG_TYPE_ACTION, 0, 0,   0, NULL, NULL, NULL, 0, NULL,           NULL,           NULL, act_vibrate_test },
};

static struct cfg_item cpu_items[] = {
    { "cpu_mode",         "CPU Governor",     "CPU 거버너",      "Frequency scaling policy", "주파수 스케일링 정책",   "cpu",     CFG_TYPE_CHOICE, 0, 4, 1, NULL, cpu_mode_choices, cpu_mode_choices_ko, 5, get_cpu_mode,     set_cpu_mode,     NULL, NULL },
    { "dual_core",        "Dual-Core",        "듀얼 코어",       "Enable second Cortex-A7",  "보조 코어(CPU1) 활성화", "cpu",     CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,             NULL,                0, get_dual_core,    set_dual_core,    NULL, NULL },
    { "screen_off_clamp", "Screen-Off Clamp", "화면 꺼짐 절전",  "Clamp to 390MHz off-screen","화면 꺼짐 시 390MHz 고정","sliders",CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,             NULL,                0, get_screen_clamp, set_screen_clamp, NULL, NULL },
    { NULL,               "Live Clock",       "현재 클럭",       "Current CPU frequency",    "실시간 CPU 주파수",      "activity",CFG_TYPE_INFO,   0, 0, 0, NULL, NULL,             NULL,                0, NULL,             NULL,             info_cpu, NULL },
};

static struct cfg_item datetime_items[] = {
    { NULL, "Preview",      "현재 설정", "Pending date & time",   "적용할 날짜 및 시간", "clock", CFG_TYPE_INFO,   0,    0,    0, NULL, NULL,         NULL,            0, NULL,           NULL,           info_dt_preview, NULL },
    { NULL, "Year",         "연도",      "Calendar year",         "연도 설정",           "clock", CFG_TYPE_INT,    2020, 2040, 1, NULL, NULL,         NULL,            0, get_dt_year,    set_dt_year,    NULL, NULL },
    { NULL, "Month",        "월",        "Month (1 - 12)",        "월 (1 - 12)",         "clock", CFG_TYPE_INT,    1,    12,   1, NULL, NULL,         NULL,            0, get_dt_month,   set_dt_month,   NULL, NULL },
    { NULL, "Day",          "일",        "Day of month",          "일 설정",             "clock", CFG_TYPE_INT,    1,    31,   1, NULL, NULL,         NULL,            0, get_dt_day,     set_dt_day,     NULL, NULL },
    { NULL, "AM / PM",      "오전 / 오후","12-hour period",       "오전 / 오후 선택",    "clock", CFG_TYPE_CHOICE, 0,    1,    1, NULL, ampm_choices, ampm_choices_ko, 2, get_dt_ampm,    set_dt_ampm,    NULL, NULL },
    { NULL, "Hour",         "시",        "Hour (1 - 12)",         "시 (1 - 12)",         "clock", CFG_TYPE_INT,    1,    12,   1, NULL, NULL,         NULL,            0, get_dt_hour12,  set_dt_hour12,  NULL, NULL },
    { NULL, "Minute",       "분",        "Minute (0 - 59)",       "분 (0 - 59)",         "clock", CFG_TYPE_INT,    0,    59,   1, NULL, NULL,         NULL,            0, get_dt_minute,  set_dt_minute,  NULL, NULL },
    { NULL, "Apply to RTC", "RTC에 저장","Save to hardware clock","하드웨어 시계에 저장","check", CFG_TYPE_ACTION, 0,    0,    0, NULL, NULL,         NULL,            0, NULL,           NULL,           NULL, apply_datetime_to_rtc },
};

static struct cfg_item bt_items[] = {
    { NULL,       "Bluetooth Radio", "블루투스 전원",  "Power MT6572 CONNSYS radio", "MT6572 무선 칩셋 전원",   "bluetooth",  CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,             NULL,             0, get_bt_enabled,  set_bt_enabled,  NULL, NULL },
    { NULL,       "Status",          "연결 상태",      "Current connection state",   "현재 블루투스 상태",      "info",       CFG_TYPE_INFO,   0, 0, 0, NULL, NULL,             NULL,             0, NULL,            NULL,            info_bt_status, NULL },
    { NULL,       "Receiver Mode",   "수신기 모드",    "Act as BT audio sink",       "블루투스 스피커/DAC 모드","radio",      CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,             NULL,             0, get_bt_receiver, set_bt_receiver, NULL, NULL },
    { "bt_codec", "A2DP Codec",      "A2DP 코덱",      "Preferred audio codec",      "선호 오디오 코덱",        "sliders",    CFG_TYPE_CHOICE, 0, 2, 1, NULL, bt_codec_choices, bt_codec_choices, 3, get_bt_codec,    set_bt_codec,    NULL, NULL },
    { NULL,       "Scan Devices",    "기기 검색",      "Discover nearby audio sinks","주변 블루투스 기기 찾기", "refresh-cw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,             NULL,             0, NULL,            NULL,            NULL, act_bt_scan },
    { NULL,       "Disconnect",      "연결 해제",      "Close active BT link",       "현재 기기 연결 끊기",     "power",      CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,             NULL,             0, NULL,            NULL,            NULL, act_bt_disconnect },
};

static int get_wifi_enable(void) { return y1_wifi_is_enabled() ? 1 : 0; }
static void set_wifi_enable(int v)
{
    y1_wifi_init();
    y1_wifi_set_enabled(v != 0);
    show_toast(v ? TR("Wi-Fi enabled", "Wi-Fi 켜짐") : TR("Wi-Fi disabled", "Wi-Fi 꺼짐"), 1200);
}
static void info_wifi_status(char *buf, size_t len)
{
    snprintf(buf, len, "%s", y1_wifi_get_status_str());
}
static void info_wifi_ip(char *buf, size_t len)
{
    char ip[32] = "";
    y1_wifi_get_ip(ip, sizeof(ip));
    snprintf(buf, len, "%s", ip[0] ? ip : TR("None", "없음"));
}
static void act_wifi_scan(void)
{
    y1_wifi_init();
    if (!y1_wifi_is_enabled())
        y1_wifi_set_enabled(true);
    y1_wifi_start_scan();
    show_toast(TR("Scanning Wi-Fi...", "Wi-Fi 검색 중..."), 1500);
}
static void act_wifi_disconnect(void)
{
    y1_wifi_disconnect();
    show_toast(TR("Wi-Fi disconnected", "Wi-Fi 연결 해제됨"), 1200);
}

static struct cfg_item wifi_items[] = {
    { NULL, "Wi-Fi Power",  "Wi-Fi 전원",  "Enable MT6572 Wi-Fi radio", "MT6572 Wi-Fi 켜기/끄기",   "wifi",       CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_wifi_enable, set_wifi_enable, NULL, NULL },
    { NULL, "Status",       "연결 상태",   "Current Wi-Fi state",       "현재 Wi-Fi 상태",          "info",       CFG_TYPE_INFO,   0, 0, 0, NULL, NULL, NULL, 0, NULL,            NULL,            info_wifi_status, NULL },
    { NULL, "IP Address",   "IP 주소",     "Assigned DHCP IPv4",        "할당된 IPv4 주소",         "globe",      CFG_TYPE_INFO,   0, 0, 0, NULL, NULL, NULL, 0, NULL,            NULL,            info_wifi_ip, NULL },
    { NULL, "Scan Networks","네트워크 검색","Find nearby access points","주변 Wi-Fi 공유기 찾기",   "refresh-cw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL,            NULL,            NULL, act_wifi_scan },
    { NULL, "Disconnect",   "연결 해제",   "Leave current network",     "현재 네트워크 연결 끊기",  "power",      CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL,            NULL,            NULL, act_wifi_disconnect },
};

static struct cfg_item usb_items[] = {
    { "usb_ipod_mode", "iPod USB Mode",    "iPod USB 모드",  "Spoof iPod 5.5G over USB",  "USB 연결 시 iPod 인식",   "usb",        CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_usb_ipod, set_usb_ipod, NULL, NULL },
    { "usb_perf_tune", "USB Performance",  "USB 성능 향상",  "Boost USB gadget throughput","USB 전송 속도 최적화",    "sliders",    CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_usb_perf, set_usb_perf, NULL, NULL },
    { NULL,            "USB Mass Storage", "USB 대용량 저장","Mount SD card to PC",       "PC에 SD 카드 마운트",     "hard-drive", CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_usb_ums,  set_usb_ums,  NULL, NULL },
    { NULL,            "Sync Storage",     "저장소 동기화",  "Flush SD card write cache", "쓰기 캐시 즉시 저장",     "refresh-cw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL,         NULL,         NULL, act_sync_disks },
    { NULL,            "Drop Caches",      "캐시 비우기",    "Free Linux page cache",     "리눅스 페이지 캐시 해제", "trash-2",    CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL,         NULL,         NULL, act_drop_caches },
};

static struct cfg_item power_items[] = {
    { "lang",       "Language",      "언어 (Language)",  "Switch English / 한국어",        "영어 / 한국어 전환",           "globe",      CFG_TYPE_CHOICE, 0, 1, 1, NULL, lang_choices, lang_choices, 2, get_lang,       set_lang,       NULL, NULL },
    { "screenshot", "Screenshot",    "스크린샷",         "MENU + Play saves to /Screenshots", "MENU + 재생으로 /Screenshots 저장", "camera",     CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,         NULL,         0, get_screenshot, set_screenshot, NULL, NULL },
    { "debug_log",  "Y1 Debug Log",  "Y1 디버그 로그",   "Write .system/y1_debug.txt",     ".system/y1_debug.txt 기록",    "info",       CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,         NULL,         0, get_debug_log,  set_debug_log,  NULL, NULL },
    { NULL,        "Clear Log",     "로그 삭제",        "Delete y1_debug.txt",        "디버그 로그 파일 삭제",  "trash-2",    CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_clear_log },
    { NULL,        "Reset Settings","설정 초기화",      "Restore default settings",   "모든 설정 기본값 복원",  "rotate-ccw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_reset_settings },
    { NULL,        "Exit ConfigApp","설정 앱 종료",     "Return to caller",           "이전 화면으로 돌아가기", "arrow-left", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_return_bootmenu },
    { NULL,        "Reboot Device", "기기 재부팅",      "Restart the Y1",             "Y1 다시 시작",           "rotate-ccw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_reboot },
    { NULL,        "Power Off",     "전원 끄기",        "Shut down safely",           "안전하게 전원 끄기",     "power",      CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,         NULL,         0, NULL,          NULL,          NULL, act_poweroff },
};

static struct cfg_item launch_items[] = {
    { NULL, "Default Launch App",     "기본 실행 앱",          "App started on boot",           "부팅 시 자동 실행할 앱",       "play",       CFG_TYPE_CHOICE, 0, 2, 1, NULL, default_player_choices, default_player_choices, 3, get_default_player, set_default_player, NULL, NULL },
    { NULL, "Always Boot ConfigApp",  "부팅 시 설정 앱 실행",  "Open ConfigApp on every boot",  "매 부팅마다 설정 앱 먼저 실행","settings",   CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL,                   NULL,                   0, get_boot_configapp, set_boot_configapp, NULL, NULL },
    { NULL, "Rescan Software",        "목록 새로고침",         "Scan hidden folders on SD",     "SD 숨겨진 앱 폴더 다시 검색",  "refresh-cw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL,                   NULL,                   0, NULL,               NULL,               NULL, scan_launch_apps },
};

static struct cfg_item serial_items[] = {
    { NULL, "Serial Console", "시리얼 콘솔",   "Enable dual USB ACM shell", "듀얼 USB ACM 셸 활성화", "terminal", CFG_TYPE_TOGGLE, 0, 1, 1, NULL, NULL, NULL, 0, get_serial_active, set_serial_active, NULL, NULL },
    { NULL, "USB ACM Ports",  "USB ACM 포트",  "ttyGS0 & ttyGS1 status",    "ttyGS0 / ttyGS1 상태",   "info",     CFG_TYPE_INFO,   0, 0, 0, NULL, NULL, NULL, 0, NULL,              NULL,              info_serial_ports, NULL },
};

static struct cfg_item debug_items[] = {
    { NULL, "Save Debug Info", "디버그 정보 저장", "Write debuginfo_*.txt to SD", "SD에 debuginfo_*.txt 저장", "hard-drive", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, NULL, act_save_debug_info },
};

static struct cfg_item exit_power_items[] = {
    { NULL, "Exit ConfigApp", "설정 앱 종료", "Return to caller", "이전 화면으로 돌아가기", "arrow-left", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, NULL, act_return_bootmenu },
    { NULL, "Reboot Device",  "기기 재부팅",  "Restart the Y1",   "Y1 다시 시작",           "rotate-ccw", CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, NULL, act_reboot },
    { NULL, "Shut Down",      "전원 끄기",    "Power off safely", "안전하게 전원 끄기",     "power",      CFG_TYPE_ACTION, 0, 0, 0, NULL, NULL, NULL, 0, NULL, NULL, NULL, act_poweroff },
};

static struct cfg_page g_pages[] = {
    { "launch",   "Launch",            "실행",             "Start installed software",   "설치된 소프트웨어 실행",     "play",      0xFF10B981U, launch_items,   (int)(sizeof(launch_items) / sizeof(launch_items[0])),     false, false, true,  false },
    { "status",   "System Status",     "시스템 상태",      "Battery, CPU, RAM & disk",   "배터리, CPU, RAM 및 저장소", "activity",  0xFF16A34AU, status_items,   (int)(sizeof(status_items) / sizeof(status_items[0])),     false, false, false, false },
    { "display",  "Display & Haptics", "화면 및 진동",     "Brightness, wheel & motor",  "밝기, 휠 및 진동 설정",      "sun",       0xFFD97706U, display_items,  (int)(sizeof(display_items) / sizeof(display_items[0])),   false, false, false, false },
    { "cpu",      "CPU & Power",       "CPU 및 전력",      "Governor & core hotplug",    "거버너 및 코어 제어",        "cpu",       0xFF2563EBU, cpu_items,      (int)(sizeof(cpu_items) / sizeof(cpu_items[0])),           false, false, false, false },
    { "datetime", "Date & Time",       "날짜 및 시간",     "12-hour clock & RTC",        "12시간제 시계 및 RTC",       "clock",     0xFF0EA5E9U, datetime_items, (int)(sizeof(datetime_items) / sizeof(datetime_items[0])), false, false, false, false },
    { "bt",       "Bluetooth",         "블루투스",         "Scan, pair & A2DP codec",    "검색, 연결 및 A2DP 코덱",    "bluetooth", 0xFF0284C7U, bt_items,       (int)(sizeof(bt_items) / sizeof(bt_items[0])),             true,  false, false, false },
    { "wifi",     "Wi-Fi",             "Wi-Fi",            "Scan & connect wireless",    "무선 네트워크 검색 및 연결", "wifi",      0xFF0284C7U, wifi_items,     (int)(sizeof(wifi_items) / sizeof(wifi_items[0])),           false, true,  false, false },
    { "usb",      "USB & Storage",     "USB 및 저장소",    "iPod mode & disk sync",      "iPod 모드 및 마운트",        "usb",       0xFF7C3AEDU, usb_items,      (int)(sizeof(usb_items) / sizeof(usb_items[0])),           false, false, false, false },
    { "serial",   "Serial Terminal",   "시리얼 터미널",    "USB ACM root console",       "USB 시리얼 콘솔",            "terminal",  0xFF0F766EU, serial_items,   (int)(sizeof(serial_items) / sizeof(serial_items[0])),     false, false, false, false },
    { "debug",    "Debug Info",        "디버그 정보",      "Hardware & system dump",     "하드웨어 및 시스템 정보",    "bug",       0xFF6366F1U, debug_items,    (int)(sizeof(debug_items) / sizeof(debug_items[0])),      false, false, false, true  },
    { "system",   "System & Power",    "시스템 및 전원",   "Language & debug logs",      "언어 및 디버그 로그",        "settings",  0xFF64748BU, power_items,      (int)(sizeof(power_items) / sizeof(power_items[0])),           false, false, false, false },
    { "exit",     "Exit / Shut Down",  "종료 / 전원 끄기", "Exit app or power off",      "앱 종료 또는 전원 끄기",     "power",     0xFFDC2626U, exit_power_items, (int)(sizeof(exit_power_items) / sizeof(exit_power_items[0])), false, false, false, false },
};

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
        snprintf(buf, len, "%s %02d:%02d", is_pm ? "오후" : "오전", h12, tm->tm_min);
    else
        snprintf(buf, len, "%02d:%02d %s", h12, tm->tm_min, is_pm ? "PM" : "AM");
}

static void draw_header(const char *title, const char *icon_name)
{
    gfx_set_text_shadow(false);
    gfx_fill_round_rect(8, 6, UI_W - 16, 48, 24, COL_SURFACE);

    char cpu_str[24];
    snprintf(cpu_str, sizeof(cpu_str), "%dM", g_cached_cpu_mhz);
    int cpu_w = gfx_text_width(cpu_str, 18.0f, UI_FONT_SEMIBOLD);

    char bat_str[16];
    int bat_pct = g_cached_bat_pct;
    bool bat_chg = g_cached_bat_chg;
    snprintf(bat_str, sizeof(bat_str), "%d%%", bat_pct);
    int bat_w = gfx_text_width(bat_str, 18.0f, UI_FONT_SEMIBOLD);

    char time_str[24];
    format_12h_clock(time_str, sizeof(time_str));
    int time_w = gfx_text_width(time_str, 18.5f, UI_FONT_SEMIBOLD);

    int rx = UI_W - 18 - time_w;
    gfx_draw_text(rx, 20, time_str, 18.5f, UI_FONT_SEMIBOLD, COL_TEXT);

    int bat_pill_w = 28 + bat_w + 10;
    rx -= bat_pill_w + 6;
    gfx_fill_round_rect(rx, 13, bat_pill_w, 34, 17, COL_SURFACE_HI);
    uint32_t bat_col = (!bat_chg && bat_pct <= 20) ? COL_DANGER : COL_TEXT;
    gfx_draw_battery_icon(rx + 6, 20, bat_pct, bat_chg, bat_col);
    gfx_draw_text(rx + 30, 19, bat_str, 18.0f, UI_FONT_SEMIBOLD, COL_TEXT);

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
    draw_header(TR("Settings", "시스템 설정"), "settings");

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
                float t = cfg_sw_get(g_active_page, idx, on != 0, cfg_now_ms());
                int pw = 58, ph = 34;
                int px = x + w - 16 - pw;
                int py = y + (row_h - ph) / 2;
                gfx_fill_round_rect(px, py, pw, ph, 17, sel ? COL_SURFACE : COL_SURFACE_HI);
                int tx = px + (int)roundf(t * (float)(pw - 34));
                uint32_t thumb_col = cfg_lerp_argb(COL_WARN, COL_SUCCESS, t);
                gfx_fill_round_rect(tx, py, 34, 34, 14, thumb_col);
                gfx_draw_icon(t >= 0.5f ? "check" : "x", tx + 7, py + 7, 20, COL_SWITCH_ICON);
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
            const char *badge = TR("Launch", "실행");
            int bw = gfx_text_width(badge, 19.0f, UI_FONT_SEMIBOLD) + 26;
            gfx_fill_round_rect(x + w - 14 - bw, y + 15, bw, 38, 19, COL_ACCENT);
            gfx_draw_text(x + w - 14 - bw + 13, y + 21, badge, 19.0f, UI_FONT_SEMIBOLD, COL_ACCENT_TEXT);
        } else if (pg->is_debug_page) {
            int d_idx = idx - pg->num_items;
            gfx_draw_icon("bug", x + 14, y + 21, 24, COL_TEXT_DIM);
            gfx_draw_text_ellipsize(x + 48, y + 22, w - 64, g_dbg_lines[d_idx], 20.0f,
                                    UI_FONT_REGULAR, COL_TEXT);
        } else if (pg->is_wifi_page) {
            int w_idx = idx - pg->num_items;
            const struct y1_wifi_network *net = &g_wifi_nets[w_idx];
            gfx_draw_icon(net->secured ? "lock" : "wifi", x + 14, y + 20, 28, COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 7, 220, net->ssid, 24.0f,
                                    UI_FONT_SEMIBOLD, COL_TEXT);
            char sub[64];
            snprintf(sub, sizeof(sub), "%d dBm  •  %s  •  %s",
                     net->rssi,
                     net->secured ? "WPA" : TR("Open", "개방"),
                     net->connected ? TR("Connected", "연결됨")
                                    : (net->saved ? TR("Saved (Right: Forget)", "저장됨 (오른쪽: 삭제)")
                                                  : TR("Available", "사용 가능")));
            gfx_draw_text_ellipsize(x + 52, y + 38, 230, sub, 18.0f,
                                    UI_FONT_REGULAR, COL_TEXT_MUTED);

            const char *badge = net->connected ? TR("Disconnect", "연결 해제") : TR("Connect", "연결");
            int bw = gfx_text_width(badge, 18.5f, UI_FONT_SEMIBOLD) + 24;
            gfx_fill_round_rect(x + w - 14 - bw, y + 15, bw, 38, 19,
                                net->connected ? COL_WARN : COL_ACCENT);
            gfx_draw_text(x + w - 14 - bw + 12, y + 21, badge, 18.5f, UI_FONT_SEMIBOLD,
                          net->connected ? COL_TEXT : COL_ACCENT_TEXT);
        } else {
            int d_idx = idx - pg->num_items;
            const struct y1_bt_device *dev = &g_bt_devs[d_idx];
            const char *name = dev->name[0] ? dev->name : TR("Unknown Device", "알 수 없는 기기");
            gfx_draw_icon("bluetooth", x + 14, y + 20, 28, COL_TEXT);
            gfx_draw_text_ellipsize(x + 52, y + 7, 220, name, 24.0f,
                                    UI_FONT_SEMIBOLD, COL_TEXT);
            char sub[56];
            snprintf(sub, sizeof(sub), "%02X:%02X:%02X:%02X:%02X:%02X  •  %s",
                     dev->bdaddr[5], dev->bdaddr[4], dev->bdaddr[3],
                     dev->bdaddr[2], dev->bdaddr[1], dev->bdaddr[0],
                     dev->connected ? TR("Connected", "연결됨")
                                    : (dev->paired ? TR("Paired (Right: Forget)", "등록됨 (오른쪽: 삭제)")
                                                   : TR("Discovered", "검색됨")));
            gfx_draw_text_ellipsize(x + 52, y + 38, 230, sub, 18.0f,
                                    UI_FONT_REGULAR, COL_TEXT_MUTED);

            const char *badge = dev->connected ? TR("Disconnect", "연결 해제") : TR("Connect", "연결");
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
    snprintf(title, sizeof(title), "%s: %s", TR("Password", "비밀번호"), g_osk_ssid);
    gfx_draw_text_ellipsize(mx + 18, my + 12, mw - 36, title, 21.0f, UI_FONT_SEMIBOLD, COL_TEXT);

    int bx = mx + 16, by = my + 46, bw = mw - 32, bh = 44;
    gfx_fill_round_rect(bx, by, bw, bh, 22, COL_MODAL_HI);
    const char *disp = g_osk_len > 0 ? g_osk_buf : TR("Enter Wi-Fi password...", "Wi-Fi 비밀번호 입력...");
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
        show_toast(TR("Connecting...", "연결 중..."), 1500);
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
            show_toast(TR("Connecting...", "연결 중..."), 1500);
        } else {
            hime_input_key(&g_osk_ime, lbl, g_osk_buf, sizeof(g_osk_buf));
            g_osk_len = (int)strlen(g_osk_buf);
        }
    }
}

static void render_wp_picker(void)
{
    draw_header(g_picker_is_font ? TR("Font", "글꼴") : TR("Wallpaper", "배경화면"),
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
                    show_toast(TR("Default font restored", "기본 글꼴로 복원됨"), 1200);
                } else {
                    gfx_clear_wallpaper();
                    show_toast(TR("Wallpaper cleared", "배경화면 삭제됨"), 1200);
                }
                g_wp_open = false;
            } else if (we->is_dir) {
                cfg_scan_wp_dir(we->path);
            } else if (g_picker_is_font) {
                if (gfx_set_custom_font(we->path)) {
                    g_wp_open = false;
                    show_toast(TR("Font applied", "글꼴 적용됨"), 1400);
                } else {
                    show_toast(TR("Failed to load font", "글꼴 로드 실패"), 1400);
                }
            } else {
                show_toast(TR("Applying wallpaper...", "배경화면 적용 중..."), 1000);
                render_frame();
                if (gfx_set_wallpaper_from_image(we->path)) {
                    g_wp_open = false;
                    show_toast(TR("Wallpaper applied", "배경화면 적용됨"), 1400);
                } else {
                    show_toast(TR("Failed to load image", "이미지 로드 실패"), 1400);
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

            if (total_rows >= 8 && (g_abc_active || g_scroll_fast_streak >= 12)) {
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
            } else if (it->type == CFG_TYPE_CHOICE && it->get_int && it->set_int && it->num_choices > 0) {
                int c = it->get_int();
                int dir = (btn == Y1_BTN_LEFT) ? -1 : 1;
                c = (c + it->num_choices + dir) % it->num_choices;
                it->set_int(c);
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
                FILE *lf = fopen("/tmp/configapp_launch", "w");
                if (lf) {
                    fprintf(lf, "%s\n", g_launch_apps[l_idx].exec_path);
                    fclose(lf);
                }
                if (g_boot_mode) {
                    g_should_exit = true;
                } else if (strcmp(g_launch_apps[l_idx].exec_path, "__ANDROID__") == 0) {
                    show_toast(TR("Reboot to Android from Boot", "부팅 메뉴에서 Android 실행 가능"), 1500);
                } else {
                    show_toast(TR("Launching...", "실행 중..."), 1000);
                    g_should_exit = true;
                }
            }
        } else if (pg->is_wifi_page) {
            int w_idx = g_item_sel - pg->num_items;
            if (w_idx >= 0 && w_idx < g_wifi_net_count) {
                if (btn == Y1_BTN_RIGHT && g_wifi_nets[w_idx].saved) {
                    y1_wifi_forget(g_wifi_nets[w_idx].ssid);
                    show_toast(TR("Network forgotten", "네트워크 삭제됨"), 1200);
                } else if (btn == Y1_BTN_LEFT && g_wifi_nets[w_idx].secured) {
                    open_wifi_osk(g_wifi_nets[w_idx].ssid);
                } else if (btn == Y1_BTN_SELECT) {
                    if (g_wifi_nets[w_idx].connected) {
                        y1_wifi_disconnect();
                        show_toast(TR("Disconnecting...", "연결 해제 중..."), 1200);
                    } else if (!g_wifi_nets[w_idx].secured || g_wifi_nets[w_idx].saved) {
                        y1_wifi_connect(g_wifi_nets[w_idx].ssid, NULL);
                        show_toast(TR("Connecting...", "연결 중..."), 1500);
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
                    show_toast(TR("Device forgotten", "기기 등록 삭제됨"), 1200);
                } else if (btn == Y1_BTN_SELECT) {
                    if (g_bt_devs[d_idx].connected) {
                        y1_bt_disconnect();
                        show_toast(TR("Disconnecting...", "연결 해제 중..."), 1200);
                    } else {
                        y1_bt_connect_device(g_bt_devs[d_idx].bdaddr);
                        show_toast(TR("Connecting...", "연결 중..."), 1500);
                    }
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bootmenu") == 0)
            g_boot_mode = true;
    }

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
        if (tnow - last_hw_ms >= 1000) {
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
            : 250;

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

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long elapsed_ms =
            (long long)(now.tv_sec - last_refresh.tv_sec) * 1000LL +
            (long long)(now.tv_nsec - last_refresh.tv_nsec) / 1000000LL;
        if (dirty || animating || elapsed_ms >= 500) {
            uint64_t rf0 = cfg_now_ms();
            render_frame();
            last_render_dur_ms = cfg_now_ms() - rf0;
            last_refresh = now;
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
