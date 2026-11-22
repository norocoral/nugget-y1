#ifndef MEUI_ALBUM_ART_H
#define MEUI_ALBUM_ART_H

#include "ipod_db.h"
#include <stdint.h>
#include <stdbool.h>

struct art_bitmap {
    uint16_t *rgb565;
    int       w;
    int       h;
};

void album_art_init(void);
void album_art_shutdown(void);

/**
 * Returns a cached RGB565 bitmap for the given track (or NULL if none found).
 * Tries: 1) liby1 native iPod ArtworkDB, 2) embedded ID3v2 APIC, 3) folder cover.jpg/png.
 */
const struct art_bitmap *album_art_get_for_track(const struct ipod_track *tr);

#endif /* MEUI_ALBUM_ART_H */
