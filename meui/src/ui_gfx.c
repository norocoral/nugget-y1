#include "ui_gfx.h"
#include "liby1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "stb_image.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

static uint32_t fb[UI_W * UI_H];
static int g_clip_y0 = 0;
static int g_clip_y1 = UI_H;

void gfx_set_clip_y(int y0, int y1)
{
    g_clip_y0 = y0 < 0 ? 0 : y0;
    g_clip_y1 = y1 > UI_H ? UI_H : y1;
}

void gfx_reset_clip(void)
{
    g_clip_y0 = 0;
    g_clip_y1 = UI_H;
}

static const struct ui_palette k_pal_light = {
    .bg           = 0xFFB6F1FAU,
    .surface      = 0xFFEAFAFAU,
    .surface_hi   = 0xFFCCEBEBU,
    .border       = 0xFF89B6BCU,
    .border_hi    = 0xFF303F42U,
    .accent       = 0xFFE8B6FBU,
    .accent_text  = 0xFF3E2F42U,
    .accent_soft  = 0xFFCCEBEBU,
    .sel_bg       = 0xFFCCEBEBU,
    .sel_text     = 0xFF303F42U,
    .modal_bg     = 0xFFEAFAFAU,
    .modal_hi     = 0xFFCCEBEBU,
    .modal_border = 0xFF89B6BCU,
    .text         = 0xFF303F42U,
    .text_dim     = 0xFF4B6166U,
    .text_muted   = 0xFF5D767BU,
    .success      = 0xFF69E484U,
    .warn         = 0xFFF9D4B7U,
    .danger       = 0xFFE11D48U,
    .key_bg       = 0xFFFFFFFFU,
    .switch_icon  = 0xFF303F42U,
};

static const struct ui_palette k_pal_dark = {
    .bg           = 0xFF27484DU,
    .surface      = 0xFF396565U,
    .surface_hi   = 0xFF588181U,
    .border       = 0xFF89B6BCU,
    .border_hi    = 0xFFDFFAFFU,
    .accent       = 0xFF9C7BA8U,
    .accent_text  = 0xFFFADFFFU,
    .accent_soft  = 0xFF588181U,
    .sel_bg       = 0xFF588181U,
    .sel_text     = 0xFFDFFAFFU,
    .modal_bg     = 0xFF396565U,
    .modal_hi     = 0xFF588181U,
    .modal_border = 0xFF89B6BCU,
    .text         = 0xFFDFFAFFU,
    .text_dim     = 0xFFB6DEE4U,
    .text_muted   = 0xFF89B6BCU,
    .success      = 0xFF6BB37AU,
    .warn         = 0xFFB3846BU,
    .danger       = 0xFFF43F5EU,
    .key_bg       = 0xFF477474U,
    .switch_icon  = 0xFFFFFFFFU,
};

/* Wallpaper glass palettes: colorless 50% surfaces, 100% opaque selection,
 * colorful full-opacity accents/switches, and 100% opaque modals/toasts */
static const struct ui_palette k_pal_glass_light = {
    .bg           = 0xFF000000U,
    .surface      = 0x80FFFFFFU,
    .surface_hi   = 0x99E4E4E7U,
    .border       = 0x80FFFFFFU,
    .border_hi    = 0xCC18181BU,
    .accent       = 0xFFE8B6FBU,
    .accent_text  = 0xFF3E2F42U,
    .accent_soft  = 0x80E4E4E7U,
    .sel_bg       = 0xFFFFFFFFU,
    .sel_text     = 0xFF09090BU,
    .modal_bg     = 0xFFF4F4F5U,
    .modal_hi     = 0xFFE4E4E7U,
    .modal_border = 0xFFA1A1AAU,
    .text         = 0xFF09090BU,
    .text_dim     = 0xFF27272AU,
    .text_muted   = 0xFF3F3F46U,
    .success      = 0xFF69E484U,
    .warn         = 0xFFF9D4B7U,
    .danger       = 0xFFE11D48U,
    .key_bg       = 0xFFFFFFFFU,
    .switch_icon  = 0xFF303F42U,
};

static const struct ui_palette k_pal_glass_dark = {
    .bg           = 0xFF000000U,
    .surface      = 0x8018181BU,
    .surface_hi   = 0x993F3F46U,
    .border       = 0x80A1A1AAU,
    .border_hi    = 0xCCFFFFFFU,
    .accent       = 0xFF9C7BA8U,
    .accent_text  = 0xFFFADFFFU,
    .accent_soft  = 0x803F3F46U,
    .sel_bg       = 0xFF3F3F46U,
    .sel_text     = 0xFFFFFFFFU,
    .modal_bg     = 0xFF18181BU,
    .modal_hi     = 0xFF27272AU,
    .modal_border = 0xFF71717AU,
    .text         = 0xFFFFFFFFU,
    .text_dim     = 0xFFE4E4E7U,
    .text_muted   = 0xFFD4D4D8U,
    .success      = 0xFF6BB37AU,
    .warn         = 0xFFB3846BU,
    .danger       = 0xFFF43F5EU,
    .key_bg       = 0xFF27272AU,
    .switch_icon  = 0xFFFFFFFFU,
};

const struct ui_palette *g_pal = &k_pal_light;
static struct ui_palette g_dyn_pal;
static bool g_dark_enabled = false;
static bool g_text_shadow = false;
static uint32_t *g_wp_sharp = NULL;
static uint32_t *g_wp_blur = NULL;
static uint32_t *g_wp_prev_sharp = NULL;
static uint32_t *g_wp_prev_blur = NULL;
static uint64_t  g_wp_fade_start_ms = 0;
static bool      g_wp_fade_active = false;
static bool      g_wp_is_unblurred = false;
static uint32_t  g_wp_fade_fa = 256;
static float     g_wp_hue_prev = 283.0f;
static float     g_wp_hue_curr = 283.0f;
static float     g_wp_hue_target = 283.0f;
static uint32_t  g_rgb565_lut[65536];
static bool      g_rgb565_lut_ready = false;

static void init_rgb565_lut(void)
{
    if (g_rgb565_lut_ready) return;
    for (uint32_t p = 0; p < 65536; p++) {
        uint32_t r5 = (p >> 11) & 0x1F;
        uint32_t g6 = (p >> 5)  & 0x3F;
        uint32_t b5 = p         & 0x1F;
        uint32_t r8 = (r5 << 3) | (r5 >> 2);
        uint32_t g8 = (g6 << 2) | (g6 >> 4);
        uint32_t b8 = (b5 << 3) | (b5 >> 2);
        g_rgb565_lut[p] = 0xFF000000U | (r8 << 16) | (g8 << 8) | b8;
    }
    g_rgb565_lut_ready = true;
}

static uint32_t shift_color_hue(uint32_t col, float hue_deg)
{
    uint32_t a = col & 0xFF000000U;
    float r = (float)((col >> 16) & 0xFF) / 255.0f;
    float g = (float)((col >> 8)  & 0xFF) / 255.0f;
    float b = (float)(col & 0xFF)         / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b));
    float mn = fminf(r, fminf(g, b));
    float l = (mx + mn) * 0.5f;
    float d = mx - mn;
    if (d < 0.001f) return col;
    float s = (l > 0.5f) ? (d / (2.0f - mx - mn)) : (d / (mx + mn));

    float h = fmodf(hue_deg, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = l - c * 0.5f;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60.0f)       { rr = c; gg = x; }
    else if (h < 120.0f) { rr = x; gg = c; }
    else if (h < 180.0f) { gg = c; bb = x; }
    else if (h < 240.0f) { gg = x; bb = c; }
    else if (h < 300.0f) { rr = x; bb = c; }
    else                 { rr = c; bb = x; }

    uint32_t r8 = (uint32_t)fminf(255.0f, fmaxf(0.0f, roundf((rr + m) * 255.0f)));
    uint32_t g8 = (uint32_t)fminf(255.0f, fmaxf(0.0f, roundf((gg + m) * 255.0f)));
    uint32_t b8 = (uint32_t)fminf(255.0f, fmaxf(0.0f, roundf((bb + m) * 255.0f)));
    return a | (r8 << 16) | (g8 << 8) | b8;
}

static float extract_wallpaper_hue(const uint32_t *sharp)
{
    if (!sharp) return 283.0f;
    double sum_cos = 0.0, sum_sin = 0.0, total_w = 0.0;
    for (int y = 0; y < UI_H; y += 6) {
        const uint32_t *row = sharp + y * UI_W;
        for (int x = 0; x < UI_W; x += 6) {
            uint32_t p = row[x];
            int r = (int)((p >> 16) & 0xFF);
            int g = (int)((p >> 8)  & 0xFF);
            int b = (int)(p & 0xFF);
            int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            int d = mx - mn;
            if (d < 20 || mx < 24) continue;
            float h;
            if (mx == r)      h = 60.0f * (float)(g - b) / (float)d;
            else if (mx == g) h = 60.0f * (2.0f + (float)(b - r) / (float)d);
            else              h = 60.0f * (4.0f + (float)(r - g) / (float)d);
            if (h < 0.0f) h += 360.0f;
            double rad = (double)h * (3.14159265358979 / 180.0);
            double w = (double)d * (double)d / (double)mx;
            sum_cos += cos(rad) * w;
            sum_sin += sin(rad) * w;
            total_w += w;
        }
    }
    if (total_w < 1.0) return 283.0f;
    double deg = atan2(sum_sin, sum_cos) * (180.0 / 3.14159265358979);
    if (deg < 0.0) deg += 360.0;
    return (float)deg;
}

static uint64_t wp_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void update_active_palette(void)
{
    if (g_wp_sharp && g_wp_blur) {
        const struct ui_palette *base = g_dark_enabled ? &k_pal_glass_dark : &k_pal_glass_light;
        g_dyn_pal = *base;
        g_dyn_pal.accent      = shift_color_hue(base->accent, g_wp_hue_curr);
        g_dyn_pal.accent_text = shift_color_hue(base->accent_text, g_wp_hue_curr);
        g_pal = &g_dyn_pal;
    } else {
        g_pal = g_dark_enabled ? &k_pal_dark : &k_pal_light;
    }
}

void gfx_set_dark_mode(bool dark)
{
    g_dark_enabled = dark;
    update_active_palette();
}

void gfx_set_text_shadow(bool enabled)
{
    g_text_shadow = enabled;
}

struct font_slot {
    unsigned char *ttf_data;
    stbtt_fontinfo info;
    bool loaded;
};

static struct font_slot g_fonts[2];
#define MAX_FALLBACK_FONTS 4
static struct font_slot g_fallback_fonts[MAX_FALLBACK_FONTS];
static size_t           g_fallback_map_sz[MAX_FALLBACK_FONTS];
static int              g_fallback_count = 0;
static bool g_custom_font_active = false;

#define CUSTOM_FONT_CONF "/mnt/sdcard/.system/meui_font.conf"

#define GLYPH_CACHE_BITS 11
#define GLYPH_CACHE_SIZE (1 << GLYPH_CACHE_BITS)
#define GLYPH_CACHE_MASK (GLYPH_CACHE_SIZE - 1)

struct glyph_entry {
    uint32_t codepoint;
    uint16_t size_x10;
    uint8_t  weight;
    bool     valid;
    int      w, h;
    int      xoff, yoff;
    int      advance;
    uint8_t *alpha;
};

static struct glyph_entry g_glyphs[GLYPH_CACHE_SIZE];

static void clear_glyph_cache(void)
{
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (g_glyphs[i].alpha) {
            free(g_glyphs[i].alpha);
            g_glyphs[i].alpha = NULL;
        }
        g_glyphs[i].valid = false;
    }
}

#define ICON_CACHE_SIZE 32
struct icon_entry {
    char     name[48];
    int      size;
    bool     valid;
    uint8_t *alpha;
};

static struct icon_entry g_icons[ICON_CACHE_SIZE];
static NSVGrasterizer *g_svg_rast = NULL;

static unsigned char *read_entire_file(const char *path, size_t *out_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 24 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_sz) *out_sz = rd;
    return buf;
}

static void free_fonts(void)
{
    if (g_fonts[UI_FONT_SEMIBOLD].ttf_data &&
        g_fonts[UI_FONT_SEMIBOLD].ttf_data != g_fonts[UI_FONT_REGULAR].ttf_data) {
        free(g_fonts[UI_FONT_SEMIBOLD].ttf_data);
    }
    if (g_fonts[UI_FONT_REGULAR].ttf_data) {
        free(g_fonts[UI_FONT_REGULAR].ttf_data);
    }
    memset(g_fonts, 0, sizeof(g_fonts));
}

static void load_fallback_fonts(void)
{
    static const char * const fb_names[MAX_FALLBACK_FONTS] = {
        "NotoSans-Regular.ttf",
        "NotoSansJP-Regular.ttf",
        "NotoSansSC-Regular.ttf",
        "NotoSansKR-Regular.ttf"
    };
    static const char * const roots[] = {
        "/mnt/sdcard/.meui/fonts",
        "/.meui/fonts",
        "/mnt/sdcard/.system/configapp/fonts",
        "/.system/configapp/fonts",
        NULL
    };
    g_fallback_count = 0;
    for (int f = 0; f < MAX_FALLBACK_FONTS; f++) {
        for (int r = 0; roots[r]; r++) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", roots[r], fb_names[f]);
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size > 128) {
                void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
                if (map != MAP_FAILED) {
                    const unsigned char *data = (const unsigned char *)map;
                    if (stbtt_InitFont(&g_fallback_fonts[g_fallback_count].info, data,
                                       stbtt_GetFontOffsetForIndex(data, 0))) {
                        g_fallback_fonts[g_fallback_count].ttf_data = (unsigned char *)map;
                        g_fallback_fonts[g_fallback_count].loaded = true;
                        g_fallback_map_sz[g_fallback_count] = (size_t)st.st_size;
                        g_fallback_count++;
                        break;
                    }
                    munmap(map, (size_t)st.st_size);
                }
            } else {
                close(fd);
            }
        }
    }
}

static void free_fallback_fonts(void)
{
    for (int i = 0; i < g_fallback_count; i++) {
        if (g_fallback_fonts[i].ttf_data && g_fallback_map_sz[i] > 0)
            munmap(g_fallback_fonts[i].ttf_data, g_fallback_map_sz[i]);
    }
    memset(g_fallback_fonts, 0, sizeof(g_fallback_fonts));
    memset(g_fallback_map_sz, 0, sizeof(g_fallback_map_sz));
    g_fallback_count = 0;
}

static bool load_font_slot(enum ui_font_weight w, const char *filename)
{
    char path[256];
    const char *roots[] = {
        "/mnt/sdcard/.meui/fonts",
        "/.meui/fonts",
        "/mnt/sdcard/.system/configapp/fonts",
        "/.system/configapp/fonts",
        NULL
    };
    for (int i = 0; roots[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", roots[i], filename);
        size_t sz = 0;
        unsigned char *data = read_entire_file(path, &sz);
        if (data) {
            if (stbtt_InitFont(&g_fonts[w].info, data,
                               stbtt_GetFontOffsetForIndex(data, 0))) {
                g_fonts[w].ttf_data = data;
                g_fonts[w].loaded = true;
                return true;
            }
            free(data);
        }
    }
    return false;
}

static bool load_custom_font_file(const char *path)
{
    if (!path || !*path) return false;
    size_t sz = 0;
    unsigned char *data = read_entire_file(path, &sz);
    if (!data) return false;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        free(data);
        return false;
    }
    clear_glyph_cache();
    free_fonts();
    g_fonts[UI_FONT_REGULAR].ttf_data = data;
    g_fonts[UI_FONT_REGULAR].info = info;
    g_fonts[UI_FONT_REGULAR].loaded = true;
    g_fonts[UI_FONT_SEMIBOLD] = g_fonts[UI_FONT_REGULAR];
    g_custom_font_active = true;
    return true;
}

static void load_default_fonts(void)
{
    clear_glyph_cache();
    free_fonts();
    load_font_slot(UI_FONT_REGULAR, "WantedSans-Regular.ttf");
    if (!load_font_slot(UI_FONT_SEMIBOLD, "WantedSans-SemiBold.ttf")) {
        if (g_fonts[UI_FONT_REGULAR].loaded)
            g_fonts[UI_FONT_SEMIBOLD] = g_fonts[UI_FONT_REGULAR];
    }
    g_custom_font_active = false;
}

bool gfx_set_custom_font(const char *path)
{
    if (!load_custom_font_file(path))
        return false;
    mkdir("/mnt/sdcard/.system", 0777);
    FILE *f = fopen(CUSTOM_FONT_CONF, "w");
    if (f) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
    return true;
}

void gfx_reset_default_font(void)
{
    unlink(CUSTOM_FONT_CONF);
    load_default_fonts();
}

bool gfx_has_custom_font(void)
{
    return g_custom_font_active;
}

#define WP_SHARP_PATH "/mnt/sdcard/.system/meui_wallpaper.raw"
#define WP_BLUR_PATH  "/mnt/sdcard/.system/meui_wallpaper_blur.raw"

static void load_saved_wallpaper(void)
{
    size_t bytes = (size_t)UI_W * (size_t)UI_H * 4;
    FILE *fs = fopen(WP_SHARP_PATH, "rb");
    FILE *fb_f = fopen(WP_BLUR_PATH, "rb");
    if (!fs || !fb_f) {
        if (fs) fclose(fs);
        if (fb_f) fclose(fb_f);
        return;
    }
    uint32_t *sh = malloc(bytes);
    uint32_t *bl = malloc(bytes);
    if (sh && bl && fread(sh, 1, bytes, fs) == bytes && fread(bl, 1, bytes, fb_f) == bytes) {
        g_wp_sharp = sh;
        g_wp_blur = bl;
        g_wp_hue_curr = g_wp_hue_target = g_wp_hue_prev = extract_wallpaper_hue(sh);
    } else {
        free(sh);
        free(bl);
    }
    fclose(fs);
    fclose(fb_f);
    update_active_palette();
}

static void box_blur_h(const uint32_t *src, uint32_t *dst, int w, int h, int rad)
{
    int diam = rad * 2 + 1;
    for (int y = 0; y < h; y++) {
        const uint32_t *srow = src + y * w;
        uint32_t *drow = dst + y * w;
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -rad; k <= rad; k++) {
            int sx = k < 0 ? 0 : (k >= w ? w - 1 : k);
            uint32_t px = srow[sx];
            sr += (px >> 16) & 0xFF;
            sg += (px >> 8) & 0xFF;
            sb += px & 0xFF;
        }
        for (int x = 0; x < w; x++) {
            drow[x] = 0xFF000000U | ((sr / (uint32_t)diam) << 16) |
                      ((sg / (uint32_t)diam) << 8) | (sb / (uint32_t)diam);
            int x_out = x - rad; if (x_out < 0) x_out = 0;
            int x_in = x + rad + 1; if (x_in >= w) x_in = w - 1;
            uint32_t po = srow[x_out], pi = srow[x_in];
            sr += ((pi >> 16) & 0xFF) - ((po >> 16) & 0xFF);
            sg += ((pi >> 8) & 0xFF) - ((po >> 8) & 0xFF);
            sb += (pi & 0xFF) - (po & 0xFF);
        }
    }
}

static void box_blur_v(const uint32_t *src, uint32_t *dst, int w, int h, int rad)
{
    int diam = rad * 2 + 1;
    for (int x = 0; x < w; x++) {
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -rad; k <= rad; k++) {
            int sy = k < 0 ? 0 : (k >= h ? h - 1 : k);
            uint32_t px = src[sy * w + x];
            sr += (px >> 16) & 0xFF;
            sg += (px >> 8) & 0xFF;
            sb += px & 0xFF;
        }
        for (int y = 0; y < h; y++) {
            dst[y * w + x] = 0xFF000000U | ((sr / (uint32_t)diam) << 16) |
                             ((sg / (uint32_t)diam) << 8) | (sb / (uint32_t)diam);
            int y_out = y - rad; if (y_out < 0) y_out = 0;
            int y_in = y + rad + 1; if (y_in >= h) y_in = h - 1;
            uint32_t po = src[y_out * w + x], pi = src[y_in * w + x];
            sr += ((pi >> 16) & 0xFF) - ((po >> 16) & 0xFF);
            sg += ((pi >> 8) & 0xFF) - ((po >> 8) & 0xFF);
            sb += (pi & 0xFF) - (po & 0xFF);
        }
    }
}

bool gfx_has_wallpaper(void)
{
    return (g_wp_sharp != NULL && g_wp_blur != NULL);
}

bool gfx_set_wallpaper_from_image(const char *path)
{
    if (!path || !*path) return false;
    size_t fsz = 0;
    unsigned char *fbuf = read_entire_file(path, &fsz);
    if (!fbuf) return false;
    int sw = 0, sh = 0, ch = 0;
    unsigned char *img = stbi_load_from_memory(fbuf, (int)fsz, &sw, &sh, &ch, 4);
    free(fbuf);
    if (!img || sw <= 0 || sh <= 0) {
        if (img) stbi_image_free(img);
        return false;
    }

    size_t bytes = (size_t)UI_W * (size_t)UI_H * 4;
    uint32_t *sharp = malloc(bytes);
    uint32_t *blur  = malloc(bytes);
    uint32_t *tmp   = malloc(bytes);
    if (!sharp || !blur || !tmp) {
        free(sharp); free(blur); free(tmp);
        stbi_image_free(img);
        return false;
    }

    float scale_x = (float)sw / (float)UI_W;
    float scale_y = (float)sh / (float)UI_H;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    float crop_w = (float)UI_W * scale;
    float crop_h = (float)UI_H * scale;
    float off_x = ((float)sw - crop_w) * 0.5f;
    float off_y = ((float)sh - crop_h) * 0.5f;

    for (int y = 0; y < UI_H; y++) {
        float sy = off_y + ((float)y + 0.5f) * scale - 0.5f;
        if (sy < 0.0f) sy = 0.0f;
        int y0 = (int)sy;
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        float fy = sy - (float)y0;
        for (int x = 0; x < UI_W; x++) {
            float sx = off_x + ((float)x + 0.5f) * scale - 0.5f;
            if (sx < 0.0f) sx = 0.0f;
            int x0 = (int)sx;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            float fx = sx - (float)x0;
            const uint8_t *p00 = img + (y0 * sw + x0) * 4;
            const uint8_t *p10 = img + (y0 * sw + x1) * 4;
            const uint8_t *p01 = img + (y1 * sw + x0) * 4;
            const uint8_t *p11 = img + (y1 * sw + x1) * 4;
            uint32_t out_c[3];
            for (int c = 0; c < 3; c++) {
                float top = (float)p00[c] * (1.0f - fx) + (float)p10[c] * fx;
                float bot = (float)p01[c] * (1.0f - fx) + (float)p11[c] * fx;
                out_c[c] = (uint32_t)(top * (1.0f - fy) + bot * fy);
                if (out_c[c] > 255) out_c[c] = 255;
            }
            sharp[y * UI_W + x] = 0xFF000000U | (out_c[0] << 16) | (out_c[1] << 8) | out_c[2];
        }
    }
    stbi_image_free(img);

    memcpy(blur, sharp, bytes);
    for (int pass = 0; pass < 3; pass++) {
        box_blur_h(blur, tmp, UI_W, UI_H, 10);
        box_blur_v(tmp, blur, UI_W, UI_H, 10);
    }
    free(tmp);

    mkdir("/mnt/sdcard/.system", 0777);
    FILE *fs = fopen(WP_SHARP_PATH, "wb");
    if (fs) { fwrite(sharp, 1, bytes, fs); fclose(fs); }
    FILE *fb_f = fopen(WP_BLUR_PATH, "wb");
    if (fb_f) { fwrite(blur, 1, bytes, fb_f); fclose(fb_f); }

    free(g_wp_sharp);
    free(g_wp_blur);
    free(g_wp_prev_sharp);
    free(g_wp_prev_blur);
    g_wp_sharp = sharp;
    g_wp_blur = blur;
    g_wp_prev_sharp = NULL;
    g_wp_prev_blur = NULL;
    g_wp_fade_active = false;
    g_wp_hue_curr = g_wp_hue_target = g_wp_hue_prev = extract_wallpaper_hue(sharp);
    update_active_palette();
    return true;
}

void gfx_set_wallpaper_rgb565(const uint16_t *rgb565, int w, int h, bool crossfade_1s)
{
    uint32_t *sharp = NULL;
    uint32_t *blur = NULL;
    size_t bytes = (size_t)UI_W * UI_H * 4;

    if (rgb565 && w > 0 && h > 0) {
        sharp = malloc(bytes);
        blur = malloc(bytes);
        uint32_t *tmp = malloc(bytes);
        if (sharp && blur && tmp) {
            float sx_r = (float)w / (float)UI_W;
            float sy_r = (float)h / (float)UI_H;
            float scale = (sx_r < sy_r) ? sx_r : sy_r;
            float src_w_used = (float)UI_W * scale;
            float src_h_used = (float)UI_H * scale;
            float off_x = ((float)w - src_w_used) * 0.5f;
            float off_y = ((float)h - src_h_used) * 0.5f;

            for (int y = 0; y < UI_H; y++) {
                float sy = off_y + ((float)y + 0.5f) * scale - 0.5f;
                if (sy < 0.0f) sy = 0.0f;
                int y0 = (int)sy;
                if (y0 >= h) y0 = h - 1;
                int y1 = y0 + 1 < h ? y0 + 1 : h - 1;
                float fy = sy - (float)y0;
                for (int x = 0; x < UI_W; x++) {
                    float sx = off_x + ((float)x + 0.5f) * scale - 0.5f;
                    if (sx < 0.0f) sx = 0.0f;
                    int x0 = (int)sx;
                    if (x0 >= w) x0 = w - 1;
                    int x1 = x0 + 1 < w ? x0 + 1 : w - 1;
                    float fx = sx - (float)x0;
                    uint16_t c00 = rgb565[y0 * w + x0];
                    uint16_t c10 = rgb565[y0 * w + x1];
                    uint16_t c01 = rgb565[y1 * w + x0];
                    uint16_t c11 = rgb565[y1 * w + x1];
                    float w00 = (1.0f - fx) * (1.0f - fy);
                    float w10 = fx * (1.0f - fy);
                    float w01 = (1.0f - fx) * fy;
                    float w11 = fx * fy;
                    float rf = ((c00 >> 11) & 0x1F) * w00 + ((c10 >> 11) & 0x1F) * w10 +
                               ((c01 >> 11) & 0x1F) * w01 + ((c11 >> 11) & 0x1F) * w11;
                    float gf = ((c00 >> 5) & 0x3F) * w00 + ((c10 >> 5) & 0x3F) * w10 +
                               ((c01 >> 5) & 0x3F) * w01 + ((c11 >> 5) & 0x3F) * w11;
                    float bf = (c00 & 0x1F) * w00 + (c10 & 0x1F) * w10 +
                               (c01 & 0x1F) * w01 + (c11 & 0x1F) * w11;
                    uint32_t r = (uint32_t)(rf * (255.0f / 31.0f)); if (r > 255) r = 255;
                    uint32_t g = (uint32_t)(gf * (255.0f / 63.0f)); if (g > 255) g = 255;
                    uint32_t b = (uint32_t)(bf * (255.0f / 31.0f)); if (b > 255) b = 255;
                    sharp[y * UI_W + x] = 0xFF000000U | (r << 16) | (g << 8) | b;
                }
            }
            memcpy(blur, sharp, bytes);
            for (int pass = 0; pass < 3; pass++) {
                box_blur_h(blur, tmp, UI_W, UI_H, 10);
                box_blur_v(tmp, blur, UI_W, UI_H, 10);
            }
            free(tmp);
        } else {
            free(sharp); free(blur); free(tmp);
            sharp = NULL; blur = NULL;
        }
    }

    free(g_wp_prev_sharp);
    free(g_wp_prev_blur);
    g_wp_prev_sharp = NULL;
    g_wp_prev_blur = NULL;
    g_wp_fade_active = false;

    if (crossfade_1s) {
        if (g_wp_sharp && g_wp_blur) {
            g_wp_prev_sharp = g_wp_sharp;
            g_wp_prev_blur = g_wp_blur;
            g_wp_fade_start_ms = wp_now_ms();
            g_wp_fade_active = true;
        } else if (sharp && blur) {
            g_wp_fade_start_ms = wp_now_ms();
            g_wp_fade_active = true;
        }
    } else {
        free(g_wp_sharp);
        free(g_wp_blur);
    }

    g_wp_sharp = sharp;
    g_wp_blur = blur;
    g_wp_hue_prev = g_wp_hue_curr;
    g_wp_hue_target = extract_wallpaper_hue(sharp);
    if (!g_wp_fade_active)
        g_wp_hue_curr = g_wp_hue_target;
    update_active_palette();
}

void gfx_clear_wallpaper(void)
{
    free(g_wp_sharp);
    free(g_wp_blur);
    free(g_wp_prev_sharp);
    free(g_wp_prev_blur);
    g_wp_sharp = NULL;
    g_wp_blur = NULL;
    g_wp_prev_sharp = NULL;
    g_wp_prev_blur = NULL;
    g_wp_fade_active = false;
    unlink(WP_SHARP_PATH);
    unlink(WP_BLUR_PATH);
    update_active_palette();
}

bool gfx_wallpaper_is_fading(void)
{
    return g_wp_fade_active;
}

static inline uint32_t sample_wp_pair(const uint32_t *sh, const uint32_t *bl, int i,
                                      uint32_t a, uint32_t inv, uint32_t bg_c)
{
    if (!sh || !bl) return bg_c;
    if (a <= 2) return sh[i];
    if (a >= 253) return bl[i];
    uint32_t s = sh[i], b = bl[i];
    uint32_t rb = ((((s & 0x00FF00FFU) * inv) + ((b & 0x00FF00FFU) * a)) >> 8) & 0x00FF00FFU;
    uint32_t g  = ((((s & 0x0000FF00U) * inv) + ((b & 0x0000FF00U) * a)) >> 8) & 0x0000FF00U;
    return 0xFF000000U | rb | g;
}

void gfx_draw_wallpaper(float blur_t)
{
    int n = UI_W * UI_H;
    uint32_t bg_c = COL_BG | 0xFF000000U;
    uint32_t ba = (blur_t <= 0.01f) ? 0 : ((blur_t >= 0.99f) ? 256 : (uint32_t)(blur_t * 256.0f));
    uint32_t binv = 256 - ba;
    g_wp_is_unblurred = (g_wp_blur != NULL && blur_t < 0.99f);
    g_wp_fade_fa = 256;

    if (g_wp_fade_active) {
        uint64_t el = wp_now_ms() - g_wp_fade_start_ms;
        if (el >= 1000ULL) {
            free(g_wp_prev_sharp);
            free(g_wp_prev_blur);
            g_wp_prev_sharp = NULL;
            g_wp_prev_blur = NULL;
            g_wp_fade_active = false;
            g_wp_hue_curr = g_wp_hue_target;
            update_active_palette();
        } else {
            float ft = (float)el / 1000.0f;
            float e = ft * ft * (3.0f - 2.0f * ft);
            float dh = fmodf(g_wp_hue_target - g_wp_hue_prev + 540.0f, 360.0f) - 180.0f;
            g_wp_hue_curr = g_wp_hue_prev + dh * e;
            update_active_palette();
            uint32_t fa = (uint32_t)(e * 256.0f);
            uint32_t finv = 256 - fa;
            g_wp_fade_fa = fa;
            const uint32_t *psh = g_wp_prev_sharp;
            const uint32_t *pbl = g_wp_prev_blur;
            const uint32_t *csh = g_wp_sharp;
            const uint32_t *cbl = g_wp_blur;
            for (int i = 0; i < n; i++) {
                uint32_t pc = sample_wp_pair(psh, pbl, i, ba, binv, bg_c);
                uint32_t cc = sample_wp_pair(csh, cbl, i, ba, binv, bg_c);
                uint32_t rb = ((((pc & 0x00FF00FFU) * finv) + ((cc & 0x00FF00FFU) * fa)) >> 8) & 0x00FF00FFU;
                uint32_t g  = ((((pc & 0x0000FF00U) * finv) + ((cc & 0x0000FF00U) * fa)) >> 8) & 0x0000FF00U;
                fb[i] = 0xFF000000U | rb | g;
            }
            return;
        }
    }

    if (!g_wp_sharp || !g_wp_blur) {
        gfx_clear(COL_BG);
        return;
    }
    if (ba == 0) {
        memcpy(fb, g_wp_sharp, (size_t)n * 4);
        return;
    }
    if (ba == 256) {
        memcpy(fb, g_wp_blur, (size_t)n * 4);
        return;
    }
    const uint32_t *sh = g_wp_sharp;
    const uint32_t *bl = g_wp_blur;
    for (int i = 0; i < n; i++) {
        uint32_t s = sh[i];
        uint32_t b = bl[i];
        uint32_t rb = ((((s & 0x00FF00FFU) * binv) + ((b & 0x00FF00FFU) * ba)) >> 8) & 0x00FF00FFU;
        uint32_t g  = ((((s & 0x0000FF00U) * binv) + ((b & 0x0000FF00U) * ba)) >> 8) & 0x0000FF00U;
        fb[i] = 0xFF000000U | rb | g;
    }
}

bool gfx_init(void)
{
    init_rgb565_lut();
    memset(g_fonts, 0, sizeof(g_fonts));
    memset(g_glyphs, 0, sizeof(g_glyphs));
    memset(g_icons, 0, sizeof(g_icons));

    load_saved_wallpaper();

    bool font_loaded = false;
    FILE *ff = fopen(CUSTOM_FONT_CONF, "r");
    if (ff) {
        char fpath[384] = "";
        if (fgets(fpath, sizeof(fpath), ff)) {
            size_t n = strlen(fpath);
            while (n > 0 && (fpath[n - 1] == '\n' || fpath[n - 1] == '\r')) fpath[--n] = '\0';
            if (fpath[0])
                font_loaded = load_custom_font_file(fpath);
        }
        fclose(ff);
    }
    if (!font_loaded)
        load_default_fonts();
    load_fallback_fonts();

    g_svg_rast = nsvgCreateRasterizer();
    return g_fonts[UI_FONT_REGULAR].loaded;
}

void gfx_shutdown(void)
{
    clear_glyph_cache();
    for (int i = 0; i < ICON_CACHE_SIZE; i++) {
        if (g_icons[i].alpha) {
            free(g_icons[i].alpha);
            g_icons[i].alpha = NULL;
        }
    }
    if (g_svg_rast) {
        nsvgDeleteRasterizer(g_svg_rast);
        g_svg_rast = NULL;
    }
    free_fonts();
    free_fallback_fonts();
    free(g_wp_sharp);
    free(g_wp_blur);
    free(g_wp_prev_sharp);
    free(g_wp_prev_blur);
    g_wp_sharp = NULL;
    g_wp_blur = NULL;
    g_wp_prev_sharp = NULL;
    g_wp_prev_blur = NULL;
    g_wp_fade_active = false;
}

static inline uint32_t div255(uint32_t v)
{
    return (v + 1 + (v >> 8)) >> 8;
}

static inline void blend_pixel(int x, int y, uint32_t col, uint8_t alpha)
{
    if ((unsigned)x >= UI_W || y < g_clip_y0 || y >= g_clip_y1 || alpha == 0)
        return;
    if (alpha == 255) {
        fb[y * UI_W + x] = col | 0xFF000000U;
        return;
    }
    uint32_t dst = fb[y * UI_W + x];
    uint32_t inv = 255 - alpha;
    uint32_t r = div255((((col >> 16) & 0xFF) * alpha) + (((dst >> 16) & 0xFF) * inv));
    uint32_t g = div255((((col >> 8) & 0xFF) * alpha) + (((dst >> 8) & 0xFF) * inv));
    uint32_t b = div255(((col & 0xFF) * alpha) + ((dst & 0xFF) * inv));
    fb[y * UI_W + x] = 0xFF000000U | (r << 16) | (g << 8) | b;
}

static inline void blend_span_fast(uint32_t *row, int x0, int x1, uint32_t col, uint8_t alpha)
{
    if (x0 >= x1 || alpha == 0) return;
    if (alpha == 255) {
        uint32_t c = col | 0xFF000000U;
        for (int px = x0; px < x1; px++) row[px] = c;
        return;
    }
    uint32_t sr = ((col >> 16) & 0xFF) * alpha;
    uint32_t sg = ((col >> 8) & 0xFF) * alpha;
    uint32_t sb = (col & 0xFF) * alpha;
    uint32_t inv = 255 - alpha;
    if (g_wp_is_unblurred && g_wp_blur) {
        ptrdiff_t row_off = row - fb;
        if (g_wp_fade_active && g_wp_prev_blur) {
            uint32_t fa = g_wp_fade_fa, finv = 256 - fa;
            const uint32_t *pb = g_wp_prev_blur + row_off;
            const uint32_t *cb = g_wp_blur + row_off;
            for (int px = x0; px < x1; px++) {
                uint32_t p = pb[px], c = cb[px];
                uint32_t rb = ((((p & 0x00FF00FFU) * finv) + ((c & 0x00FF00FFU) * fa)) >> 8) & 0x00FF00FFU;
                uint32_t g0 = ((((p & 0x0000FF00U) * finv) + ((c & 0x0000FF00U) * fa)) >> 8) & 0x0000FF00U;
                uint32_t dst = 0xFF000000U | rb | g0;
                uint32_t r = div255(sr + (((dst >> 16) & 0xFF) * inv));
                uint32_t g = div255(sg + (((dst >> 8) & 0xFF) * inv));
                uint32_t b = div255(sb + ((dst & 0xFF) * inv));
                row[px] = 0xFF000000U | (r << 16) | (g << 8) | b;
            }
        } else {
            const uint32_t *brow = g_wp_blur + row_off;
            for (int px = x0; px < x1; px++) {
                uint32_t dst = brow[px];
                uint32_t r = div255(sr + (((dst >> 16) & 0xFF) * inv));
                uint32_t g = div255(sg + (((dst >> 8) & 0xFF) * inv));
                uint32_t b = div255(sb + ((dst & 0xFF) * inv));
                row[px] = 0xFF000000U | (r << 16) | (g << 8) | b;
            }
        }
        return;
    }
    for (int px = x0; px < x1; px++) {
        uint32_t dst = row[px];
        uint32_t r = div255(sr + (((dst >> 16) & 0xFF) * inv));
        uint32_t g = div255(sg + (((dst >> 8) & 0xFF) * inv));
        uint32_t b = div255(sb + ((dst & 0xFF) * inv));
        row[px] = 0xFF000000U | (r << 16) | (g << 8) | b;
    }
}

void gfx_clear(uint32_t color)
{
    uint32_t c = color | 0xFF000000U;
    for (int i = 0; i < UI_W * UI_H; i++)
        fb[i] = c;
}

void gfx_present(void)
{
    y1_fb_present_xrgb8888(fb);
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    uint8_t a = (uint8_t)((color >> 24) & 0xFF);
    if (a == 0) a = 255;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < g_clip_y0 ? g_clip_y0 : y;
    int x1 = (x + w > UI_W) ? UI_W : x + w;
    int y1 = (y + h > g_clip_y1) ? g_clip_y1 : y + h;
    for (int py = y0; py < y1; py++)
        blend_span_fast(fb + py * UI_W, x0, x1, color, a);
}

static float corner_inset_f(int r, float exp_p, float dy)
{
    if (dy <= 0.0f || r <= 0) return 0.0f;
    if (dy >= (float)r) return (float)r;
    if (exp_p <= 2.01f) {
        float rr = (float)r;
        return rr - sqrtf(rr * rr - dy * dy);
    }
    float v = dy / (float)r;
    float vn = powf(v, exp_p);
    float rem = 1.0f - vn;
    float x_lim = (rem > 0.0f) ? (float)r * powf(rem, 1.0f / exp_p) : 0.0f;
    return (float)r - x_lim;
}

#define CORNER_CACHE_MAX 64
struct corner_cache_entry {
    int   r;
    int   exp_x10;
    bool  valid;
    float insets[65];
};
static struct corner_cache_entry g_corner_cache[CORNER_CACHE_MAX];
static int g_corner_cache_next = 0;

static int calc_corner_exp_x10(int r, int w, int h)
{
    int min_d = (w < h) ? w : h;
    if (min_d <= 0 || r * 2 >= min_d) return 20;
    float ratio = (float)(r * 2) / (float)min_d;
    if (ratio <= 0.5f) return 32;
    float t = (ratio - 0.5f) * 2.0f;
    int px10 = (int)roundf((3.2f - 1.2f * t) * 10.0f);
    if (px10 < 20) px10 = 20;
    if (px10 > 32) px10 = 32;
    return px10;
}

static const float *get_corner_insets_aa(int r, int exp_x10)
{
    for (int i = 0; i < CORNER_CACHE_MAX; i++) {
        if (g_corner_cache[i].valid && g_corner_cache[i].r == r && g_corner_cache[i].exp_x10 == exp_x10)
            return g_corner_cache[i].insets;
    }
    struct corner_cache_entry *e = &g_corner_cache[g_corner_cache_next];
    g_corner_cache_next = (g_corner_cache_next + 1) % CORNER_CACHE_MAX;
    e->r = r;
    e->exp_x10 = exp_x10;
    e->valid = true;
    float exp_p = (float)exp_x10 * 0.1f;
    for (int dy = 0; dy < r; dy++) {
        float sum = 0.0f;
        for (int s = 0; s < 4; s++) {
            float sub_dy = (float)dy + (0.125f + 0.25f * (float)s);
            sum += corner_inset_f(r, exp_p, sub_dy);
        }
        e->insets[dy] = sum * 0.25f;
    }
    return e->insets;
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color)
{
    if (w <= 0 || h <= 0) return;
    int exp_x10 = calc_corner_exp_x10(r, w, h);
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r > 64) r = 64;
    uint8_t a = (uint8_t)((color >> 24) & 0xFF);
    if (a == 0) a = 255;

    const float *insets = (r > 0) ? get_corner_insets_aa(r, exp_x10) : NULL;

    int py0 = y < g_clip_y0 ? g_clip_y0 : y;
    int py1 = (y + h) > g_clip_y1 ? g_clip_y1 : (y + h);
    for (int py = py0; py < py1; py++) {
        int dy = -1;
        if (py < y + r) dy = (y + r - 1) - py;
        else if (py >= y + h - r) dy = py - (y + h - r);

        float ins_f = (insets && dy >= 0 && dy < r) ? insets[dy] : 0.0f;
        int ins_i = (int)ins_f;
        float frac = ins_f - (float)ins_i;
        uint8_t edge_a = (uint8_t)((1.0f - frac) * (float)a);

        int rx0 = x + ins_i;
        int rx1 = x + w - ins_i;
        if (ins_f > 0.0f && edge_a > 0) {
            if (rx0 >= 0 && rx0 < UI_W) blend_pixel(rx0, py, color, edge_a);
            if (rx1 - 1 >= 0 && rx1 - 1 < UI_W && rx1 - 1 != rx0) blend_pixel(rx1 - 1, py, color, edge_a);
            rx0++;
            rx1--;
        }

        if (rx0 < 0) rx0 = 0;
        if (rx1 > UI_W) rx1 = UI_W;
        blend_span_fast(fb + py * UI_W, rx0, rx1, color, a);
    }
}

void gfx_stroke_round_rect(int x, int y, int w, int h, int r, int thickness, uint32_t color)
{
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    int exp_x10 = calc_corner_exp_x10(r, w, h);
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r > 64) r = 64;
    uint8_t a = (uint8_t)((color >> 24) & 0xFF);
    if (a == 0) a = 255;

    const float *outer = (r > 0) ? get_corner_insets_aa(r, exp_x10) : NULL;
    int ir = r - thickness;
    const float *inner = (ir > 0) ? get_corner_insets_aa(ir, exp_x10) : NULL;

    int py0 = y < g_clip_y0 ? g_clip_y0 : y;
    int py1 = (y + h) > g_clip_y1 ? g_clip_y1 : (y + h);
    for (int py = py0; py < py1; py++) {
        int dy = -1;
        if (py < y + r) dy = (y + r - 1) - py;
        else if (py >= y + h - r) dy = py - (y + h - r);

        float o_f = (outer && dy >= 0 && dy < r) ? outer[dy] : 0.0f;
        int o_i = (int)o_f;
        uint8_t o_ea = (uint8_t)((1.0f - (o_f - (float)o_i)) * (float)a);
        int ox0 = x + o_i;
        int ox1 = x + w - o_i;
        uint32_t *row = fb + py * UI_W;

        bool top_bot = (py < y + thickness || py >= y + h - thickness);
        if (top_bot) {
            if (o_f > 0.0f && o_ea > 0) {
                if (ox0 >= 0 && ox0 < UI_W) blend_pixel(ox0, py, color, o_ea);
                if (ox1 - 1 >= 0 && ox1 - 1 < UI_W && ox1 - 1 != ox0) blend_pixel(ox1 - 1, py, color, o_ea);
                ox0++; ox1--;
            }
            if (ox0 < 0) ox0 = 0;
            if (ox1 > UI_W) ox1 = UI_W;
            blend_span_fast(row, ox0, ox1, color, a);
        } else {
            int idy = -1;
            int iy = y + thickness;
            int ih = h - thickness * 2;
            if (ir > 0) {
                if (py < iy + ir) idy = (iy + ir - 1) - py;
                else if (py >= iy + ih - ir) idy = py - (iy + ih - ir);
            }
            float i_f = (inner && idy >= 0 && idy < ir) ? inner[idy] : 0.0f;
            int i_i = (int)i_f;
            uint8_t i_ea = (uint8_t)((i_f - (float)i_i) * (float)a);
            int ix0 = x + thickness + i_i;
            int ix1 = x + w - thickness - i_i;

            if (o_f > 0.0f && o_ea > 0) {
                if (ox0 >= 0 && ox0 < UI_W) blend_pixel(ox0, py, color, o_ea);
                if (ox1 - 1 >= 0 && ox1 - 1 < UI_W && ox1 - 1 != ox0) blend_pixel(ox1 - 1, py, color, o_ea);
                ox0++; ox1--;
            }
            int lx0 = ox0 < 0 ? 0 : ox0;
            int lx1 = ix0 > UI_W ? UI_W : ix0;
            blend_span_fast(row, lx0, lx1, color, a);
            int rx0 = ix1 < 0 ? 0 : ix1;
            int rx1 = ox1 > UI_W ? UI_W : ox1;
            blend_span_fast(row, rx0, rx1, color, a);
            if (i_f > 0.0f && i_ea > 0) {
                if (ix0 >= 0 && ix0 < UI_W) blend_pixel(ix0, py, color, i_ea);
                if (ix1 - 1 >= 0 && ix1 - 1 < UI_W && ix1 - 1 != ix0) blend_pixel(ix1 - 1, py, color, i_ea);
            }
        }
    }
}

void gfx_draw_battery_icon(int x, int y, int pct, bool charging, uint32_t color)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int bx = x, by = y + 4, bw = 18, bh = 12;
    gfx_stroke_round_rect(bx, by, bw, bh, 3, 2, color);
    /* Positive terminal nub */
    gfx_fill_rect(bx + bw, by + 3, 2, 6, color);

    /* Live fill bar */
    int max_fw = bw - 6; /* 12 px */
    int fw = (pct * max_fw + 50) / 100;
    if (pct > 0 && fw < 1) fw = 1;
    if (fw > max_fw) fw = max_fw;
    uint32_t fill_col = charging ? COL_SUCCESS : (pct <= 20 ? COL_DANGER : color);
    if (fw > 0)
        gfx_fill_rect(bx + 3, by + 3, fw, bh - 6, fill_col);

    if (charging) {
        /* Crisp mini lightning bolt centered on shell */
        uint32_t bolt_col = g_dark_enabled ? 0xFF000000U : 0xFFFFFFFFU;
        int cx = bx + bw / 2;
        int cy = by + bh / 2;
        gfx_fill_rect(cx,     cy - 4, 2, 3, bolt_col);
        gfx_fill_rect(cx - 2, cy - 1, 5, 2, bolt_col);
        gfx_fill_rect(cx - 1, cy + 1, 2, 3, bolt_col);
    }
}

void gfx_draw_scrollbar(int x, int y, int h, float scroll_pos, int visible_items, int total_items)
{
    if (total_items <= visible_items || h < 20) return;
    int track_w = 6;
    gfx_fill_round_rect(x, y, track_w, h, 3, COL_SURFACE_HI);
    float ratio = (float)visible_items / (float)total_items;
    int thumb_h = (int)(h * ratio);
    if (thumb_h < 22) thumb_h = 22;
    if (thumb_h > h) thumb_h = h;
    float max_scroll = (float)(total_items - visible_items);
    float frac = max_scroll > 0.0f ? (scroll_pos / max_scroll) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    int thumb_y = y + (int)((h - thumb_h) * frac);
    gfx_fill_round_rect(x, thumb_y, track_w, thumb_h, 3, COL_ACCENT);
}

void gfx_blit_rgb565_squircle_pan(int x, int y, int dst_w, int dst_h, int r,
                                  const uint16_t *src, int src_w, int src_h,
                                  float zoom, float pan_x, float pan_y)
{
    if (!src || dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0)
        return;
    if (zoom < 1.0f) zoom = 1.0f;
    if (pan_x < 0.0f) pan_x = 0.0f;
    if (pan_x > 1.0f) pan_x = 1.0f;
    if (pan_y < 0.0f) pan_y = 0.0f;
    if (pan_y > 1.0f) pan_y = 1.0f;

    if (r * 2 > dst_w) r = dst_w / 2;
    if (r * 2 > dst_h) r = dst_h / 2;
    if (r > 64) r = 64;

    const float *insets = (r > 0) ? get_corner_insets_aa(r, 32) : NULL;

    /* Aspect-fill (cover) without stretching, then zoom in by `zoom` */
    float scale_x = (float)src_w / (float)dst_w;
    float scale_y = (float)src_h / (float)dst_h;
    float base_scale = (scale_x < scale_y) ? scale_x : scale_y;
    float step_f = base_scale / zoom;
    float crop_w = (float)dst_w * step_f;
    float crop_h = (float)dst_h * step_f;
    float avail_x = (float)src_w - crop_w;
    float avail_y = (float)src_h - crop_h;
    if (avail_x < 0.0f) avail_x = 0.0f;
    if (avail_y < 0.0f) avail_y = 0.0f;
    float off_x = pan_x * avail_x;
    float off_y = pan_y * avail_y;

    int32_t step_fp = (int32_t)(step_f * 65536.0f);
    int32_t ox_fp   = (int32_t)(off_x * 65536.0f);
    int32_t oy_fp   = (int32_t)(off_y * 65536.0f);

    for (int row = 0; row < dst_h; row++) {
        int py = y + row;
        if (py < g_clip_y0 || py >= g_clip_y1) continue;
        int dy = -1;
        if (row < r) dy = (r - 1) - row;
        else if (row >= dst_h - r) dy = row - (dst_h - r);

        float ins_f = (insets && dy >= 0 && dy < r) ? insets[dy] : 0.0f;
        int ins_i = (int)ins_f;
        uint8_t edge_a = (uint8_t)((1.0f - (ins_f - (float)ins_i)) * 255.0f);

        int sy = (oy_fp + row * step_fp) >> 16;
        if (sy < 0) sy = 0;
        if (sy >= src_h) sy = src_h - 1;
        const uint16_t *srow = src + sy * src_w;
        uint32_t *drow = fb + py * UI_W;

        int c0 = ins_i;
        int c1 = dst_w - ins_i;
        int32_t sx_fp = ox_fp + c0 * step_fp;
        for (int col = c0; col < c1; col++, sx_fp += step_fp) {
            int px = x + col;
            if ((unsigned)px >= UI_W) continue;
            int sx = sx_fp >> 16;
            if (sx < 0) sx = 0;
            else if (sx >= src_w) sx = src_w - 1;
            uint32_t c32 = g_rgb565_lut[srow[sx]];
            if (ins_f > 0.0f && (col == c0 || col == c1 - 1))
                blend_pixel(px, py, c32, edge_a);
            else
                drow[px] = c32;
        }
    }
}

void gfx_blit_rgb565_squircle(int x, int y, int dst_w, int dst_h, int r,
                              const uint16_t *src, int src_w, int src_h)
{
    gfx_blit_rgb565_squircle_pan(x, y, dst_w, dst_h, r, src, src_w, src_h, 1.0f, 0.5f, 0.5f);
}

void gfx_blit_rgb565_fit_zoom(const uint16_t *src, int src_w, int src_h, float zoom)
{
    gfx_clear(0xFF000000U);
    if (!src || src_w <= 0 || src_h <= 0) return;
    if (zoom < 1.0f) zoom = 1.0f;
    float fit_x = (float)UI_W / (float)src_w;
    float fit_y = (float)UI_H / (float)src_h;
    float base_s = (fit_x < fit_y) ? fit_x : fit_y;
    float disp_s = base_s * zoom;

    int32_t inv_s_fp = (int32_t)(65536.0f / disp_s);
    float half_w = (float)UI_W * 0.5f;
    float half_h = (float)UI_H * 0.5f;
    float src_cx = (float)src_w * 0.5f;
    float src_cy = (float)src_h * 0.5f;
    int32_t ox_fp = (int32_t)((src_cx - half_w / disp_s) * 65536.0f);
    int32_t oy_fp = (int32_t)((src_cy - half_h / disp_s) * 65536.0f);

    for (int y = 0; y < UI_H; y++) {
        int sy = (oy_fp + y * inv_s_fp) >> 16;
        if (sy < 0 || sy >= src_h) continue;
        const uint16_t *srow = src + sy * src_w;
        uint32_t *drow = fb + y * UI_W;
        int32_t sx_fp = ox_fp;
        for (int x = 0; x < UI_W; x++, sx_fp += inv_s_fp) {
            int sx = sx_fp >> 16;
            if ((unsigned)sx < (unsigned)src_w)
                drow[x] = g_rgb565_lut[srow[sx]];
        }
    }
}

static uint32_t next_utf8(const char **p)
{
    const unsigned char *s = (const unsigned char *)(*p);
    if (!s || !*s) return 0;
    uint32_t cp;
    if (s[0] < 0x80) {
        cp = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3;
    } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
             ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *p += 4;
    } else {
        cp = '?';
        *p += 1;
    }
    return cp;
}

static const struct glyph_entry *get_glyph(uint32_t cp, float size_px, enum ui_font_weight w)
{
    if (!g_fonts[w].loaded) return NULL;
    uint16_t sz10 = (uint16_t)(size_px * 10.0f + 0.5f);
    uint32_t h = (cp * 2654435761U) ^ ((uint32_t)sz10 * 40503U) ^ ((uint32_t)w * 97U);
    uint32_t idx = h & GLYPH_CACHE_MASK;

    for (int probe = 0; probe < 4; probe++) {
        uint32_t s = (idx + (uint32_t)probe) & GLYPH_CACHE_MASK;
        if (g_glyphs[s].valid && g_glyphs[s].codepoint == cp &&
            g_glyphs[s].size_x10 == sz10 && g_glyphs[s].weight == (uint8_t)w)
            return &g_glyphs[s];
        if (!g_glyphs[s].valid) {
            idx = s;
            break;
        }
    }

    struct glyph_entry *slot = &g_glyphs[idx];
    if (slot->alpha) { free(slot->alpha); slot->alpha = NULL; }

    stbtt_fontinfo *fi = &g_fonts[w].info;
    int gidx = stbtt_FindGlyphIndex(fi, (int)cp);
    if (gidx == 0) {
        for (int f = 0; f < g_fallback_count; f++) {
            if (!g_fallback_fonts[f].loaded) continue;
            int fg = stbtt_FindGlyphIndex(&g_fallback_fonts[f].info, (int)cp);
            if (fg != 0) {
                fi = &g_fallback_fonts[f].info;
                gidx = fg;
                break;
            }
        }
    }
    float scale = stbtt_ScaleForPixelHeight(fi, size_px);
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(fi, gidx, &adv, &lsb);

    int gw = 0, gh = 0, xoff = 0, yoff = 0;
    unsigned char *bmp = stbtt_GetGlyphBitmap(fi, 0, scale, gidx, &gw, &gh, &xoff, &yoff);

    slot->codepoint = cp;
    slot->size_x10 = sz10;
    slot->weight = (uint8_t)w;
    slot->w = gw;
    slot->h = gh;
    slot->xoff = xoff;
    slot->yoff = yoff;
    slot->advance = (int)roundf((float)adv * scale);
    slot->alpha = bmp;
    slot->valid = true;
    return slot;
}

int gfx_text_width(const char *utf8, float size_px, enum ui_font_weight weight)
{
    if (!utf8 || !g_fonts[weight].loaded) return 0;
    int width = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = next_utf8(&p);
        if (!cp) break;
        const struct glyph_entry *ge = get_glyph(cp, size_px, weight);
        if (ge) width += ge->advance;
    }
    return width;
}

static void draw_text_pass(int x, int y, const char *utf8, float size_px,
                           enum ui_font_weight weight, uint32_t color)
{
    int ascent = 0, descent = 0, linegap = 0;
    float scale = stbtt_ScaleForPixelHeight(&g_fonts[weight].info, size_px);
    stbtt_GetFontVMetrics(&g_fonts[weight].info, &ascent, &descent, &linegap);
    int baseline = y + (int)roundf((float)ascent * scale);

    uint8_t base_a = (uint8_t)((color >> 24) & 0xFF);
    if (base_a == 0) base_a = 255;

    int pen_x = x;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = next_utf8(&p);
        if (!cp) break;
        const struct glyph_entry *ge = get_glyph(cp, size_px, weight);
        if (!ge) continue;
        if (ge->alpha && ge->w > 0 && ge->h > 0) {
            int gx0 = pen_x + ge->xoff;
            int gy0 = baseline + ge->yoff;
            for (int r = 0; r < ge->h; r++) {
                int dy = gy0 + r;
                if (dy < g_clip_y0 || dy >= g_clip_y1) continue;
                const uint8_t *arow = ge->alpha + r * ge->w;
                for (int c = 0; c < ge->w; c++) {
                    uint8_t a = arow[c];
                    if (a > 0) {
                        uint8_t ea = (base_a == 255) ? a : (uint8_t)div255((uint32_t)a * base_a);
                        if (ea > 0) blend_pixel(gx0 + c, dy, color, ea);
                    }
                }
            }
        }
        pen_x += ge->advance;
    }
}

static void draw_text_shadow_blurred(int x, int y, const char *utf8, float size_px,
                                     enum ui_font_weight weight, uint32_t sh_rgb)
{
    int ascent = 0, descent = 0, linegap = 0;
    float scale = stbtt_ScaleForPixelHeight(&g_fonts[weight].info, size_px);
    stbtt_GetFontVMetrics(&g_fonts[weight].info, &ascent, &descent, &linegap);
    int baseline = y + (int)roundf((float)ascent * scale);

    int pen_x = x;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = next_utf8(&p);
        if (!cp) break;
        const struct glyph_entry *ge = get_glyph(cp, size_px, weight);
        if (!ge) continue;
        if (ge->alpha && ge->w > 0 && ge->h > 0) {
            int gx0 = pen_x + ge->xoff + 1;
            int gy0 = baseline + ge->yoff + 1;
            int gw = ge->w, gh = ge->h;
            const int pad = 7;
            int bw = gw + pad * 2;
            int bh = gh + pad * 2;
            size_t bsz = (size_t)bw * (size_t)bh;
            uint8_t s_a[96 * 96], s_b[96 * 96];
            uint8_t *buf_a = (bsz <= sizeof(s_a)) ? s_a : malloc(bsz);
            uint8_t *buf_b = (bsz <= sizeof(s_b)) ? s_b : malloc(bsz);
            if (buf_a && buf_b) {
                /* 1. 3px spread (separable max dilation) */
                for (int r = 0; r < bh; r++) {
                    int sr = r - pad;
                    uint8_t *dst = buf_a + r * bw;
                    if (sr < 0 || sr >= gh) {
                        memset(dst, 0, (size_t)bw);
                        continue;
                    }
                    const uint8_t *arow = ge->alpha + sr * gw;
                    for (int c = 0; c < bw; c++) {
                        int sc0 = c - pad - 3, sc1 = c - pad + 3;
                        if (sc0 < 0) sc0 = 0;
                        if (sc1 >= gw) sc1 = gw - 1;
                        uint8_t mx = 0;
                        for (int sc = sc0; sc <= sc1; sc++) {
                            if (arow[sc] > mx) mx = arow[sc];
                        }
                        dst[c] = mx;
                    }
                }
                for (int r = 0; r < bh; r++) {
                    int r0 = r - 3 < 0 ? 0 : r - 3;
                    int r1 = r + 3 >= bh ? bh - 1 : r + 3;
                    uint8_t *dst = buf_b + r * bw;
                    for (int c = 0; c < bw; c++) {
                        uint8_t mx = 0;
                        for (int rr = r0; rr <= r1; rr++) {
                            uint8_t v = buf_a[rr * bw + c];
                            if (v > mx) mx = v;
                        }
                        dst[c] = mx;
                    }
                }
                /* 2. 4px separable box blur (9x9 total) at 60% opacity (153 / 255) */
                for (int r = 0; r < bh; r++) {
                    const uint8_t *src = buf_b + r * bw;
                    uint8_t *dst = buf_a + r * bw;
                    for (int c = 0; c < bw; c++) {
                        int c0 = c - 4 < 0 ? 0 : c - 4;
                        int c1 = c + 4 >= bw ? bw - 1 : c + 4;
                        uint32_t sum = 0;
                        for (int cc = c0; cc <= c1; cc++) sum += src[cc];
                        dst[c] = (uint8_t)(sum / 9U);
                    }
                }
                for (int r = 0; r < bh; r++) {
                    int dy = gy0 + r - pad;
                    if (dy < g_clip_y0 || dy >= g_clip_y1) continue;
                    int r0 = r - 4 < 0 ? 0 : r - 4;
                    int r1 = r + 4 >= bh ? bh - 1 : r + 4;
                    for (int c = 0; c < bw; c++) {
                        uint32_t sum = 0;
                        for (int rr = r0; rr <= r1; rr++) sum += buf_a[rr * bw + c];
                        uint8_t a = (uint8_t)((sum * 153U) / 2295U);
                        if (a > 0) blend_pixel(gx0 + c - pad, dy, sh_rgb, a);
                    }
                }
            }
            if (buf_a != s_a) free(buf_a);
            if (buf_b != s_b) free(buf_b);
        }
        pen_x += ge->advance;
    }
}

void gfx_draw_text(int x, int y, const char *utf8, float size_px,
                   enum ui_font_weight weight, uint32_t color)
{
    if (!utf8 || !g_fonts[weight].loaded) return;
    if (g_text_shadow && g_wp_sharp && g_wp_blur) {
        uint32_t sh_rgb = g_dark_enabled ? 0x00000000U : 0x00FFFFFFU;
        draw_text_shadow_blurred(x, y, utf8, size_px, weight, sh_rgb);
    }
    draw_text_pass(x, y, utf8, size_px, weight, color);
}

void gfx_draw_text_ellipsize(int x, int y, int max_w, const char *utf8, float size_px,
                             enum ui_font_weight weight, uint32_t color)
{
    if (!utf8) return;
    if (gfx_text_width(utf8, size_px, weight) <= max_w) {
        gfx_draw_text(x, y, utf8, size_px, weight, color);
        return;
    }
    char buf[192];
    size_t len = strlen(utf8);
    if (len >= sizeof(buf) - 4) len = sizeof(buf) - 5;
    memcpy(buf, utf8, len);
    buf[len] = '\0';

    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--;
        buf[len] = '\0';
        char cand[200];
        snprintf(cand, sizeof(cand), "%s...", buf);
        if (gfx_text_width(cand, size_px, weight) <= max_w) {
            gfx_draw_text(x, y, cand, size_px, weight, color);
            return;
        }
    }
    gfx_draw_text(x, y, "...", size_px, weight, color);
}

static const struct icon_entry *get_icon(const char *name, int size_px)
{
    if (!name || !g_svg_rast || size_px <= 0) return NULL;
    for (int i = 0; i < ICON_CACHE_SIZE; i++) {
        if (g_icons[i].valid && g_icons[i].size == size_px &&
            strcmp(g_icons[i].name, name) == 0)
            return &g_icons[i];
    }

    int slot_idx = 0;
    for (int i = 0; i < ICON_CACHE_SIZE; i++) {
        if (!g_icons[i].valid) { slot_idx = i; break; }
    }
    if (g_icons[slot_idx].alpha) {
        free(g_icons[slot_idx].alpha);
        g_icons[slot_idx].alpha = NULL;
    }
    g_icons[slot_idx].valid = false;

    char path[256];
    const char *roots[] = {
        "/mnt/sdcard/.meui/icons",
        "/.meui/icons",
        "/mnt/sdcard/.system/configapp/icons",
        "/.system/configapp/icons",
        NULL
    };
    unsigned char *svg_text = NULL;
    for (int i = 0; roots[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s.svg", roots[i], name);
        svg_text = read_entire_file(path, NULL);
        if (svg_text) break;
    }
    if (!svg_text) return NULL;

    for (char *p = (char *)svg_text; (p = strstr(p, "currentColor")) != NULL; p += 12)
        memcpy(p, "#ffffff     ", 12);

    NSVGimage *img = nsvgParse((char *)svg_text, "px", 96.0f);
    free(svg_text);
    if (!img || img->width <= 0.0f || img->height <= 0.0f) {
        if (img) nsvgDelete(img);
        return NULL;
    }

    uint8_t *rgba = malloc((size_t)size_px * (size_t)size_px * 4);
    uint8_t *alpha = malloc((size_t)size_px * (size_t)size_px);
    if (!rgba || !alpha) {
        free(rgba); free(alpha); nsvgDelete(img);
        return NULL;
    }

    float scale = (float)size_px / img->width;
    nsvgRasterize(g_svg_rast, img, 0.0f, 0.0f, scale, rgba, size_px, size_px, size_px * 4);
    nsvgDelete(img);

    for (int i = 0; i < size_px * size_px; i++)
        alpha[i] = rgba[i * 4 + 3];
    free(rgba);

    snprintf(g_icons[slot_idx].name, sizeof(g_icons[slot_idx].name), "%s", name);
    g_icons[slot_idx].size = size_px;
    g_icons[slot_idx].alpha = alpha;
    g_icons[slot_idx].valid = true;
    return &g_icons[slot_idx];
}

void gfx_draw_icon(const char *name, int x, int y, int size_px, uint32_t color)
{
    const struct icon_entry *ic = get_icon(name, size_px);
    if (!ic || !ic->alpha) return;
    for (int r = 0; r < size_px; r++) {
        int dy = y + r;
        if (dy < g_clip_y0 || dy >= g_clip_y1) continue;
        const uint8_t *arow = ic->alpha + r * size_px;
        for (int c = 0; c < size_px; c++) {
            uint8_t a = arow[c];
            if (a > 0) blend_pixel(x + c, dy, color, a);
        }
    }
}
