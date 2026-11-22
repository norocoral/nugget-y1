#define _FILE_OFFSET_BITS 64
#include "ipod_db.h"
#include "liby1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define STR_SLAB_CAP (256 * 1024)

static const char *arena_strdup(struct ipod_db *db, const char *s)
{
    if (!s || !s[0]) return "";
    size_t len = strlen(s) + 1;
    struct ipod_str_slab *slab = db->str_slabs;
    if (!slab || slab->used + len > slab->cap) {
        size_t cap = len > STR_SLAB_CAP ? len : STR_SLAB_CAP;
        slab = malloc(sizeof(struct ipod_str_slab) + cap);
        if (!slab) return "";
        slab->next = db->str_slabs;
        slab->used = 0;
        slab->cap = cap;
        db->str_slabs = slab;
    }
    char *dst = slab->data + slab->used;
    memcpy(dst, s, len);
    slab->used += len;
    return dst;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void utf16le_to_utf8(const uint8_t *src, size_t byte_len, char *dst, size_t dst_sz)
{
    size_t di = 0;
    for (size_t i = 0; i + 1 < byte_len && di + 4 < dst_sz; i += 2) {
        uint32_t cp = rd16(src + i);
        if (cp == 0) break;
        if (cp < 0x80) {
            dst[di++] = (char)cp;
        } else if (cp < 0x800) {
            dst[di++] = (char)(0xC0 | (cp >> 6));
            dst[di++] = (char)(0x80 | (cp & 0x3F));
        } else {
            dst[di++] = (char)(0xE0 | (cp >> 12));
            dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[di++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[di] = '\0';
}

static void parse_mhod_string(const uint8_t *mhod, size_t max_len, char *out, size_t out_sz)
{
    out[0] = '\0';
    if (max_len <= 40) return;
    uint32_t mhod_hdr  = rd32(mhod + 4);
    uint32_t total_len = rd32(mhod + 8);
    if (total_len > max_len) total_len = (uint32_t)max_len;
    if (mhod_hdr < 24 || mhod_hdr + 16 >= total_len) return;

    const uint8_t *sub = mhod + mhod_hdr;
    uint32_t encoding = rd32(sub + 0);
    uint32_t str_len  = rd32(sub + 4);
    size_t data_off   = mhod_hdr + 16;
    if (str_len > total_len - data_off)
        str_len = (uint32_t)(total_len - data_off);
    if (str_len == 0) return;

    const uint8_t *str_data = mhod + data_off;
    /* In iTunesDB, encoding 0 or 1 is UTF-16LE (even for non-ASCII leading chars); 2 is UTF-8 */
    bool is_utf16 = (encoding == 1) || (encoding == 0 && (str_len & 1) == 0 && str_len >= 2);
    if (is_utf16) {
        utf16le_to_utf8(str_data, str_len, out, out_sz);
    } else {
        bool valid_utf8 = true;
        for (size_t i = 0; i < str_len; ) {
            uint8_t c = str_data[i];
            if (c < 0x80) i++;
            else if ((c & 0xE0) == 0xC0 && i + 1 < str_len && (str_data[i + 1] & 0xC0) == 0x80) i += 2;
            else if ((c & 0xF0) == 0xE0 && i + 2 < str_len && (str_data[i + 1] & 0xC0) == 0x80 && (str_data[i + 2] & 0xC0) == 0x80) i += 3;
            else if ((c & 0xF8) == 0xF0 && i + 3 < str_len && (str_data[i + 1] & 0xC0) == 0x80 && (str_data[i + 2] & 0xC0) == 0x80 && (str_data[i + 3] & 0xC0) == 0x80) i += 4;
            else { valid_utf8 = false; break; }
        }
        if (valid_utf8) {
            size_t cp = str_len < out_sz - 1 ? str_len : out_sz - 1;
            memcpy(out, str_data, cp);
            out[cp] = '\0';
        } else {
            size_t di = 0;
            for (size_t i = 0; i < str_len && di + 3 < out_sz; i++) {
                uint8_t c = str_data[i];
                if (c < 0x80) out[di++] = (char)c;
                else {
                    out[di++] = (char)(0xC0 | (c >> 6));
                    out[di++] = (char)(0x80 | (c & 0x3F));
                }
            }
            out[di] = '\0';
        }
    }
}

static int cmp_group(const void *a, const void *b)
{
    const struct ipod_group *ga = (const struct ipod_group *)a;
    const struct ipod_group *gb = (const struct ipod_group *)b;
    return strcasecmp(ga->name, gb->name);
}

static int cmp_track(const void *a, const void *b)
{
    const struct ipod_track *ta = (const struct ipod_track *)a;
    const struct ipod_track *tb = (const struct ipod_track *)b;
    return strcasecmp(ta->title, tb->title);
}

static uint32_t hash_str_ci(const char *s)
{
    uint32_t h = 2166136261U;
    for (; *s; s++) {
        unsigned char c = (unsigned char)tolower((unsigned char)*s);
        h ^= c;
        h *= 16777619U;
    }
    return h;
}

static bool ensure_track_cap(struct ipod_db *db, int needed)
{
    if (needed <= db->track_cap) return true;
    if (needed > DB_MAX_TRACKS) return false;
    int new_cap = db->track_cap > 0 ? db->track_cap : 2048;
    while (new_cap < needed) {
        if (new_cap < 32768) new_cap *= 2;
        else new_cap += 32768;
    }
    if (new_cap > DB_MAX_TRACKS) new_cap = DB_MAX_TRACKS;
    struct ipod_track *nt = realloc(db->tracks, sizeof(struct ipod_track) * (size_t)new_cap);
    if (!nt) return false;
    db->tracks = nt;
    db->track_cap = new_cap;
    return true;
}

static void build_groups_hash(const struct ipod_db *db, bool is_album,
                              struct ipod_group **out_arr, int *out_cnt)
{
    *out_arr = NULL;
    *out_cnt = 0;
    if (db->track_count <= 0) return;

    int ht_cap = 2048;
    while (ht_cap < db->track_count && ht_cap < DB_MAX_GROUPS * 2) ht_cap *= 2;
    int *ht = malloc(sizeof(int) * (size_t)ht_cap);
    int arr_cap = 512;
    struct ipod_group *arr = malloc(sizeof(struct ipod_group) * (size_t)arr_cap);
    if (!ht || !arr) {
        free(ht); free(arr);
        return;
    }
    for (int i = 0; i < ht_cap; i++) ht[i] = -1;

    int cnt = 0;
    uint32_t mask = (uint32_t)(ht_cap - 1);
    for (int i = 0; i < db->track_count; i++) {
        const char *name = is_album
            ? (db->tracks[i].album[0] ? db->tracks[i].album : "Unknown Album")
            : (db->tracks[i].artist[0] ? db->tracks[i].artist : "Unknown Artist");
        uint32_t slot = hash_str_ci(name) & mask;
        int found = -1;
        while (ht[slot] >= 0) {
            if (strcasecmp(arr[ht[slot]].name, name) == 0) {
                found = ht[slot];
                break;
            }
            slot = (slot + 1) & mask;
        }
        if (found >= 0) {
            arr[found].track_count++;
        } else if (cnt < DB_MAX_GROUPS) {
            if (cnt >= arr_cap) {
                int nc = arr_cap * 2;
                if (nc > DB_MAX_GROUPS) nc = DB_MAX_GROUPS;
                struct ipod_group *na = realloc(arr, sizeof(struct ipod_group) * (size_t)nc);
                if (!na) break;
                arr = na;
                arr_cap = nc;
            }
            if (cnt * 2 >= ht_cap && ht_cap < DB_MAX_GROUPS * 4) {
                int nht_cap = ht_cap * 2;
                int *nht = malloc(sizeof(int) * (size_t)nht_cap);
                if (nht) {
                    for (int k = 0; k < nht_cap; k++) nht[k] = -1;
                    uint32_t nmask = (uint32_t)(nht_cap - 1);
                    for (int k = 0; k < cnt; k++) {
                        uint32_t ns = hash_str_ci(arr[k].name) & nmask;
                        while (nht[ns] >= 0) ns = (ns + 1) & nmask;
                        nht[ns] = k;
                    }
                    free(ht);
                    ht = nht;
                    ht_cap = nht_cap;
                    mask = nmask;
                    slot = hash_str_ci(name) & mask;
                    while (ht[slot] >= 0) slot = (slot + 1) & mask;
                }
            }
            snprintf(arr[cnt].name, sizeof(arr[0].name), "%s", name);
            arr[cnt].track_count = 1;
            arr[cnt].first_track_idx = i;
            ht[slot] = cnt;
            cnt++;
        }
    }
    free(ht);
    if (cnt > 1)
        qsort(arr, (size_t)cnt, sizeof(struct ipod_group), cmp_group);
    *out_arr = arr;
    *out_cnt = cnt;
}

static void build_groups(struct ipod_db *db)
{
    build_groups_hash(db, false, &db->artists, &db->artist_count);
    build_groups_hash(db, true,  &db->albums,  &db->album_count);
}

static bool has_audio_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *const exts[] = {
        ".mp3", ".m4a", ".m4b", ".aac", ".mp4",
        ".wav", ".aiff", ".aif", ".wma", ".opus", ".ogg",
        NULL
    };
    for (int i = 0; exts[i]; i++) {
        if (strcasecmp(dot, exts[i]) == 0) return true;
    }
    return false;
}

static bool has_ext_ci(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static uint32_t rd32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void parse_m4a_ilst_buf(const uint8_t *buf, size_t len,
                               char *title, size_t tsz,
                               char *artist, size_t arsz,
                               char *album, size_t alsz,
                               char *genre, size_t gsz,
                               uint16_t *year_out)
{
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t sz = rd32_be(buf + pos);
        if (sz < 8 || pos + sz > len) break;
        const uint8_t *tag = buf + pos + 4;
        /* Look for child 'data' atom inside this item atom */
        size_t cp = pos + 8;
        while (cp + 16 <= pos + sz) {
            uint32_t csz = rd32_be(buf + cp);
            if (csz < 8 || cp + csz > pos + sz) break;
            if (memcmp(buf + cp + 4, "data", 4) == 0 && csz > 16) {
                uint32_t dtype = rd32_be(buf + cp + 8);
                if (dtype == 1) { /* UTF-8 text */
                    size_t slen = csz - 16;
                    const char *s = (const char *)(buf + cp + 16);
                    if (memcmp(tag, "\xA9" "nam", 4) == 0 && slen > 0) {
                        size_t n = slen < tsz - 1 ? slen : tsz - 1;
                        memcpy(title, s, n); title[n] = '\0';
                    } else if (memcmp(tag, "\xA9" "ART", 4) == 0 && slen > 0) {
                        size_t n = slen < arsz - 1 ? slen : arsz - 1;
                        memcpy(artist, s, n); artist[n] = '\0';
                    } else if (memcmp(tag, "\xA9" "alb", 4) == 0 && slen > 0) {
                        size_t n = slen < alsz - 1 ? slen : alsz - 1;
                        memcpy(album, s, n); album[n] = '\0';
                    } else if (memcmp(tag, "\xA9" "gen", 4) == 0 && slen > 0) {
                        size_t n = slen < gsz - 1 ? slen : gsz - 1;
                        memcpy(genre, s, n); genre[n] = '\0';
                    } else if (memcmp(tag, "\xA9" "day", 4) == 0 && slen >= 4 && year_out) {
                        char ybuf[8];
                        size_t n = slen < 7 ? slen : 7;
                        memcpy(ybuf, s, n); ybuf[n] = '\0';
                        int y = atoi(ybuf);
                        if (y >= 1000 && y <= 2999) *year_out = (uint16_t)y;
                    }
                }
            }
            cp += csz;
        }
        pos += sz;
    }
}

static bool scan_m4a_atoms(FILE *f, uint64_t end_off, int depth,
                           char *title, size_t tsz,
                           char *artist, size_t arsz,
                           char *album, size_t alsz,
                           char *genre, size_t gsz,
                           uint16_t *year_out)
{
    if (depth > 6) return false;
    while (1) {
        uint64_t cur = (uint64_t)ftello(f);
        if (cur + 8 > end_off) break;
        uint8_t h[8];
        if (fread(h, 1, 8, f) != 8) break;
        uint64_t sz = rd32_be(h);
        uint64_t hdr = 8;
        if (sz == 1) {
            uint8_t ext[8];
            if (fread(ext, 1, 8, f) != 8) break;
            sz = ((uint64_t)rd32_be(ext) << 32) | rd32_be(ext + 4);
            hdr = 16;
        } else if (sz == 0) {
            sz = end_off - cur;
        }
        if (sz < hdr || cur + sz > end_off) break;
        uint64_t next = cur + sz;

        if (memcmp(h + 4, "moov", 4) == 0 || memcmp(h + 4, "udta", 4) == 0) {
            if (scan_m4a_atoms(f, next, depth + 1, title, tsz, artist, arsz, album, alsz, genre, gsz, year_out))
                return true;
        } else if (memcmp(h + 4, "meta", 4) == 0) {
            if (fseeko(f, (off_t)(cur + hdr + 4), SEEK_SET) == 0) {
                if (scan_m4a_atoms(f, next, depth + 1, title, tsz, artist, arsz, album, alsz, genre, gsz, year_out))
                    return true;
            }
        } else if (memcmp(h + 4, "ilst", 4) == 0) {
            uint64_t ilen = sz - hdr;
            if (ilen > 0 && ilen <= 256 * 1024) {
                uint8_t *ibuf = malloc((size_t)ilen);
                if (ibuf) {
                    if (fread(ibuf, 1, (size_t)ilen, f) == (size_t)ilen)
                        parse_m4a_ilst_buf(ibuf, (size_t)ilen, title, tsz, artist, arsz, album, alsz, genre, gsz, year_out);
                    free(ibuf);
                }
            }
            return true;
        }
        if (fseeko(f, (off_t)next, SEEK_SET) != 0) break;
    }
    return false;
}

static void read_m4a_tags(const char *path, char *title, size_t tsz,
                          char *artist, size_t arsz, char *album, size_t alsz,
                          char *genre, size_t gsz, uint16_t *year_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseeko(f, 0, SEEK_END) == 0) {
        off_t sz = ftello(f);
        if (sz > 16 && fseeko(f, 0, SEEK_SET) == 0)
            scan_m4a_atoms(f, (uint64_t)sz, 0, title, tsz, artist, arsz, album, alsz, genre, gsz, year_out);
    }
    fclose(f);
}

static void trim_right(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

static const char * const k_id3v1_genres[80] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop",
    "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap",
    "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska", "Death Metal", "Pranks",
    "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance",
    "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
    "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi",
    "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock"
};

static void read_id3v1_tags(const char *path, char *title, size_t tsz,
                            char *artist, size_t arsz, char *album, size_t alsz,
                            char *genre, size_t gsz, uint16_t *year_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, -128, SEEK_END) == 0) {
        char tag[128];
        if (fread(tag, 1, 128, f) == 128 && memcmp(tag, "TAG", 3) == 0) {
            char t[31], ar[31], al[31], yr[5];
            memcpy(t,  tag + 3,  30); t[30]  = '\0'; trim_right(t);
            memcpy(ar, tag + 33, 30); ar[30] = '\0'; trim_right(ar);
            memcpy(al, tag + 63, 30); al[30] = '\0'; trim_right(al);
            memcpy(yr, tag + 93, 4);  yr[4]  = '\0'; trim_right(yr);
            if (t[0])  snprintf(title,  tsz,  "%s", t);
            if (ar[0]) snprintf(artist, arsz, "%s", ar);
            if (al[0]) snprintf(album,  alsz, "%s", al);
            if (year_out && yr[0]) {
                int y = atoi(yr);
                if (y >= 1000 && y <= 2999) *year_out = (uint16_t)y;
            }
            uint8_t g = (uint8_t)tag[127];
            if (genre && gsz > 0 && g < 80)
                snprintf(genre, gsz, "%s", k_id3v1_genres[g]);
        }
    }
    fclose(f);
}

static bool skip_normal_dir(const char *name)
{
    return (name[0] == '.' ||
            strcasecmp(name, "iPod_Control") == 0 ||
            strcasecmp(name, "Playlists") == 0 ||
            strcasecmp(name, "System Volume Information") == 0 ||
            strcasecmp(name, "LOST.DIR") == 0 ||
            strcasecmp(name, "Android") == 0);
}

struct scan_ctx {
    ipod_db_progress_cb cb;
    void               *user;
    int                 dirs_scanned;
    bool                cancelled;
};

static void scan_dir_mp3(struct ipod_db *db, const char *dir_path, int depth, bool skip_music_root, struct scan_ctx *ctx)
{
    if (depth > 24 || db->track_count >= DB_MAX_TRACKS || (ctx && ctx->cancelled)) return;
    DIR *d = opendir(dir_path);
    if (!d) return;
    if (ctx) {
        ctx->dirs_scanned++;
        if (ctx->cb && !ctx->cb(db->track_count, ctx->dirs_scanned, dir_path, ctx->user)) {
            ctx->cancelled = true;
            closedir(d);
            return;
        }
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && db->track_count < DB_MAX_TRACKS) {
        if (ctx && ctx->cancelled) break;
        if (de->d_name[0] == '.') continue;
        if (depth == 0 && skip_music_root && strcasecmp(de->d_name, "Music") == 0) continue;

        char full[512];
        int n = snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
        if (n <= 0 || n >= (int)sizeof(full)) continue;

        bool is_dir = false;
        bool is_reg = false;
#ifdef DT_DIR
        if (de->d_type == DT_DIR) {
            is_dir = true;
        } else if (de->d_type == DT_REG) {
            is_reg = true;
        } else
#endif
        {
            struct stat st;
            if (stat(full, &st) != 0) continue;
            is_dir = S_ISDIR(st.st_mode);
            is_reg = S_ISREG(st.st_mode);
        }

        if (is_dir) {
            if (skip_normal_dir(de->d_name)) continue;
            scan_dir_mp3(db, full, depth + 1, false, ctx);
        } else if (is_reg && has_audio_ext(de->d_name)) {
            if (!ensure_track_cap(db, db->track_count + 1)) break;
            char title[128], artist[96], album[96], genre[64];
            uint16_t year = 0;
            snprintf(title, sizeof(title), "%.127s", de->d_name);
            char *dot = strrchr(title, '.');
            if (dot) *dot = '\0';

            const char *folder = strrchr(dir_path, '/');
            folder = folder ? folder + 1 : dir_path;
            snprintf(artist, sizeof(artist), "Music");
            snprintf(album, sizeof(album), "%.95s", folder[0] ? folder : "SD Card");
            genre[0] = '\0';

            if (has_ext_ci(de->d_name, ".mp3"))
                read_id3v1_tags(full, title, sizeof(title), artist, sizeof(artist), album, sizeof(album), genre, sizeof(genre), &year);
            else if (has_ext_ci(de->d_name, ".m4a") || has_ext_ci(de->d_name, ".m4b") || has_ext_ci(de->d_name, ".mp4"))
                read_m4a_tags(full, title, sizeof(title), artist, sizeof(artist), album, sizeof(album), genre, sizeof(genre), &year);

            const char *fs_p = arena_strdup(db, full);
            const char *rb_p = (strncmp(fs_p, "/mnt/sdcard", 11) == 0) ? (fs_p + 11) : fs_p;

            struct ipod_track *tr = &db->tracks[db->track_count];
            tr->track_id    = (uint32_t)(db->track_count + 1);
            tr->title       = arena_strdup(db, title);
            tr->artist      = arena_strdup(db, artist);
            tr->album       = arena_strdup(db, album);
            tr->genre       = arena_strdup(db, genre);
            tr->fs_path     = fs_p;
            tr->rb_path     = rb_p;
            tr->duration_ms = 0;
            tr->track_num   = 0;
            tr->year        = year;
            db->track_count++;

            if (ctx && ctx->cb && (db->track_count & 31) == 0) {
                if (!ctx->cb(db->track_count, ctx->dirs_scanned, full, ctx->user)) {
                    ctx->cancelled = true;
                    break;
                }
            }
        }
    }
    closedir(d);
}

struct id_map_entry {
    uint32_t track_id;
    int      index;
};

static int cmp_id_map(const void *a, const void *b)
{
    const struct id_map_entry *ia = (const struct id_map_entry *)a;
    const struct id_map_entry *ib = (const struct id_map_entry *)b;
    if (ia->track_id < ib->track_id) return -1;
    if (ia->track_id > ib->track_id) return 1;
    return 0;
}

static int find_track_by_id_map(const struct id_map_entry *map, int n, uint32_t tid)
{
    if (!map || n <= 0) return -1;
    struct id_map_entry key = { .track_id = tid, .index = 0 };
    const struct id_map_entry *res = bsearch(&key, map, (size_t)n, sizeof(struct id_map_entry), cmp_id_map);
    return res ? res->index : -1;
}

void ipod_db_init(struct ipod_db *db)
{
    memset(db, 0, sizeof(*db));
}

void ipod_db_free(struct ipod_db *db)
{
    free(db->tracks);
    free(db->artists);
    free(db->albums);
    for (int i = 0; i < db->playlist_count; i++)
        free(db->playlists[i].track_indices);
    struct ipod_str_slab *s = db->str_slabs;
    while (s) {
        struct ipod_str_slab *nx = s->next;
        free(s);
        s = nx;
    }
    memset(db, 0, sizeof(*db));
}

static bool load_itunesdb_stream(struct ipod_db *db, FILE *f)
{
    if (fseeko(f, 0, SEEK_END) != 0) return false;
    off_t fsz_off = ftello(f);
    if (fsz_off <= 24) return false;
    uint64_t fsz = (uint64_t)fsz_off;
    if (fseeko(f, 0, SEEK_SET) != 0) return false;

    uint8_t hdr[24];
    if (fread(hdr, 1, 24, f) != 24 || memcmp(hdr, "mhbd", 4) != 0)
        return false;

    uint32_t mhbd_hdr = rd32(hdr + 4);
    uint32_t num_mhsd = rd32(hdr + 20);
    uint64_t pos = mhbd_hdr;

    uint8_t *chunk = NULL;
    size_t chunk_cap = 0;

    for (uint32_t s = 0; s < num_mhsd && pos + 16 <= fsz; s++) {
        uint8_t mhsd[16];
        if (fseeko(f, (off_t)pos, SEEK_SET) != 0) break;
        if (fread(mhsd, 1, 16, f) != 16 || memcmp(mhsd, "mhsd", 4) != 0) break;

        uint32_t mhsd_hdr  = rd32(mhsd + 4);
        uint32_t mhsd_len  = rd32(mhsd + 8);
        uint32_t mhsd_type = rd32(mhsd + 12);
        if (mhsd_len == 0 || pos + mhsd_len > fsz) break;

        uint64_t mhsd_end = pos + mhsd_len;
        uint64_t child = pos + mhsd_hdr;

        if (mhsd_type == 1 && child + 12 <= mhsd_end) {
            uint8_t mhlt[12];
            if (fseeko(f, (off_t)child, SEEK_SET) == 0 &&
                fread(mhlt, 1, 12, f) == 12 && memcmp(mhlt, "mhlt", 4) == 0) {
                uint32_t mhlt_hdr   = rd32(mhlt + 4);
                uint32_t num_tracks = rd32(mhlt + 8);
                uint64_t tp = child + mhlt_hdr;
                if (num_tracks > 0 && num_tracks <= DB_MAX_TRACKS)
                    ensure_track_cap(db, (int)num_tracks);

                for (uint32_t t = 0; t < num_tracks && tp + 48 <= mhsd_end; t++) {
                    uint8_t mhit_head[48];
                    if (fseeko(f, (off_t)tp, SEEK_SET) != 0) break;
                    if (fread(mhit_head, 1, 48, f) != 48 || memcmp(mhit_head, "mhit", 4) != 0) break;
                    uint32_t mhit_hdr = rd32(mhit_head + 4);
                    uint32_t mhit_len = rd32(mhit_head + 8);
                    uint32_t num_mhod = rd32(mhit_head + 12);
                    if (mhit_len < mhit_hdr || tp + mhit_len > mhsd_end) break;

                    size_t body_len = mhit_len - mhit_hdr;
                    if (body_len > chunk_cap) {
                        size_t nc = body_len < 65536 ? 65536 : body_len;
                        uint8_t *nb = realloc(chunk, nc);
                        if (!nb) break;
                        chunk = nb;
                        chunk_cap = nc;
                    }
                    if (body_len > 0) {
                        if (fseeko(f, (off_t)(tp + mhit_hdr), SEEK_SET) != 0) break;
                        if (fread(chunk, 1, body_len, f) != body_len) break;
                    }

                    char title[128] = "", artist[96] = "", album[96] = "", genre[64] = "", loc[384] = "";
                    size_t mp = 0;
                    for (uint32_t m = 0; m < num_mhod && mp + 16 <= body_len; m++) {
                        if (memcmp(chunk + mp, "mhod", 4) != 0) break;
                        uint32_t mhod_len  = rd32(chunk + mp + 8);
                        uint32_t mhod_type = rd32(chunk + mp + 12);
                        if (mhod_len == 0 || mp + mhod_len > body_len) break;
                        if (mhod_type == 1)      parse_mhod_string(chunk + mp, mhod_len, title, sizeof(title));
                        else if (mhod_type == 2) parse_mhod_string(chunk + mp, mhod_len, loc, sizeof(loc));
                        else if (mhod_type == 3) parse_mhod_string(chunk + mp, mhod_len, album, sizeof(album));
                        else if (mhod_type == 4) parse_mhod_string(chunk + mp, mhod_len, artist, sizeof(artist));
                        else if (mhod_type == 5) parse_mhod_string(chunk + mp, mhod_len, genre, sizeof(genre));
                        mp += mhod_len;
                    }

                    trim_right(loc);
                    if (loc[0]) {
                        for (char *c = loc; *c; c++) {
                            if (*c == ':') *c = '/';
                        }
                        char fs_buf[420];
                        if (loc[0] == '/')
                            snprintf(fs_buf, sizeof(fs_buf), "/mnt/sdcard%s", loc);
                        else
                            snprintf(fs_buf, sizeof(fs_buf), "/mnt/sdcard/%s", loc);

                        if (has_audio_ext(fs_buf) && ensure_track_cap(db, db->track_count + 1)) {
                            if (!title[0]) {
                                const char *sl = strrchr(fs_buf, '/');
                                snprintf(title, sizeof(title), "%.127s", sl ? sl + 1 : fs_buf);
                            }
                            const char *fs_p = arena_strdup(db, fs_buf);
                            const char *rb_p = (strncmp(fs_p, "/mnt/sdcard", 11) == 0) ? (fs_p + 11) : fs_p;

                            struct ipod_track *tr = &db->tracks[db->track_count];
                            uint32_t y = rd32(mhit_head + 36);
                            tr->track_id    = rd32(mhit_head + 16);
                            tr->year        = (y >= 1000 && y <= 2999) ? (uint16_t)y : 0;
                            tr->duration_ms = rd32(mhit_head + 40);
                            tr->track_num   = (uint16_t)rd32(mhit_head + 44);
                            tr->title       = arena_strdup(db, title);
                            tr->artist      = arena_strdup(db, artist);
                            tr->album       = arena_strdup(db, album);
                            tr->genre       = arena_strdup(db, genre);
                            tr->fs_path     = fs_p;
                            tr->rb_path     = rb_p;
                            db->track_count++;
                        }
                    }
                    tp += mhit_len;
                }
                if (db->track_count > 1)
                    qsort(db->tracks, (size_t)db->track_count, sizeof(struct ipod_track), cmp_track);
                db->from_itunesdb = true;
            }
        } else if (mhsd_type == 2 && child + 12 <= mhsd_end) {
            uint8_t mhlp[12];
            if (fseeko(f, (off_t)child, SEEK_SET) == 0 &&
                fread(mhlp, 1, 12, f) == 12 && memcmp(mhlp, "mhlp", 4) == 0) {
                uint32_t mhlp_hdr = rd32(mhlp + 4);
                uint32_t num_pl   = rd32(mhlp + 8);
                uint64_t pp = child + mhlp_hdr;

                struct id_map_entry *id_map = NULL;
                int id_map_n = db->track_count;
                if (id_map_n > 0) {
                    id_map = malloc(sizeof(struct id_map_entry) * (size_t)id_map_n);
                    if (id_map) {
                        for (int i = 0; i < id_map_n; i++) {
                            id_map[i].track_id = db->tracks[i].track_id;
                            id_map[i].index = i;
                        }
                        qsort(id_map, (size_t)id_map_n, sizeof(struct id_map_entry), cmp_id_map);
                    }
                }

                for (uint32_t p = 0; p < num_pl && pp + 24 <= mhsd_end; p++) {
                    uint8_t mhyp[24];
                    if (fseeko(f, (off_t)pp, SEEK_SET) != 0) break;
                    if (fread(mhyp, 1, 24, f) != 24 || memcmp(mhyp, "mhyp", 4) != 0) break;
                    uint32_t mhyp_hdr  = rd32(mhyp + 4);
                    uint32_t mhyp_len  = rd32(mhyp + 8);
                    uint32_t n_mhod    = rd32(mhyp + 12);
                    uint32_t n_mhip    = rd32(mhyp + 16);
                    uint8_t  is_master = mhyp[20];
                    if (mhyp_len < mhyp_hdr || pp + mhyp_len > mhsd_end) break;

                    if (!is_master && n_mhip > 0 && db->playlist_count < DB_MAX_PLAYLISTS) {
                        struct ipod_playlist *pl = &db->playlists[db->playlist_count];
                        memset(pl, 0, sizeof(*pl));
                        snprintf(pl->name, sizeof(pl->name), "Playlist %d", db->playlist_count + 1);
                        uint64_t mp = pp + mhyp_hdr;
                        uint64_t p_end = pp + mhyp_len;

                        for (uint32_t m = 0; m < n_mhod && mp + 16 <= p_end; m++) {
                            uint8_t mh[16];
                            if (fseeko(f, (off_t)mp, SEEK_SET) != 0) break;
                            if (fread(mh, 1, 16, f) != 16 || memcmp(mh, "mhod", 4) != 0) break;
                            uint32_t ml = rd32(mh + 8);
                            uint32_t mt = rd32(mh + 12);
                            if (ml == 0 || mp + ml > p_end) break;
                            if (mt == 1 && ml <= 4096) {
                                if (ml > chunk_cap) {
                                    uint8_t *nb = realloc(chunk, ml);
                                    if (nb) { chunk = nb; chunk_cap = ml; }
                                }
                                if (chunk && ml <= chunk_cap &&
                                    fseeko(f, (off_t)mp, SEEK_SET) == 0 &&
                                    fread(chunk, 1, ml, f) == ml) {
                                    parse_mhod_string(chunk, ml, pl->name, sizeof(pl->name));
                                }
                            }
                            mp += ml;
                        }

                        pl->track_indices = malloc(sizeof(int) * n_mhip);
                        if (pl->track_indices) {
                            for (uint32_t k = 0; k < n_mhip && mp + 28 <= p_end; k++) {
                                uint8_t mhip[28];
                                if (fseeko(f, (off_t)mp, SEEK_SET) != 0) break;
                                if (fread(mhip, 1, 28, f) != 28 || memcmp(mhip, "mhip", 4) != 0) break;
                                uint32_t il  = rd32(mhip + 8);
                                uint32_t tid = rd32(mhip + 24);
                                if (il == 0 || mp + il > p_end) break;
                                int ti = find_track_by_id_map(id_map, id_map_n, tid);
                                if (ti >= 0) pl->track_indices[pl->track_count++] = ti;
                                mp += il;
                            }
                            if (pl->track_count > 0) db->playlist_count++;
                            else { free(pl->track_indices); pl->track_indices = NULL; }
                        }
                    }
                    pp += mhyp_len;
                }
                free(id_map);
            }
        }
        pos += mhsd_len;
    }

    free(chunk);
    return db->track_count > 0;
}

bool ipod_db_load(struct ipod_db *db, bool use_ipod_db, ipod_db_progress_cb cb, void *user)
{
    ipod_db_free(db);
    if (!ensure_track_cap(db, 2048)) return false;

    struct scan_ctx sctx = { .cb = cb, .user = user, .dirs_scanned = 0, .cancelled = false };

    if (!use_ipod_db) {
        scan_dir_mp3(db, "/mnt/sdcard/Music", 0, false, &sctx);
        if (!sctx.cancelled)
            scan_dir_mp3(db, "/mnt/sdcard", 0, true, &sctx);
        if (db->track_count > 1)
            qsort(db->tracks, (size_t)db->track_count, sizeof(struct ipod_track), cmp_track);
        build_groups(db);
        return db->track_count > 0;
    }

    const char *db_paths[] = {
        "/mnt/sdcard/iPod_Control/iTunes/iTunesDB",
        "/iPod_Control/iTunes/iTunesDB",
        NULL
    };

    for (int i = 0; db_paths[i]; i++) {
        FILE *f = fopen(db_paths[i], "rb");
        if (!f) continue;
        bool ok = load_itunesdb_stream(db, f);
        fclose(f);
        if (ok) break;
    }

    if (db->track_count == 0) {
        scan_dir_mp3(db, "/mnt/sdcard/iPod_Control/Music", 0, false, &sctx);
        if (!sctx.cancelled)
            scan_dir_mp3(db, "/mnt/sdcard/Music", 0, false, &sctx);
        if (db->track_count > 1)
            qsort(db->tracks, (size_t)db->track_count, sizeof(struct ipod_track), cmp_track);
    }

    build_groups(db);
    return db->track_count > 0;
}
