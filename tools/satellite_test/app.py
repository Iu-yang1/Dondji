from __future__ import annotations

import queue
import threading
import tkinter as tk

from event_actions import EventActionsMixin
from session_actions import SessionActionsMixin
from stress_actions import StressActionsMixin
from protocol import APP_NAME, APP_VERSION
from transport import SerialTransport
from ui import UiMixin


class SatelliteTesterApp(EventActionsMixin, StressActionsMixin, SessionActionsMixin, UiMixin, tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(f"{APP_NAME} {APP_VERSION}")
        self.geometry("1180x820")
        self.minsize(1000, 720)

        self.events: queue.Queue = queue.Queue()
        self.transport = SerialTransport(self.events)
        self.stream_stop = threading.Event()
        self.stream_thread: threading.Thread | None = None
        self.status_thread: threading.Thread | None = None
        self.stress_stop = threading.Event()
        self.stress_thread: threading.Thread | None = None
        self.sequence = 0
        self.session_active = False
        self.active_session_id = 0
        self.last_status: dict[str, int] = {}
        self.stress_results: list[dict[str, object]] = []
        self.log_lines: list[str] = []

        self._build_styles()
        self._build_ui()
        self.refresh_ports()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(50, self._drain_events)


def main() -> None:
    app = SatelliteTesterApp()
    app.mainloop()


if __name__ == "__main__":
    main()
