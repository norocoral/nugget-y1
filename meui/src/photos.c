#include "photos.h"
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define THUMB_CACHE_SIZE 32
#define THUMB_W 132
#define THUMB_H 92
#define FULL_MAX_DIM 960

static struct photo_item *g_photos = NULL;
static int g_photo_count = 0;
static int g_photo_cap = 0;

struct thumb_slot {
    int       photo_idx;
    uint64_t  last_use;
    uint16_t *rgb565;
    int       w;
    int       h;
};

static struct thumb_slot g_thumbs[THUMB_CACHE_SIZE];
static uint64_t g_thumb_tick = 0;

static struct photo_bitmap g_full_bmp = { NULL, 0, 0 };
static int g_full_idx = -1;

static uint16_t rd16_ex(const uint8_t *p, bool le)
{
    return le ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd32_ex(const uint8_t *p, bool le)
{
    return le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
              : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

static uint64_t parse_exif_str_time(const char *s)
{
    int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;
    if (sscanf(s, "%d:%d:%d %d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) != 6)
        return 0;
    if (yr < 1970 || mo < 1 || mo > 12 || dy < 1 || dy > 31)
        return 0;
    struct tm tm_v;
    memset(&tm_v, 0, sizeof(tm_v));
    tm_v.tm_year = yr - 1900;
    tm_v.tm_mon  = mo - 1;
    tm_v.tm_mday = dy;
    tm_v.tm_hour = hr;
    tm_v.tm_min  = mn;
    tm_v.tm_sec  = sc;
    tm_v.tm_isdst = -1;
    time_t t = mktime(&tm_v);
    return (t > 0) ? (uint64_t)t : 0;
}

static uint64_t scan_ifd_date(const uint8_t *tiff, size_t tlen, uint32_t ifd_off, bool le, int depth)
{
    if (depth > 2 || ifd_off + 2 > tlen) return 0;
    uint16_t count = rd16_ex(tiff + ifd_off, le);
    uint64_t best = 0;
    uint32_t exif_ifd = 0;
    for (uint16_t i = 0; i < count; i++) {
        size_t eo = ifd_off + 2 + (size_t)i * 12;
        if (eo + 12 > tlen) break;
        uint16_t tag = rd16_ex(tiff + eo, le);
        uint16_t typ = rd16_ex(tiff + eo + 2, le);
        uint32_t cnt = rd32_ex(tiff + eo + 4, le);
        uint32_t val = rd32_ex(tiff + eo + 8, le);
        if (tag == 0x8769 && (typ == 4 || typ == 3)) {
            exif_ifd = val;
        } else if ((tag == 0x9003 || tag == 0x9004 || tag == 0x0132) && typ == 2 && cnt >= 19) {
            if ((size_t)val + 19 <= tlen) {
                char ds[24];
                memcpy(ds, tiff + val, 19);
                ds[19] = '\0';
                uint64_t ts = parse_exif_str_time(ds);
                if (ts > 0) {
                    if (tag == 0x9003) return ts; /* DateTimeOriginal wins */
                    if (best == 0) best = ts;
                }
            }
        }
    }
    if (exif_ifd > 0) {
        uint64_t sub = scan_ifd_date(tiff, tlen, exif_ifd, le, depth + 1);
        if (sub > 0) return sub;
    }
    return best;
}

static uint64_t read_jpeg_exif_date(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[2];
    if (fread(hdr, 1, 2, f) != 2 || hdr[0] != 0xFF || hdr[1] != 0xD8) {
        fclose(f);
        return 0;
    }
    uint64_t result = 0;
    for (int seg = 0; seg < 24; seg++) {
        uint8_t mk[4];
        if (fread(mk, 1, 2, f) != 2) break;
        if (mk[0] != 0xFF) break;
        while (mk[1] == 0xFF) {
            if (fread(&mk[1], 1, 1, f) != 1) { mk[1] = 0; break; }
        }
        if (mk[1] == 0xDA || mk[1] == 0xD9 || mk[1] == 0) break;
        uint8_t lbuf[2];
        if (fread(lbuf, 1, 2, f) != 2) break;
        uint16_t seg_len = (uint16_t)((lbuf[0] << 8) | lbuf[1]);
        if (seg_len < 2) break;
        size_t payload = seg_len - 2;
        if (mk[1] == 0xE1 && payload > 14 && payload <= 65536) {
            uint8_t *buf = malloc(payload);
            if (buf) {
                if (fread(buf, 1, payload, f) == payload &&
                    memcmp(buf, "Exif\0\0", 6) == 0) {
                    const uint8_t *tiff = buf + 6;
                    size_t tlen = payload - 6;
                    if (tlen >= 8 &&
                        ((tiff[0] == 'I' && tiff[1] == 'I') || (tiff[0] == 'M' && tiff[1] == 'M'))) {
                        bool le = (tiff[0] == 'I');
                        if (rd16_ex(tiff + 2, le) == 42) {
                            uint32_t ifd0 = rd32_ex(tiff + 4, le);
                            result = scan_ifd_date(tiff, tlen, ifd0, le, 0);
                        }
                    }
                }
                free(buf);
            }
            break;
        }
        if (fseek(f, (long)payload, SEEK_CUR) != 0) break;
    }
    fclose(f);
    return result;
}

static bool has_photo_ext(const char *name)
{
    size_t n = strlen(name);
    if (n >= 4 && (strcasecmp(name + n - 4, ".jpg") == 0 ||
                   strcasecmp(name + n - 4, ".png") == 0 ||
                   strcasecmp(name + n - 4, ".bmp") == 0))
        return true;
    if (n >= 5 && strcasecmp(name + n - 5, ".jpeg") == 0)
        return true;
    return false;
}

static void push_photo(const char *path, const char *name, uint64_t ts)
{
    if (g_photo_count >= g_photo_cap) {
        int nc = g_photo_cap > 0 ? g_photo_cap * 2 : 128;
        struct photo_item *ni = realloc(g_photos, sizeof(struct photo_item) * (size_t)nc);
        if (!ni) return;
        g_photos = ni;
        g_photo_cap = nc;
    }
    snprintf(g_photos[g_photo_count].path, sizeof(g_photos[0].path), "%s", path);
    snprintf(g_photos[g_photo_count].name, sizeof(g_photos[0].name), "%s", name);
    g_photos[g_photo_count].date_ts = ts;
    g_photo_count++;
}

static void scan_photo_dir(const char *dir, int depth)
{
    if (depth > 4) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[384];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_photo_dir(full, depth + 1);
        } else if (S_ISREG(st.st_mode) && has_photo_ext(de->d_name)) {
            uint64_t ts = 0;
            size_t nl = strlen(de->d_name);
            if ((nl >= 4 && strcasecmp(de->d_name + nl - 4, ".jpg") == 0) ||
                (nl >= 5 && strcasecmp(de->d_name + nl - 5, ".jpeg") == 0)) {
                ts = read_jpeg_exif_date(full);
            }
            if (ts == 0)
                ts = (uint64_t)st.st_mtime;
            push_photo(full, de->d_name, ts);
        }
    }
    closedir(d);
}

static int cmp_photo_asc(const void *a, const void *b)
{
    const struct photo_item *pa = (const struct photo_item *)a;
    const struct photo_item *pb = (const struct photo_item *)b;
    if (pa->date_ts < pb->date_ts) return -1;
    if (pa->date_ts > pb->date_ts) return 1;
    return strcasecmp(pa->path, pb->path);
}

static void clear_caches(void)
{
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        free(g_thumbs[i].rgb565);
        g_thumbs[i].rgb565 = NULL;
        g_thumbs[i].photo_idx = -1;
    }
    free(g_full_bmp.rgb565);
    g_full_bmp.rgb565 = NULL;
    g_full_bmp.w = 0;
    g_full_bmp.h = 0;
    g_full_idx = -1;
}

void photos_init(void)
{
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        g_thumbs[i].photo_idx = -1;
        g_thumbs[i].rgb565 = NULL;
    }
}

void photos_shutdown(void)
{
    clear_caches();
    free(g_photos);
    g_photos = NULL;
    g_photo_count = 0;
    g_photo_cap = 0;
}

void photos_scan(void)
{
    clear_caches();
    g_photo_count = 0;
    scan_photo_dir("/mnt/sdcard/Photos", 0);
    scan_photo_dir("/mnt/sdcard/iPod_Control/Photos", 0);
    if (g_photo_count > 1)
        qsort(g_photos, (size_t)g_photo_count, sizeof(struct photo_item), cmp_photo_asc);
}

int photos_count(void)
{
    return g_photo_count;
}

const struct photo_item *photos_get(int idx)
{
    if (idx < 0 || idx >= g_photo_count) return NULL;
    return &g_photos[idx];
}

static uint16_t *rgb888_to_rgb565_thumb_cover(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    uint16_t *out = malloc((size_t)dw * (size_t)dh * 2);
    if (!out) return NULL;
    float sx_r = (float)sw / (float)dw;
    float sy_r = (float)sh / (float)dh;
    float sc = (sx_r < sy_r) ? sx_r : sy_r;
    float crop_w = (float)dw * sc;
    float crop_h = (float)dh * sc;
    float off_x = ((float)sw - crop_w) * 0.5f;
    float off_y = ((float)sh - crop_h) * 0.5f;
    for (int y = 0; y < dh; y++) {
        int sy = (int)(off_y + ((float)y + 0.5f) * sc);
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;
        const uint8_t *srow = src + sy * sw * 3;
        uint16_t *drow = out + y * dw;
        for (int x = 0; x < dw; x++) {
            int sx = (int)(off_x + ((float)x + 0.5f) * sc);
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;
            const uint8_t *p = srow + sx * 3;
            drow[x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
    return out;
}

#define THUMB_DISK_DIR   "/mnt/sdcard/.meui/photo_thumbs"
#define THUMB_DISK_MAGIC 0x54484D31U

struct thumb_disk_hdr {
    uint32_t magic;
    uint32_t mtime;
    uint16_t w;
    uint16_t h;
};

static void make_thumb_cache_path(const char *src_path, char *out, size_t out_sz)
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)src_path; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    snprintf(out, out_sz, "%s/%016llx.thmb", THUMB_DISK_DIR, (unsigned long long)h);
}

static uint16_t *try_load_disk_thumb(const char *src_path, uint32_t mtime)
{
    char tp[256];
    make_thumb_cache_path(src_path, tp, sizeof(tp));
    FILE *f = fopen(tp, "rb");
    if (!f) return NULL;
    struct thumb_disk_hdr hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != THUMB_DISK_MAGIC || hdr.mtime != mtime ||
        hdr.w != THUMB_W || hdr.h != THUMB_H) {
        fclose(f);
        return NULL;
    }
    size_t px_bytes = (size_t)THUMB_W * (size_t)THUMB_H * 2;
    uint16_t *bmp = malloc(px_bytes);
    if (!bmp || fread(bmp, 1, px_bytes, f) != px_bytes) {
        free(bmp);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return bmp;
}

static void save_disk_thumb(const char *src_path, uint32_t mtime, const uint16_t *bmp)
{
    mkdir("/mnt/sdcard/.meui", 0777);
    mkdir(THUMB_DISK_DIR, 0777);
    char tp[256];
    make_thumb_cache_path(src_path, tp, sizeof(tp));
    FILE *f = fopen(tp, "wb");
    if (!f) return;
    struct thumb_disk_hdr hdr = { THUMB_DISK_MAGIC, mtime, THUMB_W, THUMB_H };
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(bmp, 1, (size_t)THUMB_W * (size_t)THUMB_H * 2, f);
    fclose(f);
}

static uint16_t *rgb888_to_rgb565_scaled(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    uint16_t *out = malloc((size_t)dw * (size_t)dh * 2);
    if (!out) return NULL;
    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        const uint8_t *srow = src + sy * sw * 3;
        uint16_t *drow = out + y * dw;
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            const uint8_t *p = srow + sx * 3;
            drow[x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
    return out;
}

static uint8_t *load_image_rgb(const char *path, int *sw, int *sh)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 32 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nr != (size_t)sz) { free(buf); return NULL; }
    int ch = 0;
    uint8_t *img = stbi_load_from_memory(buf, (int)sz, sw, sh, &ch, 3);
    free(buf);
    return img;
}

const struct photo_bitmap *photos_get_thumb(int idx)
{
    static struct photo_bitmap ret;
    if (idx < 0 || idx >= g_photo_count) return NULL;
    g_thumb_tick++;
    int lru = 0;
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (g_thumbs[i].photo_idx == idx && g_thumbs[i].rgb565) {
            g_thumbs[i].last_use = g_thumb_tick;
            ret.rgb565 = g_thumbs[i].rgb565;
            ret.w = g_thumbs[i].w;
            ret.h = g_thumbs[i].h;
            return &ret;
        }
        if (!g_thumbs[i].rgb565) {
            lru = i;
        } else if (g_thumbs[lru].rgb565 && g_thumbs[i].last_use < g_thumbs[lru].last_use) {
            lru = i;
        }
    }

    struct stat st;
    uint32_t cur_mtime = (stat(g_photos[idx].path, &st) == 0) ? (uint32_t)st.st_mtime : 0;
    uint16_t *bmp = try_load_disk_thumb(g_photos[idx].path, cur_mtime);
    if (!bmp) {
        int sw = 0, sh = 0;
        uint8_t *img = load_image_rgb(g_photos[idx].path, &sw, &sh);
        if (!img || sw <= 0 || sh <= 0) {
            if (img) stbi_image_free(img);
            return NULL;
        }
        bmp = rgb888_to_rgb565_thumb_cover(img, sw, sh, THUMB_W, THUMB_H);
        stbi_image_free(img);
        if (!bmp) return NULL;
        save_disk_thumb(g_photos[idx].path, cur_mtime, bmp);
    }

    free(g_thumbs[lru].rgb565);
    g_thumbs[lru].photo_idx = idx;
    g_thumbs[lru].last_use = g_thumb_tick;
    g_thumbs[lru].rgb565 = bmp;
    g_thumbs[lru].w = THUMB_W;
    g_thumbs[lru].h = THUMB_H;

    ret.rgb565 = bmp;
    ret.w = THUMB_W;
    ret.h = THUMB_H;
    return &ret;
}

const struct photo_bitmap *photos_get_full(int idx)
{
    if (idx < 0 || idx >= g_photo_count) return NULL;
    if (g_full_idx == idx && g_full_bmp.rgb565)
        return &g_full_bmp;

    free(g_full_bmp.rgb565);
    g_full_bmp.rgb565 = NULL;
    g_full_bmp.w = 0;
    g_full_bmp.h = 0;
    g_full_idx = -1;

    int sw = 0, sh = 0;
    uint8_t *img = load_image_rgb(g_photos[idx].path, &sw, &sh);
    if (!img || sw <= 0 || sh <= 0) {
        if (img) stbi_image_free(img);
        return NULL;
    }
    int dw = sw, dh = sh;
    if (dw > FULL_MAX_DIM || dh > FULL_MAX_DIM) {
        if (dw >= dh) {
            dh = (dh * FULL_MAX_DIM) / dw;
            dw = FULL_MAX_DIM;
        } else {
            dw = (dw * FULL_MAX_DIM) / dh;
            dh = FULL_MAX_DIM;
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    uint16_t *bmp = rgb888_to_rgb565_scaled(img, sw, sh, dw, dh);
    stbi_image_free(img);
    if (!bmp) return NULL;

    g_full_bmp.rgb565 = bmp;
    g_full_bmp.w = dw;
    g_full_bmp.h = dh;
    g_full_idx = idx;
    return &g_full_bmp;
}
