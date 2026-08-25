#include "ui/satellite.h"

#include <stdio.h>

#include "app/satellite.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "functions.h"
#include "ui/helper.h"

static void formatFrequency(char out[10], uint32_t hz)
{
    const uint32_t f10 = (hz + 5u) / 10u;
    sprintf(out, "%03lu.%05lu", (unsigned long)(f10 / 100000u),
            (unsigned long)(f10 % 100000u));
}

static void formatDoppler(char out[12], int32_t hz, uint8_t decimals)
{
    const bool negative = hz < 0;
    const uint32_t magnitude = negative ? (uint32_t)(-(int64_t)hz) : (uint32_t)hz;
    if (decimals == 1u)
        sprintf(out, "%c%lu.%luk", negative ? '-' : '+',
                (unsigned long)(magnitude / 1000u),
                (unsigned long)((magnitude % 1000u) / 100u));
    else
        sprintf(out, "%c%lu.%02luk", negative ? '-' : '+',
                (unsigned long)(magnitude / 1000u),
                (unsigned long)((magnitude % 1000u) / 10u));
}

static void drawHeader(const SAT_State_t *state, bool tx)
{
    const uint8_t link = SAT_GetLinkState();
    const char *const name = state->satellite[0] != '\0' ? state->satellite : "SAT";
    const char *const usb = link == SAT_LINK_FRESH ? "USB" :
                            link == SAT_LINK_STALE ? "USB!" : "LOST";

    UI_PrintStringSmallNormalAt(name, 3u, 2u);
    UI_PrintStringSmallNormalAt(usb, link == SAT_LINK_LOST ? 78u : 84u, 2u);
    UI_PrintStringSmallNormalAt(tx ? "TX" : "RX", 112u, 2u);
    UI_DrawLineBuffer(gFrameBuffer, 0, 13, LCD_WIDTH - 1, 13, true);
}

void UI_DisplaySatellite(void)
{
    const SAT_State_t *const state = SAT_GetState();
    const bool tx = gCurrentFunction == FUNCTION_TRANSMIT;
    char frequency[10];
    char otherFrequency[10];
    char doppler[12];
    char otherDoppler[12];
    char line[32];

    UI_DisplayClear();
    drawHeader(state, tx);

    formatFrequency(frequency, tx ? state->tx_hz : state->rx_hz);
    UI_DisplayFrequency(frequency, 8u, 2u, false);

    formatFrequency(otherFrequency, tx ? state->rx_hz : state->tx_hz);
    formatDoppler(otherDoppler, tx ? state->rx_doppler_hz : state->tx_doppler_hz, 1u);
    sprintf(line, "%s %s", tx ? "RX" : "TX", otherFrequency);
    UI_PrintStringSmallNormalAt(line, 3u, 33u);
    UI_PrintStringSmallNormalAt(otherDoppler, 91u, 33u);

    formatDoppler(doppler, tx ? state->tx_doppler_hz : state->rx_doppler_hz, 2u);
    sprintf(line, "DOP %s AZ%d.%d EL%d.%d", doppler,
            state->azimuth_01deg / 10, state->azimuth_01deg < 0 ? -(state->azimuth_01deg % 10) : state->azimuth_01deg % 10,
            state->elevation_01deg / 10, state->elevation_01deg < 0 ? -(state->elevation_01deg % 10) : state->elevation_01deg % 10);
    GUI_DisplaySmallest(line, 3u, 42u, false, true);

    const uint32_t daySeconds = state->utc_unix % 86400u;
    const uint32_t remaining = state->los_unix > state->utc_unix ?
                               state->los_unix - state->utc_unix : 0u;
    sprintf(line, "%02lu:%02lu:%02luZ LOS%02lu:%02lu %uHz",
            (unsigned long)(daySeconds / 3600u),
            (unsigned long)((daySeconds / 60u) % 60u),
            (unsigned long)(daySeconds % 60u),
            (unsigned long)(remaining / 60u),
            (unsigned long)(remaining % 60u),
            state->update_rate_hz);
    GUI_DisplaySmallest(line, 3u, 50u, false, true);

    ST7565_BlitFullScreen();
}

void __real_GUI_DisplayScreen(void);
void __wrap_GUI_DisplayScreen(void)
{
    if (SAT_IsUiVisible())
        UI_DisplaySatellite();
    else
        __real_GUI_DisplayScreen();
}

void __real_UI_DisplayStatus(void);
void __wrap_UI_DisplayStatus(void)
{
    if (!SAT_IsUiVisible())
        __real_UI_DisplayStatus();
}

KEY_Code_t __real_KEYBOARD_Poll(void);
KEY_Code_t __wrap_KEYBOARD_Poll(void)
{
    static bool consumeUntilRelease;
    const KEY_Code_t key = __real_KEYBOARD_Poll();
    const SAT_State_t *const state = SAT_GetState();

    if (consumeUntilRelease) {
        if (key == KEY_INVALID)
            consumeUntilRelease = false;
        return KEY_INVALID;
    }

    if (!state->active)
        return key;

    if (SAT_IsUiVisible()) {
        if (key == KEY_EXIT) {
            SAT_SetUiVisible(false);
            consumeUntilRelease = true;
        }
        return KEY_INVALID;
    }

    if (key == KEY_SIDE2) {
        SAT_SetUiVisible(true);
        consumeUntilRelease = true;
        return KEY_INVALID;
    }

    return key;
}
