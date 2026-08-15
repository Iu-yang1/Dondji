/*
 * CN-RF 高级射频配置。
 *
 * 配置使用独立、带版本和 CRC 的外置 Flash 区，绝不改变原 16 字节信道记录。
 */
#ifndef RF_PROFILE_H
#define RF_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RF_BW_W23 = 0,
    RF_BW_W20,
    RF_BW_W17,
    RF_BW_W14,
    RF_BW_W12,
    RF_BW_N10,
    RF_BW_N9,
    RF_BW_U7,
    RF_BW_U6,
    /* 追加而不是插入，保持 v1 外置 Flash 中已有 0..8 编号的含义不变。 */
    RF_BW_W26,
    RF_BW_COUNT
} RF_Bandwidth_t;

typedef enum {
    RF_AGC_AUTO = 0,
    RF_AGC_MAN,
    RF_AGC_FAST,
    RF_AGC_NORM,
    RF_AGC_SLOW,
    RF_AGC_COUNT
} RF_AgcMode_t;

typedef struct {
    uint8_t bandwidth;
    uint8_t agc;
    uint8_t rfGain;
    uint8_t afc;
    uint8_t micGain;
    uint8_t deviation;
    uint8_t rfBoost;
    uint8_t noiseBlanker;
} RF_Profile_t;

struct VFO_Info_t;

extern const char *const gRfBandwidthNames[RF_BW_COUNT];
extern const uint8_t gRfBandwidthMenuValues[RF_BW_COUNT];
extern const char *const gRfAgcNames[RF_AGC_COUNT];

void RF_PROFILE_SetDefaults(RF_Profile_t *profile, uint8_t legacyBandwidth, uint8_t micGain);
void RF_PROFILE_Load(uint16_t channel, uint8_t vfo, RF_Profile_t *profile,
                     uint8_t legacyBandwidth, uint8_t micGain);
void RF_PROFILE_Save(uint16_t channel, uint8_t vfo, const RF_Profile_t *profile);
void RF_PROFILE_ResetAll(void);
void RF_PROFILE_SetModeDefault(struct VFO_Info_t *vfo);
void RF_PROFILE_ApplyRx(const struct VFO_Info_t *vfo);
void RF_PROFILE_ApplyTx(const struct VFO_Info_t *vfo);
void RF_PROFILE_TimeSlice10ms(void);
bool RF_PROFILE_AgcUsesRfGain(uint8_t agc);
bool RF_PROFILE_IsWideBandwidth(uint8_t bandwidth);
uint8_t RF_PROFILE_BandwidthToMenu(uint8_t bandwidth);

#endif
