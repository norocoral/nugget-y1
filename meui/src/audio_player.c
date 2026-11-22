#include "audio_player.h"
#include "liby1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_thread;
static bool            g_running = false;
static const struct ipod_db *g_db = NULL;

static int             *g_queue = NULL;
static int             *g_orig_queue = NULL;
static int              g_queue_len = 0;
static int              g_queue_pos = 0;
static bool             g_playing = false;
static bool             g_reload_req = false;
static bool             g_stop_req = false;
static bool             g_queue_dirty = false;
static uint32_t         g_start_pos_ms = 0;
static int              g_seek_req_ms = 0;
static bool             g_has_seek_req = false;
static int              g_volume_pct = 70;
static bool             g_shuffle = false;
static enum repeat_mode g_repeat = REPEAT_ALL;
static uint32_t         g_pos_ms = 0;
static uint32_t         g_dur_ms = 0;

struct ff_track {
    AVFormatContext *fmt;
    AVCodecContext  *ctx;
    SwrContext      *swr;
    AVPacket        *pkt;
    AVFrame         *frm;
    int              stream_idx;
    uint32_t         sample_rate;
    uint32_t         dur_ms;
    uint64_t         frames_out;
    bool             eof;
    bool             flushed_demux;
    int16_t         *pcm;
    size_t           pcm_cap;
    size_t           pcm_rd;
    size_t           pcm_len;
};

static void ff_track_close(struct ff_track *t)
{
    if (!t) return;
    if (t->pkt) av_packet_free(&t->pkt);
    if (t->frm) av_frame_free(&t->frm);
    if (t->swr) swr_free(&t->swr);
    if (t->ctx) avcodec_free_context(&t->ctx);
    if (t->fmt) avformat_close_input(&t->fmt);
    free(t->pcm);
    memset(t, 0, sizeof(*t));
}

static bool ff_track_open(struct ff_track *t, const char *path)
{
    memset(t, 0, sizeof(*t));
    if (!path || !*path) return false;

    if (avformat_open_input(&t->fmt, path, NULL, NULL) < 0)
        return false;
    if (avformat_find_stream_info(t->fmt, NULL) < 0) {
        ff_track_close(t);
        return false;
    }

    int si = av_find_best_stream(t->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (si < 0) {
        ff_track_close(t);
        return false;
    }
    t->stream_idx = si;
    AVStream *st = t->fmt->streams[si];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        ff_track_close(t);
        return false;
    }

    t->ctx = avcodec_alloc_context3(dec);
    if (!t->ctx || avcodec_parameters_to_context(t->ctx, st->codecpar) < 0 ||
        avcodec_open2(t->ctx, dec, NULL) < 0) {
        ff_track_close(t);
        return false;
    }

    t->sample_rate = t->ctx->sample_rate > 0 ? (uint32_t)t->ctx->sample_rate : 44100;
    if (st->duration != AV_NOPTS_VALUE && st->time_base.den > 0) {
        int64_t ms = av_rescale_q(st->duration, st->time_base, (AVRational){1, 1000});
        t->dur_ms = ms > 0 ? (uint32_t)ms : 0;
    } else if (t->fmt->duration != AV_NOPTS_VALUE && t->fmt->duration > 0) {
        t->dur_ms = (uint32_t)(t->fmt->duration / 1000);
    }

    AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_ch = t->ctx->ch_layout;
    if (in_ch.nb_channels <= 0)
        av_channel_layout_default(&in_ch, 2);

    if (swr_alloc_set_opts2(&t->swr,
                            &out_ch, AV_SAMPLE_FMT_S16, (int)t->sample_rate,
                            &in_ch, t->ctx->sample_fmt, (int)t->sample_rate,
                            0, NULL) < 0 ||
        swr_init(t->swr) < 0) {
        ff_track_close(t);
        return false;
    }

    t->pkt = av_packet_alloc();
    t->frm = av_frame_alloc();
    t->pcm_cap = 16384;
    t->pcm = malloc(t->pcm_cap * 2 * sizeof(int16_t));
    if (!t->pkt || !t->frm || !t->pcm) {
        ff_track_close(t);
        return false;
    }
    return true;
}

static bool ff_track_append_pcm(struct ff_track *t, AVFrame *frm)
{
    int out_samples = swr_get_out_samples(t->swr, frm->nb_samples);
    if (out_samples <= 0) return true;

    if (t->pcm_rd > 0) {
        size_t rem = t->pcm_len - t->pcm_rd;
        if (rem > 0)
            memmove(t->pcm, t->pcm + t->pcm_rd * 2, rem * 2 * sizeof(int16_t));
        t->pcm_len = rem;
        t->pcm_rd = 0;
    }

    if (t->pcm_len + (size_t)out_samples > t->pcm_cap) {
        size_t nc = t->pcm_cap * 2;
        while (nc < t->pcm_len + (size_t)out_samples) nc *= 2;
        int16_t *np = realloc(t->pcm, nc * 2 * sizeof(int16_t));
        if (!np) return false;
        t->pcm = np;
        t->pcm_cap = nc;
    }

    uint8_t *dst[1] = { (uint8_t *)(t->pcm + t->pcm_len * 2) };
    int conv = swr_convert(t->swr, dst, out_samples,
                           (const uint8_t **)frm->extended_data, frm->nb_samples);
    if (conv > 0)
        t->pcm_len += (size_t)conv;
    return true;
}

static size_t ff_track_read_frames(struct ff_track *t, int16_t *out_stereo, size_t want_frames)
{
    while ((t->pcm_len - t->pcm_rd) < want_frames && !t->eof) {
        int rc = avcodec_receive_frame(t->ctx, t->frm);
        if (rc == 0) {
            ff_track_append_pcm(t, t->frm);
            av_frame_unref(t->frm);
            continue;
        }
        if (rc == AVERROR_EOF) {
            t->eof = true;
            break;
        }
        if (rc != AVERROR(EAGAIN)) {
            t->eof = true;
            break;
        }

        if (t->flushed_demux) {
            t->eof = true;
            break;
        }

        int r = av_read_frame(t->fmt, t->pkt);
        if (r < 0) {
            avcodec_send_packet(t->ctx, NULL);
            t->flushed_demux = true;
        } else {
            if (t->pkt->stream_index == t->stream_idx)
                avcodec_send_packet(t->ctx, t->pkt);
            av_packet_unref(t->pkt);
        }
    }

    size_t avail = t->pcm_len - t->pcm_rd;
    size_t got = avail < want_frames ? avail : want_frames;
    if (got > 0) {
        memcpy(out_stereo, t->pcm + t->pcm_rd * 2, got * 2 * sizeof(int16_t));
        t->pcm_rd += got;
        t->frames_out += got;
    }
    return got;
}

static void ff_track_seek_ms(struct ff_track *t, uint32_t target_ms)
{
    if (!t || !t->fmt) return;
    AVStream *st = t->fmt->streams[t->stream_idx];
    int64_t ts = av_rescale_q((int64_t)target_ms, (AVRational){1, 1000}, st->time_base);
    av_seek_frame(t->fmt, t->stream_idx, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(t->ctx);
    swr_init(t->swr);
    t->pcm_rd = 0;
    t->pcm_len = 0;
    t->eof = false;
    t->flushed_demux = false;
    t->frames_out = ((uint64_t)target_ms * (uint64_t)t->sample_rate) / 1000ULL;
}

static uint32_t ff_track_cur_ms(const struct ff_track *t)
{
    if (!t || t->sample_rate == 0) return 0;
    return (uint32_t)((t->frames_out * 1000ULL) / t->sample_rate);
}

static void apply_hw_volume(int vol_pct)
{
    if (vol_pct < 0) vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;
    int gain = (vol_pct * 7) / 100;
    y1_audio_set_hp_analog_gain(gain, gain);
}

static int peek_next_pos_locked(void)
{
    if (!g_queue || g_queue_len <= 0) return -1;
    if (g_repeat == REPEAT_ONE) return g_queue_pos;
    if (g_queue_pos + 1 < g_queue_len) return g_queue_pos + 1;
    if (g_repeat == REPEAT_ALL) return 0;
    return -1;
}

static void scale_stereo_pcm(int16_t *buf, size_t frames, int scale_q12)
{
    size_t n = frames * 2;
    for (size_t i = 0; i < n; i++) {
        int32_t s = ((int32_t)buf[i] * scale_q12) >> 12;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}

#define PCM_OUT_FRAMES 32768 /* Stereo int16 sample pairs (~743ms @ 44.1kHz) */
#define PCM_OUT_MASK   (PCM_OUT_FRAMES - 1)

static pthread_mutex_t g_out_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_out_thread;
static int16_t         g_out_ring[PCM_OUT_FRAMES * 2];
static size_t          g_out_rd = 0;
static size_t          g_out_wr = 0;
static uint32_t        g_out_sr = 44100;
static bool            g_out_playing = false;
static int             g_out_vol = 70;

static void out_ring_flush(bool playing, uint32_t sr, int vol)
{
    pthread_mutex_lock(&g_out_mtx);
    g_out_rd = 0;
    g_out_wr = 0;
    g_out_playing = playing;
    if (sr > 0) g_out_sr = sr;
    g_out_vol = vol;
    pthread_mutex_unlock(&g_out_mtx);
}

static size_t out_ring_used(void)
{
    pthread_mutex_lock(&g_out_mtx);
    size_t u = (g_out_wr - g_out_rd) & PCM_OUT_MASK;
    pthread_mutex_unlock(&g_out_mtx);
    return u;
}

static void out_ring_push(const int16_t *pcm, size_t frames, uint32_t sr, int vol)
{
    if (!pcm || frames == 0) return;
    pthread_mutex_lock(&g_out_mtx);
    if (sr > 0) g_out_sr = sr;
    g_out_vol = vol;
    g_out_playing = true;
    size_t w = g_out_wr;
    size_t r = g_out_rd;
    for (size_t i = 0; i < frames; i++) {
        if (((w - r) & PCM_OUT_MASK) >= PCM_OUT_FRAMES - 2)
            break;
        g_out_ring[w * 2 + 0] = pcm[i * 2 + 0];
        g_out_ring[w * 2 + 1] = pcm[i * 2 + 1];
        w = (w + 1) & PCM_OUT_MASK;
    }
    g_out_wr = w;
    pthread_mutex_unlock(&g_out_mtx);
}

static uint64_t g_bt_pace_ns = 0;

static uint64_t ap_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static bool bt_wait_abort(void)
{
    bool abort_wait = false;
    pthread_mutex_lock(&g_mtx);
    if (!g_playing || g_reload_req || g_stop_req || g_has_seek_req)
        abort_wait = true;
    pthread_mutex_unlock(&g_mtx);
    return abort_wait;
}

static void pace_bt_a2dp_write(const int16_t *pcm, size_t frames, uint32_t sr)
{
    if (!pcm || frames == 0) return;
    uint32_t rate = (sr > 0) ? sr : 44100U;
    uint64_t chunk_ns = ((uint64_t)frames * 1000000000ULL) / (uint64_t)rate;
    uint64_t now = ap_now_ns();
    if (g_bt_pace_ns == 0) {
        g_bt_pace_ns = (now > 160000000ULL) ? (now - 160000000ULL) : 0;
    } else if (now > g_bt_pace_ns + 200000000ULL) {
        g_bt_pace_ns = now;
    }

    while (g_running && y1_bt_a2dp_is_streaming()) {
        now = ap_now_ns();
        if (now >= g_bt_pace_ns)
            break;
        if (bt_wait_abort()) return;
        uint64_t rem_us = (g_bt_pace_ns - now) / 1000ULL;
        if (rem_us > 20000ULL) rem_us = 20000ULL;
        if (rem_us < 1000ULL) rem_us = 1000ULL;
        usleep((useconds_t)rem_us);
    }

    while (g_running && y1_bt_a2dp_is_streaming()) {
        if (y1_bt_a2dp_get_ring_free() >= frames) {
            y1_bt_a2dp_write_pcm(pcm, frames * 4, sr);
            g_bt_pace_ns += chunk_ns;
            return;
        }
        if (bt_wait_abort()) return;
        usleep(20000);
    }
}

static void *audio_output_worker(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, 0, -10);

    int16_t chunk[1024 * 2];
    bool dma_active = false;
    uint32_t wired_sr = 0;

    while (g_running) {
        bool playing = false;
        uint32_t sr = 44100;
        int vol = 70;
        size_t got = 0;

        pthread_mutex_lock(&g_out_mtx);
        playing = g_out_playing;
        sr = g_out_sr;
        vol = g_out_vol;
        if (playing) {
            size_t avail = (g_out_wr - g_out_rd) & PCM_OUT_MASK;
            if (avail > 0) {
                size_t want = avail < 1024 ? avail : 1024;
                size_t r = g_out_rd;
                for (size_t i = 0; i < want; i++) {
                    chunk[i * 2 + 0] = g_out_ring[r * 2 + 0];
                    chunk[i * 2 + 1] = g_out_ring[r * 2 + 1];
                    r = (r + 1) & PCM_OUT_MASK;
                }
                g_out_rd = r;
                got = want;
            }
        }
        pthread_mutex_unlock(&g_out_mtx);

        if (!playing || got == 0) {
            if (!playing) {
                g_bt_pace_ns = 0;
                if (y1_audio_is_open()) {
                    y1_audio_set_dma_running(false);
                    y1_audio_close();
                    wired_sr = 0;
                }
                if (dma_active) {
                    y1_audio_wake_lock(false);
                    y1_audio_set_deep_idle(true);
                    dma_active = false;
                }
                usleep(120000);
            } else {
                usleep(10000);
            }
            continue;
        }

        if (!dma_active) {
            y1_audio_set_deep_idle(false);
            y1_audio_wake_lock(true);
            dma_active = true;
        }

        if (y1_bt_a2dp_is_streaming()) {
            if (y1_audio_is_open()) {
                y1_audio_set_dma_running(false);
                y1_audio_close();
                wired_sr = 0;
            }
            pace_bt_a2dp_write(chunk, got, sr);
        } else {
            g_bt_pace_ns = 0;
            if (!y1_audio_is_open()) {
                y1_audio_open(sr);
                apply_hw_volume(vol);
                wired_sr = sr;
            } else if (wired_sr != sr) {
                y1_audio_set_sample_rate(sr);
                wired_sr = sr;
            }
            y1_audio_set_dma_running(true);
            y1_audio_write_pcm(chunk, got * 4);
        }
    }

    if (y1_audio_is_open()) {
        y1_audio_set_dma_running(false);
        y1_audio_wake_lock(false);
        y1_audio_set_deep_idle(true);
        y1_audio_close();
    }
    return NULL;
}

static void *audio_worker(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, 0, -5);

    struct ff_track cur_tr;
    bool cur_open = false;
    struct ff_track next_tr;
    bool next_open = false;
    int  next_pos = -1;
    int  next_tr_idx = -1;

    uint32_t cur_sr = 44100;
    int16_t out_buf[2304 * 2];
    char cur_rb_path[256] = "";

    while (g_running) {
        bool want_play = false;
        bool need_reload = false;
        bool need_stop = false;
        bool q_dirty = false;
        bool has_seek = false;
        int  seek_delta = 0;
        int  vol_pct = 70;
        int  tr_idx = -1;
        int  planned_next_pos = -1;
        int  planned_next_tr = -1;

        pthread_mutex_lock(&g_mtx);
        want_play = g_playing;
        need_reload = g_reload_req;
        g_reload_req = false;
        need_stop = g_stop_req;
        g_stop_req = false;
        q_dirty = g_queue_dirty;
        g_queue_dirty = false;
        has_seek = g_has_seek_req;
        seek_delta = g_seek_req_ms;
        g_has_seek_req = false;
        g_seek_req_ms = 0;
        vol_pct = g_volume_pct;
        if (g_queue && g_queue_len > 0 && g_queue_pos >= 0 && g_queue_pos < g_queue_len)
            tr_idx = g_queue[g_queue_pos];
        planned_next_pos = peek_next_pos_locked();
        if (planned_next_pos >= 0 && g_queue)
            planned_next_tr = g_queue[planned_next_pos];
        pthread_mutex_unlock(&g_mtx);

        if ((need_stop || need_reload || q_dirty) && next_open) {
            ff_track_close(&next_tr);
            next_open = false;
            next_pos = -1;
            next_tr_idx = -1;
        }

        if (need_stop) {
            out_ring_flush(false, cur_sr, vol_pct);
            if (cur_open) {
                ff_track_close(&cur_tr);
                cur_open = false;
            }
            usleep(50000);
            continue;
        }

        if (need_reload) {
            out_ring_flush(want_play, cur_sr, vol_pct);
            if (cur_open) {
                ff_track_close(&cur_tr);
                cur_open = false;
            }
            if (tr_idx >= 0 && g_db && tr_idx < g_db->track_count) {
                const struct ipod_track *tr = &g_db->tracks[tr_idx];
                if (ff_track_open(&cur_tr, tr->fs_path)) {
                    cur_open = true;
                    cur_sr = cur_tr.sample_rate;
                    snprintf(cur_rb_path, sizeof(cur_rb_path), "%s", tr->rb_path);
                    uint32_t dur = cur_tr.dur_ms ? cur_tr.dur_ms : tr->duration_ms;

                    out_ring_flush(want_play, cur_sr, vol_pct);

                    uint32_t init_pos = 0;
                    pthread_mutex_lock(&g_mtx);
                    init_pos = g_start_pos_ms;
                    g_start_pos_ms = 0;
                    g_pos_ms = init_pos;
                    g_dur_ms = dur;
                    pthread_mutex_unlock(&g_mtx);

                    if (init_pos > 0)
                        ff_track_seek_ms(&cur_tr, init_pos);

                    y1_bt_update_metadata(tr->title, tr->artist, tr->album, dur, init_pos, want_play ? 1 : 2);
                } else {
                    pthread_mutex_lock(&g_mtx);
                    g_playing = false;
                    pthread_mutex_unlock(&g_mtx);
                }
            }
        }

        if (!cur_open || !want_play) {
            pthread_mutex_lock(&g_out_mtx);
            g_out_playing = false;
            pthread_mutex_unlock(&g_out_mtx);
            usleep(120000);
            continue;
        }

        if (has_seek) {
            out_ring_flush(true, cur_sr, vol_pct);
            int64_t cur_ms = (int64_t)ff_track_cur_ms(&cur_tr);
            int64_t target_ms = cur_ms + seek_delta;
            if (target_ms < 0) target_ms = 0;
            ff_track_seek_ms(&cur_tr, (uint32_t)target_ms);
            pthread_mutex_lock(&g_mtx);
            g_pos_ms = (uint32_t)target_ms;
            pthread_mutex_unlock(&g_mtx);
        }

        pthread_mutex_lock(&g_out_mtx);
        g_out_playing = true;
        g_out_vol = vol_pct;
        pthread_mutex_unlock(&g_out_mtx);

        /* Keep up to ~550ms buffered in RAM; sleep when buffer is comfortably full */
        if (out_ring_used() >= PCM_OUT_FRAMES - 4096) {
            if (cur_open) {
                uint32_t dec_ms = ff_track_cur_ms(&cur_tr);
                uint32_t buf_ms = (uint32_t)(((uint64_t)out_ring_used() * 1000ULL) / (cur_sr > 0 ? cur_sr : 44100));
                pthread_mutex_lock(&g_mtx);
                g_pos_ms = (dec_ms > buf_ms) ? (dec_ms - buf_ms) : 0;
                pthread_mutex_unlock(&g_mtx);
            }
            usleep(20000);
            continue;
        }

        /* Gapless pre-open ~5s before EOF */
        uint32_t cur_ms_now = ff_track_cur_ms(&cur_tr);
        if (!next_open && planned_next_pos >= 0 && planned_next_tr >= 0 &&
            g_db && planned_next_tr < g_db->track_count &&
            (cur_tr.dur_ms == 0 || cur_ms_now + 5000 >= cur_tr.dur_ms)) {
            const struct ipod_track *ntr = &g_db->tracks[planned_next_tr];
            if (ff_track_open(&next_tr, ntr->fs_path)) {
                next_open = true;
                next_pos = planned_next_pos;
                next_tr_idx = planned_next_tr;
            }
        }

        int scale_q12 = (vol_pct * vol_pct * 4096) / 10000;
        size_t frames = ff_track_read_frames(&cur_tr, out_buf, 2304);
        if (frames > 0)
            scale_stereo_pcm(out_buf, frames, scale_q12);

        if (frames < 2304) {
            if (cur_rb_path[0])
                y1_ipod_native_record_play(cur_rb_path, 1, (uint32_t)time(NULL), 0);

            ff_track_close(&cur_tr);
            cur_open = false;

            if (!next_open && planned_next_pos >= 0 && planned_next_tr >= 0 &&
                g_db && planned_next_tr < g_db->track_count) {
                const struct ipod_track *ntr = &g_db->tracks[planned_next_tr];
                if (ff_track_open(&next_tr, ntr->fs_path)) {
                    next_open = true;
                    next_pos = planned_next_pos;
                    next_tr_idx = planned_next_tr;
                }
            }

            if (next_open) {
                cur_tr = next_tr;
                cur_open = true;
                memset(&next_tr, 0, sizeof(next_tr));
                next_open = false;

                uint32_t new_sr = cur_tr.sample_rate;
                const struct ipod_track *ntr = &g_db->tracks[next_tr_idx];
                snprintf(cur_rb_path, sizeof(cur_rb_path), "%s", ntr->rb_path);
                uint32_t ndur = cur_tr.dur_ms ? cur_tr.dur_ms : ntr->duration_ms;

                pthread_mutex_lock(&g_mtx);
                g_queue_pos = next_pos;
                g_pos_ms = 0;
                g_dur_ms = ndur;
                pthread_mutex_unlock(&g_mtx);

                y1_bt_update_metadata(ntr->title, ntr->artist, ntr->album, ndur, 0, 1);

                if (new_sr == cur_sr) {
                    size_t rem = 2304 - frames;
                    size_t nread = ff_track_read_frames(&cur_tr, out_buf + frames * 2, rem);
                    if (nread > 0) {
                        scale_stereo_pcm(out_buf + frames * 2, nread, scale_q12);
                        frames += nread;
                    }
                } else {
                    if (frames > 0)
                        out_ring_push(out_buf, frames, cur_sr, vol_pct);
                    cur_sr = new_sr;
                    continue;
                }
            } else {
                pthread_mutex_lock(&g_mtx);
                g_playing = false;
                pthread_mutex_unlock(&g_mtx);
                if (frames == 0) continue;
            }
        }

        if (frames > 0)
            out_ring_push(out_buf, frames, cur_sr, vol_pct);

        if (cur_open) {
            uint32_t dec_ms = ff_track_cur_ms(&cur_tr);
            uint32_t buf_ms = (uint32_t)(((uint64_t)out_ring_used() * 1000ULL) / (cur_sr > 0 ? cur_sr : 44100));
            pthread_mutex_lock(&g_mtx);
            g_pos_ms = (dec_ms > buf_ms) ? (dec_ms - buf_ms) : 0;
            pthread_mutex_unlock(&g_mtx);
        }
    }

    if (next_open) ff_track_close(&next_tr);
    if (cur_open) ff_track_close(&cur_tr);
    return NULL;
}

void player_init(const struct ipod_db *db)
{
    g_db = db;
    srand((unsigned int)time(NULL));
    g_running = true;
    pthread_create(&g_thread, NULL, audio_worker, NULL);
    pthread_create(&g_out_thread, NULL, audio_output_worker, NULL);
}

static void shuffle_active_queue_locked(int keep_pos)
{
    if (!g_queue || g_queue_len <= 1) return;
    if (keep_pos >= 0 && keep_pos < g_queue_len && keep_pos != 0) {
        int tmp = g_queue[0];
        g_queue[0] = g_queue[keep_pos];
        g_queue[keep_pos] = tmp;
        g_queue_pos = 0;
    }
    int start = (keep_pos >= 0) ? 1 : 0;
    for (int i = g_queue_len - 1; i > start; i--) {
        int j = start + (rand() % (i - start + 1));
        int tmp = g_queue[i];
        g_queue[i] = g_queue[j];
        g_queue[j] = tmp;
    }
}

void player_shutdown(void)
{
    if (!g_running) return;
    g_running = false;
    pthread_join(g_out_thread, NULL);
    pthread_join(g_thread, NULL);
    pthread_mutex_lock(&g_mtx);
    free(g_queue);
    free(g_orig_queue);
    g_queue = NULL;
    g_orig_queue = NULL;
    g_queue_len = 0;
    pthread_mutex_unlock(&g_mtx);
}

void player_play_queue(const int *track_indices, int count, int start_pos)
{
    if (!track_indices || count <= 0) return;
    pthread_mutex_lock(&g_mtx);
    free(g_queue);
    free(g_orig_queue);
    g_queue = malloc(sizeof(int) * (size_t)count);
    g_orig_queue = malloc(sizeof(int) * (size_t)count);
    if (g_queue && g_orig_queue) {
        memcpy(g_orig_queue, track_indices, sizeof(int) * (size_t)count);
        memcpy(g_queue, track_indices, sizeof(int) * (size_t)count);
        g_queue_len = count;
        g_queue_pos = (start_pos >= 0 && start_pos < count) ? start_pos : 0;
        if (g_shuffle && count > 1)
            shuffle_active_queue_locked(g_queue_pos);
        g_start_pos_ms = 0;
        g_playing = true;
        g_reload_req = true;
    } else {
        free(g_queue); free(g_orig_queue);
        g_queue = NULL; g_orig_queue = NULL; g_queue_len = 0;
    }
    pthread_mutex_unlock(&g_mtx);
}

void player_prepare_resume(const int *track_indices, int count, int start_pos, uint32_t pos_ms, int vol_pct)
{
    if (!track_indices || count <= 0) return;
    if (vol_pct < 0) vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;
    pthread_mutex_lock(&g_mtx);
    free(g_queue);
    free(g_orig_queue);
    g_queue = malloc(sizeof(int) * (size_t)count);
    g_orig_queue = malloc(sizeof(int) * (size_t)count);
    if (g_queue && g_orig_queue) {
        memcpy(g_orig_queue, track_indices, sizeof(int) * (size_t)count);
        memcpy(g_queue, track_indices, sizeof(int) * (size_t)count);
        g_queue_len = count;
        g_queue_pos = (start_pos >= 0 && start_pos < count) ? start_pos : 0;
        if (g_shuffle && count > 1)
            shuffle_active_queue_locked(g_queue_pos);
        g_start_pos_ms = pos_ms;
        g_pos_ms = pos_ms;
        g_volume_pct = vol_pct;
        apply_hw_volume(g_volume_pct);
        g_playing = false;
        g_reload_req = true;
    } else {
        free(g_queue); free(g_orig_queue);
        g_queue = NULL; g_orig_queue = NULL; g_queue_len = 0;
    }
    pthread_mutex_unlock(&g_mtx);
}

void player_play_next(int track_idx)
{
    pthread_mutex_lock(&g_mtx);
    if (!g_queue || g_queue_len <= 0) {
        g_queue = malloc(sizeof(int));
        if (g_queue) {
            g_queue[0] = track_idx;
            g_queue_len = 1;
            g_queue_pos = 0;
            g_playing = true;
            g_reload_req = true;
        }
    } else {
        int *nq = realloc(g_queue, sizeof(int) * (size_t)(g_queue_len + 1));
        if (nq) {
            g_queue = nq;
            int ins = g_queue_pos + 1;
            if (ins > g_queue_len) ins = g_queue_len;
            if (ins < g_queue_len)
                memmove(&g_queue[ins + 1], &g_queue[ins], sizeof(int) * (size_t)(g_queue_len - ins));
            g_queue[ins] = track_idx;
            g_queue_len++;
            g_queue_dirty = true;
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

void player_add_to_queue(int track_idx)
{
    pthread_mutex_lock(&g_mtx);
    int *nq = realloc(g_queue, sizeof(int) * (size_t)(g_queue_len + 1));
    if (nq) {
        g_queue = nq;
        g_queue[g_queue_len++] = track_idx;
        g_queue_dirty = true;
        if (g_queue_len == 1) {
            g_queue_pos = 0;
            g_playing = true;
            g_reload_req = true;
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

void player_jump_queue_pos(int pos)
{
    pthread_mutex_lock(&g_mtx);
    if (g_queue && pos >= 0 && pos < g_queue_len) {
        g_queue_pos = pos;
        g_playing = true;
        g_reload_req = true;
    }
    pthread_mutex_unlock(&g_mtx);
}

void player_remove_queue_pos(int pos)
{
    pthread_mutex_lock(&g_mtx);
    if (g_queue && pos >= 0 && pos < g_queue_len) {
        if (g_queue_len == 1) {
            g_playing = false;
            g_reload_req = false;
            g_stop_req = true;
            free(g_queue);
            free(g_orig_queue);
            g_queue = NULL;
            g_orig_queue = NULL;
            g_queue_len = 0;
            g_queue_pos = 0;
            g_pos_ms = 0;
            g_dur_ms = 0;
        } else {
            for (int i = pos; i < g_queue_len - 1; i++)
                g_queue[i] = g_queue[i + 1];
            g_queue_len--;
            g_queue_dirty = true;
            if (pos < g_queue_pos) {
                g_queue_pos--;
            } else if (pos == g_queue_pos) {
                if (g_queue_pos >= g_queue_len)
                    g_queue_pos = 0;
                g_reload_req = true;
            }
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

void player_move_queue_pos(int from_pos, int to_pos)
{
    pthread_mutex_lock(&g_mtx);
    if (g_queue && from_pos >= 0 && from_pos < g_queue_len &&
        to_pos >= 0 && to_pos < g_queue_len && from_pos != to_pos) {
        int moved = g_queue[from_pos];
        if (from_pos < to_pos) {
            for (int i = from_pos; i < to_pos; i++)
                g_queue[i] = g_queue[i + 1];
        } else {
            for (int i = from_pos; i > to_pos; i--)
                g_queue[i] = g_queue[i - 1];
        }
        g_queue[to_pos] = moved;
        if (g_queue_pos == from_pos)
            g_queue_pos = to_pos;
        else if (from_pos < g_queue_pos && to_pos >= g_queue_pos)
            g_queue_pos--;
        else if (from_pos > g_queue_pos && to_pos <= g_queue_pos)
            g_queue_pos++;
        g_queue_dirty = true;
    }
    pthread_mutex_unlock(&g_mtx);
}

int player_get_queue(int *out_indices, int max_count)
{
    if (!out_indices || max_count <= 0) return 0;
    pthread_mutex_lock(&g_mtx);
    int n = g_queue_len < max_count ? g_queue_len : max_count;
    if (g_queue && n > 0)
        memcpy(out_indices, g_queue, sizeof(int) * (size_t)n);
    pthread_mutex_unlock(&g_mtx);
    return n;
}

void player_toggle_pause(void)
{
    pthread_mutex_lock(&g_mtx);
    if (g_queue_len > 0) g_playing = !g_playing;
    pthread_mutex_unlock(&g_mtx);
}

void player_pause(void)
{
    pthread_mutex_lock(&g_mtx);
    g_playing = false;
    pthread_mutex_unlock(&g_mtx);
}

void player_play(void)
{
    pthread_mutex_lock(&g_mtx);
    if (g_queue_len > 0) g_playing = true;
    pthread_mutex_unlock(&g_mtx);
}

void player_stop(void)
{
    pthread_mutex_lock(&g_mtx);
    g_playing = false;
    g_reload_req = false;
    g_stop_req = true;
    free(g_queue);
    free(g_orig_queue);
    g_queue = NULL;
    g_orig_queue = NULL;
    g_queue_len = 0;
    g_queue_pos = 0;
    g_pos_ms = 0;
    g_dur_ms = 0;
    pthread_mutex_unlock(&g_mtx);
    usleep(120000);
}

void player_next(void)
{
    pthread_mutex_lock(&g_mtx);
    if (g_queue_len > 0) {
        g_queue_pos = (g_queue_pos + 1) % g_queue_len;
        g_playing = true;
        g_reload_req = true;
    }
    pthread_mutex_unlock(&g_mtx);
}

void player_prev(void)
{
    pthread_mutex_lock(&g_mtx);
    if (g_queue_len > 0) {
        if (g_pos_ms > 3000) {
            g_seek_req_ms = -(int)g_pos_ms;
            g_has_seek_req = true;
        } else {
            g_queue_pos = (g_queue_pos + g_queue_len - 1) % g_queue_len;
            g_playing = true;
            g_reload_req = true;
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

void player_seek_delta_ms(int delta_ms)
{
    pthread_mutex_lock(&g_mtx);
    g_seek_req_ms += delta_ms;
    g_has_seek_req = true;
    pthread_mutex_unlock(&g_mtx);
}

void player_set_volume(int vol_pct)
{
    pthread_mutex_lock(&g_mtx);
    if (vol_pct < 0) vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;
    g_volume_pct = vol_pct;
    apply_hw_volume(g_volume_pct);
    pthread_mutex_unlock(&g_mtx);
}

void player_adjust_volume(int delta_pct)
{
    pthread_mutex_lock(&g_mtx);
    g_volume_pct += delta_pct;
    if (g_volume_pct < 0) g_volume_pct = 0;
    if (g_volume_pct > 100) g_volume_pct = 100;
    apply_hw_volume(g_volume_pct);
    pthread_mutex_unlock(&g_mtx);
}

void player_set_shuffle(bool on)
{
    pthread_mutex_lock(&g_mtx);
    if (g_shuffle != on) {
        g_shuffle = on;
        if (g_queue && g_queue_len > 1) {
            int cur_ti = (g_queue_pos >= 0 && g_queue_pos < g_queue_len) ? g_queue[g_queue_pos] : -1;
            if (on) {
                shuffle_active_queue_locked(g_queue_pos);
            } else if (g_orig_queue) {
                memcpy(g_queue, g_orig_queue, sizeof(int) * (size_t)g_queue_len);
                g_queue_pos = 0;
                for (int i = 0; i < g_queue_len; i++) {
                    if (g_queue[i] == cur_ti) { g_queue_pos = i; break; }
                }
            }
            g_queue_dirty = true;
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

void player_set_repeat(enum repeat_mode mode)
{
    pthread_mutex_lock(&g_mtx);
    g_repeat = mode;
    g_queue_dirty = true;
    pthread_mutex_unlock(&g_mtx);
}

void player_get_status(struct player_status *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_mtx);
    out->queue_len  = g_queue_len;
    out->queue_pos  = g_queue_pos;
    out->has_track  = (g_queue && g_queue_len > 0 && g_queue_pos >= 0 && g_queue_pos < g_queue_len);
    out->track_idx  = out->has_track ? g_queue[g_queue_pos] : -1;
    out->playing    = g_playing;
    out->pos_ms     = g_pos_ms;
    out->dur_ms     = g_dur_ms;
    out->volume_pct = g_volume_pct;
    out->shuffle    = g_shuffle;
    out->repeat     = g_repeat;
    pthread_mutex_unlock(&g_mtx);
}
