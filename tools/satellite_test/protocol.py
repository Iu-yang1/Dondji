from __future__ import annotations

import struct
from dataclasses import dataclass


APP_NAME = "Dondji Satellite USB Tester"
APP_VERSION = "1.1.0"

VID = 0x36B7
PID = 0xFFFF
BAUDRATE = 115200

PROTO_MAJOR = 1
PROTO_MINOR = 2
MAX_WIRE_BYTES = 64

CMD_HELLO = 0x0700
RPL_HELLO = 0x0701
CMD_BEGIN = 0x0702
RPL_BEGIN = 0x0703
CMD_UPDATE = 0x0704
RPL_UPDATE = 0x0705
CMD_STATUS = 0x0706
RPL_STATUS = 0x0707
CMD_END = 0x0708
RPL_END = 0x0709
CMD_UI = 0x070A
RPL_UI = 0x070B
CMD_PTT = 0x070C
RPL_PTT = 0x070D

SAT_BEGIN_AUTO_SHOW = 1 << 0
SAT_UPDATE_ACK_REQUEST = 1 << 0
SAT_UI_SHOW = 1
SAT_UI_HIDE = 2
SAT_UI_TOGGLE = 3
SAT_MOD_KEEP = 0
SAT_MOD_FM = 1

STATUS_BASE_FMT = "<HHHHIIHHHHBBBB"
STATUS_DIAG_FMT = "<BBB3s3s3sHHHHHH"
STATUS_BASE_SIZE = struct.calcsize(STATUS_BASE_FMT)
STATUS_DIAG_SIZE = struct.calcsize(STATUS_DIAG_FMT)

CAP_NAMES = {
    1 << 0: "Frequency pair",
    1 << 1: "High-rate stream",
    1 << 2: "Tracking UI",
    1 << 3: "CTCSS",
    1 << 4: "Physical PTT",
    1 << 5: "Telemetry",
    1 << 6: "TX diagnostics",
}

POWER_NAMES = {
    0: "USER",
    1: "LOW1",
    2: "LOW2",
    3: "LOW3",
    4: "LOW4",
    5: "LOW5",
    6: "MID",
    7: "HIGH",
}

LINK_NAMES = {
    0: "INACTIVE",
    1: "FRESH",
    2: "STALE",
    3: "LOST",
}

ERROR_NAMES = {
    1 << 0: "BAD_VERSION",
    1 << 1: "BAD_SESSION",
    1 << 2: "BAD_SIZE",
    1 << 3: "BAD_FREQUENCY",
    1 << 4: "BUSY",
    1 << 5: "UNSUPPORTED",
    1 << 6: "BAD_CTCSS",
    1 << 7: "BAND_CHANGE",
    1 << 8: "LINK_TIMEOUT",
}

OBFUSCATION = bytes(
    [0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40,
     0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80]
)


class ProtocolError(Exception):
    pass


def crc_ccitt_zero(data: bytes) -> int:
    crc = 0
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def xor_mask(data: bytes) -> bytes:
    return bytes(value ^ OBFUSCATION[i & 0x0F] for i, value in enumerate(data))


def build_frame(command_id: int, payload: bytes = b"") -> bytes:
    inner = struct.pack("<HH", command_id, len(payload)) + payload
    crc = struct.pack("<H", crc_ccitt_zero(inner))
    body = xor_mask(inner + crc)
    frame = b"\xAB\xCD" + struct.pack("<H", len(inner)) + body + b"\xDC\xBA"
    if len(frame) > MAX_WIRE_BYTES:
        raise ProtocolError(f"Wire frame too large: {len(frame)} > {MAX_WIRE_BYTES}")
    return frame


@dataclass
class ParsedFrame:
    command_id: int
    payload: bytes


@dataclass(frozen=True)
class StreamConfig:
    session_id: int
    base_rx_hz: int
    base_tx_hz: int
    rx_amp_hz: int
    tx_amp_hz: int
    period_s: float
    ack_every: int


class FrameParser:
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.host_crc_errors = 0
        self.host_bad_frames = 0

    def feed(self, data: bytes) -> list[ParsedFrame]:
        self.buffer.extend(data)
        frames: list[ParsedFrame] = []

        while True:
            start = self.buffer.find(b"\xAB\xCD")
            if start < 0:
                if len(self.buffer) > 1:
                    del self.buffer[:-1]
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < 4:
                break

            inner_size = struct.unpack_from("<H", self.buffer, 2)[0]
            if inner_size < 4 or inner_size > (MAX_WIRE_BYTES - 8):
                self.host_bad_frames += 1
                del self.buffer[0]
                continue

            wire_size = inner_size + 8
            if len(self.buffer) < wire_size:
                break
            if self.buffer[wire_size - 2:wire_size] != b"\xDC\xBA":
                self.host_bad_frames += 1
                del self.buffer[0]
                continue

            encoded = bytes(self.buffer[4:4 + inner_size + 2])
            decoded = xor_mask(encoded)
            inner = decoded[:inner_size]
            recv_crc = struct.unpack_from("<H", decoded, inner_size)[0]
            if crc_ccitt_zero(inner) != recv_crc:
                self.host_crc_errors += 1
                del self.buffer[:wire_size]
                continue

            command_id, data_size = struct.unpack_from("<HH", inner, 0)
            payload = inner[4:]
            if data_size != len(payload):
                self.host_bad_frames += 1
                del self.buffer[:wire_size]
                continue

            frames.append(ParsedFrame(command_id, payload))
            del self.buffer[:wire_size]

        return frames


def decode_errors(flags: int) -> str:
    if flags == 0:
        return "NONE"
    names = [name for bit, name in ERROR_NAMES.items() if flags & bit]
    unknown = flags & ~sum(ERROR_NAMES.keys())
    if unknown:
        names.append(f"0x{unknown:04X}")
    return " | ".join(names)


def format_hz(hz: int) -> str:
    return f"{hz / 1_000_000:.5f} MHz"