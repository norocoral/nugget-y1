#ifndef Y1_PCM_EAC_H
#define Y1_PCM_EAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum y1_mic_sources {
    Y1_MIC_AUTO = 0,
    Y1_MIC_INTERNAL,
    Y1_MIC_HEADPHONE,
    Y1_MIC_BLUETOOTH,
    Y1_NUM_MIC_SOURCES
};

void y1_audio_receiver_set_active(bool active, unsigned long sample_rate);
void y1_audio_receiver_mode_set(bool enabled);
size_t y1_audio_receiver_write(const int16_t *samples, size_t frames);

#ifdef HAVE_RECORDING
void y1_mic_source_changed(int source);
int  y1_get_effective_mic_source(void);
#endif

#endif
