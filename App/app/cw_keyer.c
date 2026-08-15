/*
 * Dondji Firmware
 *
 * 仅发送国际 Morse 的 A-Z、0-9 和空格。
 * A1A 保持 PLL/TX link 连续工作，点划沿只门控 PA-CTL 和板级 PA 使能。
 * 本路径依据公开寄存器定义独立实现，不复制闭源固件代码。
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
#include "driver/system.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"

#define CW_KEYER_FLASH_BASE  0x062000u
#define CW_KEYER_FLASH_END   0x063000u
#define CW_KEYER_TEXT_SIZE   33u
#define CW_KEYER_VISIBLE_TEXT 16u
#define CW_KEYER_WPM_MIN     5u
#define CW_KEYER_WPM_MAX     30u
#define CW_KEYER_WPM_DEFAULT 18u
#define CW_KEYER_BEACON_INTERVAL_MIN     15u
#define CW_KEYER_BEACON_INTERVAL_MAX     600u
#define CW_KEYER_BEACON_INTERVAL_STEP    15u
#define CW_KEYER_BEACON_INTERVAL_DEFAULT 60u
#define CW_KEYER_SAVE_DELAY_TICKS         200u

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
} cw_keyer_config_v1_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint16_t beaconIntervalSeconds;
    uint8_t wpm;
    uint8_t crc;
} cw_keyer_config_t;

typedef enum {
    CW_KEYER_FIELD_WPM,
    CW_KEYER_FIELD_TEXT,
    CW_KEYER_FIELD_BEACON,
    CW_KEYER_FIELD_COUNT
} cw_keyer_field_t;

/* 位 0..4 是点划图样；位 5..7 是长度减一。表来自国际 Morse 标准编码。 */
static const uint8_t kMorse[36] = {
    0x21, 0x68, 0x6A, 0x44, 0x00, 0x62, 0x46, 0x60, 0x20, 0x67,
    0x45, 0x64, 0x23, 0x22, 0x47, 0x66, 0x6D, 0x42, 0x40, 0x01,
    0x41, 0x61, 0x43, 0x69, 0x6B, 0x6C,
    0x9F, 0x8F, 0x87, 0x83, 0x81, 0x80, 0x90, 0x98, 0x9C, 0x9E
};

static bool sOpen;
static cw_keyer_field_t sEditField = CW_KEYER_FIELD_WPM;
static bool sConfigLoaded;
static uint8_t sWpm = CW_KEYER_WPM_DEFAULT;
static uint16_t sBeaconIntervalSeconds;
static uint16_t sBeaconCountdown;
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
static bool sCarrierOn;
static uint8_t sConfigSaveTicks;

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

static bool configV1Valid(const cw_keyer_config_v1_t *config)
{
    return memcmp(config->magic, "CWK1", 4) == 0 &&
           config->crc == crc8((const uint8_t *)config, sizeof(*config) - 1u) &&
           config->wpm >= CW_KEYER_WPM_MIN && config->wpm <= CW_KEYER_WPM_MAX;
}

static bool configValid(const cw_keyer_config_t *config)
{
    return memcmp(config->magic, "CWK2", 4) == 0 &&
           config->crc == crc8((const uint8_t *)config, sizeof(*config) - 1u) &&
           config->wpm >= CW_KEYER_WPM_MIN && config->wpm <= CW_KEYER_WPM_MAX &&
           (config->beaconIntervalSeconds == 0u ||
            (config->beaconIntervalSeconds >= CW_KEYER_BEACON_INTERVAL_MIN &&
             config->beaconIntervalSeconds <= CW_KEYER_BEACON_INTERVAL_MAX &&
             (config->beaconIntervalSeconds % CW_KEYER_BEACON_INTERVAL_STEP) == 0u));
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
    cw_keyer_config_v1_t configV1;
    if (sConfigLoaded)
        return;
    PY25Q16_ReadBuffer(CW_KEYER_FLASH_BASE, &config, sizeof(config));
    if (configValid(&config)) {
        sWpm = config.wpm;
        sBeaconIntervalSeconds = config.beaconIntervalSeconds;
    } else {
        PY25Q16_ReadBuffer(CW_KEYER_FLASH_BASE, &configV1, sizeof(configV1));
        if (configV1Valid(&configV1))
            sWpm = configV1.wpm; /* CWK1 平滑迁移；信标保持默认关闭。 */
    }
    sConfigLoaded = true;
}

static void saveConfig(void)
{
    cw_keyer_config_t previous;
    cw_keyer_config_t config;
    cw_keyer_config_v1_t previousV1;
    sConfigSaveTicks = 0u;
    PY25Q16_ReadBuffer(CW_KEYER_FLASH_BASE, &previous, sizeof(previous));
    PY25Q16_ReadBuffer(CW_KEYER_FLASH_BASE, &previousV1, sizeof(previousV1));
    if (!configValid(&previous) && !configV1Valid(&previousV1) && !configSectorBlank())
        return; /* 未知版本绝不覆盖。 */
    memcpy(config.magic, "CWK2", 4);
    config.wpm = sWpm;
    config.beaconIntervalSeconds = sBeaconIntervalSeconds;
    config.crc = crc8((const uint8_t *)&config, sizeof(config) - 1u);
    if (memcmp(&previous, &config, sizeof(config)) != 0)
        PY25Q16_WriteBuffer(CW_KEYER_FLASH_BASE, &config, sizeof(config));
}

static void scheduleConfigSave(void)
{
    sConfigSaveTicks = CW_KEYER_SAVE_DELAY_TICKS;
}

static void restartBeaconCountdown(void)
{
    sBeaconCountdown = (uint16_t)(sBeaconIntervalSeconds * 100u);
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
    const uint8_t power = gCurrentVfo->TXP_CalculatedSetting;

    /* 2 ms 分级上升沿；避免 PA-CTL 与板级 PA 同时硬切产生宽带键控杂散。 */
    BK4819_SetupPowerAmplifier(power >> 1, gCurrentVfo->pTX->Frequency);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);
    sCarrierOn = true;
    SYSTEM_DelayMs(2);
    BK4819_SetupPowerAmplifier(power, gCurrentVfo->pTX->Frequency);
}

static void keyUp(void)
{
    const uint8_t power = gCurrentVfo->TXP_CalculatedSetting;

    if (!sCarrierOn)
        return;
    /* 2 ms 分级下降沿；最终先断开板级 PA，再把 PA-CTL/偏置归零。 */
    BK4819_SetupPowerAmplifier(power >> 1, gCurrentVfo->pTX->Frequency);
    SYSTEM_DelayMs(2);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_WriteRegister(BK4819_REG_36, 0u);
    sCarrierOn = false;
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
        GENERIC_Key_PTT(false);
#ifdef ENABLE_FEAT_F4HWN
        /* PTT Toggle 进入“等待松键”状态，避免用户仍按住时自动再次发射。 */
        if (gSetting_set_ptt_session)
            gPttOnePushCounter = 3u;
#endif
    }
    restartBeaconCountdown();
    gUpdateDisplay = true;
    gUpdateStatus = true;
}

static bool beginTransmission(bool automatic)
{
#ifdef ENABLE_VOX
    if (automatic && gEeprom.VOX_SWITCH)
        return false; /* 自动信标不伪造物理 PTT；VOX 开启时保持拒绝。 */
#endif
    if (sTextLength == 0u || sState != CW_KEYER_IDLE || gTxVfo->Modulation != MODULATION_CW) {
        if (!automatic)
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        return false;
    }
    sTextIndex = 0;
    sTxRejected = false;
    sState = CW_KEYER_WAIT_TX;
    sStartWaitTicks = 100u;
    GENERIC_Key_PTT(true); /* 保留 TX Lock、TOT、BCL、电池和功率保护。 */
    gUpdateDisplay = true;
    return true;
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
    restartBeaconCountdown();
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
    sEditField = CW_KEYER_FIELD_WPM;
    sState = CW_KEYER_IDLE;
    sCarrierOn = false;
    sTxRejected = false;
    sTapKey = KEY_INVALID;
    sTapCountdown = 0;
    sTextLength = 0;
    sText[0] = '\0';
    restartBeaconCountdown();
    gUpdateDisplay = true;
    gUpdateStatus = true;
}

bool CW_KEYER_HandleKey(KEY_Code_t key, bool pressed, bool held)
{
    if (!sOpen)
        return false;

    if (key == KEY_PTT) {
        if (pressed && !held) {
            if (sState == CW_KEYER_IDLE)
                beginTransmission(false);
            else
                /* PTT 是整段报文的启动/中止键，松开不再截断点划。 */
                finishTransmission();
        }
        return true;
    }

    if (sState != CW_KEYER_IDLE)
        return true; /* 发射中屏蔽编辑键；再次按下 PTT 才中止整段报文。 */
    if (pressed && !held)
        return true;

    if (key == KEY_EXIT && pressed && held) {
        if (sConfigSaveTicks != 0u)
            saveConfig();
        sOpen = false;
        gUpdateDisplay = true;
        gUpdateStatus = true;
        return true;
    }
    if (pressed || held)
        return true;

    switch (key) {
    case KEY_MENU:
        sEditField = (cw_keyer_field_t)((sEditField + 1u) % CW_KEYER_FIELD_COUNT);
        break;
    case KEY_UP:
        if (sEditField == CW_KEYER_FIELD_WPM && sWpm < CW_KEYER_WPM_MAX) {
            sWpm++;
            scheduleConfigSave();
        } else if (sEditField == CW_KEYER_FIELD_BEACON) {
            if (sBeaconIntervalSeconds == 0u)
                sBeaconIntervalSeconds = CW_KEYER_BEACON_INTERVAL_DEFAULT;
            else if (sBeaconIntervalSeconds < CW_KEYER_BEACON_INTERVAL_MAX)
                sBeaconIntervalSeconds += CW_KEYER_BEACON_INTERVAL_STEP;
            restartBeaconCountdown();
            scheduleConfigSave();
        }
        break;
    case KEY_DOWN:
        if (sEditField == CW_KEYER_FIELD_WPM && sWpm > CW_KEYER_WPM_MIN) {
            sWpm--;
            scheduleConfigSave();
        } else if (sEditField == CW_KEYER_FIELD_BEACON && sBeaconIntervalSeconds != 0u) {
            if (sBeaconIntervalSeconds <= CW_KEYER_BEACON_INTERVAL_MIN)
                sBeaconIntervalSeconds = 0u;
            else
                sBeaconIntervalSeconds -= CW_KEYER_BEACON_INTERVAL_STEP;
            restartBeaconCountdown();
            scheduleConfigSave();
        }
        break;
    case KEY_EXIT:
        if (sTextLength != 0u) {
            sText[--sTextLength] = '\0';
            sTapKey = KEY_INVALID;
            restartBeaconCountdown();
        }
        break;
    case KEY_STAR:
        sTextLength = 0;
        sText[0] = '\0';
        sTapKey = KEY_INVALID;
        restartBeaconCountdown();
        break;
    case KEY_0 ... KEY_9:
        if (sEditField == CW_KEYER_FIELD_TEXT)
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
    if (sConfigSaveTicks != 0u && --sConfigSaveTicks == 0u)
        saveConfig();
    if (sTapCountdown != 0u && --sTapCountdown == 0u)
        sTapKey = KEY_INVALID;

    if (sState == CW_KEYER_IDLE) {
        if (sBeaconIntervalSeconds == 0u || sTextLength == 0u)
            return;
        if (sBeaconCountdown != 0u) {
            sBeaconCountdown--;
            return;
        }
        if (!beginTransmission(true))
            restartBeaconCountdown();
        return;
    }
    if (sState == CW_KEYER_WAIT_TX) {
        if (gCurrentFunction == FUNCTION_TRANSMIT) {
            /* RADIO_SetTxParameters 已经将调制和 PA 关闭；第一个点划才开载波。 */
            keyUp();
            if (!startCharacter())
                finishTransmission();
            return;
        }
        if (sStartWaitTicks != 0u && --sStartWaitTicks != 0u)
            return;
        sState = CW_KEYER_IDLE;
        gFlagPrepareTX = false;
        restartBeaconCountdown();
        sTxRejected = true;
        gUpdateDisplay = true;
        return;
    }
    if (gCurrentFunction != FUNCTION_TRANSMIT || gFlagEndTransmission) {
        keyUp(); /* TOT、低电或其他外部中止后立即关闭 PA。 */
        if (gCurrentFunction == FUNCTION_TRANSMIT)
            GENERIC_Key_PTT(false); /* 清除 gFlagEndTransmission 并完成通用 RX/状态恢复。 */
        sState = CW_KEYER_IDLE;
        restartBeaconCountdown();
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
    char beacon[12];
    char text[CW_KEYER_VISIBLE_TEXT + 1u];
    uint8_t textLength = sTextLength;
    const char *textSource = sText;

    if (textLength > CW_KEYER_VISIBLE_TEXT) {
        textSource = &sText[textLength - CW_KEYER_VISIBLE_TEXT];
        textLength = CW_KEYER_VISIBLE_TEXT;
    }
    if (textLength == 0u)
        text[textLength++] = '-';
    else
        memcpy(text, textSource, textLength);
    text[textLength] = '\0';

    UI_DisplayClear();
    /* 键控器独占 128x64，避免主状态栏与标题在照片所示位置重叠。 */
#ifdef ENABLE_CHINESE
    if (gUiLanguage == UI_LANGUAGE_CN)
        UI_PrintStringSmallAtPixel("CW 键控器", 0, 68, 9, 20, 0);
    else
#endif
        UI_PrintStringSmallBold("CW KEYER", 0, 68, 0);

    /* MENU 在速度、内容和信标周期之间切换；双边框表示当前字段。 */
    UI_DrawRectangleBuffer(gFrameBuffer, 3, 17, 124, 41, true);
    UI_DrawRectangleBuffer(gFrameBuffer, 3, 44, 124, 62, true);
    if (sEditField == CW_KEYER_FIELD_WPM)
        UI_DrawRectangleBuffer(gFrameBuffer, 1, 15, 126, 43, true);
    else if (sEditField == CW_KEYER_FIELD_TEXT)
        UI_DrawRectangleBuffer(gFrameBuffer, 1, 42, 126, 63, true);
    else
        UI_DrawRectangleBuffer(gFrameBuffer, 70, 0, 127, 13, true);

    snprintf(line, sizeof(line), "%u WPM", sWpm);
    UI_PrintString(line, 0, LCD_WIDTH - 1u, 3, 8);
    UI_PrintStringSmallBold(text, 0, LCD_WIDTH - 1u, 6);

    if (sState != CW_KEYER_IDLE) {
        strcpy(beacon, "TX");
    } else if (sTxRejected) {
        strcpy(beacon, "!");
    } else {
#ifdef ENABLE_CHINESE
        if (gUiLanguage == UI_LANGUAGE_CN) {
            if (sBeaconIntervalSeconds == 0u)
                strcpy(beacon, "信标关");
            else
                snprintf(beacon, sizeof(beacon), "信标%us", sBeaconIntervalSeconds);
        } else
#endif
        if (sBeaconIntervalSeconds == 0u)
            strcpy(beacon, "BCN OFF");
        else
            snprintf(beacon, sizeof(beacon), "BCN%uS", sBeaconIntervalSeconds);
    }
#ifdef ENABLE_CHINESE
    if (gUiLanguage == UI_LANGUAGE_CN && sState == CW_KEYER_IDLE && !sTxRejected)
        UI_PrintStringSmallAtPixel(beacon, 72, LCD_WIDTH - 1u, 9, 20, 0);
    else
#endif
        UI_PrintStringSmallBold(beacon, 72, LCD_WIDTH - 1u, 0);
    ST7565_BlitFullScreenDualVfoTightTop();
}
