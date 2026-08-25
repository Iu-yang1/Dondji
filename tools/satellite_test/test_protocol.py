import struct
import unittest

import protocol


class ProtocolTests(unittest.TestCase):
    def test_frame_round_trip(self):
        payload = struct.pack("<BBH", 1, protocol.PROTO_MINOR, 0)
        frame = protocol.build_frame(protocol.CMD_HELLO, payload)
        parsed = protocol.FrameParser().feed(frame)
        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[0].command_id, protocol.CMD_HELLO)
        self.assertEqual(parsed[0].payload, payload)

    def test_wire_sizes(self):
        begin = struct.pack(
            "<BBH12sIIIIHBB", 1, protocol.PROTO_MINOR, 0x1234, b"TEST".ljust(12, b"\0"),
            1, 2, 435650000, 145950000, 0, 1, 1,
        )
        update = struct.pack(
            "<HHBBIIIiihhH", 0x1234, 1, 0, 0, 1,
            435650000, 145950000, -1000, 500, 1234, 450, 1200,
        )
        self.assertEqual(len(protocol.build_frame(protocol.CMD_BEGIN, begin)), 48)
        self.assertEqual(len(protocol.build_frame(protocol.CMD_UPDATE, update)), 44)
        self.assertEqual(protocol.STATUS_BASE_SIZE, 28)
        self.assertEqual(protocol.STATUS_DIAG_SIZE, 24)
        self.assertEqual(protocol.STATUS_BASE_SIZE + protocol.STATUS_DIAG_SIZE + 12, 64)

    def test_status_v12_frame_round_trip(self):
        payload = bytes(protocol.STATUS_BASE_SIZE + protocol.STATUS_DIAG_SIZE)
        frame = protocol.build_frame(protocol.RPL_STATUS, payload)
        self.assertEqual(len(frame), 64)
        parsed = protocol.FrameParser().feed(frame)
        self.assertEqual(parsed[0].payload, payload)

    def test_crc_reference_vector(self):
        self.assertEqual(protocol.crc_ccitt_zero(bytes.fromhex("0007040001010000")), 0x5E61)


if __name__ == "__main__":
    unittest.main()