#include "rf_profile.h"

#include <string.h>

#include "driver/bk4819.h"
#include "driver/py25q16.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"

#define RF_PROFILE_FLASH_BASE       0x060000u
#define RF_PROFILE_FLASH_END        0x062000u
#define RF_PROFILE_HEADER_SIZE      16u
#define RF_PROFILE_RECORD_SIZE      6u
#define RF_PROFILE_RECORD_COUNT     (MR_CHANNELS_MAX + (BAND_N_ELEM * 2u))
#define RF_PROFILE_VERSION          1u

_Static_assert(RF_PROFILE_FLASH_BASE + RF_PROFILE_HEADER_SIZE +
               (RF_PROFILE_RECORD_COUNT * RF_PROFILE_RECORD_SIZE) <= RF_PROFILE_FLASH_END,
               "CN-RF 配置超出已审计的外置 Flash 区");

const char *const gRfBandwidthNames[RF_BW_COUNT] = {
    "W23", "W20", "W17", "W14", "W12", "N10", "N9", "U7", "U6", "W26"
};

/* 菜单按带宽从宽到窄显示，但存储枚举不能重新编号。 */
const uint8_t gRfBandwidthMenuValues[RF_BW_COUNT] = {
    RF_BW_W26, RF_BW_W23, RF_BW_W20, RF_BW_W17, RF_BW_W14,
    RF_BW_W12, RF_BW_N10, RF_BW_N9, RF_BW_U7, RF_BW_U6
};

const char *const gRfAgcNames[RF_AGC_COUNT] = {
    "AUTO", "MAN", "FAST", "NORM", "SLOW"
};

/* REG_43 的字段含义来自公开 BK4819 编程表：
 * https://alfaexploit.com/files/BK4819V3Registers_List_20201218.pdf
 * 九组值只组合该文档已定义的 RF/弱信号 RF/AF LPF/BW Mode 字段；名称和
 * 菜单行为参考：
 * https://www.ijvradio.com/manual/pages/bw.html
 * 再与 BK4829 当前驱动的同一寄存器逐字段交叉校验。名称是配置档标识，
 * 不是未经测量的占用带宽保证；最终带宽仍须仪表验证。 */
static const uint16_t kBandwidthReg43[RF_BW_COUNT] = {
    0x5968, 0x47A8, 0x35E8, 0x2428, 0x1228, 0x34C8, 0x1208, 0x1098, 0x0058,
    /* RF/弱信号 RF 滤波器均取公开字段允许的最宽档，25/20 kHz 模式。 */
    0x7F28
};

_Static_assert(ARRAY_SIZE(gRfBandwidthNames) == RF_BW_COUNT, "带宽名称数量错误");
_Static_assert(ARRAY_SIZE(gRfBandwidthMenuValues) == RF_BW_COUNT, "带宽菜单数量错误");
_Static_assert(ARRAY_SIZE(kBandwidthReg43) == RF_BW_COUNT, "带宽寄存器数量错误");

/* REG_13: short-LNA[9:8]、LNA[7:5]、Mixer[4:3]、PGA[2:0]。
 * 表项来自当前 BK4829 AM 增益表的已验证组合，按总增益单调排列。 */
static const uint16_t kGainReg13[16] = {
    0x0000, 0x0011, 0x0003, 0x000C, 0x001C, 0x001E, 0x003E, 0x005E,
    0x007E, 0x009F, 0x00FF, 0x01FF, 0x02FF, 0x035F, 0x037F, 0x03FF
};

/* REG_40[11:0] 是公开文档定义的 FM deviation 控制字。0 表示保持 Dondji
 * 原标准值；1..9 是保守的工程调节字，均低于芯片字段上限。控制字不是
 * 物理 Hz，发射占用带宽必须用频偏仪/频谱仪确认。 */
static const uint16_t kDeviationReg40[10] = {
    0, 500, 600, 700, 750, 800, 850, 900, 1000, 1100
};

static uint8_t gAgcGainIndex[2] = {15, 15};
static uint8_t gAgcTicks;
static uint8_t gNoiseBlankTicks;

static uint8_t crc8(const uint8_t *data, uint16_t size)
{
    uint8_t crc = 0;
    while (size-- != 0) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++)
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
    }
    return crc;
}

static void makeHeader(uint8_t header[RF_PROFILE_HEADER_SIZE])
{
    memset(header, 0, RF_PROFILE_HEADER_SIZE);
    header[0] = 'C'; header[1] = 'N'; header[2] = 'R'; header[3] = 'F';
    header[4] = RF_PROFILE_VERSION;
    header[5] = RF_PROFILE_RECORD_SIZE;
    header[6] = (uint8_t)RF_PROFILE_RECORD_COUNT;
    header[7] = (uint8_t)(RF_PROFILE_RECORD_COUNT >> 8);
    header[15] = crc8(header, 15);
}

static bool layoutValid(void)
{
    uint8_t actual[RF_PROFILE_HEADER_SIZE];
    uint8_t expected[RF_PROFILE_HEADER_SIZE];
    PY25Q16_ReadBuffer(RF_PROFILE_FLASH_BASE, actual, sizeof(actual));
    makeHeader(expected);
    return memcmp(actual, expected, sizeof(actual)) == 0;
}

static bool layoutBlank(void)
{
    uint8_t header[RF_PROFILE_HEADER_SIZE];
    PY25Q16_ReadBuffer(RF_PROFILE_FLASH_BASE, header, sizeof(header));
    for (uint8_t i = 0; i < sizeof(header); i++) {
        if (header[i] != 0xFFu)
            return false;
    }
    return true;
}

static bool recordIndex(uint16_t channel, uint8_t vfo, uint16_t *index)
{
    if (IS_MR_CHANNEL(channel)) {
        *index = channel;
        return true;
    }
    if (IS_FREQ_CHANNEL(channel) && vfo < 2u) {
        *index = MR_CHANNELS_MAX + ((channel - FREQ_CHANNEL_FIRST) * 2u) + vfo;
        return true;
    }
    return false;
}

static bool profileValid(const RF_Profile_t *p)
{
    return p->bandwidth < RF_BW_COUNT && p->agc < RF_AGC_COUNT && p->rfGain < 16u &&
           p->afc < 9u && p->micGain < 9u && p->deviation < 10u && p->rfBoost < 2u &&
           p->noiseBlanker < 4u;
}

static void packRecord(const RF_Profile_t *p, uint8_t data[RF_PROFILE_RECORD_SIZE])
{
    data[0] = (p->bandwidth & 0x0Fu) | ((p->agc & 0x07u) << 4);
    data[1] = (p->rfGain & 0x0Fu) | ((p->rfBoost & 1u) << 5) |
              ((p->noiseBlanker & 3u) << 6);
    data[2] = p->micGain;
    data[3] = p->deviation;
    data[4] = p->afc;
    data[5] = crc8(data, 5);
}

static bool unpackRecord(const uint8_t data[RF_PROFILE_RECORD_SIZE], RF_Profile_t *p)
{
    if (data[5] != crc8(data, 5))
        return false;
    p->bandwidth = data[0] & 0x0Fu;
    p->agc = (data[0] >> 4) & 0x07u;
    p->rfGain = data[1] & 0x0Fu;
    p->afc = data[4];
    p->rfBoost = (data[1] >> 5) & 1u;
    p->noiseBlanker = (data[1] >> 6) & 3u;
    p->micGain = data[2];
    p->deviation = data[3];
    return profileValid(p);
}

void RF_PROFILE_SetDefaults(RF_Profile_t *p, uint8_t legacyBandwidth, uint8_t micGain)
{
    p->bandwidth = legacyBandwidth == BANDWIDTH_WIDE ? RF_BW_W23 : RF_BW_N9;
    p->agc = RF_AGC_AUTO;
    p->rfGain = 15;
    p->afc = 1;
    p->micGain = micGain < 9u ? micGain : 4u;
    p->deviation = 0;
    p->rfBoost = 0;
    p->noiseBlanker = 0;
}

void RF_PROFILE_SetModeDefault(VFO_Info_t *vfo)
{
    uint8_t bandwidth;

    /* 仅在用户主动切换模式时调用；不干预之后由带宽菜单作出的选择。 */
    switch (vfo->Modulation) {
    case MODULATION_USB:
    case MODULATION_LSB:
    case MODULATION_DSB:
        bandwidth = RF_BW_N9;
        break;
    case MODULATION_CW:
        bandwidth = RF_BW_U6;
        break;
    case MODULATION_AM:
        bandwidth = RF_BW_W12;
        break;
    default:
        return;
    }

    vfo->RfProfile.bandwidth = bandwidth;
    vfo->CHANNEL_BANDWIDTH = RF_PROFILE_IsWideBandwidth(bandwidth) ? BANDWIDTH_WIDE : BANDWIDTH_NARROW;
}

void RF_PROFILE_ResetAll(void)
{
    PY25Q16_SectorErase(RF_PROFILE_FLASH_BASE);
    PY25Q16_SectorErase(RF_PROFILE_FLASH_BASE + 0x1000u);
}

void RF_PROFILE_Load(uint16_t channel, uint8_t vfo, RF_Profile_t *p,
                     uint8_t legacyBandwidth, uint8_t micGain)
{
    uint16_t index;
    uint8_t data[RF_PROFILE_RECORD_SIZE];
    RF_PROFILE_SetDefaults(p, legacyBandwidth, micGain);
    if (!recordIndex(channel, vfo, &index) || !layoutValid())
        return;
    PY25Q16_ReadBuffer(RF_PROFILE_FLASH_BASE + RF_PROFILE_HEADER_SIZE +
                       ((uint32_t)index * RF_PROFILE_RECORD_SIZE), data, sizeof(data));
    (void)unpackRecord(data, p);
}

void RF_PROFILE_Save(uint16_t channel, uint8_t vfo, const RF_Profile_t *p)
{
    uint16_t index;
    uint8_t data[RF_PROFILE_RECORD_SIZE];
    uint8_t old[RF_PROFILE_RECORD_SIZE];
    uint8_t header[RF_PROFILE_HEADER_SIZE];
    if (!profileValid(p) || !recordIndex(channel, vfo, &index))
        return;
    if (!layoutValid()) {
        /* 未知版本一律拒写，避免降级固件破坏未来版本；仅在空白区写入 v1 头。 */
        if (!layoutBlank())
            return;
        makeHeader(header);
        PY25Q16_WriteBuffer(RF_PROFILE_FLASH_BASE, header, sizeof(header));
    }
    packRecord(p, data);
    const uint32_t address = RF_PROFILE_FLASH_BASE + RF_PROFILE_HEADER_SIZE +
                             ((uint32_t)index * RF_PROFILE_RECORD_SIZE);
    if (address + sizeof(data) > RF_PROFILE_FLASH_END)
        return;
    PY25Q16_ReadBuffer(address, old, sizeof(old));
    if (memcmp(old, data, sizeof(data)) != 0)
        PY25Q16_WriteBuffer(address, data, sizeof(data));
}

bool RF_PROFILE_AgcUsesRfGain(uint8_t agc)
{
    return agc != RF_AGC_AUTO && agc < RF_AGC_COUNT;
}

bool RF_PROFILE_IsWideBandwidth(uint8_t bandwidth)
{
    return bandwidth <= RF_BW_W12 || bandwidth == RF_BW_W26;
}

uint8_t RF_PROFILE_BandwidthToMenu(uint8_t bandwidth)
{
    for (uint8_t i = 0; i < RF_BW_COUNT; i++) {
        if (gRfBandwidthMenuValues[i] == bandwidth)
            return i;
    }
    return 1u; /* W23：损坏/未知值的保守显示回退。 */
}

void RF_PROFILE_ApplyRx(const VFO_Info_t *vfo)
{
    const RF_Profile_t *p = &vfo->RfProfile;
    const uint8_t vfoIndex = vfo == &gEeprom.VfoInfo[1] ? 1u : 0u;
    BK4819_SetFilterBandwidthRaw(kBandwidthReg43[p->bandwidth]);
    BK4819_SetAfcLevel(p->afc);
    if (p->agc == RF_AGC_AUTO) {
        BK4819_SetAGC(true);
    } else {
        uint8_t index = p->rfGain + p->rfBoost;
        if (index > 15u) index = 15u;
        gAgcGainIndex[vfoIndex] = index;
        BK4819_SetFixedRxGain(kGainReg13[index]);
    }
}

void RF_PROFILE_ApplyTx(const VFO_Info_t *vfo)
{
    const RF_Profile_t *p = &vfo->RfProfile;
    BK4819_SetMicGain(gMicGain_dB2[p->micGain]);
    if (p->deviation != 0u)
        BK4819_SetTxDeviation(kDeviationReg40[p->deviation]);
    BK4819_SetFilterBandwidthRaw(kBandwidthReg43[p->bandwidth]);
}

void RF_PROFILE_TimeSlice10ms(void)
{
    const RF_Profile_t *p;
    uint8_t interval;
    uint8_t vfoIndex;
    if (gCurrentFunction == FUNCTION_TRANSMIT || gCurrentFunction == FUNCTION_POWER_SAVE || gRxIdleMode)
        return;
#ifdef ENABLE_WFM
    if (gRxVfo->Modulation == MODULATION_WFM)
        return;
#endif
    p = &gRxVfo->RfProfile;

    /* 软件 Noise Blanker 仅使用已定义的 REG_63 glitch 指示器，不猜测未知控制位。 */
    if (gNoiseBlankTicks != 0u) {
        if (--gNoiseBlankTicks == 0u && !gMute && gEnableSpeaker) {
            BK4819_AF_Type_t af = BK4819_AF_FM;
            if (gRxVfo->Modulation == MODULATION_LSB)
                af = BK4819_AF_LSB;
            else if (gRxVfo->Modulation == MODULATION_USB ||
                     gRxVfo->Modulation == MODULATION_DSB || gRxVfo->Modulation == MODULATION_CW)
                af = BK4819_AF_USB;
#ifdef ENABLE_BYP_RAW_DEMODULATORS
            else if (gRxVfo->Modulation == MODULATION_BYP)
                af = BK4819_AF_UNKNOWN3;
#endif
            BK4819_SetAF(af);
        }
    } else if (p->noiseBlanker != 0u && gEnableSpeaker && !gMute) {
        static const uint8_t threshold[4] = {255u, 180u, 140u, 100u};
        if (BK4819_GetGlitchIndicator() >= threshold[p->noiseBlanker]) {
            BK4819_SetAF(BK4819_AF_MUTE);
            gNoiseBlankTicks = 2u;
        }
    }
    if (p->agc < RF_AGC_FAST || p->agc > RF_AGC_SLOW)
        return;
    interval = p->agc == RF_AGC_FAST ? 2u : (p->agc == RF_AGC_NORM ? 8u : 25u);
    if (++gAgcTicks < interval)
        return;
    gAgcTicks = 0;
    vfoIndex = gRxVfo == &gEeprom.VfoInfo[1] ? 1u : 0u;
    uint8_t ceiling = p->rfGain + p->rfBoost;
    if (ceiling > 15u) ceiling = 15u;
    const int16_t rssi = BK4819_GetRSSI_dBm();
    if (rssi > -65 && gAgcGainIndex[vfoIndex] > 0u)
        gAgcGainIndex[vfoIndex]--;
    else if (rssi < -90 && gAgcGainIndex[vfoIndex] < ceiling)
        gAgcGainIndex[vfoIndex]++;
    BK4819_SetFixedRxGain(kGainReg13[gAgcGainIndex[vfoIndex]]);
}
