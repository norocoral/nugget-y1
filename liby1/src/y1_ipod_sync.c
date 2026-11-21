/***************************************************************************
 * liby1 - Nugget Y1 Native iTunesDB / ArtworkDB / Play Counts Engine
 *
 * Reads Apple iTunes/iPod 5.5G files (/iPod_Control) directly in memory
 * and writes live Play Counts (mhdp) & OTGPlaylistInfo (mhpo) without
 * translating databases on disk.
 *
 * Copyright (C) 2026 Nugget Y1
 ****************************************************************************/

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "liby1.h"
#include "ipod_sysinfo_5g.h"

typedef void (*y1_ipod_progress_cb)(const char *stage, int done, int total, void *user);

/* =========================================================================
 * Path Constants
 * ========================================================================= */

#define SD_ROOT             "/mnt/sdcard"
#define IPOD_CONTROL_DIR    SD_ROOT "/iPod_Control"
#define IPOD_DEVICE_DIR     IPOD_CONTROL_DIR "/Device"
#define IPOD_ITUNES_DIR     IPOD_CONTROL_DIR "/iTunes"
#define IPOD_MUSIC_DIR      IPOD_CONTROL_DIR "/Music"
#define IPOD_ARTWORK_DIR    IPOD_CONTROL_DIR "/Artwork"
#define IPOD_SYSINFO_PATH   IPOD_DEVICE_DIR "/SysInfo"
#define IPOD_SYSEXT_PATH    IPOD_DEVICE_DIR "/SysInfoExtended"
#define IPOD_ITUNESDB_PATH  IPOD_ITUNES_DIR "/iTunesDB"
#define IPOD_PLAYCOUNTS_PATH IPOD_ITUNES_DIR "/Play Counts"
#define IPOD_OTG_INFO_PATH  IPOD_ITUNES_DIR "/OTGPlaylistInfo"
#define IPOD_ARTWORKDB_PATH IPOD_ARTWORK_DIR "/ArtworkDB"

#define RB_DIR              SD_ROOT "/.rockbox"
#define RB_ALBUMART_DIR     RB_DIR "/albumart"
#define RB_VIDTHUMBS_DIR    RB_DIR "/video_thumbs"
#define RB_VIDEO_DB_PATH    RB_DIR "/video_db.dat"
#define RB_SYNC_STATE_PATH  RB_DIR "/ipod_sync_state.dat"
#define RB_OTG_MARK_PATH    RB_DIR "/itunesdb_otg.dat"
#define RB_PLAYLISTS_DIR    SD_ROOT "/Playlists"
#define RB_VIDEOS_DIR       SD_ROOT "/Videos"
#define RB_FAVORITES_PATH   RB_DIR "/favorites.m3u8"
#define RB_OTG_PLAYLIST_M3U8 RB_PLAYLISTS_DIR "/On-The-Go.m3u8"
#define RB_OTG_PLAYLIST_M3U  RB_PLAYLISTS_DIR "/On-The-Go.m3u"

#define IPOD_DEFAULT_SERIAL "8K7289LAV9K"
#define IPOD_DEFAULT_GUID   "000a270019b48c1f"

/* Mac HFS+ epoch (1904-01-01) minus Unix epoch (1970-01-01) in seconds */
#define MAC_EPOCH_OFFSET    2082844800U
#define LASTPLAYED_TIME_MIN 0x40000000U

/* Rockbox TagCache constants (must match apps/tagcache.h / tagcache.c) */
#define RB_TAGCACHE_MAGIC   0x54434811
#define RB_TAG_COUNT        24
#define RB_TAG_CHUNK        8

enum rb_tag_type {
    RB_TAG_ARTIST = 0,
    RB_TAG_ALBUM = 1,
    RB_TAG_GENRE = 2,
    RB_TAG_TITLE = 3,
    RB_TAG_FILENAME = 4,
    RB_TAG_COMPOSER = 5,
    RB_TAG_COMMENT = 6,
    RB_TAG_ALBUMARTIST = 7,
    RB_TAG_GROUPING = 8,
    RB_TAG_YEAR = 9,
    RB_TAG_DISCNUMBER = 10,
    RB_TAG_TRACKNUMBER = 11,
    RB_TAG_CANONICALARTIST = 12,
    RB_TAG_BITRATE = 13,
    RB_TAG_LENGTH = 14,
    RB_TAG_PLAYCOUNT = 15,
    RB_TAG_RATING = 16,
    RB_TAG_PLAYTIME = 17,
    RB_TAG_LASTPLAYED = 18,
    RB_TAG_COMMITID = 19,
    RB_TAG_MTIME = 20,
    RB_TAG_LASTELAPSED = 21,
    RB_TAG_LASTOFFSET = 22,
    RB_TAG_FAVORITE = 23
};

struct rb_tagcache_header {
    int32_t magic;
    int32_t datasize;
    int32_t entry_count;
};

struct rb_master_header {
    struct rb_tagcache_header tch;
    int32_t serial;
    int32_t commitid;
    int32_t dirty;
};

struct rb_index_entry {
    int32_t tag_seek[RB_TAG_COUNT];
    int32_t flag;
};

struct rb_tagfile_entry {
    int32_t tag_length;
    int32_t idx_id;
};

/* Y1 Video DB constants (must match apps/plugins/y1_video.c) */
#define Y1_VIDEO_DB_MAGIC   0x59315644U
#define Y1_VIDEO_DB_VERSION 1
#define Y1_MAX_DB_VIDEOS    256
#define Y1VID_THUMB_W       96
#define Y1VID_THUMB_H       72

struct y1_video_db_header {
    uint32_t magic;
    uint32_t version;
    int32_t  count;
};

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

/* Sync snapshot state saved across USB connections */
#define SYNC_STATE_MAGIC    0x59315353U /* "Y1SS" */
#define SYNC_STATE_VERSION  1

struct sync_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t db_size;
    uint32_t db_mtime;
    uint32_t track_count;
    uint32_t shows_count;
    uint32_t playcounts_pending;
};

struct sync_state_entry {
    uint64_t dbid;
    uint64_t mhit_offset;
    uint32_t playcount;
    uint32_t lastplayed_mac;
    uint32_t bookmark_ms;
    uint32_t media_type;
    int32_t  base_tc_lastplayed;
    uint8_t  rating;
    uint8_t  played_mark;
    uint16_t reserved;
    char     path[256];
};

#define OTG_MARK_MAGIC 0x4f544731U /* "OTG1" */

struct otg_mark_header {
    uint32_t magic;
    uint32_t size;
    uint32_t mtime;
    uint32_t db_size;
    uint32_t db_mtime;
    uint32_t count;
};

/* =========================================================================
 * Endianness & Binary Primitives
 * ========================================================================= */

static inline uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t rd64le(const uint8_t *p)
{
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

static inline uint16_t rd16be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t rd64be(const uint8_t *p)
{
    return ((uint64_t)rd32be(p) << 32) | (uint64_t)rd32be(p + 4);
}

static inline void wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline void wr64le(uint8_t *p, uint64_t v)
{
    wr32le(p, (uint32_t)(v & 0xffffffffU));
    wr32le(p + 4, (uint32_t)(v >> 32));
}

static void report_progress(y1_ipod_progress_cb cb, void *user,
                            const char *stage, int done, int total)
{
    if (cb)
        cb(stage, done, total, user);
}

/* =========================================================================
 * Filesystem & Path Utilities
 * ========================================================================= */

static bool mkdir_p(const char *path)
{
    char tmp[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return false;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return false;
    return true;
}

static bool write_full_fd(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += (size_t)w;
        left -= (size_t)w;
    }
    return true;
}

static bool write_file_atomic(const char *path, const void *buf, size_t len)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    bool ok = write_full_fd(fd, buf, len);
    if (ok && fsync(fd) != 0 && errno != EINVAL)
        ok = false;
    close(fd);
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static uint8_t *read_entire_file(const char *path, size_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 128 * 1024 * 1024) {
        close(fd);
        return NULL;
    }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(sz + 4);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    if (got != sz) {
        free(buf);
        return NULL;
    }
    memset(buf + sz, 0, 4);
    if (out_size)
        *out_size = sz;
    return buf;
}

static void ensure_root_symlinks(void)
{
    static const struct {
        const char *target;
        const char *link_path;
    } links[] = {
        { "/mnt/sdcard/iPod_Control", "/iPod_Control" },
        { "/mnt/sdcard/.rockbox",     "/.rockbox" },
        { "/mnt/sdcard/Playlists",    "/Playlists" },
        { "/mnt/sdcard/Videos",       "/Videos" },
    };

    for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
        struct stat st;
        if (stat(links[i].target, &st) == 0) {
            if (lstat(links[i].link_path, &st) != 0) {
                (void)!symlink(links[i].target, links[i].link_path);
            }
        }
    }
}

static void sd_to_real_path(const char *rb_path, char *out, size_t out_sz)
{
    if (strncmp(rb_path, "/mnt/sdcard/", 12) == 0) {
        snprintf(out, out_sz, "%s", rb_path);
    } else if (rb_path[0] == '/') {
        snprintf(out, out_sz, "%s%s", SD_ROOT, rb_path);
    } else {
        snprintf(out, out_sz, "%s/%s", SD_ROOT, rb_path);
    }
}

static uint32_t unix_to_fat_mtime(time_t t)
{
    if (t <= 315532800) /* 1980-01-01 */
        return 0;
    struct tm tm_buf;
    if (!localtime_r(&t, &tm_buf))
        return 0;
    int yr = tm_buf.tm_year + 1900;
    if (yr < 1980)
        yr = 1980;
    return ((uint32_t)(yr - 1980) << 25) |
           ((uint32_t)(tm_buf.tm_mon + 1) << 21) |
           ((uint32_t)tm_buf.tm_mday << 16) |
           ((uint32_t)tm_buf.tm_hour << 11) |
           ((uint32_t)tm_buf.tm_min << 5) |
           ((uint32_t)(tm_buf.tm_sec >> 1));
}

static bool has_video_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    return (strcasecmp(dot, ".m4v") == 0 ||
            strcasecmp(dot, ".mp4") == 0 ||
            strcasecmp(dot, ".mov") == 0 ||
            strcasecmp(dot, ".avi") == 0 ||
            strcasecmp(dot, ".mkv") == 0 ||
            strcasecmp(dot, ".mpg") == 0 ||
            strcasecmp(dot, ".mpeg") == 0);
}

/* =========================================================================
 * Device Identification Files (SysInfo & SysInfoExtended)
 * ========================================================================= */

static void read_sysinfo_ids(char *serial_out, size_t serial_sz,
                             char *guid_out, size_t guid_sz)
{
    snprintf(serial_out, serial_sz, "%s", IPOD_DEFAULT_SERIAL);
    snprintf(guid_out, guid_sz, "%s", IPOD_DEFAULT_GUID);

    FILE *fp = fopen(IPOD_SYSINFO_PATH, "r");
    if (!fp)
        return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' '  || line[len - 1] == '\t'))
            line[--len] = '\0';

        if (strncmp(line, "pszSerialNumber:", 16) == 0) {
            const char *p = line + 16;
            while (*p == ' ' || *p == '\t') p++;
            if (*p)
                snprintf(serial_out, serial_sz, "%.*s", (int)(serial_sz > 0 ? serial_sz - 1 : 0), p);
        } else if (strncmp(line, "FirewireGuid:", 13) == 0) {
            const char *p = line + 13;
            while (*p == ' ' || *p == '\t') p++;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
                p += 2;
            char hex[24];
            size_t n = 0;
            while (n + 1 < sizeof(hex) && isxdigit((unsigned char)p[n])) {
                hex[n] = (char)toupper((unsigned char)p[n]);
                n++;
            }
            hex[n] = '\0';
            if (n >= 8)
                snprintf(guid_out, guid_sz, "%.*s", (int)(guid_sz > 0 ? guid_sz - 1 : 0), hex);
        }
    }
    fclose(fp);
}

static char *render_xml_template(const char *tmpl,
                                 const char *serial,
                                 const char *guid,
                                 const char *ram,
                                 size_t *out_len)
{
    size_t max_len = strlen(tmpl) + 256;
    char *buf = (char *)malloc(max_len);
    if (!buf)
        return NULL;

    char *dst = buf;
    const char *src = tmpl;
    while (*src) {
        if (*src == '@') {
            if (strncmp(src, "@SERIAL@", 8) == 0) {
                size_t l = strlen(serial);
                memcpy(dst, serial, l);
                dst += l;
                src += 8;
                continue;
            }
            if (strncmp(src, "@GUID@", 6) == 0) {
                size_t l = strlen(guid);
                memcpy(dst, guid, l);
                dst += l;
                src += 6;
                continue;
            }
            if (strncmp(src, "@RAM@", 5) == 0) {
                size_t l = strlen(ram);
                memcpy(dst, ram, l);
                dst += l;
                src += 5;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    *out_len = (size_t)(dst - buf);
    return buf;
}

bool y1_ipod_ensure_device_files(void)
{
    mkdir_p(SD_ROOT "/.system");
    mkdir_p(IPOD_DEVICE_DIR);
    mkdir_p(IPOD_ITUNES_DIR);
    mkdir_p(IPOD_MUSIC_DIR);
    mkdir_p(IPOD_ARTWORK_DIR);
    mkdir_p(RB_ALBUMART_DIR);
    mkdir_p(RB_VIDTHUMBS_DIR);
    mkdir_p(RB_PLAYLISTS_DIR);
    mkdir_p(RB_VIDEOS_DIR);

    for (int i = 0; i < 20; i++) {
        char fdir[256];
        snprintf(fdir, sizeof(fdir), "%s/F%02d", IPOD_MUSIC_DIR, i);
        mkdir_p(fdir);
    }

    ensure_root_symlinks();

    char serial[64], guid[32];
    read_sysinfo_ids(serial, sizeof(serial), guid, sizeof(guid));

    struct stat st;
    bool need_sysinfo = (stat(IPOD_SYSINFO_PATH, &st) != 0 || st.st_size < 160);
    if (!need_sysinfo) {
        FILE *cfp = fopen(IPOD_SYSINFO_PATH, "r");
        if (cfp) {
            char check_buf[512];
            size_t cr = fread(check_buf, 1, sizeof(check_buf) - 1, cfp);
            check_buf[cr] = '\0';
            fclose(cfp);
            if (strstr(check_buf, "visibleBuildID") == NULL ||
                strstr(check_buf, "boardHwSwInterfaceRev") == NULL) {
                need_sysinfo = true;
            }
        } else {
            need_sysinfo = true;
        }
    }

    if (need_sysinfo) {
        char sysinfo_txt[1024];
        snprintf(sysinfo_txt, sizeof(sysinfo_txt),
            "BoardHwName: iPod M25\r\n"
            "pszSerialNumber: %s\r\n"
            "ModelNumStr: MA450\r\n"
            "FirewireGuid: 0x%s\r\n"
            "HddFirmwareRev: BU011A\r\n"
            "RegionCode: FB(0x001A)\r\n"
            "PolicyFlags: 0x00000000\r\n"
            "buildID: 0x06118000 (6.1.1)\r\n"
            "visibleBuildID: 0x01128000 (1.2.1)\r\n"
            "boardHwRev: 0x00000000 (0.0 0)\r\n"
            "boardHwSwInterfaceRev: 0x000C0005 (0.0.12 5)\r\n"
            "bootLoaderImageRev: 0x00000000 (0.0 0)\r\n"
            "diskModeImageRev: 0x00000000 (0.0 0)\r\n"
            "diagImageRev: 0x00000000 (0.0 0)\r\n"
            "osImageRev: 0x00000000 (0.0 0)\r\n"
            "iPodFamily: 0x00000006\r\n"
            "updaterFamily: 0x00000006\r\n",
            serial, guid);
        if (!write_file_atomic(IPOD_SYSINFO_PATH, sysinfo_txt, strlen(sysinfo_txt))) {
            y1_logf("[ipod_sync] Failed writing %s\n", IPOD_SYSINFO_PATH);
        }
    }

    if (stat(IPOD_SYSEXT_PATH, &st) != 0 || st.st_size < 256) {
        size_t xlen = 0;
        char *xml = render_xml_template(ipod_xml_template, serial, guid, "64", &xlen);
        if (xml) {
            if (!write_file_atomic(IPOD_SYSEXT_PATH, xml, xlen)) {
                y1_logf("[ipod_sync] Failed writing %s\n", IPOD_SYSEXT_PATH);
            }
            free(xml);
        }
    }

    return true;
}

/* =========================================================================
 * UTF-16 & mhod String Decoding
 * ========================================================================= */

static size_t append_utf8(uint32_t cp, char *out, size_t out_sz, size_t pos)
{
    if (cp == 0)
        return pos;
    if (cp < 0x80) {
        if (pos + 1 < out_sz)
            out[pos++] = (char)cp;
    } else if (cp < 0x800) {
        if (pos + 2 < out_sz) {
            out[pos++] = (char)(0xc0U | (cp >> 6));
            out[pos++] = (char)(0x80U | (cp & 0x3fU));
        }
    } else if (cp < 0x10000) {
        if (pos + 3 < out_sz) {
            out[pos++] = (char)(0xe0U | (cp >> 12));
            out[pos++] = (char)(0x80U | ((cp >> 6) & 0x3fU));
            out[pos++] = (char)(0x80U | (cp & 0x3fU));
        }
    } else if (cp <= 0x10ffffU) {
        if (pos + 4 < out_sz) {
            out[pos++] = (char)(0xf0U | (cp >> 18));
            out[pos++] = (char)(0x80U | ((cp >> 12) & 0x3fU));
            out[pos++] = (char)(0x80U | ((cp >> 6) & 0x3fU));
            out[pos++] = (char)(0x80U | (cp & 0x3fU));
        }
    }
    return pos;
}

static void decode_utf16(const uint8_t *src, size_t bytes, bool big_endian,
                         char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return;
    size_t pos = 0;
    size_t i = 0;
    while (i + 1 < bytes && pos + 1 < out_sz) {
        uint16_t w = big_endian ? rd16be(src + i) : rd16le(src + i);
        i += 2;
        if (w == 0)
            break;
        uint32_t cp = w;
        if (w >= 0xd800U && w <= 0xdbffU && i + 1 < bytes) {
            uint16_t w2 = big_endian ? rd16be(src + i) : rd16le(src + i);
            if (w2 >= 0xdc00U && w2 <= 0xdfffU) {
                cp = 0x10000U + (((uint32_t)(w - 0xd800U) << 10) | (uint32_t)(w2 - 0xdc00U));
                i += 2;
            }
        }
        pos = append_utf8(cp, out, out_sz, pos);
    }
    out[pos] = '\0';
}

static void decode_mhod_string(const uint8_t *p, size_t avail, char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if (avail < 40)
        return;

    uint32_t total = rd32le(p + 8);
    if (total > avail)
        total = (uint32_t)avail;
    if (total <= 40)
        return;

    uint32_t enc = rd32le(p + 24);
    uint32_t slen = rd32le(p + 28);
    if (slen > total - 40)
        slen = total - 40;
    if (slen == 0)
        return;

    const uint8_t *str_data = p + 40;
    /* Per iOpenPod MhodStringPrefix: enc=1 is UTF-16LE, enc=2 is UTF-8 */
    bool is_utf16 = (enc == 1) || (enc != 2 && slen >= 2 && str_data[1] == 0);
    if (is_utf16) {
        decode_utf16(str_data, slen, false, out, out_sz);
    } else {
        /* Check whether str_data is valid UTF-8; if not, convert ISO-8859-1 -> UTF-8 */
        bool valid_utf8 = true;
        for (size_t i = 0; i < slen; ) {
            uint8_t c = str_data[i];
            if (c < 0x80) {
                i++;
            } else if ((c & 0xE0) == 0xC0 && i + 1 < slen &&
                       (str_data[i + 1] & 0xC0) == 0x80) {
                i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < slen &&
                       (str_data[i + 1] & 0xC0) == 0x80 &&
                       (str_data[i + 2] & 0xC0) == 0x80) {
                i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < slen &&
                       (str_data[i + 1] & 0xC0) == 0x80 &&
                       (str_data[i + 2] & 0xC0) == 0x80 &&
                       (str_data[i + 3] & 0xC0) == 0x80) {
                i += 4;
            } else {
                valid_utf8 = false;
                break;
            }
        }
        if (valid_utf8) {
            size_t n = slen < out_sz - 1 ? slen : out_sz - 1;
            memcpy(out, str_data, n);
            out[n] = '\0';
        } else {
            size_t pos = 0;
            for (size_t i = 0; i < slen && pos + 1 < out_sz; i++)
                pos = append_utf8(str_data[i], out, out_sz, pos);
            out[pos] = '\0';
        }
    }
}

static char *dup_or_empty(const char *s)
{
    if (!s)
        s = "";
    size_t len = strlen(s);
    char *r = (char *)malloc(len + 1);
    if (r)
        memcpy(r, s, len + 1);
    return r;
}

/* =========================================================================
 * Parsed iTunesDB Representation
 * ========================================================================= */

struct itdb_track {
    uint64_t dbid;
    uint64_t mhit_offset;
    uint32_t id;
    uint32_t art_id;
    uint32_t length_ms;
    uint32_t bitrate;
    uint32_t playcount;
    uint32_t lastplayed_mac;
    uint32_t added_mac;
    uint32_t released_mac;
    uint32_t bookmark_ms;
    uint32_t media_type;
    uint32_t mhit_flags;
    uint16_t season;
    uint16_t episode;
    uint16_t year;
    uint16_t tracknum;
    uint16_t discnum;
    uint8_t  rating_ipod;  /* 0..100 */
    uint8_t  played_mark;
    bool     is_video;
    bool     is_favorite;

    char *path;        /* Rockbox virtual path, e.g. /iPod_Control/Music/F00/XXXX.mp3 */
    char *title;
    char *artist;
    char *album;
    char *albumartist;
    char *genre;
    char *composer;
    char *comment;
    char *grouping;
    char *tv_show;
};

struct itdb_id_map {
    uint32_t id;
    int      index;
};

struct itdb_parsed {
    uint32_t db_size;
    uint32_t db_mtime;
    uint32_t shows_count;

    struct itdb_track *tracks;
    int track_count;
    int track_cap;

    struct itdb_id_map *by_id;

    const uint8_t *raw;
    size_t raw_size;
    int64_t playlists_off;
    int64_t podcasts_off;
};

static void itdb_free(struct itdb_parsed *db)
{
    if (db->tracks) {
        for (int i = 0; i < db->track_count; i++) {
            free(db->tracks[i].path);
            free(db->tracks[i].title);
            free(db->tracks[i].artist);
            free(db->tracks[i].album);
            free(db->tracks[i].albumartist);
            free(db->tracks[i].genre);
            free(db->tracks[i].composer);
            free(db->tracks[i].comment);
            free(db->tracks[i].grouping);
            free(db->tracks[i].tv_show);
        }
        free(db->tracks);
    }
    free(db->by_id);
    memset(db, 0, sizeof(*db));
}

static int cmp_id_map(const void *a, const void *b)
{
    const struct itdb_id_map *ia = (const struct itdb_id_map *)a;
    const struct itdb_id_map *ib = (const struct itdb_id_map *)b;
    if (ia->id < ib->id) return -1;
    if (ia->id > ib->id) return 1;
    return 0;
}

static int itdb_find_by_id(const struct itdb_parsed *db, uint32_t id)
{
    int lo = 0, hi = db->track_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t mid_id = db->by_id[mid].id;
        if (mid_id == id)
            return db->by_id[mid].index;
        if (mid_id < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static void normalize_ipod_track_path(const char *raw_path, char *out, size_t out_sz)
{
    if (!raw_path || !raw_path[0]) {
        out[0] = '\0';
        return;
    }
    size_t j = 0;
    if (raw_path[0] != ':' && raw_path[0] != '/') {
        if (out_sz > 1)
            out[j++] = '/';
    }
    for (size_t i = 0; raw_path[i] && j + 1 < out_sz; i++) {
        char c = raw_path[i];
        out[j++] = (c == ':') ? '/' : c;
    }
    out[j] = '\0';
}

static uint32_t count_podcast_shows(const uint8_t *buf, size_t sz, int64_t list_off)
{
    if (list_off < 0 || (size_t)list_off + 12 > sz)
        return 0;
    const uint8_t *mhlp = buf + list_off;
    if (memcmp(mhlp, "mhlp", 4) != 0)
        return 0;

    uint32_t hlen = rd32le(mhlp + 4);
    uint32_t lists = rd32le(mhlp + 8);
    if (hlen < 12)
        return 0;

    size_t pos = (size_t)list_off + hlen;
    uint32_t shows = 0;

    for (uint32_t i = 0; i < lists && pos + 44 <= sz; i++) {
        const uint8_t *mhyp = buf + pos;
        if (memcmp(mhyp, "mhyp", 4) != 0)
            break;
        uint32_t phlen = rd32le(mhyp + 4);
        uint32_t ptotal = rd32le(mhyp + 8);
        if (ptotal == 0 || pos + ptotal > sz || phlen < 44)
            break;

        if (mhyp[42] | mhyp[43]) {
            uint32_t mhods = rd32le(mhyp + 12);
            uint32_t items = rd32le(mhyp + 16);
            size_t child = pos + phlen;
            for (uint32_t k = 0; k < mhods + items && child + 20 <= pos + ptotal; k++) {
                const uint8_t *cp = buf + child;
                const char *tag = (k < mhods) ? "mhod" : "mhip";
                if (memcmp(cp, tag, 4) != 0)
                    break;
                uint32_t ctotal = rd32le(cp + 8);
                if (ctotal == 0 || child + ctotal > pos + ptotal)
                    break;
                if (k >= mhods && rd32le(cp + 16) == 0x100U)
                    shows++;
                child += ctotal;
            }
        }
        pos += ptotal;
    }
    return shows;
}

static bool parse_itunesdb(const uint8_t *buf, size_t sz,
                           uint32_t db_mtime, struct itdb_parsed *db,
                           y1_ipod_progress_cb cb, void *user)
{
    memset(db, 0, sizeof(*db));
    db->raw = buf;
    db->raw_size = sz;
    db->db_size = (uint32_t)sz;
    db->db_mtime = db_mtime;
    db->playlists_off = -1;
    db->podcasts_off = -1;

    if (sz < 24 || memcmp(buf, "mhbd", 4) != 0)
        return false;

    uint32_t bd_hdr = rd32le(buf + 4);
    uint32_t sections = rd32le(buf + 20);
    if (bd_hdr < 24 || bd_hdr > sz)
        return false;

    int64_t tracks_off = -1;
    size_t pos = bd_hdr;
    for (uint32_t s = 0; s < sections && pos + 16 <= sz; s++) {
        const uint8_t *mhsd = buf + pos;
        if (memcmp(mhsd, "mhsd", 4) != 0)
            break;
        uint32_t shdr = rd32le(mhsd + 4);
        uint32_t stotal = rd32le(mhsd + 8);
        uint32_t stype = rd32le(mhsd + 12);
        if (stotal == 0 || pos + stotal > sz || shdr > stotal)
            break;

        if (stype == 1)
            tracks_off = (int64_t)(pos + shdr);
        else if (stype == 2)
            db->playlists_off = (int64_t)(pos + shdr);
        else if (stype == 3)
            db->podcasts_off = (int64_t)(pos + shdr);

        pos += stotal;
    }

    if (tracks_off < 0 || (size_t)tracks_off + 12 > sz)
        return false;

    const uint8_t *mhlt = buf + tracks_off;
    if (memcmp(mhlt, "mhlt", 4) != 0)
        return false;
    uint32_t thdr = rd32le(mhlt + 4);
    uint32_t tcount = rd32le(mhlt + 8);
    if (thdr < 12)
        return false;

    db->tracks = (struct itdb_track *)calloc(tcount > 0 ? tcount : 1, sizeof(struct itdb_track));
    if (!db->tracks)
        return false;
    db->track_cap = (int)(tcount > 0 ? tcount : 1);

    size_t tpos = (size_t)tracks_off + thdr;
    char sbuf[1024];

    for (uint32_t i = 0; i < tcount && tpos + 40 <= sz; i++) {
        if ((i & 63U) == 0)
            report_progress(cb, user, "Reading iTunesDB...", (int)i, (int)tcount);

        const uint8_t *mhit = buf + tpos;
        if (memcmp(mhit, "mhit", 4) != 0)
            break;

        uint32_t hlen = rd32le(mhit + 4);
        uint32_t total = rd32le(mhit + 8);
        uint32_t mhods = rd32le(mhit + 12);
        if (hlen < 100 || total < hlen || tpos + total > sz)
            break;

        struct itdb_track *tr = &db->tracks[db->track_count];
        memset(tr, 0, sizeof(*tr));
        tr->mhit_offset = (uint64_t)tpos;
        tr->id = rd32le(mhit + 16);
        tr->rating_ipod = mhit[31];
        if (tr->rating_ipod > 100)
            tr->rating_ipod = 100;
        tr->length_ms = rd32le(mhit + 40);
        tr->tracknum = (uint16_t)rd32le(mhit + 44);
        tr->year = (uint16_t)rd32le(mhit + 52);
        tr->bitrate = rd32le(mhit + 56);
        tr->playcount = rd32le(mhit + 80);
        tr->lastplayed_mac = rd32le(mhit + 0x58);
        tr->discnum = (uint16_t)rd32le(mhit + 92);
        if (hlen >= 120) {
            tr->added_mac = rd32le(mhit + 0x68);
            tr->bookmark_ms = rd32le(mhit + 0x6c);
            tr->dbid = rd64le(mhit + 0x70);
        }
        if (hlen >= 144)
            tr->released_mac = rd32le(mhit + 0x8c);
        if (hlen >= 180)
            tr->played_mark = mhit[0xb2];
        if (hlen >= 212)
            tr->media_type = rd32le(mhit + 208);
        if (hlen >= 224) {
            tr->season = (uint16_t)rd32le(mhit + 212);
            tr->episode = (uint16_t)rd32le(mhit + 216);
        }
        if (hlen >= 356)
            tr->art_id = rd32le(mhit + 352);

        size_t mpos = tpos + hlen;
        for (uint32_t m = 0; m < mhods && mpos + 16 <= tpos + total; m++) {
            const uint8_t *mhod = buf + mpos;
            if (memcmp(mhod, "mhod", 4) != 0)
                break;
            uint32_t mtotal = rd32le(mhod + 8);
            uint32_t mtype = rd32le(mhod + 12);
            if (mtotal == 0 || mpos + mtotal > tpos + total)
                break;

            if (mtype == 1 || mtype == 2 || mtype == 3 || mtype == 4 ||
                mtype == 5 || mtype == 8 || mtype == 12 || mtype == 13 ||
                mtype == 22 || mtype == 24) {
                decode_mhod_string(mhod, mtotal, sbuf, sizeof(sbuf));
                switch (mtype) {
                case 1:  tr->title = dup_or_empty(sbuf); break;
                case 2: {
                    char norm[512];
                    normalize_ipod_track_path(sbuf, norm, sizeof(norm));
                    tr->path = dup_or_empty(norm);
                    break;
                }
                case 3:  tr->album = dup_or_empty(sbuf); break;
                case 4:  tr->artist = dup_or_empty(sbuf); break;
                case 5:  tr->genre = dup_or_empty(sbuf); break;
                case 8:  tr->comment = dup_or_empty(sbuf); break;
                case 12: tr->composer = dup_or_empty(sbuf); break;
                case 13: tr->grouping = dup_or_empty(sbuf); break;
                case 22: tr->albumartist = dup_or_empty(sbuf); break;
                case 24: tr->tv_show = dup_or_empty(sbuf); break;
                default: break;
                }
            }
            mpos += mtotal;
        }

        if (!tr->path) tr->path = dup_or_empty("");
        if (!tr->title) tr->title = dup_or_empty("");
        if (!tr->artist) tr->artist = dup_or_empty("");
        if (!tr->album) tr->album = dup_or_empty("");
        if (!tr->albumartist) tr->albumartist = dup_or_empty("");
        if (!tr->genre) tr->genre = dup_or_empty("");
        if (!tr->composer) tr->composer = dup_or_empty("");
        if (!tr->comment) tr->comment = dup_or_empty("");
        if (!tr->grouping) tr->grouping = dup_or_empty("");
        if (!tr->tv_show) tr->tv_show = dup_or_empty("");

        if ((tr->media_type & (0x02U | 0x20U | 0x40U)) != 0 ||
            has_video_extension(tr->path)) {
            tr->is_video = true;
        }

        db->track_count++;
        tpos += total;
    }

    db->shows_count = count_podcast_shows(buf, sz, db->podcasts_off);

    db->by_id = (struct itdb_id_map *)malloc((size_t)(db->track_count > 0 ? db->track_count : 1) *
                                             sizeof(struct itdb_id_map));
    if (!db->by_id) {
        itdb_free(db);
        return false;
    }
    for (int i = 0; i < db->track_count; i++) {
        db->by_id[i].id = db->tracks[i].id;
        db->by_id[i].index = i;
    }
    qsort(db->by_id, (size_t)db->track_count, sizeof(struct itdb_id_map), cmp_id_map);
    report_progress(cb, user, "Reading iTunesDB...", db->track_count, db->track_count);
    return true;
}

/* =========================================================================
 * ArtworkDB + F*.ithmb Extraction -> Rockbox Album Art .bmp & Video Thumbs
 * ========================================================================= */

struct art_slot {
    uint32_t art_id;
    uint64_t song_dbid;
    uint32_t ithmb_offset;
    uint32_t image_size;
    uint16_t width;
    uint16_t height;
    int16_t  hpad;
    int16_t  vpad;
    char     ithmb_rel[64];
};

static int cmp_art_id(const void *a, const void *b)
{
    const struct art_slot *sa = (const struct art_slot *)a;
    const struct art_slot *sb = (const struct art_slot *)b;
    if (sa->art_id < sb->art_id) return -1;
    if (sa->art_id > sb->art_id) return 1;
    return 0;
}

static const struct art_slot *find_art_slot(const struct art_slot *slots, int count,
                                            uint32_t art_id, uint64_t song_dbid)
{
    if (art_id != 0 && count > 0) {
        int lo = 0, hi = count - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (slots[mid].art_id == art_id)
                return &slots[mid];
            if (slots[mid].art_id < art_id)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }
    if (song_dbid != 0) {
        for (int i = 0; i < count; i++) {
            if (slots[i].song_dbid == song_dbid)
                return &slots[i];
        }
    }
    return NULL;
}

static struct art_slot *parse_artwork_db(int *out_count)
{
    *out_count = 0;
    size_t sz = 0;
    uint8_t *buf = read_entire_file(IPOD_ARTWORKDB_PATH, &sz);
    if (!buf)
        return NULL;

    if (sz < 24 || memcmp(buf, "mhfd", 4) != 0) {
        free(buf);
        return NULL;
    }

    uint32_t fhdr = rd32le(buf + 4);
    uint32_t sections = rd32le(buf + 20);
    size_t pos = fhdr;
    const uint8_t *mhli = NULL;
    size_t mhli_sz = 0;

    for (uint32_t s = 0; s < sections && pos + 16 <= sz; s++) {
        const uint8_t *mhsd = buf + pos;
        if (memcmp(mhsd, "mhsd", 4) != 0)
            break;
        uint32_t shdr = rd32le(mhsd + 4);
        uint32_t stotal = rd32le(mhsd + 8);
        uint32_t stype = rd32le(mhsd + 12);
        if (stotal == 0 || pos + stotal > sz || shdr > stotal)
            break;
        if (stype == 1) {
            mhli = buf + pos + shdr;
            mhli_sz = stotal - shdr;
            break;
        }
        pos += stotal;
    }

    if (!mhli || mhli_sz < 12 || memcmp(mhli, "mhli", 4) != 0) {
        free(buf);
        return NULL;
    }

    uint32_t lhdr = rd32le(mhli + 4);
    uint32_t count = rd32le(mhli + 8);
    if (lhdr < 12 || count == 0) {
        free(buf);
        return NULL;
    }

    struct art_slot *slots = (struct art_slot *)calloc(count, sizeof(struct art_slot));
    if (!slots) {
        free(buf);
        return NULL;
    }

    size_t ipos = lhdr;
    int found = 0;
    for (uint32_t i = 0; i < count && ipos + 32 <= mhli_sz; i++) {
        const uint8_t *mhii = mhli + ipos;
        if (memcmp(mhii, "mhii", 4) != 0)
            break;
        uint32_t ihdr = rd32le(mhii + 4);
        uint32_t itotal = rd32le(mhii + 8);
        uint32_t children = rd32le(mhii + 12);
        if (ihdr < 28 || itotal < ihdr || ipos + itotal > mhli_sz)
            break;

        uint32_t art_id = rd32le(mhii + 16);
        uint64_t song_dbid = rd64le(mhii + 20);

        struct art_slot best;
        memset(&best, 0, sizeof(best));
        uint32_t best_area = 0;
        bool best_unpadded = false;

        size_t mpos = ihdr;
        for (uint32_t c = 0; c < children && mpos + 16 <= itotal; c++) {
            const uint8_t *mhod = mhii + mpos;
            if (memcmp(mhod, "mhod", 4) != 0)
                break;
            uint32_t mhdr = rd32le(mhod + 4);
            uint32_t mtotal = rd32le(mhod + 8);
            uint32_t mtype = rd32le(mhod + 12);
            if (mtotal == 0 || mpos + mtotal > itotal || mhdr > mtotal)
                break;

            if (mtype == 2 && mhdr + 36 <= mtotal) {
                const uint8_t *mhni = mhod + mhdr;
                if (memcmp(mhni, "mhni", 4) == 0) {
                    uint32_t nhdr = rd32le(mhni + 4);
                    uint32_t ntotal = rd32le(mhni + 8);
                    uint32_t nchild = rd32le(mhni + 12);
                    if (nhdr >= 36 && ntotal >= nhdr && mhdr + ntotal <= mtotal) {
                        uint32_t fmt = rd32le(mhni + 16);
                        uint32_t off = rd32le(mhni + 20);
                        uint32_t isz = rd32le(mhni + 24);
                        int16_t vpad = (int16_t)rd16le(mhni + 28);
                        int16_t hpad = (int16_t)rd16le(mhni + 30);
                        uint16_t h = rd16le(mhni + 32);
                        uint16_t w = rd16le(mhni + 34);

                        char fname[64];
                        fname[0] = '\0';
                        size_t fnpos = nhdr;
                        for (uint32_t nc = 0; nc < nchild && fnpos + 16 <= ntotal; nc++) {
                            const uint8_t *fm = mhni + fnpos;
                            if (memcmp(fm, "mhod", 4) != 0)
                                break;
                            uint32_t ftotal = rd32le(fm + 8);
                            uint32_t ftype = rd32le(fm + 12);
                            if (ftotal == 0 || fnpos + ftotal > ntotal)
                                break;
                            if (ftype == 3 && ftotal > 36) {
                                uint32_t flen = rd32le(fm + 24);
                                if (flen > ftotal - 36)
                                    flen = ftotal - 36;
                                if (fm[28] == 2) {
                                    decode_utf16(fm + 36, flen, false, fname, sizeof(fname));
                                } else {
                                    size_t cp = flen < sizeof(fname) - 1 ? flen : sizeof(fname) - 1;
                                    memcpy(fname, fm + 36, cp);
                                    fname[cp] = '\0';
                                }
                                break;
                            }
                            fnpos += ftotal;
                        }
                        if (!fname[0])
                            snprintf(fname, sizeof(fname), "F%u_1.ithmb", fmt);

                        /* Strip leading ':' or '/' and normalise ':' */
                        const char *rel = fname;
                        while (*rel == ':' || *rel == '/')
                            rel++;
                        uint32_t area = (uint32_t)w * (uint32_t)h;
                        bool unpadded = (hpad == 0 && vpad == 0);
                        bool better = (unpadded && !best_unpadded) ||
                                      (unpadded == best_unpadded && area > best_area);
                        if (w > 0 && h > 0 && isz >= (uint32_t)w * h * 2 &&
                            better) {
                            best_area = area;
                            best_unpadded = unpadded;
                            best.art_id = art_id;
                            best.song_dbid = song_dbid;
                            best.ithmb_offset = off;
                            best.image_size = isz;
                            best.width = w;
                            best.height = h;
                            best.hpad = hpad;
                            best.vpad = vpad;
                            snprintf(best.ithmb_rel, sizeof(best.ithmb_rel), "%s", rel);
                            for (char *q = best.ithmb_rel; *q; q++) {
                                if (*q == ':')
                                    *q = '/';
                            }
                        }
                    }
                }
            }
            mpos += mtotal;
        }

        if (best_area > 0)
            slots[found++] = best;
        ipos += itotal;
    }

    free(buf);
    if (found > 1)
        qsort(slots, (size_t)found, sizeof(struct art_slot), cmp_art_id);
    *out_count = found;
    return slots;
}

static bool load_ithmb_pixels(const struct art_slot *slot, uint16_t **out_px)
{
    *out_px = NULL;
    size_t tight_row = (size_t)slot->width * 2U;
    size_t tight_sz = tight_row * (size_t)slot->height;
    if (tight_sz == 0 || tight_sz > 8 * 1024 * 1024)
        return false;

    size_t read_sz = (slot->image_size >= tight_sz && slot->image_size <= 8 * 1024 * 1024)
        ? (size_t)slot->image_size
        : tight_sz;
    size_t row_bytes = (slot->height > 0) ? (read_sz / (size_t)slot->height) : tight_row;
    if (row_bytes < tight_row)
        row_bytes = tight_row;

    char ipath[512];
    snprintf(ipath, sizeof(ipath), "%s/%s", IPOD_ARTWORK_DIR, slot->ithmb_rel);
    int fd = open(ipath, O_RDONLY);
    if (fd < 0)
        return false;

    if (lseek(fd, (off_t)slot->ithmb_offset, SEEK_SET) != (off_t)slot->ithmb_offset) {
        close(fd);
        return false;
    }

    uint8_t *raw = (uint8_t *)malloc(read_sz);
    if (!raw) {
        close(fd);
        return false;
    }

    size_t got = 0;
    while (got < read_sz) {
        ssize_t r = read(fd, raw + got, read_sz - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
        got += (size_t)r;
    }
    close(fd);

    if (got < tight_sz || got < row_bytes * (size_t)slot->height) {
        free(raw);
        return false;
    }

    if (row_bytes == tight_row) {
        *out_px = (uint16_t *)raw;
        return true;
    }

    uint16_t *buf = (uint16_t *)malloc(tight_sz);
    if (!buf) {
        free(raw);
        return false;
    }
    uint8_t *dst = (uint8_t *)buf;
    for (uint16_t y = 0; y < slot->height; y++)
        memcpy(dst + (size_t)y * tight_row, raw + (size_t)y * row_bytes, tight_row);
    free(raw);

    *out_px = buf;
    return true;
}

/* =========================================================================
 * Native In-Memory iTunesDB / ArtworkDB / Playlists Cache
 * ========================================================================= */

struct native_playlist {
    char *name;
    int  *track_indices;
    int   count;
};

struct path_map_entry {
    const char *path;
    int         index;
};

static pthread_mutex_t g_native_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_native_loaded = false;
static struct itdb_parsed g_itdb;
static struct path_map_entry *g_by_path = NULL;
static struct art_slot *g_art_slots = NULL;
static int g_art_slot_count = 0;
static struct native_playlist *g_playlists = NULL;
static int g_playlist_count = 0;
static int g_playlist_cap = 0;

static const char *strip_sd_prefix(const char *p)
{
    if (!p)
        return "";
    if (strncmp(p, SD_ROOT "/", sizeof(SD_ROOT)) == 0)
        return p + sizeof(SD_ROOT) - 1;
    return p;
}

static int cmp_path_map(const void *a, const void *b)
{
    const struct path_map_entry *pa = (const struct path_map_entry *)a;
    const struct path_map_entry *pb = (const struct path_map_entry *)b;
    return strcasecmp(pa->path, pb->path);
}

static int find_track_by_path_locked(const char *rb_path)
{
    if (!g_native_loaded || !g_by_path || g_itdb.track_count <= 0 || !rb_path)
        return -1;
    const char *key = strip_sd_prefix(rb_path);
    int lo = 0, hi = g_itdb.track_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcasecmp(g_by_path[mid].path, key);
        if (c == 0)
            return g_by_path[mid].index;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static void free_native_playlists_locked(void)
{
    if (g_playlists) {
        for (int i = 0; i < g_playlist_count; i++) {
            free(g_playlists[i].name);
            free(g_playlists[i].track_indices);
        }
        free(g_playlists);
        g_playlists = NULL;
    }
    g_playlist_count = 0;
    g_playlist_cap = 0;
}

static void free_native_state_locked(void)
{
    free_native_playlists_locked();
    free(g_art_slots);
    g_art_slots = NULL;
    g_art_slot_count = 0;
    free(g_by_path);
    g_by_path = NULL;
    if (g_native_loaded) {
        itdb_free(&g_itdb);
        g_native_loaded = false;
    }
}

static void add_native_playlist_locked(const char *name, const int *indices, int count)
{
    if (!name || !name[0] || !indices || count <= 0)
        return;
    if (g_playlist_count >= g_playlist_cap) {
        int new_cap = g_playlist_cap > 0 ? g_playlist_cap * 2 : 16;
        struct native_playlist *np = (struct native_playlist *)realloc(
            g_playlists, (size_t)new_cap * sizeof(struct native_playlist));
        if (!np)
            return;
        g_playlists = np;
        g_playlist_cap = new_cap;
    }
    int *copy = (int *)malloc((size_t)count * sizeof(int));
    if (!copy)
        return;
    memcpy(copy, indices, (size_t)count * sizeof(int));
    g_playlists[g_playlist_count].name = dup_or_empty(name);
    g_playlists[g_playlist_count].track_indices = copy;
    g_playlists[g_playlist_count].count = count;
    g_playlist_count++;
}

static void overlay_playcounts_and_otg_locked(void)
{
    size_t pc_sz = 0;
    uint8_t *pc_buf = read_entire_file(IPOD_PLAYCOUNTS_PATH, &pc_sz);
    if (pc_buf && pc_sz >= 0x60 && memcmp(pc_buf, "mhdp", 4) == 0) {
        uint32_t hdr_len = rd32le(pc_buf + 4);
        uint32_t ent_len = rd32le(pc_buf + 8);
        uint32_t ent_cnt = rd32le(pc_buf + 12);
        if (hdr_len >= 0x10 && ent_len >= 0x14 &&
            (size_t)hdr_len + (size_t)ent_cnt * ent_len <= pc_sz) {
            uint32_t n = ent_cnt < (uint32_t)g_itdb.track_count
                         ? ent_cnt : (uint32_t)g_itdb.track_count;
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *pe = pc_buf + hdr_len + (size_t)i * ent_len;
                uint32_t pdelta = rd32le(pe + 0);
                uint32_t lmac   = rd32le(pe + 4);
                uint32_t bm     = rd32le(pe + 8);
                uint32_t stars  = rd32le(pe + 12);
                g_itdb.tracks[i].playcount += pdelta;
                if (lmac > MAC_EPOCH_OFFSET)
                    g_itdb.tracks[i].lastplayed_mac = lmac;
                if (bm > 0)
                    g_itdb.tracks[i].bookmark_ms = bm;
                if (stars <= 100)
                    g_itdb.tracks[i].rating_ipod = (uint8_t)stars;
            }
        }
    }
    free(pc_buf);

    size_t otg_sz = 0;
    uint8_t *otg_buf = read_entire_file(IPOD_OTG_INFO_PATH, &otg_sz);
    if (otg_buf && otg_sz >= 20 && memcmp(otg_buf, "mhpo", 4) == 0) {
        uint32_t hdr_len = rd32le(otg_buf + 4);
        uint32_t cnt     = rd32le(otg_buf + 8);
        if (hdr_len >= 12 && (size_t)hdr_len + (size_t)cnt * 4 <= otg_sz) {
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t tidx = rd32le(otg_buf + hdr_len + (size_t)i * 4);
                if (tidx < (uint32_t)g_itdb.track_count)
                    g_itdb.tracks[tidx].is_favorite = true;
            }
        }
    }
    free(otg_buf);
}

/* =========================================================================
 * Rockbox TagCache Writer (database_idx.tcd + database_0..8,12.tcd)
 * ========================================================================= */

struct str_ref {
    const char *str;
    int         track_idx;
};

static int cmp_str_ref_unique(const void *a, const void *b)
{
    const struct str_ref *sa = (const struct str_ref *)a;
    const struct str_ref *sb = (const struct str_ref *)b;
    return strcasecmp(sa->str, sb->str);
}

static int cmp_str_ref_nonunique(const void *a, const void *b)
{
    const struct str_ref *sa = (const struct str_ref *)a;
    const struct str_ref *sb = (const struct str_ref *)b;
    int c = strcasecmp(sa->str, sb->str);
    if (c != 0)
        return c;
    return sa->track_idx - sb->track_idx;
}

static const char *track_tag_string(const struct itdb_track *tr, int tag)
{
    const char *s = "";
    switch (tag) {
    case RB_TAG_ARTIST:          s = tr->artist; break;
    case RB_TAG_ALBUM:           s = tr->album; break;
    case RB_TAG_GENRE:           s = tr->genre; break;
    case RB_TAG_TITLE:           s = tr->title; break;
    case RB_TAG_FILENAME:        return tr->path;
    case RB_TAG_COMPOSER:        s = tr->composer; break;
    case RB_TAG_COMMENT:         s = tr->comment; break;
    case RB_TAG_ALBUMARTIST:     s = tr->albumartist; break;
    case RB_TAG_GROUPING:        s = tr->grouping; break;
    case RB_TAG_CANONICALARTIST: s = tr->albumartist[0] ? tr->albumartist : tr->artist; break;
    default:                     break;
    }
    return (s && s[0]) ? s : "<Untagged>";
}

static bool build_string_tcd_mem(int tag, bool sorted, bool unique,
                                 const struct itdb_track * const *audio, int n_audio,
                                 struct rb_index_entry *idx_entries,
                                 struct y1_ipod_ramcache_tag *out_tag)
{
    struct str_ref *refs = (struct str_ref *)malloc(
        (size_t)(n_audio > 0 ? n_audio : 1) * sizeof(struct str_ref));
    if (!refs)
        return false;

    for (int i = 0; i < n_audio; i++) {
        refs[i].str = track_tag_string(audio[i], tag);
        refs[i].track_idx = i;
    }

    if (sorted && n_audio > 1) {
        qsort(refs, (size_t)n_audio, sizeof(struct str_ref),
              unique ? cmp_str_ref_unique : cmp_str_ref_nonunique);
    }

    size_t total_size = sizeof(struct rb_tagcache_header);
    int32_t entry_count = 0;
    const char *prev_str = NULL;

    for (int i = 0; i < n_audio; i++) {
        if (unique && prev_str && strcasecmp(prev_str, refs[i].str) == 0)
            continue;
        size_t slen = strlen(refs[i].str) + 1;
        if (slen > 4096 - RB_TAG_CHUNK)
            slen = 4096 - RB_TAG_CHUNK;
        int32_t padlen = (int32_t)slen;
        if ((padlen + (int32_t)sizeof(struct rb_tagfile_entry)) % RB_TAG_CHUNK != 0) {
            padlen += RB_TAG_CHUNK -
                      ((padlen + (int32_t)sizeof(struct rb_tagfile_entry)) % RB_TAG_CHUNK);
        }
        total_size += sizeof(struct rb_tagfile_entry) + (size_t)padlen;
        prev_str = refs[i].str;
        entry_count++;
    }

    uint8_t *buf = (uint8_t *)calloc(1, total_size);
    if (!buf) {
        free(refs);
        return false;
    }

    int32_t offset = (int32_t)sizeof(struct rb_tagcache_header);
    prev_str = NULL;
    int32_t prev_offset = 0;

    for (int i = 0; i < n_audio; i++) {
        if (unique && prev_str && strcasecmp(prev_str, refs[i].str) == 0) {
            idx_entries[refs[i].track_idx].tag_seek[tag] = prev_offset;
            continue;
        }

        size_t slen = strlen(refs[i].str) + 1;
        if (slen > 4096 - RB_TAG_CHUNK)
            slen = 4096 - RB_TAG_CHUNK;
        int32_t padlen = (int32_t)slen;
        if ((padlen + (int32_t)sizeof(struct rb_tagfile_entry)) % RB_TAG_CHUNK != 0) {
            padlen += RB_TAG_CHUNK -
                      ((padlen + (int32_t)sizeof(struct rb_tagfile_entry)) % RB_TAG_CHUNK);
        }

        struct rb_tagfile_entry *tfe = (struct rb_tagfile_entry *)(buf + offset);
        tfe->tag_length = padlen;
        tfe->idx_id = unique ? -1 : (int32_t)refs[i].track_idx;
        memcpy(buf + offset + sizeof(*tfe), refs[i].str, slen - 1);
        buf[offset + sizeof(*tfe) + slen - 1] = '\0';

        idx_entries[refs[i].track_idx].tag_seek[tag] = offset;
        prev_str = refs[i].str;
        prev_offset = offset;
        offset += (int32_t)sizeof(*tfe) + padlen;
    }

    struct rb_tagcache_header *hdr = (struct rb_tagcache_header *)buf;
    hdr->magic = RB_TAGCACHE_MAGIC;
    hdr->datasize = offset - (int32_t)sizeof(*hdr);
    hdr->entry_count = entry_count;

    free(refs);
    out_tag->tag = tag;
    out_tag->entry_count = entry_count;
    out_tag->data = buf;
    out_tag->size = (size_t)offset;
    return true;
}

static bool build_ramcache_from_itdb_locked(struct y1_ipod_ramcache_data *out)
{
    memset(out, 0, sizeof(*out));

    const struct itdb_track **audio = (const struct itdb_track **)
        malloc((size_t)(g_itdb.track_count > 0 ? g_itdb.track_count : 1) * sizeof(struct itdb_track *));
    if (!audio)
        return false;

    int n_audio = 0;
    for (int i = 0; i < g_itdb.track_count; i++) {
        if (!g_itdb.tracks[i].is_video && g_itdb.tracks[i].path[0])
            audio[n_audio++] = &g_itdb.tracks[i];
    }

    size_t idx_bytes = (size_t)(n_audio > 0 ? n_audio : 1) * sizeof(struct rb_index_entry);
    struct rb_index_entry *idx = (struct rb_index_entry *)calloc(1, idx_bytes);
    if (!idx) {
        free(audio);
        return false;
    }

    static const struct {
        int  tag;
        bool sorted;
        bool unique;
    } str_tags[] = {
        { RB_TAG_ARTIST,          true,  true  },
        { RB_TAG_ALBUM,           true,  true  },
        { RB_TAG_GENRE,           true,  true  },
        { RB_TAG_TITLE,           true,  false },
        { RB_TAG_FILENAME,        false, false },
        { RB_TAG_COMPOSER,        true,  true  },
        { RB_TAG_COMMENT,         true,  true  },
        { RB_TAG_ALBUMARTIST,     true,  true  },
        { RB_TAG_GROUPING,        true,  true  },
        { RB_TAG_CANONICALARTIST, true,  true  },
    };

    size_t total_datasize = sizeof(struct rb_master_header) +
                            (size_t)n_audio * sizeof(struct rb_index_entry);

    for (int t = 0; t < (int)(sizeof(str_tags) / sizeof(str_tags[0])); t++) {
        if (!build_string_tcd_mem(str_tags[t].tag, str_tags[t].sorted,
                                  str_tags[t].unique, audio, n_audio,
                                  idx, &out->tags[out->num_tags])) {
            free(idx);
            free(audio);
            y1_ipod_native_free_ramcache(out);
            return false;
        }
        total_datasize += out->tags[out->num_tags].size + 16;
        out->num_tags++;
    }

    for (int i = 0; i < n_audio; i++) {
        const struct itdb_track *tr = audio[i];
        int32_t lp = 0;
        if (tr->lastplayed_mac > MAC_EPOCH_OFFSET) {
            uint32_t u = tr->lastplayed_mac - MAC_EPOCH_OFFSET;
            if (u >= LASTPLAYED_TIME_MIN && u <= (uint32_t)INT32_MAX)
                lp = (int32_t)u;
        }

        uint32_t fat_mtime = 0;
        char realpath_buf[512];
        struct stat st;
        sd_to_real_path(tr->path, realpath_buf, sizeof(realpath_buf));
        if (stat(realpath_buf, &st) == 0)
            fat_mtime = unix_to_fat_mtime(st.st_mtime);

        int64_t playtime_ms = (int64_t)tr->playcount * (int64_t)tr->length_ms;
        if (playtime_ms > INT32_MAX)
            playtime_ms = INT32_MAX;

        idx[i].tag_seek[RB_TAG_YEAR]        = (int32_t)tr->year;
        idx[i].tag_seek[RB_TAG_DISCNUMBER]  = (int32_t)tr->discnum;
        idx[i].tag_seek[RB_TAG_TRACKNUMBER] = (int32_t)tr->tracknum;
        idx[i].tag_seek[RB_TAG_BITRATE]     = (int32_t)tr->bitrate;
        idx[i].tag_seek[RB_TAG_LENGTH]      = (int32_t)tr->length_ms;
        idx[i].tag_seek[RB_TAG_PLAYCOUNT]   = (int32_t)tr->playcount;
        idx[i].tag_seek[RB_TAG_RATING]      = (int32_t)(tr->rating_ipod / 10);
        idx[i].tag_seek[RB_TAG_PLAYTIME]    = (int32_t)playtime_ms;
        idx[i].tag_seek[RB_TAG_LASTPLAYED]  = lp;
        idx[i].tag_seek[RB_TAG_COMMITID]    = 1;
        idx[i].tag_seek[RB_TAG_MTIME]       = (int32_t)fat_mtime;
        idx[i].tag_seek[RB_TAG_LASTELAPSED] = (int32_t)tr->bookmark_ms;
        idx[i].tag_seek[RB_TAG_LASTOFFSET]  = 0;
        idx[i].tag_seek[RB_TAG_FAVORITE]    = tr->is_favorite ? 1 : 0;
        idx[i].flag = 0;
    }

    free(audio);
    out->entry_count = n_audio;
    out->indices = idx;
    out->indices_size = (size_t)n_audio * sizeof(struct rb_index_entry);
    out->total_datasize = total_datasize;
    return true;
}

/* =========================================================================
 * Playlist & Smart Playlist Exporter: Playlists and m3u8
 * ========================================================================= */

static bool utf8_strcasestr(const char *hay, const char *needle)
{
    if (!needle[0])
        return true;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0)
            return true;
    }
    return false;
}

static bool eval_slst_rule(const struct itdb_track *tr, const uint8_t *rule, size_t rlen,
                           uint32_t now_mac, bool *supported)
{
    if (supported) *supported = true;
    if (rlen < 56) {
        if (supported) *supported = false;
        return false;
    }
    uint32_t field = rd32be(rule + 0);
    uint32_t action = rd32be(rule + 4);
    uint32_t vlen = rd32be(rule + 52);
    if (56 + vlen > rlen) {
        if (supported) *supported = false;
        return false;
    }

    bool negate = (action & 0x02000000U) != 0;
    bool is_str = (action & 0x01000000U) != 0;
    uint32_t op = action & 0x00ffffffU;
    bool res = false;

    if (is_str) {
        const char *tval = NULL;
        switch (field) {
        case 0x02: tval = tr->title; break;
        case 0x03: tval = tr->album; break;
        case 0x04: tval = tr->artist; break;
        case 0x08: tval = tr->genre; break;
        case 0x1d: tval = tr->comment; break;
        case 0x1f: tval = tr->composer; break;
        case 0x27: tval = tr->grouping; break;
        case 0x29: tval = tr->albumartist; break;
        case 0x3e: tval = tr->tv_show; break;
        default:   if (supported) *supported = false; return false;
        }
        char needle[512];
        decode_utf16(rule + 56, vlen, true, needle, sizeof(needle));
        if (op == 0x01U)
            res = (strcasecmp(tval, needle) == 0);
        else if (op == 0x02U)
            res = utf8_strcasestr(tval, needle);
        else if (op == 0x04U)
            res = (strncasecmp(tval, needle, strlen(needle)) == 0);
        else if (op == 0x08U) {
            size_t hl = strlen(tval), nl = strlen(needle);
            res = (hl >= nl && strcasecmp(tval + hl - nl, needle) == 0);
        }
    } else if (vlen >= 48) {
        uint64_t fval = rd64be(rule + 56);
        int64_t  fdate = (int64_t)rd64be(rule + 64);
        uint64_t funits = rd64be(rule + 72);
        uint64_t tval_hi = rd64be(rule + 80);
        bool is_date = false;
        uint64_t tv = 0;

        switch (field) {
        case 0x05: tv = tr->bitrate; break;
        case 0x07: tv = tr->year; break;
        case 0x0b: tv = tr->tracknum; break;
        case 0x0d: tv = tr->length_ms; break;
        case 0x16: tv = tr->playcount; break;
        case 0x17: tv = tr->lastplayed_mac; is_date = true; break;
        case 0x18: tv = tr->discnum; break;
        case 0x19: tv = tr->rating_ipod; break;
        case 0x20: tv = tr->added_mac; is_date = true; break;
        case 0x3c: tv = tr->media_type; break;
        default:   if (supported) *supported = false; return false;
        }

        if (op == 0x200U && is_date) {
            /* "In the last" N units (negative offset relative to now) */
            uint64_t span = (uint64_t)(fdate < 0 ? -fdate : fdate) * (funits ? funits : 1);
            res = (tv > 0 && now_mac >= span && tv >= (now_mac - span));
        } else if (op == 0x01U) {
            res = (tv == fval);
        } else if (op == 0x10U) {
            res = (tv > fval);
        } else if (op == 0x40U) {
            res = (tv < fval);
        } else if (op == 0x100U) {
            res = (tv >= fval && tv <= tval_hi);
        }
    } else {
        if (supported) *supported = false;
        return false;
    }

    return negate ? !res : res;
}

static void collect_playlists_from_section_locked(int64_t list_off, bool is_podcast_sec)
{
    if (list_off < 0 || (size_t)list_off + 12 > g_itdb.raw_size)
        return;
    const uint8_t *buf = g_itdb.raw;
    size_t sz = g_itdb.raw_size;
    const uint8_t *mhlp = buf + list_off;
    if (memcmp(mhlp, "mhlp", 4) != 0)
        return;

    uint32_t lhdr = rd32le(mhlp + 4);
    uint32_t lists = rd32le(mhlp + 8);
    if (lhdr < 12)
        return;

    time_t now_u = time(NULL);
    uint32_t now_mac = (now_u > (time_t)LASTPLAYED_TIME_MIN)
                       ? ((uint32_t)now_u + MAC_EPOCH_OFFSET)
                       : (g_itdb.db_mtime + MAC_EPOCH_OFFSET);

    int *idx_list = (int *)malloc(
        (size_t)(g_itdb.track_count > 0 ? g_itdb.track_count : 1) * sizeof(int));
    if (!idx_list)
        return;

    size_t pos = (size_t)list_off + lhdr;
    for (uint32_t p = 0; p < lists && pos + 44 <= sz; p++) {
        const uint8_t *mhyp = buf + pos;
        if (memcmp(mhyp, "mhyp", 4) != 0)
            break;
        uint32_t phdr = rd32le(mhyp + 4);
        uint32_t ptotal = rd32le(mhyp + 8);
        uint32_t mhods = rd32le(mhyp + 12);
        uint32_t items = rd32le(mhyp + 16);
        uint32_t is_master = rd32le(mhyp + 20);
        if (ptotal == 0 || pos + ptotal > sz || phdr < 44)
            break;

        bool is_folder = (phdr >= 42 && mhyp[41] != 0);

        char pname[256];
        snprintf(pname, sizeof(pname), is_podcast_sec ? "Podcasts_%u" : "Playlist_%u", p);

        const uint8_t *mhod50 = NULL;
        const uint8_t *slst = NULL;
        size_t slst_len = 0;

        int item_count = 0;
        size_t cpos = pos + phdr;
        for (uint32_t k = 0; k < mhods + items && cpos + 16 <= pos + ptotal; k++) {
            const uint8_t *cp = buf + cpos;
            const char *tag = (k < mhods) ? "mhod" : "mhip";
            if (memcmp(cp, tag, 4) != 0)
                break;
            uint32_t ctotal = rd32le(cp + 8);
            if (ctotal == 0 || cpos + ctotal > pos + ptotal)
                break;

            if (k < mhods) {
                uint32_t mt = rd32le(cp + 12);
                if (mt == 1) {
                    char decoded[256];
                    decode_mhod_string(cp, ctotal, decoded, sizeof(decoded));
                    if (decoded[0])
                        snprintf(pname, sizeof(pname), "%s", decoded);
                } else if (mt == 50 && ctotal >= 40) {
                    mhod50 = cp;
                } else if (mt == 51 && ctotal >= 160 && memcmp(cp + 24, "SLst", 4) == 0) {
                    slst = cp + 24;
                    slst_len = ctotal - 24;
                }
            } else if (item_count < g_itdb.track_count) {
                if (ctotal >= 28) {
                    uint32_t tid = rd32le(cp + 24);
                    if (tid != 0 && !(is_podcast_sec && rd32le(cp + 16) == 0x100U)) {
                        int tidx = itdb_find_by_id(&g_itdb, tid);
                        if (tidx >= 0)
                            idx_list[item_count++] = tidx;
                    }
                }
            }
            cpos += ctotal;
        }

        if (is_master == 0 && !is_podcast_sec && !is_folder) {
            if (slst && mhod50 && (mhod50[24] != 0 || item_count == 0) &&
                slst_len >= 136 && memcmp(slst, "SLst", 4) == 0) {
                uint32_t rcount = rd32be(slst + 8);
                uint32_t conj_or = rd32be(slst + 12);
                int eval_count = 0;
                bool rules_supported = (rcount > 0 && rcount <= 64);

                if (rules_supported) {
                    for (int ti = 0; ti < g_itdb.track_count && rules_supported; ti++) {
                        const struct itdb_track *tr = &g_itdb.tracks[ti];
                        if (tr->is_video)
                            continue;
                        bool match = (conj_or == 0);
                        size_t roff = 136;
                        for (uint32_t r = 0; r < rcount; r++) {
                            if (roff + 56 > slst_len) {
                                rules_supported = false;
                                break;
                            }
                            uint32_t vlen = rd32be(slst + roff + 52);
                            uint32_t rtot = 56 + vlen;
                            if (rtot < 56 || roff + rtot > slst_len) {
                                rules_supported = false;
                                break;
                            }
                            uint32_t fcode = rd32be(slst + roff);
                            if (fcode == 0x28U) {
                                rules_supported = false;
                                break;
                            }
                            bool rsup = true;
                            bool rmatch = eval_slst_rule(tr, slst + roff, rtot, now_mac, &rsup);
                            if (!rsup) {
                                rules_supported = false;
                                break;
                            }
                            if (conj_or != 0)
                                match = match || rmatch;
                            else
                                match = match && rmatch;
                            roff += rtot;
                        }
                        if (rules_supported && match)
                            idx_list[eval_count++] = ti;
                    }
                }

                if (rules_supported) {
                    uint8_t check_limits = mhod50[26];
                    uint8_t limit_type = mhod50[27];
                    uint32_t limit_val = rd32le(mhod50 + 32);
                    if (check_limits && limit_type == 3 &&
                        limit_val > 0 && (int)limit_val < eval_count) {
                        eval_count = (int)limit_val;
                    }
                    item_count = eval_count;
                }
            }

            if (item_count > 0)
                add_native_playlist_locked(pname, idx_list, item_count);
        }
        pos += ptotal;
    }

    free(idx_list);
}

/* =========================================================================
 * Native In-Memory Loader & Live Play Counts / OTG / ArtworkDB APIs
 * ========================================================================= */

static bool ensure_native_loaded_locked(void)
{
    if (g_native_loaded)
        return true;

    struct stat itdb_st;
    if (stat(IPOD_ITUNESDB_PATH, &itdb_st) != 0 || itdb_st.st_size < 24)
        return false;

    size_t db_sz = 0;
    uint8_t *db_buf = read_entire_file(IPOD_ITUNESDB_PATH, &db_sz);
    if (!db_buf)
        return false;

    if (!parse_itunesdb(db_buf, db_sz, (uint32_t)itdb_st.st_mtime, &g_itdb, NULL, NULL)) {
        free(db_buf);
        return false;
    }

    g_native_loaded = true;
    overlay_playcounts_and_otg_locked();

    g_by_path = (struct path_map_entry *)malloc(
        (size_t)(g_itdb.track_count > 0 ? g_itdb.track_count : 1) * sizeof(struct path_map_entry));
    if (g_by_path) {
        for (int i = 0; i < g_itdb.track_count; i++) {
            g_by_path[i].path = g_itdb.tracks[i].path;
            g_by_path[i].index = i;
        }
        if (g_itdb.track_count > 1)
            qsort(g_by_path, (size_t)g_itdb.track_count, sizeof(struct path_map_entry), cmp_path_map);
    }

    collect_playlists_from_section_locked(g_itdb.playlists_off, false);
    g_itdb.raw = NULL;
    g_itdb.raw_size = 0;
    free(db_buf);

    g_art_slots = parse_artwork_db(&g_art_slot_count);
    ensure_root_symlinks();
    return true;
}

static uint8_t *load_or_init_playcounts_locked(size_t *out_sz)
{
    uint32_t total_slots = (uint32_t)g_itdb.track_count + g_itdb.shows_count;
    size_t min_sz = 0x60U + (size_t)total_slots * 0x1cU;

    size_t pc_sz = 0;
    uint8_t *pc_buf = read_entire_file(IPOD_PLAYCOUNTS_PATH, &pc_sz);
    if (pc_buf && pc_sz >= min_sz && memcmp(pc_buf, "mhdp", 4) == 0 &&
        rd32le(pc_buf + 4) == 0x60U && rd32le(pc_buf + 8) == 0x1cU &&
        rd32le(pc_buf + 12) >= total_slots) {
        *out_sz = pc_sz;
        return pc_buf;
    }
    free(pc_buf);

    pc_buf = (uint8_t *)calloc(1, min_sz);
    if (!pc_buf)
        return NULL;

    memcpy(pc_buf, "mhdp", 4);
    wr32le(pc_buf + 4, 0x60U);
    wr32le(pc_buf + 8, 0x1cU);
    wr32le(pc_buf + 12, total_slots);

    /* Preserve each track's existing iTunesDB star rating so untouched tracks are never zeroed */
    for (int i = 0; i < g_itdb.track_count; i++) {
        uint8_t *pe = pc_buf + 0x60U + (size_t)i * 0x1cU;
        wr32le(pe + 12, (uint32_t)g_itdb.tracks[i].rating_ipod);
    }

    *out_sz = min_sz;
    return pc_buf;
}

bool y1_ipod_native_is_active(void)
{
    if (!y1_usb_get_ipod_mode())
        return false;
    struct stat st;
    return (stat(IPOD_ITUNESDB_PATH, &st) == 0 && st.st_size >= 24);
}

void y1_ipod_native_invalidate(void)
{
    pthread_mutex_lock(&g_native_lock);
    free_native_state_locked();
    pthread_mutex_unlock(&g_native_lock);
}

bool y1_ipod_native_load_ramcache(struct y1_ipod_ramcache_data *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!y1_ipod_native_is_active())
        return false;

    pthread_mutex_lock(&g_native_lock);
    bool ok = ensure_native_loaded_locked() && build_ramcache_from_itdb_locked(out);
    pthread_mutex_unlock(&g_native_lock);
    return ok;
}

void y1_ipod_native_free_ramcache(struct y1_ipod_ramcache_data *data)
{
    if (!data)
        return;
    free(data->indices);
    for (int i = 0; i < data->num_tags; i++)
        free(data->tags[i].data);
    memset(data, 0, sizeof(*data));
}

bool y1_ipod_native_record_play(const char *rb_path, uint32_t play_delta,
                                uint32_t unix_time, uint32_t bookmark_ms)
{
    if (!y1_ipod_native_is_active() || !rb_path)
        return false;

    pthread_mutex_lock(&g_native_lock);
    if (!ensure_native_loaded_locked()) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    int tidx = find_track_by_path_locked(rb_path);
    if (tidx < 0 || tidx >= g_itdb.track_count) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    size_t pc_sz = 0;
    uint8_t *pc_buf = load_or_init_playcounts_locked(&pc_sz);
    if (!pc_buf) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    uint32_t last_mac = (unix_time >= LASTPLAYED_TIME_MIN)
                        ? (unix_time + MAC_EPOCH_OFFSET)
                        : (g_itdb.db_mtime + MAC_EPOCH_OFFSET + 60U);

    g_itdb.tracks[tidx].playcount += play_delta;
    if (play_delta > 0)
        g_itdb.tracks[tidx].lastplayed_mac = last_mac;
    if (bookmark_ms > 0)
        g_itdb.tracks[tidx].bookmark_ms = bookmark_ms;

    uint8_t *pe = pc_buf + 0x60U + (size_t)tidx * 0x1cU;
    uint32_t cur_delta = rd32le(pe + 0);
    wr32le(pe + 0, cur_delta + play_delta);
    if (play_delta > 0) {
        wr32le(pe + 4, last_mac);
        pe[0x11] = 0x01; /* played_marker per iOpenPod sidecars/codec.py */
    }
    if (bookmark_ms > 0)
        wr32le(pe + 8, bookmark_ms);

    bool ok = write_file_atomic(IPOD_PLAYCOUNTS_PATH, pc_buf, pc_sz);
    free(pc_buf);
    pthread_mutex_unlock(&g_native_lock);
    return ok;
}

bool y1_ipod_native_record_rating(const char *rb_path, int rating_rb)
{
    if (!y1_ipod_native_is_active() || !rb_path)
        return false;

    if (rating_rb < 0) rating_rb = 0;
    if (rating_rb > 10) rating_rb = 10;
    uint8_t rating_ipod = (uint8_t)(rating_rb * 10);

    pthread_mutex_lock(&g_native_lock);
    if (!ensure_native_loaded_locked()) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    int tidx = find_track_by_path_locked(rb_path);
    if (tidx < 0 || tidx >= g_itdb.track_count) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    size_t pc_sz = 0;
    uint8_t *pc_buf = load_or_init_playcounts_locked(&pc_sz);
    if (!pc_buf) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    g_itdb.tracks[tidx].rating_ipod = rating_ipod;
    uint8_t *pe = pc_buf + 0x60U + (size_t)tidx * 0x1cU;
    wr32le(pe + 12, (uint32_t)rating_ipod);

    bool ok = write_file_atomic(IPOD_PLAYCOUNTS_PATH, pc_buf, pc_sz);
    free(pc_buf);
    pthread_mutex_unlock(&g_native_lock);
    return ok;
}

bool y1_ipod_native_record_favorite(const char *rb_path, bool is_favorite)
{
    if (!y1_ipod_native_is_active() || !rb_path)
        return false;

    pthread_mutex_lock(&g_native_lock);
    if (!ensure_native_loaded_locked()) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    int tidx = find_track_by_path_locked(rb_path);
    if (tidx < 0 || tidx >= g_itdb.track_count) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    g_itdb.tracks[tidx].is_favorite = is_favorite;

    uint32_t *indices = (uint32_t *)malloc(
        (size_t)(g_itdb.track_count > 0 ? g_itdb.track_count : 1) * sizeof(uint32_t));
    if (!indices) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }
    int count = 0;

    size_t otg_sz = 0;
    uint8_t *otg_buf = read_entire_file(IPOD_OTG_INFO_PATH, &otg_sz);
    if (otg_buf && otg_sz >= 20 && memcmp(otg_buf, "mhpo", 4) == 0) {
        uint32_t hdr_len = rd32le(otg_buf + 4);
        uint32_t cnt = rd32le(otg_buf + 8);
        if (hdr_len >= 12 && (size_t)hdr_len + (size_t)cnt * 4 <= otg_sz) {
            for (uint32_t i = 0; i < cnt && count < g_itdb.track_count; i++) {
                uint32_t v = rd32le(otg_buf + hdr_len + (size_t)i * 4);
                if (v < (uint32_t)g_itdb.track_count && (int)v != tidx)
                    indices[count++] = v;
            }
        }
    }
    free(otg_buf);

    if (is_favorite && count < g_itdb.track_count)
        indices[count++] = (uint32_t)tidx;

    bool ok = true;
    if (count == 0) {
        unlink(IPOD_OTG_INFO_PATH);
    } else {
        size_t osz = 20 + (size_t)count * sizeof(uint32_t);
        uint8_t *obuf = (uint8_t *)calloc(1, osz);
        if (obuf) {
            memcpy(obuf, "mhpo", 4);
            wr32le(obuf + 4, 20);
            wr32le(obuf + 8, (uint32_t)count);
            for (int i = 0; i < count; i++)
                wr32le(obuf + 20 + (size_t)i * 4, indices[i]);
            ok = write_file_atomic(IPOD_OTG_INFO_PATH, obuf, osz);
            free(obuf);
        } else {
            ok = false;
        }
    }

    free(indices);
    pthread_mutex_unlock(&g_native_lock);
    return ok;
}

bool y1_ipod_native_has_albumart(const char *rb_path)
{
    if (!y1_ipod_native_is_active() || !rb_path)
        return false;

    pthread_mutex_lock(&g_native_lock);
    if (!ensure_native_loaded_locked() || !g_art_slots || g_art_slot_count <= 0) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    int tidx = find_track_by_path_locked(rb_path);
    if (tidx < 0 || tidx >= g_itdb.track_count) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    const struct itdb_track *tr = &g_itdb.tracks[tidx];
    const struct art_slot *slot = (tr->art_id || tr->dbid)
        ? find_art_slot(g_art_slots, g_art_slot_count, tr->art_id, tr->dbid)
        : NULL;
    pthread_mutex_unlock(&g_native_lock);
    return (slot != NULL);
}

bool y1_ipod_native_load_albumart_rgb565(const char *rb_path,
                                          uint16_t **out_rgb565,
                                          int *out_w, int *out_h)
{
    if (!y1_ipod_native_is_active() || !rb_path || !out_rgb565 || !out_w || !out_h)
        return false;
    *out_rgb565 = NULL;
    *out_w = 0;
    *out_h = 0;

    pthread_mutex_lock(&g_native_lock);
    if (!ensure_native_loaded_locked() || !g_art_slots || g_art_slot_count <= 0) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    int tidx = find_track_by_path_locked(rb_path);
    if (tidx < 0 || tidx >= g_itdb.track_count) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    const struct itdb_track *tr = &g_itdb.tracks[tidx];
    const struct art_slot *slot = (tr->art_id || tr->dbid)
        ? find_art_slot(g_art_slots, g_art_slot_count, tr->art_id, tr->dbid)
        : NULL;
    if (!slot) {
        pthread_mutex_unlock(&g_native_lock);
        return false;
    }

    struct art_slot slot_copy = *slot;
    pthread_mutex_unlock(&g_native_lock);

    uint16_t *px = NULL;
    if (!load_ithmb_pixels(&slot_copy, &px))
        return false;

    size_t tight_sz = (size_t)slot_copy.width * (size_t)slot_copy.height * 2U;
    bool padded_external = (slot_copy.image_size > tight_sz);
    int x0 = (!padded_external && slot_copy.hpad > 0) ? slot_copy.hpad : 0;
    int y0 = (!padded_external && slot_copy.vpad > 0) ? slot_copy.vpad : 0;
    int cw = (int)slot_copy.width - 2 * x0;
    int ch = (int)slot_copy.height - 2 * y0;
    if (cw <= 0 || ch <= 0) {
        x0 = 0; y0 = 0;
        cw = slot_copy.width;
        ch = slot_copy.height;
    }

    uint16_t *cropped = (uint16_t *)malloc((size_t)cw * (size_t)ch * sizeof(uint16_t));
    if (!cropped) {
        free(px);
        return false;
    }

    for (int y = 0; y < ch; y++) {
        memcpy(cropped + (size_t)y * (size_t)cw,
               px + (size_t)(y0 + y) * (size_t)slot_copy.width + (size_t)x0,
               (size_t)cw * sizeof(uint16_t));
    }
    free(px);

    *out_rgb565 = cropped;
    *out_w = cw;
    *out_h = ch;
    return true;
}

int y1_ipod_native_playlist_count(void)
{
    if (!y1_ipod_native_is_active())
        return 0;
    pthread_mutex_lock(&g_native_lock);
    ensure_native_loaded_locked();
    int cnt = g_playlist_count;
    pthread_mutex_unlock(&g_native_lock);
    return cnt;
}

const char *y1_ipod_native_playlist_name(int pl_idx)
{
    pthread_mutex_lock(&g_native_lock);
    const char *res = (pl_idx >= 0 && pl_idx < g_playlist_count)
                      ? g_playlists[pl_idx].name : NULL;
    pthread_mutex_unlock(&g_native_lock);
    return res;
}

int y1_ipod_native_playlist_track_count(int pl_idx)
{
    pthread_mutex_lock(&g_native_lock);
    int res = (pl_idx >= 0 && pl_idx < g_playlist_count)
              ? g_playlists[pl_idx].count : 0;
    pthread_mutex_unlock(&g_native_lock);
    return res;
}

const char *y1_ipod_native_playlist_track_path(int pl_idx, int tr_idx)
{
    pthread_mutex_lock(&g_native_lock);
    const char *res = NULL;
    if (pl_idx >= 0 && pl_idx < g_playlist_count &&
        tr_idx >= 0 && tr_idx < g_playlists[pl_idx].count) {
        int tidx = g_playlists[pl_idx].track_indices[tr_idx];
        if (tidx >= 0 && tidx < g_itdb.track_count)
            res = g_itdb.tracks[tidx].path;
    }
    pthread_mutex_unlock(&g_native_lock);
    return res;
}

#if defined(Y1_IPOD_SYNC_MAIN) || defined(IPOD_SYNC_STANDALONE)
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <init>\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "init") == 0)
        return y1_ipod_ensure_device_files() ? 0 : 1;
    return 0;
}
#endif
