/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"

#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)

#include <string.h>
#include "lcd.h"
#include "settings.h"
#include "kernel.h"
#include "action.h"
#include "audio.h"
#include "playback.h"
#include "buffering.h"
#include "appevents.h"
#include "timefuncs.h"
#include "skin_albumart_color.h"
#if defined(HAS_BUTTON_HOLD) && !defined(INNIOASIS_Y1)
#include "button.h"
#endif

#define AA_FADE_DURATION  (HZ / 2)   /* 500ms */
#define HISTOGRAM_BUCKETS 4096
#define SAMPLE_STRIDE     4          /* sample every 4th pixel */
#define SATURATION_BASE   8          /* base score for unsaturated colors */
#define NO_ART_TIMEOUT    HZ         /* 1s timeout before concluding no art */

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

struct dynamic_colors_cache {
    unsigned int dominant;       /* target bg color */
    unsigned int accent;         /* target fg color */
    unsigned int prev_dominant;  /* fade-start bg */
    unsigned int prev_accent;    /* fade-start fg */
    unsigned int theme_fg;       /* saved original theme fg */
    unsigned int theme_bg;       /* saved original theme bg */
    unsigned int theme_lss;      /* saved selector start color */
    unsigned int theme_lse;      /* saved selector end color */
    unsigned int theme_lst;      /* saved selector text color */
    unsigned int theme_sep;      /* saved list separator color */
    long fade_start_tick;
    long track_change_tick;      /* when TRACK_CHANGE fired */
    bool valid;                  /* have valid AA colors */
    bool fading;                 /* fade in progress */
    bool fading_out;             /* fading to defaults after disable/stop */
    bool was_enabled;            /* track setting state for toggle detection */
    bool needs_full_update;      /* set when fade completes for full redraw */
    bool needs_screen_clear;     /* set when fade completes to clear bg gaps */
    int last_mode;
    int last_saturation;
    int last_contrast;
    int last_day_start;
    int last_night_start;
};

static struct dynamic_colors_cache cache;
static volatile bool needs_extraction;
static uint16_t histogram[HISTOGRAM_BUCKETS];

static bool colors_locked(void)
{
#if defined(INNIOASIS_Y1)
    return is_keys_locked();
#elif defined(HAS_BUTTON_HOLD)
    return button_hold();
#else
    return false;
#endif
}

static int compute_luminance(int r8, int g8, int b8)
{
    return (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
}

static int clamp_channel(int value)
{
    return MIN(MAX(value, 0), 255);
}

static void adjust_saturation(int *r, int *g, int *b)
{
    int saturation = global_settings.dynamic_color_saturation;
    int luminance = compute_luminance(*r, *g, *b);

    *r = clamp_channel(luminance + ((*r - luminance) * saturation) / 100);
    *g = clamp_channel(luminance + ((*g - luminance) * saturation) / 100);
    *b = clamp_channel(luminance + ((*b - luminance) * saturation) / 100);
}

static int effective_color_mode(void)
{
    if (global_settings.dynamic_color_mode != DYNAMIC_COLOR_MODE_AUTO)
        return global_settings.dynamic_color_mode;

    struct tm *now = get_time();
    if (!now)
        return DYNAMIC_COLOR_MODE_DYNAMIC;

    int hour = now->tm_hour;
    int day_start = global_settings.dynamic_color_day_start;
    int night_start = global_settings.dynamic_color_night_start;
    bool is_day = day_start <= night_start ?
                  (hour >= day_start && hour < night_start) :
                  (hour >= day_start || hour < night_start);
    return is_day ? DYNAMIC_COLOR_MODE_LIGHT : DYNAMIC_COLOR_MODE_DARK;
}

static int pixel_bucket(fb_data pixel)
{
    int r = FB_UNPACK_RED(pixel) >> 4;
    int g = FB_UNPACK_GREEN(pixel) >> 4;
    int b = FB_UNPACK_BLUE(pixel) >> 4;

    return (r << 8) | (g << 4) | b;
}

static unsigned int lerp_color(unsigned int c1, unsigned int c2, int t)
{
    /* t: 0..256, 0 = fully c1, 256 = fully c2 */
    int r1 = RGB_UNPACK_RED(c1);
    int g1 = RGB_UNPACK_GREEN(c1);
    int b1 = RGB_UNPACK_BLUE(c1);
    int r2 = RGB_UNPACK_RED(c2);
    int g2 = RGB_UNPACK_GREEN(c2);
    int b2 = RGB_UNPACK_BLUE(c2);

    int r = r1 + (((r2 - r1) * t) >> 8);
    int g = g1 + (((g2 - g1) * t) >> 8);
    int b = b1 + (((b2 - b1) * t) >> 8);

    return LCD_RGBPACK(r, g, b);
}

static int fade_progress(void)
{
    long elapsed = current_tick - cache.fade_start_tick;
    if (elapsed <= 0)
        return 0;
    if (elapsed >= AA_FADE_DURATION)
        return 256;
    return (int)((elapsed * 256) / AA_FADE_DURATION);
}

static void start_fade(unsigned int new_accent, unsigned int new_dominant,
                       bool to_defaults)
{
    if (colors_locked())
    {
        cache.accent = new_accent;
        cache.dominant = new_dominant;
        cache.fading = false;
        cache.fading_out = false;
        cache.valid = !to_defaults;
        cache.needs_full_update = true;
        cache.needs_screen_clear = true;
        return;
    }

    /* Capture current effective colors as fade start */
    if (cache.fading || cache.fading_out)
    {
        int p = fade_progress();
        if (cache.fading_out)
        {
            cache.prev_accent = lerp_color(cache.prev_accent,
                                           cache.theme_fg, p);
            cache.prev_dominant = lerp_color(cache.prev_dominant,
                                             cache.theme_bg, p);
        }
        else
        {
            cache.prev_accent = lerp_color(cache.prev_accent,
                                           cache.accent, p);
            cache.prev_dominant = lerp_color(cache.prev_dominant,
                                             cache.dominant, p);
        }
    }
    else if (cache.valid)
    {
        cache.prev_accent = cache.accent;
        cache.prev_dominant = cache.dominant;
    }
    else
    {
        cache.prev_accent = cache.theme_fg;
        cache.prev_dominant = cache.theme_bg;
    }

    cache.accent = new_accent;
    cache.dominant = new_dominant;
    cache.fade_start_tick = current_tick;
    cache.fading = !to_defaults;
    cache.fading_out = to_defaults;
    cache.valid = true;
}

static void extract_colors(const struct bitmap *bmp)
{
    if (!bmp->data || bmp->width <= 0 || bmp->height <= 0)
        return;

    fb_data *pixels = (fb_data *)bmp->data;
    int width = bmp->width;
    int height = bmp->height;
    int total_pixels = width * height;
    int i;

    memset(histogram, 0, sizeof(histogram));

    /* Pass 1: build quantized histogram */
    for (i = 0; i < total_pixels; i += SAMPLE_STRIDE)
    {
        int bucket = pixel_bucket(pixels[i]);
        if (histogram[bucket] < UINT16_MAX)
            histogram[bucket]++;
    }

    /* Find dominant bucket (skip near-black/near-white, prefer saturated) */
    int best_bucket = -1;
    unsigned int best_score = 0;
    int fallback_bucket = -1;
    uint16_t fallback_count = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        if (histogram[i] == 0)
            continue;

        int r4 = (i >> 8) & 0xF;
        int g4 = (i >> 4) & 0xF;
        int b4 = i & 0xF;

        /* Track unfiltered best as fallback */
        if (histogram[i] > fallback_count)
        {
            fallback_count = histogram[i];
            fallback_bucket = i;
        }

        /* Skip near-black and near-white */
        if (r4 < 2 && g4 < 2 && b4 < 2)
            continue;
        if (r4 > 13 && g4 > 13 && b4 > 13)
            continue;
        /* Skip very dark colors regardless of hue */
        int lum4 = (r4 * 5 + g4 * 9 + b4 * 2) >> 4;
        if (lum4 < 2)
            continue;

        /* Score by count weighted by saturation (vibrant colors preferred) */
        int max_c = MAX(MAX(r4, g4), b4);
        int min_c = MIN(MIN(r4, g4), b4);
        int sat = max_c > 0 ? ((max_c - min_c) * 15) / max_c : 0;
        unsigned int score = (unsigned int)histogram[i]
                           * (sat + SATURATION_BASE) / SATURATION_BASE;

        if (score > best_score)
        {
            best_score = score;
            best_bucket = i;
        }
    }

    if (best_bucket < 0)
        best_bucket = fallback_bucket;
    if (best_bucket < 0)
        return; /* empty image? */

    /* Pass 2: average full-precision RGB for dominant bucket */
    long sum_r = 0, sum_g = 0, sum_b = 0;
    int count = 0;

    for (i = 0; i < total_pixels; i += SAMPLE_STRIDE)
    {
        fb_data px = pixels[i];
        int bucket = pixel_bucket(px);

        if (bucket == best_bucket)
        {
            sum_r += FB_UNPACK_RED(px);
            sum_g += FB_UNPACK_GREEN(px);
            sum_b += FB_UNPACK_BLUE(px);
            count++;
        }
    }

    int dom_r = count ? (int)(sum_r / count) : 0;
    int dom_g = count ? (int)(sum_g / count) : 0;
    int dom_b = count ? (int)(sum_b / count) : 0;
    unsigned int dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
    int dom_lum = compute_luminance(dom_r, dom_g, dom_b);

    int min_contrast = 100 * global_settings.dynamic_color_contrast / 100;

    /* Find accent: best-scored bucket with sufficient contrast */
    int accent_bucket = -1;
    unsigned int accent_score = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        if (histogram[i] == 0 || i == best_bucket)
            continue;

        int r4 = (i >> 8) & 0xF;
        int g4 = (i >> 4) & 0xF;
        int b4 = i & 0xF;

        /* Approximate 8-bit values from 4-bit */
        int r8 = (r4 << 4) | r4;
        int g8 = (g4 << 4) | g4;
        int b8 = (b4 << 4) | b4;
        int lum = compute_luminance(r8, g8, b8);

        int contrast = dom_lum > lum ? dom_lum - lum : lum - dom_lum;
        if (contrast >= min_contrast)
        {
            /* Score by count weighted by saturation */
            int max_c = MAX(MAX(r4, g4), b4);
            int min_c = MIN(MIN(r4, g4), b4);
            int sat = max_c > 0 ? ((max_c - min_c) * 15) / max_c : 0;
            unsigned int score = (unsigned int)histogram[i]
                               * (sat + SATURATION_BASE) / SATURATION_BASE;

            if (score > accent_score)
            {
                accent_score = score;
                accent_bucket = i;
            }
        }
    }

    int acc_r, acc_g, acc_b;
    unsigned int accent;
    if (accent_bucket >= 0)
    {
        /* Average full-precision RGB for accent bucket */
        sum_r = sum_g = sum_b = 0;
        count = 0;
        for (i = 0; i < total_pixels; i += SAMPLE_STRIDE)
        {
            fb_data px = pixels[i];
            int bucket = pixel_bucket(px);

            if (bucket == accent_bucket)
            {
                sum_r += FB_UNPACK_RED(px);
                sum_g += FB_UNPACK_GREEN(px);
                sum_b += FB_UNPACK_BLUE(px);
                count++;
            }
        }
        acc_r = count ? (int)(sum_r / count) : 0;
        acc_g = count ? (int)(sum_g / count) : 0;
        acc_b = count ? (int)(sum_b / count) : 0;
        accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
    }
    else
    {
        /* Hard fallback: dark dominant -> white text, light -> black */
        if (dom_lum < 128)
        {
            acc_r = 255; acc_g = 255; acc_b = 255;
        }
        else
        {
            acc_r = 0; acc_g = 0; acc_b = 0;
        }
        accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
    }

    /* Readability enforcement: scale accent to hit target luminance
     * while preserving hue (proportional channel scaling) */
    int acc_lum = compute_luminance(acc_r, acc_g, acc_b);
    int contrast = dom_lum > acc_lum ? dom_lum - acc_lum : acc_lum - dom_lum;
    if (contrast < min_contrast)
    {
        int target_lum;
        if (dom_lum < 128)
            target_lum = MIN(dom_lum + min_contrast, 255);
        else
            target_lum = MAX(dom_lum - min_contrast, 0);

        if (acc_lum > 0)
        {
            int scale = (target_lum * 256) / acc_lum;
            acc_r = MIN((acc_r * scale) >> 8, 255);
            acc_g = MIN((acc_g * scale) >> 8, 255);
            acc_b = MIN((acc_b * scale) >> 8, 255);
        }
        else
        {
            acc_r = target_lum;
            acc_g = target_lum;
            acc_b = target_lum;
        }
        accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
    }

    adjust_saturation(&dom_r, &dom_g, &dom_b);
    adjust_saturation(&acc_r, &acc_g, &acc_b);
    dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
    accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
    dom_lum = compute_luminance(dom_r, dom_g, dom_b);

    int mode = effective_color_mode();
    if (mode == DYNAMIC_COLOR_MODE_LIGHT || mode == DYNAMIC_COLOR_MODE_DARK)
    {
        int target_bg = mode == DYNAMIC_COLOR_MODE_LIGHT ?
                        128 + min_contrast / 2 : 127 - min_contrast / 2;
        int target_fg = 255 - target_bg;

        if (mode == DYNAMIC_COLOR_MODE_LIGHT && dom_lum < target_bg)
        {
            int scale = (target_bg * 256) / MAX(dom_lum, 1);
            dom_r = MIN((dom_r * scale) >> 8, 255);
            dom_g = MIN((dom_g * scale) >> 8, 255);
            dom_b = MIN((dom_b * scale) >> 8, 255);
        }
        else if (mode == DYNAMIC_COLOR_MODE_DARK && dom_lum > target_bg)
        {
            dom_r = (dom_r * target_bg) / MAX(dom_lum, 1);
            dom_g = (dom_g * target_bg) / MAX(dom_lum, 1);
            dom_b = (dom_b * target_bg) / MAX(dom_lum, 1);
        }

        int lum = compute_luminance(acc_r, acc_g, acc_b);
        if (mode == DYNAMIC_COLOR_MODE_LIGHT && lum > target_fg)
        {
            acc_r = (acc_r * target_fg) / MAX(lum, 1);
            acc_g = (acc_g * target_fg) / MAX(lum, 1);
            acc_b = (acc_b * target_fg) / MAX(lum, 1);
        }
        else if (mode == DYNAMIC_COLOR_MODE_DARK && lum < target_fg)
        {
            int scale = (target_fg * 256) / MAX(lum, 1);
            acc_r = MIN((acc_r * scale) >> 8, 255);
            acc_g = MIN((acc_g * scale) >> 8, 255);
            acc_b = MIN((acc_b * scale) >> 8, 255);
        }
        dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
        accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
    }

    int final_dom_lum = compute_luminance(dom_r, dom_g, dom_b);
    int final_acc_lum = compute_luminance(acc_r, acc_g, acc_b);
    int final_contrast = final_dom_lum > final_acc_lum ?
                         final_dom_lum - final_acc_lum :
                         final_acc_lum - final_dom_lum;
    if (final_contrast < min_contrast)
    {
        int target_lum = final_acc_lum < final_dom_lum ?
                         final_dom_lum - min_contrast :
                         final_dom_lum + min_contrast;
        if (target_lum >= 0 && target_lum <= 255)
        {
            if (final_acc_lum == 0)
                acc_r = acc_g = acc_b = target_lum;
            else
            {
                int scale = (target_lum * 256) / final_acc_lum;
                acc_r = MIN((acc_r * scale) >> 8, 255);
                acc_g = MIN((acc_g * scale) >> 8, 255);
                acc_b = MIN((acc_b * scale) >> 8, 255);
            }
        }
        else
        {
            target_lum = final_acc_lum < final_dom_lum ?
                         MIN(final_acc_lum + min_contrast, 255) :
                         MAX(final_acc_lum - min_contrast, 0);
            if (final_dom_lum == 0)
                dom_r = dom_g = dom_b = target_lum;
            else
            {
                int scale = (target_lum * 256) / final_dom_lum;
                dom_r = MIN((dom_r * scale) >> 8, 255);
                dom_g = MIN((dom_g * scale) >> 8, 255);
                dom_b = MIN((dom_b * scale) >> 8, 255);
            }
        }
        dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
        accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
    }

    start_fade(accent, dominant, false);
}

static void track_change_cb(unsigned short id, void *param)
{
    (void)param;
    needs_extraction = true;
    if (id == PLAYBACK_EVENT_TRACK_CHANGE)
        cache.track_change_tick = current_tick;
}

static void save_all_theme_colors(void)
{
    cache.theme_fg  = global_settings.fg_color;
    cache.theme_bg  = global_settings.bg_color;
    cache.theme_lss = global_settings.lss_color;
    cache.theme_lse = global_settings.lse_color;
    cache.theme_lst = global_settings.lst_color;
    cache.theme_sep = global_settings.list_separator_color;
}

void dynamic_colors_init(void)
{
    static bool events_registered = false;

    memset(&cache, 0, sizeof(cache));
    save_all_theme_colors();
    cache.was_enabled = global_settings.dynamic_colors;
    cache.last_mode = effective_color_mode();
    cache.last_saturation = global_settings.dynamic_color_saturation;
    cache.last_contrast = global_settings.dynamic_color_contrast;
    cache.last_day_start = global_settings.dynamic_color_day_start;
    cache.last_night_start = global_settings.dynamic_color_night_start;
    cache.track_change_tick = current_tick;
    needs_extraction = global_settings.dynamic_colors;

    if (!events_registered)
    {
        add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_change_cb);
        add_event(PLAYBACK_EVENT_CUR_TRACK_READY, track_change_cb);
        events_registered = true;
    }
}

void dynamic_colors_save_theme(void)
{
    save_all_theme_colors();
    /* Invalidate cached colors — they were for the old theme */
    cache.valid = false;
    cache.fading = false;
    cache.fading_out = false;
    cache.needs_screen_clear = false;
    needs_extraction = true;
}

void dynamic_colors_check_extraction(int aa_slot)
{
    /* Remember valid AA slots from skin_render calls so list_draw
     * can trigger extraction with aa_slot = -1 (use last known) */
    static int last_aa_slot = -1;
    if (aa_slot >= 0)
        last_aa_slot = aa_slot;
    else
        aa_slot = last_aa_slot;

    /* Detect playback stop — initiate fade to defaults */
    if (cache.valid && !cache.fading_out &&
        global_settings.dynamic_colors &&
        !(audio_status() & AUDIO_STATUS_PLAY))
    {
        start_fade(cache.theme_fg, cache.theme_bg, true);
    }

    /* Detect setting toggle */
    bool enabled = global_settings.dynamic_colors;
    if (!enabled && cache.was_enabled && cache.valid && !cache.fading_out)
    {
        /* Setting just turned off — start fade to defaults */
        start_fade(cache.theme_fg, cache.theme_bg, true);
    }
    if (enabled && !cache.was_enabled)
    {
        /* Setting just turned on — try extraction */
        needs_extraction = true;
    }
    cache.was_enabled = enabled;

    int mode = effective_color_mode();
    if (mode != cache.last_mode ||
        global_settings.dynamic_color_saturation != cache.last_saturation ||
        global_settings.dynamic_color_contrast != cache.last_contrast ||
        global_settings.dynamic_color_day_start != cache.last_day_start ||
        global_settings.dynamic_color_night_start != cache.last_night_start)
    {
        needs_extraction = true;
        cache.last_mode = mode;
        cache.last_saturation = global_settings.dynamic_color_saturation;
        cache.last_contrast = global_settings.dynamic_color_contrast;
        cache.last_day_start = global_settings.dynamic_color_day_start;
        cache.last_night_start = global_settings.dynamic_color_night_start;
    }

    if (!needs_extraction)
        return;
    if (!enabled)
    {
        needs_extraction = false;
        return;
    }
    if (aa_slot < 0)
    {
        /* No known AA slot yet — can't extract */
        return;
    }

    int handle = playback_current_aa_hid(aa_slot);
    if (handle >= 0)
    {
        struct bitmap *bmp;
        if (bufgetdata(handle, 0, (void *)&bmp) > 0)
            extract_colors(bmp);
        needs_extraction = false;
    }
    else
    {
        /* Art not available yet — check timeout */
        long elapsed = current_tick - cache.track_change_tick;
        if (elapsed > NO_ART_TIMEOUT)
        {
            /* No art for this track — fade to defaults */
            if (cache.valid && !cache.fading_out)
                start_fade(cache.theme_fg, cache.theme_bg, true);
            needs_extraction = false;
        }
        /* else: keep trying on next render */
    }
}

/* Map a color to its dynamic equivalent using pre-computed effective colors.
 * Matches against all 6 saved theme colors with role-based fallbacks. */
static unsigned int resolve_mapped(unsigned int original,
                                   unsigned int eff_accent,
                                   unsigned int eff_dominant)
{
    /* Primary colors: always mapped */
    if (original == cache.theme_fg)
        return eff_accent;
    if (original == cache.theme_bg)
        return eff_dominant;

    /* Selector bar: default to accent (eye-catching highlight) */
    if (original == cache.theme_lss)
        return (cache.theme_lss == cache.theme_fg) ? eff_accent :
               (cache.theme_lss == cache.theme_bg) ? eff_dominant : eff_accent;

    /* Selector text: should contrast with bar, default to dominant */
    if (original == cache.theme_lst)
        return (cache.theme_lst == cache.theme_bg) ? eff_dominant :
               (cache.theme_lst == cache.theme_fg) ? eff_accent : eff_dominant;

    /* Selector gradient end: blend accent toward dominant */
    if (original == cache.theme_lse)
        return (cache.theme_lse == cache.theme_fg) ? eff_accent :
               (cache.theme_lse == cache.theme_bg) ? eff_dominant :
               lerp_color(eff_accent, eff_dominant, 64);

    /* List separator: subtle line, blend dominant toward accent */
    if (original == cache.theme_sep)
        return lerp_color(eff_dominant, eff_accent, 64);

    return original;
}

unsigned int dynamic_colors_resolve(unsigned int original)
{
    if (colors_locked() && (cache.fading || cache.fading_out))
    {
        if (cache.fading_out)
            cache.valid = false;
        cache.fading = false;
        cache.fading_out = false;
        cache.needs_full_update = true;
        cache.needs_screen_clear = true;
        if (!cache.valid)
            return original;
    }

    /* Fade-out continues even after setting is toggled off */
    if (cache.fading_out)
    {
        int p = fade_progress();
        if (p >= 256)
        {
            cache.fading_out = false;
            cache.valid = false;
            cache.needs_full_update = true;
            cache.needs_screen_clear = true;
            return original;
        }
        return resolve_mapped(original,
            lerp_color(cache.prev_accent, cache.theme_fg, p),
            lerp_color(cache.prev_dominant, cache.theme_bg, p));
    }

    if (!global_settings.dynamic_colors || !cache.valid)
        return original;

    if (cache.fading)
    {
        int p = fade_progress();
        if (p >= 256)
        {
            cache.fading = false;
            cache.needs_full_update = true;
            cache.needs_screen_clear = true;
        }
        else
            return resolve_mapped(original,
                lerp_color(cache.prev_accent, cache.accent, p),
                lerp_color(cache.prev_dominant, cache.dominant, p));
    }

    return resolve_mapped(original, cache.accent, cache.dominant);
}

bool dynamic_colors_fading(void)
{
    return cache.fading || cache.fading_out;
}

bool dynamic_colors_needs_full_update(void)
{
    if (cache.needs_full_update)
    {
        cache.needs_full_update = false;
        return true;
    }
    return false;
}

bool dynamic_colors_screen_clear_needed(void)
{
    if (cache.needs_screen_clear)
    {
        cache.needs_screen_clear = false;
        return true;
    }
    return false;
}

bool dynamic_colors_pending(void)
{
    return needs_extraction && global_settings.dynamic_colors;
}

#endif /* HAVE_ALBUMART && HAVE_LCD_COLOR */
