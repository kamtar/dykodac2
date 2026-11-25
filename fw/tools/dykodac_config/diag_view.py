"""Live diagnostics tab: strip charts, status lamps, counters, event trace."""

from __future__ import annotations

from collections import deque

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (
    QFrame, QGridLayout, QGroupBox, QHBoxLayout, QLabel, QListWidget, QPushButton, QSizePolicy, QVBoxLayout,
    QWidget,
)

from . import diagnostics as d

HISTORY = 240  # samples (2 per second -> 2 minutes)


class StripChart(QWidget):
    def __init__(self, title: str, unit: str, color: str, lo: float | None = None, hi: float | None = None,
                 fmt: str = "{:.1f}", bands: list[tuple[float, float, str]] | None = None):
        super().__init__()
        self.title, self.unit, self.color, self.fmt = title, unit, QColor(color), fmt
        self.lo, self.hi, self.bands = lo, hi, bands or []
        self.data: deque = deque(maxlen=HISTORY)
        self.setMinimumHeight(120)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

    def push(self, value):
        self.data.append(value)
        self.update()

    def clear(self):
        self.data.clear()
        self.update()

    def paintEvent(self, _):
        pa = QPainter(self)
        pa.setRenderHint(QPainter.Antialiasing)
        pa.fillRect(self.rect(), QColor("#191B20"))
        r = QRectF(self.rect()).adjusted(52, 22, -10, -8)
        vals = [v for v in self.data if v is not None]
        lo = self.lo if self.lo is not None else (min(vals) if vals else 0.0)
        hi = self.hi if self.hi is not None else (max(vals) if vals else 1.0)
        if hi - lo < 1e-9:
            hi, lo = hi + 1, lo - 1
        pad = 0 if (self.lo is not None and self.hi is not None) else (hi - lo) * 0.1
        lo, hi = lo - pad, hi + pad

        def y(v):
            return r.bottom() - (min(max(v, lo), hi) - lo) / (hi - lo) * r.height()

        for a, b, col in self.bands:
            c = QColor(col)
            c.setAlpha(38)
            pa.fillRect(QRectF(r.left(), y(b), r.width(), y(a) - y(b)), c)
        font = QFont(self.font())
        font.setPointSizeF(8)
        pa.setFont(font)
        for frac in (0.0, 0.5, 1.0):
            v = lo + (hi - lo) * frac
            pa.setPen(QPen(QColor("#2C3038"), 1))
            pa.drawLine(QPointF(r.left(), y(v)), QPointF(r.right(), y(v)))
            pa.setPen(QColor("#7D8590"))
            pa.drawText(QRectF(0, y(v) - 8, r.left() - 6, 16), Qt.AlignRight | Qt.AlignVCenter, self.fmt.format(v))
        pa.setPen(QColor("#9FB3C8"))
        f2 = QFont(self.font())
        f2.setBold(True)
        pa.setFont(f2)
        now = self.fmt.format(vals[-1]) + " " + self.unit if vals else "—"
        pa.drawText(QRectF(8, 2, self.width() - 16, 18), Qt.AlignLeft, self.title)
        pa.setPen(self.color)
        pa.drawText(QRectF(8, 2, self.width() - 16, 18), Qt.AlignRight, now)
        if len(self.data) < 2:
            return
        step = r.width() / (HISTORY - 1)
        x0 = r.right() - (len(self.data) - 1) * step
        path = QPainterPath()
        started = False
        for i, v in enumerate(self.data):
            if v is None:
                started = False
                continue
            pt = QPointF(x0 + i * step, y(v))
            if started:
                path.lineTo(pt)
            else:
                path.moveTo(pt)
                started = True
        pa.setPen(QPen(self.color, 2))
        pa.drawPath(path)


class Lamp(QLabel):
    def __init__(self, text):
        super().__init__(text)
        self.set(False)

    def set(self, on: bool, bad: bool = False):
        col = "#E57373" if bad and on else ("#81C784" if on else "#3A3F49")
        fg = "#111" if on else "#8A919C"
        self.setStyleSheet(f"background:{col}; color:{fg}; border-radius:9px; padding:3px 10px;")


class DiagnosticsTab(QWidget):
    def __init__(self):
        super().__init__()
        v = QVBoxLayout(self)
        v.setContentsMargins(10, 8, 10, 8)
        self.headline = QLabel("Waiting for the DAC…")
        self.headline.setTextFormat(Qt.RichText)
        self.headline.setStyleSheet("font-size: 15px; padding: 2px;")
        v.addWidget(self.headline)

        lamps = QHBoxLayout()
        self.lamps = {}
        for name in ("USB mounted", "Engine running", "Clock stable", "UAC2 clock valid", "Relay connected",
                     "DAC muted", "Suspended"):
            lamp = Lamp(name)
            self.lamps[name] = lamp
            lamps.addWidget(lamp)
        lamps.addStretch()
        reset = QPushButton("Clear graphs")
        reset.clicked.connect(self.clear)
        lamps.addWidget(reset)
        v.addLayout(lamps)

        grid = QGridLayout()
        half = d.RING_CAPACITY // 2
        self.c_ring = StripChart("USB ring buffer fill", "%", "#4FC3F7", 0, 100, "{:.0f}",
                                 bands=[(0, 25, "#E57373"), (40, 60, "#81C784")])
        self.c_fb = StripChart("Async feedback (frames / 125 µs)", "", "#BA68C8", fmt="{:.4f}")
        self.c_cpu = StripChart("Audio refill CPU load (avg of 1 ms)", "%", "#FFB74D", 0, 100, "{:.0f}",
                                bands=[(80, 100, "#E57373")])
        self.c_cpu_max = StripChart("Worst refill time", "µs", "#FF8A65", 0, None, "{:.0f}",
                                    bands=[(1000, 4000, "#E57373")])
        self.c_pkt = StripChart("USB packets / s", "", "#81C784", 0, None, "{:.0f}")
        self.c_err = StripChart("New errors / s (underrun+drop+overflow+DMA)", "", "#E57373", 0, None, "{:.0f}")
        for i, c in enumerate((self.c_ring, self.c_fb, self.c_cpu, self.c_cpu_max, self.c_pkt, self.c_err)):
            grid.addWidget(c, i // 2, i % 2)
        v.addLayout(grid, 3)
        self._half = half

        bottom = QHBoxLayout()
        g = QGroupBox("Counters since boot")
        self.counter_grid = QGridLayout(g)
        self.counters = {}
        names = ["Uptime", "USB packets", "Packet size (last/min/max)", "Dropped packets", "Dropped events",
                 "Underruns", "Overflows", "Padding errors", "DMA completions", "DMA errors",
                 "Start step", "Last stop", "Refill count", "Max pending refills"]
        for i, n in enumerate(names):
            k = QLabel(n)
            k.setStyleSheet("color:#9AA5B1;")
            val = QLabel("—")
            val.setTextInteractionFlags(Qt.TextSelectableByMouse)
            self.counter_grid.addWidget(k, i % 7, (i // 7) * 2)
            self.counter_grid.addWidget(val, i % 7, (i // 7) * 2 + 1)
            self.counters[n] = val
        bottom.addWidget(g, 3)
        g = QGroupBox("Recent USB events (oldest first)")
        gv = QVBoxLayout(g)
        self.events = QListWidget()
        self.events.setStyleSheet("font-family: Consolas, monospace;")
        gv.addWidget(self.events)
        bottom.addWidget(g, 2)
        v.addLayout(bottom, 2)
        self._prev = None
        self._prev_t = None

    def clear(self):
        for c in (self.c_ring, self.c_fb, self.c_cpu, self.c_cpu_max, self.c_pkt, self.c_err):
            c.clear()
        self._prev = None

    def offline(self):
        self.headline.setText("<span style='color:#E57373'>Not connected</span>")
        for lamp in self.lamps.values():
            lamp.set(False)

    def update_from(self, diag: d.Diag | None, perf: d.Perf | None, events: list[str] | None, t: float):
        if diag is None:
            return
        rate = diag.active_rate or diag.desired_rate
        playing = diag.app_state == "Playing" and diag.stream_requested
        color = "#81C784" if playing else ("#E57373" if diag.app_state == "FaultMuted" else "#FFB74D")
        head = f"<b style='color:{color}'>{diag.app_state}</b>"
        if rate:
            head += f" &nbsp;·&nbsp; {rate / 1000:g} kHz ({'48k' if diag.family_48k else '44.1k'} oscillator family)"
        head += f" &nbsp;·&nbsp; {'stream active' if diag.stream_requested else 'no stream'}"
        if diag.host_muted:
            head += " &nbsp;·&nbsp; <span style='color:#FFB74D'>host muted</span>"
        if diag.stop_reason not in ("None",):
            head += f" &nbsp;·&nbsp; last stop: {diag.stop_reason}"
        self.headline.setText(head)
        for name, lamp in self.lamps.items():
            on = diag.flags.get(name, False)
            lamp.set(on, bad=name in ("Suspended",))
        self.c_ring.push(100.0 * diag.ring_fill / d.RING_CAPACITY if diag.stream_requested else None)
        self.c_fb.push(diag.feedback if diag.stream_requested else None)
        if perf:
            self.c_cpu.push(perf.load_percent)
            self.c_cpu_max.push(perf.maximum_us)
        errors = diag.underruns + diag.dropped_packets + diag.overflows + diag.dma_errors
        if self._prev is not None and t > self._prev_t:
            dt = t - self._prev_t
            pk, er = self._prev
            self.c_pkt.push(max(0, diag.packets - pk) / dt)
            self.c_err.push(max(0, errors - er) / dt)
        self._prev, self._prev_t = (diag.packets, errors), t

        s = diag.uptime_ms // 1000
        c = self.counters
        c["Uptime"].setText(f"{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}")
        c["USB packets"].setText(f"{diag.packets:,}")
        c["Packet size (last/min/max)"].setText(f"{diag.packet_last} / {diag.packet_min} / {diag.packet_max} B")
        for key, val in (("Dropped packets", diag.dropped_packets), ("Dropped events", diag.dropped_events),
                         ("Underruns", diag.underruns), ("Overflows", diag.overflows),
                         ("Padding errors", diag.padding_errors), ("DMA errors", diag.dma_errors)):
            c[key].setText(f"<span style='color:{'#E57373' if val else '#81C784'}'>{val:,}</span>")
        c["DMA completions"].setText(f"{diag.dma_completions:,}")
        c["Start step"].setText(diag.start_step)
        c["Last stop"].setText(f"{diag.stop_reason} (at {diag.last_stop_step})")
        if perf:
            c["Refill count"].setText(f"{perf.count:,}")
            c["Max pending refills"].setText(f"{perf.max_pending} of {perf.buffers}")
        if events is not None:
            self.events.clear()
            self.events.addItems(events)
