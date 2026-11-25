"""DykoDAC2 Configurator (PySide6)."""

from __future__ import annotations

import copy
import math
import os
import sys
import time
import traceback
from pathlib import Path

from PySide6.QtCore import QObject, QPointF, QRectF, QSettings, Qt, QThread, QTimer, Signal
from PySide6.QtGui import QAction, QColor, QStandardItem, QStandardItemModel, QFont, QIcon, QPainter, QPainterPath, QPalette, QPen, QPixmap
from PySide6.QtWidgets import (
    QApplication, QButtonGroup, QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog, QFormLayout,
    QFrame, QGridLayout, QGroupBox, QHBoxLayout, QHeaderView, QInputDialog, QLabel, QListWidget,
    QListWidgetItem, QMainWindow, QMenu, QMessageBox, QToolButton,
    QProgressDialog, QPushButton, QRadioButton, QSizePolicy, QSlider, QSpinBox, QSplitter, QTableWidget, QTabWidget,
    QToolBar, QVBoxLayout, QWidget,
)

from . import autoeq, diagnostics, eq_math, presets
from .diag_view import DiagnosticsTab
from . import protocol as p

APP_NAME = "DykoDAC2 Configurator"
ACCENT = QColor("#4FC3F7")
LEFT_COLOR = QColor("#4FC3F7")
RIGHT_COLOR = QColor("#FF8A65")
WARN = "#FFB74D"
GOOD = "#81C784"
BAD = "#E57373"

HEADROOM_PRESETS = [("0 dB (no protection)", 0.0), ("−3.25 dB (default)", -3.25), ("−6 dB", -6.0)]
CHANNEL_NAMES = ["L + R", "Left", "Right"]
CHANNEL_VALUES = [p.CH_BOTH, p.CH_LEFT, p.CH_RIGHT]


def dark_palette() -> QPalette:
    pal = QPalette()
    base, alt, text = QColor("#1E2127"), QColor("#262A31"), QColor("#E6E6E6")
    pal.setColor(QPalette.Window, QColor("#202329"))
    pal.setColor(QPalette.WindowText, text)
    pal.setColor(QPalette.Base, base)
    pal.setColor(QPalette.AlternateBase, alt)
    pal.setColor(QPalette.ToolTipBase, alt)
    pal.setColor(QPalette.ToolTipText, text)
    pal.setColor(QPalette.Text, text)
    pal.setColor(QPalette.Button, QColor("#2C3038"))
    pal.setColor(QPalette.ButtonText, text)
    pal.setColor(QPalette.Highlight, ACCENT.darker(130))
    pal.setColor(QPalette.HighlightedText, QColor("#FFFFFF"))
    pal.setColor(QPalette.PlaceholderText, QColor("#888888"))
    for role in (QPalette.Text, QPalette.ButtonText, QPalette.WindowText):
        pal.setColor(QPalette.Disabled, role, QColor("#6B6F78"))
    return pal


STYLE = """
QGroupBox { border: 1px solid #343842; border-radius: 6px; margin-top: 14px; padding: 8px 8px 6px 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #9FB3C8; font-weight: 600; }
QPushButton { padding: 6px 12px; border-radius: 4px; border: 1px solid #3A3F49; background: #2C3038; }
QPushButton:hover { background: #343A44; }
QPushButton:pressed { background: #3D4450; }
QPushButton#primary { background: #2B5D7A; border-color: #3A7CA3; }
QPushButton#primary:hover { background: #336F92; }
QPushButton:disabled { color: #6B6F78; }
QPushButton#ab:checked { background: #8A5A12; border-color: #FFB74D; color: #FFF; font-weight: 600; }
QToolBar { border: none; spacing: 6px; padding: 4px; }
QTableWidget { gridline-color: #343842; }
QHeaderView::section { background: #262A31; border: none; padding: 4px; color: #9FB3C8; }
QLabel#status { padding: 4px 8px; border-radius: 4px; }
"""


def app_icon() -> QIcon:
    pix = QPixmap(64, 64)
    pix.fill(Qt.transparent)
    pa = QPainter(pix)
    pa.setRenderHint(QPainter.Antialiasing)
    pa.setBrush(QColor("#2B5D7A"))
    pa.setPen(Qt.NoPen)
    pa.drawRoundedRect(2, 2, 60, 60, 14, 14)
    pen = QPen(QColor("#E6F7FF"), 4)
    pa.setPen(pen)
    path = QPainterPath(QPointF(8, 40))
    for x in range(8, 57):
        path.lineTo(x, 34 - 14 * math.sin((x - 8) / 48 * 2 * math.pi) * math.exp(-((x - 32) / 22) ** 2))
    pa.drawPath(path)
    pa.end()
    return QIcon(pix)


# ---------------------------------------------------------------------------
class ResponsePlot(QWidget):
    """Log-frequency magnitude plot of the EQ (per band and total, L and R)."""

    F_MIN, F_MAX = 20.0, 20000.0

    def __init__(self):
        super().__init__()
        self.setMinimumHeight(230)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.settings: p.Settings | None = None
        self.boost = 0.0
        self.selected = -1

    def set_settings(self, s: p.Settings, boost: float, selected: int = -1):
        self.settings, self.boost, self.selected = s, boost, selected
        self.update()

    def _x(self, f, r: QRectF):
        return r.left() + r.width() * math.log10(f / self.F_MIN) / math.log10(self.F_MAX / self.F_MIN)

    def paintEvent(self, _):
        pa = QPainter(self)
        pa.setRenderHint(QPainter.Antialiasing)
        pa.fillRect(self.rect(), QColor("#191B20"))
        r = QRectF(self.rect()).adjusted(44, 12, -12, -26)
        s = self.settings
        bands = [b for b in (s.bands if s else []) if b.type != p.FILTER_OFF] if s and s.eq_enabled else []
        chain = eq_math.Chain(s) if s else None
        # Range adapts to the curves, at least +-12 dB.
        span = 12.0
        if chain:
            for f in (20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000):
                span = max(span, abs(chain.eq_db(f, True)) + 2, abs(chain.eq_db(f, False)) + 2)
        span = min(math.ceil(span / 6) * 6, 30)

        def y(db):
            return r.center().y() - db / span * r.height() / 2

        font = QFont(self.font())
        font.setPointSizeF(8)
        pa.setFont(font)
        grid = QPen(QColor("#2C3038"), 1)
        for f in (20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000):
            x = self._x(f, r)
            pa.setPen(grid)
            pa.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
            pa.setPen(QColor("#7D8590"))
            label = f"{f // 1000}k" if f >= 1000 else str(f)
            pa.drawText(QRectF(x - 20, r.bottom() + 4, 40, 16), Qt.AlignHCenter, label)
        step = 6 if span > 12 else 3
        db = -span
        while db <= span + 0.01:
            pa.setPen(QPen(QColor("#3A3F49" if db == 0 else "#2C3038"), 1))
            pa.drawLine(QPointF(r.left(), y(db)), QPointF(r.right(), y(db)))
            pa.setPen(QColor("#7D8590"))
            pa.drawText(QRectF(0, y(db) - 8, r.left() - 6, 16), Qt.AlignRight | Qt.AlignVCenter, f"{db:+.0f}")
            db += step
        if not chain:
            return
        n = 300
        freqs = [self.F_MIN * (self.F_MAX / self.F_MIN) ** (i / (n - 1)) for i in range(n)]
        # Individual bands.
        for idx, b in enumerate(s.bands):
            if not s.eq_enabled or b.type == p.FILTER_OFF:
                continue
            sel = idx == self.selected
            col = QColor("#FFFFFF" if sel else "#5C6370")
            col.setAlpha(200 if sel else 110)
            pa.setPen(QPen(col, 1.6 if sel else 1, Qt.DashLine if not sel else Qt.SolidLine))
            path = QPainterPath()
            for i, f in enumerate(freqs):
                pt = QPointF(self._x(f, r), y(max(-span, min(span, chain.band_db(b, f)))))
                path.moveTo(pt) if i == 0 else path.lineTo(pt)
            pa.drawPath(path)
        same = chain.left == chain.right
        for left, color in ((True, LEFT_COLOR), (False, RIGHT_COLOR)):
            if not left and same:
                continue
            path = QPainterPath()
            for i, f in enumerate(freqs):
                pt = QPointF(self._x(f, r), y(max(-span, min(span, chain.eq_db(f, left)))))
                path.moveTo(pt) if i == 0 else path.lineTo(pt)
            pa.setPen(QPen(color, 2.4))
            pa.drawPath(path)
        # Band handles.
        for idx, b in enumerate(s.bands):
            if not s.eq_enabled or b.type == p.FILTER_OFF:
                continue
            x = self._x(min(max(b.frequency_hz, self.F_MIN), self.F_MAX), r)
            yy = y(max(-span, min(span, chain.band_db(b, b.frequency_hz))))
            pa.setPen(QPen(QColor("#FFFFFF"), 1))
            pa.setBrush(ACCENT if idx == self.selected else QColor("#3A3F49"))
            pa.drawEllipse(QPointF(x, yy), 8, 8)
            pa.drawText(QRectF(x - 8, yy - 8, 16, 16), Qt.AlignCenter, str(idx + 1))
        pa.setPen(QColor("#9FB3C8"))
        legend = "EQ (L = R)" if same else "Left ━  Right ━"
        pa.drawText(QRectF(r.left() + 6, r.top() + 2, 300, 16), Qt.AlignLeft, legend)
        if self.boost > 0:
            pa.setPen(QColor(WARN))
            pa.drawText(QRectF(r.right() - 330, r.top() + 2, 324, 16), Qt.AlignRight,
                        f"max boost {self.boost:.2f} dB → removed from headroom")


# ---------------------------------------------------------------------------
class SaveWorker(QObject):
    progress = Signal(str)
    finished = Signal(int, str)

    def __init__(self, settings: p.Settings, flashloader: str | None):
        super().__init__()
        self.settings, self.flashloader = settings, flashloader

    def run(self):
        try:
            from . import flash_store
            seq = flash_store.save_settings(self.settings, Path(self.flashloader) if self.flashloader else None,
                                            progress=self.progress.emit)
            self.finished.emit(seq, "")
        except Exception as e:  # noqa: BLE001 - surfaced to the user
            self.finished.emit(-1, str(e) or traceback.format_exc())


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(APP_NAME)
        self.setWindowIcon(app_icon())
        self.resize(1280, 820)
        self.prefs = QSettings("Dyko", "DykoDAC2 Configurator")
        self.dev: p.Device | None = None
        self.info: p.Info | None = None
        self.device_settings: p.Settings | None = None  # what the DAC runs (RAM)
        self.s = p.Settings()                            # what the GUI shows
        self._loading = False
        self._busy = False
        self._ab_sent: bytes | None = None  # packed bypass settings last written for A/B

        self._build_ui()
        self.live_timer = QTimer(self, singleShot=True, interval=180, timeout=self._live_write)
        self.poll = QTimer(self, interval=1000, timeout=self._poll)
        self.poll.start()
        self._last_user_preset = None
        self._reload_presets()
        self._load_into_widgets(self.s)
        QTimer.singleShot(50, self._poll)

    # --- UI construction -----------------------------------------------------
    def _build_ui(self):
        tb = QToolBar("Device")
        tb.setMovable(False)
        self.addToolBar(tb)

        def btn(text, tip, slot, primary=False):
            b = QPushButton(text)
            b.setToolTip(tip)
            if primary:
                b.setObjectName("primary")
            b.clicked.connect(slot)
            tb.addWidget(b)
            return b

        self.b_read = btn("⟲  Read", "Read the settings the DAC is running (RAM)", self.on_read)
        self.b_write = btn("⇪  Write to RAM", "Apply these settings now (lost at power-off unless saved)",
                           self.on_write, primary=True)
        tb.addSeparator()
        self.b_save = btn("💾  Save to flash", "Store the DAC's running settings permanently.\n"
                          "The DAC briefly reboots (≈5 s, audio stops).", self.on_save)
        self.b_load = btn("⤓  Load from flash", "Discard RAM changes and run the saved settings", self.on_load)
        self.b_defaults = btn("↺  Defaults", "Factory defaults into RAM (flash unchanged until Save)", self.on_defaults)
        tb.addSeparator()
        self.live = QCheckBox("Live apply")
        self.live.setToolTip("Write every change to the DAC's RAM immediately")
        self.live.setChecked(self.prefs.value("live", True, bool))
        self.live.toggled.connect(lambda v: self.prefs.setValue("live", v))
        tb.addWidget(self.live)
        spacer = QWidget()
        spacer.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        tb.addWidget(spacer)
        self.conn = QLabel("Searching…")
        self.conn.setObjectName("status")
        tb.addWidget(self.conn)

        menu = self.menuBar()
        fm = menu.addMenu("&File")
        for text, slot in (("Import AutoEQ / Equalizer APO…", self.on_import_autoeq),
                           ("Export EQ as Equalizer APO…", self.on_export_apo), (None, None),
                           ("Import EQ preset file…", self.on_open_preset),
                           ("Export EQ preset file…", self.on_save_preset),
                           (None, None), ("Quit", self.close)):
            if text is None:
                fm.addSeparator()
                continue
            a = QAction(text, self)
            a.triggered.connect(slot)
            fm.addAction(a)
        hm = menu.addMenu("&Help")
        about = QAction("About", self)
        about.triggered.connect(lambda: QMessageBox.about(self, APP_NAME, ABOUT_TEXT))
        hm.addAction(about)

        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)
        outer.setContentsMargins(10, 6, 10, 6)

        # Device status strip.
        self.devline = QLabel("")
        self.devline.setTextFormat(Qt.RichText)
        self.devline.setStyleSheet("color:#B8C2CC; padding: 2px 4px;")
        outer.addWidget(self.devline)

        self.tabs = QTabWidget()
        outer.addWidget(self.tabs, 1)
        split = QSplitter(Qt.Horizontal)
        self.tabs.addTab(split, "Settings")
        self.diag_tab = DiagnosticsTab()
        self.tabs.addTab(self.diag_tab, "Diagnostics")
        self.tabs.currentChanged.connect(self._tab_changed)
        self._last_events = 0.0

        # ---- left column: output path
        left = QWidget()
        lv = QVBoxLayout(left)
        lv.setContentsMargins(0, 0, 6, 0)

        g = QGroupBox("Reconstruction")
        gl = QVBoxLayout(g)
        self.r_4x = QRadioButton("MCU 4× interpolator → 176.4 / 192 kHz  (default)")
        self.r_bypass = QRadioButton("Upsampler bypass → 44.1 / 48 kHz straight to the CS4398")
        self.r_4x.setToolTip("8192-tap linear-phase two-stage FFT interpolator in the MCU")
        self.r_bypass.setToolTip("Lets the CS4398's own digital filter do the oversampling.\n"
                                 "Bit-perfect when volume = 0 dB, headroom = 0 dB and no EQ/crossfeed/balance.")
        grp = QButtonGroup(self)
        grp.addButton(self.r_4x)
        grp.addButton(self.r_bypass)
        gl.addWidget(self.r_4x)
        gl.addWidget(self.r_bypass)
        fl = QFormLayout()
        self.c_filter = QComboBox()
        self.c_filter.addItems(["Fast roll-off", "Slow roll-off"])
        self.c_filter.setToolTip("CS4398 internal interpolation filter (FILT_SEL). Matters most in bypass.")
        fl.addRow("CS4398 filter", self.c_filter)
        gl.addLayout(fl)
        lv.addWidget(g)

        g = QGroupBox("Level budget")
        gf = QFormLayout(g)
        hb = QHBoxLayout()
        self.c_headroom = QComboBox()
        for name, _v in HEADROOM_PRESETS:
            self.c_headroom.addItem(name)
        self.c_headroom.addItem("Custom")
        self.s_headroom = QDoubleSpinBox(minimum=p.MIN_HEADROOM_DB, maximum=0.0, singleStep=0.25, decimals=2,
                                         suffix=" dB")
        hb.addWidget(self.c_headroom, 1)
        hb.addWidget(self.s_headroom)
        gf.addRow("Digital headroom", hb)
        self.l_effective = QLabel()
        self.l_effective.setTextFormat(Qt.RichText)
        self.l_effective.setWordWrap(True)
        gf.addRow("", self.l_effective)
        self.s_ceiling = QDoubleSpinBox(minimum=p.MIN_CEILING_DB, maximum=0.0, singleStep=0.5, decimals=1,
                                        suffix=" dB")
        self.s_ceiling.setToolTip("CS4398 analog attenuation (0.5 dB steps): the hard maximum output level.\n"
                                  "Protects headphones and ears from faults: the TPA6120 has lots of gain.")
        gf.addRow("DAC ceiling", self.s_ceiling)
        self.l_ceiling_warn = QLabel()
        self.l_ceiling_warn.setWordWrap(True)
        gf.addRow("", self.l_ceiling_warn)
        lv.addWidget(g)

        g = QGroupBox("Channels")
        gf = QFormLayout(g)
        bal = QHBoxLayout()
        self.sl_balance = QSlider(Qt.Horizontal, minimum=-48, maximum=48)  # 0.25 dB steps
        self.sl_balance.setTickInterval(8)
        self.sl_balance.setTickPosition(QSlider.TicksBelow)
        self.l_balance = QLabel()
        self.l_balance.setMinimumWidth(110)
        b0 = QPushButton("Center")
        b0.clicked.connect(lambda: self.sl_balance.setValue(0))
        bal.addWidget(QLabel("L"))
        bal.addWidget(self.sl_balance, 1)
        bal.addWidget(QLabel("R"))
        bal.addWidget(self.l_balance)
        bal.addWidget(b0)
        gf.addRow("Balance", bal)
        pol = QHBoxLayout()
        self.cb_inv_l = QCheckBox("Invert left")
        self.cb_inv_r = QCheckBox("Invert right")
        pol.addWidget(self.cb_inv_l)
        pol.addWidget(self.cb_inv_r)
        pol.addStretch()
        gf.addRow("Polarity", pol)
        self.c_xf = QComboBox()
        self.c_xf.addItems(p.CROSSFEED_NAMES)
        self.c_xf.setToolTip("Headphone crossfeed (Bauer bs2b): blends a low-passed, delayed copy of each\n"
                             "channel into the other, so hard-panned recordings sound less fatiguing.")
        gf.addRow("Crossfeed", self.c_xf)
        lv.addWidget(g)

        g = QGroupBox("Loudness")
        gf = QFormLayout(g)
        lev = QHBoxLayout()
        self.c_leveler = QComboBox()
        self.c_leveler.addItems(p.LEVELER_NAMES)
        self.c_leveler.setToolTip(
            "Automatic leveler: measures loudness (EBU R128 / BS.1770) and slowly turns loud\n"
            "material down to the target. It never turns anything up, so it cannot clip.\n"
            "Speed = attack / release time. Silence and track gaps hold the current level.")
        self.s_lev_target = QDoubleSpinBox(minimum=p.MIN_LEVELER_TARGET, maximum=p.MAX_LEVELER_TARGET,
                                           singleStep=1.0, decimals=1, suffix=" LUFS")
        self.s_lev_target.setToolTip("Loudness the leveler turns loud material down to.\n"
                                     "Modern pop/rock masters are about −8..−12 LUFS, older or classical −16..−24.")
        self.s_lev_cut = QSpinBox(minimum=p.MIN_LEVELER_CUT, maximum=p.MAX_LEVELER_CUT, suffix=" dB")
        self.s_lev_cut.setToolTip("Largest level reduction the leveler may apply.")
        lev.addWidget(self.c_leveler, 1)
        lev.addWidget(self.s_lev_target)
        lev.addWidget(QLabel("max cut"))
        lev.addWidget(self.s_lev_cut)
        gf.addRow("Leveler", lev)
        self.l_leveler = QLabel()
        gf.addRow("", self.l_leveler)
        loud = QHBoxLayout()
        self.cb_loud = QCheckBox("On")
        self.cb_loud.setToolTip(
            "Loudness compensation (ISO 226 equal-loudness contours): below the reference volume\n"
            "the ear loses bass (and some treble), so both are lifted as the host volume goes down.\n"
            "The lift never exceeds the volume reduction, so it cannot clip.")
        self.s_loud_ref = QDoubleSpinBox(minimum=p.MIN_LOUDNESS_REFERENCE, maximum=0.0, singleStep=1.0,
                                         decimals=1, suffix=" dB")
        self.s_loud_ref.setToolTip("Host volume at which you hear music at its intended (loud) level:\n"
                                   "flat here and above, compensated below.")
        self.b_loud_ref = QPushButton("Use current")
        self.b_loud_ref.setToolTip("Take the host volume the DAC is set to right now as the reference")
        self.b_loud_ref.clicked.connect(self._loudness_reference_from_volume)
        loud.addWidget(self.cb_loud)
        loud.addWidget(QLabel("reference"))
        loud.addWidget(self.s_loud_ref)
        loud.addWidget(self.b_loud_ref)
        loud.addStretch()
        gf.addRow("Compensation", loud)
        loud2 = QHBoxLayout()
        self.s_loud_amount = QSpinBox(minimum=p.MIN_LOUDNESS_AMOUNT, maximum=p.MAX_LOUDNESS_AMOUNT,
                                      singleStep=5, suffix=" %")
        self.s_loud_amount.setToolTip("Strength relative to the ISO 226 contours (100 % = full correction).")
        self.s_loud_boost = QSpinBox(minimum=p.MIN_LOUDNESS_BOOST, maximum=p.MAX_LOUDNESS_BOOST, suffix=" dB")
        self.s_loud_boost.setToolTip("Largest bass lift (the treble lift is scaled with it).")
        loud2.addWidget(QLabel("amount"))
        loud2.addWidget(self.s_loud_amount)
        loud2.addWidget(QLabel("max bass lift"))
        loud2.addWidget(self.s_loud_boost)
        loud2.addStretch()
        gf.addRow("", loud2)
        self.l_loud = QLabel()
        self.l_loud.setWordWrap(True)
        gf.addRow("", self.l_loud)
        lv.addWidget(g)
        lv.addStretch()
        split.addWidget(left)

        # ---- right column: EQ
        right = QWidget()
        rv = QVBoxLayout(right)
        rv.setContentsMargins(6, 0, 0, 0)
        g = QGroupBox("Parametric EQ  (float, before the single dithered 24-bit quantisation)")
        gv = QVBoxLayout(g)
        top = QHBoxLayout()
        self.cb_eq = QCheckBox("Enable EQ")
        top.addWidget(self.cb_eq)
        self.b_ab = QPushButton("A/B: bypass EQ")
        self.b_ab.setObjectName("ab")
        self.b_ab.setCheckable(True)
        self.b_ab.setShortcut("Ctrl+B")
        self.b_ab.setToolTip("Hold-free A/B test (Ctrl+B): the DAC plays without EQ but at the same level\n"
                             "reduction the EQ causes, so loudness doesn't bias the comparison.\n"
                             "Only in the DAC's RAM; never saved. Click again to switch the EQ back on.")
        self.b_ab.toggled.connect(self._ab_toggled)
        top.addWidget(self.b_ab)
        top.addSpacing(18)
        top.addWidget(QLabel("Preset"))
        self.preset_combo = QComboBox()
        self.preset_combo.setMinimumWidth(300)
        self.preset_combo.setMaxVisibleItems(24)
        self.preset_combo.activated.connect(self._preset_chosen)
        top.addWidget(self.preset_combo, 1)
        self.preset_menu_button = QToolButton()
        self.preset_menu_button.setText("Presets ▾")
        self.preset_menu_button.setPopupMode(QToolButton.InstantPopup)
        self.preset_menu_button.setToolTip("Manage EQ presets (stored on this PC)")
        self.preset_menu = QMenu(self)
        self.preset_menu.aboutToShow.connect(self._fill_preset_menu)
        self.preset_menu_button.setMenu(self.preset_menu)
        top.addWidget(self.preset_menu_button)
        b = QPushButton("Import AutoEQ…")
        b.setToolTip("Load an AutoEQ / Equalizer APO ParametricEQ.txt (e.g. from autoeq.app)")
        b.clicked.connect(self.on_import_autoeq)
        top.addWidget(b)
        gv.addLayout(top)
        self.preset_desc = QLabel()
        self.preset_desc.setWordWrap(True)
        self.preset_desc.setStyleSheet("color:#9AA5B1; padding: 0 2px 2px 2px;")
        gv.addWidget(self.preset_desc)
        self.plot = ResponsePlot()
        gv.addWidget(self.plot, 3)
        self.table = QTableWidget(p.EQ_BANDS, 5)
        self.table.setHorizontalHeaderLabels(["Type", "Channels", "Frequency", "Gain", "Q"])
        self.table.verticalHeader().setDefaultSectionSize(30)
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self.table.setSelectionMode(QTableWidget.NoSelection)
        self.table.setFocusPolicy(Qt.NoFocus)
        self.band_widgets = []
        for i in range(p.EQ_BANDS):
            t = QComboBox()
            t.addItems(p.FILTER_NAMES)
            ch = QComboBox()
            ch.addItems(CHANNEL_NAMES)
            fr = QDoubleSpinBox(minimum=p.MIN_FREQ, maximum=p.MAX_FREQ, decimals=1, suffix=" Hz")
            fr.setStepType(QDoubleSpinBox.AdaptiveDecimalStepType)
            ga = QDoubleSpinBox(minimum=-p.MAX_GAIN, maximum=p.MAX_GAIN, singleStep=0.1, decimals=2, suffix=" dB")
            q = QDoubleSpinBox(minimum=p.MIN_Q, maximum=p.MAX_Q, singleStep=0.05, decimals=3)
            for col, w in enumerate((t, ch, fr, ga, q)):
                self.table.setCellWidget(i, col, w)
                w.installEventFilter(self)
                w.setProperty("band", i)
            self.band_widgets.append((t, ch, fr, ga, q))
            t.currentIndexChanged.connect(self._changed)
            ch.currentIndexChanged.connect(self._changed)
            for w in (fr, ga, q):
                w.valueChanged.connect(self._changed)
        gv.addWidget(self.table, 2)
        rv.addWidget(g)
        split.addWidget(right)
        split.setStretchFactor(0, 2)
        split.setStretchFactor(1, 3)

        self.statusBar().showMessage("Ready")

        for w in (self.r_4x, self.r_bypass, self.cb_inv_l, self.cb_inv_r, self.cb_eq):
            w.toggled.connect(self._changed)
        for w in (self.c_filter, self.c_xf, self.c_leveler):
            w.currentIndexChanged.connect(self._changed)
        self.cb_loud.toggled.connect(self._changed)
        for w in (self.s_lev_target, self.s_lev_cut, self.s_loud_ref, self.s_loud_amount, self.s_loud_boost):
            w.valueChanged.connect(self._changed)
        self.c_headroom.currentIndexChanged.connect(self._headroom_preset)
        self.s_headroom.valueChanged.connect(self._changed)
        self.s_ceiling.valueChanged.connect(self._changed)
        self.sl_balance.valueChanged.connect(self._changed)
        self.selected_band = -1

    # --- EQ presets (PC only; the DAC stores exactly one settings set) ------
    def _reload_presets(self):
        self._builtin = list(presets.BUILTIN)
        self._user = presets.load_user()
        model = QStandardItemModel()

        def header(text):
            it = QStandardItem(text)
            it.setFlags(Qt.NoItemFlags)
            f = it.font()
            f.setBold(True)
            it.setFont(f)
            it.setForeground(QColor("#9FB3C8"))
            model.appendRow(it)

        def entry(pr, key):
            it = QStandardItem("    " + pr.name)
            it.setData(key, Qt.UserRole)
            it.setToolTip(pr.description)
            model.appendRow(it)

        custom = QStandardItem("Custom (edited)")
        custom.setData(None, Qt.UserRole)
        custom.setFlags(Qt.ItemIsEnabled)
        custom.setForeground(QColor("#FFB74D"))
        model.appendRow(custom)
        header("Built-in")
        for i, pr in enumerate(self._builtin):
            entry(pr, ("b", i))
        header("My presets")
        if not self._user:
            it = QStandardItem("    (none yet: Presets \u25be \u2192 Save as new)")
            it.setFlags(Qt.NoItemFlags)
            it.setForeground(QColor("#6B6F78"))
            model.appendRow(it)
        for i, pr in enumerate(self._user):
            entry(pr, ("u", i))
        self.preset_combo.setModel(model)
        self._sync_preset_combo()

    def _preset_for(self, key):
        if not key:
            return None
        kind, i = key
        return (self._builtin if kind == "b" else self._user)[i]

    def _active_preset(self):
        """The preset whose EQ the panel shows right now (user presets win)."""
        for i, pr in enumerate(self._user):
            if pr.matches(self.s):
                return ("u", i), pr
        for i, pr in enumerate(self._builtin):
            if pr.matches(self.s):
                return ("b", i), pr
        return None, None

    def _sync_preset_combo(self):
        if not hasattr(self, "_builtin"):
            return
        key, pr = self._active_preset()
        model = self.preset_combo.model()
        row = 0
        for r in range(model.rowCount()):
            if model.item(r).data(Qt.UserRole) == key and key is not None:
                row = r
                break
        self.preset_combo.blockSignals(True)
        self.preset_combo.setCurrentIndex(row)
        self.preset_combo.blockSignals(False)
        self.preset_desc.setText(pr.description if pr else
                                 "Custom EQ. Save it with Presets \u25be \u2192 Save as new\u2026")

    def _preset_chosen(self, row):
        key = self.preset_combo.model().item(row).data(Qt.UserRole)
        pr = self._preset_for(key)
        if not pr:
            self._sync_preset_combo()
            return
        if key[0] == "u":
            self._last_user_preset = pr.name
        self._load_into_widgets(pr.apply_to(self.s))
        self._changed()
        tail = (" and applied to the DAC." if self.live.isChecked() and self.dev
                else ". Press Write to RAM to send it to the DAC.")
        self.statusBar().showMessage(f"EQ preset '{pr.name}' loaded" + tail, 5000)

    def _fill_preset_menu(self):
        m = self.preset_menu
        m.clear()
        key, pr = self._active_preset()
        user = pr if key and key[0] == "u" else None
        m.addAction("Save as new\u2026", self.on_preset_save_as)
        a = m.addAction(f"Overwrite '{user.name}'" if user else "Overwrite (select one of My presets)",
                        self.on_preset_overwrite)
        a.setEnabled(user is not None)
        # Overwrite targets the last chosen user preset even after edits.
        if user is None and getattr(self, "_last_user_preset", None):
            a.setText(f"Overwrite '{self._last_user_preset}' with this EQ")
            a.setEnabled(True)
        a = m.addAction(f"Rename '{user.name}'\u2026" if user else "Rename\u2026", self.on_preset_rename)
        a.setEnabled(user is not None)
        a = m.addAction(f"Delete '{user.name}'" if user else "Delete", self.on_preset_delete)
        a.setEnabled(user is not None)
        m.addSeparator()
        m.addAction("Import preset file\u2026", self.on_open_preset)
        m.addAction("Export this EQ to file\u2026", self.on_save_preset)
        m.addAction("Export as Equalizer APO .txt\u2026", self.on_export_apo)
        m.addSeparator()
        m.addAction("Open presets folder", self.on_preset_folder)

    def on_preset_save_as(self):
        key, pr = self._active_preset()
        name, ok = QInputDialog.getText(self, "Save EQ preset", "Preset name:",
                                        text=pr.name if pr and not pr.builtin else "My EQ")
        name = name.strip()
        if not ok or not name:
            return
        if presets.path_for(name).exists() and QMessageBox.question(
                self, APP_NAME, f"A preset called '{name}' exists. Replace it?") != QMessageBox.Yes:
            return
        presets.save_user(presets.Preset(name, copy.deepcopy(self.s.bands), self.s.eq_enabled))
        self._last_user_preset = name
        self._reload_presets()
        self.statusBar().showMessage(f"Saved EQ preset '{name}'.", 4000)

    def on_preset_overwrite(self):
        key, pr = self._active_preset()
        name = pr.name if key and key[0] == "u" else getattr(self, "_last_user_preset", None)
        if not name:
            return
        if QMessageBox.question(self, APP_NAME, f"Replace preset '{name}' with the current EQ?") != QMessageBox.Yes:
            return
        old = next((u for u in self._user if u.name == name), None)
        presets.save_user(presets.Preset(name, copy.deepcopy(self.s.bands), self.s.eq_enabled,
                                         old.description if old else ""))
        self._reload_presets()

    def on_preset_rename(self):
        key, pr = self._active_preset()
        if not key or key[0] != "u":
            return
        name, ok = QInputDialog.getText(self, "Rename EQ preset", "New name:", text=pr.name)
        name = name.strip()
        if ok and name and name != pr.name:
            presets.rename_user(pr.name, name)
            self._last_user_preset = name
            self._reload_presets()

    def on_preset_delete(self):
        key, pr = self._active_preset()
        if key and key[0] == "u" and QMessageBox.question(
                self, APP_NAME, f"Delete EQ preset '{pr.name}'?\n(The EQ on screen stays as it is.)") == QMessageBox.Yes:
            presets.delete_user(pr.name)
            self._last_user_preset = None
            self._reload_presets()

    def on_preset_folder(self):
        os.startfile(str(presets.user_dir()))

    def eventFilter(self, obj, ev):
        if ev.type() == ev.Type.FocusIn and obj.property("band") is not None:
            self.selected_band = int(obj.property("band"))
            self._refresh_derived()
        return super().eventFilter(obj, ev)

    # --- settings <-> widgets ------------------------------------------------
    def _load_into_widgets(self, s: p.Settings):
        self._loading = True
        try:
            (self.r_bypass if s.upsampler_bypass else self.r_4x).setChecked(True)
            self.c_filter.setCurrentIndex(1 if s.dac_slow_rolloff else 0)
            idx = next((i for i, (_n, v) in enumerate(HEADROOM_PRESETS) if abs(v - s.headroom_db) < 1e-6),
                       len(HEADROOM_PRESETS))
            self.c_headroom.setCurrentIndex(idx)
            self.s_headroom.setValue(s.headroom_db)
            self.s_headroom.setEnabled(idx == len(HEADROOM_PRESETS))
            self.s_ceiling.setValue(s.dac_ceiling_db)
            self.sl_balance.setValue(int(round(s.balance_db * 4)))
            self.cb_inv_l.setChecked(s.invert_left)
            self.cb_inv_r.setChecked(s.invert_right)
            self.c_xf.setCurrentIndex(s.crossfeed)
            self.c_leveler.setCurrentIndex(s.leveler_mode)
            self.s_lev_target.setValue(s.leveler_target_db)
            self.s_lev_cut.setValue(int(s.leveler_max_cut_db))
            self.cb_loud.setChecked(s.loudness_enabled)
            self.s_loud_ref.setValue(s.loudness_reference_db)
            self.s_loud_amount.setValue(int(s.loudness_amount_pct))
            self.s_loud_boost.setValue(int(s.loudness_max_boost_db))
            self.cb_eq.setChecked(s.eq_enabled)
            for (t, ch, fr, ga, q), b in zip(self.band_widgets, s.bands):
                t.setCurrentIndex(b.type)
                ch.setCurrentIndex(CHANNEL_VALUES.index(b.channels) if b.channels in CHANNEL_VALUES else 0)
                fr.setValue(b.frequency_hz)
                ga.setValue(b.gain_db)
                q.setValue(b.q)
        finally:
            self._loading = False
        self.s = s
        self._refresh_derived()

    def _from_widgets(self) -> p.Settings:
        s = p.Settings()
        s.upsampler_bypass = self.r_bypass.isChecked()
        s.dac_slow_rolloff = self.c_filter.currentIndex() == 1
        s.headroom_db = round(self.s_headroom.value() * 256) / 256
        s.dac_ceiling_db = self.s_ceiling.value()
        s.balance_db = self.sl_balance.value() / 4.0
        s.invert_left = self.cb_inv_l.isChecked()
        s.invert_right = self.cb_inv_r.isChecked()
        s.crossfeed = self.c_xf.currentIndex()
        s.leveler_mode = self.c_leveler.currentIndex()
        s.leveler_target_db = round(self.s_lev_target.value() * 256) / 256
        s.leveler_max_cut_db = self.s_lev_cut.value()
        s.loudness_enabled = self.cb_loud.isChecked()
        s.loudness_reference_db = round(self.s_loud_ref.value() * 256) / 256
        s.loudness_amount_pct = self.s_loud_amount.value()
        s.loudness_max_boost_db = self.s_loud_boost.value()
        s.eq_enabled = self.cb_eq.isChecked()
        s.bands = []
        for t, ch, fr, ga, q in self.band_widgets:
            s.bands.append(p.EqBand(t.currentIndex(), CHANNEL_VALUES[ch.currentIndex()], fr.value(), ga.value(), q.value()))
        return s

    def _loudness_reference_from_volume(self):
        if not self.info:
            self.statusBar().showMessage("Connect the DAC to read its current volume.", 3000)
            return
        self.s_loud_ref.setValue(max(p.MIN_LOUDNESS_REFERENCE, min(0.0, self.info.host_volume_db)))

    def _render_loudness(self):
        s, i = self.s, self.info
        if s.leveler_mode and i is not None:
            self.l_leveler.setText(f"Now: {i.leveler_gain_db:+.1f} dB")
        else:
            self.l_leveler.setText("")
        if not s.loudness_enabled:
            self.l_loud.setText("")
            return
        if i is None:
            self.l_loud.setText("Flat at and above the reference volume.")
            return
        vol = i.host_volume_db
        # What the panel would do at the current volume (the DAC's own values
        # below follow once it has been written).
        att = max(0.0, s.loudness_reference_db - vol)
        bass = p.LOUDNESS_BASS_PER_DB * s.loudness_amount_pct / 100.0 * att
        treble = p.LOUDNESS_TREBLE_PER_DB * s.loudness_amount_pct / 100.0 * att
        if bass > s.loudness_max_boost_db:
            treble *= s.loudness_max_boost_db / bass
            bass = s.loudness_max_boost_db
        self.l_loud.setText(f"At host volume {vol:+.1f} dB: bass +{bass:.1f} dB (111 Hz shelf), "
                            f"treble +{treble:.1f} dB (10 kHz shelf)")

    def _headroom_preset(self, idx):
        if idx < len(HEADROOM_PRESETS):
            self._loading = True
            self.s_headroom.setValue(HEADROOM_PRESETS[idx][1])
            self._loading = False
        self.s_headroom.setEnabled(idx == len(HEADROOM_PRESETS))
        self._changed()

    def _changed(self, *_):
        if self._loading:
            return
        self.s = self._from_widgets()
        self._refresh_derived()
        if self.live.isChecked() and self.dev and not self._busy:
            self.live_timer.start()

    def _refresh_derived(self):
        s = self.s
        boost = eq_math.Chain(s).boost_db()
        self.plot.set_settings(s, boost, self.selected_band)
        self._sync_preset_combo()
        eff = s.headroom_db - boost
        txt = (f"Effective headroom: <b>{eff:+.2f} dB</b>"
               f" = {s.headroom_db:+.2f} dB set − {boost:.2f} dB EQ/crossfeed boost (automatic)")
        if s.headroom_db == 0 and boost == 0:
            txt += f"<br><span style='color:{WARN}'>0 dB: inter-sample peaks of loud masters will clip.</span>"
        self.l_effective.setText(txt)
        self.l_ceiling_warn.setText(
            f"<span style='color:{WARN}'>Above −6 dB: check your headphones can take full output.</span>"
            if s.dac_ceiling_db > -6 else "")
        self.l_balance.setText("centre" if s.balance_db == 0 else
                               f"{abs(s.balance_db):.2f} dB {'→ R' if s.balance_db > 0 else '← L'}")
        for w in (self.s_lev_target, self.s_lev_cut):
            w.setEnabled(s.leveler_mode != 0)
        for w in (self.s_loud_ref, self.b_loud_ref, self.s_loud_amount, self.s_loud_boost):
            w.setEnabled(s.loudness_enabled)
        self._render_loudness()
        for i, (t, ch, fr, ga, q) in enumerate(self.band_widgets):
            active = t.currentIndex() != p.FILTER_OFF
            gain_used = t.currentIndex() in (p.FILTER_PEAKING, p.FILTER_LOW_SHELF, p.FILTER_HIGH_SHELF)
            for w in (ch, fr, q):
                w.setEnabled(active)
            ga.setEnabled(active and gain_used)
        self._update_buttons()

    # --- device ----------------------------------------------------------------
    def _set_conn(self, text, color):
        self.conn.setText(text)
        self.conn.setStyleSheet(f"background:{color}; color:#111; font-weight:600;")

    def _disconnect(self, why=""):
        if self.dev:
            self.dev.close()
        self.dev, self.info, self.device_settings = None, None, None
        self._ab_set(False)  # _ab_sent is kept so a reconnect can restore the EQ
        self._set_conn("Not connected", BAD)
        self.devline.setText(why or "Plug in the DykoDAC2 (USB). It is detected automatically.")
        self._update_buttons()

    def _connect(self) -> bool:
        try:
            self.dev = p.Device()
            self.info = self.dev.info()
        except Exception as e:  # noqa: BLE001
            if self.dev:
                self.dev.close()
            self.dev = None
            self._disconnect(f"<span style='color:{BAD}'>{e}</span>" if "not found" not in str(e) else "")
            return False
        self._set_conn("Connected", GOOD)
        self.on_read(quiet=True)
        return True

    def _tab_changed(self, index):
        # Faster sampling while the diagnostics graphs are visible.
        self.poll.setInterval(500 if index == 1 else 1000)

    def _poll(self):
        if self._busy:
            return
        if not self.dev:
            self._connect()
            if not self.dev:
                self.diag_tab.offline()
            return
        try:
            self.info = self.dev.info()
            if self.tabs.currentIndex() == 1:
                now = time.monotonic()
                diag = diagnostics.decode_diag(
                    diagnostics.query(self.dev, diagnostics.DIAG_COMMAND, diagnostics.DIAG_MAGIC) or b"")
                perf = diagnostics.decode_perf(
                    diagnostics.query(self.dev, diagnostics.PERF_COMMAND, diagnostics.PERF_MAGIC) or b"")
                events = None
                if now - self._last_events > 2.0:
                    self._last_events = now
                    events = diagnostics.decode_events(
                        diagnostics.query(self.dev, diagnostics.EVENT_COMMAND, diagnostics.EVENT_MAGIC) or b"")
                self.diag_tab.update_from(diag, perf, events, now)
        except Exception as e:  # noqa: BLE001
            self._disconnect(f"<span style='color:{BAD}'>Connection lost: {e}</span>")
            self.diag_tab.offline()
            return
        self._render_info()

    def _render_info(self):
        i = self.info
        if not i:
            return
        parts = [f"<b>{i.app_state}</b>" + (" · streaming" if i.streaming else "")]
        if i.source_rate_hz:
            mode = "bypass → CS4398 filter" if i.upsampler_bypass_active else "4× MCU interpolator"
            parts.append(f"{i.source_rate_hz / 1000:g} kHz → {i.output_rate_hz / 1000:g} kHz ({mode})")
            if i.bit_exact:
                parts.append(f"<span style='color:{GOOD}'>bit-perfect</span>")
        parts.append(f"host volume {i.host_volume_db:+.1f} dB")
        parts.append(f"effective headroom {i.effective_headroom_db:+.2f} dB")
        if i.leveler_gain_db < -0.05:
            parts.append(f"leveler {i.leveler_gain_db:+.1f} dB")
        if i.loudness_bass_db > 0.05:
            parts.append(f"loudness +{i.loudness_bass_db:.1f}/+{i.loudness_treble_db:.1f} dB")
        if i.flash_record:
            flash = (f"<span style='color:{WARN}'>RAM differs from flash</span>" if i.unsaved
                     else f"<span style='color:{GOOD}'>saved (record #{i.flash_sequence})</span>")
        else:
            flash = f"<span style='color:{WARN}'>nothing saved in flash yet</span>"
        parts.append(flash)
        self.devline.setText("  ·  ".join(parts))
        self._render_loudness()
        self._update_buttons()

    def _update_buttons(self):
        on = self.dev is not None and not self._busy
        for b in (self.b_read, self.b_write, self.b_save, self.b_load, self.b_defaults, self.b_ab):
            b.setEnabled(on)
        pending = self.device_settings is not None and self.device_settings.pack() != self.s.pack()
        self.b_write.setText("⇪  Write to RAM" + ("  •" if pending else ""))
        title = APP_NAME + ("  — unapplied changes" if pending else "")
        self.setWindowTitle(title)

    def _guard(self, what, fn):
        if not self.dev:
            return None
        try:
            return fn()
        except p.DeviceError as e:
            QMessageBox.warning(self, APP_NAME, f"{what}: {e}")
        except Exception as e:  # noqa: BLE001
            self._disconnect(f"<span style='color:{BAD}'>{what} failed: {e}</span>")
        return None

    # --- A/B: EQ bypass at matched level (RAM only) -------------------------------
    def _for_device(self) -> p.Settings:
        """What to send: the panel, or while A/B is on the panel without EQ but with
        the level drop the EQ's boost would cause moved into the headroom."""
        if not self.b_ab.isChecked():
            return self.s
        flat = copy.deepcopy(self.s)
        flat.eq_enabled = False
        drop = eq_math.Chain(self.s).boost_db() - eq_math.Chain(flat).boost_db()
        flat.headroom_db = max(p.MIN_HEADROOM_DB, round((self.s.headroom_db - drop) * 256) / 256)
        return flat

    def _ab_set(self, on: bool):
        self.b_ab.blockSignals(True)
        self.b_ab.setChecked(on)
        self.b_ab.blockSignals(False)
        self.b_ab.setText("A/B: EQ BYPASSED" if on else "A/B: bypass EQ")

    def _ab_toggled(self, on: bool):
        self._ab_set(on)
        if not self.dev or self._busy:
            self._ab_set(False)
            return
        if not self._write():
            self._ab_set(not on)
            return
        if on:
            f = self._for_device()
            self.statusBar().showMessage(f"A/B: EQ bypassed at matched level (headroom {f.headroom_db:+.2f} dB, "
                                         "RAM only). Click again or Ctrl+B to switch the EQ back on.")
        else:
            self.statusBar().showMessage("A/B: EQ back on.", 3000)

    def on_read(self, quiet=False):
        s = self._guard("Read", self.dev.read_settings) if self.dev else None
        if s and self._ab_sent is not None and s.pack() == self._ab_sent:
            # The DAC is running our A/B bypass: keep the panel's real settings.
            self.device_settings = copy.deepcopy(self.s)
            if not self.b_ab.isChecked():  # A/B was dropped (e.g. reconnect): restore the EQ
                self._write()
            self._poll()
            return
        if s:
            self._ab_set(False)
            self._ab_sent = None
            self.device_settings = copy.deepcopy(s)
            self._load_into_widgets(s)
            if not quiet:
                self.statusBar().showMessage("Read the running settings from the DAC.", 4000)
            self._poll()

    def _write(self) -> bool:
        problems = self.s.problems()
        if problems:
            QMessageBox.warning(self, APP_NAME, "\n".join(problems))
            return False
        out = self._for_device()
        ok = self._guard("Write", lambda: self.dev.write_settings(out) or True)
        if ok:
            self._ab_sent = out.pack() if out is not self.s else None
            self.device_settings = copy.deepcopy(self.s)
            self._poll()
        return bool(ok)

    def on_write(self):
        if self._write():
            self.statusBar().showMessage("Applied to the DAC (RAM). Use Save to keep it after power-off.", 5000)

    def _live_write(self):
        if self.dev and not self._busy and not self.s.problems():
            if self._write():
                self.statusBar().showMessage("Live: applied", 1500)

    def on_load(self):
        self._ab_set(False)
        self._ab_sent = None
        s = self._guard("Load from flash", self.dev.load)
        if s:
            self.device_settings = copy.deepcopy(s)
            self._load_into_widgets(s)
            self.statusBar().showMessage("Loaded the saved settings from flash.", 4000)

    def on_defaults(self):
        if QMessageBox.question(self, APP_NAME, "Put factory defaults into the DAC's RAM?\n"
                                "(Flash keeps your saved settings until you press Save.)") != QMessageBox.Yes:
            return
        self._ab_set(False)
        self._ab_sent = None
        s = self._guard("Defaults", self.dev.defaults)
        if s:
            self.device_settings = copy.deepcopy(s)
            self._load_into_widgets(s)
            self.statusBar().showMessage("Factory defaults applied to RAM.", 4000)

    def on_save(self):
        if self.b_ab.isChecked():
            self.b_ab.setChecked(False)  # never save the A/B bypass: put the EQ back first
            if self.b_ab.isChecked():
                return
        if self.device_settings is None or self.device_settings.pack() != self.s.pack():
            if not self._write():
                return
        msg = ("Save the settings the DAC is running into its flash?\n\n"
               "The DAC reboots through its boot ROM to do this: audio stops for about 5 seconds "
               "and playback apps may need to restart the stream.")
        if QMessageBox.question(self, APP_NAME, msg) != QMessageBox.Yes:
            return
        settings = copy.deepcopy(self.device_settings)
        self._busy = True
        self.live_timer.stop()
        self.dev.close()
        self.dev = None
        self._set_conn("Saving…", WARN)
        self._update_buttons()
        dlg = QProgressDialog("Saving to flash…", None, 0, 0, self)
        dlg.setWindowTitle(APP_NAME)
        dlg.setWindowModality(Qt.WindowModal)
        dlg.setMinimumDuration(0)
        dlg.show()
        self._thread = QThread(self)
        self._worker = SaveWorker(settings, self.prefs.value("flashloader", "", str) or None)
        self._worker.moveToThread(self._thread)
        self._thread.started.connect(self._worker.run)
        self._worker.progress.connect(dlg.setLabelText)

        def done(seq, err):
            self._thread.quit()
            dlg.close()
            self._busy = False
            ok = self._connect()
            if err:
                if "flashloader.bin not found" in err:
                    self._ask_flashloader()
                QMessageBox.critical(self, APP_NAME, f"Save failed:\n{err}\n\nThe previously saved settings "
                                     "are intact. If the DAC stays silent, unplug and replug it.")
                return
            if ok and self.info and self.info.flash_sequence == seq and \
                    self.device_settings and self.device_settings.pack() == settings.pack():
                self.statusBar().showMessage(f"Saved to flash (record #{seq}) and verified after reboot.", 8000)
            else:
                QMessageBox.warning(self, APP_NAME, "The DAC rebooted but is not reporting the saved record. "
                                    "Press Read and check, then Save again.")

        self._worker.finished.connect(done)
        self._thread.start()

    def _ask_flashloader(self):
        path, _ = QFileDialog.getOpenFileName(self, "Locate NXP RT1010 ivt_flashloader.bin", "",
                                              "Flashloader (ivt_flashloader.bin *.bin)")
        if path:
            self.prefs.setValue("flashloader", path)

    # --- files -------------------------------------------------------------
    def on_import_autoeq(self):
        path, _ = QFileDialog.getOpenFileName(self, "Import AutoEQ / Equalizer APO parametric EQ",
                                              self.prefs.value("dir", "", str),
                                              "Parametric EQ (*.txt);;All files (*)")
        if not path:
            return
        self.prefs.setValue("dir", str(Path(path).parent))
        try:
            parsed = autoeq.load_parametric_eq(path, p.EQ_BANDS)
        except Exception as e:  # noqa: BLE001
            QMessageBox.warning(self, APP_NAME, f"Could not read {path}:\n{e}")
            return
        s = copy.deepcopy(self.s)
        s.bands = [p.EqBand(b.type, b.channels, b.frequency_hz, b.gain_db, b.q) for b in parsed.bands]
        s.bands += [p.EqBand() for _ in range(p.EQ_BANDS - len(s.bands))]
        s.eq_enabled = True
        self._load_into_widgets(s)
        self._changed()
        notes = [f"Imported {len(parsed.bands)} filter(s) from {Path(path).name}."]
        if parsed.preamp_db:
            notes.append(f"The file's preamp ({parsed.preamp_db:+.1f} dB) is not needed: the DAC subtracts "
                         f"the EQ's maximum boost ({eq_math.Chain(s).boost_db():.2f} dB) from the headroom "
                         "automatically.")
        notes += parsed.warnings
        QMessageBox.information(self, APP_NAME, "\n\n".join(notes))

    def on_export_apo(self):
        path, _ = QFileDialog.getSaveFileName(self, "Export EQ", "ParametricEQ.txt", "Text (*.txt)")
        if path:
            bands = [autoeq.Band(b.type, b.channels, b.frequency_hz, b.gain_db, b.q) for b in self.s.bands]
            Path(path).write_text(autoeq.format_parametric_eq(bands, -eq_math.Chain(self.s).boost_db()),
                                  encoding="utf-8")

    def on_open_preset(self):
        """Import an EQ preset file into My presets and load it."""
        path, _ = QFileDialog.getOpenFileName(self, "Import EQ preset", self.prefs.value("dir", "", str),
                                              "DykoDAC2 EQ preset (*.json)")
        if not path:
            return
        self.prefs.setValue("dir", str(Path(path).parent))
        try:
            pr = presets.Preset.from_json(Path(path).read_text(encoding="utf-8"), Path(path).stem)
        except Exception as e:  # noqa: BLE001
            QMessageBox.warning(self, APP_NAME, f"Not a DykoDAC2 EQ preset:\n{e}")
            return
        presets.save_user(pr)
        self._last_user_preset = pr.name
        self._load_into_widgets(pr.apply_to(self.s))
        self._reload_presets()
        self._changed()

    def on_save_preset(self):
        """Export the current EQ to a preset file."""
        key, pr = self._active_preset()
        name = pr.name if pr else "My EQ"
        path, _ = QFileDialog.getSaveFileName(self, "Export EQ preset", f"{name}.json",
                                              "DykoDAC2 EQ preset (*.json)")
        if path:
            Path(path).write_text(presets.Preset(name, self.s.bands, self.s.eq_enabled,
                                                 pr.description if pr else "").to_json(), encoding="utf-8")

    def closeEvent(self, ev):
        if self._busy:
            ev.ignore()
            return
        if self.b_ab.isChecked():
            self.b_ab.setChecked(False)  # don't leave the DAC running without its EQ
        if self.dev:
            self.dev.close()
        super().closeEvent(ev)


ABOUT_TEXT = """<h3>DykoDAC2 Configurator</h3>
<p><b>Read</b> loads what the DAC is running. <b>Write to RAM</b> applies the panel now
(or tick <i>Live apply</i>). <b>Save to flash</b> keeps the running settings across power cycles;
the DAC briefly reboots through its boot ROM for that. <b>Load from flash</b> discards RAM changes,
<b>Defaults</b> puts factory settings into RAM.</p>
<p>All processing (EQ, crossfeed, balance, gain) is 32-bit float before one TPDF-dithered
24-bit quantisation. The EQ's worst-case boost is always subtracted from the headroom.</p>"""


def main() -> int:
    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setStyle("Fusion")
    app.setPalette(dark_palette())
    app.setStyleSheet(STYLE)
    w = MainWindow()
    w.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
