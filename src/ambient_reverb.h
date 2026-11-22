/**
 * @file ambient_reverb.h
 * @brief FRDM-MCXN947 48 kHz フルレート SPX90 風 Reverse Gate / Early Reflections
 *        + Dimension Chorus + 高域/低域独立波形ハーモニック・パンニング
 *        + 4x4 Hadamard FDN + シマー (Fuzz 抜き)
 */

#ifndef AMBIENT_REVERB_H
#define AMBIENT_REVERB_H

#include <stdint.h>

typedef enum
{
    PAN_WAVE_SINE = 0,       /* 滑らかなサイン波 */
    PAN_WAVE_TRIANGLE,       /* 等速直線パン (三角波) */
    PAN_WAVE_RAMP_UP,        /* ノコギリ波 (左 -> 右へ流れて戻る) */
    PAN_WAVE_RAMP_DOWN,      /* 逆ノコギリ波 (右 -> 左へ流れて戻る) */
    PAN_WAVE_SOFT_SQUARE,    /* 丸めた矩形波 (跳ね / スライサー風) */
    PAN_WAVE_SMOOTH_RANDOM,  /* 滑らかランダム (xorshift + 補間) */
    PAN_WAVE_COUNT
} ambient_pan_wave_t;

typedef struct
{
    uint8_t decay;        /* 0..255: FDN フィードバック量 */
    uint8_t shimmer;      /* 0..255: +1 オクターブ・シマー帰還量 */
    uint8_t tap_level;    /* 0..255: 32 タップ FIR (Reverse Gate / ER) レベル */
    uint8_t chorus;       /* 0..255: Dimension Chorus レベル */
    uint8_t mix;          /* 0..255: Dry / Wet バランス */
    uint8_t tap_mode;     /* 0: OFF, 1: Reverse Gate, 2: Early Reflections */
    uint8_t pan_depth;    /* 0..255: ハーモニック・パンニング深さ (0=OFF) */
    uint8_t pan_rate;     /* 0..255: パンニング速度 (約 0.20..3.40 Hz) */
    uint8_t pan_hi_wave;  /* ambient_pan_wave_t: 高域パン波形 */
    uint8_t pan_lo_wave;  /* ambient_pan_wave_t: 低域パン波形 */
} ambient_params_t;

typedef enum
{
    AMBIENT_PRESET_REVERSE_GATE = 0,
    AMBIENT_PRESET_EARLY_REFLECTIONS,
    AMBIENT_PRESET_CHORUS_3PHASE,
    AMBIENT_PRESET_PAN_CROSS_RAMP,
    AMBIENT_PRESET_PAN_STARDUST,
    AMBIENT_PRESET_PAN_PSYCHE_CHOP,
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
const char *ambient_reverb_get_pan_wave_name(ambient_pan_wave_t wave);
void ambient_reverb_process_stereo_q23(int32_t x, int32_t *out_l, int32_t *out_r);
void ambient_reverb_process_stereo(int16_t x, int16_t *out_l, int16_t *out_r);

#endif /* AMBIENT_REVERB_H */
