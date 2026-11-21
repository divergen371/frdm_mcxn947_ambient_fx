/**
 * @file main.c
 * @brief FRDM-MCXN947 + PCM5102A 48 kHz フルレート アンビエント／シューゲイザー DSP エフェクタ
 */

#include <stdbool.h>
#include <stdint.h>
#include "fsl_debug_console.h"
#include "fsl_sai.h"

#include "ambient_reverb.h"
#include "audio_synth.h"
#include "board_audio.h"

static bool g_ch_right = false;
static int16_t g_cur_l = 0;
static int16_t g_cur_r = 0;

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
            int16_t raw = audio_synth_next_sample();
            ambient_reverb_process_stereo(raw, &g_cur_l, &g_cur_r);
            SAI_WriteData(AUDIO_SAI, AUDIO_SAI_CHANNEL, (uint32_t)(uint16_t)g_cur_l);
        }
        else
        {
            SAI_WriteData(AUDIO_SAI, AUDIO_SAI_CHANNEL, (uint32_t)(uint16_t)g_cur_r);
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

    PRINTF("\r\n=== FRDM-MCXN947 48kHz Full-Rate Ambient/Shoegaze DSP ===\r\n");
    PRINTF("Core: %lu Hz | SAI1 MCLK: %u Hz | Fs: %u Hz (0 ppm)\r\n",
           (unsigned long)SystemCoreClock, AUDIO_MCLK_HZ, AUDIO_SAMPLE_RATE_HZ);
    PRINTF("SW2: Preset | SW3: Source\r\n");
    PRINTF("Preset: %s\r\n", ambient_reverb_get_preset_name(preset));
    PRINTF("Source: %s\r\n", audio_synth_get_name(src));

    init_sai_i2s_master();

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
            }
        }

        sw2_prev = sw2_now;
        sw3_prev = sw3_now;
        SDK_DelayAtLeastUs(10000U, SystemCoreClock);
    }
}
