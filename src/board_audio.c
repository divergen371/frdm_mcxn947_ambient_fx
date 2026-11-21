/**
 * @file board_audio.c
 * @brief FRDM-MCXN947 ハードウェア初期化 (150 MHz Core + 12.288 MHz Audio PLL1 + SAI1 + LED/SW)
 */

#include "board_audio.h"
#include "board.h"
#include "clock_config.h"
#include "fsl_clock.h"
#include "fsl_gpio.h"
#include "fsl_port.h"

static void init_pins(void)
{
    CLOCK_EnableClock(kCLOCK_Port0);
    CLOCK_EnableClock(kCLOCK_Port1);
    CLOCK_EnableClock(kCLOCK_Port3);
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);

    /* デバッグ UART (LP_FLEXCOMM4: P1_8 RX, P1_9 TX) */
    const port_pin_config_t uart_cfg = {
        kPORT_PullDisable,
        kPORT_LowPullResistor,
        kPORT_FastSlewRate,
        kPORT_PassiveFilterDisable,
        kPORT_OpenDrainDisable,
        kPORT_LowDriveStrength,
        kPORT_MuxAlt2,
        kPORT_InputBufferEnable,
        kPORT_InputNormal,
        kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(PORT1, 8U, &uart_cfg);
    PORT_SetPinConfig(PORT1, 9U, &uart_cfg);

    /* SAI1 ピン (ALT10): P3_16 BCLK, P3_17 FS, P3_20 TXD0, P1_21 MCLK */
    const port_pin_config_t sai_cfg = {
        kPORT_PullDisable,
        kPORT_LowPullResistor,
        kPORT_FastSlewRate,
        kPORT_PassiveFilterDisable,
        kPORT_OpenDrainDisable,
        kPORT_HighDriveStrength,
        kPORT_MuxAlt10,
        kPORT_InputBufferEnable,
        kPORT_InputNormal,
        kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(PORT3, 16U, &sai_cfg); /* J1[1]  SAI1_TX_BCLK */
    PORT_SetPinConfig(PORT3, 17U, &sai_cfg); /* J1[11] SAI1_TX_FS   */
    PORT_SetPinConfig(PORT3, 20U, &sai_cfg); /* J1[5]  SAI1_TXD0    */
    PORT_SetPinConfig(PORT1, 21U, &sai_cfg); /* J1[7]  SAI1_MCLK    */

    /* RGB LED: P0_10 (Red), P0_27 (Green), P1_2 (Blue) - Active Low */
    const port_pin_config_t led_cfg = {
        kPORT_PullDisable,
        kPORT_LowPullResistor,
        kPORT_FastSlewRate,
        kPORT_PassiveFilterDisable,
        kPORT_OpenDrainDisable,
        kPORT_LowDriveStrength,
        kPORT_MuxAlt0,
        kPORT_InputBufferDisable,
        kPORT_InputNormal,
        kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(PORT0, BOARD_LED_RED_GPIO_PIN, &led_cfg);
    PORT_SetPinConfig(PORT0, BOARD_LED_GREEN_GPIO_PIN, &led_cfg);
    PORT_SetPinConfig(PORT1, BOARD_LED_BLUE_GPIO_PIN, &led_cfg);

    const gpio_pin_config_t led_gpio = { kGPIO_DigitalOutput, 1U };
    GPIO_PinInit(BOARD_LED_RED_GPIO, BOARD_LED_RED_GPIO_PIN, &led_gpio);
    GPIO_PinInit(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_GPIO_PIN, &led_gpio);
    GPIO_PinInit(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_GPIO_PIN, &led_gpio);

    /* SW2 (P0_23), SW3 (P0_6) - Active Low, 内部プルアップ有効 */
    const port_pin_config_t sw_cfg = {
        kPORT_PullUp,
        kPORT_LowPullResistor,
        kPORT_FastSlewRate,
        kPORT_PassiveFilterEnable,
        kPORT_OpenDrainDisable,
        kPORT_LowDriveStrength,
        kPORT_MuxAlt0,
        kPORT_InputBufferEnable,
        kPORT_InputNormal,
        kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(PORT0, BOARD_SW2_GPIO_PIN, &sw_cfg);
    PORT_SetPinConfig(PORT0, BOARD_SW3_GPIO_PIN, &sw_cfg);

    const gpio_pin_config_t sw_gpio = { kGPIO_DigitalInput, 0U };
    GPIO_PinInit(BOARD_SW2_GPIO, BOARD_SW2_GPIO_PIN, &sw_gpio);
    GPIO_PinInit(BOARD_SW3_GPIO, BOARD_SW3_GPIO_PIN, &sw_gpio);
}

void board_audio_init_hardware(void)
{
    init_pins();

    /* Core Clock = 150 MHz (PLL0) */
    BOARD_InitBootClocks();

    /* Debug Console UART (115200 bps) */
    CLOCK_AttachClk(BOARD_DEBUG_UART_CLK_ATTACH);
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom4Clk, 1U);
    BOARD_InitDebugConsole();

    /*
     * Audio PLL1 (外部 24 MHz 水晶 -> 122.88 MHz -> /10 = 12.288 MHz, 理論誤差 0 ppm)
     * 参照: mcuxsdk/examples/_boards/frdmmcxn947/driver_examples/pdm/pdm_sai_interrupt/cm33_core0/hardware_init.c
     */
    CLOCK_SetupExtClocking(24000000U);
    const pll_setup_t pll1Setup = {
        .pllctrl = SCG_SPLLCTRL_SOURCE(0U) | SCG_SPLLCTRL_SELI(15U) | SCG_SPLLCTRL_SELP(31U),
        .pllndiv = SCG_SPLLNDIV_NDIV(25U),
        .pllpdiv = SCG_SPLLPDIV_PDIV(2U),
        .pllmdiv = SCG_SPLLMDIV_MDIV(512U),
        .pllRate = 122880000U,
    };
    CLOCK_SetPLL1Freq(&pll1Setup);

    /* PLL1_CLK0 = 122.88 MHz / 10 = 12.288 MHz -> SAI1 */
    CLOCK_SetClkDiv(kCLOCK_DivPLL1Clk0, 10U);
    CLOCK_AttachClk(kPLL1_CLK0_to_SAI1);
    CLOCK_SetClkDiv(kCLOCK_DivSai1Clk, 1U);
}

void board_audio_set_rgb(bool r, bool g, bool b)
{
    /* Active Low */
    GPIO_PinWrite(BOARD_LED_RED_GPIO, BOARD_LED_RED_GPIO_PIN, r ? 0U : 1U);
    GPIO_PinWrite(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_GPIO_PIN, g ? 0U : 1U);
    GPIO_PinWrite(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_GPIO_PIN, b ? 0U : 1U);
}

bool board_audio_sw2_pressed(void)
{
    return (GPIO_PinRead(BOARD_SW2_GPIO, BOARD_SW2_GPIO_PIN) == 0U);
}

bool board_audio_sw3_pressed(void)
{
    return (GPIO_PinRead(BOARD_SW3_GPIO, BOARD_SW3_GPIO_PIN) == 0U);
}
