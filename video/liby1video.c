/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * External SD Card Video Library for Innioasis Y1 (liby1video.so)
 * Placed at /mnt/sdcard/.rockbox/lib/liby1video.so
 *
 * Features:
 * - Supports MP4, MKV, MOV containers
 * - 64-bit file I/O (off_t / int64_t) for long videos (>2 hours, >4GB, co64)
 * - Built-in FFmpeg H.264 decoder (Baseline, Main, High Profile CABAC/8x8 IDCT)
 *   statically linked into liby1video.so (no external .so dependencies needed)
 * - Built-in FAAD2 AAC-LC / HE-AAC + PCM audio decoder statically linked into
 *   liby1video.so with lock-free stereo 16-bit PCM ring buffer
 * - Direct YUV420P -> RGB565 360p downscaler for MT6572 (480x360 & 320x240)
 * - Optional dynamic external FFmpeg override if full libavformat.so is present
 ****************************************************************************/
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "../../apps/plugins/y1_video_lib.h"

/* =========================================================================
 * Built-in FFmpeg H.264 Decoder & FAAD2 AAC Decoder Declarations
 * (Linked from hosted_linux_port/video/h264_objs/ & faad_objs/)
 * ========================================================================= */
extern uint8_t ff_h264_decoder[];
extern uint8_t ff_h264_parser[];

extern void  avcodec_register(void *codec);
extern void  av_register_codec_parser(void *parser);
extern void *avcodec_find_decoder(int id);
extern void *avcodec_alloc_context3(const void *codec);
extern int   avcodec_open2(void *avctx, const void *codec, void **options);
extern int   avcodec_decode_video2(void *avctx, void *picture,
                                   int *got_picture_ptr, const void *avpkt);
extern void  avcodec_flush_buffers(void *avctx);
extern int   avcodec_close(void *avctx);
extern void *av_frame_alloc(void);
extern void  av_frame_free(void **frame);
extern void  av_frame_unref(void *frame);
extern void  av_init_packet(void *pkt);
extern void *av_mallocz(size_t size);
extern void  av_free(void *ptr);
extern int   av_opt_set_int(void *obj, const char *name, int64_t val, int search_flags);

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

typedef void (*qpel_mc_func)(uint8_t *dst, const uint8_t *src, ptrdiff_t stride);

static void neon_put_pixels16(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 16; i++) {
        vst1q_u8(dst, vld1q_u8(src));
        src += stride;
        dst += stride;
    }
}

static void neon_avg_pixels16(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 16; i++) {
        vst1q_u8(dst, vrhaddq_u8(vld1q_u8(dst), vld1q_u8(src)));
        src += stride;
        dst += stride;
    }
}

static void neon_put_qpel16_h(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 16; i++) {
        vst1q_u8(dst, vrhaddq_u8(vld1q_u8(src), vld1q_u8(src + 1)));
        src += stride;
        dst += stride;
    }
}

static void neon_avg_qpel16_h(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 16; i++) {
        vst1q_u8(dst, vrhaddq_u8(vld1q_u8(dst), vrhaddq_u8(vld1q_u8(src), vld1q_u8(src + 1))));
        src += stride;
        dst += stride;
    }
}

static void neon_put_qpel16_v(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 16; i++) {
        vst1q_u8(dst, vrhaddq_u8(vld1q_u8(src), vld1q_u8(src + stride)));
        src += stride;
        dst += stride;
    }
}

static void neon_avg_qpel16_v(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 16; i++) {
        vst1q_u8(dst, vrhaddq_u8(vld1q_u8(dst), vrhaddq_u8(vld1q_u8(src), vld1q_u8(src + stride))));
        src += stride;
        dst += stride;
    }
}

static void neon_put_qpel16_hv(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 16; i++) {
        uint8x16_t s0 = vrhaddq_u8(vld1q_u8(src), vld1q_u8(src + 1));
        uint8x16_t s1 = vrhaddq_u8(vld1q_u8(src + stride), vld1q_u8(src + stride + 1));
        vst1q_u8(dst, vrhaddq_u8(s0, s1));
        src += stride;
        dst += stride;
    }
}

static void neon_avg_qpel16_hv(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 16; i++) {
        uint8x16_t s0 = vrhaddq_u8(vld1q_u8(src), vld1q_u8(src + 1));
        uint8x16_t s1 = vrhaddq_u8(vld1q_u8(src + stride), vld1q_u8(src + stride + 1));
        vst1q_u8(dst, vrhaddq_u8(vld1q_u8(dst), vrhaddq_u8(s0, s1)));
        src += stride;
        dst += stride;
    }
}

static void neon_put_pixels8(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 8; i++) {
        vst1_u8(dst, vld1_u8(src));
        src += stride;
        dst += stride;
    }
}

static void neon_avg_pixels8(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 8; i++) {
        vst1_u8(dst, vrhadd_u8(vld1_u8(dst), vld1_u8(src)));
        src += stride;
        dst += stride;
    }
}

static void neon_put_qpel8_h(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 8; i++) {
        vst1_u8(dst, vrhadd_u8(vld1_u8(src), vld1_u8(src + 1)));
        src += stride;
        dst += stride;
    }
}

static void neon_avg_qpel8_h(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 8; i++) {
        vst1_u8(dst, vrhadd_u8(vld1_u8(dst), vrhadd_u8(vld1_u8(src), vld1_u8(src + 1))));
        src += stride;
        dst += stride;
    }
}

static void neon_put_qpel8_v(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 8; i++) {
        vst1_u8(dst, vrhadd_u8(vld1_u8(src), vld1_u8(src + stride)));
        src += stride;
        dst += stride;
    }
}

static void neon_avg_qpel8_v(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 8; i++) {
        vst1_u8(dst, vrhadd_u8(vld1_u8(dst), vrhadd_u8(vld1_u8(src), vld1_u8(src + stride))));
        src += stride;
        dst += stride;
    }
}

static void neon_put_qpel8_hv(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 8; i++) {
        uint8x8_t s0 = vrhadd_u8(vld1_u8(src), vld1_u8(src + 1));
        uint8x8_t s1 = vrhadd_u8(vld1_u8(src + stride), vld1_u8(src + stride + 1));
        vst1_u8(dst, vrhadd_u8(s0, s1));
        src += stride;
        dst += stride;
    }
}

static void neon_avg_qpel8_hv(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 8; i++) {
        uint8x8_t s0 = vrhadd_u8(vld1_u8(src), vld1_u8(src + 1));
        uint8x8_t s1 = vrhadd_u8(vld1_u8(src + stride), vld1_u8(src + stride + 1));
        vst1_u8(dst, vrhadd_u8(vld1_u8(dst), vrhadd_u8(s0, s1)));
        src += stride;
        dst += stride;
    }
}

static void neon_put_pixels4(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 4; i++) {
        *(uint32_t *)dst = *(const uint32_t *)src;
        src += stride;
        dst += stride;
    }
}

static void neon_avg_pixels4(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 4; i++) {
        uint32_t a = *(uint32_t *)dst;
        uint32_t b = *(const uint32_t *)src;
        *(uint32_t *)dst = (a | b) - (((a ^ b) >> 1) & 0x7f7f7f7f);
        src += stride;
        dst += stride;
    }
}

static void neon_put_qpel4_h(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 4; i++) {
        uint32_t a = *(const uint32_t *)src;
        uint32_t b = *(const uint32_t *)(src + 1);
        *(uint32_t *)dst = (a | b) - (((a ^ b) >> 1) & 0x7f7f7f7f);
        src += stride;
        dst += stride;
    }
}

static void neon_avg_qpel4_h(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 4; i++) {
        uint32_t d = *(uint32_t *)dst;
        uint32_t a = *(const uint32_t *)src;
        uint32_t b = *(const uint32_t *)(src + 1);
        uint32_t s = (a | b) - (((a ^ b) >> 1) & 0x7f7f7f7f);
        *(uint32_t *)dst = (d | s) - (((d ^ s) >> 1) & 0x7f7f7f7f);
        src += stride;
        dst += stride;
    }
}

static void neon_put_qpel4_v(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 4; i++) {
        uint32_t a = *(const uint32_t *)src;
        uint32_t b = *(const uint32_t *)(src + stride);
        *(uint32_t *)dst = (a | b) - (((a ^ b) >> 1) & 0x7f7f7f7f);
        src += stride;
        dst += stride;
    }
}

static void neon_avg_qpel4_v(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 4; i++) {
        uint32_t d = *(uint32_t *)dst;
        uint32_t a = *(const uint32_t *)src;
        uint32_t b = *(const uint32_t *)(src + stride);
        uint32_t s = (a | b) - (((a ^ b) >> 1) & 0x7f7f7f7f);
        *(uint32_t *)dst = (d | s) - (((d ^ s) >> 1) & 0x7f7f7f7f);
        src += stride;
        dst += stride;
    }
}

static void neon_put_qpel4_hv(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 4; i++) {
        uint32_t a0 = *(const uint32_t *)src;
        uint32_t a1 = *(const uint32_t *)(src + 1);
        uint32_t b0 = *(const uint32_t *)(src + stride);
        uint32_t b1 = *(const uint32_t *)(src + stride + 1);
        uint32_t r0 = (a0 | a1) - (((a0 ^ a1) >> 1) & 0x7f7f7f7f);
        uint32_t r1 = (b0 | b1) - (((b0 ^ b1) >> 1) & 0x7f7f7f7f);
        *(uint32_t *)dst = (r0 | r1) - (((r0 ^ r1) >> 1) & 0x7f7f7f7f);
        src += stride;
        dst += stride;
    }
}

static void neon_avg_qpel4_hv(uint8_t *dst, const uint8_t *src, ptrdiff_t stride)
{
    for (int i = 0; i < 4; i++) {
        uint32_t d  = *(uint32_t *)dst;
        uint32_t a0 = *(const uint32_t *)src;
        uint32_t a1 = *(const uint32_t *)(src + 1);
        uint32_t b0 = *(const uint32_t *)(src + stride);
        uint32_t b1 = *(const uint32_t *)(src + stride + 1);
        uint32_t r0 = (a0 | a1) - (((a0 ^ a1) >> 1) & 0x7f7f7f7f);
        uint32_t r1 = (b0 | b1) - (((b0 ^ b1) >> 1) & 0x7f7f7f7f);
        uint32_t s  = (r0 | r1) - (((r0 ^ r1) >> 1) & 0x7f7f7f7f);
        *(uint32_t *)dst = (d | s) - (((d ^ s) >> 1) & 0x7f7f7f7f);
        src += stride;
        dst += stride;
    }
}

typedef void (*h264_chroma_mc_func)(uint8_t *dst, const uint8_t *src,
                                    ptrdiff_t stride, int h, int x, int y);

static void neon_put_h264_chroma8(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                                  int h, int x, int y)
{
    if (x == 0 && y == 0) {
        for (int i = 0; i < h; i++) {
            vst1_u8(dst, vld1_u8(src));
            src += stride;
            dst += stride;
        }
        return;
    }
    uint8_t cD = (uint8_t)(x * y);
    uint8_t cC = (uint8_t)((8 - x) * y);
    uint8_t cB = (uint8_t)(x * (8 - y));
    uint8_t cA = (uint8_t)((8 - x) * (8 - y));

    uint8x8_t vA = vdup_n_u8(cA);
    uint8x8_t vB = vdup_n_u8(cB);
    uint8x8_t vC = vdup_n_u8(cC);
    uint8x8_t vD = vdup_n_u8(cD);

    for (int i = 0; i < h; i++) {
        uint8x8_t s00 = vld1_u8(src);
        uint8x8_t s01 = vld1_u8(src + 1);
        uint8x8_t s10 = vld1_u8(src + stride);
        uint8x8_t s11 = vld1_u8(src + stride + 1);

        uint16x8_t res = vmull_u8(s00, vA);
        res = vmlal_u8(res, s01, vB);
        res = vmlal_u8(res, s10, vC);
        res = vmlal_u8(res, s11, vD);

        vst1_u8(dst, vrshrn_n_u16(res, 6));
        src += stride;
        dst += stride;
    }
}

static void neon_avg_h264_chroma8(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                                  int h, int x, int y)
{
    uint8_t tmp[8 * 16];
    neon_put_h264_chroma8(tmp, src, 8, h, x, y);
    for (int i = 0; i < h; i++) {
        uint8x8_t d = vld1_u8(dst);
        uint8x8_t s = vld1_u8(&tmp[i * 8]);
        vst1_u8(dst, vrhadd_u8(d, s));
        dst += stride;
    }
}

static void neon_put_h264_chroma4(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                                  int h, int x, int y)
{
    if (x == 0 && y == 0) {
        for (int i = 0; i < h; i++) {
            *(uint32_t *)dst = *(const uint32_t *)src;
            src += stride;
            dst += stride;
        }
        return;
    }
    int cD = (x * y);
    int cC = ((8 - x) * y);
    int cB = (x * (8 - y));
    int cA = ((8 - x) * (8 - y));

    for (int i = 0; i < h; i++) {
        for (int j = 0; j < 4; j++) {
            dst[j] = (uint8_t)((cA * src[j] + cB * src[j + 1] +
                                cC * src[j + stride] + cD * src[j + stride + 1] + 32) >> 6);
        }
        src += stride;
        dst += stride;
    }
}

static void neon_avg_h264_chroma4(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                                  int h, int x, int y)
{
    uint8_t tmp[4 * 8];
    neon_put_h264_chroma4(tmp, src, 4, h, x, y);
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < 4; j++) {
            dst[j] = (uint8_t)((dst[j] + tmp[i * 4 + j] + 1) >> 1);
        }
        dst += stride;
    }
}
#endif

/* Architecture init & parse stubs required by h264_objs */
void ff_h264_pred_init_arm(void *h, int codec_id, int bit_depth, int chroma_format_idc)
{ (void)h; (void)codec_id; (void)bit_depth; (void)chroma_format_idc; }
void ff_h264_pred_init_mips(void *h, int codec_id, int bit_depth, int chroma_format_idc)
{ (void)h; (void)codec_id; (void)bit_depth; (void)chroma_format_idc; }
void ff_h264_pred_init_x86(void *h, int codec_id, int bit_depth, int chroma_format_idc)
{ (void)h; (void)codec_id; (void)bit_depth; (void)chroma_format_idc; }

void ff_h264chroma_init_arm(void *c, int bit_depth)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (bit_depth != 8 || !c) return;
    h264_chroma_mc_func *put = (h264_chroma_mc_func *)c;
    h264_chroma_mc_func *avg = (h264_chroma_mc_func *)((uint8_t *)c + 16);
    put[0] = neon_put_h264_chroma8;
    put[1] = neon_put_h264_chroma4;
    avg[0] = neon_avg_h264_chroma8;
    avg[1] = neon_avg_h264_chroma4;
#else
    (void)c; (void)bit_depth;
#endif
}

void ff_h264chroma_init_mips(void *c, int bit_depth) { (void)c; (void)bit_depth; }
void ff_h264chroma_init_x86(void *c, int bit_depth) { (void)c; (void)bit_depth; }
void ff_h264dsp_init_arm(void *c, int bit_depth, int chroma_format_idc)
{ (void)c; (void)bit_depth; (void)chroma_format_idc; }
void ff_h264dsp_init_mips(void *c, int bit_depth, int chroma_format_idc)
{ (void)c; (void)bit_depth; (void)chroma_format_idc; }
void ff_h264dsp_init_x86(void *c, int bit_depth, int chroma_format_idc)
{ (void)c; (void)bit_depth; (void)chroma_format_idc; }

void ff_h264qpel_init_arm(void *c, int bit_depth)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (bit_depth != 8 || !c) return;
    qpel_mc_func (*put)[16] = (qpel_mc_func (*)[16])c;
    qpel_mc_func (*avg)[16] = (qpel_mc_func (*)[16])((uint8_t *)c + 256);

    for (int sub = 0; sub < 16; sub++) {
        int x = sub & 3;
        int y = (sub >> 2) & 3;
        if (x == 0 && y == 0) {
            put[0][sub] = neon_put_pixels16;
            put[1][sub] = neon_put_pixels8;
            put[2][sub] = neon_put_pixels4;
            avg[0][sub] = neon_avg_pixels16;
            avg[1][sub] = neon_avg_pixels8;
            avg[2][sub] = neon_avg_pixels4;
        } else if (y == 0) {
            put[0][sub] = neon_put_qpel16_h;
            put[1][sub] = neon_put_qpel8_h;
            put[2][sub] = neon_put_qpel4_h;
            avg[0][sub] = neon_avg_qpel16_h;
            avg[1][sub] = neon_avg_qpel8_h;
            avg[2][sub] = neon_avg_qpel4_h;
        } else if (x == 0) {
            put[0][sub] = neon_put_qpel16_v;
            put[1][sub] = neon_put_qpel8_v;
            put[2][sub] = neon_put_qpel4_v;
            avg[0][sub] = neon_avg_qpel16_v;
            avg[1][sub] = neon_avg_qpel8_v;
            avg[2][sub] = neon_avg_qpel4_v;
        } else {
            put[0][sub] = neon_put_qpel16_hv;
            put[1][sub] = neon_put_qpel8_hv;
            put[2][sub] = neon_put_qpel4_hv;
            avg[0][sub] = neon_avg_qpel16_hv;
            avg[1][sub] = neon_avg_qpel8_hv;
            avg[2][sub] = neon_avg_qpel4_hv;
        }
    }
#else
    (void)c; (void)bit_depth;
#endif
}

void ff_h264qpel_init_mips(void *c, int bit_depth) { (void)c; (void)bit_depth; }
void ff_h264qpel_init_x86(void *c, int bit_depth) { (void)c; (void)bit_depth; }
void ff_me_cmp_init_arm(void *c, void *avctx) { (void)c; (void)avctx; }
void ff_me_cmp_init_mips(void *c, void *avctx) { (void)c; (void)avctx; }
void ff_me_cmp_init_x86(void *c, void *avctx) { (void)c; (void)avctx; }
void ff_videodsp_init_arm(void *ctx, int bpc) { (void)ctx; (void)bpc; }
void ff_videodsp_init_x86(void *ctx, int bpc) { (void)ctx; (void)bpc; }
int av_parse_color(uint8_t *rgba_color, const char *color_string, int slen, void *log_ctx)
{ (void)rgba_color; (void)color_string; (void)slen; (void)log_ctx; return -1; }
int av_parse_time(int64_t *timeval, const char *timestr, int duration)
{ (void)timeval; (void)timestr; (void)duration; return -1; }
int av_parse_video_rate(void *rate, const char *arg)
{ (void)rate; (void)arg; return -1; }
int av_parse_video_size(int *width_ptr, int *height_ptr, const char *str)
{ (void)width_ptr; (void)height_ptr; (void)str; return -1; }

/* Built-in FAAD2 AAC decoder declarations (linked from faad_objs/) */
typedef void *NeAACDecHandle;

typedef struct NeAACDecConfiguration {
    unsigned char defObjectType;
    unsigned long defSampleRate;
    unsigned char outputFormat;
    unsigned char downMatrix;
    unsigned char useOldADTSFormat;
    unsigned char dontUpSampleImplicitSBR;
} NeAACDecConfiguration, *NeAACDecConfigurationPtr;

typedef struct NeAACDecFrameInfo {
    unsigned long bytesconsumed;
    unsigned long samples;
    unsigned char channels;
    unsigned char error;
    unsigned long samplerate;
    unsigned char sbr;
    unsigned char object_type;
    unsigned char header_type;
    unsigned char num_front_channels;
    unsigned char num_side_channels;
    unsigned char num_back_channels;
    unsigned char num_lfe_channels;
    unsigned char channel_position[8];
    unsigned char ps;
} NeAACDecFrameInfo;

extern NeAACDecHandle NeAACDecOpen(void);
extern NeAACDecConfigurationPtr NeAACDecGetCurrentConfiguration(NeAACDecHandle hDecoder);
extern uint8_t NeAACDecSetConfiguration(NeAACDecHandle hDecoder, NeAACDecConfigurationPtr config);
extern int32_t NeAACDecInit(NeAACDecHandle hDecoder, uint8_t *buffer, uint32_t buffer_size,
                            uint32_t *samplerate, uint8_t *channels);
extern int8_t  NeAACDecInit2(NeAACDecHandle hDecoder, uint8_t *pBuffer,
                             uint32_t SizeOfDecoderSpecificInfo,
                             uint32_t *samplerate, uint8_t *channels);
extern void    NeAACDecPostSeekReset(NeAACDecHandle hDecoder, int32_t frame);
extern void   *NeAACDecDecode(NeAACDecHandle hDecoder, NeAACDecFrameInfo *hInfo,
                              uint8_t *buffer, uint32_t buffer_size);

static int g_builtin_h264_initialized = 0;

static void init_builtin_h264(void)
{
    if (!g_builtin_h264_initialized) {
        avcodec_register(ff_h264_decoder);
        av_register_codec_parser(ff_h264_parser);
        g_builtin_h264_initialized = 1;
    }
}

#define AV_CODEC_ID_H264    28
#define AVDISCARD_DEFAULT    0
#define AVDISCARD_NONREF     8
#define AVDISCARD_BIDIR     16
#define AVDISCARD_NONKEY    32
#define AVDISCARD_ALL       48

/* =========================================================================
 * Big-Endian & EBML Helpers
 * ========================================================================= */
static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint64_t read_be64(const uint8_t *p)
{
    return ((uint64_t)read_be32(p) << 32) | (uint64_t)read_be32(p + 4);
}

#define FOURCC(a,b,c,d) (((uint32_t)(uint8_t)(a) << 24) | \
                         ((uint32_t)(uint8_t)(b) << 16) | \
                         ((uint32_t)(uint8_t)(c) << 8)  | \
                         ((uint32_t)(uint8_t)(d)))

static void extract_filename_stem(const char *path, char *out, size_t out_sz)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    snprintf(out, out_sz, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && dot > out) *dot = '\0';
}

/* =========================================================================
 * Native 64-bit MP4 / MOV / MKV Container Demuxer Structures
 * ========================================================================= */
struct mp4_stts_entry {
    uint32_t count;
    uint32_t delta;
};

struct mp4_stsc_entry {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t sd_idx;
};

struct mp4_track_table {
    int      valid;
    int      is_video;
    int      is_audio;
    uint32_t fourcc;
    uint32_t timescale;
    uint64_t duration;
    int      width;
    int      height;
    int      channels;
    int      samplerate;
    int      bits_per_sample;
    int      nalu_len_size;

    uint8_t *extradata;
    int      extradata_len;

    uint32_t sample_count;
    uint32_t default_sample_size;
    uint32_t *sample_sizes;

    uint32_t num_stts;
    struct mp4_stts_entry *stts;

    uint32_t num_stsc;
    struct mp4_stsc_entry *stsc;

    uint32_t num_chunks;
    uint64_t *chunk_offsets;

    uint32_t num_stss;
    uint32_t *stss;

    /* Sequential cursor state */
    uint32_t cur_sample;
    uint32_t cur_chunk;
    uint32_t cur_stsc_idx;
    uint32_t sample_in_chunk;
    uint64_t cur_offset;
    uint64_t cur_dts_ticks;
    uint32_t cur_stts_idx;
    uint32_t sample_in_stts;
};

static void free_mp4_track_table(struct mp4_track_table *t)
{
    if (!t) return;
    free(t->extradata);
    free(t->sample_sizes);
    free(t->stts);
    free(t->stsc);
    free(t->chunk_offsets);
    free(t->stss);
    memset(t, 0, sizeof(*t));
}

/* Convert avcC box payload into Annex-B (\x00\x00\x00\x01 + SPS + \x00\x00\x00\x01 + PPS) */
static uint8_t *convert_avcc_to_annexb(const uint8_t *avcc, int avcc_len,
                                       int *nalu_len_size_out, int *out_len)
{
    *out_len = 0;
    if (!avcc || avcc_len < 7) return NULL;

    if (nalu_len_size_out)
        *nalu_len_size_out = (avcc[4] & 0x03) + 1;

    uint8_t *out = calloc((size_t)avcc_len + 128, 1);
    if (!out) return NULL;

    int pos = 5;
    int wpos = 0;
    int num_sps = avcc[pos++] & 0x1F;
    for (int i = 0; i < num_sps && pos + 2 <= avcc_len; i++) {
        int slen = read_be16(avcc + pos);
        pos += 2;
        if (slen <= 0 || pos + slen > avcc_len) break;
        out[wpos++] = 0x00;
        out[wpos++] = 0x00;
        out[wpos++] = 0x00;
        out[wpos++] = 0x01;
        memcpy(out + wpos, avcc + pos, (size_t)slen);
        wpos += slen;
        pos += slen;
    }

    if (pos < avcc_len) {
        int num_pps = avcc[pos++];
        for (int i = 0; i < num_pps && pos + 2 <= avcc_len; i++) {
            int plen = read_be16(avcc + pos);
            pos += 2;
            if (plen <= 0 || pos + plen > avcc_len) break;
            out[wpos++] = 0x00;
            out[wpos++] = 0x00;
            out[wpos++] = 0x00;
            out[wpos++] = 0x01;
            memcpy(out + wpos, avcc + pos, (size_t)plen);
            wpos += plen;
            pos += plen;
        }
    }

    if (wpos == 0) {
        free(out);
        return NULL;
    }
    *out_len = wpos;
    return out;
}

static int read_esds_descr_len(const uint8_t *buf, int len, int *pos_io)
{
    int val = 0;
    int count = 0;
    while (*pos_io < len && count < 4) {
        uint8_t b = buf[(*pos_io)++];
        val = (val << 7) | (b & 0x7F);
        count++;
        if (!(b & 0x80)) break;
    }
    return val;
}

/* Extract AudioSpecificConfig (tag 0x05) from an MP4/MOV esds box payload */
static uint8_t *extract_esds_asc(const uint8_t *esds, int esds_len, int *out_len)
{
    *out_len = 0;
    if (!esds || esds_len < 8) return NULL;

    int pos = 4; /* Skip 4-byte version/flags */
    while (pos < esds_len) {
        uint8_t tag = esds[pos++];
        int dlen = read_esds_descr_len(esds, esds_len, &pos);
        if (dlen < 0 || pos + dlen > esds_len) break;

        if (tag == 0x03) { /* ES_Descr */
            if (dlen < 3) break;
            pos += 2; /* ES_ID */
            uint8_t flags = esds[pos++];
            if (flags & 0x80) pos += 2;
            if ((flags & 0x40) && pos < esds_len) {
                int ulen = esds[pos++];
                pos += ulen;
            }
            if (flags & 0x20) pos += 2;
        } else if (tag == 0x04) { /* DecoderConfigDescr */
            if (dlen < 13) break;
            pos += 13;
        } else if (tag == 0x05) { /* DecSpecificInfo */
            if (dlen > 0 && dlen <= 64) {
                uint8_t *asc = calloc((size_t)dlen + 16, 1);
                if (asc) {
                    memcpy(asc, esds + pos, (size_t)dlen);
                    /* Trim trailing zero padding added by some MP4 muxers (keep at least 2 bytes) */
                    while (dlen > 2 && asc[dlen - 1] == 0)
                        dlen--;
                    *out_len = dlen;
                    return asc;
                }
            }
            break;
        } else {
            pos += dlen;
        }
    }
    return NULL;
}

/* Parse iTunes/MP4 metadata ilst atom */
static void parse_mp4_ilst(FILE *fp, int64_t ilst_start, int64_t ilst_end, struct y1vid_meta *meta)
{
    int64_t pos = ilst_start;
    uint8_t hdr[16];

    while (pos + 8 <= ilst_end) {
        if (fseeko(fp, pos, SEEK_SET) != 0) break;
        if (fread(hdr, 1, 8, fp) != 8) break;
        uint64_t sz = read_be32(hdr);
        uint32_t type = read_be32(hdr + 4);
        int64_t hdr_len = 8;
        if (sz == 1) {
            if (fread(hdr + 8, 1, 8, fp) != 8) break;
            sz = read_be64(hdr + 8);
            hdr_len = 16;
        } else if (sz == 0) {
            sz = ilst_end - pos;
        }
        if (sz < (uint64_t)hdr_len || pos + (int64_t)sz > ilst_end) break;

        int64_t dpos = pos + hdr_len;
        int64_t item_end = pos + (int64_t)sz;
        while (dpos + 16 <= item_end) {
            if (fseeko(fp, dpos, SEEK_SET) != 0) break;
            uint8_t dhdr[16];
            if (fread(dhdr, 1, 16, fp) != 16) break;
            uint32_t dsz = read_be32(dhdr);
            uint32_t dtype = read_be32(dhdr + 4);
            if (dsz < 16 || dpos + dsz > item_end) break;

            if (dtype == FOURCC('d','a','t','a')) {
                uint32_t payload_len = dsz - 16;
                if (payload_len > 0 && payload_len < 512) {
                    char buf[512];
                    size_t r = fread(buf, 1, payload_len, fp);
                    buf[r] = '\0';
                    if (type == FOURCC(0xA9,'n','a','m')) {
                        snprintf(meta->title, sizeof(meta->title), "%s", buf);
                    } else if (type == FOURCC(0xA9,'A','R','T') || type == FOURCC(0xA9,'a','r','t') ||
                               type == FOURCC('a','A','R','T')) {
                        if (!meta->creator[0])
                            snprintf(meta->creator, sizeof(meta->creator), "%s", buf);
                    } else if (type == FOURCC('t','v','s','h')) {
                        snprintf(meta->tv_show, sizeof(meta->tv_show), "%s", buf);
                    } else if (type == FOURCC(0xA9,'d','a','y')) {
                        int y = atoi(buf);
                        if (y >= 1900 && y <= 2100) meta->year = y;
                    } else if (type == FOURCC('t','v','s','n') && payload_len >= 4) {
                        meta->season = (int)read_be32((const uint8_t *)buf);
                    } else if (type == FOURCC('t','v','e','s') && payload_len >= 4) {
                        meta->episode = (int)read_be32((const uint8_t *)buf);
                    }
                }
                break;
            }
            dpos += dsz;
        }
        pos += (int64_t)sz;
    }
}

static void parse_mp4_boxes(FILE *fp, int64_t start, int64_t end,
                            struct y1vid_meta *meta,
                            struct mp4_track_table *cur_trk,
                            struct mp4_track_table *v_trk_out,
                            struct mp4_track_table *a_trk_out,
                            int load_tables, int depth)
{
    if (depth > 12) return;
    int64_t pos = start;
    uint8_t hdr[16];

    while (pos + 8 <= end) {
        if (fseeko(fp, pos, SEEK_SET) != 0) break;
        if (fread(hdr, 1, 8, fp) != 8) break;

        uint64_t sz = read_be32(hdr);
        uint32_t type = read_be32(hdr + 4);
        int64_t hdr_len = 8;

        if (sz == 1) {
            if (fread(hdr + 8, 1, 8, fp) != 8) break;
            sz = read_be64(hdr + 8);
            hdr_len = 16;
        } else if (sz == 0) {
            sz = end - pos;
        }

        if (sz < (uint64_t)hdr_len || pos + (int64_t)sz > end)
            break;

        int64_t body = pos + hdr_len;
        int64_t box_end = pos + (int64_t)sz;

        if (type == FOURCC('m','o','o','v') ||
            type == FOURCC('m','d','i','a') ||
            type == FOURCC('m','i','n','f') ||
            type == FOURCC('s','t','b','l') ||
            type == FOURCC('u','d','t','a')) {
            parse_mp4_boxes(fp, body, box_end, meta, cur_trk, v_trk_out, a_trk_out, load_tables, depth + 1);
        } else if (type == FOURCC('t','r','a','k')) {
            struct mp4_track_table trk;
            memset(&trk, 0, sizeof(trk));
            trk.nalu_len_size = 4;
            parse_mp4_boxes(fp, body, box_end, meta, &trk, v_trk_out, a_trk_out, load_tables, depth + 1);

            if (trk.is_video && !meta->has_video) {
                meta->has_video = 1;
                if (trk.width > 0)  meta->width = trk.width;
                if (trk.height > 0) meta->height = trk.height;
                if (trk.fourcc) {
                    meta->codec_name[0] = (char)((trk.fourcc >> 24) & 0xFF);
                    meta->codec_name[1] = (char)((trk.fourcc >> 16) & 0xFF);
                    meta->codec_name[2] = (char)((trk.fourcc >> 8) & 0xFF);
                    meta->codec_name[3] = (char)(trk.fourcc & 0xFF);
                    meta->codec_name[4] = '\0';
                }
                if (trk.timescale > 0 && trk.duration > 0) {
                    int64_t d_ms = (int64_t)((trk.duration * 1000ULL) / trk.timescale);
                    if (d_ms > meta->duration_ms) meta->duration_ms = d_ms;
                    if (trk.sample_count > 0 && d_ms > 0) {
                        meta->fps_x100 = (int)(((uint64_t)trk.sample_count * 100000ULL) / (uint64_t)d_ms);
                    }
                }
                if (load_tables && v_trk_out && !v_trk_out->valid) {
                    *v_trk_out = trk;
                    v_trk_out->valid = 1;
                    memset(&trk, 0, sizeof(trk));
                }
            } else if (trk.is_audio && !meta->has_audio) {
                meta->has_audio = 1;
                meta->audio_channels = trk.channels > 0 ? trk.channels : 2;
                meta->audio_samplerate = trk.samplerate > 0 ? trk.samplerate : 44100;
                if (load_tables && a_trk_out && !a_trk_out->valid) {
                    *a_trk_out = trk;
                    a_trk_out->valid = 1;
                    memset(&trk, 0, sizeof(trk));
                }
            }
            free_mp4_track_table(&trk);
        } else if (type == FOURCC('m','e','t','a')) {
            if (body + 4 < box_end)
                parse_mp4_boxes(fp, body + 4, box_end, meta, cur_trk, v_trk_out, a_trk_out, load_tables, depth + 1);
        } else if (type == FOURCC('i','l','s','t')) {
            parse_mp4_ilst(fp, body, box_end, meta);
        } else if (type == FOURCC('m','v','h','d')) {
            uint8_t mvhd[32];
            if (fseeko(fp, body, SEEK_SET) == 0 && fread(mvhd, 1, 32, fp) >= 24) {
                uint8_t ver = mvhd[0];
                uint32_t ts = 0;
                uint64_t dur = 0;
                if (ver == 1 && box_end - body >= 32) {
                    ts  = read_be32(mvhd + 20);
                    dur = read_be64(mvhd + 24);
                } else {
                    ts  = read_be32(mvhd + 12);
                    dur = read_be32(mvhd + 16);
                }
                if (ts > 0 && dur > 0 && dur != UINT32_MAX && dur != UINT64_MAX) {
                    meta->duration_ms = (int64_t)((dur * 1000ULL) / ts);
                }
            }
        } else if (type == FOURCC('t','k','h','d') && cur_trk) {
            uint8_t tkhd[96];
            size_t want = (box_end - body) < 96 ? (size_t)(box_end - body) : 96;
            if (fseeko(fp, body, SEEK_SET) == 0 && fread(tkhd, 1, want, fp) == want) {
                uint8_t ver = tkhd[0];
                int w_off = (ver == 1) ? 88 : 76;
                if ((int)want >= w_off + 8) {
                    int w = (int)(read_be32(tkhd + w_off) >> 16);
                    int h = (int)(read_be32(tkhd + w_off + 4) >> 16);
                    if (w > 0 && h > 0 && cur_trk->width == 0) {
                        cur_trk->width = w;
                        cur_trk->height = h;
                    }
                }
            }
        } else if (type == FOURCC('m','d','h','d') && cur_trk) {
            uint8_t mdhd[32];
            if (fseeko(fp, body, SEEK_SET) == 0 && fread(mdhd, 1, 32, fp) >= 24) {
                uint8_t ver = mdhd[0];
                if (ver == 1) {
                    cur_trk->timescale = read_be32(mdhd + 20);
                    cur_trk->duration  = read_be64(mdhd + 24);
                } else {
                    cur_trk->timescale = read_be32(mdhd + 12);
                    cur_trk->duration  = read_be32(mdhd + 16);
                }
            }
        } else if (type == FOURCC('h','d','l','r') && cur_trk) {
            uint8_t hdlr[16];
            if (fseeko(fp, body, SEEK_SET) == 0 && fread(hdlr, 1, 12, fp) == 12) {
                uint32_t htype = read_be32(hdlr + 8);
                if (htype == FOURCC('v','i','d','e')) cur_trk->is_video = 1;
                if (htype == FOURCC('s','o','u','n')) cur_trk->is_audio = 1;
            }
        } else if (type == FOURCC('s','t','s','d') && cur_trk) {
            int64_t stsd_len = box_end - body;
            if (stsd_len >= 16 && stsd_len <= 65536) {
                uint8_t *sbuf = malloc((size_t)stsd_len);
                if (sbuf) {
                    if (fseeko(fp, body, SEEK_SET) == 0 &&
                        fread(sbuf, 1, (size_t)stsd_len, fp) == (size_t)stsd_len) {
                        uint32_t entry_sz = read_be32(sbuf + 8);
                        uint32_t fmt = read_be32(sbuf + 12);
                        cur_trk->fourcc = fmt;

                        /* Also infer track media type from sample entry FourCC in case stbl precedes hdlr */
                        if (fmt == FOURCC('a','v','c','1') || fmt == FOURCC('a','v','c','3') ||
                            fmt == FOURCC('h','2','6','4') || fmt == FOURCC('H','2','6','4') ||
                            fmt == FOURCC('m','p','4','v')) {
                            cur_trk->is_video = 1;
                        } else if (fmt == FOURCC('m','p','4','a') || fmt == FOURCC('s','o','w','t') ||
                                   fmt == FOURCC('t','w','o','s') || fmt == FOURCC('l','p','c','m') ||
                                   fmt == FOURCC('r','a','w',' ')) {
                            cur_trk->is_audio = 1;
                        }

                        if (cur_trk->is_video && stsd_len >= 44) {
                            int w = read_be16(sbuf + 8 + 32);
                            int h = read_be16(sbuf + 8 + 34);
                            if (w > 0 && h > 0) {
                                cur_trk->width = w;
                                cur_trk->height = h;
                            }
                            /* Locate child avcC box inside sample entry (offset 8 + 86) */
                            int sub = 8 + 86;
                            int sub_end = (entry_sz <= (uint32_t)(stsd_len - 8))
                                          ? (int)(8 + entry_sz) : (int)stsd_len;
                            while (sub + 8 <= sub_end) {
                                uint32_t bsz = read_be32(sbuf + sub);
                                uint32_t btp = read_be32(sbuf + sub + 4);
                                if (bsz < 8 || sub + (int)bsz > sub_end) break;
                                if (btp == FOURCC('a','v','c','C') && load_tables) {
                                    free(cur_trk->extradata);
                                    cur_trk->extradata = convert_avcc_to_annexb(
                                        sbuf + sub + 8, (int)bsz - 8,
                                        &cur_trk->nalu_len_size, &cur_trk->extradata_len);
                                    break;
                                }
                                sub += (int)bsz;
                            }
                        } else if (cur_trk->is_audio && stsd_len >= 44) {
                            cur_trk->channels        = read_be16(sbuf + 8 + 24);
                            cur_trk->bits_per_sample = read_be16(sbuf + 8 + 26);
                            cur_trk->samplerate      = (int)(read_be32(sbuf + 8 + 32) >> 16);
                            if (load_tables) {
                                /* Scan inside audio sample entry for 'esds' box */
                                for (int off = 8 + 28; off + 12 <= (int)stsd_len; off++) {
                                    if (read_be32(sbuf + off + 4) == FOURCC('e','s','d','s')) {
                                        uint32_t esz = read_be32(sbuf + off);
                                        if (esz >= 12 && off + (int)esz <= (int)stsd_len) {
                                            free(cur_trk->extradata);
                                            cur_trk->extradata = extract_esds_asc(
                                                sbuf + off + 8, (int)esz - 8, &cur_trk->extradata_len);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    free(sbuf);
                }
            }
        } else if (type == FOURCC('s','t','s','z') && cur_trk) {
            uint8_t stsz_hdr[12];
            if (fseeko(fp, body, SEEK_SET) == 0 && fread(stsz_hdr, 1, 12, fp) == 12) {
                cur_trk->default_sample_size = read_be32(stsz_hdr + 4);
                cur_trk->sample_count        = read_be32(stsz_hdr + 8);
                if (load_tables && cur_trk->default_sample_size == 0 &&
                    cur_trk->sample_count > 0 && cur_trk->sample_count <= 2000000) {
                    cur_trk->sample_sizes = malloc((size_t)cur_trk->sample_count * sizeof(uint32_t));
                    if (cur_trk->sample_sizes) {
                        size_t r = fread(cur_trk->sample_sizes, sizeof(uint32_t), cur_trk->sample_count, fp);
                        for (size_t i = 0; i < r; i++) {
                            cur_trk->sample_sizes[i] = read_be32((const uint8_t *)&cur_trk->sample_sizes[i]);
                        }
                    }
                }
            }
        } else if (type == FOURCC('s','t','t','s') && cur_trk && load_tables) {
            uint8_t stts_hdr[8];
            if (fseeko(fp, body, SEEK_SET) == 0 && fread(stts_hdr, 1, 8, fp) == 8) {
                uint32_t cnt = read_be32(stts_hdr + 4);
                if (cnt > 0 && cnt <= 1000000) {
                    cur_trk->stts = malloc((size_t)cnt * sizeof(struct mp4_stts_entry));
                    if (cur_trk->stts) {
                        cur_trk->num_stts = cnt;
                        for (uint32_t i = 0; i < cnt; i++) {
                            uint8_t raw[8];
                            if (fread(raw, 1, 8, fp) != 8) { cur_trk->num_stts = i; break; }
                            cur_trk->stts[i].count = read_be32(raw);
                            cur_trk->stts[i].delta = read_be32(raw + 4);
                        }
                    }
                }
            }
        } else if (type == FOURCC('s','t','s','c') && cur_trk && load_tables) {
            uint8_t stsc_hdr[8];
            if (fseeko(fp, body, SEEK_SET) == 0 && fread(stsc_hdr, 1, 8, fp) == 8) {
                uint32_t cnt = read_be32(stsc_hdr + 4);
                if (cnt > 0 && cnt <= 1000000) {
                    cur_trk->stsc = malloc((size_t)cnt * sizeof(struct mp4_stsc_entry));
                    if (cur_trk->stsc) {
                        cur_trk->num_stsc = cnt;
                        for (uint32_t i = 0; i < cnt; i++) {
                            uint8_t raw[12];
                            if (fread(raw, 1, 12, fp) != 12) { cur_trk->num_stsc = i; break; }
                            cur_trk->stsc[i].first_chunk       = read_be32(raw);
                            cur_trk->stsc[i].samples_per_chunk = read_be32(raw + 4);
                            cur_trk->stsc[i].sd_idx            = read_be32(raw + 8);
                        }
                    }
                }
            }
        } else if ((type == FOURCC('s','t','c','o') || type == FOURCC('c','o','6','4')) && cur_trk && load_tables) {
            uint8_t co_hdr[8];
            if (fseeko(fp, body, SEEK_SET) == 0 && fread(co_hdr, 1, 8, fp) == 8) {
                uint32_t cnt = read_be32(co_hdr + 4);
                if (cnt > 0 && cnt <= 2000000) {
                    cur_trk->chunk_offsets = malloc((size_t)cnt * sizeof(uint64_t));
                    if (cur_trk->chunk_offsets) {
                        cur_trk->num_chunks = cnt;
                        if (type == FOURCC('s','t','c','o')) {
                            for (uint32_t i = 0; i < cnt; i++) {
                                uint8_t raw[4];
                                if (fread(raw, 1, 4, fp) != 4) { cur_trk->num_chunks = i; break; }
                                cur_trk->chunk_offsets[i] = (uint64_t)read_be32(raw);
                            }
                        } else {
                            for (uint32_t i = 0; i < cnt; i++) {
                                uint8_t raw[8];
                                if (fread(raw, 1, 8, fp) != 8) { cur_trk->num_chunks = i; break; }
                                cur_trk->chunk_offsets[i] = read_be64(raw);
                            }
                        }
                    }
                }
            }
        } else if (type == FOURCC('s','t','s','s') && cur_trk && load_tables) {
            uint8_t stss_hdr[8];
            if (fseeko(fp, body, SEEK_SET) == 0 && fread(stss_hdr, 1, 8, fp) == 8) {
                uint32_t cnt = read_be32(stss_hdr + 4);
                if (cnt > 0 && cnt <= 1000000) {
                    cur_trk->stss = malloc((size_t)cnt * sizeof(uint32_t));
                    if (cur_trk->stss) {
                        cur_trk->num_stss = cnt;
                        for (uint32_t i = 0; i < cnt; i++) {
                            uint8_t raw[4];
                            if (fread(raw, 1, 4, fp) != 4) { cur_trk->num_stss = i; break; }
                            cur_trk->stss[i] = read_be32(raw);
                        }
                    }
                }
            }
        }

        pos = box_end;
    }
}

static uint32_t mp4_track_sample_size(const struct mp4_track_table *t, uint32_t idx)
{
    if (t->default_sample_size != 0)
        return t->default_sample_size;
    if (t->sample_sizes && idx < t->sample_count)
        return t->sample_sizes[idx];
    return 0;
}

static bool mp4_track_is_keyframe(const struct mp4_track_table *t, uint32_t idx)
{
    if (!t->stss || t->num_stss == 0)
        return true;
    uint32_t target_1based = idx + 1;
    uint32_t lo = 0, hi = t->num_stss;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (t->stss[mid] == target_1based) return true;
        if (t->stss[mid] < target_1based) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

/* Position MP4 track cursor at exact sample_idx (0-based) */
static int mp4_track_seek_sample(struct mp4_track_table *t, uint32_t target_idx)
{
    if (!t || !t->valid || t->num_stsc == 0 || t->num_chunks == 0)
        return -1;
    if (target_idx >= t->sample_count) {
        t->cur_sample = t->sample_count;
        return 0;
    }

    uint32_t sample_accum = 0;
    for (uint32_t i = 0; i < t->num_stsc; i++) {
        uint32_t fc0 = t->stsc[i].first_chunk > 0 ? (t->stsc[i].first_chunk - 1) : 0;
        uint32_t nc0 = (i + 1 < t->num_stsc && t->stsc[i + 1].first_chunk > 0)
                       ? (t->stsc[i + 1].first_chunk - 1) : t->num_chunks;
        if (nc0 > t->num_chunks) nc0 = t->num_chunks;
        uint32_t spc = t->stsc[i].samples_per_chunk;
        if (spc == 0 || nc0 <= fc0) continue;

        uint64_t run_samples = (uint64_t)(nc0 - fc0) * (uint64_t)spc;
        if ((uint64_t)target_idx < (uint64_t)sample_accum + run_samples) {
            uint32_t rel = target_idx - sample_accum;
            uint32_t chunk_idx = fc0 + (rel / spc);
            uint32_t sin_chunk = rel % spc;
            uint32_t first_s = target_idx - sin_chunk;
            uint64_t off = t->chunk_offsets[chunk_idx];
            for (uint32_t s = first_s; s < target_idx; s++) {
                off += mp4_track_sample_size(t, s);
            }
            t->cur_sample = target_idx;
            t->cur_chunk = chunk_idx;
            t->cur_stsc_idx = i;
            t->sample_in_chunk = sin_chunk;
            t->cur_offset = off;
            break;
        }
        sample_accum += (uint32_t)run_samples;
    }

    /* Compute DTS ticks for target_idx from stts */
    uint64_t ticks = 0;
    uint32_t rem = target_idx;
    t->cur_stts_idx = 0;
    t->sample_in_stts = 0;
    for (uint32_t i = 0; i < t->num_stts; i++) {
        if (rem < t->stts[i].count) {
            ticks += (uint64_t)rem * (uint64_t)t->stts[i].delta;
            t->cur_stts_idx = i;
            t->sample_in_stts = rem;
            break;
        }
        ticks += (uint64_t)t->stts[i].count * (uint64_t)t->stts[i].delta;
        rem -= t->stts[i].count;
    }
    t->cur_dts_ticks = ticks;
    return 0;
}

/* Find sample index corresponding to target_ms using stts */
static uint32_t mp4_track_ms_to_sample(const struct mp4_track_table *t, int64_t target_ms)
{
    if (!t || !t->valid || t->timescale == 0 || target_ms <= 0)
        return 0;
    uint64_t target_ticks = ((uint64_t)target_ms * (uint64_t)t->timescale) / 1000ULL;
    uint64_t accum_ticks = 0;
    uint32_t accum_samples = 0;

    for (uint32_t i = 0; i < t->num_stts; i++) {
        uint32_t cnt = t->stts[i].count;
        uint32_t delta = t->stts[i].delta;
        if (delta == 0) {
            accum_samples += cnt;
            continue;
        }
        uint64_t run_ticks = (uint64_t)cnt * (uint64_t)delta;
        if (target_ticks < accum_ticks + run_ticks) {
            uint32_t s = accum_samples + (uint32_t)((target_ticks - accum_ticks) / delta);
            return (s < t->sample_count) ? s : (t->sample_count > 0 ? t->sample_count - 1 : 0);
        }
        accum_ticks += run_ticks;
        accum_samples += cnt;
    }
    return t->sample_count > 0 ? t->sample_count - 1 : 0;
}

/* Pop next sample from MP4 track cursor in O(1) time */
static int mp4_track_next_sample(struct mp4_track_table *t,
                                 uint64_t *offset_out, uint32_t *size_out,
                                 int64_t *dts_ms_out, bool *is_keyframe_out)
{
    if (!t || !t->valid || t->cur_sample >= t->sample_count)
        return 0;

    uint32_t idx = t->cur_sample;
    uint32_t sz  = mp4_track_sample_size(t, idx);
    *offset_out  = t->cur_offset;
    *size_out    = sz;
    if (dts_ms_out) {
        *dts_ms_out = t->timescale > 0
                      ? (int64_t)((t->cur_dts_ticks * 1000ULL) / t->timescale)
                      : (int64_t)idx * 33;
    }
    if (is_keyframe_out)
        *is_keyframe_out = mp4_track_is_keyframe(t, idx);

    /* Advance sample & chunk cursor */
    t->cur_sample++;
    t->cur_offset += sz;
    t->sample_in_chunk++;
    if (t->cur_stsc_idx < t->num_stsc &&
        t->sample_in_chunk >= t->stsc[t->cur_stsc_idx].samples_per_chunk) {
        t->cur_chunk++;
        t->sample_in_chunk = 0;
        while (t->cur_stsc_idx + 1 < t->num_stsc &&
               (t->cur_chunk + 1) >= t->stsc[t->cur_stsc_idx + 1].first_chunk) {
            t->cur_stsc_idx++;
        }
        if (t->cur_chunk < t->num_chunks)
            t->cur_offset = t->chunk_offsets[t->cur_chunk];
    }

    /* Advance DTS ticks */
    if (t->cur_stts_idx < t->num_stts) {
        t->cur_dts_ticks += t->stts[t->cur_stts_idx].delta;
        t->sample_in_stts++;
        if (t->sample_in_stts >= t->stts[t->cur_stts_idx].count) {
            t->cur_stts_idx++;
            t->sample_in_stts = 0;
        }
    }
    return 1;
}

/* =========================================================================
 * Matroska (MKV) EBML Metadata & Packet Demuxer
 * ========================================================================= */
static int read_ebml_vint(FILE *fp, uint64_t *val_out, int strip_length_bit)
{
    int c = fgetc(fp);
    if (c == EOF) return 0;
    uint8_t first = (uint8_t)c;
    if (first == 0) return 0;

    int len = 1;
    uint8_t mask = 0x80;
    while (len <= 8 && !(first & mask)) {
        mask >>= 1;
        len++;
    }
    if (len > 8) return 0;

    uint64_t val = strip_length_bit ? (first & (mask - 1)) : first;
    for (int i = 1; i < len; i++) {
        c = fgetc(fp);
        if (c == EOF) return 0;
        val = (val << 8) | (uint8_t)c;
    }
    *val_out = val;
    return len;
}

static uint64_t read_ebml_uint(FILE *fp, uint64_t sz)
{
    uint64_t v = 0;
    if (sz > 8) return 0;
    for (uint64_t i = 0; i < sz; i++) {
        int c = fgetc(fp);
        if (c == EOF) break;
        v = (v << 8) | (uint8_t)c;
    }
    return v;
}

static double read_ebml_float(FILE *fp, uint64_t sz)
{
    if (sz == 4) {
        uint32_t u = (uint32_t)read_ebml_uint(fp, 4);
        float f;
        memcpy(&f, &u, sizeof(f));
        return (double)f;
    } else if (sz == 8) {
        uint64_t u = read_ebml_uint(fp, 8);
        double d;
        memcpy(&d, &u, sizeof(d));
        return d;
    }
    return 0.0;
}

struct mkv_demux_info {
    int      is_mkv;
    uint64_t timecode_scale;
    int64_t  first_cluster_pos;
    int      v_track_num;
    int      a_track_num;
    int      v_nalu_len_size;
    uint8_t *v_extradata;
    int      v_extradata_len;
    uint8_t *a_extradata;
    int      a_extradata_len;

    int      cur_track_num;
    int      cur_track_type;
    uint8_t *cur_codec_priv;
    int      cur_codec_priv_len;
};

static void parse_mkv_elements(FILE *fp, int64_t start, int64_t end,
                               struct y1vid_meta *meta, struct mkv_demux_info *mkv,
                               char *cur_tag_name, size_t tag_name_sz, int depth)
{
    if (depth > 8) return;
    int64_t pos = start;

    while (pos < end) {
        if (fseeko(fp, pos, SEEK_SET) != 0) break;
        uint64_t id = 0, sz = 0;
        int id_len = read_ebml_vint(fp, &id, 0);
        if (!id_len) break;
        int sz_len = read_ebml_vint(fp, &sz, 1);
        if (!sz_len) break;

        int64_t body = pos + id_len + sz_len;
        int64_t el_end = (sz >= 0x00FFFFFFFFFFFFFFULL || body + (int64_t)sz > end)
                         ? end : (body + (int64_t)sz);

        if (id == 0x1F43B675ULL) {
            if (mkv && mkv->first_cluster_pos == 0)
                mkv->first_cluster_pos = pos;
            break; /* Stop at first Cluster */
        }

        if (id == 0xAEULL && mkv) { /* TrackEntry */
            mkv->cur_track_num = 0;
            mkv->cur_track_type = 0;
            free(mkv->cur_codec_priv);
            mkv->cur_codec_priv = NULL;
            mkv->cur_codec_priv_len = 0;
            parse_mkv_elements(fp, body, el_end, meta, mkv, cur_tag_name, tag_name_sz, depth + 1);
            if (mkv->cur_track_type == 1 && mkv->v_track_num == 0) {
                mkv->v_track_num = mkv->cur_track_num;
                if (mkv->cur_codec_priv && mkv->cur_codec_priv_len > 0) {
                    mkv->v_extradata = convert_avcc_to_annexb(
                        mkv->cur_codec_priv, mkv->cur_codec_priv_len,
                        &mkv->v_nalu_len_size, &mkv->v_extradata_len);
                }
            } else if (mkv->cur_track_type == 2 && mkv->a_track_num == 0) {
                mkv->a_track_num = mkv->cur_track_num;
                if (mkv->cur_codec_priv && mkv->cur_codec_priv_len > 0) {
                    mkv->a_extradata = calloc((size_t)mkv->cur_codec_priv_len + 16, 1);
                    if (mkv->a_extradata) {
                        memcpy(mkv->a_extradata, mkv->cur_codec_priv, (size_t)mkv->cur_codec_priv_len);
                        mkv->a_extradata_len = mkv->cur_codec_priv_len;
                    }
                }
            }
            free(mkv->cur_codec_priv);
            mkv->cur_codec_priv = NULL;
            mkv->cur_codec_priv_len = 0;
        } else if (id == 0x18538067ULL || id == 0x1549A966ULL || id == 0x1654AE6BULL ||
                   id == 0xE0ULL       || id == 0xE1ULL       ||
                   id == 0x1254C367ULL || id == 0x7373ULL     || id == 0x67C8ULL) {
            parse_mkv_elements(fp, body, el_end, meta, mkv, cur_tag_name, tag_name_sz, depth + 1);
        } else if (id == 0x2AD7B1ULL && mkv) {
            mkv->timecode_scale = read_ebml_uint(fp, sz);
        } else if (id == 0x4489ULL) {
            double dur = read_ebml_float(fp, sz);
            uint64_t tc = (mkv && mkv->timecode_scale > 0) ? mkv->timecode_scale : 1000000ULL;
            double scale_ms = (double)tc / 1000000.0;
            meta->duration_ms = (int64_t)(dur * scale_ms);
        } else if (id == 0x7BA9ULL && sz > 0 && sz < sizeof(meta->title)) {
            size_t r = fread(meta->title, 1, (size_t)sz, fp);
            meta->title[r] = '\0';
        } else if (id == 0xD7ULL && mkv) {
            mkv->cur_track_num = (int)read_ebml_uint(fp, sz);
        } else if (id == 0x83ULL) {
            uint64_t ttype = read_ebml_uint(fp, sz);
            if (mkv) mkv->cur_track_type = (int)ttype;
            if (ttype == 1) meta->has_video = 1;
            else if (ttype == 2) meta->has_audio = 1;
        } else if (id == 0x63A2ULL && mkv && sz > 0 && sz <= 4096) {
            free(mkv->cur_codec_priv);
            mkv->cur_codec_priv = malloc((size_t)sz);
            if (mkv->cur_codec_priv) {
                if (fread(mkv->cur_codec_priv, 1, (size_t)sz, fp) == (size_t)sz)
                    mkv->cur_codec_priv_len = (int)sz;
            }
        } else if (id == 0x86ULL && sz > 0 && sz < 64) {
            char cid[64];
            size_t r = fread(cid, 1, (size_t)sz, fp);
            cid[r] = '\0';
            if (strncmp(cid, "V_", 2) == 0 && !meta->codec_name[0]) {
                snprintf(meta->codec_name, sizeof(meta->codec_name), "%s", cid + 2);
            }
        } else if (id == 0x23E383ULL) {
            uint64_t f_ns = read_ebml_uint(fp, sz);
            if (f_ns > 0) meta->fps_x100 = (int)(100000000000ULL / f_ns);
        } else if (id == 0xB0ULL) {
            meta->width = (int)read_ebml_uint(fp, sz);
        } else if (id == 0xBAULL) {
            meta->height = (int)read_ebml_uint(fp, sz);
        } else if (id == 0xB5ULL) {
            meta->audio_samplerate = (int)read_ebml_float(fp, sz);
        } else if (id == 0x9FULL) {
            meta->audio_channels = (int)read_ebml_uint(fp, sz);
        } else if (id == 0x45A3ULL && sz > 0 && sz < tag_name_sz) {
            size_t r = fread(cur_tag_name, 1, (size_t)sz, fp);
            cur_tag_name[r] = '\0';
        } else if (id == 0x4487ULL && sz > 0 && sz < 128) {
            char val[128];
            size_t r = fread(val, 1, (size_t)sz, fp);
            val[r] = '\0';
            if (strcasecmp(cur_tag_name, "TITLE") == 0 && !meta->title[0]) {
                snprintf(meta->title, sizeof(meta->title), "%s", val);
            } else if ((strcasecmp(cur_tag_name, "ARTIST") == 0 ||
                        strcasecmp(cur_tag_name, "DIRECTOR") == 0 ||
                        strcasecmp(cur_tag_name, "CREATOR") == 0) && !meta->creator[0]) {
                snprintf(meta->creator, sizeof(meta->creator), "%s", val);
            } else if ((strcasecmp(cur_tag_name, "SHOW") == 0 ||
                        strcasecmp(cur_tag_name, "TVSHOW") == 0 ||
                        strcasecmp(cur_tag_name, "COLLECTION") == 0 ||
                        strcasecmp(cur_tag_name, "ALBUM") == 0) && !meta->tv_show[0]) {
                snprintf(meta->tv_show, sizeof(meta->tv_show), "%s", val);
            } else if (strcasecmp(cur_tag_name, "DATE_RELEASED") == 0 ||
                       strcasecmp(cur_tag_name, "DATE") == 0 ||
                       strcasecmp(cur_tag_name, "YEAR") == 0) {
                int y = atoi(val);
                if (y >= 1900 && y <= 2100) meta->year = y;
            }
        }

        pos = el_end;
    }
}

static void fill_fallback_meta_from_path(const char *path, struct y1vid_meta *meta)
{
    char stem[128];
    extract_filename_stem(path, stem, sizeof(stem));

    if (!meta->title[0])
        snprintf(meta->title, sizeof(meta->title), "%s", stem);

    if (!meta->tv_show[0]) {
        const char *last_slash = strrchr(path, '/');
        if (last_slash && last_slash > path) {
            const char *prev_slash = last_slash - 1;
            while (prev_slash > path && *prev_slash != '/') prev_slash--;
            if (*prev_slash == '/') {
                size_t dlen = (size_t)(last_slash - (prev_slash + 1));
                if (dlen > 0 && dlen < sizeof(meta->tv_show)) {
                    char dir_name[128];
                    memcpy(dir_name, prev_slash + 1, dlen);
                    dir_name[dlen] = '\0';
                    if (strcasecmp(dir_name, "Videos") != 0 &&
                        strcasecmp(dir_name, "sdcard") != 0) {
                        snprintf(meta->tv_show, sizeof(meta->tv_show), "%s", dir_name);
                    }
                }
            }
        }
    }

    for (int i = 0; stem[i] && stem[i + 1] && stem[i + 2]; i++) {
        if ((stem[i] == 'S' || stem[i] == 's') &&
            stem[i + 1] >= '0' && stem[i + 1] <= '9') {
            int s = 0, e = 0;
            if (sscanf(stem + i + 1, "%dE%d", &s, &e) == 2 ||
                sscanf(stem + i + 1, "%de%d", &s, &e) == 2) {
                if (!meta->season)  meta->season = s;
                if (!meta->episode) meta->episode = e;
                break;
            }
        }
    }

    if (meta->year == 0) {
        for (int i = 0; stem[i]; i++) {
            if ((stem[i] == '(' || stem[i] == '[' || stem[i] == '.' || stem[i] == ' ') &&
                stem[i + 1] >= '1' && stem[i + 1] <= '2') {
                int y = atoi(stem + i + 1);
                if (y >= 1920 && y <= 2099) {
                    meta->year = y;
                    break;
                }
            }
        }
    }

    if (!meta->creator[0])
        snprintf(meta->creator, sizeof(meta->creator), "%s",
                 meta->tv_show[0] ? meta->tv_show : "Unknown Creator");
    if (!meta->tv_show[0])
        snprintf(meta->tv_show, sizeof(meta->tv_show), "Unassigned");
}

static void generate_fallback_thumb(const struct y1vid_meta *meta, uint16_t *thumb, int tw, int th)
{
    if (!thumb || tw <= 0 || th <= 0) return;
    uint32_t hash = 2166136261u;
    for (int i = 0; meta->title[i]; i++)
        hash = (hash ^ (uint8_t)meta->title[i]) * 16777619u;

    int r_base = 24 + (hash & 0x3F);
    int g_base = 28 + ((hash >> 8) & 0x3F);
    int b_base = 48 + ((hash >> 16) & 0x5F);

    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            if (x < 6 || x >= tw - 6) {
                int hole = ((y % 12) >= 3 && (y % 12) <= 8 && (x >= 2 && x <= 3)) ||
                           ((y % 12) >= 3 && (y % 12) <= 8 && (x >= tw - 4 && x <= tw - 3));
                thumb[y * tw + x] = hole ? 0x3186 : 0x1082;
                continue;
            }
            int r = r_base + (x * 40) / tw;
            int g = g_base + (y * 40) / th;
            int b = b_base + ((tw - x) * 30) / tw;
            int cx = tw / 2 - 6;
            int cy = th / 2;
            int dy = abs(y - cy);
            if (x >= cx && x <= cx + 14 && dy <= (cx + 14 - x) * 2 / 3) {
                r = 240; g = 244; b = 255;
            }
            uint16_t px = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            thumb[y * tw + x] = px;
        }
    }
}

/* =========================================================================
 * Runtime Context for Video & Audio Decoding
 * ========================================================================= */
#define AUDIO_RING_SAMPLES (48000 * 4) /* 4 seconds stereo buffer */
#define MAX_PACKET_SIZE    (2 * 1024 * 1024)

/* Precomputed branchless BT.601 YUV420P -> RGB565 lookup tables (8 KB total, fits in L1D cache) */
static int      g_yuv_tables_ready = 0;
static int16_t  g_cr_to_r[256];
static int16_t  g_cb_to_b[256];
static int16_t  g_cb_to_g[256];
static int16_t  g_cr_to_g[256];
static uint16_t g_clip_r5[1024];
static uint16_t g_clip_g6[1024];
static uint16_t g_clip_b5[1024];

static void init_yuv_tables(void)
{
    if (g_yuv_tables_ready) return;
    for (int i = 0; i < 256; i++) {
        int v = i - 128;
        g_cr_to_r[i] = (int16_t)((359 * v) >> 8);
        g_cb_to_b[i] = (int16_t)((454 * v) >> 8);
        g_cb_to_g[i] = (int16_t)(-((88 * v) >> 8));
        g_cr_to_g[i] = (int16_t)(-((183 * v) >> 8));
    }
    for (int i = 0; i < 1024; i++) {
        int val = i - 384;
        if (val < 0)   val = 0;
        if (val > 255) val = 255;
        g_clip_r5[i] = (uint16_t)((val & 0xF8) << 8);
        g_clip_g6[i] = (uint16_t)((val & 0xFC) << 3);
        g_clip_b5[i] = (uint16_t)(val >> 3);
    }
    g_yuv_tables_ready = 1;
}

struct y1vid_context {
    char path[512];
    FILE *fp;
    FILE *a_fp;
    uint8_t *v_io_buf;
    uint8_t *a_io_buf;
    uint64_t v_file_pos;
    uint64_t a_file_pos;
    struct y1vid_meta meta;

    int out_w;
    int out_h;
    int scale_mode;
    int out_audio_sampr;

    int64_t cur_pos_ms;
    int eof_reached;

    /* Container state */
    int is_mkv;
    struct mp4_track_table v_mp4;
    struct mp4_track_table a_mp4;
    struct mkv_demux_info  mkv;
    int64_t mkv_cur_pos;
    int64_t mkv_cluster_end;
    int64_t mkv_cluster_tc_ms;

    /* Built-in FFmpeg H.264 decoder handles */
    void    *v_ctx;
    void    *v_frame;
    int      need_sps_pps;
    int      has_decoded_frame;
    int      reorder_packets;
    int      first_frame_out;
    int64_t  reorder_delay_ms;
    uint8_t *raw_buf;
    size_t   raw_buf_cap;
    uint8_t *pkt_buf;
    size_t   pkt_buf_cap;

    /* Built-in FAAD2 AAC decoder handle */
    NeAACDecHandle aac_dec;
    uint32_t       aac_samplerate;
    uint8_t        aac_channels;

    /* Scaled output geometry & precomputed coordinate maps */
    int last_src_w;
    int last_src_h;
    int scaled_w;
    int scaled_h;
    int dst_x;
    int dst_y;
    int x_map[480];
    int y_map[360];

    /* Lock-Free Single-Producer Single-Consumer Stereo 16-bit PCM Ring Buffer */
    int16_t          *audio_ring;
    volatile uint32_t audio_ring_rpos;
    volatile uint32_t audio_ring_wpos;
    volatile int64_t  audio_wpos_pts_ms;
    int64_t           audio_pts_ms;
    uint32_t          resample_pos_fp;
};

static void compute_scaling_rect(y1vid_ctx *ctx, int src_w, int src_h, int out_w, int out_h,
                                 int scale_mode, int *dst_x, int *dst_y, int *scaled_w, int *scaled_h)
{
    if (src_w <= 0 || src_h <= 0 || scale_mode == Y1VID_SCALE_STRETCH) {
        *dst_x = 0;
        *dst_y = 0;
        *scaled_w = out_w;
        *scaled_h = out_h;
    } else if (scale_mode == Y1VID_SCALE_FILL) {
        if (src_w * out_h > out_w * src_h) {
            *scaled_h = out_h;
            *scaled_w = (src_w * out_h) / src_h;
        } else {
            *scaled_w = out_w;
            *scaled_h = (src_h * out_w) / src_w;
        }
        *scaled_w = (*scaled_w + 1) & ~1;
        *scaled_h = (*scaled_h + 1) & ~1;
        if (*scaled_w < 2) *scaled_w = 2;
        if (*scaled_h < 2) *scaled_h = 2;
        *dst_x = (out_w - *scaled_w) / 2;
        *dst_y = (out_h - *scaled_h) / 2;
    } else {
        if (src_w * out_h > out_w * src_h) {
            *scaled_w = out_w;
            *scaled_h = (src_h * out_w) / src_w;
        } else {
            *scaled_h = out_h;
            *scaled_w = (src_w * out_h) / src_h;
        }
        *scaled_w = (*scaled_w + 1) & ~1;
        *scaled_h = (*scaled_h + 1) & ~1;
        if (*scaled_w < 2) *scaled_w = 2;
        if (*scaled_h < 2) *scaled_h = 2;
        *dst_x = (out_w - *scaled_w) / 2;
        *dst_y = (out_h - *scaled_h) / 2;
    }

    if (ctx && src_w > 0 && src_h > 0 && *scaled_w > 0 && *scaled_h > 0) {
        int max_w = out_w > 480 ? 480 : out_w;
        int max_h = out_h > 360 ? 360 : out_h;
        uint32_t step_x_fp = ((uint32_t)src_w << 16) / (uint32_t)(*scaled_w);
        uint32_t step_y_fp = ((uint32_t)src_h << 16) / (uint32_t)(*scaled_h);
        for (int dx = 0; dx < max_w; dx++) {
            int rel_x = dx - *dst_x;
            if (rel_x < 0) rel_x = 0;
            int sx = (int)(((uint32_t)rel_x * step_x_fp) >> 16);
            if (sx >= src_w) sx = src_w - 1;
            ctx->x_map[dx] = sx;
        }
        for (int dy = 0; dy < max_h; dy++) {
            int rel_y = dy - *dst_y;
            if (rel_y < 0) rel_y = 0;
            int sy = (int)(((uint32_t)rel_y * step_y_fp) >> 16);
            if (sy >= src_h) sy = src_h - 1;
            ctx->y_map[dy] = sy;
        }
    }
}

/* Fast direct YUV420P -> RGB565 360p downscaler for MT6572:
 * Uses precomputed coordinate maps and L1-cached branchless lookup tables,
 * processing pixel pairs with shared chroma to minimize Cortex-A7 CPU cycles. */
static void fast_downscale_yuv420p_to_rgb565(const y1vid_ctx *ctx,
                                             const uint8_t *y_plane, const uint8_t *u_plane, const uint8_t *v_plane,
                                             int y_stride, int u_stride, int v_stride,
                                             uint16_t *rgb565_buf, int out_w, int out_h,
                                             int dst_x, int dst_y, int scaled_w, int scaled_h)
{
    int y_start = dst_y < 0 ? 0 : dst_y;
    int y_end   = (dst_y + scaled_h > out_h) ? out_h : (dst_y + scaled_h);
    int x_start = dst_x < 0 ? 0 : dst_x;
    int x_end   = (dst_x + scaled_w > out_w) ? out_w : (dst_x + scaled_w);

    /* Clear only top and bottom letterbox bars if present */
    if (y_start > 0) {
        memset(rgb565_buf, 0, (size_t)y_start * (size_t)out_w * sizeof(uint16_t));
    }
    if (y_end < out_h) {
        memset(rgb565_buf + (size_t)y_end * (size_t)out_w, 0,
               (size_t)(out_h - y_end) * (size_t)out_w * sizeof(uint16_t));
    }

    const uint16_t *r5_tbl = &g_clip_r5[384];
    const uint16_t *g6_tbl = &g_clip_g6[384];
    const uint16_t *b5_tbl = &g_clip_b5[384];
    const int *x_map = ctx->x_map;
    const int *y_map = ctx->y_map;

    for (int dy = y_start; dy < y_end; dy++) {
        int sy = y_map[dy];
        const uint8_t *y_row = y_plane + sy * y_stride;
        const uint8_t *u_row = u_plane + (sy >> 1) * u_stride;
        const uint8_t *v_row = v_plane + (sy >> 1) * v_stride;
        uint16_t *dst_row = rgb565_buf + dy * out_w;

        if (x_start > 0) {
            memset(dst_row, 0, (size_t)x_start * sizeof(uint16_t));
        }
        if (x_end < out_w) {
            memset(dst_row + x_end, 0, (size_t)(out_w - x_end) * sizeof(uint16_t));
        }

        int dx = x_start;
        for (; dx + 1 < x_end; dx += 2) {
            int sx0 = x_map[dx];
            int sx1 = x_map[dx + 1];
            int cx  = sx0 >> 1;

            int u  = u_row[cx];
            int v  = v_row[cx];
            int rv = g_cr_to_r[v];
            int gv = g_cb_to_g[u] + g_cr_to_g[v];
            int bv = g_cb_to_b[u];

            int y0 = y_row[sx0];
            dst_row[dx] = r5_tbl[y0 + rv] | g6_tbl[y0 + gv] | b5_tbl[y0 + bv];

            int y1 = y_row[sx1];
            dst_row[dx + 1] = r5_tbl[y1 + rv] | g6_tbl[y1 + gv] | b5_tbl[y1 + bv];
        }
        for (; dx < x_end; dx++) {
            int sx = x_map[dx];
            int cx = sx >> 1;
            int y0 = y_row[sx];
            int u  = u_row[cx];
            int v  = v_row[cx];
            int rv = g_cr_to_r[v];
            int gv = g_cb_to_g[u] + g_cr_to_g[v];
            int bv = g_cb_to_b[u];
            dst_row[dx] = r5_tbl[y0 + rv] | g6_tbl[y0 + gv] | b5_tbl[y0 + bv];
        }
    }
}

static void blit_decoded_avframe(y1vid_ctx *ctx, void *v_frame, uint16_t *rgb565_buf)
{
    const uint8_t *fbytes = (const uint8_t *)v_frame;
    const uint8_t *y_plane = *(const uint8_t *const *)(fbytes + 0x00);
    const uint8_t *u_plane = *(const uint8_t *const *)(fbytes + 0x04);
    const uint8_t *v_plane = *(const uint8_t *const *)(fbytes + 0x08);
    int y_stride = *(const int *)(fbytes + 0x20);
    int u_stride = *(const int *)(fbytes + 0x24);
    int v_stride = *(const int *)(fbytes + 0x28);
    int fw       = *(const int *)(fbytes + 0x44);
    int fh       = *(const int *)(fbytes + 0x48);

    int src_w = fw > 0 ? fw : ctx->meta.width;
    int src_h = fh > 0 ? fh : ctx->meta.height;
    if (!y_plane || !u_plane || !v_plane || src_w <= 0 || src_h <= 0)
        return;

    ctx->meta.width = src_w;
    ctx->meta.height = src_h;

    if (src_w != ctx->last_src_w || src_h != ctx->last_src_h) {
        ctx->last_src_w = src_w;
        ctx->last_src_h = src_h;
        compute_scaling_rect(ctx, src_w, src_h, ctx->out_w, ctx->out_h, ctx->scale_mode,
                             &ctx->dst_x, &ctx->dst_y, &ctx->scaled_w, &ctx->scaled_h);
    }

    fast_downscale_yuv420p_to_rgb565(ctx, y_plane, u_plane, v_plane,
                                     y_stride, u_stride, v_stride,
                                     rgb565_buf, ctx->out_w, ctx->out_h,
                                     ctx->dst_x, ctx->dst_y,
                                     ctx->scaled_w, ctx->scaled_h);
}

static inline int16_t clip_s16(int32_t v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void push_stereo_pcm_to_ring(y1vid_ctx *ctx, const int16_t *stereo_in,
                                    int in_samples, int in_sr, int64_t pkt_dts_ms, bool is_aac)
{
    if (!ctx || !ctx->audio_ring || in_samples <= 0) return;
    int out_sr = ctx->out_audio_sampr > 0 ? ctx->out_audio_sampr : 44100;
    if (in_sr <= 0) in_sr = out_sr;

    uint32_t wpos = ctx->audio_ring_wpos;
    uint32_t rpos = ctx->audio_ring_rpos;
    uint32_t pushed = 0;

    if (in_sr == out_sr) {
        for (int i = 0; i < in_samples; i++) {
            if (wpos - rpos >= AUDIO_RING_SAMPLES) break;
            uint32_t slot = (wpos % AUDIO_RING_SAMPLES) * 2;
            ctx->audio_ring[slot]     = stereo_in[i * 2];
            ctx->audio_ring[slot + 1] = stereo_in[i * 2 + 1];
            wpos++;
            pushed++;
        }
    } else {
        /* Continuous fractional phase accumulator prevents sample truncation drift (e.g. 48kHz -> 44.1kHz) */
        uint32_t step_fp = (uint32_t)(((uint64_t)in_sr << 16) / (uint64_t)out_sr);
        uint32_t pos_fp  = ctx->resample_pos_fp;
        uint32_t end_fp  = (uint32_t)in_samples << 16;
        while (pos_fp < end_fp) {
            if (wpos - rpos >= AUDIO_RING_SAMPLES) break;
            int idx = (int)(pos_fp >> 16);
            if (idx >= in_samples) idx = in_samples - 1;
            uint32_t slot = (wpos % AUDIO_RING_SAMPLES) * 2;
            ctx->audio_ring[slot]     = stereo_in[idx * 2];
            ctx->audio_ring[slot + 1] = stereo_in[idx * 2 + 1];
            wpos++;
            pushed++;
            pos_fp += step_fp;
        }
        ctx->resample_pos_fp = (pos_fp >= end_fp) ? (pos_fp - end_fp) : 0;
    }

    /* Update exact media timestamp of write head (wpos):
     * For FAAD2 AAC, Frame k outputs MDCT samples of Frame k-1, whose end timestamp is pkt_dts_ms.
     * For uncompressed PCM, end timestamp is pkt_dts_ms + pushed_ms. */
    if (pkt_dts_ms >= 0) {
        if (is_aac) {
            ctx->audio_wpos_pts_ms = pkt_dts_ms;
        } else {
            ctx->audio_wpos_pts_ms = pkt_dts_ms + ((int64_t)pushed * 1000LL) / out_sr;
        }
    } else {
        ctx->audio_wpos_pts_ms += ((int64_t)pushed * 1000LL) / out_sr;
    }
    ctx->audio_ring_wpos = wpos;
}

static void decode_audio_packet_bytes(y1vid_ctx *ctx, uint8_t *buf, uint32_t sz, int64_t dts_ms)
{
    if (!ctx || !buf || sz == 0 || !ctx->audio_ring) return;

    if (ctx->aac_dec) {
        /* Rockbox's libfaad/bits.c requires buffer_size + 12 <= 2048 (max 2036 bytes) */
        uint32_t faad_sz = (sz > 2036) ? 2036 : sz;
        NeAACDecFrameInfo fi;
        memset(&fi, 0, sizeof(fi));
        void *ret = NeAACDecDecode(ctx->aac_dec, &fi, buf, faad_sz);
        if (ret != NULL && fi.error == 0 && fi.samples > 0) {
            /* In Rockbox's libfaad, time_out[0] and time_out[1] are at byte offsets 0x24 and 0x28
             * of NeAACDecStruct, holding 29-bit fixed-point (s15.14) int32_t PCM samples. */
            const int32_t *const *time_out =
                (const int32_t *const *)((const uint8_t *)ctx->aac_dec + 0x24);
            const int32_t *ch0 = time_out[0];
            const int32_t *ch1 = time_out[1];
            int frame_samples = (fi.channels > 1) ? (int)(fi.samples / fi.channels) : (int)(fi.samples >> 1);
            if (frame_samples > 2048) frame_samples = 2048;
            if (ch0 && frame_samples > 0) {
                int16_t pcm_tmp[2048 * 2];
                for (int i = 0; i < frame_samples; i++) {
                    int32_t l = (ch0[i] + (1 << 13)) >> 14;
                    int32_t r = ch1 ? ((ch1[i] + (1 << 13)) >> 14) : l;
                    pcm_tmp[i * 2]     = clip_s16(l);
                    pcm_tmp[i * 2 + 1] = clip_s16(r);
                }
                int sr = (fi.samplerate > 0) ? (int)fi.samplerate :
                         (ctx->aac_samplerate > 0 ? (int)ctx->aac_samplerate : ctx->meta.audio_samplerate);
                push_stereo_pcm_to_ring(ctx, pcm_tmp, frame_samples, sr, dts_ms, true);
            }
        }
    } else if (ctx->a_mp4.fourcc == FOURCC('s','o','w','t') ||
               ctx->a_mp4.fourcc == FOURCC('l','p','c','m') ||
               ctx->a_mp4.fourcc == FOURCC('t','w','o','s')) {
        bool big_endian = (ctx->a_mp4.fourcc == FOURCC('t','w','o','s'));
        int ch = ctx->meta.audio_channels > 0 ? ctx->meta.audio_channels : 2;
        int frames = (int)sz / (2 * ch);
        const int16_t *src = (const int16_t *)buf;
        int16_t pcm_tmp[1024 * 2];
        int pos = 0;
        int sr = ctx->meta.audio_samplerate > 0 ? ctx->meta.audio_samplerate : 44100;
        while (pos < frames) {
            int chunk = (frames - pos > 1024) ? 1024 : (frames - pos);
            for (int i = 0; i < chunk; i++) {
                int16_t s0 = src[(pos + i) * ch];
                int16_t s1 = (ch > 1) ? src[(pos + i) * ch + 1] : s0;
                if (big_endian) {
                    s0 = (int16_t)(((uint16_t)s0 >> 8) | ((uint16_t)s0 << 8));
                    s1 = (int16_t)(((uint16_t)s1 >> 8) | ((uint16_t)s1 << 8));
                }
                pcm_tmp[i * 2]     = s0;
                pcm_tmp[i * 2 + 1] = s1;
            }
            int64_t sub_dts = (dts_ms >= 0) ? (dts_ms + ((int64_t)pos * 1000LL) / sr) : -1;
            push_stereo_pcm_to_ring(ctx, pcm_tmp, chunk, sr, sub_dts, false);
            pos += chunk;
        }
    }
}

/* Pump MP4 audio samples using dedicated ctx->a_fp stream so video I/O is never disturbed */
static void pump_mp4_audio(y1vid_ctx *ctx, int max_packets)
{
    FILE *afp = (ctx && ctx->a_fp) ? ctx->a_fp : (ctx ? ctx->fp : NULL);
    if (!ctx || !afp || !ctx->a_mp4.valid || ctx->out_audio_sampr <= 0)
        return;

    /* Keep ~1.0 second (44100 samples) buffered in audio_ring so hardware DMA never underruns */
    uint32_t target_fill = (uint32_t)(ctx->out_audio_sampr > 0 ? ctx->out_audio_sampr : 44100);
    if (target_fill > AUDIO_RING_SAMPLES - 4096)
        target_fill = AUDIO_RING_SAMPLES - 4096;

    uint8_t abuf[16384 + 32];
    int decoded = 0;
    while (ctx->a_mp4.cur_sample < ctx->a_mp4.sample_count && decoded < max_packets) {
        uint32_t buffered = ctx->audio_ring_wpos - ctx->audio_ring_rpos;
        if (buffered >= target_fill)
            break;

        uint64_t a_off = 0;
        uint32_t a_sz  = 0;
        int64_t  a_dts = 0;
        if (!mp4_track_next_sample(&ctx->a_mp4, &a_off, &a_sz, &a_dts, NULL))
            break;
        if (a_sz == 0 || a_sz > 16384)
            continue;
        if (ctx->a_file_pos != a_off) {
            if (fseeko(afp, (off_t)a_off, SEEK_SET) != 0) {
                ctx->a_file_pos = UINT64_MAX;
                break;
            }
        }
        if (fread(abuf, 1, a_sz, afp) != a_sz) {
            ctx->a_file_pos = UINT64_MAX;
            break;
        }
        ctx->a_file_pos = a_off + a_sz;
        memset(abuf + a_sz, 0, 32);
        decode_audio_packet_bytes(ctx, abuf, a_sz, a_dts);
        decoded++;
    }
}

/* Convert length-prefixed H.264 packet into Annex-B packet in ctx->pkt_buf */
static int prepare_h264_annexb_packet(y1vid_ctx *ctx, const uint8_t *raw_in, uint32_t raw_sz,
                                      int nalu_len_sz, const uint8_t *extradata, int extradata_len,
                                      bool prepend_extradata, int *out_pkt_size)
{
    size_t needed = (size_t)raw_sz * 2 + (size_t)extradata_len + 128;
    if (needed > ctx->pkt_buf_cap) {
        uint8_t *nb = realloc(ctx->pkt_buf, needed);
        if (!nb) return -1;
        ctx->pkt_buf = nb;
        ctx->pkt_buf_cap = needed;
    }

    int wpos = 0;
    if (prepend_extradata && extradata && extradata_len > 0) {
        memcpy(ctx->pkt_buf, extradata, (size_t)extradata_len);
        wpos += extradata_len;
    }

    /* Check if raw_in is already Annex-B */
    if (raw_sz >= 4 && raw_in[0] == 0 && raw_in[1] == 0 &&
        (raw_in[2] == 1 || (raw_in[2] == 0 && raw_in[3] == 1))) {
        memcpy(ctx->pkt_buf + wpos, raw_in, raw_sz);
        wpos += (int)raw_sz;
    } else {
        if (nalu_len_sz < 1 || nalu_len_sz > 4) nalu_len_sz = 4;
        uint32_t rpos = 0;
        while (rpos + (uint32_t)nalu_len_sz <= raw_sz) {
            uint32_t nlen = 0;
            for (int i = 0; i < nalu_len_sz; i++)
                nlen = (nlen << 8) | raw_in[rpos + i];
            rpos += (uint32_t)nalu_len_sz;
            if (nlen == 0 || rpos + nlen > raw_sz)
                break;
            ctx->pkt_buf[wpos++] = 0x00;
            ctx->pkt_buf[wpos++] = 0x00;
            ctx->pkt_buf[wpos++] = 0x00;
            ctx->pkt_buf[wpos++] = 0x01;
            memcpy(ctx->pkt_buf + wpos, raw_in + rpos, nlen);
            wpos += (int)nlen;
            rpos += nlen;
        }
    }

    memset(ctx->pkt_buf + wpos, 0, 32); /* FF_INPUT_BUFFER_PADDING_SIZE */
    *out_pkt_size = wpos;
    return (wpos > 0) ? 0 : -1;
}

/* Read next video packet from MKV (while decoding interleaved MKV audio blocks) */
static int read_next_mkv_video_packet(y1vid_ctx *ctx, int *pkt_size_out,
                                      int64_t *pts_ms_out, bool *is_keyframe_out)
{
    if (!ctx || !ctx->fp) return 0;

    uint64_t tc_scale = ctx->mkv.timecode_scale > 0 ? ctx->mkv.timecode_scale : 1000000ULL;

    while (ctx->mkv_cur_pos < ctx->meta.file_size) {
        if (fseeko(ctx->fp, ctx->mkv_cur_pos, SEEK_SET) != 0)
            return 0;

        uint64_t id = 0, sz = 0;
        int id_len = read_ebml_vint(ctx->fp, &id, 0);
        if (!id_len) return 0;
        int sz_len = read_ebml_vint(ctx->fp, &sz, 1);
        if (!sz_len) return 0;

        int64_t body = ctx->mkv_cur_pos + id_len + sz_len;
        int64_t el_end = (sz >= 0x00FFFFFFFFFFFFFFULL || body + (int64_t)sz > ctx->meta.file_size)
                         ? ctx->meta.file_size : (body + (int64_t)sz);

        if (id == 0x1F43B675ULL || id == 0xA0ULL) { /* Cluster or BlockGroup: descend into children */
            if (id == 0x1F43B675ULL)
                ctx->mkv_cluster_end = el_end;
            ctx->mkv_cur_pos = body;
            continue;
        }

        ctx->mkv_cur_pos = el_end;

        if (id == 0xE7ULL) { /* Cluster Timecode */
            uint64_t tc = read_ebml_uint(ctx->fp, sz);
            ctx->mkv_cluster_tc_ms = (int64_t)((tc * tc_scale) / 1000000ULL);
        } else if ((id == 0xA3ULL || id == 0xA1ULL) && sz > 4 && sz < MAX_PACKET_SIZE) {
            uint64_t trk_num = 0;
            int vlen = read_ebml_vint(ctx->fp, &trk_num, 1);
            if (!vlen || (uint64_t)(vlen + 3) >= sz)
                continue;
            uint8_t bhdr[3];
            if (fread(bhdr, 1, 3, ctx->fp) != 3)
                continue;
            int16_t rel_tc = (int16_t)(((uint16_t)bhdr[0] << 8) | bhdr[1]);
            uint8_t flags = bhdr[2];
            int lacing = (flags >> 1) & 0x03;
            int64_t blk_pts_ms = ctx->mkv_cluster_tc_ms + (int64_t)((rel_tc * (int64_t)tc_scale) / 1000000LL);
            if (blk_pts_ms < 0) blk_pts_ms = 0;

            uint32_t data_sz = (uint32_t)(sz - (uint64_t)vlen - 3);
            if (lacing != 0) {
                /* Skip lace header if present for single-frame fallback */
                int c = fgetc(ctx->fp);
                if (c == EOF) continue;
                int laces = c + 1;
                data_sz--;
                if (lacing == 1) {
                    for (int l = 0; l < laces - 1 && data_sz > 0; l++) {
                        int b;
                        do {
                            b = fgetc(ctx->fp);
                            if (b == EOF) break;
                            data_sz--;
                        } while (b == 255 && data_sz > 0);
                    }
                } else if (lacing == 3) {
                    for (int l = 0; l < laces - 1 && data_sz > 0; l++) {
                        uint64_t dummy = 0;
                        int ll = read_ebml_vint(ctx->fp, &dummy, 1);
                        if (!ll || (uint32_t)ll > data_sz) break;
                        data_sz -= (uint32_t)ll;
                    }
                }
            }

            if ((int)trk_num == ctx->mkv.v_track_num) {
                if (data_sz > ctx->raw_buf_cap) {
                    uint8_t *nb = realloc(ctx->raw_buf, data_sz);
                    if (!nb) continue;
                    ctx->raw_buf = nb;
                    ctx->raw_buf_cap = data_sz;
                }
                if (fread(ctx->raw_buf, 1, data_sz, ctx->fp) != data_sz) {
                    continue;
                }
                bool is_kf = (id == 0xA3ULL) ? ((flags & 0x80) != 0) : true;
                int psize = 0;
                int ok = prepare_h264_annexb_packet(
                    ctx, ctx->raw_buf, data_sz,
                    ctx->mkv.v_nalu_len_size > 0 ? ctx->mkv.v_nalu_len_size : 4,
                    ctx->mkv.v_extradata, ctx->mkv.v_extradata_len,
                    is_kf || ctx->need_sps_pps, &psize);
                if (ok == 0 && psize > 0) {
                    ctx->need_sps_pps = 0;
                    *pkt_size_out = psize;
                    *pts_ms_out = blk_pts_ms;
                    *is_keyframe_out = is_kf;
                    return 1;
                }
            } else if ((int)trk_num == ctx->mkv.a_track_num && ctx->out_audio_sampr > 0 && data_sz <= 16384) {
                uint8_t abuf[16384 + 32];
                if (fread(abuf, 1, data_sz, ctx->fp) == data_sz) {
                    memset(abuf + data_sz, 0, 32);
                    decode_audio_packet_bytes(ctx, abuf, data_sz, blk_pts_ms);
                }
            }
        }
    }
    return 0;
}

/* =========================================================================
 * Exported ABI Implementation
 * ========================================================================= */
int y1vid_get_abi_version(void)
{
    init_builtin_h264();
    init_yuv_tables();
    return Y1VID_ABI_VERSION;
}

int y1vid_probe_metadata(const char *path, struct y1vid_meta *meta,
                         uint16_t *thumb_rgb565, int thumb_w, int thumb_h)
{
    if (!path || !meta) return -1;
    memset(meta, 0, sizeof(*meta));

    struct stat st;
    if (stat(path, &st) == 0) {
        meta->file_size = (int64_t)st.st_size;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    uint8_t magic[12];
    size_t r = fread(magic, 1, sizeof(magic), fp);
    if (r >= 8) {
        uint32_t ebml_id = read_be32(magic);
        uint32_t box_type = read_be32(magic + 4);
        if (ebml_id == 0x1A45DFA3u) {
            struct mkv_demux_info mkv_tmp;
            memset(&mkv_tmp, 0, sizeof(mkv_tmp));
            mkv_tmp.timecode_scale = 1000000ULL;
            char cur_tag[64] = {0};
            parse_mkv_elements(fp, 0, meta->file_size, meta, &mkv_tmp, cur_tag, sizeof(cur_tag), 0);
            free(mkv_tmp.v_extradata);
            free(mkv_tmp.a_extradata);
            free(mkv_tmp.cur_codec_priv);
        } else if (box_type == FOURCC('f','t','y','p') ||
                   box_type == FOURCC('m','o','o','v') ||
                   box_type == FOURCC('m','d','a','t') ||
                   box_type == FOURCC('w','i','d','e') ||
                   box_type == FOURCC('f','r','e','e')) {
            parse_mp4_boxes(fp, 0, meta->file_size, meta, NULL, NULL, NULL, 0, 0);
        }
    }
    fclose(fp);

    if (meta->fps_x100 <= 0) meta->fps_x100 = 3000;
    if (meta->width <= 0)    meta->width = 1920;
    if (meta->height <= 0)   meta->height = 1080;

    fill_fallback_meta_from_path(path, meta);

    if (thumb_rgb565 && thumb_w > 0 && thumb_h > 0) {
        int decoded_thumb = 0;
        /* Pass out_audio_sampr = 0 so thumbnail extraction does not touch audio state */
        y1vid_ctx *ctx = y1vid_open(path, thumb_w, thumb_h, Y1VID_SCALE_FIT, 0);
        if (ctx) {
            int64_t seek_target = ctx->meta.duration_ms > 10000 ? 5000 : (ctx->meta.duration_ms / 4);
            if (seek_target > 0)
                y1vid_seek(ctx, seek_target);
            int64_t pts = 0;
            for (int tries = 0; tries < 10; tries++) {
                if (y1vid_decode_video(ctx, thumb_rgb565, &pts, 0) == 1) {
                    decoded_thumb = 1;
                    break;
                }
            }
            y1vid_close(ctx);
        }
        if (!decoded_thumb) {
            generate_fallback_thumb(meta, thumb_rgb565, thumb_w, thumb_h);
        }
        meta->has_thumb = 1;
    }

    return 0;
}

y1vid_ctx* y1vid_open(const char *path, int out_w, int out_h, int scale_mode, int out_audio_sampr)
{
    if (!path) return NULL;
    init_builtin_h264();
    init_yuv_tables();

    y1vid_ctx *ctx = calloc(1, sizeof(y1vid_ctx));
    if (!ctx) return NULL;

    snprintf(ctx->path, sizeof(ctx->path), "%s", path);
    ctx->out_w = out_w > 0 ? out_w : 480;
    ctx->out_h = out_h > 0 ? out_h : 360;
    ctx->scale_mode = scale_mode;
    ctx->out_audio_sampr = out_audio_sampr;
    ctx->v_file_pos = UINT64_MAX;
    ctx->a_file_pos = UINT64_MAX;

    struct stat st;
    if (stat(path, &st) == 0)
        ctx->meta.file_size = (int64_t)st.st_size;

    ctx->fp = fopen(path, "rb");
    if (!ctx->fp) {
        free(ctx);
        return NULL;
    }
    ctx->v_io_buf = malloc(256 * 1024);
    if (ctx->v_io_buf) {
        setvbuf(ctx->fp, (char *)ctx->v_io_buf, _IOFBF, 256 * 1024);
    }

    uint8_t magic[12];
    size_t r = fread(magic, 1, sizeof(magic), ctx->fp);
    if (r >= 8) {
        uint32_t ebml_id = read_be32(magic);
        uint32_t box_type = read_be32(magic + 4);
        if (ebml_id == 0x1A45DFA3u) {
            ctx->is_mkv = 1;
            ctx->mkv.is_mkv = 1;
            ctx->mkv.timecode_scale = 1000000ULL;
            ctx->mkv.v_nalu_len_size = 4;
            char cur_tag[64] = {0};
            parse_mkv_elements(ctx->fp, 0, ctx->meta.file_size, &ctx->meta,
                               &ctx->mkv, cur_tag, sizeof(cur_tag), 0);
            ctx->mkv_cur_pos = ctx->mkv.first_cluster_pos;
        } else if (box_type == FOURCC('f','t','y','p') ||
                   box_type == FOURCC('m','o','o','v') ||
                   box_type == FOURCC('m','d','a','t') ||
                   box_type == FOURCC('w','i','d','e') ||
                   box_type == FOURCC('f','r','e','e')) {
            parse_mp4_boxes(ctx->fp, 0, ctx->meta.file_size, &ctx->meta,
                            NULL, &ctx->v_mp4, &ctx->a_mp4, 1, 0);
            if (ctx->v_mp4.valid)
                mp4_track_seek_sample(&ctx->v_mp4, 0);
            if (ctx->a_mp4.valid)
                mp4_track_seek_sample(&ctx->a_mp4, 0);
        }
    }
    ctx->v_file_pos = UINT64_MAX;

    /* Open dedicated second FILE* stream for MP4 audio so video and audio reads
     * never seek or flush each other's stdio buffer during playback */
    if (!ctx->is_mkv && ctx->a_mp4.valid && ctx->out_audio_sampr > 0) {
        ctx->a_fp = fopen(path, "rb");
        if (ctx->a_fp) {
            ctx->a_io_buf = malloc(64 * 1024);
            if (ctx->a_io_buf) {
                setvbuf(ctx->a_fp, (char *)ctx->a_io_buf, _IOFBF, 64 * 1024);
            }
        }
    }

    if (ctx->meta.fps_x100 <= 0) ctx->meta.fps_x100 = 3000;
    if (ctx->meta.width <= 0)    ctx->meta.width = 1920;
    if (ctx->meta.height <= 0)   ctx->meta.height = 1080;
    fill_fallback_meta_from_path(path, &ctx->meta);

    compute_scaling_rect(ctx, ctx->meta.width, ctx->meta.height,
                         ctx->out_w, ctx->out_h, ctx->scale_mode,
                         &ctx->dst_x, &ctx->dst_y, &ctx->scaled_w, &ctx->scaled_h);

    /* Initialize Built-in FFmpeg H.264 Decoder */
    void *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        y1vid_close(ctx);
        return NULL;
    }
    ctx->v_ctx = avcodec_alloc_context3(codec);
    if (!ctx->v_ctx) {
        y1vid_close(ctx);
        return NULL;
    }

    /* Configure AVCodecContext offsets verified from h264_objs:
     * +0x5c: flags (|= 0x8000 CODEC_FLAG_TRUNCATED)
     * +0x60: flags2 (|= 0x0001 CODEC_FLAG2_FAST)
     * +0x64: extradata (uint8_t*)
     * +0x68: extradata_size (int)
     * +0x320: thread_count = 1
     * +0x344: skip_loop_filter (enum AVDiscard: 48 = AVDISCARD_ALL)
     * +0x348: skip_idct (enum AVDiscard: 8 = AVDISCARD_NONREF)
     * +0x34c: skip_frame (enum AVDiscard: 8 = AVDISCARD_NONREF) */
    uint8_t *vctx_bytes = (uint8_t *)ctx->v_ctx;
    *(int *)(vctx_bytes + 0x5c) |= 0x00008000;
    *(int *)(vctx_bytes + 0x60) |= 0x00000001;
    *(int *)(vctx_bytes + 0x320) = 1;
    *(int *)(vctx_bytes + 0x344) = AVDISCARD_ALL;
    *(int *)(vctx_bytes + 0x348) = AVDISCARD_NONREF;

    const uint8_t *v_ext = ctx->is_mkv ? ctx->mkv.v_extradata : ctx->v_mp4.extradata;
    int v_ext_len        = ctx->is_mkv ? ctx->mkv.v_extradata_len : ctx->v_mp4.extradata_len;
    if (v_ext && v_ext_len > 0) {
        uint8_t *ext_copy = av_mallocz((size_t)v_ext_len + 32);
        if (ext_copy) {
            memcpy(ext_copy, v_ext, (size_t)v_ext_len);
            *(uint8_t **)(vctx_bytes + 0x64) = ext_copy;
            *(int *)(vctx_bytes + 0x68)      = v_ext_len;
        }
    }

    /* Skip in-loop deblocking filter and non-ref IDCT for 360p LCD downscaling on MT6572
     * (cuts Cortex-A7 H.264 decode time by ~35-40% with no visible loss at 480x360) */
    av_opt_set_int(ctx->v_ctx, "skip_loop_filter", AVDISCARD_ALL, 0);
    av_opt_set_int(ctx->v_ctx, "skip_idct", AVDISCARD_NONREF, 0);

    if (avcodec_open2(ctx->v_ctx, codec, NULL) < 0) {
        av_free(ctx->v_ctx);
        ctx->v_ctx = NULL;
        y1vid_close(ctx);
        return NULL;
    }

    ctx->v_frame = av_frame_alloc();
    if (!ctx->v_frame) {
        y1vid_close(ctx);
        return NULL;
    }

    ctx->raw_buf_cap = 256 * 1024;
    ctx->raw_buf = malloc(ctx->raw_buf_cap);
    ctx->pkt_buf_cap = 256 * 1024;
    ctx->pkt_buf = malloc(ctx->pkt_buf_cap);
    ctx->need_sps_pps = 1;

    /* Feed SPS/PPS extradata packet once immediately after open */
    if (v_ext && v_ext_len > 0 && ctx->pkt_buf) {
        memcpy(ctx->pkt_buf, v_ext, (size_t)v_ext_len);
        memset(ctx->pkt_buf + v_ext_len, 0, 32);
        uint8_t avpkt[96];
        memset(avpkt, 0, sizeof(avpkt));
        av_init_packet(avpkt);
        *(uint8_t **)(avpkt + 0x18) = ctx->pkt_buf;
        *(int *)(avpkt + 0x1c)      = v_ext_len;
        int got_dummy = 0;
        avcodec_decode_video2(ctx->v_ctx, ctx->v_frame, &got_dummy, avpkt);
    }

    /* Initialize Built-in FAAD2 AAC Audio Decoder when audio output is requested */
    if (ctx->out_audio_sampr > 0) {
        ctx->audio_ring = calloc(AUDIO_RING_SAMPLES * 2, sizeof(int16_t));
        const uint8_t *a_ext = ctx->is_mkv ? ctx->mkv.a_extradata : ctx->a_mp4.extradata;
        int a_ext_len        = ctx->is_mkv ? ctx->mkv.a_extradata_len : ctx->a_mp4.extradata_len;
        uint8_t fallback_asc[2] = { 0x12, 0x10 }; /* Default AAC-LC 44100Hz Stereo */
        if ((!a_ext || a_ext_len < 2) &&
            (ctx->is_mkv ? (ctx->mkv.a_track_num > 0) : (ctx->a_mp4.valid && ctx->a_mp4.fourcc == FOURCC('m','p','4','a')))) {
            static const int aac_sr_table[13] = {
                96000, 88200, 64000, 48000, 44100, 32000,
                24000, 22050, 16000, 12000, 11025, 8000, 7350
            };
            int sr_idx = 4; /* 44100 */
            int sr_val = ctx->meta.audio_samplerate > 0 ? ctx->meta.audio_samplerate : 44100;
            for (int i = 0; i < 13; i++) {
                if (aac_sr_table[i] == sr_val) { sr_idx = i; break; }
            }
            int ch_cfg = (ctx->meta.audio_channels >= 1 && ctx->meta.audio_channels <= 2) ? ctx->meta.audio_channels : 2;
            fallback_asc[0] = (uint8_t)((2 << 3) | ((sr_idx >> 1) & 0x07));
            fallback_asc[1] = (uint8_t)(((sr_idx & 0x01) << 7) | ((ch_cfg & 0x0F) << 3));
            a_ext = fallback_asc;
            a_ext_len = 2;
        }
        if (a_ext && a_ext_len >= 2) {
            ctx->aac_dec = NeAACDecOpen();
            if (ctx->aac_dec) {
                NeAACDecConfigurationPtr conf = NeAACDecGetCurrentConfiguration(ctx->aac_dec);
                if (conf) {
                    conf->outputFormat = 2; /* FAAD_FMT_24BIT */
                    NeAACDecSetConfiguration(ctx->aac_dec, conf);
                }
                uint32_t sr = 0;
                uint8_t ch = 0;
                if (NeAACDecInit2(ctx->aac_dec, (uint8_t *)a_ext, (uint32_t)a_ext_len, &sr, &ch) == 0 && sr > 0) {
                    ctx->aac_samplerate = sr;
                    ctx->aac_channels = ch;
                    ctx->meta.audio_samplerate = (int)sr;
                    ctx->meta.audio_channels = (int)ch;
                } else {
                    ctx->aac_dec = NULL;
                }
            }
        }
        /* Pre-buffer initial ~1.0s of audio before playback starts */
        if (!ctx->is_mkv && ctx->a_mp4.valid) {
            pump_mp4_audio(ctx, 64);
        }
    }

    return ctx;
}

void y1vid_set_output_format(y1vid_ctx *ctx, int out_w, int out_h, int scale_mode)
{
    if (!ctx) return;
    ctx->out_w = out_w > 0 ? out_w : 480;
    ctx->out_h = out_h > 0 ? out_h : 360;
    ctx->scale_mode = scale_mode;

    compute_scaling_rect(ctx, ctx->meta.width, ctx->meta.height,
                         ctx->out_w, ctx->out_h, ctx->scale_mode,
                         &ctx->dst_x, &ctx->dst_y, &ctx->scaled_w, &ctx->scaled_h);
}

/* drop_nonref modes:
 *   0 = decode next frame (AVDISCARD_DEFAULT) + blit YUV420P->RGB565 to rgb565_buf
 *   1 = decode next frame (AVDISCARD_NONREF)  + blit YUV420P->RGB565 to rgb565_buf
 *   2 = decode next frame (AVDISCARD_DEFAULT) WITHOUT blit (leaves frame in ctx->v_frame)
 *   3 = decode next frame (AVDISCARD_NONREF)  WITHOUT blit (leaves frame in ctx->v_frame)
 *   4 = blit already-decoded ctx->v_frame to rgb565_buf without decoding a new packet */
int y1vid_decode_video(y1vid_ctx *ctx, uint16_t *rgb565_buf, int64_t *pts_ms, int drop_nonref)
{
    if (!ctx || !rgb565_buf || !ctx->v_ctx || !ctx->v_frame || !ctx->fp)
        return -1;

    if (drop_nonref == 4) {
        if (!ctx->has_decoded_frame)
            return -1;
        if (pts_ms) *pts_ms = ctx->cur_pos_ms;
        blit_decoded_avframe(ctx, ctx->v_frame, rgb565_buf);
        return 1;
    }

    int skip_nonref = (drop_nonref == 1 || drop_nonref == 3) ? 1 : 0;
    int do_blit     = (drop_nonref == 2 || drop_nonref == 3) ? 0 : 1;

    uint8_t *vctx_bytes = (uint8_t *)ctx->v_ctx;
    *(int *)(vctx_bytes + 0x344) = AVDISCARD_ALL;
    *(int *)(vctx_bytes + 0x348) = AVDISCARD_NONREF;
    *(int *)(vctx_bytes + 0x34c) = skip_nonref ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;

    av_opt_set_int(ctx->v_ctx, "skip_frame",
                   skip_nonref ? AVDISCARD_NONREF : AVDISCARD_DEFAULT, 0);

    for (int tries = 0; tries < 96; tries++) {
        int pkt_size = 0;
        int64_t pkt_dts_ms = ctx->cur_pos_ms;
        bool is_kf = false;
        int have_pkt = 0;

        if (ctx->is_mkv) {
            have_pkt = read_next_mkv_video_packet(ctx, &pkt_size, &pkt_dts_ms, &is_kf);
        } else if (ctx->v_mp4.valid) {
            uint64_t s_off = 0;
            uint32_t s_sz  = 0;
            if (mp4_track_next_sample(&ctx->v_mp4, &s_off, &s_sz, &pkt_dts_ms, &is_kf)) {
                if (s_sz > 0 && s_sz < MAX_PACKET_SIZE) {
                    if (s_sz > ctx->raw_buf_cap) {
                        uint8_t *nb = realloc(ctx->raw_buf, s_sz);
                        if (nb) {
                            ctx->raw_buf = nb;
                            ctx->raw_buf_cap = s_sz;
                        }
                    }
                    if (ctx->raw_buf && s_sz <= ctx->raw_buf_cap) {
                        bool seek_ok = true;
                        if (ctx->v_file_pos != s_off) {
                            if (fseeko(ctx->fp, (off_t)s_off, SEEK_SET) != 0) {
                                ctx->v_file_pos = UINT64_MAX;
                                seek_ok = false;
                            }
                        }
                        if (seek_ok && fread(ctx->raw_buf, 1, s_sz, ctx->fp) == s_sz) {
                            ctx->v_file_pos = s_off + s_sz;
                            if (prepare_h264_annexb_packet(
                                    ctx, ctx->raw_buf, s_sz, ctx->v_mp4.nalu_len_size,
                                    ctx->v_mp4.extradata, ctx->v_mp4.extradata_len,
                                    is_kf || ctx->need_sps_pps, &pkt_size) == 0) {
                                ctx->need_sps_pps = 0;
                                have_pkt = 1;
                            }
                        } else {
                            ctx->v_file_pos = UINT64_MAX;
                        }
                    }
                }
            }
        } else {
            return -1;
        }

        if (!have_pkt) {
            /* Flush any delayed B-frames remaining in the H.264 reorder buffer */
            uint8_t avpkt[96];
            memset(avpkt, 0, sizeof(avpkt));
            av_init_packet(avpkt);
            *(uint8_t **)(avpkt + 0x18) = NULL;
            *(int *)(avpkt + 0x1c)      = 0;
            int got_frame = 0;
            if (avcodec_decode_video2(ctx->v_ctx, ctx->v_frame, &got_frame, avpkt) >= 0 && got_frame) {
                int step_ms = ctx->meta.fps_x100 > 0 ? (100000 / ctx->meta.fps_x100) : 33;
                ctx->cur_pos_ms += step_ms;
                ctx->has_decoded_frame = 1;
                if (pts_ms) *pts_ms = ctx->cur_pos_ms;
                if (do_blit) {
                    blit_decoded_avframe(ctx, ctx->v_frame, rgb565_buf);
                }
                if (!ctx->is_mkv && ctx->a_mp4.valid)
                    pump_mp4_audio(ctx, 16);
                return 1;
            }
            ctx->eof_reached = 1;
            return 0;
        }

        if (!ctx->is_mkv && ctx->a_mp4.valid) {
            pump_mp4_audio(ctx, 16);
        }

        uint8_t avpkt[96];
        memset(avpkt, 0, sizeof(avpkt));
        av_init_packet(avpkt);
        *(uint8_t **)(avpkt + 0x18) = ctx->pkt_buf;
        *(int *)(avpkt + 0x1c)      = pkt_size;

        int got_frame = 0;
        int ret = avcodec_decode_video2(ctx->v_ctx, ctx->v_frame, &got_frame, avpkt);
        if (ret >= 0 && !got_frame && !ctx->first_frame_out && !skip_nonref) {
            if (ctx->reorder_packets < 3)
                ctx->reorder_packets++;
        }
        if (ret >= 0 && got_frame) {
            if (!ctx->first_frame_out) {
                int step_ms = ctx->meta.fps_x100 > 0 ? (100000 / ctx->meta.fps_x100) : 33;
                ctx->reorder_delay_ms = (int64_t)ctx->reorder_packets * step_ms;
                ctx->first_frame_out = 1;
            }
            int64_t adj_pts = pkt_dts_ms - ctx->reorder_delay_ms;
            if (adj_pts < 0) adj_pts = 0;
            ctx->cur_pos_ms = adj_pts;
            ctx->has_decoded_frame = 1;
            if (pts_ms) *pts_ms = ctx->cur_pos_ms;
            if (do_blit) {
                blit_decoded_avframe(ctx, ctx->v_frame, rgb565_buf);
            }
            return 1;
        }
    }

    return -1;
}

int y1vid_decode_audio(y1vid_ctx *ctx, int16_t *pcm_stereo_out, int max_samples_per_ch, int64_t *pts_ms)
{
    if (!ctx || !pcm_stereo_out || max_samples_per_ch <= 0) return 0;

    if (ctx->audio_ring) {
        uint32_t rpos = ctx->audio_ring_rpos;
        uint32_t wpos = ctx->audio_ring_wpos;
        uint32_t avail = wpos - rpos;
        if (avail > 0) {
            int out_sr = ctx->out_audio_sampr > 0 ? ctx->out_audio_sampr : 44100;
            int64_t rpos_pts = ctx->audio_wpos_pts_ms - ((int64_t)avail * 1000LL) / out_sr;
            if (rpos_pts < 0) rpos_pts = 0;
            ctx->audio_pts_ms = rpos_pts;
            if (pts_ms) *pts_ms = rpos_pts;

            int take = (avail < (uint32_t)max_samples_per_ch) ? (int)avail : max_samples_per_ch;
            for (int i = 0; i < take; i++) {
                uint32_t slot = ((rpos + (uint32_t)i) % AUDIO_RING_SAMPLES) * 2;
                pcm_stereo_out[i * 2]     = ctx->audio_ring[slot];
                pcm_stereo_out[i * 2 + 1] = ctx->audio_ring[slot + 1];
            }
            ctx->audio_ring_rpos = rpos + (uint32_t)take;
            return take;
        }
    }

    if (pts_ms) *pts_ms = -1;
    memset(pcm_stereo_out, 0, (size_t)max_samples_per_ch * 2 * sizeof(int16_t));
    return 0;
}

int y1vid_seek(y1vid_ctx *ctx, int64_t target_ms)
{
    if (!ctx) return -1;
    if (target_ms < 0) target_ms = 0;
    if (ctx->meta.duration_ms > 0 && target_ms > ctx->meta.duration_ms)
        target_ms = ctx->meta.duration_ms;

    ctx->cur_pos_ms = target_ms;
    ctx->audio_pts_ms = target_ms;
    ctx->audio_wpos_pts_ms = target_ms;
    ctx->eof_reached = 0;
    ctx->audio_ring_rpos = 0;
    ctx->audio_ring_wpos = 0;
    ctx->resample_pos_fp = 0;
    ctx->need_sps_pps = 1;
    ctx->has_decoded_frame = 0;
    ctx->reorder_packets = 0;
    ctx->first_frame_out = 0;
    ctx->reorder_delay_ms = 0;
    ctx->v_file_pos = UINT64_MAX;
    ctx->a_file_pos = UINT64_MAX;

    if (ctx->v_ctx) {
        avcodec_flush_buffers(ctx->v_ctx);
    }

    if (ctx->is_mkv) {
        if (target_ms <= 0 || ctx->meta.duration_ms <= 0) {
            ctx->mkv_cur_pos = ctx->mkv.first_cluster_pos;
        } else {
            /* Scan forward from proportional offset for next MKV Cluster (0x1F 0x43 0xB6 0x75) */
            int64_t data_len = ctx->meta.file_size - ctx->mkv.first_cluster_pos;
            int64_t approx = ctx->mkv.first_cluster_pos + (data_len * target_ms) / ctx->meta.duration_ms;
            if (approx < ctx->mkv.first_cluster_pos) approx = ctx->mkv.first_cluster_pos;
            uint8_t scan[4096];
            int found = 0;
            for (int blk = 0; blk < 64 && approx < ctx->meta.file_size - 4; blk++) {
                if (fseeko(ctx->fp, approx, SEEK_SET) != 0) break;
                size_t r = fread(scan, 1, sizeof(scan), ctx->fp);
                if (r < 4) break;
                for (size_t i = 0; i + 4 <= r; i++) {
                    if (scan[i] == 0x1F && scan[i + 1] == 0x43 &&
                        scan[i + 2] == 0xB6 && scan[i + 3] == 0x75) {
                        ctx->mkv_cur_pos = approx + (int64_t)i;
                        found = 1;
                        break;
                    }
                }
                if (found) break;
                approx += (int64_t)(r - 3);
            }
            if (!found) ctx->mkv_cur_pos = ctx->mkv.first_cluster_pos;
        }
        if (ctx->aac_dec) {
            NeAACDecPostSeekReset(ctx->aac_dec, -1);
        }
    } else {
        if (ctx->v_mp4.valid) {
            uint32_t target_sample = mp4_track_ms_to_sample(&ctx->v_mp4, target_ms);
            /* Snap to nearest preceding keyframe in stss */
            if (ctx->v_mp4.stss && ctx->v_mp4.num_stss > 0) {
                uint32_t kf_sample = 0;
                for (uint32_t i = 0; i < ctx->v_mp4.num_stss; i++) {
                    uint32_t s0 = ctx->v_mp4.stss[i] > 0 ? (ctx->v_mp4.stss[i] - 1) : 0;
                    if (s0 <= target_sample) kf_sample = s0;
                    else break;
                }
                target_sample = kf_sample;
            }
            mp4_track_seek_sample(&ctx->v_mp4, target_sample);
            if (ctx->v_mp4.timescale > 0) {
                ctx->cur_pos_ms = (int64_t)((ctx->v_mp4.cur_dts_ticks * 1000ULL) / ctx->v_mp4.timescale);
            }
        }
        if (ctx->a_mp4.valid) {
            ctx->audio_pts_ms = ctx->cur_pos_ms;
            ctx->audio_wpos_pts_ms = ctx->cur_pos_ms;
            uint32_t a_sample = mp4_track_ms_to_sample(&ctx->a_mp4, ctx->cur_pos_ms);
            mp4_track_seek_sample(&ctx->a_mp4, a_sample);
            if (ctx->aac_dec) {
                NeAACDecPostSeekReset(ctx->aac_dec, (int32_t)a_sample);
            }
            if (ctx->out_audio_sampr > 0) {
                pump_mp4_audio(ctx, 64);
            }
        }
    }

    return 0;
}

int64_t y1vid_get_duration_ms(y1vid_ctx *ctx)
{
    return ctx ? ctx->meta.duration_ms : 0;
}

int64_t y1vid_get_position_ms(y1vid_ctx *ctx)
{
    return ctx ? ctx->cur_pos_ms : 0;
}

void y1vid_get_meta(y1vid_ctx *ctx, struct y1vid_meta *meta_out)
{
    if (!ctx || !meta_out) return;
    *meta_out = ctx->meta;
}

void y1vid_close(y1vid_ctx *ctx)
{
    if (!ctx) return;

    if (ctx->v_ctx) {
        uint8_t *vctx_bytes = (uint8_t *)ctx->v_ctx;
        uint8_t *ext = *(uint8_t **)(vctx_bytes + 0x64);
        if (ext) {
            av_free(ext);
            *(uint8_t **)(vctx_bytes + 0x64) = NULL;
            *(int *)(vctx_bytes + 0x68)      = 0;
        }
        avcodec_close(ctx->v_ctx);
        av_free(ctx->v_ctx);
        ctx->v_ctx = NULL;
    }
    if (ctx->v_frame) {
        av_frame_free(&ctx->v_frame);
        ctx->v_frame = NULL;
    }

    free_mp4_track_table(&ctx->v_mp4);
    free_mp4_track_table(&ctx->a_mp4);
    free(ctx->mkv.v_extradata);
    free(ctx->mkv.a_extradata);
    free(ctx->mkv.cur_codec_priv);
    free(ctx->raw_buf);
    free(ctx->pkt_buf);
    free(ctx->audio_ring);

    if (ctx->a_fp) {
        fclose(ctx->a_fp);
        ctx->a_fp = NULL;
    }
    if (ctx->fp) {
        fclose(ctx->fp);
        ctx->fp = NULL;
    }
    free(ctx->a_io_buf);
    free(ctx->v_io_buf);
    free(ctx);
}
