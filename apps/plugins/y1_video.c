/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Innioasis Y1 Exclusive Video Player Plugin (y1_video.c)
 *
 * Features:
 * - Uses external library on SD card (/mnt/sdcard/.rockbox/lib/liby1video.so)
 * - Supports MP4, MKV, MOV (and M4V) containers
 * - Supports HD video (1080p 30fps) with dual-core NEON boost & frame-drop sync
 * - Supports synchronized stereo PCM audio playback
 * - Uses iPod Video control scheme:
 *     * Play/Pause toggles playback
 *     * Scroll wheel adjusts Volume by default (bottom volume bar OSD)
 *     * Center (Select) toggles Timeline OSD -> Diamond Scrubbing mode
 *       where Scroll wheel scrubs forward/backward through the video
 *     * Right/Left short press skips; Right/Left hold fast-forwards/rewinds
 *     * Long press Center opens in-video options menu
 *     * Menu saves 64-bit bookmark and exits
 * - Supports long videos (>2 hours) with 64-bit millisecond timestamps
 * - Keeps backlight on during playback
 * - Integrates with File Browser, Main Menu ("Videos"), and Settings ("Videos")
 * - Builds and browses Video Database (Thumbnail, Title, TV Show, Creator, Year)
 * - Uses standard Rockbox synclists (gui_synclist) for all list UIs
 ****************************************************************************/
#include "plugin.h"
#include "y1_video_lib.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#define Y1_VIDEO_LIB_PATH    "/mnt/sdcard/.rockbox/lib/liby1video.so"
#define Y1_VIDEO_DB_PATH     ROCKBOX_DIR "/video_db.dat"
#define Y1_VIDEO_THUMB_DIR   ROCKBOX_DIR "/video_thumbs"
#define Y1_VIDEO_DB_MAGIC    0x59315644U /* 'Y1VD' */
#define Y1_VIDEO_DB_VERSION  1
#define MAX_DB_VIDEOS        256
#define MAX_UNIQUE_GROUPS    128

struct y1_video_db_entry {
    char     path[256];
    char     title[128];
    char     tv_show[128];
    char     creator[128];
    int      year;
    int      season;
    int      episode;
    int      width;
    int      height;
    int      fps_x100;
    int64_t  duration_ms;
    int64_t  resume_ms;
    uint32_t thumb_crc;
    int      has_thumb;
};

struct y1_video_db_header {
    uint32_t magic;
    uint32_t version;
    int32_t  count;
};

static struct y1_video_db_entry g_db[MAX_DB_VIDEOS];
static int g_db_count = 0;
static bool g_db_loaded = false;

static void *g_lib_handle = NULL;
static struct y1vid_api g_vid;
static bool g_lib_ready = false;

/* Audio streaming double buffer for Rockbox mixer channel (512 frames = 11.6ms at 44.1kHz) */
#define PCM_CHUNK_FRAMES     512
#define AUDIO_HW_LATENCY_MS  185
static int16_t g_pcm_buf[2][PCM_CHUNK_FRAMES * 2];
static int g_pcm_active_buf = 0;
static y1vid_ctx *g_active_vid_ctx = NULL;
static volatile bool g_pcm_paused = false;
static volatile int64_t g_audio_played_samples = 0;
static volatile int64_t g_audio_clock_ms = 0;
static volatile int64_t g_audio_clock_wall_ms = 0;
static volatile bool g_audio_clock_valid = false;

static int64_t now_ms_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
}

/* Video framebuffer (supports up to 480x360 RGB565) */
static uint16_t g_video_fb[480 * 360];
static uint16_t g_thumb_buf[Y1VID_THUMB_W * Y1VID_THUMB_H];

/* =========================================================================
 * External SD Card Library Loader (/mnt/sdcard/.rockbox/lib/liby1video.so)
 * ========================================================================= */
static bool load_external_video_library(void)
{
    if (g_lib_ready) return true;

    const char *libm_candidates[] = {
        "/mnt/sdcard/.rockbox/lib/libm.so.6",
        ROCKBOX_DIR "/lib/libm.so.6",
        "/lib/libm.so.6",
        NULL
    };
    for (int i = 0; libm_candidates[i] != NULL; i++) {
        if (dlopen(libm_candidates[i], RTLD_NOW | RTLD_GLOBAL))
            break;
    }

    const char *candidates[] = {
        Y1_VIDEO_LIB_PATH,
        ROCKBOX_DIR "/lib/liby1video.so",
        "/lib/liby1video.so",
        NULL
    };

    for (int i = 0; candidates[i] != NULL; i++) {
        g_lib_handle = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (g_lib_handle) break;
    }

    if (!g_lib_handle) {
        rb->splashf(HZ * 2, "Missing SD library:\n%s", Y1_VIDEO_LIB_PATH);
        return false;
    }

    g_vid.get_abi_version   = (fn_y1vid_get_abi_version)dlsym(g_lib_handle, "y1vid_get_abi_version");
    g_vid.probe_metadata    = (fn_y1vid_probe_metadata)dlsym(g_lib_handle, "y1vid_probe_metadata");
    g_vid.open              = (fn_y1vid_open)dlsym(g_lib_handle, "y1vid_open");
    g_vid.set_output_format = (fn_y1vid_set_output_format)dlsym(g_lib_handle, "y1vid_set_output_format");
    g_vid.decode_video      = (fn_y1vid_decode_video)dlsym(g_lib_handle, "y1vid_decode_video");
    g_vid.decode_audio      = (fn_y1vid_decode_audio)dlsym(g_lib_handle, "y1vid_decode_audio");
    g_vid.seek              = (fn_y1vid_seek)dlsym(g_lib_handle, "y1vid_seek");
    g_vid.get_duration_ms   = (fn_y1vid_get_duration_ms)dlsym(g_lib_handle, "y1vid_get_duration_ms");
    g_vid.get_position_ms   = (fn_y1vid_get_position_ms)dlsym(g_lib_handle, "y1vid_get_position_ms");
    g_vid.get_meta          = (fn_y1vid_get_meta)dlsym(g_lib_handle, "y1vid_get_meta");
    g_vid.close             = (fn_y1vid_close)dlsym(g_lib_handle, "y1vid_close");

    if (!g_vid.get_abi_version || !g_vid.probe_metadata || !g_vid.open ||
        !g_vid.decode_video || !g_vid.decode_audio || !g_vid.seek || !g_vid.close) {
        rb->splash(HZ * 2, "Invalid liby1video.so ABI");
        dlclose(g_lib_handle);
        g_lib_handle = NULL;
        return false;
    }

    if (g_vid.get_abi_version() != Y1VID_ABI_VERSION) {
        rb->splash(HZ * 2, "liby1video.so version mismatch");
        dlclose(g_lib_handle);
        g_lib_handle = NULL;
        return false;
    }

    g_lib_ready = true;
    return true;
}

/* =========================================================================
 * Hardware Performance Boost (1.2 GHz Dual Cortex-A7 for 1080p 30fps)
 * ========================================================================= */
static void sysfs_write_str(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        ssize_t r = write(fd, val, strlen(val));
        (void)r;
        close(fd);
    }
}

static void y1_video_set_cpu_boost(bool boost)
{
#ifdef HAVE_SCHEDULER_BOOSTCTRL
    if (boost) rb->trigger_cpu_boost();
    else       rb->cancel_cpu_boost();
#endif
    if (boost) {
        sysfs_write_str("/sys/devices/system/cpu/cpu1/online", "1");
        sysfs_write_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "performance");
        sysfs_write_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "1201000");
        sysfs_write_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed", "1201000");
    } else {
        sysfs_write_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "ondemand");
    }
}

/* =========================================================================
 * Time Formatting Helper (64-bit safe, supports >2 hours e.g. 03:45:12)
 * ========================================================================= */
static void format_time_64(int64_t ms, bool neg_prefix, char *buf, size_t buf_sz)
{
    if (ms < 0) ms = 0;
    int64_t total_sec = ms / 1000;
    int hours = (int)(total_sec / 3600);
    int mins  = (int)((total_sec % 3600) / 60);
    int secs  = (int)(total_sec % 60);

    if (hours > 0) {
        rb->snprintf(buf, buf_sz, "%s%d:%02d:%02d",
                     neg_prefix ? "-" : "", hours, mins, secs);
    } else {
        rb->snprintf(buf, buf_sz, "%s%02d:%02d",
                     neg_prefix ? "-" : "", mins, secs);
    }
}

/* =========================================================================
 * Video Database & Thumbnail Cache Engine
 * ========================================================================= */
static const char *get_configured_video_dir(void)
{
    const char *dir = rb->global_settings->y1_video_dir;
    if (!dir || !dir[0] || dir[0] != '/')
        return "/Videos";
    return dir;
}

static bool is_supported_video_file(const char *name)
{
    if (!name || name[0] == '.') return false;
    const char *dot = rb->strrchr(name, '.');
    if (!dot) return false;
    return (rb->strcasecmp(dot, ".mp4") == 0 ||
            rb->strcasecmp(dot, ".mkv") == 0 ||
            rb->strcasecmp(dot, ".mov") == 0 ||
            rb->strcasecmp(dot, ".m4v") == 0);
}

static void get_thumb_cache_path(uint32_t crc, char *out, size_t out_sz)
{
    rb->snprintf(out, out_sz, "%s/%08x.rgb", Y1_VIDEO_THUMB_DIR, (unsigned int)crc);
}

static bool save_thumbnail_cache(uint32_t crc, const uint16_t *rgb565)
{
    if (!rb->dir_exists(Y1_VIDEO_THUMB_DIR))
        rb->mkdir(Y1_VIDEO_THUMB_DIR);

    char tpath[MAX_PATH];
    get_thumb_cache_path(crc, tpath, sizeof(tpath));
    int fd = rb->open(tpath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    size_t bytes = Y1VID_THUMB_W * Y1VID_THUMB_H * sizeof(uint16_t);
    ssize_t w = rb->write(fd, rgb565, bytes);
    rb->close(fd);
    return (w == (ssize_t)bytes);
}

static bool load_thumbnail_cache(uint32_t crc, uint16_t *rgb565_out)
{
    char tpath[MAX_PATH];
    get_thumb_cache_path(crc, tpath, sizeof(tpath));
    int fd = rb->open(tpath, O_RDONLY);
    if (fd < 0) return false;
    size_t bytes = Y1VID_THUMB_W * Y1VID_THUMB_H * sizeof(uint16_t);
    ssize_t r = rb->read(fd, rgb565_out, bytes);
    rb->close(fd);
    return (r == (ssize_t)bytes);
}

static void save_video_database(void)
{
    int fd = rb->open(Y1_VIDEO_DB_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;

    struct y1_video_db_header hdr;
    hdr.magic   = Y1_VIDEO_DB_MAGIC;
    hdr.version = Y1_VIDEO_DB_VERSION;
    hdr.count   = g_db_count;

    rb->write(fd, &hdr, sizeof(hdr));
    if (g_db_count > 0) {
        rb->write(fd, g_db, (size_t)g_db_count * sizeof(struct y1_video_db_entry));
    }
    rb->close(fd);
}

static bool load_video_database(void)
{
    if (g_db_loaded) return true;
    int fd = rb->open(Y1_VIDEO_DB_PATH, O_RDONLY);
    if (fd < 0) return false;

    struct y1_video_db_header hdr;
    if (rb->read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        hdr.magic != Y1_VIDEO_DB_MAGIC ||
        hdr.version != Y1_VIDEO_DB_VERSION ||
        hdr.count < 0 || hdr.count > MAX_DB_VIDEOS) {
        rb->close(fd);
        return false;
    }

    g_db_count = hdr.count;
    if (g_db_count > 0) {
        ssize_t want = (ssize_t)g_db_count * (ssize_t)sizeof(struct y1_video_db_entry);
        if (rb->read(fd, g_db, (size_t)want) != want) {
            g_db_count = 0;
            rb->close(fd);
            return false;
        }
    }
    rb->close(fd);
    g_db_loaded = true;
    return true;
}

static int find_db_entry_by_path(const char *path)
{
    for (int i = 0; i < g_db_count; i++) {
        if (rb->strcmp(g_db[i].path, path) == 0)
            return i;
    }
    return -1;
}

static int ensure_db_entry_for_path(const char *path)
{
    load_video_database();
    int idx = find_db_entry_by_path(path);
    if (idx >= 0) return idx;

    if (!load_external_video_library()) return -1;
    if (g_db_count >= MAX_DB_VIDEOS) return -1;

    struct y1vid_meta meta;
    bool extract_thumbs = rb->global_settings->y1_video_show_thumbs;
    if (g_vid.probe_metadata(path, &meta,
                             extract_thumbs ? g_thumb_buf : NULL,
                             Y1VID_THUMB_W, Y1VID_THUMB_H) != 0) {
        return -1;
    }

    idx = g_db_count++;
    struct y1_video_db_entry *e = &g_db[idx];
    rb->memset(e, 0, sizeof(*e));
    rb->strlcpy(e->path, path, sizeof(e->path));
    rb->strlcpy(e->title, meta.title, sizeof(e->title));
    rb->strlcpy(e->tv_show, meta.tv_show, sizeof(e->tv_show));
    rb->strlcpy(e->creator, meta.creator, sizeof(e->creator));
    e->year        = meta.year;
    e->season      = meta.season;
    e->episode     = meta.episode;
    e->width       = meta.width;
    e->height      = meta.height;
    e->fps_x100    = meta.fps_x100;
    e->duration_ms = meta.duration_ms;
    e->resume_ms   = 0;
    e->thumb_crc   = rb->crc_32(path, rb->strlen(path), 0xFFFFFFFFU);
    if (extract_thumbs && meta.has_thumb) {
        e->has_thumb = save_thumbnail_cache(e->thumb_crc, g_thumb_buf) ? 1 : 0;
    }
    save_video_database();
    return idx;
}

/* Preserve bookmarks across database rescans */
struct bookmark_backup {
    uint32_t crc;
    int64_t  resume_ms;
};
static struct bookmark_backup g_bm_backup[MAX_DB_VIDEOS];
static int g_bm_backup_count = 0;

static void scan_dir_recursive(const char *dir_path, int depth)
{
    if (depth > 6 || g_db_count >= MAX_DB_VIDEOS) return;
    DIR *dir = rb->opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = rb->readdir(dir)) != NULL && g_db_count < MAX_DB_VIDEOS) {
        const char *name = entry->d_name;
        if (name[0] == '.') continue;

        char fullpath[MAX_PATH];
        if (rb->strcmp(dir_path, "/") == 0)
            rb->snprintf(fullpath, sizeof(fullpath), "/%s", name);
        else
            rb->snprintf(fullpath, sizeof(fullpath), "%s/%s", dir_path, name);

        struct dirinfo info = rb->dir_get_info(dir, entry);
        if (info.attribute & ATTR_DIRECTORY) {
            scan_dir_recursive(fullpath, depth + 1);
        } else if (is_supported_video_file(name)) {
            rb->splash_progress(g_db_count + 1, MAX_DB_VIDEOS,
                                "Scanning videos...\n%s", name);
            struct y1vid_meta meta;
            bool extract_thumbs = rb->global_settings->y1_video_show_thumbs;
            if (g_vid.probe_metadata(fullpath, &meta,
                                     extract_thumbs ? g_thumb_buf : NULL,
                                     Y1VID_THUMB_W, Y1VID_THUMB_H) == 0) {
                struct y1_video_db_entry *e = &g_db[g_db_count++];
                rb->memset(e, 0, sizeof(*e));
                rb->strlcpy(e->path, fullpath, sizeof(e->path));
                rb->strlcpy(e->title, meta.title, sizeof(e->title));
                rb->strlcpy(e->tv_show, meta.tv_show, sizeof(e->tv_show));
                rb->strlcpy(e->creator, meta.creator, sizeof(e->creator));
                e->year        = meta.year;
                e->season      = meta.season;
                e->episode     = meta.episode;
                e->width       = meta.width;
                e->height      = meta.height;
                e->fps_x100    = meta.fps_x100;
                e->duration_ms = meta.duration_ms;
                e->thumb_crc   = rb->crc_32(fullpath, rb->strlen(fullpath), 0xFFFFFFFFU);

                for (int b = 0; b < g_bm_backup_count; b++) {
                    if (g_bm_backup[b].crc == e->thumb_crc) {
                        e->resume_ms = g_bm_backup[b].resume_ms;
                        break;
                    }
                }
                if (extract_thumbs && meta.has_thumb) {
                    e->has_thumb = save_thumbnail_cache(e->thumb_crc, g_thumb_buf) ? 1 : 0;
                }
            }
        }
    }
    rb->closedir(dir);
}

static int db_cmp_title(const void *a, const void *b)
{
    const struct y1_video_db_entry *ea = (const struct y1_video_db_entry *)a;
    const struct y1_video_db_entry *eb = (const struct y1_video_db_entry *)b;
    return rb->strcasecmp(ea->title, eb->title);
}

static void rebuild_video_database(bool show_splash)
{
    if (!load_external_video_library()) return;

    load_video_database();
    g_bm_backup_count = g_db_count;
    for (int i = 0; i < g_db_count; i++) {
        g_bm_backup[i].crc       = g_db[i].thumb_crc;
        g_bm_backup[i].resume_ms = g_db[i].resume_ms;
    }

    const char *vdir = get_configured_video_dir();
    if (!rb->dir_exists(vdir)) {
        rb->mkdir(vdir);
    }

    g_db_count = 0;
    g_db_loaded = true;
    scan_dir_recursive(vdir, 0);

    if (g_db_count > 1) {
        rb->qsort(g_db, (size_t)g_db_count, sizeof(struct y1_video_db_entry), db_cmp_title);
    }

    save_video_database();
    if (show_splash) {
        rb->splashf(HZ, "Database Updated: %d videos", g_db_count);
    }
}

/* =========================================================================
 * Video Details Menu (Standard Rockbox Menu via do_menu)
 * ========================================================================= */
struct video_details_menu_ctx {
    const struct y1_video_db_entry *entry;
    char lines[8][192];
};

static char *video_details_get_name(int selected_item, void *data,
                                    char *buffer, size_t buffer_len)
{
    struct video_details_menu_ctx *dctx = (struct video_details_menu_ctx *)data;
    if (!dctx) return "";
    if (selected_item < 0) {
        rb->strlcpy(buffer, dctx->entry ? dctx->entry->title : "Video Details", buffer_len);
        return buffer;
    }
    if (selected_item >= 8) return "";
    rb->strlcpy(buffer, dctx->lines[selected_item], buffer_len);
    return buffer;
}

static void show_video_details_screen(const struct y1_video_db_entry *e)
{
    if (!e) return;

    char dur_str[32], res_str[32];
    format_time_64(e->duration_ms, false, dur_str, sizeof(dur_str));
    format_time_64(e->resume_ms, false, res_str, sizeof(res_str));

    static struct video_details_menu_ctx dctx;
    rb->memset(&dctx, 0, sizeof(dctx));
    dctx.entry = e;

    rb->snprintf(dctx.lines[0], sizeof(dctx.lines[0]), "Title: %s",
                 e->title[0] ? e->title : "Unknown");
    rb->snprintf(dctx.lines[1], sizeof(dctx.lines[1]), "TV Show: %s",
                 e->tv_show[0] ? e->tv_show : "None");
    if (e->season > 0 || e->episode > 0) {
        rb->snprintf(dctx.lines[2], sizeof(dctx.lines[2]), "Episode: Season %d, Episode %d",
                     e->season, e->episode);
    } else {
        rb->snprintf(dctx.lines[2], sizeof(dctx.lines[2]), "Episode: None");
    }
    rb->snprintf(dctx.lines[3], sizeof(dctx.lines[3]), "Creator: %s",
                 e->creator[0] ? e->creator : "Unknown");
    if (e->year > 0)
        rb->snprintf(dctx.lines[4], sizeof(dctx.lines[4]), "Year: %d", e->year);
    else
        rb->snprintf(dctx.lines[4], sizeof(dctx.lines[4]), "Year: Unknown");
    rb->snprintf(dctx.lines[5], sizeof(dctx.lines[5]), "Duration: %s (Saved: %s)",
                 dur_str, res_str);
    rb->snprintf(dctx.lines[6], sizeof(dctx.lines[6]), "Format: %dx%d @ %d.%02d fps",
                 e->width, e->height, e->fps_x100 / 100, e->fps_x100 % 100);
    rb->snprintf(dctx.lines[7], sizeof(dctx.lines[7]), "File: %s", e->path);

    static const struct menu_get_name_and_icon details_dyn = {
        .menu_callback      = NULL,
        .list_get_name      = video_details_get_name,
        .list_speak_item    = NULL,
        .list_get_name_data = &dctx,
        .icon_id            = Icon_Submenu_Entered,
    };
    static const struct menu_item_ex details_menu = {
        .flags = MT_RETURN_ID | MENU_DYNAMIC_DESC | MENU_ITEM_COUNT(8),
        .strings = NULL,
        .menu_get_name_and_icon = &details_dyn,
    };

    int selected = 0;
    while (1) {
        int ret = rb->do_menu(&details_menu, &selected, NULL, false);
        if (ret < 0 || ret == GO_TO_PREVIOUS || ret == GO_TO_ROOT || ret == MENU_ATTACHED_USB)
            break;
    }
}

/* =========================================================================
 * Audio Mixer Streaming Callback & Master Audio Clock
 * ========================================================================= */
static void video_pcm_get_more(const void **start, size_t *size)
{
    if (!g_active_vid_ctx || g_pcm_paused) {
        rb->memset(g_pcm_buf[g_pcm_active_buf], 0, sizeof(g_pcm_buf[0]));
        *start = g_pcm_buf[g_pcm_active_buf];
        *size  = sizeof(g_pcm_buf[0]);
        g_pcm_active_buf ^= 1;
        return;
    }

    int64_t a_pts = -1;
    int got = g_vid.decode_audio(g_active_vid_ctx,
                                 g_pcm_buf[g_pcm_active_buf],
                                 PCM_CHUNK_FRAMES, &a_pts);
    if (got <= 0) {
        rb->memset(g_pcm_buf[g_pcm_active_buf], 0, sizeof(g_pcm_buf[0]));
        got = PCM_CHUNK_FRAMES;
    } else {
        g_audio_played_samples += got;
        if (a_pts >= 0) {
            g_audio_clock_ms      = a_pts;
            g_audio_clock_wall_ms = now_ms_monotonic();
            g_audio_clock_valid   = true;
        }
    }

    *start = g_pcm_buf[g_pcm_active_buf];
    *size  = (size_t)got * 2 * sizeof(int16_t);
    g_pcm_active_buf ^= 1;
}

static int64_t get_master_clock_ms(int64_t media_anchor_ms, int64_t wall_anchor_ms)
{
    int64_t now_ms = now_ms_monotonic();
    if (g_audio_clock_valid && !g_pcm_paused) {
        int64_t since_chunk_ms = now_ms - g_audio_clock_wall_ms;
        if (since_chunk_ms < 0)  since_chunk_ms = 0;
        if (since_chunk_ms > 40) since_chunk_ms = 40;
        int64_t aud_ms = g_audio_clock_ms + since_chunk_ms - AUDIO_HW_LATENCY_MS;
        if (aud_ms < media_anchor_ms)
            aud_ms = media_anchor_ms;
        return aud_ms;
    }
    return media_anchor_ms + (now_ms - wall_anchor_ms);
}

/* =========================================================================
 * iPod Video OSD & Playback Engine
 * ========================================================================= */
enum ipod_osd_mode {
    OSD_HIDDEN = 0,
    OSD_TIMELINE,   /* Top progress bar shown; wheel adjusts volume */
    OSD_SCRUBBING   /* Top progress bar with diamond scrubber; wheel scrubs video */
};

static void draw_ipod_osd(int sw, int sh, enum ipod_osd_mode osd_mode,
                          long vol_osd_until_tick, long status_osd_until_tick,
                          int64_t pos_ms, int64_t scrub_ms, int64_t dur_ms,
                          bool paused, int ff_rew_speed)
{
    long now = *rb->current_tick;
    int fh = rb->screens[SCREEN_MAIN]->getcharheight();
    if (fh < 12) fh = 14;

    /* 1. Top Timeline / Scrubbing Bar (iPod Video style) */
    if (osd_mode != OSD_HIDDEN || now < status_osd_until_tick || paused || ff_rew_speed != 0) {
        int bar_h = fh + 22;
        rb->lcd_set_foreground(LCD_RGBPACK(16, 18, 24));
        rb->lcd_fillrect(0, 0, sw, bar_h);
        rb->lcd_set_foreground(LCD_RGBPACK(70, 80, 100));
        rb->lcd_hline(0, sw - 1, bar_h);

        int64_t disp_ms = (osd_mode == OSD_SCRUBBING) ? scrub_ms : pos_ms;
        if (disp_ms < 0) disp_ms = 0;
        if (dur_ms > 0 && disp_ms > dur_ms) disp_ms = dur_ms;
        int64_t rem_ms = (dur_ms > disp_ms) ? (dur_ms - disp_ms) : 0;

        char left_t[32], right_t[32];
        format_time_64(disp_ms, false, left_t, sizeof(left_t));
        format_time_64(rem_ms, true, right_t, sizeof(right_t));

        rb->lcd_set_background(LCD_RGBPACK(16, 18, 24));
        rb->lcd_set_foreground(LCD_RGBPACK(245, 248, 255));
        rb->lcd_putsxy(8, 4, left_t);

        int rw = 0, rh = 0;
        rb->lcd_getstringsize(right_t, &rw, &rh);
        rb->lcd_putsxy(sw - rw - 8, 4, right_t);

        /* Status badge in top center (Play / Pause / Scrubbing / 2x..8x) */
        const char *mode_str = paused ? "PAUSED" :
                               (osd_mode == OSD_SCRUBBING ? "SCRUBBING" :
                               (ff_rew_speed > 0 ? "FAST FWD" :
                               (ff_rew_speed < 0 ? "REWIND" : "PLAYING")));
        int mw = 0, mh = 0;
        rb->lcd_getstringsize(mode_str, &mw, &mh);
        rb->lcd_set_foreground(osd_mode == OSD_SCRUBBING ?
                               LCD_RGBPACK(100, 200, 255) : LCD_RGBPACK(190, 200, 215));
        rb->lcd_putsxy((sw - mw) / 2, 4, mode_str);

        /* Timeline track bar */
        int track_x = 12;
        int track_w = sw - 24;
        int track_y = fh + 8;
        int track_h = 8;

        rb->lcd_set_foreground(LCD_RGBPACK(55, 62, 75));
        rb->lcd_fillrect(track_x, track_y, track_w, track_h);
        rb->lcd_set_foreground(LCD_RGBPACK(120, 130, 150));
        rb->lcd_drawrect(track_x, track_y, track_w, track_h);

        int fill_w = (dur_ms > 0) ? (int)((disp_ms * (int64_t)(track_w - 2)) / dur_ms) : 0;
        if (fill_w < 0) fill_w = 0;
        if (fill_w > track_w - 2) fill_w = track_w - 2;

        rb->lcd_set_foreground(osd_mode == OSD_SCRUBBING ?
                               LCD_RGBPACK(70, 175, 255) : LCD_RGBPACK(230, 238, 250));
        if (fill_w > 0)
            rb->lcd_fillrect(track_x + 1, track_y + 1, fill_w, track_h - 2);

        /* iPod Video Diamond Scrubber Indicator when in OSD_SCRUBBING mode */
        if (osd_mode == OSD_SCRUBBING) {
            int dx = track_x + 1 + fill_w;
            int dy = track_y + track_h / 2;
            rb->lcd_set_foreground(LCD_RGBPACK(255, 255, 255));
            for (int r = -5; r <= 5; r++) {
                int span = 5 - (r < 0 ? -r : r);
                rb->lcd_hline(dx - span, dx + span, dy + r);
            }
        }
    }

    /* 2. Bottom Volume Bar OSD (iPod Video style when scrolling wheel in normal mode) */
    if (now < vol_osd_until_tick && osd_mode != OSD_SCRUBBING) {
        int vbar_h = 26;
        int vbar_y = sh - vbar_h;
        rb->lcd_set_foreground(LCD_RGBPACK(16, 18, 24));
        rb->lcd_fillrect(0, vbar_y, sw, vbar_h);
        rb->lcd_set_foreground(LCD_RGBPACK(70, 80, 100));
        rb->lcd_hline(0, sw - 1, vbar_y);

        int vmin = rb->sound_min(SOUND_VOLUME);
        int vmax = rb->sound_max(SOUND_VOLUME);
        int vcur = rb->global_status->volume;
        int vspan = (vmax > vmin) ? (vmax - vmin) : 100;
        int vpct = ((vcur - vmin) * 100) / vspan;
        if (vpct < 0) vpct = 0;
        if (vpct > 100) vpct = 100;

        int bx = 36;
        int bw = sw - 72;
        int by = vbar_y + 9;
        int bh = 8;

        rb->lcd_set_background(LCD_RGBPACK(16, 18, 24));
        rb->lcd_set_foreground(LCD_RGBPACK(220, 228, 240));
        rb->lcd_putsxy(10, vbar_y + 5, "-");
        rb->lcd_putsxy(sw - 20, vbar_y + 5, "+");

        rb->lcd_set_foreground(LCD_RGBPACK(55, 62, 75));
        rb->lcd_fillrect(bx, by, bw, bh);
        rb->lcd_set_foreground(LCD_RGBPACK(120, 130, 150));
        rb->lcd_drawrect(bx, by, bw, bh);

        int vfill = (bw - 2) * vpct / 100;
        if (vfill > 0) {
            rb->lcd_set_foreground(LCD_RGBPACK(240, 245, 255));
            rb->lcd_fillrect(bx + 1, by + 1, vfill, bh - 2);
        }
    }
}

/* Forward declarations for Rockbox menus */
static void run_video_settings_menu(void);
static void init_video_menus(void);

/* In-video long-press Select context menu (uses standard Rockbox do_menu) */
static int show_in_video_context_menu(struct y1_video_db_entry *db_entry,
                                      y1vid_ctx *ctx, int sw, int sh,
                                      bool *restart_requested)
{
    MENUITEM_STRINGLIST(menu, "Video Options", NULL,
                        "Aspect Ratio",
                        "Video Details",
                        "Play from Beginning",
                        "Video Settings",
                        "Exit Video");

    int selected = 0;
    int res = rb->do_menu(&menu, &selected, NULL, false);
    if (res == 0) {
        const struct settings_list *s = rb->find_setting(&rb->global_settings->y1_video_aspect);
        if (s) {
            rb->option_screen(s, NULL, false, ID2P(LANG_Y1_VIDEO_ASPECT));
            rb->settings_save();
        }
        g_vid.set_output_format(ctx, sw, sh, rb->global_settings->y1_video_aspect);
    } else if (res == 1) {
        if (db_entry) show_video_details_screen(db_entry);
    } else if (res == 2) {
        if (restart_requested) *restart_requested = true;
    } else if (res == 3) {
        run_video_settings_menu();
        g_vid.set_output_format(ctx, sw, sh, rb->global_settings->y1_video_aspect);
    } else if (res == 4) {
        return 1; /* Exit video */
    }
    return 0;
}

/* Returns:
 *  0 = user pressed MENU to exit
 *  1 = video finished naturally (for autoplay next)
 * -1 = user pressed LEFT at start of video (previous video)
 */
static int play_single_video(const char *path, bool force_from_beginning)
{
    if (!load_external_video_library())
        return 0;

    int db_idx = ensure_db_entry_for_path(path);
    struct y1_video_db_entry *db_entry = (db_idx >= 0) ? &g_db[db_idx] : NULL;

    struct screen *disp = rb->screens[SCREEN_MAIN];
    int sw = disp->getwidth();
    int sh = disp->getheight();
    if (sw > 480) sw = 480;
    if (sh > 360) sh = 360;

    int aspect = rb->global_settings->y1_video_aspect;
    y1vid_ctx *ctx = g_vid.open(path, sw, sh, aspect, 44100);
    if (!ctx) {
        rb->splashf(HZ * 2, "Cannot open video:\n%s", path);
        return 0;
    }

    int64_t dur_ms = g_vid.get_duration_ms(ctx);
    if (db_entry && dur_ms > 0)
        db_entry->duration_ms = dur_ms;

    /* Check resume bookmark */
    int64_t start_ms = 0;
    if (!force_from_beginning && db_entry && db_entry->resume_ms > 3000 &&
        (dur_ms == 0 || db_entry->resume_ms + 3000 < dur_ms)) {
        int rmode = rb->global_settings->y1_video_resume;
        if (rmode == 2) {
            start_ms = db_entry->resume_ms;
        } else if (rmode == 1) {
            char tbuf[32], msg[96];
            format_time_64(db_entry->resume_ms, false, tbuf, sizeof(tbuf));
            rb->snprintf(msg, sizeof(msg), "Resume at %s?", tbuf);
            if (rb->yesno_pop(msg))
                start_ms = db_entry->resume_ms;
        }
    }
    if (start_ms > 0) {
        g_vid.seek(ctx, start_ms);
    }

    /* Disable theme statusbar/viewports for fullscreen video playback */
    FOR_NB_SCREENS(i)
        rb->viewportmanager_theme_enable(i, false, NULL);
    rb->lcd_set_viewport(NULL);

    /* Stop background music & initialize PCM mixer channel with unity amplitude */
    rb->audio_stop();
    rb->pcmbuf_fade(false, true);
    rb->mixer_set_frequency(44100);
    rb->sound_set(SOUND_VOLUME, rb->global_status->volume);
    rb->mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, MIX_AMP_UNITY);

    g_active_vid_ctx = ctx;
    g_pcm_paused = false;
    g_audio_played_samples = 0;
    g_audio_clock_ms = start_ms;
    g_audio_clock_wall_ms = now_ms_monotonic();
    g_audio_clock_valid = false;

    static const struct mixer_play_cbs pcm_cbs = {
        .get_more = video_pcm_get_more,
    };
    rb->mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, &pcm_cbs, NULL, 0);

    /* Boost CPU to 1.2 GHz dual-core & keep backlight on */
    y1_video_set_cpu_boost(true);
    rb->backlight_on();

    enum ipod_osd_mode osd_mode = OSD_HIDDEN;
    long status_osd_until = *rb->current_tick + HZ * 3;
    long vol_osd_until    = 0;
    long last_bl_kick     = *rb->current_tick;
    long last_scrub_tick  = 0;
    bool scrub_dirty      = false;
    int64_t scrub_ms      = start_ms;
    int64_t cur_pts_ms    = start_ms;
    bool paused           = false;
    int  ff_rew_speed     = 0;
    int  exit_reason      = 0;

    /* Master clock anchors for A/V sync & 100% real-time frame-drop pacing */
    int64_t wall_anchor_ms  = now_ms_monotonic();
    int64_t media_anchor_ms = start_ms;

    /* Decode initial frame */
    g_vid.decode_video(ctx, g_video_fb, &cur_pts_ms, 0);

    while (1) {
        long now = *rb->current_tick;

        /* Keep backlight on continuously while video plugin is playing */
        if (now - last_bl_kick >= HZ) {
            rb->backlight_on();
            rb->reset_poweroff_timer();
            last_bl_kick = now;
        }

        /* Auto-commit scrub seek 400ms after user stops turning the wheel in scrub mode */
        if (osd_mode == OSD_SCRUBBING && scrub_dirty && (now - last_scrub_tick >= (HZ * 2) / 5)) {
            g_audio_clock_valid = false;
            g_vid.seek(ctx, scrub_ms);
            cur_pts_ms      = scrub_ms;
            media_anchor_ms = scrub_ms;
            wall_anchor_ms  = now_ms_monotonic();
            scrub_dirty     = false;
            g_vid.decode_video(ctx, g_video_fb, &cur_pts_ms, 0);
        }

        /* Fast-Forward / Rewind scan when holding Right / Left */
        if (ff_rew_speed != 0) {
            int64_t jump_ms = (int64_t)ff_rew_speed * 250;
            int64_t next_ms = cur_pts_ms + jump_ms;
            if (next_ms < 0) next_ms = 0;
            if (dur_ms > 0 && next_ms > dur_ms) next_ms = dur_ms;
            g_audio_clock_valid = false;
            g_vid.seek(ctx, next_ms);
            g_vid.decode_video(ctx, g_video_fb, &cur_pts_ms, 1);
            media_anchor_ms = cur_pts_ms;
            wall_anchor_ms  = now_ms_monotonic();
        } else if (!paused && !scrub_dirty) {
            int64_t clock_ms = get_master_clock_ms(media_anchor_ms, wall_anchor_ms);
            int64_t lag_ms   = clock_ms - cur_pts_ms;

            /* Decode into ctx->v_frame first without YUV->RGB565 conversion:
             * drop_mode 3 = AVDISCARD_NONREF (skip B-frames) + no blit
             * drop_mode 2 = AVDISCARD_DEFAULT + no blit */
            int drop_mode = (lag_ms > 20) ? 3 : 2;
            int dec = g_vid.decode_video(ctx, g_video_fb, &cur_pts_ms, drop_mode);

            /* If the frame we just decoded is still >35ms behind the master audio clock,
             * skip YUV->RGB565 conversion and LCD update on it and immediately decode
             * the next frame (up to 4 catch-up frames) so video stays at 100% speed! */
            int catchup_steps = 0;
            while (dec > 0 && catchup_steps < 4) {
                clock_ms = get_master_clock_ms(media_anchor_ms, wall_anchor_ms);
                lag_ms   = clock_ms - cur_pts_ms;
                if (lag_ms <= 35)
                    break;
                dec = g_vid.decode_video(ctx, g_video_fb, &cur_pts_ms, 3);
                catchup_steps++;
            }

            if (dec <= 0) {
                /* Reached End of Video or decode error */
                if (dec == 0 && db_entry) {
                    db_entry->resume_ms = 0;
                    save_video_database();
                }
                exit_reason = (dec == 0) ? 1 : 0;
                break;
            }

            /* Blit the caught-up YUV420P frame into g_video_fb (drop_nonref == 4) */
            g_vid.decode_video(ctx, g_video_fb, &cur_pts_ms, 4);
        }

        /* Blit decoded video frame + iPod OSD to LCD */
        rb->lcd_bitmap((const fb_data *)g_video_fb, 0, 0, sw, sh);
        draw_ipod_osd(sw, sh, osd_mode, vol_osd_until, status_osd_until,
                      cur_pts_ms, scrub_ms, dur_ms, paused, ff_rew_speed);
        rb->lcd_update();

        /* Sub-millisecond frame pacing locked to master audio clock */
        long btn = BUTTON_NONE;
        if (paused || scrub_dirty) {
            btn = rb->button_get_w_tmo(HZ / 20);
        } else {
            btn = rb->button_get(false);
            if (btn == BUTTON_NONE) {
                int64_t clock_ms = get_master_clock_ms(media_anchor_ms, wall_anchor_ms);
                int64_t ahead_ms = cur_pts_ms - clock_ms;
                if (ahead_ms > 80) ahead_ms = 80;
                while (ahead_ms > 2) {
                    int64_t step_us = (ahead_ms > 6) ? 4000 : ((ahead_ms - 1) * 1000);
                    usleep((useconds_t)step_us);
                    btn = rb->button_get(false);
                    if (btn != BUTTON_NONE)
                        break;
                    clock_ms = get_master_clock_ms(media_anchor_ms, wall_anchor_ms);
                    ahead_ms = cur_pts_ms - clock_ms;
                }
            }
        }

        if (btn == BUTTON_NONE) {
            ff_rew_speed = 0;
            continue;
        }

        if (rb->default_event_handler(btn) == SYS_USB_CONNECTED) {
            exit_reason = 0;
            break;
        }

        /* ---- iPod Video Button Mapping ---- */
        if (btn == BUTTON_MENU || btn == (BUTTON_MENU | BUTTON_REL)) {
            if (osd_mode == OSD_SCRUBBING) {
                /* Cancel scrubbing and hide OSD */
                osd_mode = OSD_HIDDEN;
                scrub_dirty = false;
                continue;
            }
            /* Save 64-bit bookmark & exit */
            if (db_entry) {
                db_entry->resume_ms = (dur_ms > 0 && cur_pts_ms + 3000 >= dur_ms) ? 0 : cur_pts_ms;
                save_video_database();
            }
            exit_reason = 0;
            break;
        } else if (btn == BUTTON_PLAY || btn == (BUTTON_PLAY | BUTTON_REL)) {
            if (btn == (BUTTON_PLAY | BUTTON_REL)) continue;
            paused = !paused;
            g_pcm_paused = paused;
            rb->mixer_channel_play_pause(PCM_MIXER_CHAN_PLAYBACK, !paused);
            ff_rew_speed = 0;
            media_anchor_ms = cur_pts_ms;
            wall_anchor_ms  = now_ms_monotonic();
            g_audio_clock_wall_ms = wall_anchor_ms;
            status_osd_until = *rb->current_tick + HZ * 3;
        } else if (btn == (BUTTON_SELECT | BUTTON_REPEAT)) {
            /* Long-press Center: Open In-Video Options Menu */
            bool was_paused = paused;
            g_pcm_paused = true;
            rb->mixer_channel_play_pause(PCM_MIXER_CHAN_PLAYBACK, false);
            bool restart_req = false;
            int mres = show_in_video_context_menu(db_entry, ctx, sw, sh, &restart_req);
            rb->lcd_set_viewport(NULL);
            sw = disp->getwidth();
            sh = disp->getheight();
            if (sw > 480) sw = 480;
            if (sh > 360) sh = 360;
            if (mres == 1) {
                if (db_entry) {
                    db_entry->resume_ms = cur_pts_ms;
                    save_video_database();
                }
                exit_reason = 0;
                break;
            }
            if (restart_req) {
                g_audio_clock_valid = false;
                g_vid.seek(ctx, 0);
                cur_pts_ms = 0;
                scrub_ms   = 0;
            }
            g_pcm_paused = was_paused;
            rb->mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, MIX_AMP_UNITY);
            if (rb->mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_STOPPED) {
                rb->mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, &pcm_cbs, NULL, 0);
            }
            rb->mixer_channel_play_pause(PCM_MIXER_CHAN_PLAYBACK, !was_paused);
            media_anchor_ms = cur_pts_ms;
            wall_anchor_ms  = now_ms_monotonic();
            g_audio_clock_wall_ms = wall_anchor_ms;
        } else if (btn == (BUTTON_SELECT | BUTTON_REL) || btn == BUTTON_SELECT) {
            if (btn == BUTTON_SELECT) continue; /* Handle on release so long-press works */
            /* Short-press Center: Cycle iPod OSD Mode (Hidden -> Timeline -> Diamond Scrubbing -> Hidden) */
            if (osd_mode == OSD_HIDDEN) {
                osd_mode = OSD_TIMELINE;
                scrub_ms = cur_pts_ms;
                status_osd_until = *rb->current_tick + HZ * 5;
            } else if (osd_mode == OSD_TIMELINE) {
                osd_mode = OSD_SCRUBBING;
                scrub_ms = cur_pts_ms;
                scrub_dirty = false;
            } else {
                if (scrub_dirty) {
                    g_audio_clock_valid = false;
                    g_vid.seek(ctx, scrub_ms);
                    cur_pts_ms      = scrub_ms;
                    media_anchor_ms = scrub_ms;
                    wall_anchor_ms  = now_ms_monotonic();
                    scrub_dirty     = false;
                }
                osd_mode = OSD_HIDDEN;
            }
        } else if (btn & (BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK)) {
            int dir = (btn & BUTTON_SCROLL_FWD) ? 1 : -1;
            if (osd_mode == OSD_SCRUBBING) {
                /* Scroll Wheel in Scrubbing Mode: Scrub video timeline (64-bit safe for >2hr videos) */
                int64_t step_ms = (dur_ms > 3600000LL) ? 15000LL :
                                  (dur_ms > 600000LL   ? 5000LL  : 2000LL);
                scrub_ms += dir * step_ms;
                if (scrub_ms < 0) scrub_ms = 0;
                if (dur_ms > 0 && scrub_ms > dur_ms) scrub_ms = dur_ms;
                scrub_dirty = true;
                last_scrub_tick = *rb->current_tick;
            } else {
                /* Scroll Wheel in Default Mode: Adjust Volume & show bottom Volume Bar */
                rb->adjust_volume(dir);
                vol_osd_until = *rb->current_tick + HZ * 2;
            }
        } else if (btn == (BUTTON_RIGHT | BUTTON_REPEAT)) {
            ff_rew_speed = 4;
            status_osd_until = *rb->current_tick + HZ * 2;
        } else if (btn == (BUTTON_LEFT | BUTTON_REPEAT)) {
            ff_rew_speed = -4;
            status_osd_until = *rb->current_tick + HZ * 2;
        } else if (btn == (BUTTON_RIGHT | BUTTON_REL)) {
            if (ff_rew_speed != 0) {
                ff_rew_speed = 0;
            } else {
                int skip_s = rb->global_settings->y1_video_skip_step;
                if (skip_s <= 0) skip_s = 30;
                int64_t next_ms = cur_pts_ms + (int64_t)skip_s * 1000LL;
                if (dur_ms > 0 && next_ms >= dur_ms) {
                    exit_reason = 1;
                    break;
                }
                g_audio_clock_valid = false;
                g_vid.seek(ctx, next_ms);
                cur_pts_ms      = next_ms;
                scrub_ms        = next_ms;
                media_anchor_ms = next_ms;
                wall_anchor_ms  = now_ms_monotonic();
                status_osd_until = *rb->current_tick + HZ * 2;
            }
        } else if (btn == (BUTTON_LEFT | BUTTON_REL)) {
            if (ff_rew_speed != 0) {
                ff_rew_speed = 0;
            } else if (cur_pts_ms < 3000) {
                exit_reason = -1;
                break;
            } else {
                int skip_s = rb->global_settings->y1_video_skip_step;
                if (skip_s <= 0) skip_s = 30;
                int64_t prev_ms = cur_pts_ms - (int64_t)skip_s * 1000LL;
                if (prev_ms < 0) prev_ms = 0;
                g_audio_clock_valid = false;
                g_vid.seek(ctx, prev_ms);
                cur_pts_ms      = prev_ms;
                scrub_ms        = prev_ms;
                media_anchor_ms = prev_ms;
                wall_anchor_ms  = now_ms_monotonic();
                status_osd_until = *rb->current_tick + HZ * 2;
            }
        }
    }

    rb->mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
    g_active_vid_ctx = NULL;
    g_vid.close(ctx);
    y1_video_set_cpu_boost(false);

    /* Restore Rockbox theme & statusbar viewports */
    FOR_NB_SCREENS(i)
        rb->viewportmanager_theme_undo(i, true);

    return exit_reason;
}

/* =========================================================================
 * Standard Rockbox Menus (do_menu / MAKE_MENU) for Video Library & Settings
 * ========================================================================= */
struct filtered_video_list {
    const char *title;
    int indices[MAX_DB_VIDEOS];
    int count;
};

static void show_video_item_context_menu(int db_index)
{
    if (db_index < 0 || db_index >= g_db_count) return;
    struct y1_video_db_entry *e = &g_db[db_index];

    MENUITEM_STRINGLIST(menu, "Video Menu", NULL,
                        "Play",
                        "Play from Beginning",
                        "Video Details",
                        "Clear Resume Bookmark");
    int sel = 0;
    int res = rb->do_menu(&menu, &sel, NULL, false);
    if (res == 0) {
        play_single_video(e->path, false);
    } else if (res == 1) {
        play_single_video(e->path, true);
    } else if (res == 2) {
        show_video_details_screen(e);
    } else if (res == 3) {
        e->resume_ms = 0;
        save_video_database();
        rb->splash(HZ, "Bookmark cleared");
    }
}

static char *filtered_video_menu_get_name(int selected_item, void *data,
                                          char *buffer, size_t buffer_len)
{
    struct filtered_video_list *fl = (struct filtered_video_list *)data;
    if (!fl) return "";
    if (selected_item < 0) {
        rb->strlcpy(buffer, fl->title ? fl->title : "Videos", buffer_len);
        return buffer;
    }
    if (selected_item >= fl->count)
        return "";
    const struct y1_video_db_entry *e = &g_db[fl->indices[selected_item]];
    char dur[24];
    format_time_64(e->duration_ms, false, dur, sizeof(dur));
    if (e->season > 0 || e->episode > 0) {
        rb->snprintf(buffer, buffer_len, "S%02dE%02d - %s (%s)",
                     e->season, e->episode, e->title, dur);
    } else if (e->resume_ms > 3000) {
        rb->snprintf(buffer, buffer_len, "* %s (%s)", e->title, dur);
    } else {
        rb->snprintf(buffer, buffer_len, "%s (%s)", e->title, dur);
    }
    return buffer;
}

static int filtered_video_menu_cb(int action, const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    if (action == ACTION_STD_CONTEXT && this_item && this_list) {
        struct filtered_video_list *fl =
            (struct filtered_video_list *)this_item->menu_get_name_and_icon->list_get_name_data;
        int sel = rb->gui_synclist_get_sel_pos(this_list);
        if (fl && sel >= 0 && sel < fl->count) {
            show_video_item_context_menu(fl->indices[sel]);
            return ACTION_REDRAW;
        }
    }
    return action;
}

static void browse_filtered_videos(const char *title, struct filtered_video_list *fl)
{
    if (fl->count <= 0) {
        rb->splash(HZ, "No videos found");
        return;
    }
    fl->title = title;

    int menu_count = fl->count < 64 ? fl->count : 64;
    struct menu_get_name_and_icon dyn_desc = {
        .menu_callback      = filtered_video_menu_cb,
        .list_get_name      = filtered_video_menu_get_name,
        .list_speak_item    = NULL,
        .list_get_name_data = fl,
        .icon_id            = Icon_Submenu_Entered,
    };
    struct menu_item_ex dyn_menu = {
        .flags = MT_RETURN_ID | MENU_DYNAMIC_DESC | MENU_ITEM_COUNT(menu_count),
        .strings = NULL,
        .menu_get_name_and_icon = &dyn_desc,
    };

    int selected = 0;
    while (1) {
        int sel = rb->do_menu(&dyn_menu, &selected, NULL, false);
        if (sel < 0 || sel >= fl->count ||
            sel == GO_TO_PREVIOUS || sel == GO_TO_ROOT || sel == MENU_ATTACHED_USB) {
            break;
        }
        while (sel >= 0 && sel < fl->count) {
            selected = sel;
            int rc = play_single_video(g_db[fl->indices[sel]].path, false);
            if (rc == 1 && rb->global_settings->y1_video_autoplay_next && sel + 1 < fl->count) {
                sel++;
                continue;
            } else if (rc == -1 && sel > 0) {
                sel--;
                continue;
            }
            break;
        }
    }
}

/* Unique Group Browser (for TV Shows, Creators, Years) via do_menu */
struct group_list_data {
    const char *title;
    char names[MAX_UNIQUE_GROUPS][128];
    int  years[MAX_UNIQUE_GROUPS];
    int  count;
};

static char *group_list_get_name(int selected_item, void *data,
                                 char *buffer, size_t buffer_len)
{
    struct group_list_data *gl = (struct group_list_data *)data;
    if (!gl) return "";
    if (selected_item < 0) {
        rb->strlcpy(buffer, gl->title ? gl->title : "Videos", buffer_len);
        return buffer;
    }
    if (selected_item >= gl->count)
        return "";
    rb->strlcpy(buffer, gl->names[selected_item], buffer_len);
    return buffer;
}

enum group_filter_type {
    GROUP_BY_TV_SHOW = 0,
    GROUP_BY_CREATOR = 1,
    GROUP_BY_YEAR    = 2
};

static void browse_by_group(enum group_filter_type gtype, const char *menu_title)
{
    load_video_database();
    if (g_db_count == 0) {
        rebuild_video_database(true);
    }
    if (g_db_count == 0) {
        rb->splash(HZ, "No videos in database");
        return;
    }

    static struct group_list_data gl;
    gl.title = menu_title;
    gl.count = 0;

    for (int i = 0; i < g_db_count && gl.count < MAX_UNIQUE_GROUPS; i++) {
        const char *val = (gtype == GROUP_BY_TV_SHOW) ? g_db[i].tv_show : g_db[i].creator;
        int yval = g_db[i].year;
        bool exists = false;
        for (int j = 0; j < gl.count; j++) {
            if (gtype == GROUP_BY_YEAR) {
                if (gl.years[j] == yval) { exists = true; break; }
            } else {
                if (rb->strcasecmp(gl.names[j], val) == 0) { exists = true; break; }
            }
        }
        if (!exists) {
            gl.years[gl.count] = yval;
            if (gtype == GROUP_BY_YEAR) {
                if (yval > 0) rb->snprintf(gl.names[gl.count], sizeof(gl.names[0]), "%d", yval);
                else          rb->strlcpy(gl.names[gl.count], "Unknown Year", sizeof(gl.names[0]));
            } else {
                rb->strlcpy(gl.names[gl.count], val[0] ? val : "Unknown", sizeof(gl.names[0]));
            }
            gl.count++;
        }
    }

    int menu_count = gl.count < 64 ? gl.count : 64;
    struct menu_get_name_and_icon group_dyn = {
        .menu_callback      = NULL,
        .list_get_name      = group_list_get_name,
        .list_speak_item    = NULL,
        .list_get_name_data = &gl,
        .icon_id            = Icon_Submenu_Entered,
    };
    struct menu_item_ex group_menu = {
        .flags = MT_RETURN_ID | MENU_DYNAMIC_DESC | MENU_ITEM_COUNT(menu_count),
        .strings = NULL,
        .menu_get_name_and_icon = &group_dyn,
    };

    int selected = 0;
    while (1) {
        int sel = rb->do_menu(&group_menu, &selected, NULL, false);
        if (sel < 0 || sel >= gl.count ||
            sel == GO_TO_PREVIOUS || sel == GO_TO_ROOT || sel == MENU_ATTACHED_USB) {
            break;
        }
        static struct filtered_video_list fl;
        fl.count = 0;
        for (int i = 0; i < g_db_count; i++) {
            if (gtype == GROUP_BY_YEAR) {
                if (g_db[i].year == gl.years[sel]) fl.indices[fl.count++] = i;
            } else if (gtype == GROUP_BY_TV_SHOW) {
                const char *tv = g_db[i].tv_show[0] ? g_db[i].tv_show : "Unknown";
                if (rb->strcasecmp(tv, gl.names[sel]) == 0) fl.indices[fl.count++] = i;
            } else {
                const char *cr = g_db[i].creator[0] ? g_db[i].creator : "Unknown";
                if (rb->strcasecmp(cr, gl.names[sel]) == 0) fl.indices[fl.count++] = i;
            }
        }
        browse_filtered_videos(gl.names[sel], &fl);
    }
}

/* Native Rockbox Browser callback for video files */
static bool video_browse_show_item(char *name, int attr, struct tree_context *tc)
{
    (void)tc;
    if (attr & ATTR_DIRECTORY)
        return true;
    return is_supported_video_file(name);
}

/* Directory selector using do_menu */
#define MAX_FOLDER_ITEMS 64
struct folder_browser_state {
    char current_dir[MAX_PATH];
    char names[MAX_FOLDER_ITEMS][128];
    bool is_dir[MAX_FOLDER_ITEMS];
    int  count;
};

static void populate_folder_state(struct folder_browser_state *st)
{
    st->count = 0;
    rb->strlcpy(st->names[st->count], "[Use This Directory]", sizeof(st->names[0]));
    st->is_dir[st->count] = true;
    st->count++;

    if (rb->strcmp(st->current_dir, "/") != 0 && st->count < MAX_FOLDER_ITEMS) {
        rb->strlcpy(st->names[st->count], ".. (Parent Directory)", sizeof(st->names[0]));
        st->is_dir[st->count] = true;
        st->count++;
    }

    DIR *dir = rb->opendir(st->current_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = rb->readdir(dir)) != NULL && st->count < MAX_FOLDER_ITEMS) {
        const char *name = entry->d_name;
        if (name[0] == '.') continue;
        struct dirinfo info = rb->dir_get_info(dir, entry);
        if (info.attribute & ATTR_DIRECTORY) {
            rb->strlcpy(st->names[st->count], name, sizeof(st->names[0]));
            st->is_dir[st->count] = true;
            st->count++;
        }
    }
    rb->closedir(dir);
}

static char *folder_browser_get_name(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    struct folder_browser_state *st = (struct folder_browser_state *)data;
    if (!st) return "";
    if (selected_item < 0) {
        rb->strlcpy(buffer, st->current_dir, buffer_len);
        return buffer;
    }
    if (selected_item >= st->count)
        return "";
    rb->strlcpy(buffer, st->names[selected_item], buffer_len);
    return buffer;
}

static void run_folder_browser(const char *initial_dir, bool select_dir_mode)
{
    const char *start_root = (initial_dir && initial_dir[0]) ? initial_dir : "/Videos";
    if (!rb->dir_exists(start_root)) {
        rb->mkdir(start_root);
        if (!rb->dir_exists(start_root))
            start_root = "/";
    }

    if (!select_dir_mode) {
        /* Use Rockbox's native file browser (rockbox_browse) to browse & play videos */
        char selected_path[MAX_PATH];
        while (1) {
            selected_path[0] = '\0';
            struct browse_context browse = {
                .dirfilter = SHOW_ALL,
                .flags = BROWSE_SELECTONLY | BROWSE_NO_CONTEXT_MENU | BROWSE_DIRFILTER,
                .title = "Browse Videos",
                .icon = Icon_Folder,
                .root = start_root,
                .buf = selected_path,
                .bufsize = sizeof(selected_path),
                .callback_show_item = video_browse_show_item,
            };
            int res = rb->rockbox_browse(&browse);
            if ((browse.flags & BROWSE_SELECTED) && selected_path[0]) {
                play_single_video(selected_path, false);
                continue;
            }
            if (res == GO_TO_PREVIOUS || res == GO_TO_ROOT || !selected_path[0])
                break;
        }
        return;
    }

    /* Select Video Directory mode using standard Rockbox do_menu */
    static struct folder_browser_state st;
    rb->strlcpy(st.current_dir, start_root, sizeof(st.current_dir));

    while (1) {
        populate_folder_state(&st);
        struct menu_get_name_and_icon folder_dyn = {
            .menu_callback      = NULL,
            .list_get_name      = folder_browser_get_name,
            .list_speak_item    = NULL,
            .list_get_name_data = &st,
            .icon_id            = Icon_Folder,
        };
        struct menu_item_ex folder_menu = {
            .flags = MT_RETURN_ID | MENU_DYNAMIC_DESC | MENU_ITEM_COUNT(st.count),
            .strings = NULL,
            .menu_get_name_and_icon = &folder_dyn,
        };

        int sel = 0;
        int res = rb->do_menu(&folder_menu, &sel, NULL, false);
        if (res < 0 || res >= st.count ||
            res == GO_TO_PREVIOUS || res == GO_TO_ROOT || res == MENU_ATTACHED_USB) {
            break;
        }
        if (res == 0) {
            rb->strlcpy(rb->global_settings->y1_video_dir, st.current_dir,
                        sizeof(rb->global_settings->y1_video_dir));
            rb->settings_save();
            rb->splashf(HZ, "Video Directory: %s", st.current_dir);
            break;
        } else if (rb->strncmp(st.names[res], "..", 2) == 0) {
            char *slash = rb->strrchr(st.current_dir, '/');
            if (slash && slash > st.current_dir) *slash = '\0';
            else rb->strlcpy(st.current_dir, "/", sizeof(st.current_dir));
        } else {
            char next_dir[MAX_PATH];
            if (rb->strcmp(st.current_dir, "/") == 0)
                rb->snprintf(next_dir, sizeof(next_dir), "/%s", st.names[res]);
            else
                rb->snprintf(next_dir, sizeof(next_dir), "%s/%s", st.current_dir, st.names[res]);
            rb->strlcpy(st.current_dir, next_dir, sizeof(st.current_dir));
        }
    }
}

/* =========================================================================
 * Native Rockbox Settings & Videos Menus (MAKE_MENU / MT_SETTING / do_menu)
 * ========================================================================= */
static int menu_act_select_dir(void)
{
    run_folder_browser(get_configured_video_dir(), true);
    return 0;
}

static char *menu_get_video_dir_name(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    (void)selected_item;
    (void)data;
    rb->snprintf(buffer, buffer_len, "%s: %s",
                 rb->str(LANG_Y1_VIDEO_DIR), get_configured_video_dir());
    return buffer;
}

static int menu_act_reset_dir(void)
{
    rb->strlcpy(rb->global_settings->y1_video_dir, "/Videos",
                sizeof(rb->global_settings->y1_video_dir));
    rb->settings_save();
    rb->splash(HZ, ID2P(LANG_RESET_DONE_CLEAR));
    return 0;
}

static int menu_act_scan_db(void)
{
    rebuild_video_database(true);
    return 0;
}

static int video_settings_callback(int action,
                                   const struct menu_item_ex *this_item,
                                   struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_EXIT_MENUITEM) {
        rb->settings_save();
    }
    return action;
}

MENUITEM_FUNCTION_DYNTEXT(vid_set_dir_item, 0, menu_act_select_dir,
                          menu_get_video_dir_name, NULL, NULL, NULL, Icon_Folder);
MENUITEM_FUNCTION(vid_reset_dir_item, 0, ID2P(LANG_RESET),
                  menu_act_reset_dir, NULL, Icon_Menu_functioncall);
static struct menu_item_ex vid_resume_item    = { MT_SETTING, { .variable = NULL }, { video_settings_callback } };
static struct menu_item_ex vid_aspect_item    = { MT_SETTING, { .variable = NULL }, { video_settings_callback } };
static struct menu_item_ex vid_skip_step_item = { MT_SETTING, { .variable = NULL }, { video_settings_callback } };
static struct menu_item_ex vid_autoplay_item  = { MT_SETTING, { .variable = NULL }, { video_settings_callback } };
static struct menu_item_ex vid_thumbs_item    = { MT_SETTING, { .variable = NULL }, { video_settings_callback } };
MENUITEM_FUNCTION(vid_scan_db_item, 0, ID2P(LANG_Y1_VIDEO_SCAN_DB),
                  menu_act_scan_db, NULL, Icon_Menu_functioncall);

MAKE_MENU(video_settings_menu, ID2P(LANG_Y1_VIDEOS),
          video_settings_callback, Icon_Menu_setting,
          &vid_set_dir_item,
          &vid_reset_dir_item,
          &vid_resume_item,
          &vid_aspect_item,
          &vid_skip_step_item,
          &vid_autoplay_item,
          &vid_thumbs_item,
          &vid_scan_db_item);

static void run_video_settings_menu(void)
{
    init_video_menus();
    int selected = 0;
    rb->do_menu(&video_settings_menu, &selected, NULL, false);
    rb->settings_save();
}

/* Root Videos Menu Callbacks */
static char *root_all_videos_get_name(int selected_item, void *data,
                                      char *buffer, size_t buffer_len)
{
    (void)selected_item;
    (void)data;
    if (g_db_loaded) {
        rb->snprintf(buffer, buffer_len, "All Videos (%d)", g_db_count);
        return buffer;
    }
    return "All Videos";
}

static int root_act_all_videos(void)
{
    if (!g_db_loaded) load_video_database();
    if (g_db_count == 0) rebuild_video_database(true);
    static struct filtered_video_list fl;
    fl.count = g_db_count;
    for (int i = 0; i < g_db_count; i++) fl.indices[i] = i;
    browse_filtered_videos("All Videos", &fl);
    return 0;
}

static int root_act_tv_shows(void)
{
    browse_by_group(GROUP_BY_TV_SHOW, "TV Shows");
    return 0;
}

static int root_act_creators(void)
{
    browse_by_group(GROUP_BY_CREATOR, "Creators");
    return 0;
}

static int root_act_years(void)
{
    browse_by_group(GROUP_BY_YEAR, "Years");
    return 0;
}

static int root_act_browse_folder(void)
{
    run_folder_browser(get_configured_video_dir(), false);
    return 0;
}

MENUITEM_FUNCTION_DYNTEXT(root_all_videos_item, 0, root_act_all_videos,
                          root_all_videos_get_name, NULL, NULL, NULL, Icon_Submenu);
MENUITEM_FUNCTION(root_tv_shows_item, 0, "TV Shows",
                  root_act_tv_shows, NULL, Icon_Submenu);
MENUITEM_FUNCTION(root_creators_item, 0, "Creators",
                  root_act_creators, NULL, Icon_Submenu);
MENUITEM_FUNCTION(root_years_item, 0, "Years",
                  root_act_years, NULL, Icon_Submenu);
MENUITEM_FUNCTION(root_browse_folder_item, 0, "Browse Videos",
                  root_act_browse_folder, NULL, Icon_Folder);
MENUITEM_FUNCTION(root_update_db_item, 0, ID2P(LANG_Y1_VIDEO_SCAN_DB),
                  menu_act_scan_db, NULL, Icon_Menu_functioncall);

MAKE_MENU(videos_root_menu, ID2P(LANG_Y1_VIDEOS), NULL, Icon_Submenu_Entered,
          &root_all_videos_item,
          &root_tv_shows_item,
          &root_creators_item,
          &root_years_item,
          &root_browse_folder_item,
          &root_update_db_item,
          &video_settings_menu);

static void init_video_menus(void)
{
    vid_resume_item.variable    = &rb->global_settings->y1_video_resume;
    vid_aspect_item.variable    = &rb->global_settings->y1_video_aspect;
    vid_skip_step_item.variable = &rb->global_settings->y1_video_skip_step;
    vid_autoplay_item.variable  = &rb->global_settings->y1_video_autoplay_next;
    vid_thumbs_item.variable    = &rb->global_settings->y1_video_show_thumbs;
}

static void run_videos_library_menu(void)
{
    init_video_menus();
    if (!load_video_database()) {
        rebuild_video_database(false);
    }
    int selected = 0;
    rb->do_menu(&videos_root_menu, &selected, NULL, false);
}

/* =========================================================================
 * Plugin Entry Point
 * ========================================================================= */
enum plugin_status plugin_start(const void *parameter)
{
    init_video_menus();

    if (!rb->global_settings->y1_video_dir[0]) {
        rb->strlcpy(rb->global_settings->y1_video_dir, "/Videos",
                    sizeof(rb->global_settings->y1_video_dir));
    }

    const char *arg = (const char *)parameter;
    if (arg && arg[0]) {
        if (rb->strcmp(arg, "--settings") == 0) {
            run_video_settings_menu();
            return PLUGIN_OK;
        } else if (rb->strcmp(arg, "--scan-db") == 0) {
            rebuild_video_database(true);
            return PLUGIN_OK;
        } else if (rb->strcmp(arg, "--select-dir") == 0) {
            run_folder_browser(get_configured_video_dir(), true);
            return PLUGIN_OK;
        } else if (rb->strcmp(arg, "--library") == 0) {
            run_videos_library_menu();
            return PLUGIN_OK;
        } else if (arg[0] == '/') {
            /* Direct file path from File Browser -> Play Video immediately */
            play_single_video(arg, false);
            return PLUGIN_OK;
        }
    }

    /* Opened from Main Menu ("Videos") or Plugins menu -> Open Rockbox Videos Menu */
    run_videos_library_menu();
    return PLUGIN_OK;
}

