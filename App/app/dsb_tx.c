#include "app/dsb_tx.h"

#include <stddef.h>

#include "driver/bk4819.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"

#include "py32f0xx.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_rcc.h"

#define DSB_TX_SAMPLE_RATE_HZ 8000u
#define DSB_TX_PIN_CSN        LL_GPIO_PIN_9
#define DSB_TX_PIN_SCL        LL_GPIO_PIN_8
#define DSB_TX_PIN_SDA        LL_GPIO_PIN_9

/* Four CPU NOPs plus the GPIO store overhead keep the fast serial clock well
 * below the CPU clock while remaining an order of magnitude faster than the
 * normal 1 us-per-edge driver.  The normal driver remains the authoritative
 * path for configuration registers; this path is only for REG64/REG36 while
 * experimental DSB TX is active. */
#define DSB_TX_SPI_DELAY() do { __NOP(); __NOP(); __NOP(); __NOP(); } while (0)

static volatile bool gDsbTxActive;
static volatile uint16_t gDsbTxVoiceAmplitude;
static uint8_t gDsbTxPeakBias;
static uint8_t gDsbTxPaGain;
static uint16_t gDsbTxLastPaWord = 0xFFFFu;

static bool isDsbTransmitContext(void)
{
    return gCurrentFunction == FUNCTION_TRANSMIT &&
           gCurrentVfo != NULL &&
           gCurrentVfo->Modulation == MODULATION_DSB &&
           gPttIsPressed &&
           !gFlagEndTransmission;
}

static inline bool fastBusIsIdle(void)
{
    /* Normal BK4829 transactions hold CSN low for their complete transfer.  If
     * TIM3 interrupts one of them, discard this 125 us sample instead of ever
     * interleaving clocks/data with the normal bit-bang driver. */
    return LL_GPIO_IsOutputPinSet(GPIOF, DSB_TX_PIN_CSN) != 0u;
}

static inline void fastWriteBit(bool one)
{
    if (one)
        LL_GPIO_SetOutputPin(GPIOB, DSB_TX_PIN_SDA);
    else
        LL_GPIO_ResetOutputPin(GPIOB, DSB_TX_PIN_SDA);

    DSB_TX_SPI_DELAY();
    LL_GPIO_SetOutputPin(GPIOB, DSB_TX_PIN_SCL);
    DSB_TX_SPI_DELAY();
    LL_GPIO_ResetOutputPin(GPIOB, DSB_TX_PIN_SCL);
    DSB_TX_SPI_DELAY();
}

static void fastWriteU8(uint8_t data)
{
    for (uint8_t i = 0; i < 8u; i++) {
        fastWriteBit((data & 0x80u) != 0u);
        data <<= 1;
    }
}

static void fastWriteU16(uint16_t data)
{
    for (uint8_t i = 0; i < 16u; i++) {
        fastWriteBit((data & 0x8000u) != 0u);
        data <<= 1;
    }
}

static bool fastReadRegister(BK4819_REGISTER_t reg, uint16_t *value)
{
    uint16_t data = 0;

    if (!fastBusIsIdle())
        return false;

    LL_GPIO_ResetOutputPin(GPIOB, DSB_TX_PIN_SCL);
    LL_GPIO_ResetOutputPin(GPIOF, DSB_TX_PIN_CSN);
    fastWriteU8((uint8_t)reg | 0x80u);

    LL_GPIO_SetPinMode(GPIOB, DSB_TX_PIN_SDA, LL_GPIO_MODE_INPUT);
    DSB_TX_SPI_DELAY();

    for (uint8_t i = 0; i < 16u; i++) {
        data <<= 1;
        if (LL_GPIO_IsInputPinSet(GPIOB, DSB_TX_PIN_SDA))
            data |= 1u;

        LL_GPIO_SetOutputPin(GPIOB, DSB_TX_PIN_SCL);
        DSB_TX_SPI_DELAY();
        LL_GPIO_ResetOutputPin(GPIOB, DSB_TX_PIN_SCL);
        DSB_TX_SPI_DELAY();
    }

    LL_GPIO_SetPinMode(GPIOB, DSB_TX_PIN_SDA, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetOutputPin(GPIOF, DSB_TX_PIN_CSN);
    LL_GPIO_SetOutputPin(GPIOB, DSB_TX_PIN_SCL);
    LL_GPIO_SetOutputPin(GPIOB, DSB_TX_PIN_SDA);

    *value = data;
    return true;
}

static void fastWriteRegisterLocked(BK4819_REGISTER_t reg, uint16_t data)
{
    /* Called immediately after a successful fast read while foreground code is
     * suspended by the IRQ, so no second idle check is required. */
    LL_GPIO_ResetOutputPin(GPIOB, DSB_TX_PIN_SCL);
    LL_GPIO_ResetOutputPin(GPIOF, DSB_TX_PIN_CSN);
    fastWriteU8((uint8_t)reg);
    fastWriteU16(data);
    LL_GPIO_SetOutputPin(GPIOF, DSB_TX_PIN_CSN);
    LL_GPIO_SetOutputPin(GPIOB, DSB_TX_PIN_SCL);
    LL_GPIO_SetOutputPin(GPIOB, DSB_TX_PIN_SDA);
}

static uint32_t getTimer3ClockHz(void)
{
    /* PY32F071 clocks general-purpose timers from PCLK when APB1=/1, otherwise
     * from 2*PCLK. SystemCoreClock is HCLK in this firmware, so derive TIM_PCLK
     * from the bootloader-preserved APB1 divider rather than assuming /1. */
    switch (LL_RCC_GetAPB1Prescaler()) {
    case LL_RCC_APB1_DIV_1:
    case LL_RCC_APB1_DIV_2:
        return SystemCoreClock;
    case LL_RCC_APB1_DIV_4:
        return SystemCoreClock / 2u;
    case LL_RCC_APB1_DIV_8:
        return SystemCoreClock / 4u;
    case LL_RCC_APB1_DIV_16:
        return SystemCoreClock / 8u;
    default:
        return SystemCoreClock;
    }
}

static void stopTimerFromIrq(void)
{
    TIM3->DIER = 0u;
    TIM3->CR1 &= ~TIM_CR1_CEN;
    gDsbTxActive = false;
}

void TIM3_IRQHandler(void)
{
    uint16_t voice;

    if ((TIM3->SR & TIM_SR_UIF) == 0u)
        return;
    TIM3->SR = 0u;

    if (!gDsbTxActive)
        return;

    if (!isDsbTransmitContext()) {
        if (fastBusIsIdle()) {
            /* Best-effort emergency mute.  Normal end-of-TX handling also calls
             * DSB_TX_Stop() before reconfiguring the BK4829. */
            LL_GPIO_ResetOutputPin(GPIOB, DSB_TX_PIN_SCL);
            LL_GPIO_ResetOutputPin(GPIOF, DSB_TX_PIN_CSN);
            fastWriteU8((uint8_t)BK4819_REG_36);
            fastWriteU16(0u);
            LL_GPIO_SetOutputPin(GPIOF, DSB_TX_PIN_CSN);
            LL_GPIO_SetOutputPin(GPIOB, DSB_TX_PIN_SCL);
            LL_GPIO_SetOutputPin(GPIOB, DSB_TX_PIN_SDA);
        }
        stopTimerFromIrq();
        return;
    }

    if (!fastReadRegister(BK4819_REG_64, &voice))
        return;

    voice &= 0x7FFFu;
    gDsbTxVoiceAmplitude = voice;

    /* REG64 is documented as Voice Amplitude Out, not signed PCM.  Preserve the
     * existing DSB envelope semantics but sample at 8 kHz and use all 15 bits
     * before scaling to the calibrated PA-bias ceiling.  This intentionally
     * does not attempt Hilbert/phase processing until signed audio is proven. */
    const uint8_t bias = (uint8_t)((((uint32_t)voice * gDsbTxPeakBias) + 0x4000u) >> 15);
    const uint16_t paWord = ((uint16_t)bias << 8) | (1u << 7) | gDsbTxPaGain;

    if (paWord != gDsbTxLastPaWord) {
        fastWriteRegisterLocked(BK4819_REG_36, paWord);
        gDsbTxLastPaWord = paWord;
    }
}

bool DSB_TX_IsActive(void)
{
    return gDsbTxActive;
}

void DSB_TX_Start(uint8_t peakBias, uint32_t frequency)
{
    gDsbTxPeakBias = peakBias;
    gDsbTxPaGain = frequency < 28000000u ? 0x08u : 0x22u;

    if (gDsbTxActive)
        return;

    gDsbTxVoiceAmplitude = 0u;
    gDsbTxLastPaWord = 0xFFFFu;

    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3);
    TIM3->CR1 = 0u;
    TIM3->DIER = 0u;
    TIM3->PSC = 0u;
    TIM3->ARR = (getTimer3ClockHz() / DSB_TX_SAMPLE_RATE_HZ) - 1u;
    TIM3->CNT = 0u;
    TIM3->EGR = TIM_EGR_UG;
    TIM3->SR = 0u;

    /* Priority 0 makes each short fast-SPI transaction atomic with respect to
     * ordinary maskable firmware IRQs. If foreground SPI was already active,
     * the CSN guard above simply drops that 125 us envelope sample. */
    NVIC_SetPriority(TIM3_IRQn, 0u);
    NVIC_ClearPendingIRQ(TIM3_IRQn);
    NVIC_EnableIRQ(TIM3_IRQn);

    gDsbTxActive = true;
    TIM3->DIER = TIM_DIER_UIE;
    TIM3->CR1 = TIM_CR1_CEN;
}

void DSB_TX_Stop(void)
{
    if (!gDsbTxActive)
        return;

    TIM3->DIER = 0u;
    TIM3->CR1 &= ~TIM_CR1_CEN;
    NVIC_DisableIRQ(TIM3_IRQn);
    NVIC_ClearPendingIRQ(TIM3_IRQn);
    gDsbTxActive = false;

    /* Use the normal, conservative SPI path once the ISR can no longer race it. */
    BK4819_WriteRegister(BK4819_REG_36, 0u);
    gDsbTxLastPaWord = 0xFFFFu;
}

uint16_t DSB_TX_LegacyGetVoiceAmplitude(void)
{
    if (isDsbTransmitContext()) {
        DSB_TX_Start(gCurrentVfo->TXP_CalculatedSetting, gCurrentVfo->pTX->Frequency);
        return gDsbTxVoiceAmplitude;
    }

    DSB_TX_Stop();
    return BK4819_GetVoiceAmplitudeOut();
}

void DSB_TX_LegacySetupPowerAmplifier(uint8_t bias, uint32_t frequency)
{
    if (gDsbTxActive && isDsbTransmitContext() &&
        gCurrentVfo != NULL && frequency == gCurrentVfo->pTX->Frequency) {
        /* The legacy HandleTransmit() expression still executes, but TIM3 now
         * owns REG36.  Suppress only that redundant PA write. */
        return;
    }

    BK4819_SetupPowerAmplifier(bias, frequency);
}

void DSB_TX_LegacySendEndOfTransmission(void)
{
    DSB_TX_Stop();
    RADIO_SendEndOfTransmission();
}
