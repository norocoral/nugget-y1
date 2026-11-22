#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int main(int argc, char **argv)
{
    if (argc < 4)
        return 1;

    const char *ttf_path = argv[1];
    float size_px = (float)atof(argv[2]);
    const char *text = argv[3];
    if (size_px <= 0.0f) size_px = 16.0f;

    FILE *f = fopen(ttf_path, "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 3; }
    unsigned char *ttf = malloc((size_t)sz);
    if (!ttf) { fclose(f); return 4; }
    if (fread(ttf, 1, (size_t)sz, f) != (size_t)sz) {
        free(ttf);
        fclose(f);
        return 5;
    }
    fclose(f);

    stbtt_fontinfo fi;
    if (!stbtt_InitFont(&fi, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        free(ttf);
        return 6;
    }

    float scale = stbtt_ScaleForPixelHeight(&fi, size_px);
    int ascent = 0, descent = 0, linegap = 0;
    stbtt_GetFontVMetrics(&fi, &ascent, &descent, &linegap);
    int baseline = (int)ceilf((float)ascent * scale);
    int out_h = (int)ceilf((float)(ascent - descent) * scale) + 2;
    int out_w = 0;

    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&fi, (int)*p, &adv, &lsb);
        out_w += (int)roundf((float)adv * scale);
    }
    if (out_w < 1) out_w = 1;
    out_w += 2;

    uint8_t *alpha = calloc((size_t)out_w * (size_t)out_h, 1);
    if (!alpha) { free(ttf); return 7; }

    int pen_x = 1;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&fi, (int)*p, &adv, &lsb);
        int gw = 0, gh = 0, xoff = 0, yoff = 0;
        unsigned char *bmp = stbtt_GetCodepointBitmap(&fi, 0, scale, (int)*p, &gw, &gh, &xoff, &yoff);
        if (bmp) {
            int gx0 = pen_x + xoff;
            int gy0 = baseline + yoff;
            for (int r = 0; r < gh; r++) {
                int dy = gy0 + r;
                if (dy < 0 || dy >= out_h) continue;
                for (int c = 0; c < gw; c++) {
                    int dx = gx0 + c;
                    if (dx < 0 || dx >= out_w) continue;
                    uint8_t a = bmp[r * gw + c];
                    if (a > alpha[dy * out_w + dx])
                        alpha[dy * out_w + dx] = a;
                }
            }
            stbtt_FreeBitmap(bmp, NULL);
        }
        pen_x += (int)roundf((float)adv * scale);
    }

    uint32_t hdr[2] = { (uint32_t)out_w, (uint32_t)out_h };
    fwrite(hdr, sizeof(uint32_t), 2, stdout);
    fwrite(alpha, 1, (size_t)out_w * (size_t)out_h, stdout);

    free(alpha);
    free(ttf);
    return 0;
}
