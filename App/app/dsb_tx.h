#ifndef APP_DSB_TX_H
#define APP_DSB_TX_H

#include <stdbool.h>
#include <stdint.h>

bool DSB_TX_IsActive(void);
void DSB_TX_Start(uint8_t peakBias, uint32_t frequency);
void DSB_TX_Stop(void);

/* GNU ld --wrap entry points used only by CN_RF. */
uint16_t __wrap_BK4819_GetVoiceAmplitudeOut(void);
void __wrap_BK4819_SetupPowerAmplifier(uint8_t bias, uint32_t frequency);
void __wrap_RADIO_SendEndOfTransmission(void);

uint16_t __real_BK4819_GetVoiceAmplitudeOut(void);
void __real_BK4819_SetupPowerAmplifier(uint8_t bias, uint32_t frequency);
void __real_RADIO_SendEndOfTransmission(void);

#endif
