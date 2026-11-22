/**
 * @file audio_synth.c
 * @brief 48 kHz / 16-bit 固定小数点 DDS テスト音源 (グライドコード / アルペジオ / ノコギリ波)
 */

#include "audio_synth.h"
#include <stdbool.h>

/* 32-bit 位相増分 = round(f * 2^32 / 48000) */
#define PHASE_INC_F3 15624260UL
#define PHASE_INC_G3 17537541UL
#define PHASE_INC_A3 19685267UL
#define PHASE_INC_C4 23409808UL
#define PHASE_INC_D4 26276415UL
#define PHASE_INC_E4 29494312UL
#define PHASE_INC_F4 31248413UL
#define PHASE_INC_A4 39370534UL
#define PHASE_INC_B4 44191472UL
#define PHASE_INC_C5 46819616UL
#define PHASE_INC_E5 58988625UL
#define PHASE_INC_G5 70150165UL
#define PHASE_INC_B5 88382944UL

#define ARP_STEP_SAMPLES 12000UL   /* 0.25 秒 / 音 */
#define GAZE_BAR_SAMPLES 120000UL  /* 2.50 秒ごとにコードチェンジ */
#define GAZE_GLIDE_INC   76056UL   /* 約 0.85 Hz アーム揺らし (減衰中に約 2 往復) */

/* 256 点サイン波テーブル (振幅 2048 ≒ -24 dBFS) */
static const int16_t g_sine_lut[256] = {
        0,    50,   100,   151,   201,   251,   301,   350,
      400,   449,   498,   546,   595,   642,   690,   737,
      784,   830,   876,   921,   965,  1009,  1053,  1096,
     1138,  1179,  1220,  1260,  1299,  1338,  1375,  1412,
     1448,  1483,  1517,  1551,  1583,  1615,  1645,  1674,
     1703,  1730,  1757,  1782,  1806,  1829,  1851,  1872,
     1892,  1911,  1928,  1945,  1960,  1974,  1987,  1998,
     2009,  2018,  2026,  2033,  2038,  2042,  2046,  2047,
     2048,  2047,  2046,  2042,  2038,  2033,  2026,  2018,
     2009,  1998,  1987,  1974,  1960,  1945,  1928,  1911,
     1892,  1872,  1851,  1829,  1806,  1782,  1757,  1730,
     1703,  1674,  1645,  1615,  1583,  1551,  1517,  1483,
     1448,  1412,  1375,  1338,  1299,  1260,  1220,  1179,
     1138,  1096,  1053,  1009,   965,   921,   876,   830,
      784,   737,   690,   642,   595,   546,   498,   449,
      400,   350,   301,   251,   201,   151,   100,    50,
        0,   -50,  -100,  -151,  -201,  -251,  -301,  -350,
     -400,  -449,  -498,  -546,  -595,  -642,  -690,  -737,
     -784,  -830,  -876,  -921,  -965, -1009, -1053, -1096,
    -1138, -1179, -1220, -1260, -1299, -1338, -1375, -1412,
    -1448, -1483, -1517, -1551, -1583, -1615, -1645, -1674,
    -1703, -1730, -1757, -1782, -1806, -1829, -1851, -1872,
    -1892, -1911, -1928, -1945, -1960, -1974, -1987, -1998,
    -2009, -2018, -2026, -2033, -2038, -2042, -2046, -2047,
    -2048, -2047, -2046, -2042, -2038, -2033, -2026, -2018,
    -2009, -1998, -1987, -1974, -1960, -1945, -1928, -1911,
    -1892, -1872, -1851, -1829, -1806, -1782, -1757, -1730,
    -1703, -1674, -1645, -1615, -1583, -1551, -1517, -1483,
    -1448, -1412, -1375, -1338, -1299, -1260, -1220, -1179,
    -1138, -1096, -1053, -1009,  -965,  -921,  -876,  -830,
     -784,  -737,  -690,  -642,  -595,  -546,  -498,  -449,
     -400,  -350,  -301,  -251,  -201,  -151,  -100,   -50
};

static volatile audio_synth_src_t g_src = SYNTH_SRC_SHOEGAZE_GLIDE;
static uint32_t g_ph[4] = { 0UL, 0UL, 0UL, 0UL };
static uint32_t g_v_freq[4] = { PHASE_INC_F3, PHASE_INC_C4, PHASE_INC_E4, PHASE_INC_A4 };
static uint16_t g_v_env[4] = { 0U, 0U, 0U, 0U };
static uint8_t g_v_atk[4] = { 0U, 0U, 0U, 0U };
static uint32_t g_glide_ph = 0UL;
static uint32_t g_cnt = 0UL;
static uint16_t g_env = 0U;
static uint8_t g_attack = 1U;
static uint8_t g_step = 0U;

void audio_synth_set_src(audio_synth_src_t src)
{
    if (src >= SYNTH_SRC_COUNT)
    {
        src = SYNTH_SRC_SHOEGAZE_GLIDE;
    }
    g_src = src;
    for (uint8_t i = 0U; i < 4U; i++)
    {
        g_ph[i] = 0UL;
        g_v_env[i] = 0U;
        g_v_atk[i] = 0U;
    }
    g_v_freq[0] = PHASE_INC_F3;
    g_v_freq[1] = PHASE_INC_C4;
    g_v_freq[2] = PHASE_INC_E4;
    g_v_freq[3] = PHASE_INC_A4;
    g_glide_ph = 0UL;
    g_cnt = 0UL;
    g_env = 0U;
    g_attack = 1U;
    g_step = 0U;
}

audio_synth_src_t audio_synth_get_src(void)
{
    return g_src;
}

const char *audio_synth_get_name(audio_synth_src_t src)
{
    switch (src)
    {
        case SYNTH_SRC_SHOEGAZE_GLIDE: return "Shoegaze Glide Chord (Fmaj7 - G6 - Asus4 - G9sus4)";
        case SYNTH_SRC_PLUCK_ARP:      return "Bell Pluck Arpeggio (C5-E5-G5-B5)";
        case SYNTH_SRC_SAW_220:        return "Sawtooth 220 Hz";
        default:                       return "Unknown";
    }
}

int16_t audio_synth_next_sample(void)
{
    switch (g_src)
    {
        case SYNTH_SRC_SHOEGAZE_GLIDE:
        {
            static const uint32_t gaze_chords[4][4] = {
                { PHASE_INC_F3, PHASE_INC_C4, PHASE_INC_E4, PHASE_INC_A4 }, /* Fmaj7 */
                { PHASE_INC_G3, PHASE_INC_D4, PHASE_INC_E4, PHASE_INC_B4 }, /* G6 */
                { PHASE_INC_A3, PHASE_INC_D4, PHASE_INC_E4, PHASE_INC_A4 }, /* Asus4 */
                { PHASE_INC_G3, PHASE_INC_C4, PHASE_INC_F4, PHASE_INC_A4 }, /* G9sus4 (sus9) */
            };
            /* 低音弦から高音弦へ 18 ms 刻み (計 54 ms) のダウンストローク・ストラム */
            static const uint32_t strum_at[4] = { 0UL, 864UL, 1728UL, 2592UL };
            /* 低音弦ほど長く、高音弦ほど早めに自然減衰 (64 サンプルごとの Q15 減衰係数) */
            static const uint32_t decay_q15[4] = { 32732UL, 32724UL, 32718UL, 32710UL };

            /* 滑らかなトレモロアーム曲線 (0..1023 -> 最大約 -83 cents のジャズマスター風グライド) */
            uint16_t gp = (uint16_t)(g_glide_ph >> 21);
            uint16_t tri = (gp < 1024U) ? gp : (uint16_t)(2047U - gp);
            /* 押し込みを強調する軽い S 字カーブ: (3*t^2 - 2*t^3) の簡易近似として t*(2048-t)>>10 と併用 */
            uint32_t bend = ((uint32_t)tri * (uint32_t)tri) >> 10; /* 0..1022: 戻りは安定、押し込みが深い */
            const uint32_t *chord = gaze_chords[g_step & 3U];
            bool do_decay = ((g_cnt & 0x3FUL) == 0UL);
            int32_t mix = 0;

            for (uint8_t i = 0U; i < 4U; i++)
            {
                /* 各弦のストラム時刻でコード周波数を切替え、滑らかなピックアタックを開始 */
                if (g_cnt == strum_at[i])
                {
                    g_v_freq[i] = chord[i];
                    g_v_atk[i] = 1U;
                }

                uint32_t f = g_v_freq[i];
                f -= (((f >> 15) * bend) * 3UL) >> 1;

                uint32_t ph = g_ph[i];
                int32_t fund = (int32_t)g_sine_lut[(uint8_t)(ph >> 24)];
                int32_t h2 = (int32_t)g_sine_lut[(uint8_t)(ph >> 23)];
                int32_t h3 = (int32_t)g_sine_lut[(uint8_t)((ph * 3UL) >> 24)];
                g_ph[i] = ph + f;

                /* ピッキング直後は倍音が明るく鳴り、減衰とともに基音中心の丸い音へ変化 (env^2) */
                int32_t env = (int32_t)g_v_env[i];
                int32_t h_env = (env * env) >> 15;
                int32_t voice = ((fund * env) >> 15)
                              + (((h2 * 3 + h3) * h_env) >> 18);
                mix += voice;

                if (g_v_atk[i] != 0U)
                {
                    /* 約 6.2 ms の滑らかなピックアタック (クリックゼロ) */
                    uint32_t next_env = (uint32_t)g_v_env[i] + 110UL;
                    if (next_env >= 32767UL)
                    {
                        g_v_env[i] = 32767U;
                        g_v_atk[i] = 0U;
                    }
                    else
                    {
                        g_v_env[i] = (uint16_t)next_env;
                    }
                }
                else if (do_decay && (g_v_env[i] > 0U))
                {
                    g_v_env[i] = (uint16_t)(((uint32_t)g_v_env[i] * decay_q15[i]) >> 15);
                }
            }

            g_glide_ph += GAZE_GLIDE_INC;
            g_cnt++;
            if (g_cnt >= GAZE_BAR_SAMPLES)
            {
                g_cnt = 0UL;
                g_step = (uint8_t)((g_step + 1U) & 3U);
                g_glide_ph = 0UL; /* ストラム直後に原音ピッチからグッとアームダウンが始まる */
            }
            return (int16_t)(mix >> 2);
        }

        case SYNTH_SRC_PLUCK_ARP:
        {
            static const uint32_t arp_inc[4] = {
                PHASE_INC_C5, PHASE_INC_E5, PHASE_INC_G5, PHASE_INC_B5
            };
            int32_t s = 0;

            if (g_step < 4U)
            {
                s = (int32_t)g_sine_lut[(uint8_t)(g_ph[0] >> 24)]
                  + ((int32_t)g_sine_lut[(uint8_t)(g_ph[0] >> 23)] >> 2);
                g_ph[0] += arp_inc[g_step];
                s = (s * (int32_t)g_env) >> 15;
            }

            g_cnt++;
            if (g_attack != 0U)
            {
                /* 約 3.8 ms のアタックランプでベル音の輪郭を保ちつつクリックを防止 */
                uint32_t next_env = (uint32_t)g_env + 180UL;
                if (next_env >= 32767UL)
                {
                    g_env = 32767U;
                    g_attack = 0U;
                }
                else
                {
                    g_env = (uint16_t)next_env;
                }
            }
            else if ((g_cnt & 0x07UL) == 0UL)
            {
                g_env = (uint16_t)(((uint32_t)g_env * 32600UL) >> 15);
            }
            if (g_cnt >= ARP_STEP_SAMPLES)
            {
                g_cnt = 0UL;
                g_step++;
                if (g_step >= 8U)
                {
                    g_step = 0U;
                }
                g_env = 0U;
                g_attack = (g_step < 4U) ? 1U : 0U;
                g_ph[0] = 0UL;
            }
            return (int16_t)s;
        }

        case SYNTH_SRC_SAW_220:
        default:
        {
            int16_t saw = (int16_t)((uint16_t)(g_ph[0] >> 20)) - 2048;
            saw = (int16_t)((saw * 3) >> 2);
            g_ph[0] += PHASE_INC_A3;
            return saw;
        }
    }
}
