from __future__ import annotations

import threading
import time
from tkinter import messagebox

from protocol import APP_NAME, StreamConfig


class StressActionsMixin:
    def _ensure_status_worker(self) -> None:
        if self.status_thread and self.status_thread.is_alive():
            return
        self.status_thread = threading.Thread(target=self._status_worker, daemon=True)
        self.status_thread.start()

    def _status_worker(self) -> None:
        while self.transport.connected and (self.stream_thread and self.stream_thread.is_alive() or self.stress_thread and self.stress_thread.is_alive()):
            self.send_status(quiet=True)
            time.sleep(1.0)

    def toggle_stress(self) -> None:
        if self.stress_thread and self.stress_thread.is_alive():
            self.stress_stop.set()
            return
        if not self.require_connection():
            return
        if not self.session_active:
            messagebox.showwarning(APP_NAME, "请先成功执行 SAT_BEGIN。")
            return
        try:
            duration = float(self.stress_duration_var.get())
            if duration < 3:
                raise ValueError("每档压测建议至少 3 秒")
            config = self._stream_config()
        except ValueError as exc:
            messagebox.showerror(APP_NAME, str(exc))
            return

        self.stream_stop.set()
        self.stress_stop.clear()
        self.clear_results()
        self.stress_thread = threading.Thread(target=self._stress_worker, args=(duration, config), daemon=True)
        self.stress_thread.start()
        self._ensure_status_worker()
        self.stress_btn.configure(text="停止自动压测")
        self.log(f"Stress test started: {duration:g}s each @ 1/5/10/20/50/100 Hz")

    def _stress_worker(self, duration: float, config: StreamConfig) -> None:
        for rate in (1, 5, 10, 20, 50, 100):
            if self.stress_stop.is_set():
                break
            baseline = dict(self.last_status)
            start_seq = self.sequence
            self.events.put(("stress_stage", f"{rate} Hz"))
            sent = self._stream_worker(rate, config, duration=duration)
            try:
                self.send_status(quiet=True)
            except Exception:
                pass
            time.sleep(0.35)
            status = dict(self.last_status)
            crc_delta = max(0, int(status.get("crc_errors", 0)) - int(baseline.get("crc_errors", 0)))
            drop_delta = max(0, int(status.get("dropped_updates", 0)) - int(baseline.get("dropped_updates", 0)))
            applied = status.get("applied_seq", "-")
            received = status.get("received_seq", "-")
            result = "PASS" if crc_delta == 0 and drop_delta == 0 else "CHECK"
            row = {
                "rate": rate,
                "seconds": duration,
                "sent": sent,
                "received": received,
                "applied": applied,
                "crc_delta": crc_delta,
                "drop_delta": drop_delta,
                "result": result,
                "start_seq": start_seq,
                "end_seq": self.sequence,
            }
            self.events.put(("stress_result", row))
        self.events.put(("stress_finished", None))

    def reset_host_stats(self) -> None:
        self.transport.parser.host_crc_errors = 0
        self.transport.parser.host_bad_frames = 0
        self.metric_vars["host_crc"].set("0")
        self.metric_vars["host_bad"].set("0")
        self.log("Host parser counters reset")

    def stop_all_workers(self) -> None:
        self.stream_stop.set()
        self.stress_stop.set()
