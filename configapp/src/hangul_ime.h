#ifndef HANGUL_IME_H
#define HANGUL_IME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

struct hangul_ime {
    int    cho;
    int    jung;
    int    jong;
    size_t commit_len;
};

static inline void hime_reset(struct hangul_ime *ime)
{
    ime->cho = -1;
    ime->jung = -1;
    ime->jong = -1;
    ime->commit_len = 0;
}

static inline void hime_commit(struct hangul_ime *ime, const char *buf)
{
    ime->commit_len = strlen(buf);
    ime->cho = -1;
    ime->jung = -1;
    ime->jong = -1;
}

static inline int hime_encode_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
}

static inline uint32_t hime_decode_utf8(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    if (!u || !u[0]) return 0;
    if (u[0] < 0x80) return u[0];
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80)
        return ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80)
        return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    return 0;
}

static inline int hime_cho_idx(uint32_t cp)
{
    static const uint16_t k_cho[19] = {
        0x3131,0x3132,0x3134,0x3137,0x3138,0x3139,0x3141,0x3142,0x3143,0x3145,
        0x3146,0x3147,0x3148,0x3149,0x314A,0x314B,0x314C,0x314D,0x314E
    };
    for (int i = 0; i < 19; i++) if (k_cho[i] == cp) return i;
    return -1;
}

static inline int hime_jung_idx(uint32_t cp)
{
    if (cp >= 0x314F && cp <= 0x3163) return (int)(cp - 0x314F);
    return -1;
}

static inline int hime_cho_to_jong(int cho)
{
    static const int8_t map[19] = {
        1, 2, 4, 7, -1, 8, 16, 17, -1, 19, 20, 21, 22, -1, 23, 24, 25, 26, 27
    };
    return (cho >= 0 && cho < 19) ? map[cho] : -1;
}

static inline int hime_jong_to_cho(int jong)
{
    static const int8_t map[28] = {
        -1, 0, 1, -1, 2, -1, -1, 3, 5, -1, -1, -1, -1, -1, -1, -1,
        6, 7, -1, 9, 10, 11, 12, 14, 15, 16, 17, 18
    };
    return (jong >= 0 && jong < 28) ? map[jong] : -1;
}

static inline int hime_combine_vowel(int v1, int v2)
{
    if (v1 == 8) {
        if (v2 == 0) return 9;
        if (v2 == 1) return 10;
        if (v2 == 20) return 11;
    } else if (v1 == 13) {
        if (v2 == 4) return 14;
        if (v2 == 5) return 15;
        if (v2 == 20) return 16;
    } else if (v1 == 18 && v2 == 20) {
        return 19;
    }
    return -1;
}

static inline int hime_split_vowel_first(int v)
{
    if (v == 9 || v == 10 || v == 11) return 8;
    if (v == 14 || v == 15 || v == 16) return 13;
    if (v == 19) return 18;
    return -1;
}

static inline int hime_combine_jong(int j1, int cho2)
{
    if (j1 == 1 && cho2 == 9) return 3;
    if (j1 == 4) {
        if (cho2 == 12) return 5;
        if (cho2 == 18) return 6;
    } else if (j1 == 8) {
        if (cho2 == 0) return 9;
        if (cho2 == 6) return 10;
        if (cho2 == 7) return 11;
        if (cho2 == 9) return 12;
        if (cho2 == 16) return 13;
        if (cho2 == 17) return 14;
        if (cho2 == 18) return 15;
    } else if (j1 == 17 && cho2 == 9) {
        return 18;
    }
    return -1;
}

static inline bool hime_split_jong(int j, int *j1, int *cho2)
{
    switch (j) {
        case 3:  *j1 = 1;  *cho2 = 9;  return true;
        case 5:  *j1 = 4;  *cho2 = 12; return true;
        case 6:  *j1 = 4;  *cho2 = 18; return true;
        case 9:  *j1 = 8;  *cho2 = 0;  return true;
        case 10: *j1 = 8;  *cho2 = 6;  return true;
        case 11: *j1 = 8;  *cho2 = 7;  return true;
        case 12: *j1 = 8;  *cho2 = 9;  return true;
        case 13: *j1 = 8;  *cho2 = 16; return true;
        case 14: *j1 = 8;  *cho2 = 17; return true;
        case 15: *j1 = 8;  *cho2 = 18; return true;
        case 18: *j1 = 17; *cho2 = 9;  return true;
        default: return false;
    }
}

static inline void hime_render_composing(struct hangul_ime *ime, char *buf, size_t buf_sz)
{
    static const uint16_t k_cho[19] = {
        0x3131,0x3132,0x3134,0x3137,0x3138,0x3139,0x3141,0x3142,0x3143,0x3145,
        0x3146,0x3147,0x3148,0x3149,0x314A,0x314B,0x314C,0x314D,0x314E
    };
    if (ime->cho < 0) {
        buf[ime->commit_len] = '\0';
        return;
    }
    uint32_t cp = (ime->jung >= 0)
        ? (uint32_t)(0xAC00 + ime->cho * 588 + ime->jung * 28 + (ime->jong >= 0 ? ime->jong : 0))
        : (uint32_t)k_cho[ime->cho];
    if (ime->commit_len + 4 <= buf_sz) {
        int n = hime_encode_utf8(cp, buf + ime->commit_len);
        buf[ime->commit_len + (size_t)n] = '\0';
    }
}

static inline void hime_input_key(struct hangul_ime *ime, const char *lbl, char *buf, size_t buf_sz)
{
    uint32_t cp = hime_decode_utf8(lbl);
    int c = hime_cho_idx(cp);
    int v = hime_jung_idx(cp);

    if (c < 0 && v < 0) {
        hime_commit(ime, buf);
        size_t len = strlen(buf);
        size_t kl = strlen(lbl);
        if (len + kl < buf_sz) {
            memcpy(buf + len, lbl, kl + 1);
            ime->commit_len = len + kl;
        }
        return;
    }

    if (c >= 0) {
        if (ime->cho < 0) {
            ime->cho = c;
            ime->jung = -1;
            ime->jong = -1;
            hime_render_composing(ime, buf, buf_sz);
        } else if (ime->jung < 0) {
            hime_commit(ime, buf);
            ime->cho = c;
            hime_render_composing(ime, buf, buf_sz);
        } else if (ime->jong < 0) {
            int j = hime_cho_to_jong(c);
            if (j >= 0) {
                ime->jong = j;
                hime_render_composing(ime, buf, buf_sz);
            } else {
                hime_commit(ime, buf);
                ime->cho = c;
                hime_render_composing(ime, buf, buf_sz);
            }
        } else {
            int cj = hime_combine_jong(ime->jong, c);
            if (cj >= 0) {
                ime->jong = cj;
                hime_render_composing(ime, buf, buf_sz);
            } else {
                hime_commit(ime, buf);
                ime->cho = c;
                hime_render_composing(ime, buf, buf_sz);
            }
        }
    } else {
        if (ime->cho < 0) {
            hime_commit(ime, buf);
            size_t len = strlen(buf);
            if (len + 4 <= buf_sz) {
                int n = hime_encode_utf8(cp, buf + len);
                buf[len + (size_t)n] = '\0';
                ime->commit_len = len + (size_t)n;
            }
        } else if (ime->jung < 0) {
            ime->jung = v;
            hime_render_composing(ime, buf, buf_sz);
        } else if (ime->jong < 0) {
            int cv = hime_combine_vowel(ime->jung, v);
            if (cv >= 0) {
                ime->jung = cv;
                hime_render_composing(ime, buf, buf_sz);
            } else {
                hime_commit(ime, buf);
                size_t len = strlen(buf);
                if (len + 4 <= buf_sz) {
                    int n = hime_encode_utf8(cp, buf + len);
                    buf[len + (size_t)n] = '\0';
                    ime->commit_len = len + (size_t)n;
                }
            }
        } else {
            int j1 = 0, cho2 = 0;
            if (hime_split_jong(ime->jong, &j1, &cho2)) {
                ime->jong = j1;
                hime_render_composing(ime, buf, buf_sz);
                hime_commit(ime, buf);
                ime->cho = cho2;
                ime->jung = v;
                hime_render_composing(ime, buf, buf_sz);
            } else {
                int nc = hime_jong_to_cho(ime->jong);
                ime->jong = -1;
                hime_render_composing(ime, buf, buf_sz);
                hime_commit(ime, buf);
                if (nc >= 0) {
                    ime->cho = nc;
                    ime->jung = v;
                    hime_render_composing(ime, buf, buf_sz);
                }
            }
        }
    }
}

static inline void hime_backspace(struct hangul_ime *ime, char *buf)
{
    if (ime->jong >= 0) {
        int j1 = 0, c2 = 0;
        if (hime_split_jong(ime->jong, &j1, &c2))
            ime->jong = j1;
        else
            ime->jong = -1;
        hime_render_composing(ime, buf, 128);
        return;
    }
    if (ime->jung >= 0) {
        int v1 = hime_split_vowel_first(ime->jung);
        ime->jung = v1;
        hime_render_composing(ime, buf, 128);
        return;
    }
    if (ime->cho >= 0) {
        ime->cho = -1;
        buf[ime->commit_len] = '\0';
        return;
    }
    size_t len = strlen(buf);
    if (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--;
        buf[len] = '\0';
        ime->commit_len = len;
    }
}

#endif /* HANGUL_IME_H */
