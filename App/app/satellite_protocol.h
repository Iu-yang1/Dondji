#ifndef APP_SATELLITE_PROTOCOL_H
#define APP_SATELLITE_PROTOCOL_H

#include <stdint.h>

#define SAT_PROTOCOL_MAJOR          1u
#define SAT_PROTOCOL_MINOR          1u
#define SAT_PROTOCOL_MAX_WIRE_BYTES 64u
#define SAT_PROTOCOL_MAX_UPDATE_HZ  100u
#define SAT_FREQUENCY_RESOLUTION_HZ 10u

#define SAT_CMD_HELLO        0x0700u
#define SAT_REPLY_HELLO      0x0701u
#define SAT_CMD_BEGIN        0x0702u
#define SAT_REPLY_BEGIN      0x0703u
#define SAT_CMD_UPDATE       0x0704u
#define SAT_REPLY_UPDATE     0x0705u
#define SAT_CMD_STATUS       0x0706u
#define SAT_REPLY_STATUS     0x0707u
#define SAT_CMD_END          0x0708u
#define SAT_REPLY_END        0x0709u
#define SAT_CMD_UI_CONTROL   0x070Au
#define SAT_REPLY_UI_CONTROL 0x070Bu
#define SAT_CMD_PTT          0x070Cu
#define SAT_REPLY_PTT        0x070Du

#define SAT_CAP_FREQ_PAIR    (1u << 0)
#define SAT_CAP_HIGH_RATE    (1u << 1)
#define SAT_CAP_TRACK_UI     (1u << 2)
#define SAT_CAP_CTCSS        (1u << 3)
#define SAT_CAP_PHYSICAL_PTT (1u << 4)
#define SAT_CAP_TELEMETRY    (1u << 5)

#define SAT_BEGIN_AUTO_SHOW    (1u << 0)
#define SAT_UPDATE_ACK_REQUEST (1u << 0)

#define SAT_UI_SHOW   1u
#define SAT_UI_HIDE   2u
#define SAT_UI_TOGGLE 3u

#define SAT_MOD_KEEP 0u
#define SAT_MOD_FM   1u

enum SAT_LinkState {
    SAT_LINK_INACTIVE = 0,
    SAT_LINK_FRESH,
    SAT_LINK_STALE,
    SAT_LINK_LOST,
};

enum SAT_ErrorFlags {
    SAT_ERR_NONE          = 0,
    SAT_ERR_BAD_VERSION   = (1u << 0),
    SAT_ERR_BAD_SESSION   = (1u << 1),
    SAT_ERR_BAD_SIZE      = (1u << 2),
    SAT_ERR_BAD_FREQUENCY = (1u << 3),
    SAT_ERR_BUSY          = (1u << 4),
    SAT_ERR_UNSUPPORTED   = (1u << 5),
    SAT_ERR_BAD_CTCSS     = (1u << 6),
    SAT_ERR_BAND_CHANGE   = (1u << 7),
    SAT_ERR_LINK_TIMEOUT  = (1u << 8),
};

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t size;
} SAT_InnerHeader_t;

typedef struct __attribute__((packed)) {
    uint8_t requested_major;
    uint8_t requested_minor;
    uint16_t host_flags;
} SAT_Hello_t;

typedef struct __attribute__((packed)) {
    uint8_t major;
    uint8_t minor;
    uint8_t max_update_hz;
    uint8_t frequency_resolution_hz;
    uint16_t max_wire_bytes;
    uint16_t capabilities;
} SAT_HelloReply_t;

typedef struct __attribute__((packed)) {
    uint8_t major;
    uint8_t minor;
    uint16_t session_id;
    char satellite[12];
    uint32_t aos_unix;
    uint32_t los_unix;
    uint32_t rx_base_hz;
    uint32_t tx_base_hz;
    uint16_t tx_ctcss_01hz;
    uint8_t modulation;
    uint8_t flags;
} SAT_Begin_t;

typedef struct __attribute__((packed)) {
    uint16_t session_id;
    uint16_t status;
    uint32_t applied_rx_hz;
    uint32_t target_tx_hz;
} SAT_BeginReply_t;

typedef struct __attribute__((packed)) {
    uint16_t session_id;
    uint16_t sequence;
    uint8_t flags;
    uint8_t reserved;
    uint32_t utc_unix;
    uint32_t rx_hz;
    uint32_t tx_hz;
    int32_t rx_doppler_hz;
    int32_t tx_doppler_hz;
    int16_t azimuth_01deg;
    int16_t elevation_01deg;
    uint16_t range_km;
} SAT_Update_t;

typedef struct __attribute__((packed)) {
    uint16_t session_id;
    uint16_t received_seq;
    uint16_t applied_seq;
    uint16_t error_flags;
    uint32_t applied_hz;
    uint32_t target_tx_hz;
    uint16_t age_ms;
    uint8_t ptt;
    uint8_t link_state;
} SAT_UpdateReply_t;

typedef struct __attribute__((packed)) {
    uint16_t session_id;
    uint16_t reserved;
} SAT_Status_t;

typedef struct __attribute__((packed)) {
    uint16_t session_id;
    uint16_t received_seq;
    uint16_t applied_seq;
    uint16_t error_flags;
    uint32_t applied_hz;
    uint32_t target_tx_hz;
    uint16_t age_ms;
    uint16_t update_rate_hz;
    uint16_t crc_errors;
    uint16_t dropped_updates;
    uint8_t link_state;
    uint8_t ptt;
    uint8_t ui_visible;
    uint8_t reserved;
} SAT_StatusReply_t;

typedef struct __attribute__((packed)) {
    uint16_t session_id;
    uint8_t reason;
    uint8_t flags;
} SAT_End_t;

typedef struct __attribute__((packed)) {
    uint16_t session_id;
    uint16_t status;
} SAT_EndReply_t;

typedef struct __attribute__((packed)) {
    uint16_t session_id;
    uint8_t action;
    uint8_t reserved;
} SAT_UiControl_t;

_Static_assert(sizeof(SAT_Update_t) + sizeof(SAT_InnerHeader_t) + 8u <= SAT_PROTOCOL_MAX_WIRE_BYTES,
               "SAT_UPDATE must fit in one USB FS packet");
_Static_assert(sizeof(SAT_StatusReply_t) + sizeof(SAT_InnerHeader_t) + 8u <= SAT_PROTOCOL_MAX_WIRE_BYTES,
               "SAT_STATUS reply must fit in one USB FS packet");

#endif
