#include "app/satellite.h"

#include <string.h>

#include "app/app.h"
#include "dcs.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/crc.h"
#include "driver/gpio.h"
#include "driver/vcp.h"
#include "frequencies.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"

#define SAT_RX_BURST_LIMIT       4u
#define SAT_STALE_TICKS          50u
#define SAT_LOST_TICKS           300u
#define SAT_RATE_WINDOW_TICKS    100u
#define SAT_MAX_INNER_BYTES      (SAT_PROTOCOL_MAX_WIRE_BYTES - 8u)

static const uint8_t kObfuscation[16] = {
    0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40,
    0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80
};

typedef struct {
    VFO_Info_t vfo;
    uint8_t tx_vfo;
    uint8_t rx_vfo;
    uint8_t dual_watch;
    uint8_t cross_band;
} SAT_Snapshot_t;

static SAT_State_t sState;
static SAT_Snapshot_t sSnapshot;
static uint16_t sVcpReadIndex;
static uint8_t sRateTicks;
static uint8_t sUpdatesInWindow;
static bool sRfDirty;
static bool sWaitingPttRelease;

static uint16_t ringIndex(uint16_t index)
{
    return index >= VCP_RX_BUF_SIZE ? (uint16_t)(index - VCP_RX_BUF_SIZE) : index;
}

static uint16_t ringAdvance(uint16_t index, uint16_t count)
{
    return (uint16_t)((index + count) % VCP_RX_BUF_SIZE);
}

static uint16_t ringAvailable(uint16_t read, uint16_t write)
{
    if (write >= VCP_RX_BUF_SIZE)
        write = 0u;
    return write >= read ? (uint16_t)(write - read)
                         : (uint16_t)(write + VCP_RX_BUF_SIZE - read);
}

static uint8_t ringByte(uint16_t start, uint16_t offset)
{
    return VCP_RxBuf[(start + offset) % VCP_RX_BUF_SIZE];
}

static void clearRingBytes(uint16_t start, uint16_t count)
{
    while (count-- != 0u) {
        VCP_RxBuf[start] = 0u;
        start = ringAdvance(start, 1u);
    }
}

static uint32_t hzTo10Hz(uint32_t hz)
{
    return (hz + 5u) / 10u;
}

static uint32_t quantizedHz(uint32_t hz)
{
    return hzTo10Hz(hz) * 10u;
}

static bool validFrequencyHz(uint32_t hz)
{
    const uint32_t f10 = hzTo10Hz(hz);
    return RX_freq_check(f10) == 0;
}

static bool exactCtcss(uint16_t ctcss)
{
    if (ctcss == 0u)
        return true;
    const uint8_t index = DCS_GetCtcssCode(ctcss);
    return index < 50u && CTCSS_Options[index] == ctcss;
}

static bool sequenceIsNewer(uint16_t sequence, uint16_t previous)
{
    const uint16_t delta = (uint16_t)(sequence - previous);
    return delta != 0u && delta < 0x8000u;
}

uint8_t SAT_GetLinkState(void)
{
    if (!sState.active)
        return SAT_LINK_INACTIVE;
    if (sState.age_ticks_10ms <= SAT_STALE_TICKS)
        return SAT_LINK_FRESH;
    if (sState.age_ticks_10ms <= SAT_LOST_TICKS)
        return SAT_LINK_STALE;
    return SAT_LINK_LOST;
}

const SAT_State_t *SAT_GetState(void)
{
    return &sState;
}

bool SAT_IsUiVisible(void)
{
    return sState.active && sState.ui_visible;
}

void SAT_SetUiVisible(bool visible)
{
    if (!sState.active)
        return;
    sState.ui_visible = visible;
    gUpdateDisplay = true;
    gUpdateStatus = true;
    if (visible)
        BACKLIGHT_TurnOn();
}

static void sendFrame(uint16_t id, const void *data, uint16_t dataSize)
{
    static uint8_t frame[SAT_PROTOCOL_MAX_WIRE_BYTES];
    SAT_InnerHeader_t inner;
    const uint16_t innerSize = (uint16_t)(sizeof(inner) + dataSize);
    const uint16_t wireSize = (uint16_t)(innerSize + 8u);

    if (innerSize > SAT_MAX_INNER_BYTES || wireSize > sizeof(frame))
        return;

    frame[0] = 0xABu;
    frame[1] = 0xCDu;
    frame[2] = (uint8_t)innerSize;
    frame[3] = (uint8_t)(innerSize >> 8);

    inner.id = id;
    inner.size = dataSize;
    memcpy(frame + 4u, &inner, sizeof(inner));
    if (dataSize != 0u)
        memcpy(frame + 4u + sizeof(inner), data, dataSize);

    const uint16_t crc = CRC_Calculate(frame + 4u, innerSize);
    frame[4u + innerSize] = (uint8_t)crc;
    frame[5u + innerSize] = (uint8_t)(crc >> 8);
    for (uint16_t i = 0; i < innerSize + 2u; i++)
        frame[4u + i] ^= kObfuscation[i & 0x0Fu];

    frame[6u + innerSize] = 0xDCu;
    frame[7u + innerSize] = 0xBAu;
    VCP_SendAsync(frame, wireSize);
}

static void sendSimpleStatus(uint16_t replyId, uint16_t session, uint16_t status)
{
    SAT_EndReply_t reply = {
        .session_id = session,
        .status = status,
    };
    sendFrame(replyId, &reply, sizeof(reply));
}

static void restoreSession(void)
{
    if (!sState.active)
        return;

    memcpy(&gEeprom.VfoInfo[sSnapshot.tx_vfo], &sSnapshot.vfo, sizeof(sSnapshot.vfo));
    gEeprom.TX_VFO = sSnapshot.tx_vfo;
    gEeprom.RX_VFO = sSnapshot.rx_vfo;
    gEeprom.DUAL_WATCH = sSnapshot.dual_watch;
    gEeprom.CROSS_BAND_RX_TX = sSnapshot.cross_band;
#ifdef ENABLE_FEAT_F4HWN
    gSaveRxMode = false;
#endif
    RADIO_SelectVfos();
    RADIO_SetupRegisters(true);

    memset(&sState, 0, sizeof(sState));
    sRfDirty = false;
    sWaitingPttRelease = false;
    sRateTicks = 0u;
    sUpdatesInWindow = 0u;
    gSerialConfigCountDown_500ms = 0u;
    gUpdateDisplay = true;
    gUpdateStatus = true;
}

static void requestEnd(void)
{
    if (!sState.active)
        return;

    sState.ending = true;
    if (GPIO_IsPttPressed() || gPttIsPressed || gCurrentFunction == FUNCTION_TRANSMIT) {
        sWaitingPttRelease = true;
        gSerialConfigCountDown_500ms = 2u;
        if (gCurrentFunction == FUNCTION_TRANSMIT && !gFlagEndTransmission)
            APP_EndTransmission();
        return;
    }

    restoreSession();
}

static uint16_t startSession(const SAT_Begin_t *begin)
{
    uint16_t error = SAT_ERR_NONE;
    const uint32_t rx10 = hzTo10Hz(begin->rx_base_hz);
    const uint32_t tx10 = hzTo10Hz(begin->tx_base_hz);

    if (begin->major != SAT_PROTOCOL_MAJOR)
        error |= SAT_ERR_BAD_VERSION;
    if (sState.active || SerialConfigInProgress() || gCurrentFunction == FUNCTION_TRANSMIT ||
        gScanStateDir != SCAN_OFF)
        error |= SAT_ERR_BUSY;
    if (!validFrequencyHz(begin->rx_base_hz) || !validFrequencyHz(begin->tx_base_hz))
        error |= SAT_ERR_BAD_FREQUENCY;
    if (!exactCtcss(begin->tx_ctcss_01hz))
        error |= SAT_ERR_BAD_CTCSS;
    if (begin->modulation != SAT_MOD_KEEP && begin->modulation != SAT_MOD_FM)
        error |= SAT_ERR_UNSUPPORTED;
#ifdef ENABLE_WFM
    if (begin->modulation == SAT_MOD_KEEP &&
        gEeprom.VfoInfo[gEeprom.TX_VFO].Modulation == MODULATION_WFM)
        error |= SAT_ERR_UNSUPPORTED;
#endif
    if (error != SAT_ERR_NONE)
        return error;

    const uint8_t vfoIndex = gEeprom.TX_VFO;
    VFO_Info_t *const vfo = &gEeprom.VfoInfo[vfoIndex];

    sSnapshot.tx_vfo = gEeprom.TX_VFO;
    sSnapshot.rx_vfo = gEeprom.RX_VFO;
    sSnapshot.dual_watch = gEeprom.DUAL_WATCH;
    sSnapshot.cross_band = gEeprom.CROSS_BAND_RX_TX;
    memcpy(&sSnapshot.vfo, vfo, sizeof(sSnapshot.vfo));

    memset(&sState, 0, sizeof(sState));
    sState.active = true;
    sState.ui_visible = (begin->flags & SAT_BEGIN_AUTO_SHOW) != 0u;
    sState.session_id = begin->session_id;
    sState.aos_unix = begin->aos_unix;
    sState.los_unix = begin->los_unix;
    sState.rx_hz = quantizedHz(begin->rx_base_hz);
    sState.tx_hz = quantizedHz(begin->tx_base_hz);
    sState.rx_band = (uint8_t)FREQUENCY_GetBand(rx10);
    sState.tx_band = (uint8_t)FREQUENCY_GetBand(tx10);
    for (uint8_t i = 0; i < 12u; i++) {
        const uint8_t c = (uint8_t)begin->satellite[i];
        if (c == 0u)
            break;
        sState.satellite[i] = (c >= 0x20u && c <= 0x7Eu) ? (char)c : '?';
    }

    gEeprom.DUAL_WATCH = DUAL_WATCH_OFF;
    gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
    gEeprom.RX_VFO = vfoIndex;
#ifdef ENABLE_FEAT_F4HWN
    gSaveRxMode = false;
#endif

    vfo->FrequencyReverse = false;
    vfo->TX_OFFSET_FREQUENCY_DIRECTION = TX_OFFSET_FREQUENCY_DIRECTION_OFF;
    vfo->freq_config_RX.Frequency = rx10;
    vfo->freq_config_TX.Frequency = tx10;
    vfo->pRX = &vfo->freq_config_RX;
    vfo->pTX = &vfo->freq_config_TX;
    vfo->Band = (FREQUENCY_Band_t)sState.rx_band;
    if (begin->modulation == SAT_MOD_FM)
        vfo->Modulation = MODULATION_FM;

    if (begin->tx_ctcss_01hz == 0u) {
        vfo->pTX->CodeType = CODE_TYPE_OFF;
        vfo->pTX->Code = 0u;
    } else {
        vfo->pTX->CodeType = CODE_TYPE_CONTINUOUS_TONE;
        vfo->pTX->Code = DCS_GetCtcssCode(begin->tx_ctcss_01hz);
    }

    RADIO_SelectVfos();
    RADIO_ConfigureSquelchAndOutputPower(vfo);
    RADIO_SetupRegisters(true);
    sState.applied_hz = sState.rx_hz;
    sRfDirty = false;
    sRateTicks = 0u;
    sUpdatesInWindow = 0u;
    gUpdateDisplay = true;
    gUpdateStatus = true;
    if (sState.ui_visible)
        BACKLIGHT_TurnOn();

    return SAT_ERR_NONE;
}

static uint16_t updateSession(const SAT_Update_t *update)
{
    if (!sState.active || update->session_id != sState.session_id)
        return SAT_ERR_BAD_SESSION;
    if (sState.ending)
        return SAT_ERR_BUSY;
    if (!validFrequencyHz(update->rx_hz) || !validFrequencyHz(update->tx_hz))
        return SAT_ERR_BAD_FREQUENCY;

    const uint32_t rx10 = hzTo10Hz(update->rx_hz);
    const uint32_t tx10 = hzTo10Hz(update->tx_hz);
    if ((uint8_t)FREQUENCY_GetBand(rx10) != sState.rx_band ||
        (uint8_t)FREQUENCY_GetBand(tx10) != sState.tx_band)
        return SAT_ERR_BAND_CHANGE;

    if (sState.sequence_valid) {
        if (!sequenceIsNewer(update->sequence, sState.received_seq))
            return SAT_ERR_NONE;
        const uint16_t delta = (uint16_t)(update->sequence - sState.received_seq);
        if (delta > 1u) {
            const uint32_t dropped = (uint32_t)sState.dropped_updates + delta - 1u;
            sState.dropped_updates = dropped > UINT16_MAX ? UINT16_MAX : (uint16_t)dropped;
        }
    }

    sState.sequence_valid = true;
    sState.received_seq = update->sequence;
    sState.utc_unix = update->utc_unix;
    sState.rx_hz = quantizedHz(update->rx_hz);
    sState.tx_hz = quantizedHz(update->tx_hz);
    sState.rx_doppler_hz = update->rx_doppler_hz;
    sState.tx_doppler_hz = update->tx_doppler_hz;
    sState.azimuth_01deg = update->azimuth_01deg;
    sState.elevation_01deg = update->elevation_01deg;
    sState.range_km = update->range_km;
    sState.age_ticks_10ms = 0u;
    if (sUpdatesInWindow != UINT8_MAX)
        sUpdatesInWindow++;

    VFO_Info_t *const vfo = &gEeprom.VfoInfo[sSnapshot.tx_vfo];
    vfo->freq_config_RX.Frequency = rx10;
    vfo->freq_config_TX.Frequency = tx10;
    sRfDirty = true;
    return SAT_ERR_NONE;
}

static void sendUpdateReply(void)
{
    SAT_UpdateReply_t reply = {
        .session_id = sState.session_id,
        .received_seq = sState.received_seq,
        .applied_seq = sState.applied_seq,
        .error_flags = sState.error_flags,
        .applied_hz = sState.applied_hz,
        .target_tx_hz = sState.tx_hz,
        .age_ms = (uint16_t)MIN((uint32_t)sState.age_ticks_10ms * 10u, UINT16_MAX),
        .ptt = gCurrentFunction == FUNCTION_TRANSMIT,
        .link_state = SAT_GetLinkState(),
    };
    sendFrame(SAT_REPLY_UPDATE, &reply, sizeof(reply));
}

static void handleCommand(uint16_t id, const uint8_t *data, uint16_t size)
{
    switch (id) {
    case SAT_CMD_HELLO: {
        if (size != sizeof(SAT_Hello_t))
            return;
        SAT_HelloReply_t reply = {
            .major = SAT_PROTOCOL_MAJOR,
            .minor = SAT_PROTOCOL_MINOR,
            .max_update_hz = SAT_PROTOCOL_MAX_UPDATE_HZ,
            .frequency_resolution_hz = SAT_FREQUENCY_RESOLUTION_HZ,
            .max_wire_bytes = SAT_PROTOCOL_MAX_WIRE_BYTES,
            .capabilities = SAT_CAP_FREQ_PAIR | SAT_CAP_HIGH_RATE | SAT_CAP_TRACK_UI |
                            SAT_CAP_CTCSS | SAT_CAP_PHYSICAL_PTT | SAT_CAP_TELEMETRY,
        };
        sendFrame(SAT_REPLY_HELLO, &reply, sizeof(reply));
        break;
    }
    case SAT_CMD_BEGIN: {
        if (size != sizeof(SAT_Begin_t)) {
            sendSimpleStatus(SAT_REPLY_BEGIN, 0u, SAT_ERR_BAD_SIZE);
            return;
        }
        SAT_Begin_t begin;
        memcpy(&begin, data, sizeof(begin));
        const uint16_t status = startSession(&begin);
        SAT_BeginReply_t reply = {
            .session_id = begin.session_id,
            .status = status,
            .applied_rx_hz = status == 0u ? sState.applied_hz : 0u,
            .target_tx_hz = status == 0u ? sState.tx_hz : 0u,
        };
        sendFrame(SAT_REPLY_BEGIN, &reply, sizeof(reply));
        break;
    }
    case SAT_CMD_UPDATE: {
        if (size != sizeof(SAT_Update_t))
            return;
        SAT_Update_t update;
        memcpy(&update, data, sizeof(update));
        const uint16_t status = updateSession(&update);
        if (status != SAT_ERR_NONE)
            sState.error_flags |= status;
        if ((update.flags & SAT_UPDATE_ACK_REQUEST) != 0u)
            sendUpdateReply();
        break;
    }
    case SAT_CMD_STATUS: {
        if (size != sizeof(SAT_Status_t))
            return;
        SAT_Status_t request;
        memcpy(&request, data, sizeof(request));
        if (sState.active && request.session_id != 0u && request.session_id != sState.session_id) {
            sendSimpleStatus(SAT_REPLY_STATUS, request.session_id, SAT_ERR_BAD_SESSION);
            return;
        }
        SAT_StatusReply_t reply = {
            .session_id = sState.session_id,
            .received_seq = sState.received_seq,
            .applied_seq = sState.applied_seq,
            .error_flags = sState.error_flags,
            .applied_hz = sState.applied_hz,
            .target_tx_hz = sState.tx_hz,
            .age_ms = (uint16_t)MIN((uint32_t)sState.age_ticks_10ms * 10u, UINT16_MAX),
            .update_rate_hz = sState.update_rate_hz,
            .link_state = SAT_GetLinkState(),
            .ptt = gCurrentFunction == FUNCTION_TRANSMIT,
            .ui_visible = SAT_IsUiVisible(),
        };
        sendFrame(SAT_REPLY_STATUS, &reply, sizeof(reply));
        break;
    }
    case SAT_CMD_END: {
        if (size != sizeof(SAT_End_t))
            return;
        SAT_End_t end;
        memcpy(&end, data, sizeof(end));
        uint16_t status = SAT_ERR_NONE;
        if (!sState.active || end.session_id != sState.session_id)
            status = SAT_ERR_BAD_SESSION;
        sendSimpleStatus(SAT_REPLY_END, end.session_id, status);
        if (status == SAT_ERR_NONE)
            requestEnd();
        break;
    }
    case SAT_CMD_UI_CONTROL: {
        if (size != sizeof(SAT_UiControl_t))
            return;
        SAT_UiControl_t control;
        memcpy(&control, data, sizeof(control));
        uint16_t status = SAT_ERR_NONE;
        if (!sState.active || control.session_id != sState.session_id) {
            status = SAT_ERR_BAD_SESSION;
        } else if (control.action == SAT_UI_SHOW) {
            SAT_SetUiVisible(true);
        } else if (control.action == SAT_UI_HIDE) {
            SAT_SetUiVisible(false);
        } else if (control.action == SAT_UI_TOGGLE) {
            SAT_SetUiVisible(!sState.ui_visible);
        } else {
            status = SAT_ERR_UNSUPPORTED;
        }
        sendSimpleStatus(SAT_REPLY_UI_CONTROL, control.session_id, status);
        break;
    }
    case SAT_CMD_PTT:
        sendSimpleStatus(SAT_REPLY_PTT, sState.session_id, SAT_ERR_UNSUPPORTED);
        break;
    default:
        break;
    }
}

static bool isSatelliteCommand(uint16_t id)
{
    return id >= SAT_CMD_HELLO && id <= SAT_CMD_PTT && (id & 1u) == 0u;
}

static bool parseOneFrame(uint16_t writeIndex)
{
    uint16_t available = ringAvailable(sVcpReadIndex, writeIndex);
    while (available >= 2u) {
        if (ringByte(sVcpReadIndex, 0u) == 0xABu && ringByte(sVcpReadIndex, 1u) == 0xCDu)
            break;
        sVcpReadIndex = ringAdvance(sVcpReadIndex, 1u);
        available--;
    }
    if (available < 4u)
        return false;

    const uint16_t innerSize = (uint16_t)ringByte(sVcpReadIndex, 2u) |
                               ((uint16_t)ringByte(sVcpReadIndex, 3u) << 8);
    if (innerSize < sizeof(SAT_InnerHeader_t) || innerSize > SAT_MAX_INNER_BYTES) {
        sVcpReadIndex = ringAdvance(sVcpReadIndex, 1u);
        return true;
    }

    const uint16_t wireSize = (uint16_t)(innerSize + 8u);
    if (available < wireSize)
        return false;
    if (ringByte(sVcpReadIndex, (uint16_t)(innerSize + 6u)) != 0xDCu ||
        ringByte(sVcpReadIndex, (uint16_t)(innerSize + 7u)) != 0xBAu) {
        sVcpReadIndex = ringAdvance(sVcpReadIndex, 1u);
        return true;
    }

    uint8_t decoded[SAT_MAX_INNER_BYTES + 2u] __attribute__((aligned(4)));
    for (uint16_t i = 0; i < innerSize + 2u; i++)
        decoded[i] = ringByte(sVcpReadIndex, (uint16_t)(4u + i)) ^ kObfuscation[i & 0x0Fu];

    SAT_InnerHeader_t inner;
    memcpy(&inner, decoded, sizeof(inner));
    const uint16_t receivedCrc = (uint16_t)decoded[innerSize] |
                                 ((uint16_t)decoded[innerSize + 1u] << 8);
    const bool crcOk = CRC_Calculate(decoded, innerSize) == receivedCrc;
    const bool satFrame = isSatelliteCommand(inner.id);

    if (satFrame) {
        if (!crcOk) {
            if (sState.crc_errors != UINT16_MAX)
                sState.crc_errors++;
        } else if (inner.size == innerSize - sizeof(inner)) {
            handleCommand(inner.id, decoded + sizeof(inner), inner.size);
        } else {
            sState.error_flags |= SAT_ERR_BAD_SIZE;
        }
        clearRingBytes(sVcpReadIndex, wireSize);
    }

    sVcpReadIndex = ringAdvance(sVcpReadIndex, wireSize);
    return true;
}

void SAT_USB_Poll(void)
{
#ifdef ENABLE_USB
    uint16_t writeIndex = (uint16_t)VCP_RxBufPointer;
    for (uint8_t i = 0; i < SAT_RX_BURST_LIMIT; i++) {
        if (!parseOneFrame(writeIndex))
            break;
        writeIndex = (uint16_t)VCP_RxBufPointer;
    }
#endif
}

void SAT_ApplyFastFrequency(void)
{
    if (!sState.active || sState.ending || SAT_GetLinkState() == SAT_LINK_LOST ||
        gCurrentFunction == FUNCTION_POWER_SAVE)
        return;

    const bool tx = gCurrentFunction == FUNCTION_TRANSMIT;
    const uint32_t target = tx ? sState.tx_hz : sState.rx_hz;
    if (!sRfDirty && target == sState.applied_hz) {
        sState.applied_seq = sState.received_seq;
        return;
    }

    BK4819_SetFrequency(hzTo10Hz(target));
    sState.applied_hz = target;
    sState.applied_seq = sState.received_seq;
    sRfDirty = false;
}

void SAT_TimeSlice10ms(void)
{
    if (!sState.active)
        return;

    if (sState.age_ticks_10ms != UINT16_MAX)
        sState.age_ticks_10ms++;

    if (++sRateTicks >= SAT_RATE_WINDOW_TICKS) {
        sRateTicks = 0u;
        sState.update_rate_hz = sUpdatesInWindow;
        sUpdatesInWindow = 0u;
    }

    if (sState.age_ticks_10ms > SAT_LOST_TICKS) {
        sState.error_flags |= SAT_ERR_LINK_TIMEOUT;
        sState.ending = true;
        if (GPIO_IsPttPressed() || gPttIsPressed || gCurrentFunction == FUNCTION_TRANSMIT) {
            sWaitingPttRelease = true;
            gSerialConfigCountDown_500ms = 2u;
            if (gCurrentFunction == FUNCTION_TRANSMIT && !gFlagEndTransmission)
                APP_EndTransmission();
        } else {
            restoreSession();
            return;
        }
    }

    if (sWaitingPttRelease) {
        gSerialConfigCountDown_500ms = 2u;
        if (!GPIO_IsPttPressed() && !gPttIsPressed && gCurrentFunction != FUNCTION_TRANSMIT) {
            restoreSession();
            return;
        }
    }

    if (sState.ui_visible && (sState.age_ticks_10ms % 20u) == 0u)
        gUpdateDisplay = true;
}

void __real_APP_Update(void);
void __wrap_APP_Update(void)
{
    SAT_USB_Poll();
    __real_APP_Update();
    SAT_ApplyFastFrequency();
}

void __real_APP_TimeSlice10ms(void);
void __wrap_APP_TimeSlice10ms(void)
{
    SAT_TimeSlice10ms();
    __real_APP_TimeSlice10ms();
}
