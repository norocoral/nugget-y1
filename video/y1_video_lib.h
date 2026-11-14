/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * External SD Card Video Library ABI for Innioasis Y1
 * Loaded dynamically from /mnt/sdcard/.rockbox/lib/liby1video.so
 ****************************************************************************/
#ifndef Y1_VIDEO_LIB_H
#define Y1_VIDEO_LIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Y1VID_ABI_VERSION 1

/* Aspect / Scaling Modes */
#define Y1VID_SCALE_FIT     0   /* Letterbox / Pillarbox preserving aspect ratio */
#define Y1VID_SCALE_FILL    1   /* Crop to fill screen preserving aspect ratio */
#define Y1VID_SCALE_STRETCH 2   /* Stretch to full screen dimensions */

/* Thumbnail dimensions stored in database cache */
#define Y1VID_THUMB_W       96
#define Y1VID_THUMB_H       72

struct y1vid_meta {
    char title[128];
    char tv_show[128];
    char creator[128];
    int  year;
    int  season;
    int  episode;
    int  width;
    int  height;
    int  fps_x100;          /* e.g. 2997 for 29.97 fps, 3000 for 30.00 fps */
    int  audio_channels;
    int  audio_samplerate;
    int64_t duration_ms;    /* 64-bit millisecond duration (supports >2 hour videos) */
    int64_t file_size;      /* 64-bit file size in bytes */
    int  has_video;
    int  has_audio;
    int  has_thumb;
    char codec_name[32];
};

typedef struct y1vid_context y1vid_ctx;

int        y1vid_get_abi_version(void);
int        y1vid_probe_metadata(const char *path, struct y1vid_meta *meta,
                                uint16_t *thumb_rgb565, int thumb_w, int thumb_h);
y1vid_ctx* y1vid_open(const char *path, int out_w, int out_h,
                      int scale_mode, int out_audio_sampr);
void       y1vid_set_output_format(y1vid_ctx *ctx, int out_w, int out_h, int scale_mode);
int        y1vid_decode_video(y1vid_ctx *ctx, uint16_t *rgb565_buf,
                              int64_t *pts_ms, int drop_nonref);
int        y1vid_decode_audio(y1vid_ctx *ctx, int16_t *pcm_stereo_out,
                              int max_samples_per_ch, int64_t *pts_ms);
int        y1vid_seek(y1vid_ctx *ctx, int64_t target_ms);
int64_t    y1vid_get_duration_ms(y1vid_ctx *ctx);
int64_t    y1vid_get_position_ms(y1vid_ctx *ctx);
void       y1vid_get_meta(y1vid_ctx *ctx, struct y1vid_meta *meta_out);
void       y1vid_close(y1vid_ctx *ctx);

typedef int        (*fn_y1vid_get_abi_version)(void);
typedef int        (*fn_y1vid_probe_metadata)(const char *path, struct y1vid_meta *meta,
                                              uint16_t *thumb_rgb565, int thumb_w, int thumb_h);
typedef y1vid_ctx* (*fn_y1vid_open)(const char *path, int out_w, int out_h,
                                    int scale_mode, int out_audio_sampr);
typedef void       (*fn_y1vid_set_output_format)(y1vid_ctx *ctx, int out_w, int out_h, int scale_mode);
typedef int        (*fn_y1vid_decode_video)(y1vid_ctx *ctx, uint16_t *rgb565_buf,
                                            int64_t *pts_ms, int drop_nonref);
typedef int        (*fn_y1vid_decode_audio)(y1vid_ctx *ctx, int16_t *pcm_stereo_out,
                                            int max_samples_per_ch, int64_t *pts_ms);
typedef int        (*fn_y1vid_seek)(y1vid_ctx *ctx, int64_t target_ms);
typedef int64_t    (*fn_y1vid_get_duration_ms)(y1vid_ctx *ctx);
typedef int64_t    (*fn_y1vid_get_position_ms)(y1vid_ctx *ctx);
typedef void       (*fn_y1vid_get_meta)(y1vid_ctx *ctx, struct y1vid_meta *meta_out);
typedef void       (*fn_y1vid_close)(y1vid_ctx *ctx);

struct y1vid_api {
    fn_y1vid_get_abi_version   get_abi_version;
    fn_y1vid_probe_metadata    probe_metadata;
    fn_y1vid_open              open;
    fn_y1vid_set_output_format set_output_format;
    fn_y1vid_decode_video      decode_video;
    fn_y1vid_decode_audio      decode_audio;
    fn_y1vid_seek              seek;
    fn_y1vid_get_duration_ms   get_duration_ms;
    fn_y1vid_get_position_ms   get_position_ms;
    fn_y1vid_get_meta          get_meta;
    fn_y1vid_close             close;
};

#ifdef __cplusplus
}
#endif

#endif /* Y1_VIDEO_LIB_H */

