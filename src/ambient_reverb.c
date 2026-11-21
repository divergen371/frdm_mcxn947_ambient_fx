/**
 * @file ambient_reverb.c
 * @brief FRDM-MCXN947 48 kHz フルレート・間引きゼロ版 アンビエント／シューゲイザー DSP エンジン
 *
 * 512 KB SRAM の余裕を活かし、dsPIC33CK 版で行っていた 16 kHz / 8 kHz 間引きを撤廃。
 * すべての段 (32 タップ FIR / 4 段オールパス拡散 / 4x4 Hadamard FDN / 3 相 6 Hz コーラス / +1oct シマー)
 * を 48,000 Hz フル帯域・独立バッファで動作させる。
 */

#include "ambient_reverb.h"
#include <string.h>
#include "cmsis_compiler.h"

/* 4x4 Hadamard FDN 素数遅延長 @ 48 kHz (31.0 / 36.5 / 42.8 / 49.6 ms) */
#define FDN_LEN_0        1487U
#define FDN_LEN_1        1753U
#define FDN_LEN_2        2053U
#define FDN_LEN_3        2381U

/* 軽いオールパス拡散器 (L/R 独立, 各 2 段 @ 48 kHz: 2.3..3.4 ms) */
#define DIFF_L0_LEN       113U
#define DIFF_L1_LEN       149U
#define DIFF_R0_LEN       127U
#define DIFF_R1_LEN       163U

/* 48 kHz フルレート マルチタップ FIR ディレイライン (16384 サンプル = 341.3 ms) */
#define TAP_BUF_SIZE     16384U
#define TAP_BUF_MASK     (TAP_BUF_SIZE - 1U)
#define TAP_COUNT        16U

/* 48 kHz フルレート 3 相 6 Hz コーラス専用バッファ (1024 サンプル = 21.3 ms) */
#define CHO_BUF_SIZE     1024U
#define CHO_BUF_MASK     (CHO_BUF_SIZE - 1U)

/* 48 kHz フルレート シマー専用バッファ (2048 サンプル = 42.7 ms 窓) */
#define SHIM_BUF_SIZE    2048U
#define SHIM_BUF_MASK    (SHIM_BUF_SIZE - 1U)

/* LFO 位相増分 @ 48 kHz: round(f * 2^32 / 48000) */
#define FDN_LFO_INC      40265UL     /* 約 0.45 Hz */
#define CHORUS_LFO_6HZ   536871UL    /* 6.00 Hz */
#define PHASE_120_DEG    0x55555555UL
#define PHASE_240_DEG    0xAAAAAAAAUL

/* SPX90 風 L/R 非対称 16+16 素数反射タップ @ 48 kHz (20 ms .. 255 ms) */
static const uint16_t g_tap_dly_l[TAP_COUNT] = {
     977U,  1511U,  2113U,  2801U,  3527U,  4327U,  5119U,  5953U,
    6779U,  7669U,  8539U,  9403U, 10253U, 10987U, 11597U, 12007U
};

static const uint16_t g_tap_dly_r[TAP_COUNT] = {
    1181U,  1759U,  2411U,  3121U,  3853U,  4621U,  5443U,  6301U,
    7159U,  7963U,  8887U,  9679U, 10513U, 11261U, 11839U, 12197U
};

/* Reverse Gate ゲイン: 255 ms のゲート端へ向かって壁が迫りスパッと切れる */
static const int16_t g_gain_rev_gate[TAP_COUNT] = {
     14,  -18,   24,  -32,   42,  -56,   72,  -92,
    116, -144,  176, -212,  248, -284,  312, -330
};

/* Early Reflections ゲイン: 初期反射が明瞭に鳴り自然に減衰 */
static const int16_t g_gain_early_ref[TAP_COUNT] = {
    290, -250,  218, -188,  160, -136,  114,  -96,
     80,  -66,   54,  -44,   34,  -26,   18,  -12
};

/* 静的ディレイバッファ (合計 約 55 KB / 512 KB SRAM) */
static int16_t g_fdn0[FDN_LEN_0];
static int16_t g_fdn1[FDN_LEN_1];
static int16_t g_fdn2[FDN_LEN_2];
static int16_t g_fdn3[FDN_LEN_3];
static int16_t g_diff_l0[DIFF_L0_LEN];
static int16_t g_diff_l1[DIFF_L1_LEN];
static int16_t g_diff_r0[DIFF_R0_LEN];
static int16_t g_diff_r1[DIFF_R1_LEN];
static int16_t g_tap_buf[TAP_BUF_SIZE];
static int16_t g_cho_buf[CHO_BUF_SIZE];
static int16_t g_shim_buf[SHIM_BUF_SIZE];

static uint16_t g_fdn_idx[4];
static int32_t g_fdn_damp[4];
static uint16_t g_diff_idx[4];
static uint16_t g_tap_wr = 0U;
static uint16_t g_cho_wr = 0U;
static uint16_t g_shim_wr = 0U;
static uint16_t g_shim_ph = 0U;
static int32_t g_shim_dc = 0;
static int32_t g_shim_lp1 = 0;
static int32_t g_shim_lp2 = 0;

static uint32_t g_fdn_lfo = 0UL;
static uint32_t g_chorus_lfo = 0UL;

static volatile ambient_preset_t g_preset = AMBIENT_PRESET_REVERSE_GATE;
static volatile ambient_params_t g_params = { 160U, 0U, 255U, 0U, 225U, 1U };

static const ambient_params_t g_preset_table[AMBIENT_PRESET_COUNT] = {
    /* decay, shimmer, tap_lvl, chorus, mix, tap_mode (1=RevGate, 2=ER) */
    { 165U,   0U, 255U,   0U, 225U, 1U }, /* REVERSE_GATE */
    { 180U,   0U, 255U,   0U, 180U, 2U }, /* EARLY_REFLECTIONS */
    { 222U,   0U,   0U, 255U, 220U, 0U }, /* CHORUS_3PHASE */
    { 242U, 130U,   0U,   0U, 190U, 0U }, /* SHIMMER_HALL */
    { 238U, 110U, 235U, 215U, 230U, 1U }, /* SHOEGAZE_CATHEDRAL */
    {   0U,   0U,   0U,   0U,   0U, 0U }, /* BYPASS */
    {   0U,   0U,   0U,   0U,   0U, 0U }  /* OFF */
};

static inline int16_t clamp_s16(int32_t v)
{
#if defined(__clang__) && !__has_builtin(__builtin_arm_ssat)
    if (v > 32767)
    {
        return 32767;
    }
    if (v < -32768)
    {
        return -32768;
    }
    return (int16_t)v;
#else
    return (int16_t)__SSAT(v, 16);
#endif
}

static inline uint16_t tri_u10(uint32_t ph)
{
    uint16_t p = (uint16_t)(ph >> 21);
    return (p < 1024U) ? p : (uint16_t)(2047U - p);
}

static inline int16_t allpass_light_step(int16_t *buf, uint16_t len, uint16_t *p_idx, int16_t in)
{
    uint16_t idx = *p_idx;
    int16_t b = buf[idx];
    int32_t v = (int32_t)in + ((int32_t)b >> 1);
    int32_t out = (int32_t)b - (v >> 1);

    buf[idx] = clamp_s16(v);
    if (++idx >= len)
    {
        idx = 0U;
    }
    *p_idx = idx;
    return clamp_s16(out);
}

static void multitap_fir_step_48k(int16_t in, uint8_t tap_mode, int16_t *out_l, int16_t *out_r)
{
    const int16_t *gains = (tap_mode == 1U) ? g_gain_rev_gate : g_gain_early_ref;
    int32_t sum_l = 0;
    int32_t sum_r = 0;

    g_tap_buf[g_tap_wr] = in;

    for (uint8_t k = 0U; k < TAP_COUNT; k++)
    {
        int32_t g = (int32_t)gains[k];
        sum_l += (int32_t)g_tap_buf[(g_tap_wr - g_tap_dly_l[k]) & TAP_BUF_MASK] * g;
        sum_r += (int32_t)g_tap_buf[(g_tap_wr - g_tap_dly_r[k]) & TAP_BUF_MASK] * g;
    }

    g_tap_wr = (g_tap_wr + 1U) & TAP_BUF_MASK;
    *out_l = clamp_s16(sum_l >> 10);
    *out_r = clamp_s16(sum_r >> 10);
}

static inline int16_t cho_read_q8(uint32_t dly_q8)
{
    uint16_t di = (uint16_t)(dly_q8 >> 8);
    uint16_t fr = (uint16_t)(dly_q8 & 0xFFU);
    uint16_t r0 = (g_cho_wr - di) & CHO_BUF_MASK;
    uint16_t r1 = (r0 - 1U) & CHO_BUF_MASK;
    int32_t s0 = g_cho_buf[r0];
    int32_t s1 = g_cho_buf[r1];

    return (int16_t)(s0 + (((s1 - s0) * (int32_t)fr) >> 8));
}

/* 48 kHz フルレート 酩酊 3 相 6 Hz ステレオ・コーラス */
static void chorus_3phase_step_48k(int16_t in, int16_t *out_l, int16_t *out_r)
{
    uint16_t l0 = tri_u10(g_chorus_lfo);
    uint16_t l1 = tri_u10(g_chorus_lfo + PHASE_120_DEG);
    uint16_t l2 = tri_u10(g_chorus_lfo + PHASE_240_DEG);
    /* 0.45 Hz 船酔いドリフト: 0..96 サンプル (2.0 ms @ 48 kHz) */
    uint32_t drift_q8 = (uint32_t)tri_u10(g_fdn_lfo) * 24UL;
    uint32_t base_q8 = (96UL << 8) + drift_q8;

    /* 6 Hz 3 相変調: 0..264 サンプル (5.5 ms @ 48 kHz -> ±2.75 ms) */
    int32_t t0 = cho_read_q8(base_q8 + ((uint32_t)l0 * 66UL));
    int32_t t1 = cho_read_q8(base_q8 + ((uint32_t)l1 * 66UL));
    int32_t t2 = cho_read_q8(base_q8 + ((uint32_t)l2 * 66UL));
    int32_t fb_sum = (t0 + t1 + t2) >> 2;

    g_cho_buf[g_cho_wr] = clamp_s16((int32_t)in + ((fb_sum * 96) >> 8));
    g_cho_wr = (g_cho_wr + 1U) & CHO_BUF_MASK;
    g_chorus_lfo += CHORUS_LFO_6HZ;

    *out_l = clamp_s16(((t0 + (t1 >> 1) - (t2 >> 2)) * 200) >> 8);
    *out_r = clamp_s16(((t2 - (t1 >> 1) + (t0 >> 2)) * 200) >> 8);
}

/* 48 kHz フルレート 42.7 ms 窓 +1 オクターブ・シマー (クリックレス三角窓) */
static int16_t shimmer_step_48k(int16_t in)
{
    uint16_t d_a = (SHIM_BUF_SIZE - 1U) - g_shim_ph;
    uint16_t d_b = (d_a + (SHIM_BUF_SIZE >> 1)) & SHIM_BUF_MASK;
    uint16_t w_a = (d_a < (SHIM_BUF_SIZE >> 1)) ? d_a : ((SHIM_BUF_SIZE - 1U) - d_a);
    uint16_t w_b = ((SHIM_BUF_SIZE >> 1) - 1U) - w_a;

    int32_t out = ((int32_t)g_shim_buf[(g_shim_wr - d_a) & SHIM_BUF_MASK] * (int32_t)w_a)
                + ((int32_t)g_shim_buf[(g_shim_wr - d_b) & SHIM_BUF_MASK] * (int32_t)w_b);

    g_shim_buf[g_shim_wr] = in;
    g_shim_wr = (g_shim_wr + 1U) & SHIM_BUF_MASK;
    g_shim_ph = (g_shim_ph + 1U) & SHIM_BUF_MASK;

    return clamp_s16(out >> 10);
}

static inline int16_t fdn_read_mod(const int16_t *buf, uint16_t len, uint16_t wr_idx, uint16_t mod_q8)
{
    uint16_t offset = (uint16_t)(mod_q8 >> 8);
    uint16_t frac = (uint16_t)(mod_q8 & 0xFFU);
    uint16_t r0 = wr_idx + offset;
    uint16_t r1;

    if (r0 >= len)
    {
        r0 -= len;
    }
    r1 = r0 + 1U;
    if (r1 >= len)
    {
        r1 = 0U;
    }

    int32_t s0 = buf[r0];
    int32_t s1 = buf[r1];
    return (int16_t)(s0 + (((s1 - s0) * (int32_t)frac) >> 8));
}

static void clear_buffers(void)
{
    memset(g_fdn0, 0, sizeof(g_fdn0));
    memset(g_fdn1, 0, sizeof(g_fdn1));
    memset(g_fdn2, 0, sizeof(g_fdn2));
    memset(g_fdn3, 0, sizeof(g_fdn3));
    memset(g_diff_l0, 0, sizeof(g_diff_l0));
    memset(g_diff_l1, 0, sizeof(g_diff_l1));
    memset(g_diff_r0, 0, sizeof(g_diff_r0));
    memset(g_diff_r1, 0, sizeof(g_diff_r1));
    memset(g_tap_buf, 0, sizeof(g_tap_buf));
    memset(g_cho_buf, 0, sizeof(g_cho_buf));
    memset(g_shim_buf, 0, sizeof(g_shim_buf));

    memset(g_fdn_idx, 0, sizeof(g_fdn_idx));
    memset(g_fdn_damp, 0, sizeof(g_fdn_damp));
    memset(g_diff_idx, 0, sizeof(g_diff_idx));

    g_tap_wr = 0U;
    g_cho_wr = 0U;
    g_shim_wr = 0U;
    g_shim_ph = 0U;
    g_shim_dc = 0;
    g_shim_lp1 = 0;
    g_shim_lp2 = 0;
    g_fdn_lfo = 0UL;
    g_chorus_lfo = 0UL;
}

void ambient_reverb_init(void)
{
    clear_buffers();
    g_preset = AMBIENT_PRESET_REVERSE_GATE;
    g_params = g_preset_table[AMBIENT_PRESET_REVERSE_GATE];
}

void ambient_reverb_set_preset(ambient_preset_t preset)
{
    if (preset >= AMBIENT_PRESET_COUNT)
    {
        preset = AMBIENT_PRESET_REVERSE_GATE;
    }
    g_preset = preset;
    g_params = g_preset_table[preset];
    clear_buffers();
}

void ambient_reverb_set_params(const ambient_params_t *params)
{
    if (params != NULL)
    {
        g_params = *params;
    }
}

const char *ambient_reverb_get_preset_name(ambient_preset_t preset)
{
    switch (preset)
    {
        case AMBIENT_PRESET_REVERSE_GATE:       return "1: SPX90 Reverse Gate (48kHz 32-tap FIR)";
        case AMBIENT_PRESET_EARLY_REFLECTIONS:  return "2: SPX90 Early Reflections (48kHz 32-tap FIR)";
        case AMBIENT_PRESET_CHORUS_3PHASE:      return "3: Hypnotic 3-Phase 6Hz Chorus + Hall";
        case AMBIENT_PRESET_SHIMMER_HALL:       return "4: 4x4 Hadamard FDN + Shimmer";
        case AMBIENT_PRESET_SHOEGAZE_CATHEDRAL: return "5: Shoegaze Cathedral (All-in 48kHz)";
        case AMBIENT_PRESET_BYPASS:             return "6: Bypass (Dry)";
        case AMBIENT_PRESET_OFF:                return "7: Mute / Off";
        default:                                return "Unknown";
    }
}

void ambient_reverb_process_stereo(int16_t x, int16_t *out_l, int16_t *out_r)
{
    ambient_preset_t preset = g_preset;
    if (preset == AMBIENT_PRESET_OFF)
    {
        *out_l = 0;
        *out_r = 0;
        return;
    }
    if (preset == AMBIENT_PRESET_BYPASS)
    {
        *out_l = x;
        *out_r = x;
        return;
    }

    int16_t tap_l = 0;
    int16_t tap_r = 0;
    int32_t wet_l = 0;
    int32_t wet_r = 0;

    ambient_params_t params = g_params;
    int32_t fb_g = (int32_t)params.decay;
    int32_t shim_g = (int32_t)params.shimmer;
    int32_t tap_g = (int32_t)params.tap_level;
    int32_t cho_g = (int32_t)params.chorus;
    uint8_t tap_mode = params.tap_mode;

    /* 1) 48 kHz 32 タップ FIR + 軽い 2 段オールパス拡散 */
    if ((tap_mode != 0U) && (tap_g > 0))
    {
        multitap_fir_step_48k(x, tap_mode, &tap_l, &tap_r);
        tap_l = allpass_light_step(g_diff_l0, DIFF_L0_LEN, &g_diff_idx[0], tap_l);
        tap_l = allpass_light_step(g_diff_l1, DIFF_L1_LEN, &g_diff_idx[1], tap_l);
        tap_r = allpass_light_step(g_diff_r0, DIFF_R0_LEN, &g_diff_idx[2], tap_r);
        tap_r = allpass_light_step(g_diff_r1, DIFF_R1_LEN, &g_diff_idx[3], tap_r);
        wet_l += ((int32_t)tap_l * tap_g) >> 8;
        wet_r += ((int32_t)tap_r * tap_g) >> 8;
    }

    /* 2) 48 kHz 4x4 Hadamard FDN */
    int32_t pre = (tap_mode != 0U)
                      ? ((((int32_t)tap_l + (int32_t)tap_r) >> 1) + ((int32_t)x >> 2))
                      : (int32_t)x;
    if (shim_g > 0)
    {
        pre += (g_shim_lp2 * shim_g) >> 8;
    }
    int16_t fdn_in = clamp_s16(pre);

    /* LFO モジュレーション (0..12 サンプル Q8 @ 48 kHz) */
    uint16_t tri = tri_u10(g_fdn_lfo);
    uint16_t mod_a = (uint16_t)(tri * 3U);
    uint16_t mod_b = (uint16_t)((1023U - tri) * 3U);

    int32_t y0 = fdn_read_mod(g_fdn0, FDN_LEN_0, g_fdn_idx[0], mod_a);
    int32_t y1 = fdn_read_mod(g_fdn1, FDN_LEN_1, g_fdn_idx[1], mod_b);
    int32_t y2 = fdn_read_mod(g_fdn2, FDN_LEN_2, g_fdn_idx[2], mod_a);
    int32_t y3 = fdn_read_mod(g_fdn3, FDN_LEN_3, g_fdn_idx[3], mod_b);

    /* 独立ダンピング LPF @ 48 kHz (fc ≒ 4.5..5.5 kHz) */
    g_fdn_damp[0] += ((y0 - g_fdn_damp[0]) * 145) >> 8;
    g_fdn_damp[1] += ((y1 - g_fdn_damp[1]) * 140) >> 8;
    g_fdn_damp[2] += ((y2 - g_fdn_damp[2]) * 135) >> 8;
    g_fdn_damp[3] += ((y3 - g_fdn_damp[3]) * 130) >> 8;

    y0 = g_fdn_damp[0];
    y1 = g_fdn_damp[1];
    y2 = g_fdn_damp[2];
    y3 = g_fdn_damp[3];

    int32_t m0 = (y0 + y1 + y2 + y3) >> 1;
    int32_t m1 = (y0 - y1 + y2 - y3) >> 1;
    int32_t m2 = (y0 + y1 - y2 - y3) >> 1;
    int32_t m3 = (y0 - y1 - y2 + y3) >> 1;

    int32_t d_in = (int32_t)fdn_in >> 1;
    g_fdn0[g_fdn_idx[0]] = clamp_s16(d_in + ((m0 * fb_g) >> 8));
    g_fdn1[g_fdn_idx[1]] = clamp_s16(-d_in + ((m1 * fb_g) >> 8));
    g_fdn2[g_fdn_idx[2]] = clamp_s16(d_in + ((m2 * fb_g) >> 8));
    g_fdn3[g_fdn_idx[3]] = clamp_s16(-d_in + ((m3 * fb_g) >> 8));

    if (++g_fdn_idx[0] >= FDN_LEN_0) g_fdn_idx[0] = 0U;
    if (++g_fdn_idx[1] >= FDN_LEN_1) g_fdn_idx[1] = 0U;
    if (++g_fdn_idx[2] >= FDN_LEN_2) g_fdn_idx[2] = 0U;
    if (++g_fdn_idx[3] >= FDN_LEN_3) g_fdn_idx[3] = 0U;

    int16_t fdn_l = clamp_s16(y0 - y1);
    int16_t fdn_r = clamp_s16(y2 - y3);

    if ((preset == AMBIENT_PRESET_REVERSE_GATE) || (preset == AMBIENT_PRESET_EARLY_REFLECTIONS))
    {
        wet_l += (int32_t)fdn_l >> 2;
        wet_r += (int32_t)fdn_r >> 2;
    }
    else if (preset == AMBIENT_PRESET_SHOEGAZE_CATHEDRAL)
    {
        wet_l = (wet_l >> 1) + ((int32_t)fdn_l >> 1);
        wet_r = (wet_r >> 1) + ((int32_t)fdn_r >> 1);
    }
    else
    {
        wet_l += (int32_t)fdn_l;
        wet_r += (int32_t)fdn_r;
    }

    /* 3) 48 kHz 酩酊 3 相 6 Hz コーラス */
    if (cho_g > 0)
    {
        int16_t cho_in = clamp_s16(((int32_t)x + ((wet_l + wet_r) >> 1)) >> 1);
        int16_t cho_l;
        int16_t cho_r;

        chorus_3phase_step_48k(cho_in, &cho_l, &cho_r);
        if (preset == AMBIENT_PRESET_CHORUS_3PHASE)
        {
            wet_l = ((int32_t)cho_l * 220 + (wet_l * 90)) >> 8;
            wet_r = ((int32_t)cho_r * 220 + (wet_r * 90)) >> 8;
        }
        else
        {
            wet_l = (wet_l >> 1) + (((int32_t)cho_l * cho_g) >> 9);
            wet_r = (wet_r >> 1) + (((int32_t)cho_r * cho_g) >> 9);
        }
    }

    /* 4) 48 kHz +1 オクターブ・シマー (独立 42.7 ms 窓 + DC ブロッカ + 2 段 LPF) */
    if (shim_g > 0)
    {
        int16_t mono_fdn = (int16_t)(((int32_t)fdn_l + (int32_t)fdn_r) >> 1);
        int16_t oct_up = shimmer_step_48k(mono_fdn);

        g_shim_dc += ((int32_t)oct_up - g_shim_dc) >> 6; /* fc ≒ 120 Hz @ 48 kHz */
        int32_t oct_hp = (int32_t)oct_up - g_shim_dc;

        g_shim_lp1 += (oct_hp - g_shim_lp1) >> 2;
        g_shim_lp2 += (g_shim_lp1 - g_shim_lp2) >> 2;
        if (g_shim_lp2 > 3500) g_shim_lp2 = 3500;
        if (g_shim_lp2 < -3500) g_shim_lp2 = -3500;

        wet_l += (g_shim_lp2 * shim_g) >> 8;
        wet_r += (g_shim_lp2 * shim_g) >> 8;
    }

    g_fdn_lfo += FDN_LFO_INC;

    uint8_t mix = params.mix;
    uint8_t dry_g = (uint8_t)(255U - mix);
    *out_l = clamp_s16((((int32_t)x * (int32_t)dry_g) + (wet_l * (int32_t)mix)) >> 8);
    *out_r = clamp_s16((((int32_t)x * (int32_t)dry_g) + (wet_r * (int32_t)mix)) >> 8);
}
