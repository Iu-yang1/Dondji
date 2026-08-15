/*
 * Dondji Firmware
 *
 * CW 键控器：独立实现，只使用现有、已验证的 CW 发射静音接口。
 */

#ifndef APP_CW_KEYER_H
#define APP_CW_KEYER_H

#include <stdbool.h>

#include "driver/keyboard.h"

bool CW_KEYER_IsOpen(void);
void CW_KEYER_Open(void);
bool CW_KEYER_HandleKey(KEY_Code_t key, bool pressed, bool held);
void CW_KEYER_TimeSlice10ms(void);
void CW_KEYER_Display(void);

#endif
