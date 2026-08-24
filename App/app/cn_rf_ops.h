#ifndef APP_CN_RF_OPS_H
#define APP_CN_RF_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/keyboard.h"
#include "radio.h"

bool CN_RF_OPS_IsCustomMenuPage(void);
bool CN_RF_OPS_HandleMenuKey(KEY_Code_t key, bool pressed, bool held);
void CN_RF_OPS_DrawMenuPage(void);
void CN_RF_OPS_TimeSlice10ms(void);

uint32_t CN_RF_OPS_AdjustRxFrequency(uint32_t frequency);
uint32_t CN_RF_OPS_AdjustTxFrequency(uint32_t frequency);

/* GNU ld --wrap entry points used only by CN_RF. */
bool __wrap_RF_PROFILE_AgcUsesRfGain(uint8_t agc);
void __wrap_MENU_ProcessKeys(KEY_Code_t key, bool pressed, bool held);
void __wrap_UI_DisplayMenu(void);
void __wrap_RF_PROFILE_ApplyRx(const VFO_Info_t *vfo);
void __wrap_RF_PROFILE_ApplyTx(const VFO_Info_t *vfo);
void __wrap_RF_PROFILE_TimeSlice10ms(void);

bool __real_RF_PROFILE_AgcUsesRfGain(uint8_t agc);
void __real_MENU_ProcessKeys(KEY_Code_t key, bool pressed, bool held);
void __real_UI_DisplayMenu(void);
void __real_RF_PROFILE_ApplyRx(const VFO_Info_t *vfo);
void __real_RF_PROFILE_ApplyTx(const VFO_Info_t *vfo);
void __real_RF_PROFILE_TimeSlice10ms(void);

#endif
