#include "album_art.h"
#include "liby1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include "stb_image.h"

#define ART_CACHE_SIZE 48
#define ART_MAX_DIM    160

struct art_slot {
    char              key[256];
    bool              valid;
    bool              has_art;
    struct art_bitmap bmp;
    uint32_t          stamp;
};

static struct art_slot g_cache[ART_CACHE_SIZE];
static uint32_t        g_stamp = 1;

void album_art_init(void)
{
    memset(g_cache, 0, sizeof(g_cache));
    g_stamp = 1;
}

void album_art_shutdown(void)
{
    for (int i = 0; i < ART_CACHE_SIZE; i++) {
        free(g_cache[i].bmp.rgb565);
        g_cache[i].bmp.rgb565 = NULL;
        g_cache[i].valid = false;
    }
}

static bool decode_mem_to_rgb565(const uint8_t *data, size_t len, struct art_bitmap *out)
{
    int iw = 0, ih = 0, ch = 0;
    unsigned char *rgb = stbi_load_from_memory(data, (int)len, &iw, &ih, &ch, 3);
    if (!rgb || iw <= 0 || ih <= 0) {
        if (rgb) stbi_image_free(rgb);
        return false;
    }

    int dw = iw, dh = ih;
    if (dw > ART_MAX_DIM || dh > ART_MAX_DIM) {
        if (dw >= dh) {
            dh = (dh * ART_MAX_DIM) / dw;
            dw = ART_MAX_DIM;
        } else {
            dw = (dw * ART_MAX_DIM) / dh;
            dh = ART_MAX_DIM;
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    uint16_t *px = malloc((size_t)dw * (size_t)dh * sizeof(uint16_t));
    if (!px) {
        stbi_image_free(rgb);
        return false;
    }

    for (int y = 0; y < dh; y++) {
        int sy = (y * ih) / dh;
        for (int x = 0; x < dw; x++) {
            int sx = (x * iw) / dw;
            const unsigned char *p = rgb + (sy * iw + sx) * 3;
            uint16_t r5 = (uint16_t)(p[0] >> 3);
            uint16_t g6 = (uint16_t)(p[1] >> 2);
            uint16_t b5 = (uint16_t)(p[2] >> 3);
            px[y * dw + x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }

    stbi_image_free(rgb);
    out->rgb565 = px;
    out->w = dw;
    out->h = dh;
    return true;
}

static bool try_id3v2_apic(const char *mp3_path, struct art_bitmap *out)
{
    FILE *f = fopen(mp3_path, "rb");
    if (!f) return false;
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) {
        fclose(f);
        return false;
    }
    uint8_t ver = hdr[3];
    uint32_t tag_sz = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                      ((uint32_t)(hdr[7] & 0x7F) << 14) |
                      ((uint32_t)(hdr[8] & 0x7F) << 7)  |
                      ((uint32_t)(hdr[9] & 0x7F));
    if (tag_sz == 0 || tag_sz > 2 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    uint8_t *buf = malloc(tag_sz);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t rd = fread(buf, 1, tag_sz, f);
    fclose(f);

    bool ok = false;
    size_t pos = 0;
    while (pos + 10 < rd) {
        if (buf[pos] == 0) break;
        uint32_t fsz;
        if (ver == 4) {
            fsz = ((uint32_t)(buf[pos + 4] & 0x7F) << 21) |
                  ((uint32_t)(buf[pos + 5] & 0x7F) << 14) |
                  ((uint32_t)(buf[pos + 6] & 0x7F) << 7)  |
                  ((uint32_t)(buf[pos + 7] & 0x7F));
        } else {
            fsz = ((uint32_t)buf[pos + 4] << 24) |
                  ((uint32_t)buf[pos + 5] << 16) |
                  ((uint32_t)buf[pos + 6] << 8)  |
                  ((uint32_t)buf[pos + 7]);
        }
        if (fsz == 0 || pos + 10 + fsz > rd) break;
        if (memcmp(buf + pos, "APIC", 4) == 0 && fsz > 16) {
            const uint8_t *p = buf + pos + 10;
            size_t rem = fsz;
            uint8_t enc = p[0];
            size_t idx = 1;
            while (idx < rem && p[idx] != 0) idx++; /* MIME */
            idx++; /* skip NUL */
            idx++; /* skip picture type */
            if (enc == 1 || enc == 2) {
                while (idx + 1 < rem && !(p[idx] == 0 && p[idx + 1] == 0)) idx += 2;
                idx += 2;
            } else {
                while (idx < rem && p[idx] != 0) idx++;
                idx++;
            }
            if (idx < rem) {
                ok = decode_mem_to_rgb565(p + idx, rem - idx, out);
                if (ok) break;
            }
        }
        pos += 10 + fsz;
    }
    free(buf);
    return ok;
}

static uint32_t art_rd32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool scan_m4a_covr(FILE *f, uint64_t end_off, int depth, struct art_bitmap *out)
{
    if (depth > 6) return false;
    while (1) {
        uint64_t cur = (uint64_t)ftell(f);
        if (cur + 8 > end_off) break;
        uint8_t h[8];
        if (fread(h, 1, 8, f) != 8) break;
        uint64_t sz = art_rd32_be(h);
        uint64_t hdr = 8;
        if (sz == 1) {
            uint8_t ext[8];
            if (fread(ext, 1, 8, f) != 8) break;
            sz = ((uint64_t)art_rd32_be(ext) << 32) | art_rd32_be(ext + 4);
            hdr = 16;
        } else if (sz == 0) {
            sz = end_off - cur;
        }
        if (sz < hdr || cur + sz > end_off) break;
        uint64_t next = cur + sz;

        if (memcmp(h + 4, "moov", 4) == 0 || memcmp(h + 4, "udta", 4) == 0 ||
            memcmp(h + 4, "ilst", 4) == 0 || memcmp(h + 4, "covr", 4) == 0) {
            if (scan_m4a_covr(f, next, depth + 1, out)) return true;
        } else if (memcmp(h + 4, "meta", 4) == 0) {
            if (fseek(f, (long)(cur + hdr + 4), SEEK_SET) == 0) {
                if (scan_m4a_covr(f, next, depth + 1, out)) return true;
            }
        } else if (memcmp(h + 4, "data", 4) == 0 && sz > hdr + 8 && sz - hdr <= 2 * 1024 * 1024) {
            uint8_t dh[8];
            if (fread(dh, 1, 8, f) == 8) {
                uint32_t dtype = art_rd32_be(dh);
                if (dtype == 13 || dtype == 14) { /* JPEG or PNG */
                    size_t ilen = (size_t)(sz - hdr - 8);
                    uint8_t *ibuf = malloc(ilen);
                    if (ibuf) {
                        if (fread(ibuf, 1, ilen, f) == ilen && decode_mem_to_rgb565(ibuf, ilen, out)) {
                            free(ibuf);
                            return true;
                        }
                        free(ibuf);
                    }
                }
            }
        }
        if (fseek(f, (long)next, SEEK_SET) != 0) break;
    }
    return false;
}

static bool try_m4a_covr(const char *path, struct art_bitmap *out)
{
    const char *dot = strrchr(path, '.');
    if (!dot || (strcasecmp(dot, ".m4a") != 0 && strcasecmp(dot, ".m4b") != 0 && strcasecmp(dot, ".mp4") != 0))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    bool ok = false;
    if (sz > 16 && fseek(f, 0, SEEK_SET) == 0)
        ok = scan_m4a_covr(f, (uint64_t)sz, 0, out);
    fclose(f);
    return ok;
}

static bool try_folder_cover(const char *mp3_path, struct art_bitmap *out)
{
    const char *sl = strrchr(mp3_path, '/');
    if (!sl) return false;
    size_t dlen = (size_t)(sl - mp3_path);
    if (dlen >= 220) return false;

    static const char *names[] = { "cover.jpg", "folder.jpg", "album.jpg", "cover.png", "folder.png", NULL };
    char path[256];
    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof(path), "%.*s/%s", (int)dlen, mp3_path, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz > 64 && sz < 2 * 1024 * 1024) {
            fseek(f, 0, SEEK_SET);
            uint8_t *buf = malloc((size_t)sz);
            if (buf) {
                size_t rd = fread(buf, 1, (size_t)sz, f);
                bool ok = decode_mem_to_rgb565(buf, rd, out);
                free(buf);
                if (ok) { fclose(f); return true; }
            }
        }
        fclose(f);
    }
    return false;
}

const struct art_bitmap *album_art_get_for_track(const struct ipod_track *tr)
{
    if (!tr || !tr->fs_path[0]) return NULL;

    /* Cache lookup by album+artist (or path if unknown album) */
    char key[256];
    if (tr->album[0])
        snprintf(key, sizeof(key), "%s::%s", tr->artist, tr->album);
    else
        snprintf(key, sizeof(key), "%s", tr->fs_path);

    int lru_idx = 0;
    uint32_t oldest = 0xFFFFFFFFU;
    for (int i = 0; i < ART_CACHE_SIZE; i++) {
        if (g_cache[i].valid && strcmp(g_cache[i].key, key) == 0) {
            g_cache[i].stamp = ++g_stamp;
            return g_cache[i].has_art ? &g_cache[i].bmp : NULL;
        }
        if (!g_cache[i].valid) {
            lru_idx = i;
            oldest = 0;
        } else if (g_cache[i].stamp < oldest) {
            oldest = g_cache[i].stamp;
            lru_idx = i;
        }
    }

    struct art_slot *slot = &g_cache[lru_idx];
    free(slot->bmp.rgb565);
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->key, sizeof(slot->key), "%s", key);
    slot->valid = true;
    slot->stamp = ++g_stamp;

    /* 1. Native iPod ArtworkDB */
    if (tr->rb_path[0] &&
        y1_ipod_native_load_albumart_rgb565(tr->rb_path, &slot->bmp.rgb565, &slot->bmp.w, &slot->bmp.h)) {
        slot->has_art = true;
        return &slot->bmp;
    }
    /* 2. Embedded ID3v2 APIC or M4A covr */
    if (try_id3v2_apic(tr->fs_path, &slot->bmp) || try_m4a_covr(tr->fs_path, &slot->bmp)) {
        slot->has_art = true;
        return &slot->bmp;
    }
    /* 3. Folder cover.jpg / folder.jpg */
    if (try_folder_cover(tr->fs_path, &slot->bmp)) {
        slot->has_art = true;
        return &slot->bmp;
    }

    slot->has_art = false;
    return NULL;
}
