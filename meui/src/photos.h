#ifndef MEUI_PHOTOS_H
#define MEUI_PHOTOS_H

#include <stdbool.h>
#include <stdint.h>

struct photo_item {
    char     path[384];
    char     name[96];
    uint64_t date_ts;
};

struct photo_bitmap {
    uint16_t *rgb565;
    int       w;
    int       h;
};

void photos_init(void);
void photos_shutdown(void);
void photos_scan(void);
int  photos_count(void);
const struct photo_item   *photos_get(int idx);
const struct photo_bitmap *photos_get_thumb(int idx);
const struct photo_bitmap *photos_get_full(int idx);

#endif /* MEUI_PHOTOS_H */
