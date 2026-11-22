#ifndef MEUI_UI_GFX_H
#define MEUI_UI_GFX_H

#include <stdint.h>
#include <stdbool.h>

#define UI_W 480
#define UI_H 360

/* MeUI dynamic Light / Dark UI palette (shared with ConfigApp) */
struct ui_palette {
    uint32_t bg;
    uint32_t surface;
    uint32_t surface_hi;
    uint32_t border;
    uint32_t border_hi;
    uint32_t accent;
    uint32_t accent_text;
    uint32_t accent_soft;
    uint32_t sel_bg;
    uint32_t sel_text;
    uint32_t modal_bg;
    uint32_t modal_hi;
    uint32_t modal_border;
    uint32_t text;
    uint32_t text_dim;
    uint32_t text_muted;
    uint32_t success;
    uint32_t warn;
    uint32_t danger;
    uint32_t key_bg;
    uint32_t switch_icon;
};

extern const struct ui_palette *g_pal;
void gfx_set_dark_mode(bool dark);
void gfx_set_text_shadow(bool enabled);

#define COL_BG           (g_pal->bg)
#define COL_SURFACE      (g_pal->surface)
#define COL_SURFACE_HI   (g_pal->surface_hi)
#define COL_BORDER       (g_pal->border)
#define COL_BORDER_HI    (g_pal->border_hi)
#define COL_ACCENT       (g_pal->accent)
#define COL_ACCENT_TEXT  (g_pal->accent_text)
#define COL_ACCENT_SOFT  (g_pal->accent_soft)
#define COL_SEL_BG       (g_pal->sel_bg)
#define COL_SEL_TEXT     (g_pal->sel_text)
#define COL_MODAL_BG     (g_pal->modal_bg)
#define COL_MODAL_HI     (g_pal->modal_hi)
#define COL_MODAL_BORDER (g_pal->modal_border)
#define COL_TEXT         (g_pal->text)
#define COL_TEXT_DIM     (g_pal->text_dim)
#define COL_TEXT_MUTED   (g_pal->text_muted)
#define COL_SUCCESS      (g_pal->success)
#define COL_WARN         (g_pal->warn)
#define COL_DANGER       (g_pal->danger)
#define COL_KEY_BG       (g_pal->key_bg)
#define COL_SWITCH_ICON  (g_pal->switch_icon)

enum ui_font_weight {
    UI_FONT_REGULAR = 0,
    UI_FONT_SEMIBOLD = 1
};

bool gfx_init(void);
void gfx_shutdown(void);
void gfx_clear(uint32_t color);
void gfx_draw_wallpaper(float blur_t);
bool gfx_has_wallpaper(void);
bool gfx_wallpaper_is_fading(void);
bool gfx_set_wallpaper_from_image(const char *path);
void gfx_set_wallpaper_rgb565(const uint16_t *rgb565, int w, int h, bool crossfade_1s);
void gfx_clear_wallpaper(void);
bool gfx_set_custom_font(const char *path);
void gfx_reset_default_font(void);
bool gfx_has_custom_font(void);
void gfx_present(void);

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color);
void gfx_stroke_round_rect(int x, int y, int w, int h, int r, int thickness, uint32_t color);
void gfx_blit_rgb565_squircle(int x, int y, int dst_w, int dst_h, int r,
                              const uint16_t *src, int src_w, int src_h);
void gfx_blit_rgb565_squircle_pan(int x, int y, int dst_w, int dst_h, int r,
                                  const uint16_t *src, int src_w, int src_h,
                                  float zoom, float pan_x, float pan_y);
void gfx_blit_rgb565_fit_zoom(const uint16_t *src, int src_w, int src_h, float zoom);
void gfx_draw_battery_icon(int x, int y, int pct, bool charging, uint32_t color);
void gfx_set_clip_y(int y0, int y1);
void gfx_reset_clip(void);
void gfx_draw_scrollbar(int x, int y, int h, float scroll_pos, int visible_items, int total_items);

int  gfx_text_width(const char *utf8, float size_px, enum ui_font_weight weight);
void gfx_draw_text(int x, int y, const char *utf8, float size_px,
                   enum ui_font_weight weight, uint32_t color);
void gfx_draw_text_ellipsize(int x, int y, int max_w, const char *utf8, float size_px,
                             enum ui_font_weight weight, uint32_t color);

void gfx_draw_icon(const char *name, int x, int y, int size_px, uint32_t color);

#endif /* MEUI_UI_GFX_H */
