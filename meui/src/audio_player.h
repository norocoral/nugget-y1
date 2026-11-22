#ifndef MEUI_AUDIO_PLAYER_H
#define MEUI_AUDIO_PLAYER_H

#include "ipod_db.h"
#include <stdbool.h>
#include <stdint.h>

enum repeat_mode {
    REPEAT_OFF = 0,
    REPEAT_ALL = 1,
    REPEAT_ONE = 2
};

struct player_status {
    bool             has_track;
    bool             playing;
    int              queue_pos;
    int              queue_len;
    int              track_idx;
    uint32_t         pos_ms;
    uint32_t         dur_ms;
    int              volume_pct; /* 0..100 */
    bool             shuffle;
    enum repeat_mode repeat;
};

void player_init(const struct ipod_db *db);
void player_shutdown(void);

void player_play_queue(const int *track_indices, int count, int start_pos);
void player_prepare_resume(const int *track_indices, int count, int start_pos, uint32_t pos_ms, int vol_pct);
void player_play_next(int track_idx);
void player_add_to_queue(int track_idx);
void player_jump_queue_pos(int pos);
void player_remove_queue_pos(int pos);
void player_move_queue_pos(int from_pos, int to_pos);
int  player_get_queue(int *out_indices, int max_count);
void player_toggle_pause(void);
void player_pause(void);
void player_play(void);
void player_stop(void);
void player_next(void);
void player_prev(void);
void player_seek_delta_ms(int delta_ms);
void player_set_volume(int vol_pct);
void player_adjust_volume(int delta_pct);
void player_set_shuffle(bool on);
void player_set_repeat(enum repeat_mode mode);

void player_get_status(struct player_status *out);

#endif /* MEUI_AUDIO_PLAYER_H */
