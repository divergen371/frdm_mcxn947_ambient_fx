/**
 * @file audio_synth.h
 * @brief 48 kHz / 16-bit 固定小数点 DDS テスト音源
 */

#ifndef AUDIO_SYNTH_H
#define AUDIO_SYNTH_H

#include <stdint.h>

typedef enum
{
    SYNTH_SRC_SHOEGAZE_GLIDE = 0,
    SYNTH_SRC_PLUCK_ARP,
    SYNTH_SRC_SAW_220,
    SYNTH_SRC_COUNT
} audio_synth_src_t;

void audio_synth_set_src(audio_synth_src_t src);
audio_synth_src_t audio_synth_get_src(void);
const char *audio_synth_get_name(audio_synth_src_t src);
int16_t audio_synth_next_sample(void);

#endif /* AUDIO_SYNTH_H */
