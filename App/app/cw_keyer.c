/*
 * Dondji Firmware
 *
 * 仅发送国际 Morse 的 A-Z、0-9 和空格。实现不使用 IJV 或其他闭源固件代码；
 * 载波键控只调用 BK4829 驱动已有的 TX mute 接口。
 */

#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "app/cw_keyer.h"
#include "app/generic.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "driver/py25q16.h"
#include "driver/st7565.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"

#define CW_KEYER_FLASH_BASE  0x062000u
#define CW_KEYER_FLASH_END   0x063000u
#define CW_KEYER_TEXT_SIZE   33u
#define CW_KEYER_WPM_MIN     5u
#define CW_KEYER_WPM_MAX     30u
#define CW_KEYER_WPM_DEFAULT 18u

_Static_assert(CW_KEYER_FLASH_BASE >= 0x062000u && CW_KEYER_FLASH_END <= 0x063000u,
               "CW 键控器必须使用独立且已审计的外置 Flash 扇区");

typedef enum {
    CW_KEYER_IDLE,
    CW_KEYER_WAIT_TX,
    CW_KEYER_MARK,
    CW_KEYER_ELEMENT_GAP,
    CW_KEYER_CHAR_GAP,
    CW_KEYER_WORD_GAP
} cw_keyer_state_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t wpm;
    uint8_t crc;
} cw_keyer_config_t;

/* 位 0..4 是点划图样；位 5..7 是长度减一。表来自国际 Morse 标准编码。 */
static const uint8_t kMorse[36] = {
    0x21, 0x68, 0x6A, 0x44, 0x00, 0x62, 0x46, 0x60, 0x20, 0x67,
    0x45, 0x64, 0x23, 0x22, 0x47, 0x66, 0x6D, 0x42, 0x40, 0x01,
    0x41, 0x61, 0x43, 0x69, 0x6B, 0x6C,
    0x9F, 0x8F, 0x87, 0x83, 0x81, 0x80, 0x90, 0x98, 0x9C, 0x9E
};

static bool sOpen;
static bool sEditWpm = true;
static bool sConfigLoaded;
static uint8_t sWpm = CW_KEYER_WPM_DEFAULT;
static char sText[CW_KEYER_TEXT_SIZE];
static uint8_t sTextLength;
static KEY_Code_t sTapKey = KEY_INVALID;
static uint8_t sTapIndex;
static uint8_t sTapCountdown;
static cw_keyer_state_t sState;
static uint8_t sTextIndex;
static uint8_t sCode;
static uint8_t sElement;
static uint8_t sElementCount;
static uint16_t sTicks;
static uint8_t sStartWaitTicks;
static bool sTxRejected;

static uint8_t crc8(const uint8_t *data, uint8_t size)
{
    uint8_t crc = 0;
    while (size-- != 0) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++)
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
    }
    return crc;
}

static bool configValid(const cw_keyer_config_t *config)
{
    return memcmp(config->magic, "CWK1", 4) == 0 &&
           config->crc == crc8((const uint8_t *)config, sizeof(*config) - 1u) &&
           config->wpm >= CW_KEYER_WPM_MIN && config->wpm <= CW_KEYER_WPM_MAX;
}

static bool configSectorBlank(void)
{
    uint8_t bytes[16];
    for (uint32_t address = CW_KEYER_FLASH_BASE; address < CW_KEYER_FLASH_END;
         address += sizeof(bytes)) {
        PY25Q16_ReadBuffer(address, bytes, sizeof(bytes));
        for (uint8_t i = 0; i < sizeof(bytes); i++) {
            if (bytes[i] != 0xFFu)
                return false;
        }
    }
    return true;
}

static void loadConfig(void)
{
    cw_keyer_config_t config;
    if (sConfigLoaded)
        return;
    PY25Q16_ReadBuffer(CW_KEYER_FLASH_BASE, &config, sizeof(config));
    if (configValid(&config))
        sWpm = config.wpm;
    sConfigLoaded = true;
}

static void saveConfig(void)
{
    cw_keyer_config_t previous;
    cw_keyer_config_t config;
    PY25Q16_ReadBuffer(CW_KEYER_FLASH_BASE, &previous, sizeof(previous));
    if (!configValid(&previous) && !configSectorBlank())
        return; /* 未知版本绝不覆盖。 */
    memcpy(config.magic, "CWK1", 4);
    config.wpm = sWpm;
    config.crc = crc8((const uint8_t *)&config, sizeof(config) - 1u);
    if (memcmp(&previous, &config, sizeof(config)) != 0)
        PY25Q16_WriteBuffer(CW_KEYER_FLASH_BASE, &config, sizeof(config));
}

static uint8_t dotTicks(void)
{
    return (uint8_t)((120u + (sWpm / 2u)) / sWpm);
}

static bool encode(char c, uint8_t *code)
{
    if (c >= 'a' && c <= 'z')
        c -= (char)('a' - 'A');
    if (c >= 'A' && c <= 'Z') {
        *code = kMorse[(uint8_t)(c - 'A')];
        return true;
    }
    if (c >= '0' && c <= '9') {
        *code = kMorse[26u + (uint8_t)(c - '0')];
        return true;
    }
    return false;
}

static void keyDown(void)
{
    BK4819_ExitTxMute();
}

static void keyUp(void)
{
    BK4819_EnterTxMute();
}

static void startElement(void)
{
    const bool dash = ((sCode >> (sElementCount - 1u - sElement)) & 1u) != 0;
    keyDown();
    sTicks = (uint16_t)dotTicks() * (dash ? 3u : 1u);
    sState = CW_KEYER_MARK;
}

static bool startCharacter(void)
{
    while (sTextIndex < sTextLength && !encode(sText[sTextIndex], &sCode))
        sTextIndex++;
    if (sTextIndex >= sTextLength)
        return false;
    sElement = 0;
    sElementCount = (sCode >> 5) + 1u;
    startElement();
    return true;
}

static void finishTransmission(void)
{
    keyUp();
    sState = CW_KEYER_IDLE;
    sStartWaitTicks = 0;
    gFlagPrepareTX = false;
    if (gCurrentFunction == FUNCTION_TRANSMIT) {
        APP_EndTransmission();
        FUNCTION_Select(FUNCTION_FOREGROUND);
        gFlagEndTransmission = false;
        RADIO_SetVfoState(VFO_STATE_NORMAL);
#ifdef ENABLE_FEAT_F4HWN
        /* PTT Toggle 进入“等待松键”状态，避免用户仍按住时自动再次发射。 */
        if (gSetting_set_ptt_session && gPttOnePushCounter == 1u)
            gPttOnePushCounter = 3u;
#endif
    }
    gUpdateDisplay = true;
    gUpdateStatus = true;
}

static void beginTransmission(void)
{
    if (sTextLength == 0u || sState != CW_KEYER_IDLE || gTxVfo->Modulation != MODULATION_CW) {
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        return;
    }
    sTextIndex = 0;
    sTxRejected = false;
    sState = CW_KEYER_WAIT_TX;
    sStartWaitTicks = 100u;
    GENERIC_Key_PTT(true); /* 保留 TX Lock、TOT、BCL、电池和功率保护。 */
    gUpdateDisplay = true;
}

static const char *keyCharacters(KEY_Code_t key)
{
    switch (key) {
    case KEY_0: return " 0";
    case KEY_1: return "1";
    case KEY_2: return "ABC2";
    case KEY_3: return "DEF3";
    case KEY_4: return "GHI4";
    case KEY_5: return "JKL5";
    case KEY_6: return "MNO6";
    case KEY_7: return "PQRS7";
    case KEY_8: return "TUV8";
    case KEY_9: return "WXYZ9";
    default:    return 0;
    }
}

static void appendCharacter(KEY_Code_t key)
{
    const char *chars = keyCharacters(key);
    const uint8_t count = chars == 0 ? 0u : (uint8_t)strlen(chars);
    if (count == 0u)
        return;
    if (key == sTapKey && sTapCountdown != 0u && sTextLength != 0u) {
        sTapIndex = (uint8_t)((sTapIndex + 1u) % count);
        sText[sTextLength - 1u] = chars[sTapIndex];
    } else if (sTextLength < CW_KEYER_TEXT_SIZE - 1u) {
        sTapKey = key;
        sTapIndex = 0;
        sText[sTextLength++] = chars[0];
        sText[sTextLength] = '\0';
    } else {
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        return;
    }
    sTapCountdown = 70u;
    gUpdateDisplay = true;
}

bool CW_KEYER_IsOpen(void)
{
    return sOpen;
}

void CW_KEYER_Open(void)
{
    if (gTxVfo->Modulation != MODULATION_CW)
        return;
    loadConfig();
    sOpen = true;
    sEditWpm = true;
    sState = CW_KEYER_IDLE;
    sTxRejected = false;
    sTapKey = KEY_INVALID;
    sTapCountdown = 0;
    sTextLength = 0;
    sText[0] = '\0';
    gUpdateDisplay = true;
    gUpdateStatus = true;
}

bool CW_KEYER_HandleKey(KEY_Code_t key, bool pressed, bool held)
{
    if (!sOpen)
        return false;

    if (key == KEY_PTT) {
        if (pressed && !held)
            beginTransmission();
        else if (!pressed) {
            if (gCurrentFunction == FUNCTION_TRANSMIT) {
                if (gFlagEndTransmission)
                    GENERIC_Key_PTT(false); /* 处理 TOT 等外部结束留下的 TX 状态。 */
                else
                    finishTransmission();
            } else if (sState != CW_KEYER_IDLE) {
                finishTransmission();
            }
        }
        return true;
    }

    if (sState != CW_KEYER_IDLE)
        return true; /* 发射中仅允许物理 PTT 松开来中止。 */
    if (pressed && !held)
        return true;

    if (key == KEY_EXIT && pressed && held) {
        sOpen = false;
        gUpdateDisplay = true;
        return true;
    }
    if (pressed || held)
        return true;

    switch (key) {
    case KEY_MENU:
        sEditWpm = !sEditWpm;
        break;
    case KEY_UP:
        if (sEditWpm && sWpm < CW_KEYER_WPM_MAX) {
            sWpm++;
            saveConfig();
        }
        break;
    case KEY_DOWN:
        if (sEditWpm && sWpm > CW_KEYER_WPM_MIN) {
            sWpm--;
            saveConfig();
        }
        break;
    case KEY_EXIT:
        if (sTextLength != 0u) {
            sText[--sTextLength] = '\0';
            sTapKey = KEY_INVALID;
        }
        break;
    case KEY_STAR:
        sTextLength = 0;
        sText[0] = '\0';
        sTapKey = KEY_INVALID;
        break;
    case KEY_0 ... KEY_9:
        if (!sEditWpm)
            appendCharacter(key);
        break;
    default:
        return true;
    }
    gUpdateDisplay = true;
    return true;
}

void CW_KEYER_TimeSlice10ms(void)
{
    if (!sOpen)
        return;
    if (sTapCountdown != 0u && --sTapCountdown == 0u)
        sTapKey = KEY_INVALID;

    if (sState == CW_KEYER_IDLE)
        return;
    if (sState == CW_KEYER_WAIT_TX) {
        if (gCurrentFunction == FUNCTION_TRANSMIT) {
            if (!startCharacter())
                finishTransmission();
            return;
        }
        if (sStartWaitTicks != 0u && --sStartWaitTicks != 0u)
            return;
        sState = CW_KEYER_IDLE;
        sTxRejected = true;
        gUpdateDisplay = true;
        return;
    }
    if (gCurrentFunction != FUNCTION_TRANSMIT || gFlagEndTransmission) {
        keyUp(); /* TOT、低电或其他外部中止后的保守静音。 */
        sState = CW_KEYER_IDLE;
        gUpdateDisplay = true;
        return;
    }
    if (sTicks != 0u && --sTicks != 0u)
        return;

    switch (sState) {
    case CW_KEYER_MARK:
        keyUp();
        if (++sElement < sElementCount) {
            sState = CW_KEYER_ELEMENT_GAP;
            sTicks = dotTicks();
        } else {
            sTextIndex++;
            if (sTextIndex < sTextLength && sText[sTextIndex] == ' ') {
                while (sTextIndex < sTextLength && sText[sTextIndex] == ' ')
                    sTextIndex++;
                sState = CW_KEYER_WORD_GAP;
                sTicks = (uint16_t)dotTicks() * 7u;
            } else {
                sState = CW_KEYER_CHAR_GAP;
                sTicks = (uint16_t)dotTicks() * 3u;
            }
        }
        break;
    case CW_KEYER_ELEMENT_GAP:
        startElement();
        break;
    case CW_KEYER_CHAR_GAP:
    case CW_KEYER_WORD_GAP:
        if (!startCharacter())
            finishTransmission();
        break;
    default:
        break;
    }
}

void CW_KEYER_Display(void)
{
    char line[22];
    const char *text = sText;
    if (sTextLength > 20u)
        text = &sText[sTextLength - 20u];

    UI_DisplayClear();
#ifdef ENABLE_CHINESE
    if (gUiLanguage == UI_LANGUAGE_CN) {
        UI_PrintStringSmallAtPixel("CW 键控器", 0, 127, 1, 12, 0);
        UI_PrintStringSmallAtPixel(sEditWpm ? "速度  ▲▼ 调节" : "内容  数字键输入", 0, 127, 19, 30, 0);
        UI_PrintStringSmallAtPixel("菜单切换  *清空  EXIT删除", 0, 127, 47, 58, 0);
    } else
#endif
    {
        UI_PrintStringSmallBold("CW KEYER", 0, 127, 0);
        UI_PrintStringSmallNormal(sEditWpm ? "WPM: UP/DOWN" : "TEXT: 2-9 ABC", 0, 127, 2);
        UI_PrintStringSmallNormal("MENU FIELD *CLEAR EXIT DEL", 0, 127, 6);
    }
    snprintf(line, sizeof(line), "WPM %u", sWpm);
    UI_PrintStringSmallBold(line, 0, 127, 1);
    UI_PrintStringSmallNormal(text, 0, 127, 4);
    if (sState != CW_KEYER_IDLE) {
#ifdef ENABLE_CHINESE
        if (gUiLanguage == UI_LANGUAGE_CN)
            UI_PrintStringSmallAtPixel("正在 CW 发射", 0, 127, 40, 46, 0);
        else
#endif
            UI_PrintStringSmallBold("TX CW", 0, 127, 5);
    } else if (sTxRejected) {
#ifdef ENABLE_CHINESE
        if (gUiLanguage == UI_LANGUAGE_CN)
            UI_PrintStringSmallAtPixel("发射被保护功能拒绝", 0, 127, 40, 46, 0);
        else
#endif
            UI_PrintStringSmallBold("TX BLOCKED", 0, 127, 5);
    } else {
#ifdef ENABLE_CHINESE
        if (gUiLanguage == UI_LANGUAGE_CN)
            UI_PrintStringSmallAtPixel("PTT 发射  长按EXIT返回", 0, 127, 40, 46, 0);
        else
#endif
            UI_PrintStringSmallNormal("PTT SEND / HOLD EXIT BACK", 0, 127, 5);
    }
    ST7565_BlitFullScreen();
}
