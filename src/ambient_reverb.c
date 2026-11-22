/**
 * @file ambient_reverb.c
 * @brief FRDM-MCXN947 48 kHz / 24-bit Q23 フルレート アンビエント／シューゲイザー DSP エンジン
 *
 * 512 KB SRAM の余裕を活かし、全ディレイバッファと全内部演算を符号付き 24-bit (Q23: ±8,388,607) 化。
 * 入力最前段 25 Hz DC ブロッカ、内部 -6 dB ヘッドルーム、Shimmer ソフトサチュレータ、
 * および最終段 -2.5 dBFS ニー付きソフトリミッタにより、PCM1808 経由のギター強ピッキング入力でも
 * 音質崩壊・デジタルクリップを防ぎつつ PCM5102A の 24-bit ダイナミックレンジをフル活用する。
 */

#include "ambient_reverb.h"
#include <string.h>
#include "cmsis_compiler.h"

/* 4x4 Hadamard FDN 素数遅延長 @ 48 kHz (31.0 / 36.5 / 42.8 / 49.6 ms) */
#define FDN_LEN_0        1487U
#define FDN_LEN_1        1753U
#define FDN_LEN_2        2053U
#define FDN_LEN_3        2381U

/* 前段 4 段カスケード拡散オールパス (非調和素数長 @ 48 kHz: 4.0 / 6.5 / 9.6 / 14.1 ms) */
#define PDIFF0_LEN        193U
#define PDIFF1_LEN        311U
#define PDIFF2_LEN        463U
#define PDIFF3_LEN        677U

/* 48 kHz フルレート 64 タップ (L/R 32+32) FIR ディレイライン (16384 サンプル = 341.3 ms) */
#define TAP_BUF_SIZE     16384U
#define TAP_BUF_MASK     (TAP_BUF_SIZE - 1U)
#define TAP_COUNT        32U

/* 48 kHz フルレート Dimension Chorus 専用バッファ (2048 サンプル = 42.7 ms, 純フィードフォワード) */
#define CHO_BUF_SIZE     2048U
#define CHO_BUF_MASK     (CHO_BUF_SIZE - 1U)

/* 48 kHz フルレート シマー専用バッファ (2048 サンプル = 42.7 ms 窓) */
#define SHIM_BUF_SIZE    2048U
#define SHIM_BUF_MASK    (SHIM_BUF_SIZE - 1U)

/* 24-bit Q23 範囲およびソフトリミッタ／サチュレータ閾値 */
#define Q23_MAX          8388607
#define Q23_MIN          (-8388608)
#define SHIM_SAT_THRESH  2621440    /* 内部 -10 dBFS (出力換算 -4 dBFS 相当) の滑らか飽和閾値 */
#define OUT_LIM_KNEE     6291456    /* 0.75 * 2^23 (-2.5 dBFS): ここまでは完全リニア */
#define OUT_LIM_ROOM     2097151    /* 0.25 * 2^23: 0 dBFS へ漸近圧縮するヘッドルーム */

/* LFO 位相増分 @ 48 kHz: round(f * 2^32 / 48000) */
#define FDN_LFO_INC        40265UL     /* 約 0.45 Hz */
#define SHIM_WOW_INC       25054UL     /* 0.28 Hz (3.57 秒周期: テープワウ風の儚い揺らぎ) */
#define DIM_LFO_INC        29528UL     /* 0.33 Hz (3.03 秒周期: DC-2 風 揺れない逆相ペア) */
#define PAN_RATE_MIN_INC   17896UL     /* 0.20 Hz (pan_rate = 0) */
#define PAN_RATE_STEP_INC  1120UL      /* pan_rate 1 刻みあたり +0.0125 Hz (255 で約 3.39 Hz) */
#define PAN_DRIFT_INC      17001UL     /* 0.19 Hz (5.26 秒周期: テンポ揺らぎによる酩酊感) */

/* ハーモニック・パンニング 2 次 Linkwitz-Riley (LR2) TPT SVF 係数 (fc = 600 Hz @ 48 kHz, Q28) */
#define PAN_SVF_A1_Q28     248522831LL /* 1 / (1 + g*(g + 2)), g = tan(pi*600/48000) */
#define PAN_SVF_A2_Q28     9764489LL   /* g * a1 */
#define PAN_SVF_A3_Q28     383648LL    /* g * a2 */
#define PAN_SVF_SHIFT      28
#define PAN_SVF_ROUND      (1LL << (PAN_SVF_SHIFT - 1))
#define PHASE_60_DEG       0x2AAAAAABUL
#define PHASE_90_DEG       0x40000000UL

/* SPX90 風 L/R 非対称 32+32 非周期素数反射タップ @ 48 kHz (19 ms .. 255 ms) */
static const uint16_t g_tap_dly_l[TAP_COUNT] = {
      907U,  1483U,  1663U,  2251U,  2591U,  3041U,  3463U,  3709U,
     4177U,  4733U,  5119U,  5431U,  5827U,  6229U,  6491U,  7039U,
     7219U,  7669U,  8069U,  8353U,  8627U,  9173U,  9439U,  9733U,
    10133U, 10337U, 10627U, 11131U, 11273U, 11519U, 11777U, 12203U
};

static const uint16_t g_tap_dly_r[TAP_COUNT] = {
      997U,  1423U,  1901U,  2237U,  2689U,  3137U,  3373U,  3793U,
     4327U,  4561U,  5011U,  5569U,  5737U,  6271U,  6673U,  7121U,
     7351U,  7621U,  7963U,  8563U,  8887U,  9187U,  9539U,  9721U,
    10037U, 10589U, 10691U, 11003U, 11329U, 11677U, 11887U, 12211U
};

/* Reverse Gate ゲイン: 非交互バランス極性で 255 ms のゲート端へ向かって滑らかに立ち上がり切れる */
static const int16_t g_gain_rev_gate[TAP_COUNT] = {
      28,  -29,   31,   34,  -38,  -42,   47,  -52,
     -58,   65,  -72,   79,   87,  -95,  104, -113,
    -123,  133,  143, -154,  165, -176, -188,  200,
     213, -226, -239,  253, -266,  281,  295, -310
};

/* Early Reflections ゲイン: 初期反射が明瞭に鳴り自然に減衰 */
static const int16_t g_gain_early_ref[TAP_COUNT] = {
     288, -274,  261,  247, -234, -222,  209, -197,
    -185,  174, -163,  152,  141, -131,  121, -112,
    -103,   94,   85,  -77,   69,  -62,  -55,   49,
      43,  -38,  -33,   28,  -24,   21,   19,  -18
};

/* 24-bit Q23 静的ディレイバッファ (合計 約 119 KB / 312 KB m_data) */
static int32_t g_fdn0[FDN_LEN_0];
static int32_t g_fdn1[FDN_LEN_1];
static int32_t g_fdn2[FDN_LEN_2];
static int32_t g_fdn3[FDN_LEN_3];
static int32_t g_pdiff0[PDIFF0_LEN];
static int32_t g_pdiff1[PDIFF1_LEN];
static int32_t g_pdiff2[PDIFF2_LEN];
static int32_t g_pdiff3[PDIFF3_LEN];
static int32_t g_tap_buf[TAP_BUF_SIZE];
static int32_t g_cho_buf[CHO_BUF_SIZE];
static int32_t g_shim_buf[SHIM_BUF_SIZE];

static int64_t g_in_dc_q16 = 0;
static uint16_t g_fdn_idx[4];
static int32_t g_fdn_damp[4];
static uint16_t g_pdiff_idx[4];
static uint16_t g_tap_wr = 0U;
static int32_t g_tap_hp_l = 0;
static int32_t g_tap_hp_r = 0;
static int32_t g_tap_lp_l = 0;
static int32_t g_tap_lp_r = 0;
static uint16_t g_cho_wr = 0U;
static uint16_t g_shim_wr = 0U;
static uint32_t g_shim_ph_q8 = 0UL;
static uint32_t g_shim_wow_lfo = 0UL;
static uint32_t g_tvoice_ph_q8[3] = { 0UL, 0UL, 0UL };
static int32_t g_tvoice_inc_q16[3] = { 128 << 8, 320 << 8, 765 << 8 };
static int32_t g_env_fast = 0;
static int32_t g_env_slow = 0;
static int32_t g_env_peak = 65536;
static int32_t g_bloom_q16 = 0;
static uint16_t g_atk_hold = 0U;
static uint16_t g_bloom_hold = 0U;
static uint8_t g_tension_voicing = 0U;
static int32_t g_fdn_hp = 0;
static int32_t g_cat_cho_hp = 0;
static int32_t g_shim_dc_l = 0;
static int32_t g_shim_dc_r = 0;
static int32_t g_shim_lp_l = 0;
static int32_t g_shim_lp_r = 0;

static int32_t g_dim_lp = 0;
static int32_t g_pan_svf_l_ic1 = 0;
static int32_t g_pan_svf_l_ic2 = 0;
static int32_t g_pan_svf_r_ic1 = 0;
static int32_t g_pan_svf_r_ic2 = 0;
static int32_t g_pan_mod_hi_sm = 0;
static int32_t g_pan_mod_lo_sm = 0;
static uint32_t g_fdn_lfo = 0UL;
static uint32_t g_dim_lfo = 0UL;
static uint32_t g_pan_lfo = 0UL;
static uint32_t g_pan_drift = 0UL;
static uint32_t g_pan_rng = 0x13579BDFUL;
static uint32_t g_pan_prev_hi_ph = 0UL;
static uint32_t g_pan_prev_lo_ph = 0UL;
static int32_t g_pan_rand_hi_cur = 0;
static int32_t g_pan_rand_hi_next = 256;
static int32_t g_pan_rand_lo_cur = 0;
static int32_t g_pan_rand_lo_next = -256;

static volatile ambient_preset_t g_preset = AMBIENT_PRESET_REVERSE_GATE;
static volatile ambient_params_t g_params = {
    110U, 0U, 255U, 0U, 165U, 1U, 0U, 36U, PAN_WAVE_SINE, PAN_WAVE_SINE, 0U, 0U
};

static const ambient_params_t g_preset_table[AMBIENT_PRESET_COUNT] = {
    /* decay, shim, tap, cho, mix, tap_m, pan_d, pan_r, hi_wave, lo_wave, tension, t_mode */
    { 110U,   0U, 255U,   0U, 165U, 1U,   0U,  36U, PAN_WAVE_SINE,          PAN_WAVE_SINE,        0U, 0U }, /* REVERSE_GATE */
    { 180U,   0U, 255U,   0U, 180U, 2U,   0U,  36U, PAN_WAVE_SINE,          PAN_WAVE_SINE,        0U, 0U }, /* EARLY_REFLECTIONS */
    { 218U,   0U,   0U, 255U, 148U, 0U, 235U,  34U, PAN_WAVE_SINE,          PAN_WAVE_SINE,        0U, 0U }, /* DIM + SINE/SINE */
    { 218U,   0U,   0U, 255U, 148U, 0U, 240U,  38U, PAN_WAVE_RAMP_UP,       PAN_WAVE_RAMP_DOWN,   0U, 0U }, /* DIM + CROSS-RAMP */
    { 224U,   0U,   0U, 255U, 148U, 0U, 240U,  48U, PAN_WAVE_SMOOTH_RANDOM, PAN_WAVE_SINE,        0U, 0U }, /* DIM + STARDUST */
    { 218U,   0U,   0U, 255U, 148U, 0U, 245U,  64U, PAN_WAVE_SOFT_SQUARE,   PAN_WAVE_SINE,        0U, 0U }, /* DIM + PSYCHE CHOP */
    { 236U, 130U,   0U,   0U, 190U, 0U,   0U,  36U, PAN_WAVE_SINE,          PAN_WAVE_SINE,        0U, 0U }, /* SHIMMER_HALL */
    { 234U, 155U, 215U, 125U, 175U, 1U, 180U,  32U, PAN_WAVE_SINE,          PAN_WAVE_SINE,        0U, 0U }, /* SHOEGAZE_CATHEDRAL */
    { 236U, 175U, 195U, 135U, 180U, 1U, 155U,  28U, PAN_WAVE_SINE,          PAN_WAVE_SINE,      250U, 1U }, /* TENSION_BLOOM */
    { 240U, 185U, 215U, 150U, 185U, 1U, 175U,  22U, PAN_WAVE_RAMP_UP,       PAN_WAVE_SINE,      255U, 2U }, /* RUIN_CATHEDRAL */
    {   0U,   0U,   0U,   0U,   0U, 0U,   0U,   0U, PAN_WAVE_SINE,          PAN_WAVE_SINE,        0U, 0U }, /* BYPASS */
    {   0U,   0U,   0U,   0U,   0U, 0U,   0U,   0U, PAN_WAVE_SINE,          PAN_WAVE_SINE,        0U, 0U }  /* OFF */
};

static inline int32_t clamp_s24(int32_t v)
{
#if defined(__clang__) && !__has_builtin(__builtin_arm_ssat)
    if (v > Q23_MAX)
    {
        return Q23_MAX;
    }
    if (v < Q23_MIN)
    {
        return Q23_MIN;
    }
    return v;
#else
    return __SSAT(v, 24);
#endif
}

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

/* 64-bit 中間積によるオーバーフロー安全な Q8 乗算: (a * b) >> 8 */
static inline int32_t mul_q8(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 8);
}

/* 滑らかなソフトサチュレータ (32-bit ハードウェア UDIV + SMULL で高速化) */
static inline int32_t soft_sat_s24(int32_t v, int32_t thresh)
{
    uint32_t av = (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    uint32_t denom = ((uint32_t)thresh + av) >> 8;
    if (denom == 0U)
    {
        return 0;
    }
    uint32_t gain_q15 = ((uint32_t)thresh << 7) / denom;
    if (gain_q15 > 32768U)
    {
        gain_q15 = 32768U;
    }
    int32_t mag = (int32_t)(((int64_t)av * (int64_t)gain_q15) >> 15);
    return (v < 0) ? -mag : mag;
}

/* 最終段 -2.5 dBFS ニー付きソフトリミッタ (|v| <= -2.5 dBFS は無歪み、超過分のみ 0 dBFS へ漸近圧縮) */
static inline int32_t soft_limit_s24(int32_t v)
{
    uint32_t av = (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    if (av <= (uint32_t)OUT_LIM_KNEE)
    {
        return v;
    }
    uint32_t excess = av - (uint32_t)OUT_LIM_KNEE;
    uint32_t denom = ((uint32_t)OUT_LIM_ROOM + excess) >> 8;
    uint32_t gain_q15 = ((uint32_t)OUT_LIM_ROOM << 7) / denom;
    if (gain_q15 > 32768U)
    {
        gain_q15 = 32768U;
    }
    int32_t comp = (int32_t)(((int64_t)excess * (int64_t)gain_q15) >> 15);
    int32_t out = clamp_s24(OUT_LIM_KNEE + comp);
    return (v < 0) ? -out : out;
}

static inline uint16_t tri_u10(uint32_t ph)
{
    uint16_t p = (uint16_t)(ph >> 21);
    return (p < 1024U) ? p : (uint16_t)(2047U - p);
}

/* 折り返し点の角が無い滑らかなサイン近似 LFO (出力 0..1023) */
static inline uint16_t sine_u10(uint32_t ph)
{
    uint32_t p = ph >> 22;
    uint32_t q = p & 511U;
    uint32_t val = (q * (512U - q)) >> 7; /* 0..512 */

    if (p < 512U)
    {
        uint32_t y = 512U + val;
        return (uint16_t)((y > 1023U) ? 1023U : y);
    }
    return (uint16_t)(512U - val);
}

static inline int32_t allpass_dense_step(int32_t *buf, uint16_t len, uint16_t *p_idx, int32_t in, int32_t g_q8)
{
    uint16_t idx = *p_idx;
    int32_t b = buf[idx];
    int32_t v = in + mul_q8(b, g_q8);
    int32_t out = b - mul_q8(v, g_q8);

    buf[idx] = clamp_s24(v);
    if (++idx >= len)
    {
        idx = 0U;
    }
    *p_idx = idx;
    return clamp_s24(out);
}

/* モジュレーション付き拡散オールパス (定常和音での静的コム共振を溶かす) */
static inline int32_t allpass_mod_step(int32_t *buf, uint16_t len, uint16_t *p_idx, int32_t in, uint16_t mod_q8, int32_t g_q8)
{
    uint16_t idx = *p_idx;
    uint16_t offset = (uint16_t)(mod_q8 >> 8);
    uint16_t frac = (uint16_t)(mod_q8 & 0xFFU);
    uint16_t r0 = idx + offset;
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
    int32_t b = s0 + mul_q8(s1 - s0, (int32_t)frac);
    int32_t v = in + mul_q8(b, g_q8);
    int32_t out = b - mul_q8(v, g_q8);

    buf[idx] = clamp_s24(v);
    if (++idx >= len)
    {
        idx = 0U;
    }
    *p_idx = idx;
    return clamp_s24(out);
}

/*
 * 48 kHz / 24-bit フルレート SPX90 風 64 タップ (L/R 32+32) Reverse Gate / Early Reflections
 * - 前段カスケード拡散オールパスで入力を濃密な音の雲へ変えてから FIR へ書き込むことで、
 *   パタパタした粒立ちと金属的コム共振を消しつつ 255 ms ゲート端の切れ味を維持する
 * - 出力段に SPX90 風の軽いトーンシェーピング (HPF ≒ 140 Hz, LPF ≒ 10 kHz) を適用
 */
static void multitap_fir_step_48k(int32_t in, uint8_t tap_mode, int32_t *out_l, int32_t *out_r)
{
    const int16_t *gains = (tap_mode == 1U) ? g_gain_rev_gate : g_gain_early_ref;
    int32_t d = allpass_dense_step(g_pdiff0, PDIFF0_LEN, &g_pdiff_idx[0], in, 152);
    d = allpass_dense_step(g_pdiff1, PDIFF1_LEN, &g_pdiff_idx[1], d, 152);

    if (tap_mode == 1U)
    {
        /* Reverse Gate は 4 段フル拡散 + 微弱モジュレーションでベルベット状の音の壁を作る */
        uint16_t mod_q8 = (uint16_t)(sine_u10(g_fdn_lfo + PHASE_90_DEG) * 2U); /* 0..8 サンプル Q8 */
        d = allpass_dense_step(g_pdiff2, PDIFF2_LEN, &g_pdiff_idx[2], d, 160);
        d = allpass_mod_step(g_pdiff3, PDIFF3_LEN, &g_pdiff_idx[3], d, mod_q8, 144);
    }

    int64_t sum_l = 0;
    int64_t sum_r = 0;

    g_tap_buf[g_tap_wr] = d;

    for (uint8_t k = 0U; k < TAP_COUNT; k++)
    {
        int64_t g = (int64_t)gains[k];
        sum_l += (int64_t)g_tap_buf[(g_tap_wr - g_tap_dly_l[k]) & TAP_BUF_MASK] * g;
        sum_r += (int64_t)g_tap_buf[(g_tap_wr - g_tap_dly_r[k]) & TAP_BUF_MASK] * g;
    }

    g_tap_wr = (g_tap_wr + 1U) & TAP_BUF_MASK;

    int32_t raw_l = (int32_t)((sum_l * 7LL) >> 12);
    int32_t raw_r = (int32_t)((sum_r * 7LL) >> 12);

    /* SPX90 風トーンシェーピング: HPF ≒ 140 Hz (5/256) + LPF ≒ 10 kHz (190/256) */
    g_tap_hp_l += mul_q8(raw_l - g_tap_hp_l, 5);
    g_tap_hp_r += mul_q8(raw_r - g_tap_hp_r, 5);
    int32_t hp_l = raw_l - g_tap_hp_l;
    int32_t hp_r = raw_r - g_tap_hp_r;

    g_tap_lp_l += mul_q8(hp_l - g_tap_lp_l, 190);
    g_tap_lp_r += mul_q8(hp_r - g_tap_lp_r, 190);

    *out_l = clamp_s24(g_tap_lp_l);
    *out_r = clamp_s24(g_tap_lp_r);
}

static inline int32_t cho_read_q8(uint32_t dly_q8)
{
    uint16_t di = (uint16_t)(dly_q8 >> 8);
    uint16_t fr = (uint16_t)(dly_q8 & 0xFFU);
    uint16_t r0 = (g_cho_wr - di) & CHO_BUF_MASK;
    uint16_t r1 = (r0 - 1U) & CHO_BUF_MASK;
    int32_t s0 = g_cho_buf[r0];
    int32_t s1 = g_cho_buf[r1];

    return s0 + mul_q8(s1 - s0, (int32_t)fr);
}

/*
 * 48 kHz / 24-bit フルレート Dimension Chorus (Roland SDD-320 / BOSS DC-2 方式, Pure Wet 出力)
 * - フィードバック 0 の純フィードフォワード + BBD 風ローパス (fc ≒ 9.0 kHz)
 * - 0.33 Hz 逆相ペア × 2 系統 (90° 位相差, ±4.5 cents) に左右相補の +2.5 ms 中心オフセットを与え、
 *   LFO 交差点での Side 成分消滅を防ぎながら常に極ワイドな広がりを維持する
 */
static void dimension_chorus_step_48k(int32_t in, int32_t *out_l, int32_t *out_r)
{
    uint32_t t_a = (uint32_t)tri_u10(g_dim_lfo);
    uint32_t t_b = 1023U - t_a;
    uint32_t t_c = (uint32_t)tri_u10(g_dim_lfo + PHASE_90_DEG);
    uint32_t t_d = 1023U - t_c;

    /*
     * 中心オフセット付き 4 タップ (L/R 平均遅延はいずれも 10.75 ms で完全対称):
     *   a:  5.0.. 9.0 ms (中心  7.0 ms, ±2.0 ms)
     *   b:  7.5..11.5 ms (中心  9.5 ms, ±2.0 ms, a と逆相)
     *   c: 12.5..16.5 ms (中心 14.5 ms, ±2.0 ms, a と 90° ズレ)
     *   d: 10.0..14.0 ms (中心 12.0 ms, ±2.0 ms, c と逆相)
     */
    int32_t a = cho_read_q8((240UL << 8) + (t_a * 48UL));
    int32_t b = cho_read_q8((360UL << 8) + (t_b * 48UL));
    int32_t c = cho_read_q8((600UL << 8) + (t_c * 48UL));
    int32_t d = cho_read_q8((480UL << 8) + (t_d * 48UL));

    /* BBD 風ウォーム LPF (fc ≒ 9.0 kHz) を通して純フィードフォワード書き込み */
    g_dim_lp += mul_q8(in - g_dim_lp, 180);
    g_cho_buf[g_cho_wr] = clamp_s24(g_dim_lp);
    g_cho_wr = (g_cho_wr + 1U) & CHO_BUF_MASK;
    g_dim_lfo += DIM_LFO_INC;

    /* クロスブレンド (Pure Wet): Mid でピッチ相殺しつつ Side へ濃密に展開 */
    int32_t w_l = (int32_t)(((int64_t)a * 3LL + (int64_t)c * 2LL - (int64_t)b * 2LL - (int64_t)d) >> 2);
    int32_t w_r = (int32_t)(((int64_t)b * 3LL + (int64_t)d * 2LL - (int64_t)a * 2LL - (int64_t)c) >> 2);

    *out_l = clamp_s24(mul_q8(w_l, 290));
    *out_r = clamp_s24(mul_q8(w_r, 290));
}

static inline int32_t pan_rng_next_s10(void)
{
    uint32_t x = g_pan_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_pan_rng = x;
    return (int32_t)(x & 1023U) - 512; /* -512..+511 */
}

/* 各種パンニング LFO 波形を評価して -512..+511 (+が左寄り, -が右寄り) を返す */
static inline int32_t eval_pan_wave_s10(uint32_t ph, uint8_t wave, int32_t r_cur, int32_t r_next)
{
    switch ((ambient_pan_wave_t)wave)
    {
        case PAN_WAVE_TRIANGLE:
            return (int32_t)tri_u10(ph) - 512;

        case PAN_WAVE_RAMP_UP:
            return (int32_t)(ph >> 22) - 512;

        case PAN_WAVE_RAMP_DOWN:
            return 511 - (int32_t)(ph >> 22);

        case PAN_WAVE_SOFT_SQUARE:
        {
            int32_t v = ((int32_t)sine_u10(ph) - 512) * 3;
            if (v > 511) v = 511;
            if (v < -512) v = -512;
            return v;
        }

        case PAN_WAVE_SMOOTH_RANDOM:
        {
            int32_t frac = (int32_t)(ph >> 22); /* 0..1023 */
            return r_cur + (((r_next - r_cur) * frac) >> 10);
        }

        case PAN_WAVE_SINE:
        default:
            return (int32_t)sine_u10(ph) - 512;
    }
}

/*
 * 24-bit Q23 2 次 Linkwitz-Riley (LR2) TPT SVF クロスオーバー (fc = 600 Hz @ 48 kHz, Q = 0.5)
 * - 1 本の SVF で対称な -12 dB/oct の LPF / HPF を同時出力 (600 Hz で両方 -6.02 dB)
 * - LR2 は全周波数で LPF/HPF が 180° 逆相のため、HPF 極性反転 (hi_inv) を返すことで
 *   lo + hi_inv が全帯域 0.000 dB フラット (オールパス合成) になる
 */
static inline void lr2_svf_split(int32_t in,
                                 int32_t *ic1eq,
                                 int32_t *ic2eq,
                                 int32_t *out_lo,
                                 int32_t *out_hi_inv)
{
    int32_t ic1 = *ic1eq;
    int32_t ic2 = *ic2eq;
    int32_t v3 = in - ic2;

    int32_t v1 = (int32_t)((((int64_t)PAN_SVF_A1_Q28 * ic1)
                          + ((int64_t)PAN_SVF_A2_Q28 * v3)
                          + PAN_SVF_ROUND) >> PAN_SVF_SHIFT);
    int32_t v2 = ic2 + (int32_t)((((int64_t)PAN_SVF_A2_Q28 * ic1)
                                + ((int64_t)PAN_SVF_A3_Q28 * v3)
                                + PAN_SVF_ROUND) >> PAN_SVF_SHIFT);

    *ic1eq = (v1 << 1) - ic1;
    *ic2eq = (v2 << 1) - ic2;

    *out_lo = v2;
    *out_hi_inv = (v2 + (v1 << 1)) - in;
}

/*
 * 48 kHz / 24-bit フルレート 高域/低域独立波形ハーモニック・パンニング・トレモロ
 * - 2 次 Linkwitz-Riley (LR2) TPT SVF クロスオーバー (-12 dB/oct, fc = 600 Hz) で低域/高域を対称分離
 * - 高域と低域それぞれに任意のカーブ波形を選択可能 (低域は 60° 位相オフセット)
 * - 波形出力へ約 5.3 ms の 1 次スムージング (>> 8) を挟み、折り返しクリックを除去
 */
static void harmonic_pan_tremolo_step_48k(int32_t *io_l,
                                          int32_t *io_r,
                                          int32_t depth,
                                          uint8_t rate,
                                          uint8_t hi_wave,
                                          uint8_t lo_wave)
{
    int32_t lo_l, hi_l;
    int32_t lo_r, hi_r;

    lr2_svf_split(*io_l, &g_pan_svf_l_ic1, &g_pan_svf_l_ic2, &lo_l, &hi_l);
    lr2_svf_split(*io_r, &g_pan_svf_r_ic1, &g_pan_svf_r_ic2, &lo_r, &hi_r);

    /* 0.19 Hz ドリフトで旋回位相を ±45° 緩やかに揺らし機械的な単調さを消す */
    uint32_t drift_ph = ((uint32_t)sine_u10(g_pan_drift) - 512U) << 20;
    uint32_t ph_hi = g_pan_lfo + drift_ph;
    uint32_t ph_lo = ph_hi + PHASE_60_DEG;

    /* 周期ラップごとに独立ランダム目標値を更新 (Smooth Random 用) */
    if (ph_hi < g_pan_prev_hi_ph)
    {
        g_pan_rand_hi_cur = g_pan_rand_hi_next;
        g_pan_rand_hi_next = pan_rng_next_s10();
    }
    if (ph_lo < g_pan_prev_lo_ph)
    {
        g_pan_rand_lo_cur = g_pan_rand_lo_next;
        g_pan_rand_lo_next = pan_rng_next_s10();
    }
    g_pan_prev_hi_ph = ph_hi;
    g_pan_prev_lo_ph = ph_lo;

    int32_t raw_hi = eval_pan_wave_s10(ph_hi, hi_wave, g_pan_rand_hi_cur, g_pan_rand_hi_next);
    int32_t raw_lo = eval_pan_wave_s10(ph_lo, lo_wave, g_pan_rand_lo_cur, g_pan_rand_lo_next);

    /* Q16 精度で τ ≒ 5.3 ms のスルー制限を掛けてキレを保ちつつ折り返しクリックを除去 */
    g_pan_mod_hi_sm += ((raw_hi << 8) - g_pan_mod_hi_sm) >> 8;
    g_pan_mod_lo_sm += ((raw_lo << 8) - g_pan_mod_lo_sm) >> 8;

    int32_t sm_hi = g_pan_mod_hi_sm >> 8; /* -512..+511 */
    int32_t sm_lo = g_pan_mod_lo_sm >> 8; /* -512..+511 */

    int32_t mod_hi = (sm_hi * depth * 7) >> 12;
    int32_t mod_lo = (((sm_lo * depth * 7) >> 12) * 7) >> 3;

    int32_t base = 224;
    int32_t g_hi_l = base + mod_hi;
    int32_t g_hi_r = base - mod_hi;
    int32_t g_lo_l = base + mod_lo;
    int32_t g_lo_r = base - mod_lo;

    if (g_hi_l < 8) g_hi_l = 8;
    if (g_hi_r < 8) g_hi_r = 8;
    if (g_lo_l < 8) g_lo_l = 8;
    if (g_lo_r < 8) g_lo_r = 8;

    *io_l = (int32_t)((((int64_t)hi_l * g_hi_l) + ((int64_t)lo_l * g_lo_l)) >> 8);
    *io_r = (int32_t)((((int64_t)hi_r * g_hi_r) + ((int64_t)lo_r * g_lo_r)) >> 8);

    g_pan_lfo += PAN_RATE_MIN_INC + ((uint32_t)rate * PAN_RATE_STEP_INC);
    g_pan_drift += PAN_DRIFT_INC;
}

static inline int32_t shim_read_q8(uint32_t dly_q8)
{
    uint16_t di = (uint16_t)(dly_q8 >> 8);
    uint16_t fr = (uint16_t)(dly_q8 & 0xFFU);
    uint16_t r0 = (g_shim_wr - di) & SHIM_BUF_MASK;
    uint16_t r1 = (r0 - 1U) & SHIM_BUF_MASK;
    int32_t s0 = g_shim_buf[r0];
    int32_t s1 = g_shim_buf[r1];

    return s0 + mul_q8(s1 - s0, (int32_t)fr);
}

static inline int32_t shim_voice_eval(uint32_t *p_ph_q8, uint32_t inc_q8)
{
    const uint32_t mask_q8 = ((uint32_t)SHIM_BUF_SIZE << 8) - 1UL;
    const uint32_t half_q8 = (uint32_t)(SHIM_BUF_SIZE >> 1) << 8;

    uint32_t ph = *p_ph_q8;
    uint32_t d_a_q8 = mask_q8 - ph;
    uint32_t d_b_q8 = (d_a_q8 + half_q8) & mask_q8;

    uint16_t d_a_i = (uint16_t)(d_a_q8 >> 8);
    uint16_t w_a = (d_a_i < (SHIM_BUF_SIZE >> 1)) ? d_a_i : ((SHIM_BUF_SIZE - 1U) - d_a_i);
    uint16_t w_b = ((SHIM_BUF_SIZE >> 1) - 1U) - w_a;

    int32_t sa = shim_read_q8(d_a_q8);
    int32_t sb = shim_read_q8(d_b_q8);
    int64_t out = ((int64_t)sa * (int64_t)w_a) + ((int64_t)sb * (int64_t)w_b);

    *p_ph_q8 = (ph + inc_q8) & mask_q8;
    return clamp_s24((int32_t)(out >> 10));
}

/*
 * 入力エンベロープ・アタック検出 & ボイシング変態エンジン
 * - mode == 1 (Choir Bloom): add9 / sus4 / 6-9 / m7 を軽く下からすくい上げてポルタメント着地
 * - mode == 2 (Ruin Cathedral): maj7 + 微デチューン亡霊聖歌隊 + maj14/#11 を、
 *   アタックのたびに地の底 (原音近傍) から約 0.9 秒かけて天窓へ這い上がらせる
 */
static int32_t update_tension_bloom_48k(int32_t x_in, uint8_t tension_mode)
{
    static const int16_t k_bloom_inc_q8[4][3] = {
        { 128, 320, 765 }, /* 0: Lydian add9    (+5th 3/2,  +9th 9/4,   +2oct) */
        {  85, 427, 510 }, /* 1: Suspended 11th (sus4 4/3,  +11th 8/3,  +12th 3/1) */
        { 171, 318, 765 }, /* 2: Melancholy 6/9 (+6th 5/3,  +9th 9/4,   +2oct) */
        { 205, 510, 765 }  /* 3: Dreamy m7      (m7 9/5,    +12th 3/1,  +2oct) */
    };
    static const int16_t k_ruin_inc_q8[4][3] = {
        { 224, 261, 704 }, /* 0: Twin Choir + maj7  (maj7 15/8,  +1oct+16c,  maj14 15/4) */
        { 224, 320, 776 }, /* 1: Cathedral maj9     (maj7 15/8,  +9th 9/4,   +2oct+18c) */
        { 249, 262, 704 }, /* 2: Ghost Choir Detune (+1oct-22c,  +1oct+20c,  maj14 15/4) */
        { 224, 464, 757 }  /* 3: Lydian Ruin #11    (maj7 15/8,  #11 45/16,  +2oct-18c) */
    };
    const int16_t (*voicing_tbl)[3] = (tension_mode == 2U) ? k_ruin_inc_q8 : k_bloom_inc_q8;

    int32_t ax = (x_in < 0) ? -x_in : x_in;
    if (ax > g_env_fast)
    {
        g_env_fast += (ax - g_env_fast) >> 8;   /* 約 5.3 ms アタック */
    }
    else
    {
        g_env_fast += (ax - g_env_fast) >> 12;  /* 約 85 ms リリース (和音ビートを平滑化) */
    }
    g_env_slow += (g_env_fast - g_env_slow) >> 12; /* g_env_fast の遅延追従: 減衰・定常時は必ず fast <= slow */

    if (g_env_fast > g_env_peak)
    {
        g_env_peak = g_env_fast;
    }
    else if (g_env_peak > 65536)
    {
        g_env_peak -= (g_env_peak >> 17) + 1;
    }

    if (g_atk_hold > 0U)
    {
        g_atk_hold--;
    }
    if (g_bloom_hold > 0U)
    {
        g_bloom_hold--;
    }

    /* 新規ピッキング／ストラムによる急峻な立ち上がりのみを百発百中検出 */
    if ((g_atk_hold == 0U) &&
        (g_env_fast > 48000) &&
        (g_env_fast > (g_env_slow + (g_env_peak >> 2))))
    {
        g_tension_voicing = (uint8_t)((g_tension_voicing + 1U) & 3U);
        const int16_t *tgt = voicing_tbl[g_tension_voicing];

        if (tension_mode == 2U)
        {
            /* Ruin Cathedral: 地の底 (ユニゾン近傍〜低音域) から天窓へ壮大に這い上がる */
            g_tvoice_inc_q16[0] = 12 << 8;
            g_tvoice_inc_q16[1] = 28 << 8;
            g_tvoice_inc_q16[2] = 85 << 8;
            g_atk_hold = 19200U;   /* 400 ms */
            g_bloom_hold = 26400U; /* 550 ms 上昇行程の間しっかり開花保持 */
        }
        else
        {
            /* Choir Bloom: 少し下からすくい上げるクワイア・グライド (元の P9 仕様) */
            g_tvoice_inc_q16[0] = ((int32_t)tgt[0] - 20) << 8;
            g_tvoice_inc_q16[1] = ((int32_t)tgt[1] - 28) << 8;
            g_tvoice_inc_q16[2] = ((int32_t)tgt[2] - 40) << 8;
            g_atk_hold = 19200U;   /* 400 ms */
            g_bloom_hold = 16800U; /* 350 ms 開花ブースト保持 */
        }
    }

    uint32_t rel_q10 = ((uint32_t)g_env_fast << 10) / (uint32_t)g_env_peak;
    if (rel_q10 > 1024U)
    {
        rel_q10 = 1024U;
    }
    int32_t target_q16 = (int32_t)(rel_q10 << 6);
    int32_t hold_floor = (tension_mode == 2U) ? (920 << 6) : (860 << 6);
    if ((g_bloom_hold > 0U) && (target_q16 < hold_floor))
    {
        target_q16 = hold_floor;
    }

    if (target_q16 > g_bloom_q16)
    {
        g_bloom_q16 += (target_q16 - g_bloom_q16) >> 13;
    }
    else
    {
        g_bloom_q16 += (target_q16 - g_bloom_q16) >> 15;
    }

    const int16_t *tgt = voicing_tbl[g_tension_voicing];
    uint8_t glide_shift = (tension_mode == 2U) ? 14U : 12U; /* Ruin: τ≒341ms / Bloom: τ≒85ms */
    for (uint8_t k = 0U; k < 3U; k++)
    {
        int32_t diff = ((int32_t)tgt[k] << 8) - g_tvoice_inc_q16[k];
        int32_t step = diff >> glide_shift;
        if ((step == 0) && (diff != 0))
        {
            step = (diff > 0) ? 1 : -1;
        }
        g_tvoice_inc_q16[k] += step;
    }

    return g_bloom_q16 >> 6; /* 0..1024 (Q10) */
}

/*
 * 48 kHz / 24-bit ノスタルジック & 4 ボイス変態テンション・シマー
 * - tension_mode == 0: 微弱フラット (-10 cents) + 0.28 Hz テープワウの儚い単一 +1 oct
 * - tension_mode == 1 (Choir Bloom): +1 oct 軸 + 9th/sus4/6-9/m7 クワイアグライド開花
 * - tension_mode == 2 (Ruin Cathedral): 深い亡霊デチューン聖歌隊 + maj7/maj14/#11 下から這い上がり
 */
static void shimmer_poly_step_48k(int32_t in,
                                  int32_t x_in,
                                  uint8_t tension,
                                  uint8_t tension_mode,
                                  int32_t *out_l,
                                  int32_t *out_r)
{
    int32_t wow = ((int32_t)sine_u10(g_shim_wow_lfo) - 512) >> 8; /* -2..+1 */
    g_shim_wow_lfo += SHIM_WOW_INC;

    uint32_t base_inc = (tension_mode == 2U) ? (uint32_t)(251 + (wow * 2)) : (uint32_t)(253 + wow);
    int32_t v0 = shim_voice_eval(&g_shim_ph_q8, base_inc);

    if ((tension == 0U) || (tension_mode == 0U))
    {
        g_shim_buf[g_shim_wr] = in;
        g_shim_wr = (g_shim_wr + 1U) & SHIM_BUF_MASK;
        *out_l = v0;
        *out_r = v0;
        return;
    }

    int32_t bloom_q10 = update_tension_bloom_48k(x_in, tension_mode);
    int32_t t_gain_q8 = (bloom_q10 * (int32_t)tension) >> 10;
    int32_t crown_q8 = (((bloom_q10 * bloom_q10) >> 10) * (int32_t)tension) >> 10;

    int32_t det = (tension_mode == 2U) ? (wow * 3) : wow;
    uint32_t inc1 = (uint32_t)((g_tvoice_inc_q16[0] >> 8) + det);
    uint32_t inc2 = (uint32_t)((g_tvoice_inc_q16[1] >> 8) - det);
    uint32_t inc3 = (uint32_t)((g_tvoice_inc_q16[2] >> 8) + (det * 2));

    int32_t v1 = shim_voice_eval(&g_tvoice_ph_q8[0], inc1);
    int32_t v2 = shim_voice_eval(&g_tvoice_ph_q8[1], inc2);
    int32_t v3 = shim_voice_eval(&g_tvoice_ph_q8[2], inc3);

    g_shim_buf[g_shim_wr] = in;
    g_shim_wr = (g_shim_wr + 1U) & SHIM_BUF_MASK;

    int32_t ten_l = mul_q8(v1, t_gain_q8) + (mul_q8(v2, t_gain_q8) >> 2);
    int32_t ten_r = mul_q8(v2, t_gain_q8) + (mul_q8(v1, t_gain_q8) >> 2);
    int32_t crown = mul_q8(v3, crown_q8) >> 1;

    *out_l = clamp_s24((v0 >> 1) + ten_l + crown);
    *out_r = clamp_s24((v0 >> 1) + ten_r + crown);
}

static inline int32_t fdn_read_mod(const int32_t *buf, uint16_t len, uint16_t wr_idx, uint16_t mod_q8)
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
    return s0 + mul_q8(s1 - s0, (int32_t)frac);
}

static void clear_buffers(void)
{
    memset(g_fdn0, 0, sizeof(g_fdn0));
    memset(g_fdn1, 0, sizeof(g_fdn1));
    memset(g_fdn2, 0, sizeof(g_fdn2));
    memset(g_fdn3, 0, sizeof(g_fdn3));
    memset(g_pdiff0, 0, sizeof(g_pdiff0));
    memset(g_pdiff1, 0, sizeof(g_pdiff1));
    memset(g_pdiff2, 0, sizeof(g_pdiff2));
    memset(g_pdiff3, 0, sizeof(g_pdiff3));
    memset(g_tap_buf, 0, sizeof(g_tap_buf));
    memset(g_cho_buf, 0, sizeof(g_cho_buf));
    memset(g_shim_buf, 0, sizeof(g_shim_buf));

    memset(g_fdn_idx, 0, sizeof(g_fdn_idx));
    memset(g_fdn_damp, 0, sizeof(g_fdn_damp));
    memset(g_pdiff_idx, 0, sizeof(g_pdiff_idx));

    g_in_dc_q16 = 0;
    g_tap_wr = 0U;
    g_tap_hp_l = 0;
    g_tap_hp_r = 0;
    g_tap_lp_l = 0;
    g_tap_lp_r = 0;
    g_cho_wr = 0U;
    g_shim_wr = 0U;
    g_shim_ph_q8 = 0UL;
    g_shim_wow_lfo = 0UL;
    g_tvoice_ph_q8[0] = 0UL;
    g_tvoice_ph_q8[1] = 0x20000UL;
    g_tvoice_ph_q8[2] = 0x40000UL;
    g_tvoice_inc_q16[0] = 128 << 8;
    g_tvoice_inc_q16[1] = 320 << 8;
    g_tvoice_inc_q16[2] = 765 << 8;
    g_env_fast = 0;
    g_env_slow = 0;
    g_env_peak = 65536;
    g_bloom_q16 = 0;
    g_atk_hold = 0U;
    g_bloom_hold = 0U;
    g_tension_voicing = 0U;
    g_fdn_hp = 0;
    g_cat_cho_hp = 0;
    g_shim_dc_l = 0;
    g_shim_dc_r = 0;
    g_shim_lp_l = 0;
    g_shim_lp_r = 0;
    g_dim_lp = 0;
    g_pan_svf_l_ic1 = 0;
    g_pan_svf_l_ic2 = 0;
    g_pan_svf_r_ic1 = 0;
    g_pan_svf_r_ic2 = 0;
    g_pan_mod_hi_sm = 0;
    g_pan_mod_lo_sm = 0;
    g_fdn_lfo = 0UL;
    g_dim_lfo = 0UL;
    g_pan_lfo = 0UL;
    g_pan_drift = 0UL;
    g_pan_prev_hi_ph = 0UL;
    g_pan_prev_lo_ph = PHASE_60_DEG;
    g_pan_rand_hi_cur = 0;
    g_pan_rand_hi_next = 256;
    g_pan_rand_lo_cur = 0;
    g_pan_rand_lo_next = -256;
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

const char *ambient_reverb_get_pan_wave_name(ambient_pan_wave_t wave)
{
    switch (wave)
    {
        case PAN_WAVE_SINE:          return "Sine";
        case PAN_WAVE_TRIANGLE:      return "Triangle";
        case PAN_WAVE_RAMP_UP:       return "RampUp";
        case PAN_WAVE_RAMP_DOWN:     return "RampDown";
        case PAN_WAVE_SOFT_SQUARE:   return "SoftSquare";
        case PAN_WAVE_SMOOTH_RANDOM: return "SmoothRand";
        default:                     return "Unknown";
    }
}

const char *ambient_reverb_get_preset_name(ambient_preset_t preset)
{
    switch (preset)
    {
        case AMBIENT_PRESET_REVERSE_GATE:       return "1: SPX90 Reverse Gate (48kHz 64-tap Diffused)";
        case AMBIENT_PRESET_EARLY_REFLECTIONS:  return "2: SPX90 Early Reflections (48kHz 64-tap FIR)";
        case AMBIENT_PRESET_CHORUS_3PHASE:      return "3: Dimension + Harmonic Pan (Sine x Sine)";
        case AMBIENT_PRESET_PAN_CROSS_RAMP:     return "4: Dimension + Infinite Cross-Ramp (Up x Down)";
        case AMBIENT_PRESET_PAN_STARDUST:       return "5: Dimension + Stardust Pan (Rand x Sine)";
        case AMBIENT_PRESET_PAN_PSYCHE_CHOP:    return "6: Dimension + Psyche Chop (SoftSq x Sine)";
        case AMBIENT_PRESET_SHIMMER_HALL:       return "7: 4x4 Hadamard FDN + Shimmer";
        case AMBIENT_PRESET_SHOEGAZE_CATHEDRAL: return "8: Shoegaze Cathedral (All-in 48kHz)";
        case AMBIENT_PRESET_TENSION_BLOOM:      return "9: Tension Choir Bloom (+1/+2oct & 9th/sus4)";
        case AMBIENT_PRESET_RUIN_CATHEDRAL:     return "10: Ruin Cathedral (maj7 + Rising Detune Choir)";
        case AMBIENT_PRESET_BYPASS:             return "11: Bypass (Dry)";
        case AMBIENT_PRESET_OFF:                return "12: Mute / Off";
        default:                                return "Unknown";
    }
}

void ambient_reverb_process_stereo_q23(int32_t x, int32_t *out_l, int32_t *out_r)
{
    ambient_preset_t preset = g_preset;
    if (preset == AMBIENT_PRESET_OFF)
    {
        *out_l = 0;
        *out_r = 0;
        return;
    }

    /* 入力最前段 25 Hz DC ブロッカ (ADC の微小 DC オフセット蓄積を除去) */
    int64_t x_q16 = (int64_t)x << 16;
    g_in_dc_q16 += ((x_q16 - g_in_dc_q16) * 214LL) >> 16;
    int32_t x_clean = clamp_s24(x - (int32_t)(g_in_dc_q16 >> 16));

    if (preset == AMBIENT_PRESET_BYPASS)
    {
        *out_l = x_clean;
        *out_r = x_clean;
        return;
    }

    /* 内部 -6 dB ヘッドルームで全段処理し、ギター強入力時の中間段クリップを防止 */
    int32_t x_in = x_clean >> 1;

    int32_t tap_l = 0;
    int32_t tap_r = 0;
    int32_t wet_l = 0;
    int32_t wet_r = 0;

    ambient_params_t params = g_params;
    int32_t fb_g = (int32_t)params.decay;
    int32_t shim_g = (int32_t)params.shimmer;
    int32_t tap_g = (int32_t)params.tap_level;
    int32_t cho_g = (int32_t)params.chorus;
    uint8_t tap_mode = params.tap_mode;

    /* 1) 48 kHz 64 タップ (32+32) FIR + 前段カスケード拡散 */
    if ((tap_mode != 0U) && (tap_g > 0))
    {
        multitap_fir_step_48k(x_in, tap_mode, &tap_l, &tap_r);
        wet_l += mul_q8(tap_l, tap_g);
        wet_r += mul_q8(tap_r, tap_g);
    }

    /* 2) 48 kHz 4x4 Hadamard FDN */
    int32_t pre;
    if ((preset == AMBIENT_PRESET_SHOEGAZE_CATHEDRAL) ||
        (preset == AMBIENT_PRESET_TENSION_BLOOM) ||
        (preset == AMBIENT_PRESET_RUIN_CATHEDRAL))
    {
        /* Reverse Gate の逆スウェルを主役に FDN/Shimmer を膨らませる直列寄りの励振 */
        pre = (x_in >> 1) + (int32_t)((((int64_t)tap_l + tap_r) * 3LL) >> 2);
    }
    else if (tap_mode != 0U)
    {
        pre = ((tap_l + tap_r) >> 1) + (x_in >> 2);
    }
    else
    {
        pre = x_in;
    }
    if (shim_g > 0)
    {
        /* シマーの FDN 帰還を 1/4 に抑え、ギラつき続けず残響の中で儚く減衰させる */
        int32_t shim_mono = (g_shim_lp_l + g_shim_lp_r) >> 1;
        pre += mul_q8(shim_mono, shim_g) >> 2;
    }
    /* FDN 入力の軽いローカット (fc ≒ 180 Hz @ 48 kHz) で低域のこもり膨張を防止 */
    g_fdn_hp += mul_q8(pre - g_fdn_hp, 6);
    int32_t fdn_in = clamp_s24(pre - g_fdn_hp);

    /* LFO モジュレーション (0..12 サンプル Q8 @ 48 kHz) */
    uint16_t tri = tri_u10(g_fdn_lfo);
    uint16_t mod_a = (uint16_t)(tri * 3U);
    uint16_t mod_b = (uint16_t)((1023U - tri) * 3U);

    int32_t y0 = fdn_read_mod(g_fdn0, FDN_LEN_0, g_fdn_idx[0], mod_a);
    int32_t y1 = fdn_read_mod(g_fdn1, FDN_LEN_1, g_fdn_idx[1], mod_b);
    int32_t y2 = fdn_read_mod(g_fdn2, FDN_LEN_2, g_fdn_idx[2], mod_a);
    int32_t y3 = fdn_read_mod(g_fdn3, FDN_LEN_3, g_fdn_idx[3], mod_b);

    /* 48 kHz フルレート向け自然な空気吸収ダンピング LPF (残響テイルの高域を保つ) */
    g_fdn_damp[0] += mul_q8(y0 - g_fdn_damp[0], 232);
    g_fdn_damp[1] += mul_q8(y1 - g_fdn_damp[1], 226);
    g_fdn_damp[2] += mul_q8(y2 - g_fdn_damp[2], 220);
    g_fdn_damp[3] += mul_q8(y3 - g_fdn_damp[3], 214);

    y0 = g_fdn_damp[0];
    y1 = g_fdn_damp[1];
    y2 = g_fdn_damp[2];
    y3 = g_fdn_damp[3];

    int32_t m0 = (int32_t)(((int64_t)y0 + y1 + y2 + y3) >> 1);
    int32_t m1 = (int32_t)(((int64_t)y0 - y1 + y2 - y3) >> 1);
    int32_t m2 = (int32_t)(((int64_t)y0 + y1 - y2 - y3) >> 1);
    int32_t m3 = (int32_t)(((int64_t)y0 - y1 - y2 + y3) >> 1);

    int32_t d_in = fdn_in >> 1;
    g_fdn0[g_fdn_idx[0]] = clamp_s24(d_in + mul_q8(m0, fb_g));
    g_fdn1[g_fdn_idx[1]] = clamp_s24(-d_in + mul_q8(m1, fb_g));
    g_fdn2[g_fdn_idx[2]] = clamp_s24(d_in + mul_q8(m2, fb_g));
    g_fdn3[g_fdn_idx[3]] = clamp_s24(-d_in + mul_q8(m3, fb_g));

    if (++g_fdn_idx[0] >= FDN_LEN_0) g_fdn_idx[0] = 0U;
    if (++g_fdn_idx[1] >= FDN_LEN_1) g_fdn_idx[1] = 0U;
    if (++g_fdn_idx[2] >= FDN_LEN_2) g_fdn_idx[2] = 0U;
    if (++g_fdn_idx[3] >= FDN_LEN_3) g_fdn_idx[3] = 0U;

    int32_t fdn_l = clamp_s24(y0 - y1);
    int32_t fdn_r = clamp_s24(y2 - y3);

    if ((preset == AMBIENT_PRESET_REVERSE_GATE) || (preset == AMBIENT_PRESET_EARLY_REFLECTIONS))
    {
        wet_l += fdn_l >> 2;
        wet_r += fdn_r >> 2;
    }
    else
    {
        wet_l += fdn_l;
        wet_r += fdn_r;
    }

    /* 3) 48 kHz Dimension Chorus (揺れない極ワイド空間拡張) */
    if (cho_g > 0)
    {
        int32_t cho_in;
        if ((preset == AMBIENT_PRESET_SHOEGAZE_CATHEDRAL) ||
            (preset == AMBIENT_PRESET_TENSION_BLOOM) ||
            (preset == AMBIENT_PRESET_RUIN_CATHEDRAL))
        {
            /* Cathedral 系では 300 Hz 以下を軽く切った信号を Dimension へ送り低域団子化を防止 */
            int32_t raw_cho = x_in + ((tap_l + tap_r) >> 3);
            g_cat_cho_hp += mul_q8(raw_cho - g_cat_cho_hp, 10); /* fc ≒ 300 Hz */
            cho_in = clamp_s24(raw_cho - g_cat_cho_hp);
        }
        else
        {
            cho_in = x_in;
        }
        int32_t dim_l;
        int32_t dim_r;

        dimension_chorus_step_48k(cho_in, &dim_l, &dim_r);
        if (tap_mode == 0U)
        {
            /* Pure Wet Dimension を主役に立て、FDN 空間を奥行きとして添える */
            wet_l = (int32_t)((((int64_t)dim_l * 280LL) + ((int64_t)wet_l * 130LL)) >> 8);
            wet_r = (int32_t)((((int64_t)dim_r * 280LL) + ((int64_t)wet_r * 130LL)) >> 8);
        }
        else
        {
            wet_l += mul_q8(dim_l, cho_g);
            wet_r += mul_q8(dim_r, cho_g);
        }
    }

    /* 4) 48 kHz ノスタルジック & 入力反応 4 ボイス変態テンション・シマー */
    if (shim_g > 0)
    {
        int32_t mono_fdn = (fdn_l + fdn_r) >> 1;
        int32_t sh_l;
        int32_t sh_r;

        shimmer_poly_step_48k(mono_fdn, x_in, params.tension, params.tension_mode, &sh_l, &sh_r);

        g_shim_dc_l += (sh_l - g_shim_dc_l) >> 5; /* fc ≒ 240 Hz @ 48 kHz */
        g_shim_dc_r += (sh_r - g_shim_dc_r) >> 5;
        int32_t hp_l = sh_l - g_shim_dc_l;
        int32_t hp_r = sh_r - g_shim_dc_r;

        /* Ruin は退廃的な 4.2 kHz、Choir Bloom は透明な 4.8 kHz、通常はノスタルジック 3.5 kHz */
        int32_t lp_alpha = (params.tension_mode == 2U) ? 106 : ((params.tension_mode == 1U) ? 118 : 92);
        int32_t next_l = g_shim_lp_l + mul_q8(hp_l - g_shim_lp_l, lp_alpha);
        int32_t next_r = g_shim_lp_r + mul_q8(hp_r - g_shim_lp_r, lp_alpha);
        g_shim_lp_l = soft_sat_s24(next_l, SHIM_SAT_THRESH);
        g_shim_lp_r = soft_sat_s24(next_r, SHIM_SAT_THRESH);

        wet_l += mul_q8(g_shim_lp_l, shim_g);
        wet_r += mul_q8(g_shim_lp_r, shim_g);
    }

    g_fdn_lfo += FDN_LFO_INC;

    uint8_t mix = params.mix;
    uint8_t dry_g = (uint8_t)(255U - mix);
    int32_t final_l = (int32_t)((((int64_t)x_in * dry_g) + ((int64_t)wet_l * mix)) >> 8);
    int32_t final_r = (int32_t)((((int64_t)x_in * dry_g) + ((int64_t)wet_r * mix)) >> 8);

    /* 5) 高域/低域独立波形ハーモニック・パンニング・トレモロ */
    if (params.pan_depth > 0U)
    {
        harmonic_pan_tremolo_step_48k(&final_l,
                                      &final_r,
                                      (int32_t)params.pan_depth,
                                      params.pan_rate,
                                      params.pan_hi_wave,
                                      params.pan_lo_wave);
    }

    /* 内部 -6 dB ヘッドルームを +6 dB 戻し、最終段 -2.5 dBFS ニー付きソフトリミッタで出力 */
    *out_l = soft_limit_s24(final_l << 1);
    *out_r = soft_limit_s24(final_r << 1);
}

void ambient_reverb_process_stereo(int16_t x, int16_t *out_l, int16_t *out_r)
{
    int32_t l24;
    int32_t r24;

    ambient_reverb_process_stereo_q23((int32_t)x << 8, &l24, &r24);
    *out_l = clamp_s16(l24 >> 8);
    *out_r = clamp_s16(r24 >> 8);
}
