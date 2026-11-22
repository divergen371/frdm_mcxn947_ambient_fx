/**
 * @file main.c
 * @brief FRDM-MCXN947 + PCM5102A 48 kHz フルレート アンビエント／シューゲイザー DSP エフェクタ
 */

#include <stdbool.h>
#include <stdint.h>
#include "fsl_common.h"
#include "fsl_debug_console.h"
#include "fsl_sai.h"

#include "ambient_reverb.h"
#include "audio_synth.h"
#include "board_audio.h"

/* DSP 負荷の実測区間 (ms)。UART 出力自体が負荷になるため、区間ごとに集計をリセットする */
#define DSP_PROFILE_REPORT_MS 5000U

static bool g_ch_right = false;
static int32_t g_cur_l = 0;
static int32_t g_cur_r = 0;

/*
 * DSP 1 フレーム (L/R ペア) あたりの実行サイクル集計。
 * ISR 側は加算のみ行い、UART 出力はメインループ側で行う。
 */
static volatile uint32_t g_prof_sum   = 0U;
static volatile uint32_t g_prof_max   = 0U;
static volatile uint32_t g_prof_count = 0U;

static void dsp_profile_accumulate(uint32_t cycles)
{
    g_prof_sum += cycles;
    if (cycles > g_prof_max)
    {
        g_prof_max = cycles;
    }
    g_prof_count++;
}

/* プリセット／原音切替直後の過渡を混ぜないため、区間を明示的に区切る */
static void dsp_profile_reset(void)
{
    DisableIRQ(AUDIO_SAI_IRQ);
    g_prof_sum   = 0U;
    g_prof_max   = 0U;
    g_prof_count = 0U;
    EnableIRQ(AUDIO_SAI_IRQ);
}

/*
 * 集計値を取り出して表示し、次の区間へ向けてリセットする。
 * 1 サンプル周期は SystemCoreClock / Fs なので、その比がそのまま DSP 占有率になる。
 */
static void dsp_profile_report(ambient_preset_t preset)
{
    DisableIRQ(AUDIO_SAI_IRQ);
    uint32_t sum   = g_prof_sum;
    uint32_t max   = g_prof_max;
    uint32_t count = g_prof_count;
    g_prof_sum     = 0U;
    g_prof_max     = 0U;
    g_prof_count   = 0U;
    EnableIRQ(AUDIO_SAI_IRQ);

    if (count == 0U)
    {
        return;
    }

    uint32_t budget = SystemCoreClock / AUDIO_SAMPLE_RATE_HZ;
    uint32_t avg    = sum / count;
    uint32_t pct100 = (budget != 0U) ? ((avg * 10000U) / budget) : 0U;

    /*
     * Debug console lite は [length] 指定子 (%lu 等) を解釈しないため、
     * 32 bit 環境である前提で %u / %d のみを使う。
     */
    PRINTF("[DSP] avg %u cyc/frame (%u.%02u%% of %u) max %u over %u frames | %s\r\n",
           (unsigned int)avg,
           (unsigned int)(pct100 / 100U),
           (unsigned int)(pct100 % 100U),
           (unsigned int)budget,
           (unsigned int)max,
           (unsigned int)count,
           ambient_reverb_get_preset_name(preset));
}

static void update_led_for_preset(ambient_preset_t preset)
{
    switch (preset)
    {
        case AMBIENT_PRESET_REVERSE_GATE:       board_audio_set_rgb(true,  false, false); break; /* Red */
        case AMBIENT_PRESET_EARLY_REFLECTIONS:  board_audio_set_rgb(false, true,  false); break; /* Green */
        case AMBIENT_PRESET_CHORUS_3PHASE:      board_audio_set_rgb(false, false, true ); break; /* Blue */
        case AMBIENT_PRESET_PAN_CROSS_RAMP:     board_audio_set_rgb(true,  true,  false); break; /* Yellow */
        case AMBIENT_PRESET_PAN_STARDUST:       board_audio_set_rgb(false, true,  true ); break; /* Cyan */
        case AMBIENT_PRESET_PAN_PSYCHE_CHOP:    board_audio_set_rgb(true,  false, true ); break; /* Magenta */
        case AMBIENT_PRESET_SHIMMER_HALL:       board_audio_set_rgb(false, true,  true ); break; /* Cyan */
        case AMBIENT_PRESET_SHOEGAZE_CATHEDRAL: board_audio_set_rgb(true,  false, true ); break; /* Magenta */
        case AMBIENT_PRESET_TENSION_BLOOM:      board_audio_set_rgb(true,  true,  false); break; /* Gold / Yellow */
        case AMBIENT_PRESET_RUIN_CATHEDRAL:     board_audio_set_rgb(true,  false, false); break; /* Crimson / Red */
        case AMBIENT_PRESET_BYPASS:             board_audio_set_rgb(true,  true,  true ); break; /* White */
        case AMBIENT_PRESET_OFF:
        default:                                board_audio_set_rgb(false, false, false); break; /* Off */
    }
}

static void sai_fill_fifo(void)
{
    uint32_t watermark = (uint32_t)FSL_FEATURE_SAI_FIFO_COUNTn(AUDIO_SAI) / 2U;

    for (uint32_t i = 0U; i < watermark; i++)
    {
        if (!g_ch_right)
        {
            /* 原音生成と 24-bit DSP をまとめて 1 フレーム分の実コストとして計測する */
            uint32_t t0   = MSDK_GetCpuCycleCount();
            int32_t in_q23 = (int32_t)audio_synth_next_sample() << 8;
            ambient_reverb_process_stereo_q23(in_q23, &g_cur_l, &g_cur_r);
            dsp_profile_accumulate(MSDK_GetCpuCycleCount() - t0);

            /* 32-bit I2S スロットの上位 24-bit へ左詰め送信 (PCM5102A / PCM1808 64fs 共通) */
            SAI_WriteData(AUDIO_SAI, AUDIO_SAI_CHANNEL, (uint32_t)g_cur_l << 8);
        }
        else
        {
            SAI_WriteData(AUDIO_SAI, AUDIO_SAI_CHANNEL, (uint32_t)g_cur_r << 8);
        }
        g_ch_right = !g_ch_right;
    }
}

void AUDIO_SAI_IRQ_HANDLER(void)
{
    uint32_t flags = SAI_TxGetStatusFlag(AUDIO_SAI);

    if ((flags & (uint32_t)kSAI_FIFOErrorFlag) != 0U)
    {
        SAI_TxClearStatusFlags(AUDIO_SAI, kSAI_FIFOErrorFlag);
    }

    if ((flags & (uint32_t)kSAI_FIFOWarningFlag) != 0U)
    {
        sai_fill_fifo();
    }

    SDK_ISR_EXIT_BARRIER;
}

static void init_sai_i2s_master(void)
{
    sai_transceiver_t sai_cfg;
    sai_master_clock_t mclk_cfg;

    SAI_Init(AUDIO_SAI);

    SAI_GetClassicI2SConfig(&sai_cfg, AUDIO_BIT_WIDTH, kSAI_Stereo, 1U << AUDIO_SAI_CHANNEL);
    sai_cfg.masterSlave = kSAI_Master;
    sai_cfg.syncMode    = kSAI_ModeAsync;
    sai_cfg.fifo.fifoWatermark = (uint8_t)(FSL_FEATURE_SAI_FIFO_COUNTn(AUDIO_SAI) / 2U);

    SAI_TxSetConfig(AUDIO_SAI, &sai_cfg);
    SAI_TxSetBitClockRate(AUDIO_SAI, AUDIO_MCLK_HZ, AUDIO_SAMPLE_RATE_HZ, AUDIO_BIT_WIDTH, AUDIO_DATA_CHANNELS);

    mclk_cfg.mclkOutputEnable = true;
    mclk_cfg.mclkHz           = AUDIO_MCLK_HZ;
    mclk_cfg.mclkSourceClkHz  = AUDIO_MCLK_HZ;
    SAI_SetMasterClockConfig(AUDIO_SAI, &mclk_cfg);

    /* 初回 FIFO を充填してから割り込み・送信を有効化 */
    g_ch_right = false;
    sai_fill_fifo();
    sai_fill_fifo();

    NVIC_SetPriority(AUDIO_SAI_IRQ, 2U);
    EnableIRQ(AUDIO_SAI_IRQ);
    SAI_TxEnableInterrupts(AUDIO_SAI, kSAI_FIFOWarningInterruptEnable | kSAI_FIFOErrorInterruptEnable);
    SAI_TxEnable(AUDIO_SAI, true);
}

int main(void)
{
    ambient_preset_t preset = AMBIENT_PRESET_REVERSE_GATE;
    audio_synth_src_t src   = SYNTH_SRC_SHOEGAZE_GLIDE;
    bool sw2_prev = false;
    bool sw3_prev = false;

    board_audio_init_hardware();
    ambient_reverb_init();
    audio_synth_set_src(src);
    ambient_reverb_set_preset(preset);
    update_led_for_preset(preset);

    /* DWT サイクルカウンタを有効化 (DSP 負荷の実測用) */
    MSDK_EnableCpuCycleCounter();

    PRINTF("\r\n=== FRDM-MCXN947 48kHz / 24-bit Full-Rate Ambient/Shoegaze DSP ===\r\n");
    PRINTF("Core: %u Hz | SAI1 MCLK: %u Hz | Fs: %u Hz 24-bit (64fs, 0 ppm)\r\n",
           (unsigned int)SystemCoreClock, (unsigned int)AUDIO_MCLK_HZ, (unsigned int)AUDIO_SAMPLE_RATE_HZ);
    PRINTF("SW2: Preset | SW3: Source\r\n");
    PRINTF("Preset: %s\r\n", ambient_reverb_get_preset_name(preset));
    PRINTF("Source: %s\r\n", audio_synth_get_name(src));

    init_sai_i2s_master();

    uint32_t prof_start      = MSDK_GetCpuCycleCount();
    const uint32_t prof_span = (SystemCoreClock / 1000U) * DSP_PROFILE_REPORT_MS;

    for (;;)
    {
        bool sw2_now = board_audio_sw2_pressed();
        bool sw3_now = board_audio_sw3_pressed();

        if (sw2_now && !sw2_prev)
        {
            SDK_DelayAtLeastUs(20000U, SystemCoreClock);
            if (board_audio_sw2_pressed())
            {
                preset = (ambient_preset_t)((preset + 1U) % AMBIENT_PRESET_COUNT);
                DisableIRQ(AUDIO_SAI_IRQ);
                ambient_reverb_set_preset(preset);
                EnableIRQ(AUDIO_SAI_IRQ);
                update_led_for_preset(preset);
                PRINTF("Preset: %s\r\n", ambient_reverb_get_preset_name(preset));
                dsp_profile_reset();
                prof_start = MSDK_GetCpuCycleCount();
            }
        }

        if (sw3_now && !sw3_prev)
        {
            SDK_DelayAtLeastUs(20000U, SystemCoreClock);
            if (board_audio_sw3_pressed())
            {
                src = (audio_synth_src_t)((src + 1U) % SYNTH_SRC_COUNT);
                DisableIRQ(AUDIO_SAI_IRQ);
                audio_synth_set_src(src);
                EnableIRQ(AUDIO_SAI_IRQ);
                PRINTF("Source: %s\r\n", audio_synth_get_name(src));
                dsp_profile_reset();
                prof_start = MSDK_GetCpuCycleCount();
            }
        }

        if ((uint32_t)(MSDK_GetCpuCycleCount() - prof_start) >= prof_span)
        {
            dsp_profile_report(preset);
            prof_start = MSDK_GetCpuCycleCount();
        }

        sw2_prev = sw2_now;
        sw3_prev = sw3_now;
        SDK_DelayAtLeastUs(10000U, SystemCoreClock);
    }
}
