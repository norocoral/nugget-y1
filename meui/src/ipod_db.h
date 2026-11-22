#ifndef MEUI_IPOD_DB_H
#define MEUI_IPOD_DB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DB_MAX_TRACKS    524288
#define DB_MAX_GROUPS    131072
#define DB_MAX_PLAYLISTS 512

struct ipod_str_slab {
    struct ipod_str_slab *next;
    size_t used;
    size_t cap;
    char   data[];
};

struct ipod_track {
    uint32_t    track_id;
    const char *title;
    const char *artist;
    const char *album;
    const char *genre;
    const char *fs_path; /* /mnt/sdcard/iPod_Control/Music/... */
    const char *rb_path; /* /iPod_Control/Music/... for liby1 albumart/playcount */
    uint32_t    duration_ms;
    uint16_t    track_num;
    uint16_t    year;
};

struct ipod_group {
    char name[96];
    int  track_count;
    int  first_track_idx;
};

struct ipod_playlist {
    char name[96];
    int *track_indices;
    int  track_count;
};

struct ipod_db {
    struct ipod_track    *tracks;
    int                   track_count;
    int                   track_cap;
    struct ipod_group    *artists;
    int                   artist_count;
    struct ipod_group    *albums;
    int                   album_count;
    struct ipod_playlist  playlists[DB_MAX_PLAYLISTS];
    int                   playlist_count;
    struct ipod_str_slab *str_slabs;
    bool                  from_itunesdb;
};

typedef bool (*ipod_db_progress_cb)(int tracks_found, int dirs_scanned, const char *cur_path, void *user);

void ipod_db_init(struct ipod_db *db);
void ipod_db_free(struct ipod_db *db);
bool ipod_db_load(struct ipod_db *db, bool use_ipod_db, ipod_db_progress_cb cb, void *user);

#endif /* MEUI_IPOD_DB_H */
