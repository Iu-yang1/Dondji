from __future__ import annotations

import queue
import struct
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox

from protocol import (
    APP_NAME, CAP_NAMES, LINK_NAMES, POWER_NAMES, RPL_BEGIN, RPL_END, RPL_HELLO, RPL_PTT,
    RPL_STATUS, RPL_UI, RPL_UPDATE, STATUS_BASE_FMT, STATUS_BASE_SIZE, STATUS_DIAG_FMT,
    STATUS_DIAG_SIZE, ProtocolError, decode_errors, format_hz,
)


class EventActionsMixin:
    def _drain_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "frame":
                    self._handle_frame(payload)
                elif kind == "host_parser":
                    crc, bad = payload
                    self.metric_vars["host_crc"].set(str(crc))
                    self.metric_vars["host_bad"].set(str(bad))
                elif kind in ("transport_error", "worker_error"):
                    self.log(str(payload))
                    if kind == "transport_error":
                        self.connection_label.configure(text="连接异常", style="Bad.TLabel")
                elif kind == "stream_finished":
                    self.stream_btn.configure(text="开始实时流")
                    self.log("Realtime stream stopped")
                elif kind == "stress_stage":
                    self.log(f"Stress stage: {payload}")
                elif kind == "stress_result":
                    self._append_stress_result(payload)
                elif kind == "stress_finished":
                    self.stress_btn.configure(text="自动 1→100 Hz 压测")
                    self.log("Stress test finished")
        except queue.Empty:
            pass
        self.after(50, self._drain_events)

    def _handle_frame(self, frame) -> None:
        cid, data = frame.command_id, frame.payload
        try:
            if cid == RPL_HELLO:
                if len(data) != 8:
                    raise ProtocolError(f"HELLO_REPLY size={len(data)}, expected 8")
                major, minor, max_hz, resolution, max_wire, caps = struct.unpack("<BBBBHH", data)
                cap_text = ", ".join(name for bit, name in CAP_NAMES.items() if caps & bit) or "none"
                self.metric_vars["protocol"].set(f"{major}.{minor} / max {max_hz} Hz / {resolution} Hz step")
                self.metric_vars["caps"].set(cap_text)
                self.log(f"RX HELLO_REPLY protocol={major}.{minor}, max={max_hz}Hz, step={resolution}Hz, wire={max_wire}B, caps=0x{caps:04X}")
            elif cid == RPL_BEGIN:
                if len(data) != 12:
                    raise ProtocolError(f"BEGIN_REPLY size={len(data)}, expected 12")
                session, status, applied_rx, target_tx = struct.unpack("<HHII", data)
                self.session_active = status == 0
                self.active_session_id = session if status == 0 else 0
                self.metric_vars["session"].set(f"{session} / {'ACTIVE' if status == 0 else 'ERROR'}")
                self.metric_vars["errors"].set(decode_errors(status))
                self.metric_vars["applied"].set(format_hz(applied_rx) if applied_rx else "-")
                self.metric_vars["tx_target"].set(format_hz(target_tx) if target_tx else "-")
                self.log(f"RX BEGIN_REPLY session={session}, status={decode_errors(status)}, RX={applied_rx}, TX={target_tx}")
            elif cid == RPL_UPDATE:
                if len(data) != 20:
                    raise ProtocolError(f"UPDATE_REPLY size={len(data)}, expected 20")
                session, received, applied, errors, applied_hz, target_tx, age_ms, ptt, link = struct.unpack("<HHHHIIHBB", data)
                self._apply_common_status(session, received, applied, errors, applied_hz, target_tx, age_ms, link, ptt, None, None, None)
            elif cid == RPL_STATUS:
                expected_v12 = STATUS_BASE_SIZE + STATUS_DIAG_SIZE
                if len(data) not in (STATUS_BASE_SIZE, expected_v12):
                    raise ProtocolError(
                        f"STATUS_REPLY size={len(data)}, expected {STATUS_BASE_SIZE} (v1.1) or {expected_v12} (v1.2)"
                    )
                values = struct.unpack_from(STATUS_BASE_FMT, data, 0)
                session, received, applied, errors, applied_hz, target_tx, age_ms, rate_hz, crc_errors, dropped, link, ptt, ui, _ = values
                self.last_status = {
                    "session": session,
                    "received_seq": received,
                    "applied_seq": applied,
                    "error_flags": errors,
                    "applied_hz": applied_hz,
                    "target_tx_hz": target_tx,
                    "age_ms": age_ms,
                    "update_rate_hz": rate_hz,
                    "crc_errors": crc_errors,
                    "dropped_updates": dropped,
                    "link_state": link,
                    "ptt": ptt,
                    "ui_visible": ui,
                }
                self._apply_common_status(session, received, applied, errors, applied_hz, target_tx, age_ms, link, ptt, rate_hz, crc_errors, dropped, ui)

                if len(data) == expected_v12:
                    diag = struct.unpack_from(STATUS_DIAG_FMT, data, STATUS_BASE_SIZE)
                    tx_power, tx_bias, pa_enable, cal_low, cal_mid, cal_high, reg30, reg33, reg36, reg37, reg38, reg39 = diag
                    power_name = POWER_NAMES.get(tx_power, f"UNKNOWN({tx_power})")
                    self.metric_vars["tx_power"].set(power_name)
                    self.metric_vars["tx_bias"].set(f"{tx_bias} / 0x{tx_bias:02X}")
                    self.metric_vars["pa_enable"].set("HIGH" if pa_enable else "LOW")
                    self.metric_vars["tx_cal"].set(
                        f"{cal_low.hex(' ').upper()} / {cal_mid.hex(' ').upper()} / {cal_high.hex(' ').upper()}"
                    )
                    self.metric_vars["regs_a"].set(f"30={reg30:04X} 33={reg33:04X} 36={reg36:04X}")
                    self.metric_vars["regs_b"].set(f"37={reg37:04X} 38={reg38:04X} 39={reg39:04X}")
                    self.last_status.update({
                        "tx_power": tx_power,
                        "txp_calculated": tx_bias,
                        "pa_enable": pa_enable,
                        "reg30": reg30,
                        "reg33": reg33,
                        "reg36": reg36,
                        "reg37": reg37,
                        "reg38": reg38,
                        "reg39": reg39,
                    })
                else:
                    self.metric_vars["tx_power"].set("v1.1 firmware")
                    self.metric_vars["tx_bias"].set("-")
                    self.metric_vars["pa_enable"].set("-")
                    self.metric_vars["tx_cal"].set("-")
                    self.metric_vars["regs_a"].set("-")
                    self.metric_vars["regs_b"].set("-")
            elif cid in (RPL_END, RPL_UI, RPL_PTT):
                if len(data) != 4:
                    raise ProtocolError(f"Simple reply size={len(data)}, expected 4")
                session, status = struct.unpack("<HH", data)
                label = {RPL_END: "END", RPL_UI: "UI", RPL_PTT: "PTT"}[cid]
                self.log(f"RX {label}_REPLY session={session}, status={decode_errors(status)}")
                if cid == RPL_END and status == 0:
                    self.session_active = False
                    self.active_session_id = 0
                    self.stop_all_workers()
                    self.metric_vars["session"].set(f"{session} / ENDED")
            else:
                self.log(f"RX unknown reply 0x{cid:04X}, payload={data.hex(' ')}")
        except Exception as exc:  # noqa: BLE001
            self.log(f"RX decode error 0x{cid:04X}: {exc}; raw={data.hex(' ')}")

    def _apply_common_status(
        self, session: int, received: int, applied: int, errors: int, applied_hz: int,
        target_tx: int, age_ms: int, link: int, ptt: int, rate_hz: int | None,
        crc_errors: int | None, dropped: int | None, ui: int | None = None,
    ) -> None:
        self.metric_vars["session"].set(str(session))
        self.metric_vars["received_seq"].set(str(received))
        self.metric_vars["applied_seq"].set(str(applied))
        self.metric_vars["errors"].set(decode_errors(errors))
        self.metric_vars["applied"].set(format_hz(applied_hz))
        self.metric_vars["tx_target"].set(format_hz(target_tx))
        self.metric_vars["age"].set(f"{age_ms} ms")
        self.metric_vars["link"].set(LINK_NAMES.get(link, str(link)))
        self.metric_vars["ptt"].set("TX" if ptt else "RX")
        if rate_hz is not None:
            self.metric_vars["rate"].set(f"{rate_hz} Hz")
        if crc_errors is not None:
            self.metric_vars["device_crc"].set(str(crc_errors))
        if dropped is not None:
            self.metric_vars["device_drop"].set(str(dropped))
        if ui is not None:
            self.metric_vars["ui"].set("VISIBLE" if ui else "HIDDEN")

    def _append_stress_result(self, row: dict[str, object]) -> None:
        self.stress_results.append(row)
        self.result_tree.insert("", "end", values=(
            row["rate"], f"{row['seconds']:g}", row["sent"], row["received"], row["applied"],
            row["crc_delta"], row["drop_delta"], row["result"],
        ))
        self.log(
            f"Stress {row['rate']}Hz: sent={row['sent']} recv={row['received']} applied={row['applied']} "
            f"CRCΔ={row['crc_delta']} DROPΔ={row['drop_delta']} => {row['result']}"
        )

    def clear_log(self) -> None:
        self.log_lines.clear()
        self.log_text.delete("1.0", "end")

    def export_log(self) -> None:
        path = filedialog.asksaveasfilename(
            title="导出日志", defaultextension=".txt",
            filetypes=[("Text files", "*.txt"), ("All files", "*.*")],
            initialfile=f"dondji_satellite_test_{datetime.now():%Y%m%d_%H%M%S}.txt",
        )
        if not path:
            return
        Path(path).write_text("\n".join(self.log_lines) + "\n", encoding="utf-8")

    def clear_results(self) -> None:
        self.stress_results.clear()
        for item in self.result_tree.get_children():
            self.result_tree.delete(item)

    def export_results(self) -> None:
        if not self.stress_results:
            messagebox.showinfo(APP_NAME, "当前没有压测结果。")
            return
        path = filedialog.asksaveasfilename(
            title="导出压测 CSV", defaultextension=".csv",
            filetypes=[("CSV files", "*.csv")],
            initialfile=f"dondji_satellite_stress_{datetime.now():%Y%m%d_%H%M%S}.csv",
        )
        if not path:
            return
        lines = ["rate_hz,duration_s,sent,received_seq,applied_seq,crc_delta,drop_delta,result,start_seq,end_seq"]
        for r in self.stress_results:
            lines.append(
                f"{r['rate']},{r['seconds']},{r['sent']},{r['received']},{r['applied']},"
                f"{r['crc_delta']},{r['drop_delta']},{r['result']},{r['start_seq']},{r['end_seq']}"
            )
        Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8-sig")

    def on_close(self) -> None:
        self.stop_all_workers()
        self.transport.disconnect()
        self.destroy()