from __future__ import annotations

import tkinter as tk
from tkinter import ttk

from protocol import APP_NAME, APP_VERSION, PID, VID, SAT_UI_HIDE, SAT_UI_SHOW, SAT_UI_TOGGLE


class UiMixin:
    def _build_styles(self) -> None:
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Title.TLabel", font=("Segoe UI", 14, "bold"))
        style.configure("Metric.TLabel", font=("Consolas", 10, "bold"))
        style.configure("Good.TLabel", foreground="#007800")
        style.configure("Bad.TLabel", foreground="#A00000")

    def _build_ui(self) -> None:
        outer = ttk.Frame(self, padding=10)
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(3, weight=1)

        self._build_connection(outer)
        self._build_session_and_stream(outer)
        self._build_telemetry(outer)
        self._build_bottom(outer)

    def _build_connection(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="USB CDC 连接", padding=8)
        frame.grid(row=0, column=0, sticky="ew", pady=(0, 8))
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="COM 端口").grid(row=0, column=0, sticky="w")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(frame, textvariable=self.port_var, state="readonly", width=48)
        self.port_combo.grid(row=0, column=1, sticky="ew", padx=6)
        ttk.Button(frame, text="刷新", command=self.refresh_ports).grid(row=0, column=2, padx=3)
        self.connect_btn = ttk.Button(frame, text="连接", command=self.toggle_connection)
        self.connect_btn.grid(row=0, column=3, padx=3)
        self.connection_label = ttk.Label(frame, text="未连接", style="Bad.TLabel")
        self.connection_label.grid(row=0, column=4, padx=(10, 0))

        self.device_info_var = tk.StringVar(value="目标 USB: VID 36B7 / PID FFFF")
        ttk.Label(frame, textvariable=self.device_info_var).grid(row=1, column=0, columnspan=5, sticky="w", pady=(6, 0))

    def _build_session_and_stream(self, parent: ttk.Frame) -> None:
        holder = ttk.Frame(parent)
        holder.grid(row=1, column=0, sticky="ew", pady=(0, 8))
        holder.columnconfigure(0, weight=1)
        holder.columnconfigure(1, weight=1)

        session = ttk.LabelFrame(holder, text="Satellite Session", padding=8)
        session.grid(row=0, column=0, sticky="nsew", padx=(0, 4))
        for col in (1, 3):
            session.columnconfigure(col, weight=1)

        self.satellite_var = tk.StringVar(value="TEST")
        self.session_var = tk.StringVar(value="4660")
        self.rx_mhz_var = tk.StringVar(value="435.650000")
        self.tx_mhz_var = tk.StringVar(value="145.950000")
        self.ctcss_var = tk.StringVar(value="0")
        self.mod_var = tk.StringVar(value="FM")
        self.auto_show_var = tk.BooleanVar(value=True)

        fields = [
            ("卫星名称", self.satellite_var),
            ("Session ID", self.session_var),
            ("RX MHz", self.rx_mhz_var),
            ("TX MHz", self.tx_mhz_var),
            ("TX CTCSS Hz", self.ctcss_var),
        ]
        for i, (label, var) in enumerate(fields):
            row, pair = divmod(i, 2)
            col = pair * 2
            ttk.Label(session, text=label).grid(row=row, column=col, sticky="w", padx=(0, 4), pady=2)
            ttk.Entry(session, textvariable=var, width=16).grid(row=row, column=col + 1, sticky="ew", padx=(0, 8), pady=2)

        ttk.Label(session, text="调制").grid(row=3, column=0, sticky="w", pady=2)
        ttk.Combobox(session, textvariable=self.mod_var, values=("KEEP", "FM"), state="readonly", width=10).grid(row=3, column=1, sticky="w")
        ttk.Checkbutton(session, text="SAT_BEGIN 后自动显示手台卫星页", variable=self.auto_show_var).grid(row=3, column=2, columnspan=2, sticky="w")

        buttons = ttk.Frame(session)
        buttons.grid(row=4, column=0, columnspan=4, sticky="ew", pady=(8, 0))
        for i in range(7):
            buttons.columnconfigure(i, weight=1)
        ttk.Button(buttons, text="HELLO", command=self.send_hello).grid(row=0, column=0, sticky="ew", padx=2)
        ttk.Button(buttons, text="BEGIN", command=self.send_begin).grid(row=0, column=1, sticky="ew", padx=2)
        ttk.Button(buttons, text="STATUS", command=self.send_status).grid(row=0, column=2, sticky="ew", padx=2)
        ttk.Button(buttons, text="END", command=self.send_end).grid(row=0, column=3, sticky="ew", padx=2)
        ttk.Button(buttons, text="显示 UI", command=lambda: self.send_ui(SAT_UI_SHOW)).grid(row=0, column=4, sticky="ew", padx=2)
        ttk.Button(buttons, text="隐藏 UI", command=lambda: self.send_ui(SAT_UI_HIDE)).grid(row=0, column=5, sticky="ew", padx=2)
        ttk.Button(buttons, text="切换 UI", command=lambda: self.send_ui(SAT_UI_TOGGLE)).grid(row=0, column=6, sticky="ew", padx=2)

        stream = ttk.LabelFrame(holder, text="实时 Doppler / 压测", padding=8)
        stream.grid(row=0, column=1, sticky="nsew", padx=(4, 0))
        stream.columnconfigure(1, weight=1)
        stream.columnconfigure(3, weight=1)

        self.rate_var = tk.StringVar(value="20")
        self.rx_amp_var = tk.StringVar(value="10000")
        self.tx_amp_var = tk.StringVar(value="3500")
        self.period_var = tk.StringVar(value="60")
        self.ack_every_var = tk.StringVar(value="20")
        self.stress_duration_var = tk.StringVar(value="15")

        ttk.Label(stream, text="UPDATE Hz").grid(row=0, column=0, sticky="w")
        ttk.Combobox(stream, textvariable=self.rate_var, values=("1", "5", "10", "20", "50", "100"), state="readonly", width=8).grid(row=0, column=1, sticky="w")
        ttk.Label(stream, text="ACK 每 N 包").grid(row=0, column=2, sticky="w")
        ttk.Entry(stream, textvariable=self.ack_every_var, width=8).grid(row=0, column=3, sticky="w")

        ttk.Label(stream, text="RX Doppler ±Hz").grid(row=1, column=0, sticky="w", pady=2)
        ttk.Entry(stream, textvariable=self.rx_amp_var).grid(row=1, column=1, sticky="ew", padx=(0, 8))
        ttk.Label(stream, text="TX Doppler ±Hz").grid(row=1, column=2, sticky="w", pady=2)
        ttk.Entry(stream, textvariable=self.tx_amp_var).grid(row=1, column=3, sticky="ew")

        ttk.Label(stream, text="Sweep 周期 s").grid(row=2, column=0, sticky="w", pady=2)
        ttk.Entry(stream, textvariable=self.period_var).grid(row=2, column=1, sticky="ew", padx=(0, 8))
        ttk.Label(stream, text="每档压测 s").grid(row=2, column=2, sticky="w", pady=2)
        ttk.Entry(stream, textvariable=self.stress_duration_var).grid(row=2, column=3, sticky="ew")

        btns = ttk.Frame(stream)
        btns.grid(row=3, column=0, columnspan=4, sticky="ew", pady=(8, 0))
        for i in range(4):
            btns.columnconfigure(i, weight=1)
        self.stream_btn = ttk.Button(btns, text="开始实时流", command=self.toggle_stream)
        self.stream_btn.grid(row=0, column=0, sticky="ew", padx=2)
        ttk.Button(btns, text="单发 UPDATE", command=self.send_single_update).grid(row=0, column=1, sticky="ew", padx=2)
        self.stress_btn = ttk.Button(btns, text="自动 1→100 Hz 压测", command=self.toggle_stress)
        self.stress_btn.grid(row=0, column=2, sticky="ew", padx=2)
        ttk.Button(btns, text="清零 Host 统计", command=self.reset_host_stats).grid(row=0, column=3, sticky="ew", padx=2)

    def _build_telemetry(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="实时 Telemetry / TX Diagnostics", padding=8)
        frame.grid(row=2, column=0, sticky="ew", pady=(0, 8))
        keys = [
            "protocol", "caps", "session", "link", "received_seq", "applied_seq",
            "device_crc", "device_drop", "host_crc", "host_bad", "rate", "age",
            "applied", "tx_target", "ptt", "ui", "errors",
            "tx_power", "tx_bias", "pa_enable", "tx_cal", "regs_a", "regs_b",
        ]
        self.metric_vars = {key: tk.StringVar(value="-") for key in keys}
        labels = [
            ("Protocol", "protocol"), ("Capabilities", "caps"), ("Session", "session"), ("Link", "link"),
            ("Received seq", "received_seq"), ("Applied seq", "applied_seq"),
            ("Device CRC", "device_crc"), ("Device drop", "device_drop"),
            ("Host CRC", "host_crc"), ("Host bad frame", "host_bad"),
            ("Device rate", "rate"), ("Packet age", "age"),
            ("Applied RF", "applied"), ("TX target", "tx_target"),
            ("PTT", "ptt"), ("UI", "ui"), ("Error flags", "errors"),
            ("TX power", "tx_power"), ("TXP bias", "tx_bias"),
            ("PA enable", "pa_enable"), ("TX calib L/M/H", "tx_cal"),
            ("REG30/33/36", "regs_a"), ("REG37/38/39", "regs_b"),
        ]
        for c in range(4):
            frame.columnconfigure(c, weight=1 if c % 2 else 0)
        for i, (label, key) in enumerate(labels):
            row = i // 2
            base_col = (i % 2) * 2
            ttk.Label(frame, text=label + ":").grid(row=row, column=base_col, sticky="w", padx=(0, 4), pady=1)
            ttk.Label(frame, textvariable=self.metric_vars[key], style="Metric.TLabel").grid(row=row, column=base_col + 1, sticky="w", padx=(0, 16), pady=1)

    def _build_bottom(self, parent: ttk.Frame) -> None:
        pane = ttk.Panedwindow(parent, orient="horizontal")
        pane.grid(row=3, column=0, sticky="nsew")

        log_frame = ttk.LabelFrame(pane, text="通信日志", padding=5)
        result_frame = ttk.LabelFrame(pane, text="压测结果", padding=5)
        pane.add(log_frame, weight=3)
        pane.add(result_frame, weight=2)

        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)
        self.log_text = tk.Text(log_frame, wrap="none", font=("Consolas", 9), height=16)
        self.log_text.grid(row=0, column=0, sticky="nsew")
        log_scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        log_scroll.grid(row=0, column=1, sticky="ns")
        self.log_text.configure(yscrollcommand=log_scroll.set)
        log_btns = ttk.Frame(log_frame)
        log_btns.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(4, 0))
        ttk.Button(log_btns, text="清空日志", command=self.clear_log).pack(side="left")
        ttk.Button(log_btns, text="导出日志", command=self.export_log).pack(side="left", padx=4)

        columns = ("rate", "seconds", "sent", "rx", "applied", "crc", "drop", "result")
        self.result_tree = ttk.Treeview(result_frame, columns=columns, show="headings", height=14)
        headings = {
            "rate": "Hz", "seconds": "时长", "sent": "发送", "rx": "recv seq",
            "applied": "applied", "crc": "CRC Δ", "drop": "Drop Δ", "result": "结果",
        }
        widths = {"rate": 45, "seconds": 55, "sent": 65, "rx": 75, "applied": 75, "crc": 55, "drop": 60, "result": 70}
        for key in columns:
            self.result_tree.heading(key, text=headings[key])
            self.result_tree.column(key, width=widths[key], anchor="center")
        self.result_tree.pack(fill="both", expand=True)
        result_btns = ttk.Frame(result_frame)
        result_btns.pack(fill="x", pady=(4, 0))
        ttk.Button(result_btns, text="清空结果", command=self.clear_results).pack(side="left")
        ttk.Button(result_btns, text="导出 CSV", command=self.export_results).pack(side="left", padx=4)