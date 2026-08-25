from __future__ import annotations

import math
import struct
import threading
import time
from datetime import datetime
from tkinter import messagebox

from serial.tools import list_ports

from protocol import (
    APP_NAME, BAUDRATE, CMD_BEGIN, CMD_END, CMD_HELLO, CMD_STATUS, CMD_UI, CMD_UPDATE,
    PID, PROTO_MAJOR, PROTO_MINOR, SAT_BEGIN_AUTO_SHOW, SAT_MOD_FM, SAT_MOD_KEEP,
    SAT_UPDATE_ACK_REQUEST, StreamConfig, VID,
)


class SessionActionsMixin:
    def log(self, message: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        line = f"[{stamp}] {message}"
        self.log_lines.append(line)
        self.log_text.insert("end", line + "\n")
        self.log_text.see("end")

    def require_connection(self) -> bool:
        if not self.transport.connected:
            messagebox.showwarning(APP_NAME, "请先连接 UV-K1 的 USB CDC COM 端口。")
            return False
        return True

    def refresh_ports(self) -> None:
        ports = list(list_ports.comports())
        display = []
        preferred_index = None
        for p in ports:
            marker = "  [Dondji VID/PID]" if p.vid == VID and p.pid == PID else ""
            display.append(f"{p.device} — {p.description}{marker}")
            if marker and preferred_index is None:
                preferred_index = len(display) - 1
        self.port_combo["values"] = display
        if display:
            self.port_combo.current(preferred_index if preferred_index is not None else 0)
        else:
            self.port_var.set("")
        self.device_info_var.set(f"发现 {len(ports)} 个串口；目标 USB: VID {VID:04X} / PID {PID:04X}")

    def _selected_port(self) -> str:
        value = self.port_var.get().strip()
        if not value:
            return ""
        return value.split(" — ", 1)[0]

    def toggle_connection(self) -> None:
        if self.transport.connected:
            self.stop_all_workers()
            self.transport.disconnect()
            self.connect_btn.configure(text="连接")
            self.connection_label.configure(text="未连接", style="Bad.TLabel")
            self.log("Serial disconnected")
            return
        port = self._selected_port()
        if not port:
            messagebox.showwarning(APP_NAME, "没有可用 COM 端口。")
            return
        try:
            self.transport.connect(port)
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror(APP_NAME, f"打开 {port} 失败：\n{exc}")
            return
        self.connect_btn.configure(text="断开")
        self.connection_label.configure(text=f"已连接 {port}", style="Good.TLabel")
        self.log(f"Serial connected: {port} @ API baud {BAUDRATE}, DTR=1")

    def send_cmd(self, command_id: int, payload: bytes = b"", label: str | None = None) -> bool:
        if not self.require_connection():
            return False
        try:
            self.transport.send(command_id, payload)
            if label:
                self.log(f"TX {label} (0x{command_id:04X}, {len(payload)} B payload)")
            return True
        except Exception as exc:  # noqa: BLE001
            self.log(f"TX error: {exc}")
            return False

    def send_hello(self) -> None:
        payload = struct.pack("<BBH", PROTO_MAJOR, PROTO_MINOR, 0)
        self.send_cmd(CMD_HELLO, payload, "SAT_HELLO")

    def _session_id(self) -> int:
        value = int(self.session_var.get(), 0)
        if not 1 <= value <= 0xFFFF:
            raise ValueError("Session ID 必须为 1..65535")
        return value

    def _base_freqs_hz(self) -> tuple[int, int]:
        rx = round(float(self.rx_mhz_var.get()) * 1_000_000)
        tx = round(float(self.tx_mhz_var.get()) * 1_000_000)
        if not (1 <= rx <= 0xFFFFFFFF and 1 <= tx <= 0xFFFFFFFF):
            raise ValueError("RX/TX 频率超出 uint32 Hz 范围")
        return rx, tx

    def send_begin(self) -> None:
        if not self.require_connection():
            return
        try:
            session_id = self._session_id()
            rx_hz, tx_hz = self._base_freqs_hz()
            name_raw = self.satellite_var.get().encode("ascii", errors="replace")[:12]
            name = name_raw.ljust(12, b"\0")
            now = int(time.time())
            aos = now
            los = now + 15 * 60
            ctcss_text = self.ctcss_var.get().strip()
            ctcss = 0 if not ctcss_text or float(ctcss_text) == 0 else round(float(ctcss_text) * 10)
            modulation = SAT_MOD_FM if self.mod_var.get() == "FM" else SAT_MOD_KEEP
            flags = SAT_BEGIN_AUTO_SHOW if self.auto_show_var.get() else 0
            payload = struct.pack(
                "<BBH12sIIIIHBB",
                PROTO_MAJOR, PROTO_MINOR, session_id, name,
                aos, los, rx_hz, tx_hz, ctcss, modulation, flags,
            )
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror(APP_NAME, f"BEGIN 参数错误：\n{exc}")
            return
        self.sequence = 0
        self.send_cmd(CMD_BEGIN, payload, "SAT_BEGIN")

    def send_status(self, quiet: bool = False) -> None:
        if not self.transport.connected:
            return
        try:
            session = self.active_session_id if self.session_active else 0
            payload = struct.pack("<HH", session, 0)
            self.transport.send(CMD_STATUS, payload)
            if not quiet:
                self.log(f"TX SAT_STATUS session={session}")
        except Exception as exc:  # noqa: BLE001
            if not quiet:
                self.log(f"STATUS error: {exc}")

    def send_end(self) -> None:
        if not self.require_connection():
            return
        try:
            session = self._session_id()
        except ValueError as exc:
            messagebox.showerror(APP_NAME, str(exc))
            return
        payload = struct.pack("<HBB", session, 0, 0)
        if self.send_cmd(CMD_END, payload, "SAT_END"):
            self.stream_stop.set()

    def send_ui(self, action: int) -> None:
        if not self.require_connection():
            return
        try:
            session = self._session_id()
        except ValueError as exc:
            messagebox.showerror(APP_NAME, str(exc))
            return
        self.send_cmd(CMD_UI, struct.pack("<HBB", session, action, 0), f"SAT_UI({action})")

    def _stream_config(self) -> StreamConfig:
        session = self._session_id()
        base_rx, base_tx = self._base_freqs_hz()
        return StreamConfig(
            session_id=session,
            base_rx_hz=base_rx,
            base_tx_hz=base_tx,
            rx_amp_hz=int(float(self.rx_amp_var.get())),
            tx_amp_hz=int(float(self.tx_amp_var.get())),
            period_s=max(1.0, float(self.period_var.get())),
            ack_every=max(0, int(self.ack_every_var.get() or "0")),
        )

    def _make_update(self, config: StreamConfig, elapsed: float) -> bytes:
        phase = 2.0 * math.pi * ((elapsed % config.period_s) / config.period_s)
        rx_dop = round(config.rx_amp_hz * math.sin(phase))
        tx_dop = round(config.tx_amp_hz * math.sin(phase + math.pi / 4.0))
        rx_hz = max(1, config.base_rx_hz + rx_dop)
        tx_hz = max(1, config.base_tx_hz + tx_dop)
        flags = SAT_UPDATE_ACK_REQUEST if config.ack_every and self.sequence % config.ack_every == 0 else 0
        utc = int(time.time())
        az = round(((elapsed * 3.0) % 360.0) * 10)
        el = round((15.0 + 55.0 * abs(math.sin(phase / 2.0))) * 10)
        range_km = round(800 + 1200 * abs(math.cos(phase / 2.0)))
        return struct.pack(
            "<HHBBIIIiihhH",
            config.session_id, self.sequence & 0xFFFF, flags, 0, utc,
            rx_hz, tx_hz, rx_dop, tx_dop, az, el, range_km,
        )

    def send_single_update(self) -> None:
        if not self.require_connection():
            return
        try:
            config = self._stream_config()
            self.sequence = (self.sequence + 1) & 0xFFFF
            payload = self._make_update(config, 0.0)
            self.transport.send(CMD_UPDATE, payload)
            self.log(f"TX SAT_UPDATE seq={self.sequence}")
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror(APP_NAME, f"UPDATE 参数/发送错误：\n{exc}")

    def toggle_stream(self) -> None:
        if self.stream_thread and self.stream_thread.is_alive():
            self.stream_stop.set()
            return
        if not self.require_connection():
            return
        if not self.session_active:
            messagebox.showwarning(APP_NAME, "请先成功执行 SAT_BEGIN。")
            return
        try:
            rate = int(self.rate_var.get())
            if rate not in (1, 5, 10, 20, 50, 100):
                raise ValueError("UPDATE rate 必须为 1/5/10/20/50/100 Hz")
            config = self._stream_config()
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror(APP_NAME, str(exc))
            return

        self.stream_stop.clear()
        self.stream_thread = threading.Thread(target=self._stream_worker, args=(rate, config), daemon=True)
        self.stream_thread.start()
        self._ensure_status_worker()
        self.stream_btn.configure(text="停止实时流")
        self.log(f"Realtime stream started: {rate} Hz")

    def _stream_worker(self, rate: int, config: StreamConfig, duration: float | None = None) -> int:
        interval = 1.0 / rate
        start = time.perf_counter()
        deadline = None if duration is None else start + duration
        next_tick = start
        sent = 0
        stop_event = self.stress_stop if duration is not None else self.stream_stop
        try:
            while not stop_event.is_set():
                now = time.perf_counter()
                if deadline is not None and now >= deadline:
                    break
                if now < next_tick:
                    time.sleep(min(next_tick - now, 0.002))
                    continue
                self.sequence = (self.sequence + 1) & 0xFFFF
                payload = self._make_update(config, now - start)
                self.transport.send(CMD_UPDATE, payload)
                sent += 1
                next_tick += interval
                if now - next_tick > interval * 4:
                    next_tick = now + interval
        except Exception as exc:  # noqa: BLE001
            self.events.put(("worker_error", f"Stream error: {exc}"))
        finally:
            if duration is None:
                self.events.put(("stream_finished", None))
        return sent
