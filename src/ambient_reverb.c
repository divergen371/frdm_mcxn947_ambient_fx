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

/* 48 kHz フルレート Dimension Chorus 専用バッファ (2048 サンプル = 42.7 ms, 純フィードフォワード) */
#define CHO_BUF_SIZE     2048U
#define CHO_BUF_MASK     (CHO_BUF_SIZE - 1U)

/* 48 kHz フルレート シマー専用バッファ (2048 サンプル = 42.7 ms 窓) */
#define SHIM_BUF_SIZE    2048U
#define SHIM_BUF_MASK    (SHIM_BUF_SIZE - 1U)

/* LFO 位相増分 @ 48 kHz: round(f * 2^32 / 48000) */
#define FDN_LFO_INC        40265UL     /* 約 0.45 Hz */
#define DIM_LFO_INC        29528UL     /* 0.33 Hz (3.03 秒周期: DC-2 風 揺れない逆相ペア) */
#define PAN_RATE_MIN_INC   17896UL     /* 0.20 Hz (pan_rate = 0) */
#define PAN_RATE_STEP_INC  1120UL      /* pan_rate 1 刻みあたり +0.0125 Hz (255 で約 3.39 Hz) */
#define PAN_DRIFT_INC      17001UL     /* 0.19 Hz (5.26 秒周期: テンポ揺らぎによる酩酊感) */
#define PHASE_90_DEG       0x40000000UL
#define PHASE_180_DEG      0x80000000UL

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
static int32_t g_fdn_hp = 0;
static int32_t g_cat_cho_hp = 0;
static int32_t g_shim_dc = 0;
static int32_t g_shim_lp = 0;

static int32_t g_dim_lp = 0;
static int32_t g_pan_lp_l = 0;
static int32_t g_pan_lp_r = 0;
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
    165U, 0U, 255U, 0U, 225U, 1U, 0U, 36U, PAN_WAVE_SINE, PAN_WAVE_SINE
};

static const ambient_params_t g_preset_table[AMBIENT_PRESET_COUNT] = {
    /* decay, shim, tap, cho, mix, tap_m, pan_d, pan_r, hi_wave, lo_wave */
    { 165U,   0U, 255U,   0U, 225U, 1U,   0U,  36U, PAN_WAVE_SINE,          PAN_WAVE_SINE      }, /* REVERSE_GATE */
    { 180U,   0U, 255U,   0U, 180U, 2U,   0U,  36U, PAN_WAVE_SINE,          PAN_WAVE_SINE      }, /* EARLY_REFLECTIONS */
    { 218U,   0U,   0U, 255U, 148U, 0U, 205U,  34U, PAN_WAVE_SINE,          PAN_WAVE_SINE      }, /* DIM + SINE/SINE */
    { 218U,   0U,   0U, 255U, 148U, 0U, 205U,  38U, PAN_WAVE_RAMP_UP,       PAN_WAVE_RAMP_DOWN }, /* DIM + CROSS-RAMP */
    { 224U,   0U,   0U, 255U, 148U, 0U, 205U,  46U, PAN_WAVE_SMOOTH_RANDOM, PAN_WAVE_SINE      }, /* DIM + STARDUST */
    { 218U,   0U,   0U, 255U, 148U, 0U, 205U,  62U, PAN_WAVE_SOFT_SQUARE,   PAN_WAVE_SINE      }, /* DIM + PSYCHE CHOP */
    { 236U, 130U,   0U,   0U, 190U, 0U,   0U,  36U, PAN_WAVE_SINE,          PAN_WAVE_SINE      }, /* SHIMMER_HALL */
    { 234U, 155U, 220U, 125U, 175U, 1U, 135U,  30U, PAN_WAVE_SINE,          PAN_WAVE_SINE      }, /* SHOEGAZE_CATHEDRAL */
    {   0U,   0U,   0U,   0U,   0U, 0U,   0U,   0U, PAN_WAVE_SINE,          PAN_WAVE_SINE      }, /* BYPASS */
    {   0U,   0U,   0U,   0U,   0U, 0U,   0U,   0U, PAN_WAVE_SINE,          PAN_WAVE_SINE      }  /* OFF */
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

/*
 * 48 kHz フルレート Dimension Chorus (Roland SDD-320 / BOSS DC-2 方式, Pure Wet 出力)
 * - フィードバック 0 の純フィードフォワード + BBD 風ローパス (fc ≒ 9.0 kHz)
 * - 0.33 Hz 逆相ペア × 2 系統 (90° 位相差, ±4.5 cents) に左右相補の +2.5 ms 中心オフセットを与え、
 *   LFO 交差点での Side 成分消滅を防ぎながら常に極ワイドな広がりを維持する
 * - Dry との合成は呼び出し側の mix で一元管理し、二重 Dry による希釈を防ぐ
 */
static void dimension_chorus_step_48k(int16_t in, int16_t *out_l, int16_t *out_r)
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
    g_dim_lp += (((int32_t)in - g_dim_lp) * 180) >> 8;
    g_cho_buf[g_cho_wr] = clamp_s16(g_dim_lp);
    g_cho_wr = (g_cho_wr + 1U) & CHO_BUF_MASK;
    g_dim_lfo += DIM_LFO_INC;

    /* クロスブレンド (Pure Wet): Mid でピッチ相殺しつつ Side へ濃密に展開 */
    int32_t w_l = ((a * 3) + (c * 2) - (b * 2) - d) >> 2;
    int32_t w_r = ((b * 3) + (d * 2) - (a * 2) - c) >> 2;

    *out_l = clamp_s16((w_l * 290) >> 8);
    *out_r = clamp_s16((w_r * 290) >> 8);
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

/*
 * 各種パンニング LFO 波形を評価して -512..+511 (+が左寄り, -が右寄り) を返す
 */
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
 * 48 kHz フルレート 高域/低域独立波形ハーモニック・パンニング・トレモロ
 * - 1 次クロスオーバー (fc ≒ 550 Hz) で低域/高域を分離
 * - 高域と低域それぞれに任意のカーブ波形を選択可能 (低域は 180° 位相オフセット)
 * - 波形出力へ約 10.7 ms の 1 次スムージング (>> 9) を挟み、
 *   ノコギリ波折り返しや矩形波エッジでのクリックノイズを物理的に除去する
 */
static void harmonic_pan_tremolo_step_48k(int32_t *io_l,
                                          int32_t *io_r,
                                          int32_t depth,
                                          uint8_t rate,
                                          uint8_t hi_wave,
                                          uint8_t lo_wave)
{
    int32_t in_l = *io_l;
    int32_t in_r = *io_r;

    /* fc ≒ 550 Hz @ 48 kHz (alpha = 18/256) */
    g_pan_lp_l += ((in_l - g_pan_lp_l) * 18) >> 8;
    g_pan_lp_r += ((in_r - g_pan_lp_r) * 18) >> 8;

    int32_t lo_l = g_pan_lp_l;
    int32_t hi_l = in_l - lo_l;
    int32_t lo_r = g_pan_lp_r;
    int32_t hi_r = in_r - lo_r;

    /* 0.19 Hz ドリフトで旋回位相を ±45° 緩やかに揺らし機械的な単調さを消す */
    uint32_t drift_ph = ((uint32_t)sine_u10(g_pan_drift) - 512U) << 20;
    uint32_t ph_hi = g_pan_lfo + drift_ph;
    uint32_t ph_lo = ph_hi + PHASE_180_DEG;

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

    /* Q16 精度で τ ≒ 10.7 ms のスルー制限を掛けて折り返しクリックを除去 */
    g_pan_mod_hi_sm += ((raw_hi << 8) - g_pan_mod_hi_sm) >> 9;
    g_pan_mod_lo_sm += ((raw_lo << 8) - g_pan_mod_lo_sm) >> 9;

    int32_t sm_hi = g_pan_mod_hi_sm >> 8; /* -512..+511 */
    int32_t sm_lo = g_pan_mod_lo_sm >> 8; /* -512..+511 */

    int32_t mod_hi = (sm_hi * depth) >> 10;              /* depth=240 -> ±120 */
    int32_t mod_lo = ((sm_lo * depth) >> 10) * 3 >> 2;   /* 低域は土台を保ちつつ ±90 */

    int32_t base = 240; /* Q8: 約 0.94 を中心に 0.47..1.41 の一定パワー寄り変調 */
    int32_t g_hi_l = base + mod_hi;
    int32_t g_hi_r = base - mod_hi;
    int32_t g_lo_l = base + mod_lo;
    int32_t g_lo_r = base - mod_lo;

    *io_l = ((hi_l * g_hi_l) + (lo_l * g_lo_l)) >> 8;
    *io_r = ((hi_r * g_hi_r) + (lo_r * g_lo_r)) >> 8;

    g_pan_lfo += PAN_RATE_MIN_INC + ((uint32_t)rate * PAN_RATE_STEP_INC);
    g_pan_drift += PAN_DRIFT_INC;
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
    g_fdn_hp = 0;
    g_cat_cho_hp = 0;
    g_shim_dc = 0;
    g_shim_lp = 0;
    g_dim_lp = 0;
    g_pan_lp_l = 0;
    g_pan_lp_r = 0;
    g_pan_mod_hi_sm = 0;
    g_pan_mod_lo_sm = 0;
    g_fdn_lfo = 0UL;
    g_dim_lfo = 0UL;
    g_pan_lfo = 0UL;
    g_pan_drift = 0UL;
    g_pan_prev_hi_ph = 0UL;
    g_pan_prev_lo_ph = PHASE_180_DEG;
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
        case AMBIENT_PRESET_REVERSE_GATE:       return "1: SPX90 Reverse Gate (48kHz 32-tap FIR)";
        case AMBIENT_PRESET_EARLY_REFLECTIONS:  return "2: SPX90 Early Reflections (48kHz 32-tap FIR)";
        case AMBIENT_PRESET_CHORUS_3PHASE:      return "3: Dimension + Harmonic Pan (Sine x Sine)";
        case AMBIENT_PRESET_PAN_CROSS_RAMP:     return "4: Dimension + Infinite Cross-Ramp (Up x Down)";
        case AMBIENT_PRESET_PAN_STARDUST:       return "5: Dimension + Stardust Pan (Rand x Sine)";
        case AMBIENT_PRESET_PAN_PSYCHE_CHOP:    return "6: Dimension + Psyche Chop (SoftSq x Sine)";
        case AMBIENT_PRESET_SHIMMER_HALL:       return "7: 4x4 Hadamard FDN + Shimmer";
        case AMBIENT_PRESET_SHOEGAZE_CATHEDRAL: return "8: Shoegaze Cathedral (All-in 48kHz)";
        case AMBIENT_PRESET_BYPASS:             return "9: Bypass (Dry)";
        case AMBIENT_PRESET_OFF:                return "10: Mute / Off";
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
    int32_t pre;
    if (preset == AMBIENT_PRESET_SHOEGAZE_CATHEDRAL)
    {
        pre = (int32_t)x + (((int32_t)tap_l + (int32_t)tap_r) >> 1);
    }
    else if (tap_mode != 0U)
    {
        pre = (((int32_t)tap_l + (int32_t)tap_r) >> 1) + ((int32_t)x >> 2);
    }
    else
    {
        pre = (int32_t)x;
    }
    if (shim_g > 0)
    {
        pre += (g_shim_lp * shim_g) >> 8;
    }
    /* FDN 入力の軽いローカット (fc ≒ 180 Hz @ 48 kHz) で低域のこもり膨張を防止 */
    g_fdn_hp += ((pre - g_fdn_hp) * 6) >> 8;
    int16_t fdn_in = clamp_s16(pre - g_fdn_hp);

    /* LFO モジュレーション (0..12 サンプル Q8 @ 48 kHz) */
    uint16_t tri = tri_u10(g_fdn_lfo);
    uint16_t mod_a = (uint16_t)(tri * 3U);
    uint16_t mod_b = (uint16_t)((1023U - tri) * 3U);

    int32_t y0 = fdn_read_mod(g_fdn0, FDN_LEN_0, g_fdn_idx[0], mod_a);
    int32_t y1 = fdn_read_mod(g_fdn1, FDN_LEN_1, g_fdn_idx[1], mod_b);
    int32_t y2 = fdn_read_mod(g_fdn2, FDN_LEN_2, g_fdn_idx[2], mod_a);
    int32_t y3 = fdn_read_mod(g_fdn3, FDN_LEN_3, g_fdn_idx[3], mod_b);

    /* 48 kHz フルレート向け自然な空気吸収ダンピング LPF (残響テイルの高域を保つ) */
    g_fdn_damp[0] += ((y0 - g_fdn_damp[0]) * 232) >> 8;
    g_fdn_damp[1] += ((y1 - g_fdn_damp[1]) * 226) >> 8;
    g_fdn_damp[2] += ((y2 - g_fdn_damp[2]) * 220) >> 8;
    g_fdn_damp[3] += ((y3 - g_fdn_damp[3]) * 214) >> 8;

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
    else
    {
        wet_l += (int32_t)fdn_l;
        wet_r += (int32_t)fdn_r;
    }

    /* 3) 48 kHz Dimension Chorus (揺れない極ワイド空間拡張) */
    if (cho_g > 0)
    {
        int16_t cho_in;
        if (preset == AMBIENT_PRESET_SHOEGAZE_CATHEDRAL)
        {
            /* Cathedral では 300 Hz 以下を軽く切った信号を Dimension へ送り低域団子化を防止 */
            int32_t raw_cho = (int32_t)x + (((int32_t)tap_l + (int32_t)tap_r) >> 3);
            g_cat_cho_hp += ((raw_cho - g_cat_cho_hp) * 10) >> 8; /* fc ≒ 300 Hz */
            cho_in = clamp_s16(raw_cho - g_cat_cho_hp);
        }
        else
        {
            cho_in = x;
        }
        int16_t dim_l;
        int16_t dim_r;

        dimension_chorus_step_48k(cho_in, &dim_l, &dim_r);
        if (tap_mode == 0U)
        {
            /* Pure Wet Dimension を主役に立て、FDN 空間を奥行きとして添える */
            wet_l = ((int32_t)dim_l * 280 + (wet_l * 130)) >> 8;
            wet_r = ((int32_t)dim_r * 280 + (wet_r * 130)) >> 8;
        }
        else
        {
            wet_l += ((int32_t)dim_l * cho_g) >> 8;
            wet_r += ((int32_t)dim_r * cho_g) >> 8;
        }
    }

    /* 4) 48 kHz +1 オクターブ・シマー (独立 42.7 ms 窓 + 低域カット + エアリー LPF) */
    if (shim_g > 0)
    {
        int16_t mono_fdn = (int16_t)(((int32_t)fdn_l + (int32_t)fdn_r) >> 1);
        int16_t oct_up = shimmer_step_48k(mono_fdn);

        g_shim_dc += ((int32_t)oct_up - g_shim_dc) >> 5; /* fc ≒ 240 Hz @ 48 kHz */
        int32_t oct_hp = (int32_t)oct_up - g_shim_dc;

        g_shim_lp += ((oct_hp - g_shim_lp) * 165) >> 8;  /* fc ≒ 7.8 kHz @ 48 kHz */
        if (g_shim_lp > 3500) g_shim_lp = 3500;
        if (g_shim_lp < -3500) g_shim_lp = -3500;

        wet_l += (g_shim_lp * shim_g) >> 8;
        wet_r += (g_shim_lp * shim_g) >> 8;
    }

    g_fdn_lfo += FDN_LFO_INC;

    uint8_t mix = params.mix;
    uint8_t dry_g = (uint8_t)(255U - mix);
    int32_t final_l = (((int32_t)x * (int32_t)dry_g) + (wet_l * (int32_t)mix)) >> 8;
    int32_t final_r = (((int32_t)x * (int32_t)dry_g) + (wet_r * (int32_t)mix)) >> 8;

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

    *out_l = clamp_s16(final_l);
    *out_r = clamp_s16(final_r);
}
