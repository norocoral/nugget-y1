#ifndef Y1_PCM_EAC_H
#define Y1_PCM_EAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void y1_audio_receiver_set_active(bool active, unsigned long sample_rate);
void y1_audio_receiver_mode_set(bool enabled);
size_t y1_audio_receiver_write(const int16_t *samples, size_t frames);

#endif
