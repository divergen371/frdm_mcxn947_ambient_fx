/**
 * @file board_audio.h
 * @brief FRDM-MCXN947 SAI1 (I2S Master) / PLL1 (12.288 MHz) / SW2・SW3 / RGB LED 定義
 *
 * ピン配置 (UM12018 Table 17 / Table 19):
 *   J1[1]  P3_16 (ALT10) -> SAI1_TX_BCLK -> PCM5102A BCK  (3.072 MHz, 64fs)
 *   J1[11] P3_17 (ALT10) -> SAI1_TX_FS   -> PCM5102A LCK  (48.000 kHz)
 *   J1[5]  P3_20 (ALT10) -> SAI1_TXD0    -> PCM5102A DIN
 *   J1[7]  P1_21 (ALT10) -> SAI1_MCLK    -> (任意: 12.288 MHz)
 *
 * オンボード UI (board.h):
 *   SW2: P0_23 (Active Low) -> プリセット切替
 *   SW3: P0_6  (Active Low) -> 原音切替
 *   LED: Red P0_10, Green P0_27, Blue P1_2 (Active Low)
 */

#ifndef BOARD_AUDIO_H
#define BOARD_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "fsl_sai.h"

#define AUDIO_SAI                 SAI1
#define AUDIO_SAI_CHANNEL         0U
#define AUDIO_SAI_IRQ             SAI1_IRQn
#define AUDIO_SAI_IRQ_HANDLER     SAI1_IRQHandler

#define AUDIO_SAMPLE_RATE_HZ      48000U
#define AUDIO_BIT_WIDTH           kSAI_WordWidth32bits /* 32-bit スロット / 上位 24-bit 有効 (64fs, PCM5102A/PCM1808 互換) */
#define AUDIO_DATA_CHANNELS       2U
#define AUDIO_MCLK_HZ             12288000U

/**
 * @brief クロック (PLL0 150 MHz, PLL1 122.88 MHz -> 12.288 MHz), ピン, UART, LED, SW を初期化する
 */
void board_audio_init_hardware(void);

/**
 * @brief RGB LED の点灯色を設定する (1: 点灯, 0: 消灯)
 */
void board_audio_set_rgb(bool r, bool g, bool b);

/**
 * @brief オンボード SW2 (プリセット切替) の押下状態を取得する
 */
bool board_audio_sw2_pressed(void);

/**
 * @brief オンボード SW3 (原音切替) の押下状態を取得する
 */
bool board_audio_sw3_pressed(void);

#endif /* BOARD_AUDIO_H */
