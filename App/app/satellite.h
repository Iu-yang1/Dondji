#ifndef APP_SATELLITE_H
#define APP_SATELLITE_H

#include <stdbool.h>
#include <stdint.h>

#include "app/satellite_protocol.h"

typedef struct {
    bool active;
    bool ui_visible;
    bool ending;
    bool sequence_valid;
    uint8_t rx_band;
    uint8_t tx_band;
    uint8_t update_rate_hz;
    uint16_t session_id;
    uint16_t received_seq;
    uint16_t applied_seq;
    uint16_t error_flags;
    uint16_t crc_errors;
    uint16_t dropped_updates;
    uint16_t age_ticks_10ms;
    uint32_t aos_unix;
    uint32_t los_unix;
    uint32_t utc_unix;
    uint32_t rx_hz;
    uint32_t tx_hz;
    uint32_t applied_hz;
    int32_t rx_doppler_hz;
    int32_t tx_doppler_hz;
    int16_t azimuth_01deg;
    int16_t elevation_01deg;
    uint16_t range_km;
    char satellite[13];
} SAT_State_t;

const SAT_State_t *SAT_GetState(void);
uint8_t SAT_GetLinkState(void);
bool SAT_IsUiVisible(void);
void SAT_SetUiVisible(bool visible);

void SAT_USB_Poll(void);
void SAT_ApplyFastFrequency(void);
void SAT_TimeSlice10ms(void);

#endif
