#ifndef APP_DSB_TX_H
#define APP_DSB_TX_H

#include <stdbool.h>
#include <stdint.h>

bool DSB_TX_IsActive(void);
void DSB_TX_Start(uint8_t peakBias, uint32_t frequency);
void DSB_TX_Stop(void);

/* app/app.c compatibility entry points.  That file's legacy 4 kHz DSB hook is
 * redirected to these names only for CN_RF so the TIM3 engine owns REG64/36. */
uint16_t DSB_TX_LegacyGetVoiceAmplitude(void);
void DSB_TX_LegacySetupPowerAmplifier(uint8_t bias, uint32_t frequency);
void DSB_TX_LegacySendEndOfTransmission(void);

#endif
