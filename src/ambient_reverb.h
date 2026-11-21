/**
 * @file ambient_reverb.h
 * @brief FRDM-MCXN947 48 kHz フルレート SPX90 風 Reverse Gate / Early Reflections
 *        + 酩酊 3 相 6 Hz コーラス + 4x4 Hadamard FDN + シマー (Fuzz 抜き)
 */

#ifndef AMBIENT_REVERB_H
#define AMBIENT_REVERB_H

#include <stdint.h>

typedef struct
{
    uint8_t decay;      /* 0..255: FDN フィードバック量 */
    uint8_t shimmer;    /* 0..255: +1 オクターブ・シマー帰還量 */
    uint8_t tap_level;  /* 0..255: 32 タップ FIR (Reverse Gate / ER) レベル */
    uint8_t chorus;     /* 0..255: 3 相 6 Hz コーラスレベル */
    uint8_t mix;        /* 0..255: Dry / Wet バランス */
    uint8_t tap_mode;   /* 0: OFF, 1: Reverse Gate, 2: Early Reflections */
} ambient_params_t;

typedef enum
{
    AMBIENT_PRESET_REVERSE_GATE = 0,
    AMBIENT_PRESET_EARLY_REFLECTIONS,
    AMBIENT_PRESET_CHORUS_3PHASE,
    AMBIENT_PRESET_SHIMMER_HALL,
    AMBIENT_PRESET_SHOEGAZE_CATHEDRAL,
    AMBIENT_PRESET_BYPASS,
    AMBIENT_PRESET_OFF,
    AMBIENT_PRESET_COUNT
} ambient_preset_t;

void ambient_reverb_init(void);
void ambient_reverb_set_preset(ambient_preset_t preset);
void ambient_reverb_set_params(const ambient_params_t *params);
const char *ambient_reverb_get_preset_name(ambient_preset_t preset);
void ambient_reverb_process_stereo(int16_t x, int16_t *out_l, int16_t *out_r);

#endif /* AMBIENT_REVERB_H */
