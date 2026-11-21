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
#define PAN_LFO_INC        58161UL     /* 0.65 Hz (1.54 秒周期: ハーモニック・パンニング旋回) */
#define PAN_DRIFT_INC      17001UL     /* 0.19 Hz (5.26 秒周期: テンポ揺らぎによる酩酊感) */
#define PHASE_90_DEG       0x40000000UL

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

static int32_t g_dim_lp = 0;
static int32_t g_pan_lp_l = 0;
static int32_t g_pan_lp_r = 0;
static uint32_t g_fdn_lfo = 0UL;
static uint32_t g_dim_lfo = 0UL;
static uint32_t g_pan_lfo = 0UL;
static uint32_t g_pan_drift = 0UL;

static volatile ambient_preset_t g_preset = AMBIENT_PRESET_REVERSE_GATE;
static volatile ambient_params_t g_params = { 160U, 0U, 255U, 0U, 225U, 1U };

static const ambient_params_t g_preset_table[AMBIENT_PRESET_COUNT] = {
    /* decay, shimmer, tap_lvl, chorus, mix, tap_mode (1=RevGate, 2=ER) */
    { 165U,   0U, 255U,   0U, 225U, 1U }, /* REVERSE_GATE */
    { 180U,   0U, 255U,   0U, 180U, 2U }, /* EARLY_REFLECTIONS */
    { 218U,   0U,   0U, 255U, 215U, 0U }, /* DIMENSION + HARMONIC PAN TREMOLO */
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
 * 48 kHz フルレート Dimension Chorus (Roland SDD-320 / BOSS DC-2 方式)
 * - フィードバック 0 の純フィードフォワード + BBD 風ローパス (fc ≒ 6.2 kHz)
 * - 0.33 Hz 逆相ペア × 2 系統 (90° 位相差) のクロスブレンドで、
 *   Mid のピッチ揺れを相殺しつつ Side 方向へ極ワイドに広げる (±3.4..4.0 cents)
 */
static void dimension_chorus_step_48k(int16_t in, int16_t *out_l, int16_t *out_r)
{
    uint32_t t_a = (uint32_t)tri_u10(g_dim_lfo);
    uint32_t t_b = 1023U - t_a;
    uint32_t t_c = (uint32_t)tri_u10(g_dim_lfo + PHASE_90_DEG);
    uint32_t t_d = 1023U - t_c;

    /* ペア 1: 4.0..7.0 ms (中心 5.5 ms, ±1.50 ms), ペア 2: 7.25..10.75 ms (中心 9.0 ms, ±1.75 ms) */
    int32_t a = cho_read_q8((192UL << 8) + (t_a * 36UL));
    int32_t b = cho_read_q8((192UL << 8) + (t_b * 36UL));
    int32_t c = cho_read_q8((348UL << 8) + (t_c * 42UL));
    int32_t d = cho_read_q8((348UL << 8) + (t_d * 42UL));

    /* BBD 風ウォーム LPF を通して純フィードフォワード書き込み (フィードバック無し) */
    g_dim_lp += (((int32_t)in - g_dim_lp) * 145) >> 8;
    g_cho_buf[g_cho_wr] = clamp_s16(g_dim_lp);
    g_cho_wr = (g_cho_wr + 1U) & CHO_BUF_MASK;
    g_dim_lfo += DIM_LFO_INC;

    /* クロスブレンド: Mid で逆相ペアがピッチ相殺、Side で極ワイドに展開 */
    int32_t w_l = ((a * 3) + (c * 2) - (b * 2) - d) >> 2;
    int32_t w_r = ((b * 3) + (d * 2) - (a * 2) - c) >> 2;

    /* 揺れない原音芯 + Dimension 広がり成分 */
    *out_l = clamp_s16((int32_t)in + ((w_l * 190) >> 8));
    *out_r = clamp_s16((int32_t)in + ((w_r * 190) >> 8));
}

/*
 * 48 kHz フルレート 酩酊ハーモニック・パンニング・トレモロ
 * - 1 次クロスオーバー (fc ≒ 550 Hz) で低域/高域を分離
 * - L 高域↑・L 低域↓ と R 高域↓・R 低域↑ を 0.65 Hz (+ 0.19 Hz テンポ揺らぎ) で旋回させ、
 *   音程を狂わせず・音量を途切れさせずに空間が酔うように回る酩酊感を作る
 */
static void harmonic_pan_tremolo_step_48k(int32_t *io_l, int32_t *io_r, int32_t depth)
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

    /* 0.19 Hz ドリフトで 0.65 Hz 旋回位相を ±45° 緩やかに揺らし機械的な単調さを消す */
    uint32_t drift_ph = ((uint32_t)sine_u10(g_pan_drift) - 512U) << 20;
    int32_t s = (int32_t)sine_u10(g_pan_lfo + drift_ph) - 512; /* -512..+511 */
    int32_t mod_hi = (s * depth) >> 10;                        /* depth=230 -> ±115 */
    int32_t mod_lo = (mod_hi * 3) >> 2;                        /* 低域はやや穏やかに逆相旋回 */

    int32_t base = 240; /* Q8: 約 0.94 を中心に 0.49..1.39 の一定パワー寄り変調 */
    int32_t g_hi_l = base + mod_hi;
    int32_t g_lo_l = base - mod_lo;
    int32_t g_hi_r = base - mod_hi;
    int32_t g_lo_r = base + mod_lo;

    *io_l = ((hi_l * g_hi_l) + (lo_l * g_lo_l)) >> 8;
    *io_r = ((hi_r * g_hi_r) + (lo_r * g_lo_r)) >> 8;

    g_pan_lfo += PAN_LFO_INC;
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
    g_shim_dc = 0;
    g_shim_lp1 = 0;
    g_shim_lp2 = 0;
    g_dim_lp = 0;
    g_pan_lp_l = 0;
    g_pan_lp_r = 0;
    g_fdn_lfo = 0UL;
    g_dim_lfo = 0UL;
    g_pan_lfo = 0UL;
    g_pan_drift = 0UL;
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
        case AMBIENT_PRESET_CHORUS_3PHASE:      return "3: Dimension Chorus + Harmonic Pan";
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
    int32_t pre;
    if (preset == AMBIENT_PRESET_SHOEGAZE_CATHEDRAL)
    {
        pre = (((int32_t)tap_l + (int32_t)tap_r) >> 1) + ((int32_t)x >> 1);
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
        wet_l += ((int32_t)fdn_l * 3) >> 2;
        wet_r += ((int32_t)fdn_r * 3) >> 2;
    }
    else
    {
        wet_l += (int32_t)fdn_l;
        wet_r += (int32_t)fdn_r;
    }

    /* 3) 48 kHz Dimension Chorus (揺れない極ワイド空間拡張) */
    if (cho_g > 0)
    {
        int16_t cho_in = (preset == AMBIENT_PRESET_SHOEGAZE_CATHEDRAL)
                             ? clamp_s16((int32_t)x + ((wet_l + wet_r) >> 2))
                             : x;
        int16_t dim_l;
        int16_t dim_r;

        dimension_chorus_step_48k(cho_in, &dim_l, &dim_r);
        if (preset == AMBIENT_PRESET_CHORUS_3PHASE)
        {
            /* 揺れない Dimension の広がりを主役に、FDN 空間を程よく重ねる */
            wet_l = ((int32_t)dim_l * 210 + (wet_l * 105)) >> 8;
            wet_r = ((int32_t)dim_r * 210 + (wet_r * 105)) >> 8;
        }
        else
        {
            wet_l = ((wet_l * 3) >> 2) + (((int32_t)dim_l * cho_g) >> 8);
            wet_r = ((wet_r * 3) >> 2) + (((int32_t)dim_r * cho_g) >> 8);
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
    int32_t final_l = (((int32_t)x * (int32_t)dry_g) + (wet_l * (int32_t)mix)) >> 8;
    int32_t final_r = (((int32_t)x * (int32_t)dry_g) + (wet_r * (int32_t)mix)) >> 8;

    /* 5) 酩酊ハーモニック・パンニング・トレモロ (広がりを保ったまま空間ごと旋回) */
    if (cho_g > 0)
    {
        int32_t pan_depth = (preset == AMBIENT_PRESET_CHORUS_3PHASE) ? 230 : 140;
        harmonic_pan_tremolo_step_48k(&final_l, &final_r, pan_depth);
    }

    *out_l = clamp_s16(final_l);
    *out_r = clamp_s16(final_r);
}
