#include "app/cn_rf_ops.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "functions.h"
#include "misc.h"
#include "rf_profile.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/menu.h"
#include "ui/ui.h"

#define RF_MONITOR_PAGE 6
#define RF_OPS_PAGE     7
#define RIT_XIT_LIMIT_10HZ 2000

static int16_t sRit10Hz;
static int16_t sXit10Hz;
static uint8_t sOffsetStepIndex = 1u;
static bool sSatSplit;
static bool sSatSaved;
static uint8_t sSavedDualWatch;
static uint8_t sSavedCrossBand;
static uint8_t sMonitorTicks;

static const uint16_t kOffsetStep10Hz[] = {1u, 10u, 100u};

static uint32_t applyOffset(uint32_t frequency, int16_t offset10Hz)
{
    if (offset10Hz >= 0) {
        const uint32_t delta = (uint16_t)offset10Hz;
        if (frequency > UINT32_MAX - delta)
            return UINT32_MAX;
        return frequency + delta;
    }

    const uint32_t delta = (uint16_t)(-offset10Hz);
    return frequency < delta ? 0u : frequency - delta;
}

static bool rfOpsLockedByWfm(void)
{
#ifdef ENABLE_WFM
    if (RADIO_IsWfmActive())
        return true;
    if (gRxVfo != NULL && gRxVfo->Modulation == MODULATION_WFM)
        return true;
    if (gTxVfo != NULL && gTxVfo->Modulation == MODULATION_WFM)
        return true;
#endif
    return false;
}

uint32_t CN_RF_OPS_AdjustRxFrequency(uint32_t frequency)
{
    return applyOffset(frequency, sRit10Hz);
}

uint32_t CN_RF_OPS_AdjustTxFrequency(uint32_t frequency)
{
    return applyOffset(frequency, sXit10Hz);
}

void __wrap_RF_PROFILE_ApplyRx(const VFO_Info_t *vfo)
{
    __real_RF_PROFILE_ApplyRx(vfo);

#ifdef ENABLE_WFM
    if (vfo->Modulation == MODULATION_WFM)
        return;
#endif
    if (sRit10Hz != 0)
        BK4819_SetFrequency(CN_RF_OPS_AdjustRxFrequency(vfo->pRX->Frequency));
}

void __wrap_RF_PROFILE_ApplyTx(const VFO_Info_t *vfo)
{
    __real_RF_PROFILE_ApplyTx(vfo);
#ifdef ENABLE_WFM
    if (vfo->Modulation == MODULATION_WFM)
        return;
#endif
    /* RADIO_SetTxParameters writes the base PLL frequency first. Applying XIT
     * here occurs before DSB TIM3 or CW PA keying starts; neither path rewrites
     * REG38/39, so the offset remains stable for the whole transmission. */
    if (sXit10Hz != 0)
        BK4819_SetFrequency(CN_RF_OPS_AdjustTxFrequency(vfo->pTX->Frequency));
}

void __wrap_RF_PROFILE_TimeSlice10ms(void)
{
    /* FAST/NORM/SLOW now live entirely in rf_profile.c. This wrapper only adds
     * RF/SAT UI housekeeping, leaving the backend as the sole AGC/REG13 owner. */
    __real_RF_PROFILE_TimeSlice10ms();
    CN_RF_OPS_TimeSlice10ms();
}

static bool isSysInfo(void)
{
    return gIsInSubMenu && UI_MENU_GetCurrentMenuId() == MENU_VOL;
}

bool CN_RF_OPS_IsCustomMenuPage(void)
{
    return isSysInfo() &&
           (gSubMenuSelection == RF_MONITOR_PAGE || gSubMenuSelection == RF_OPS_PAGE);
}

static void requestMenuRefresh(void)
{
    gRequestDisplayScreen = DISPLAY_MENU;
    gUpdateDisplay = true;
    gMenuCountdown = menu_timeout_long_500ms;
}

static void retuneRxNow(void)
{
    if (gRxVfo == NULL || gCurrentFunction == FUNCTION_TRANSMIT || gRxIdleMode ||
        rfOpsLockedByWfm())
        return;
    BK4819_SetFrequency(CN_RF_OPS_AdjustRxFrequency(gRxVfo->pRX->Frequency));
}

static void changeOffset(int16_t *value, int8_t direction)
{
    const int16_t step = (int16_t)kOffsetStep10Hz[sOffsetStepIndex];
    int32_t next = (int32_t)*value + ((direction > 0) ? step : -step);
    if (next > RIT_XIT_LIMIT_10HZ)
        next = RIT_XIT_LIMIT_10HZ;
    else if (next < -RIT_XIT_LIMIT_10HZ)
        next = -RIT_XIT_LIMIT_10HZ;
    *value = (int16_t)next;
}

static bool toggleSatSplit(void)
{
    if (!sSatSplit) {
#ifdef ENABLE_WFM
        /* SAT split owns BK4829 RX/TX selection. Never activate it while either
         * VFO is configured as the independent BK1080 WFM receiver. */
        if (gEeprom.VfoInfo[0].Modulation == MODULATION_WFM ||
            gEeprom.VfoInfo[1].Modulation == MODULATION_WFM)
            return false;
#endif
        if (gCurrentFunction == FUNCTION_TRANSMIT)
            return false;
        sSavedDualWatch = gEeprom.DUAL_WATCH;
        sSavedCrossBand = gEeprom.CROSS_BAND_RX_TX;
        sSatSaved = true;
        sSatSplit = true;
        gEeprom.DUAL_WATCH = DUAL_WATCH_OFF;
        /* Current main VFO remains TX; the opposite VFO becomes RX. */
        gEeprom.CROSS_BAND_RX_TX = (uint8_t)(gEeprom.TX_VFO + 1u);
#ifdef ENABLE_FEAT_F4HWN
        /* SETTINGS_SaveSettings keeps the persistent RX mode in gDW/gCB while
         * this temporary satellite mode is active. */
        gSaveRxMode = false;
#endif
    } else {
        sSatSplit = false;
        if (sSatSaved) {
            gEeprom.DUAL_WATCH = sSavedDualWatch;
            gEeprom.CROSS_BAND_RX_TX = sSavedCrossBand;
        }
        sSatSaved = false;
#ifdef ENABLE_FEAT_F4HWN
        gSaveRxMode = false;
#endif
    }

    RADIO_SelectVfos();
    gFlagReconfigureVfos = true;
    gUpdateStatus = true;
    gUpdateDisplay = true;
    return true;
}

bool CN_RF_OPS_HandleMenuKey(KEY_Code_t key, bool pressed, bool held)
{
    if (!isSysInfo())
        return false;

    /* Pages 6/7 are part of the normal MENU_VOL range in CN_RF builds.
     * Legacy pages 0..5 therefore use the stock menu navigation path. */

    if (gSubMenuSelection < RF_MONITOR_PAGE || gSubMenuSelection > RF_OPS_PAGE)
        return false;

    if (key == KEY_UP || key == KEY_DOWN) {
        if (pressed && !held) {
            if (key == KEY_UP)
                gSubMenuSelection = (gSubMenuSelection == RF_MONITOR_PAGE) ? RF_OPS_PAGE : 0;
            else
                gSubMenuSelection = (gSubMenuSelection == RF_OPS_PAGE) ? RF_MONITOR_PAGE : 5;
            gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
            requestMenuRefresh();
        }
        return true;
    }

    if (gSubMenuSelection != RF_OPS_PAGE)
        return false;

    if (key >= KEY_0 && key <= KEY_9) {
        if (pressed && !held) {
            bool accepted = true;
            const bool wfmLocked = rfOpsLockedByWfm();
            switch (key) {
            case KEY_1:
                if (wfmLocked) { accepted = false; break; }
                changeOffset(&sRit10Hz, 1);
                retuneRxNow();
                break;
            case KEY_7:
                if (wfmLocked) { accepted = false; break; }
                changeOffset(&sRit10Hz, -1);
                retuneRxNow();
                break;
            case KEY_2:
                if (wfmLocked) { accepted = false; break; }
                changeOffset(&sXit10Hz, 1);
                break;
            case KEY_8:
                if (wfmLocked) { accepted = false; break; }
                changeOffset(&sXit10Hz, -1);
                break;
            case KEY_3:
                if (wfmLocked) { accepted = false; break; }
                accepted = toggleSatSplit();
                break;
            case KEY_0:
                sRit10Hz = 0;
                sXit10Hz = 0;
                retuneRxNow();
                break;
            default:
                accepted = false;
                break;
            }
            gBeepToPlay = accepted ? BEEP_1KHZ_60MS_OPTIONAL
                                   : BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            requestMenuRefresh();
        }
        return true;
    }

    if (key == KEY_STAR) {
        if (pressed && !held) {
            sOffsetStepIndex++;
            if (sOffsetStepIndex >= (uint8_t)ARRAY_SIZE(kOffsetStep10Hz))
                sOffsetStepIndex = 0u;
            gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
            requestMenuRefresh();
        }
        return true;
    }

    return false;
}

static void drawLine(const char *text, uint8_t y)
{
    UI_PrintStringSmallNormalAt(text, 3u, y);
}

static void drawHeader(const char *title)
{
    UI_DisplayClear();
    UI_DrawLineBuffer(gFrameBuffer, 0, 0, LCD_WIDTH - 1, 0, true);
#ifdef ENABLE_CHINESE
    if (gUiLanguage == UI_LANGUAGE_CN && strcmp(title, "RF OBSERVE") == 0)
        UI_PrintStringSmallAtPixel("RF\xe8\xa7\x82\xe5\xaf\x9f", 3u, 124u, 1u, 12u, 0u);
    else
#endif
        UI_PrintStringSmallNormalAt(title, 3u, 2u);
    UI_DrawLineBuffer(gFrameBuffer, 0, 13, LCD_WIDTH - 1, 13, true);
}

static void formatOffset(char *out, const char *name, int16_t value)
{
    const uint16_t magnitude = value < 0 ? (uint16_t)(-value) : (uint16_t)value;
    sprintf(out, "%s %c%u.%02uk", name, value < 0 ? '-' : '+',
            magnitude / 100u, magnitude % 100u);
}

static void drawMonitor(void)
{
    char text[24];
    RF_Profile_t *profile;

    drawHeader("RF OBSERVE");
    if (gRxVfo == NULL) {
        drawLine("RX NOT READY", 18u);
        return;
    }
    profile = &gRxVfo->RfProfile;

    if (rfOpsLockedByWfm()) {
        drawLine("WFM / BK1080 ACTIVE", 17u);
        drawLine("BK4829 MON LOCKED", 28u);
    } else if (gCurrentFunction == FUNCTION_TRANSMIT) {
        drawLine("TX ACTIVE", 18u);
    } else if (gRxIdleMode) {
        drawLine("RX IDLE", 18u);
    } else {
        const int16_t rssi = BK4819_GetRSSI_dBm();
        const uint8_t noise = BK4819_GetExNoiceIndicator();
        const uint8_t glitch = BK4819_GetGlitchIndicator();
        const uint16_t reg7e = BK4819_ReadRegister(BK4819_REG_7E);
        sprintf(text, "RSSI %ddBm", rssi);
        drawLine(text, 15u);
        sprintf(text, "NOISE %u  GL %u", noise, glitch);
        drawLine(text, 24u);
        sprintf(text, "REG7E %04X", reg7e);
        drawLine(text, 33u);
    }

    if (profile->agc == RF_AGC_AUTO)
        sprintf(text, "AGC AUTO  B%u", profile->rfBoost != 0u);
    else
        sprintf(text, "AGC %s G%u B%u", gRfAgcNames[profile->agc],
                RF_PROFILE_GetRuntimeGainIndex(gRxVfo), profile->rfBoost != 0u);
    drawLine(text, 42u);
    sprintf(text, "BW %s AFC%u NB%u", gRfBandwidthNames[profile->bandwidth],
            profile->afc, profile->noiseBlanker);
    drawLine(text, 51u);
}

static void drawOps(void)
{
    char text[24];

    drawHeader("RF / SAT OPS");
    formatOffset(text, "RIT", sRit10Hz);
    drawLine(text, 14u);
    formatOffset(text, "XIT", sXit10Hz);
    drawLine(text, 22u);

    switch (sOffsetStepIndex) {
    case 0: strcpy(text, "STEP 10Hz  *=NEXT"); break;
    case 2: strcpy(text, "STEP 1kHz  *=NEXT"); break;
    default: strcpy(text, "STEP 100Hz *=NEXT"); break;
    }
    drawLine(text, 30u);

    if (rfOpsLockedByWfm())
        strcpy(text, "WFM: RF OPS LOCK");
    else if (!sSatSplit)
        strcpy(text, "SAT SPLIT OFF");
    else if (gEeprom.TX_VFO == 0u)
        strcpy(text, "SAT TX:A  RX:B");
    else
        strcpy(text, "SAT TX:B  RX:A");
    drawLine(text, 38u);
    /* F4HWN blits framebuffer rows 0..6. Keep the two help rows in
     * rows 5 and 6 rather than y=48/56 (rows 6/7), where the latter is lost. */
    drawLine("1/7 RIT  2/8 XIT", 46u);
    drawLine("3 SAT   0 CLR", 54u);
}

void CN_RF_OPS_DrawMenuPage(void)
{
    if (gSubMenuSelection == RF_MONITOR_PAGE)
        drawMonitor();
    else
        drawOps();
    ST7565_BlitFullScreen();
}

void CN_RF_OPS_TimeSlice10ms(void)
{
    if (sSatSplit &&
        (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF ||
         gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF)) {
        /* A normal RX-mode action explicitly overrides temporary SAT split. */
        sSatSplit = false;
        sSatSaved = false;
    }

    if (!CN_RF_OPS_IsCustomMenuPage()) {
        sMonitorTicks = 0u;
        return;
    }

    if (++sMonitorTicks >= 20u) {
        sMonitorTicks = 0u;
        gUpdateDisplay = true;
    }
}
