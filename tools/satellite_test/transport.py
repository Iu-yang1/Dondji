from __future__ import annotations

import queue
import threading

import serial

from protocol import BAUDRATE, FrameParser, build_frame


class SerialTransport:
    def __init__(self, event_queue: queue.Queue) -> None:
        self.event_queue = event_queue
        self.ser: serial.Serial | None = None
        self.parser = FrameParser()
        self.reader_thread: threading.Thread | None = None
        self.stop_event = threading.Event()
        self.write_lock = threading.Lock()

    @property
    def connected(self) -> bool:
        return bool(self.ser and self.ser.is_open)

    def connect(self, port: str) -> None:
        self.disconnect()
        ser = serial.Serial(
            port=port,
            baudrate=BAUDRATE,
            timeout=0.05,
            write_timeout=0.5,
            exclusive=None,
        )
        ser.dtr = True
        ser.rts = False
        self.ser = ser
        self.stop_event.clear()
        self.parser = FrameParser()
        self.reader_thread = threading.Thread(target=self._reader, name="serial-reader", daemon=True)
        self.reader_thread.start()

    def disconnect(self) -> None:
        self.stop_event.set()
        ser, self.ser = self.ser, None
        if ser:
            try:
                ser.close()
            except serial.SerialException:
                pass

    def send(self, command_id: int, payload: bytes = b"") -> None:
        ser = self.ser
        if not ser or not ser.is_open:
            raise serial.SerialException("Serial port is not connected")
        frame = build_frame(command_id, payload)
        with self.write_lock:
            ser.write(frame)

    def _reader(self) -> None:
        try:
            while not self.stop_event.is_set():
                ser = self.ser
                if not ser or not ser.is_open:
                    return
                data = ser.read(256)
                if not data:
                    continue
                for frame in self.parser.feed(data):
                    self.event_queue.put(("frame", frame))
                self.event_queue.put(("host_parser", (self.parser.host_crc_errors, self.parser.host_bad_frames)))
        except Exception as exc:  # noqa: BLE001 - UI must surface transport failures
            self.event_queue.put(("transport_error", str(exc)))
