#ifndef CONFIGAPP_ZMODEM_H
#define CONFIGAPP_ZMODEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <time.h>

#define ZPAD   '*'
#define ZDLE   0x18
#define ZBIN   'A'
#define ZHEX   'B'
#define ZBIN32 'C'

#define ZRQINIT    0
#define ZRINIT     1
#define ZSINIT     2
#define ZACK       3
#define ZFILE      4
#define ZSKIP      5
#define ZNAK       6
#define ZABORT     7
#define ZFIN       8
#define ZRPOS      9
#define ZDATA      10
#define ZEOF       11
#define ZFERR      12
#define ZCRC       13
#define ZCHALLENGE 14
#define ZCOMPL     15
#define ZCAN       16

#define ZCRCE 'h'
#define ZCRCG 'i'
#define ZCRCQ 'j'
#define ZCRCW 'k'

#define ZCANFDX  0x01
#define ZCANOVIO 0x02

static uint16_t zm_crc16_byte(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

static uint16_t zm_crc16_buf(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++)
        crc = zm_crc16_byte(crc, buf[i]);
    return crc;
}

static void zm_write_raw(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(STDOUT_FILENO, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        off += (size_t)w;
    }
}

static int zm_read_byte(int timeout_ds)
{
    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &tio) == 0) {
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = (cc_t)(timeout_ds > 255 ? 255 : timeout_ds);
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
    }
    uint8_t c = 0;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return (n == 1) ? (int)c : -1;
}

static int zm_read_zdle_byte(int timeout_ds)
{
    int c = zm_read_byte(timeout_ds);
    if (c < 0) return -1;
    if (c != ZDLE) return c;
    c = zm_read_byte(timeout_ds);
    if (c < 0) return -1;
    if (c == ZCRCE || c == ZCRCG || c == ZCRCQ || c == ZCRCW)
        return 0x100 | c;
    if (c == ZDLE) {
        /* 5 consecutive CANs = abort */
        return -2;
    }
    return c ^ 0x40;
}

static void zm_send_zdle_byte(uint8_t b, uint8_t *prev_sent)
{
    bool esc = (b == ZDLE || b == 0x10 || b == 0x90 || b == 0x11 || b == 0x91 ||
                b == 0x13 || b == 0x93 || b == 0xFF);
    if (!esc && b == '@' && (*prev_sent == 0x0D || *prev_sent == 0x8D))
        esc = true;
    if (esc) {
        uint8_t seq[2] = { ZDLE, (uint8_t)(b ^ 0x40) };
        zm_write_raw(seq, 2);
    } else {
        zm_write_raw(&b, 1);
    }
    *prev_sent = b;
}

static void zm_send_hex_hdr(uint8_t type, uint32_t pos)
{
    uint8_t hdr[5] = {
        type,
        (uint8_t)(pos & 0xFF),
        (uint8_t)((pos >> 8) & 0xFF),
        (uint8_t)((pos >> 16) & 0xFF),
        (uint8_t)((pos >> 24) & 0xFF)
    };
    uint16_t crc = zm_crc16_buf(hdr, 5);
    char out[32];
    int n = snprintf(out, sizeof(out), "**\x18B%02x%02x%02x%02x%02x%02x%02x\r\n",
                     hdr[0], hdr[1], hdr[2], hdr[3], hdr[4],
                     (crc >> 8) & 0xFF, crc & 0xFF);
    zm_write_raw(out, (size_t)n);
    if (type != ZFIN && type != ZACK) {
        uint8_t xon = 0x11;
        zm_write_raw(&xon, 1);
    }
}

static void zm_send_bin_hdr(uint8_t type, uint32_t pos)
{
    uint8_t hdr[5] = {
        type,
        (uint8_t)(pos & 0xFF),
        (uint8_t)((pos >> 8) & 0xFF),
        (uint8_t)((pos >> 16) & 0xFF),
        (uint8_t)((pos >> 24) & 0xFF)
    };
    uint16_t crc = zm_crc16_buf(hdr, 5);
    uint8_t pre[3] = { ZPAD, ZDLE, ZBIN };
    zm_write_raw(pre, 3);
    uint8_t prev = 0;
    for (int i = 0; i < 5; i++) zm_send_zdle_byte(hdr[i], &prev);
    zm_send_zdle_byte((uint8_t)(crc >> 8), &prev);
    zm_send_zdle_byte((uint8_t)(crc & 0xFF), &prev);
}

static void zm_send_subpkt(const uint8_t *data, size_t len, uint8_t frameend)
{
    uint8_t prev = 0;
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        zm_send_zdle_byte(data[i], &prev);
        crc = zm_crc16_byte(crc, data[i]);
    }
    crc = zm_crc16_byte(crc, frameend);
    uint8_t end_seq[2] = { ZDLE, frameend };
    zm_write_raw(end_seq, 2);
    zm_send_zdle_byte((uint8_t)(crc >> 8), &prev);
    zm_send_zdle_byte((uint8_t)(crc & 0xFF), &prev);
}

static int zm_hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int zm_read_hex_byte(int timeout_ds)
{
    int h = zm_read_byte(timeout_ds);
    int l = zm_read_byte(timeout_ds);
    int hv = zm_hex_val(h), lv = zm_hex_val(l);
    if (hv < 0 || lv < 0) return -1;
    return (hv << 4) | lv;
}

static int zm_recv_hdr(uint8_t *out_type, uint32_t *out_pos, int timeout_ds)
{
    int can_cnt = 0;
    for (int tries = 0; tries < 256; tries++) {
        int c = zm_read_byte(timeout_ds);
        if (c < 0) return -1;
        if (c == ZDLE) {
            can_cnt++;
            if (can_cnt >= 5) return -2;
            continue;
        }
        can_cnt = 0;
        if (c != ZPAD) continue;
        while ((c = zm_read_byte(timeout_ds)) == ZPAD) {}
        if (c != ZDLE) continue;
        int enc = zm_read_byte(timeout_ds);
        if (enc == ZBIN) {
            uint8_t b[7];
            for (int i = 0; i < 7; i++) {
                int v = zm_read_zdle_byte(timeout_ds);
                if (v < 0 || v >= 0x100) return -1;
                b[i] = (uint8_t)v;
            }
            uint16_t rx_crc = (uint16_t)((b[5] << 8) | b[6]);
            if (zm_crc16_buf(b, 5) != rx_crc) continue;
            *out_type = b[0];
            *out_pos = (uint32_t)b[1] | ((uint32_t)b[2] << 8) |
                       ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 24);
            return 0;
        } else if (enc == ZHEX) {
            uint8_t b[7];
            for (int i = 0; i < 7; i++) {
                int v = zm_read_hex_byte(timeout_ds);
                if (v < 0) return -1;
                b[i] = (uint8_t)v;
            }
            uint16_t rx_crc = (uint16_t)((b[5] << 8) | b[6]);
            if (zm_crc16_buf(b, 5) != rx_crc) continue;
            *out_type = b[0];
            *out_pos = (uint32_t)b[1] | ((uint32_t)b[2] << 8) |
                       ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 24);
            return 0;
        }
    }
    return -1;
}

static int zm_recv_subpkt(uint8_t *buf, size_t max_len, size_t *out_len, uint8_t *out_end, int timeout_ds)
{
    size_t n = 0;
    uint16_t crc = 0;
    while (1) {
        int v = zm_read_zdle_byte(timeout_ds);
        if (v < 0) return -1;
        if (v >= 0x100) {
            uint8_t endc = (uint8_t)(v & 0xFF);
            crc = zm_crc16_byte(crc, endc);
            int c1 = zm_read_zdle_byte(timeout_ds);
            int c2 = zm_read_zdle_byte(timeout_ds);
            if (c1 < 0 || c2 < 0) return -1;
            uint16_t rx_crc = (uint16_t)((c1 << 8) | c2);
            if (rx_crc != crc) return -3;
            *out_len = n;
            *out_end = endc;
            return 0;
        }
        if (n < max_len) buf[n++] = (uint8_t)v;
        crc = zm_crc16_byte(crc, (uint8_t)v);
    }
}

static int zmodem_send_buf(const char *filename, const uint8_t *data, size_t len)
{
    struct termios orig_tio, raw_tio;
    bool have_tio = (tcgetattr(STDIN_FILENO, &orig_tio) == 0);
    if (have_tio) {
        raw_tio = orig_tio;
        cfmakeraw(&raw_tio);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_tio);
    }

    int rc = 1;
    zm_write_raw("rz\r", 3);
    zm_send_hex_hdr(ZRQINIT, 0);

    uint8_t rtype = 0;
    uint32_t rpos = 0;
    for (int i = 0; i < 6; i++) {
        if (zm_recv_hdr(&rtype, &rpos, 50) == 0 && rtype == ZRINIT) break;
        zm_send_hex_hdr(ZRQINIT, 0);
    }
    if (rtype != ZRINIT) goto done;

    const char *base = strrchr(filename, '/');
    base = base ? (base + 1) : filename;

    uint8_t finfo[256];
    int flen = snprintf((char *)finfo, sizeof(finfo), "%s%c%zu 0 0 0 1 %zu",
                        base, '\0', len, len);
    if (flen < 0) goto done;
    finfo[flen++] = '\0';

    zm_send_bin_hdr(ZFILE, 0);
    zm_send_subpkt(finfo, (size_t)flen, ZCRCW);

    if (zm_recv_hdr(&rtype, &rpos, 50) != 0) goto done;
    if (rtype == ZSKIP) { rc = 0; goto finish; }
    if (rtype != ZRPOS) goto done;

    size_t off = (size_t)rpos;
    if (off > len) off = len;
    zm_send_bin_hdr(ZDATA, (uint32_t)off);
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 1024) chunk = 1024;
        uint8_t fend = (off + chunk >= len) ? ZCRCE : ZCRCG;
        zm_send_subpkt(data + off, chunk, fend);
        off += chunk;
    }

    zm_send_bin_hdr(ZEOF, (uint32_t)len);
    for (int i = 0; i < 4; i++) {
        if (zm_recv_hdr(&rtype, &rpos, 50) == 0 && rtype == ZRINIT) break;
    }
    rc = 0;

finish:
    zm_send_bin_hdr(ZFIN, 0);
    zm_recv_hdr(&rtype, &rpos, 20);
    zm_write_raw("OO", 2);

done:
    if (have_tio) tcsetattr(STDIN_FILENO, TCSANOW, &orig_tio);
    return rc;
}

static int zmodem_send_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return 1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)(sz > 0 ? sz : 1));
    if (!buf) { fclose(f); return 1; }
    size_t nr = (sz > 0) ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    int rc = zmodem_send_buf(path, buf, nr);
    free(buf);
    return rc;
}

static int zmodem_recv_dir(const char *dest_dir)
{
    struct termios orig_tio, raw_tio;
    bool have_tio = (tcgetattr(STDIN_FILENO, &orig_tio) == 0);
    if (have_tio) {
        raw_tio = orig_tio;
        cfmakeraw(&raw_tio);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_tio);
    }

    uint8_t pkt[2048];
    FILE *cur_f = NULL;
    uint32_t cur_pos = 0;
    int rc = 1;

    zm_send_hex_hdr(ZRINIT, ZCANFDX | ZCANOVIO);
    while (1) {
        uint8_t rtype = 0;
        uint32_t rpos = 0;
        if (zm_recv_hdr(&rtype, &rpos, 100) != 0) {
            zm_send_hex_hdr(ZRINIT, ZCANFDX | ZCANOVIO);
            continue;
        }
        if (rtype == ZRQINIT) {
            zm_send_hex_hdr(ZRINIT, ZCANFDX | ZCANOVIO);
        } else if (rtype == ZFILE) {
            size_t plen = 0;
            uint8_t fend = 0;
            if (zm_recv_subpkt(pkt, sizeof(pkt) - 1, &plen, &fend, 50) != 0) {
                zm_send_hex_hdr(ZNAK, 0);
                continue;
            }
            pkt[plen] = '\0';
            const char *name = (const char *)pkt;
            const char *base = strrchr(name, '/');
            base = base ? (base + 1) : name;
            char full[256];
            snprintf(full, sizeof(full), "%s/%s", (dest_dir && dest_dir[0]) ? dest_dir : ".", base);
            if (cur_f) fclose(cur_f);
            cur_f = fopen(full, "wb");
            cur_pos = 0;
            zm_send_hex_hdr(cur_f ? ZRPOS : ZSKIP, 0);
        } else if (rtype == ZDATA) {
            if (cur_f && rpos != cur_pos) {
                fseek(cur_f, (long)rpos, SEEK_SET);
                cur_pos = rpos;
            }
            while (1) {
                size_t plen = 0;
                uint8_t fend = 0;
                int s = zm_recv_subpkt(pkt, sizeof(pkt), &plen, &fend, 50);
                if (s != 0) {
                    zm_send_hex_hdr(ZRPOS, cur_pos);
                    break;
                }
                if (cur_f && plen > 0) {
                    fwrite(pkt, 1, plen, cur_f);
                    cur_pos += (uint32_t)plen;
                }
                if (fend == ZCRCQ) zm_send_hex_hdr(ZACK, cur_pos);
                else if (fend == ZCRCW) { zm_send_hex_hdr(ZACK, cur_pos); break; }
                else if (fend == ZCRCE) break;
            }
        } else if (rtype == ZEOF) {
            if (cur_f) { fclose(cur_f); cur_f = NULL; }
            zm_send_hex_hdr(ZRINIT, ZCANFDX | ZCANOVIO);
        } else if (rtype == ZFIN) {
            if (cur_f) { fclose(cur_f); cur_f = NULL; }
            zm_send_hex_hdr(ZFIN, 0);
            zm_read_byte(10);
            zm_read_byte(10);
            rc = 0;
            break;
        } else if (rtype == ZCAN || rtype == ZABORT) {
            break;
        }
    }

    if (cur_f) fclose(cur_f);
    if (have_tio) tcsetattr(STDIN_FILENO, TCSANOW, &orig_tio);
    return rc;
}

#endif
