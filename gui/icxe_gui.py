#!/usr/bin/env python3
"""ICXe DAQ window: edit the shared JSON, run the digitizer, or plot one channel.

From this directory, with the virtualenv:

    .venv/bin/python icxe_gui.py
    .venv/bin/python icxe_gui.py --config ../config/default.json

The buffer line is computed in this process. Load, save, and that line work
before the C++ binary exists. Start looks for icxe-daq via --bin, then
ICXE_DAQ_BIN, then ../build/icxe-daq, then ./icxe-daq.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import shlex
import signal
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

N_CHANNELS = 16
# Nominal SRAM slices for Nb = 1, 2, 4, ... 1024. Usable size is nominal
# minus 10 on this firmware. The Nb=1024 entry is 5120, as published in the
# DAQ plan, and the line shows the usable count (5110).
_BUFFER_NOMINAL: Tuple[int, ...] = (
    5_120_000,
    2_560_000,
    1_280_000,
    640_000,
    320_000,
    160_000,
    80_000,
    40_000,
    20_000,
    10_000,
    5_120,
)
BUFFER_SAMPLES: Dict[int, int] = {
    1 << index: nominal - 10 for index, nominal in enumerate(_BUFFER_NOMINAL)
}
BUFFER_ORDER: Tuple[int, ...] = tuple(sorted(BUFFER_SAMPLES))

SCP1_MAGIC = b"SCP1"
SCP1_HEADER = 8
MAX_SCOPE_SAMPLES = 5_120_000

ROOT = Path(__file__).resolve().parent.parent
GUI_DIR = Path(__file__).resolve().parent

KNOWN_TOP = {
    "name",
    "output_directory",
    "duration_s",
    "waveforms_per_file",
    "record_length",
    "pre_trigger_samples",
    "n_buffers",
    "dynamic_range",
    "max_events",
    "connection",
    "baseline",
    "channels",
    "trigger",
    "output",
}
KNOWN_CONNECTION = {
    "link_type",
    "link_num",
    "conet_node",
    "vme_base_address",
    "use_vme_base_address",
}
KNOWN_TRIGGER = {"type", "mode", "io_level", "rate_hz", "coincidence", "thresholds"}
KNOWN_COINCIDENCE = {"pairs", "majority", "window_ns"}
KNOWN_BASELINE = {
    "target_percent",
    "polarity",
    "tolerance_lsb",
    "max_iterations",
    "apply_on_start",
    "triggers_per_step",
}
KNOWN_OUTPUT = {"directory", "waveforms_per_file", "name"}
KNOWN_CHANNEL = {
    "index",
    "enabled",
    "polarity",
    "target_percent",
    "threshold",
    "dynamic_range",
}
HARDWARE_PAIRS: Tuple[Tuple[int, int], ...] = tuple(
    (index, index + 1) for index in range(0, N_CHANNELS, 2)
)
PAIR_INDEX = {pair: index for index, pair in enumerate(HARDWARE_PAIRS)}

SUGGESTED_BASELINE = {"positive": 10, "negative": 90}


def _missing_dependency(exc: BaseException) -> None:
    sys.stderr.write(
        "Could not import the GUI libraries (%s).\n"
        "From the gui directory:\n"
        "  python3 -m venv --system-site-packages .venv\n"
        "  .venv/bin/pip install -r requirements.txt\n"
        "  .venv/bin/python icxe_gui.py\n" % (exc,)
    )
    raise SystemExit(1)


try:
    import numpy as np
    import pyqtgraph as pg
    from PySide6.QtCore import Qt, QProcess, QTimer
    from PySide6.QtGui import QFont
    from PySide6.QtWidgets import (
        QAbstractItemView,
        QApplication,
        QButtonGroup,
        QCheckBox,
        QComboBox,
        QDoubleSpinBox,
        QFileDialog,
        QFormLayout,
        QFrame,
        QGridLayout,
        QGroupBox,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QMainWindow,
        QMessageBox,
        QPlainTextEdit,
        QPushButton,
        QRadioButton,
        QScrollArea,
        QSizePolicy,
        QSpinBox,
        QSplitter,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )
except ImportError as exc:
    _missing_dependency(exc)


def largest_fitting_buffers(record_length: int) -> Optional[int]:
    """Largest Nb whose usable buffer is at least record_length."""
    chosen: Optional[int] = None
    for number in BUFFER_ORDER:
        if BUFFER_SAMPLES[number] >= record_length:
            chosen = number
    return chosen


def buffer_description(record_length: int, n_buffers: Optional[int] = None) -> str:
    """Read-only buffer line. Example: '1024 buffers x 5110 samples, record uses 1020'."""
    try:
        record_length = int(record_length)
    except (TypeError, ValueError):
        return "record length must be a positive multiple of 10"
    if record_length <= 0:
        return "record length must be a positive multiple of 10"

    chosen: Optional[int]
    if n_buffers not in (None, 0):
        try:
            requested = int(n_buffers)  # type: ignore[arg-type]
        except (TypeError, ValueError):
            return (
                "n_buffers must be 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, or 1024"
            )
        if requested not in BUFFER_SAMPLES:
            return (
                "n_buffers must be 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, or 1024"
            )
        size = BUFFER_SAMPLES[requested]
        if size < record_length:
            word = "buffer" if requested == 1 else "buffers"
            verb = "holds" if requested == 1 else "hold"
            return (
                "%d %s %s %d samples, which cannot fit record length %d"
                % (requested, word, verb, size, record_length)
            )
        chosen = requested
    else:
        chosen = largest_fitting_buffers(record_length)
        if chosen is None:
            return (
                "record length %d does not fit in one buffer (largest is %d samples)"
                % (record_length, BUFFER_SAMPLES[1])
            )

    size = BUFFER_SAMPLES[chosen]
    word = "buffer" if chosen == 1 else "buffers"
    return "%d %s x %d samples, record uses %d" % (
        chosen,
        word,
        size,
        record_length,
    )


def _looks_like_text(blob: bytes) -> bool:
    return all(byte in (9, 10, 13) or 32 <= byte <= 126 for byte in blob)


class ScopeDecoder:
    """Parse scope stdout: magic SCP1, uint32 sample count, then int16 samples."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._text: List[str] = []
        self.locked = False

    def take_text(self) -> str:
        if not self._text:
            return ""
        text = "".join(self._text)
        self._text.clear()
        return text

    def flush(self) -> None:
        """Keep a trailing stdout error line that never received a newline."""
        if self.locked or not self._buf:
            return
        pending = bytes(self._buf)
        if _looks_like_text(pending):
            self._remember_text(pending)
            self._buf.clear()

    def feed(self, data: bytes) -> List[Any]:
        if data:
            self._buf.extend(data)
        frames: List[Any] = []
        while True:
            if not self.locked:
                if self._take_leading_text():
                    continue
                magic_at = self._buf.find(SCP1_MAGIC)
                if magic_at < 0:
                    if len(self._buf) > 65536:
                        pending = bytes(self._buf)
                        if _looks_like_text(pending):
                            self._remember_text(pending)
                        self._buf.clear()
                    break
                if magic_at > 0:
                    prefix = bytes(self._buf[:magic_at])
                    if _looks_like_text(prefix):
                        self._remember_text(prefix)
                    del self._buf[:magic_at]
                    continue
            if len(self._buf) < SCP1_HEADER:
                break
            if bytes(self._buf[:4]) != SCP1_MAGIC:
                start = self._buf.find(SCP1_MAGIC)
                if start < 0:
                    if len(self._buf) > 3:
                        del self._buf[:-3]
                    break
                del self._buf[:start]
                continue
            n_samples = struct.unpack_from("<I", self._buf, 4)[0]
            if n_samples > MAX_SCOPE_SAMPLES:
                del self._buf[:1]
                continue
            nbytes = SCP1_HEADER + n_samples * 2
            if len(self._buf) < nbytes:
                break
            if n_samples:
                payload = bytes(self._buf[SCP1_HEADER:nbytes])
                samples = np.frombuffer(payload, dtype=np.dtype("<i2")).copy()
            else:
                samples = np.empty(0, dtype=np.int16)
            del self._buf[:nbytes]
            self.locked = True
            frames.append(samples)
        return frames

    def _take_leading_text(self) -> bool:
        """Remove one leading text line that arrives before the first frame."""
        if self.locked or not self._buf:
            return False
        magic_at = self._buf.find(SCP1_MAGIC)
        newline = self._buf.find(b"\n")
        if newline < 0 or (magic_at >= 0 and newline > magic_at):
            return False
        chunk = bytes(self._buf[: newline + 1])
        if not _looks_like_text(chunk):
            return False
        del self._buf[: newline + 1]
        self._remember_text(chunk)
        return True

    def _remember_text(self, chunk: bytes) -> None:
        text = chunk.decode("utf-8", "replace")
        if text:
            self._text.append(text)


@dataclass
class BinaryLocation:
    path: Optional[Path]
    notes: List[str]


def _candidate_note(label: str, path: Path) -> str:
    try:
        exists = path.is_file()
    except OSError as exc:
        return "%s: %s (%s)" % (label, path, exc)
    if exists and os.access(path, os.X_OK):
        return "%s: %s is executable" % (label, path)
    if exists:
        return "%s: %s exists but is not executable" % (label, path)
    return "%s: %s not found" % (label, path)


def _default_candidates() -> List[Tuple[str, Path]]:
    found = [
        ("../build/icxe-daq", ROOT / "build" / "icxe-daq"),
        ("./icxe-daq", GUI_DIR / "icxe-daq"),
    ]
    cwd_path = Path.cwd() / "icxe-daq"
    try:
        same = cwd_path.resolve() == (GUI_DIR / "icxe-daq").resolve()
    except OSError:
        same = False
    if not same:
        found.append(("./icxe-daq (current directory)", cwd_path))
    return found


def _usable_binary(path: Path) -> Optional[Path]:
    try:
        if path.is_file() and os.access(path, os.X_OK):
            return path.resolve()
    except OSError:
        return None
    return None


def locate_binary(explicit: Optional[str]) -> BinaryLocation:
    """Resolve icxe-daq: --bin, else ICXE_DAQ_BIN, else paths next to the GUI."""
    notes: List[str] = []
    defaults = _default_candidates()

    def finish_specified(label: str, raw: str) -> BinaryLocation:
        path = Path(raw).expanduser()
        usable = _usable_binary(path)
        notes.append(_candidate_note(label, path))
        if usable is not None:
            return BinaryLocation(usable, notes)
        notes.append("Later locations were not used because %s was set." % label)
        for default_label, default_path in defaults:
            note = _candidate_note(default_label, default_path)
            if "is executable" in note:
                note += " (not selected)"
            notes.append(note)
        return BinaryLocation(None, notes)

    if explicit:
        return finish_specified("--bin", explicit)
    env = os.environ.get("ICXE_DAQ_BIN", "").strip()
    if env:
        return finish_specified("ICXE_DAQ_BIN", env)

    notes.append("--bin was not given")
    notes.append("ICXE_DAQ_BIN is not set")
    for label, path in defaults:
        usable = _usable_binary(path)
        notes.append(_candidate_note(label, path))
        if usable is not None:
            return BinaryLocation(usable, notes)
    return BinaryLocation(None, notes)


def _json_number(value: float) -> Any:
    if isinstance(value, float) and value.is_integer():
        return int(value)
    return value


def _as_int(value: Any, default: int) -> int:
    try:
        if isinstance(value, bool):
            raise TypeError
        if isinstance(value, int):
            return value
        if isinstance(value, float):
            return int(value)
        return int(float(str(value).strip()))
    except (TypeError, ValueError):
        return default


def _as_float(value: Any, default: float) -> float:
    try:
        if isinstance(value, bool):
            raise TypeError
        return float(value)
    except (TypeError, ValueError):
        return default


def _as_bool(value: Any, default: bool) -> bool:
    if isinstance(value, bool):
        return value
    return default


def _as_dict(value: Any) -> Dict[str, Any]:
    if isinstance(value, dict):
        return value
    return {}


def _unknown_fields(blob: Dict[str, Any], known: Sequence[str]) -> Dict[str, Any]:
    skip = set(known)
    return {
        key: copy.deepcopy(value) for key, value in blob.items() if key not in skip
    }


def _with_extra(body: Dict[str, Any], extra: Dict[str, Any]) -> Dict[str, Any]:
    merged = dict(body)
    for key, value in extra.items():
        if key not in merged:
            merged[key] = copy.deepcopy(value)
    return merged


def _empty_extra() -> Dict[str, Dict[str, Any]]:
    return {
        "top": {},
        "connection": {},
        "trigger": {},
        "coincidence": {},
        "baseline": {},
        "output": {},
        "target_percent": {},
        "polarity": {},
        "thresholds": {},
    }


def _split_channel_map(raw: Any) -> Tuple[Dict[int, Any], Dict[str, Any]]:
    good: Dict[int, Any] = {}
    extra: Dict[str, Any] = {}
    if not isinstance(raw, dict):
        return good, extra
    for key, value in raw.items():
        try:
            index = int(key)
        except (TypeError, ValueError):
            extra[str(key)] = copy.deepcopy(value)
            continue
        if 0 <= index < N_CHANNELS:
            good[index] = value
        else:
            extra[str(key)] = copy.deepcopy(value)
    return good, extra


def _pair_logic_key(logic: Any) -> str:
    if not isinstance(logic, str):
        return ""
    return logic.strip().lower()


def _ui_pair_logic(logic: Any) -> str:
    key = _pair_logic_key(logic)
    if key == "or":
        return "OR"
    if key in ("even", "odd", "only"):
        return "ONLY"
    return "AND"


def _match_pair(pair: Any) -> Optional[Tuple[int, Optional[int]]]:
    """Map a coincidence pair onto a hardware row.

    The on-screen logic is AND / OR / ONLY. The file uses and / or / even / odd,
    and older GUI files used AND / OR / ONLY.
    """
    if not isinstance(pair, dict):
        return None
    logic = _pair_logic_key(pair.get("logic"))
    channels = pair.get("channels")
    if logic not in ("and", "or", "even", "odd", "only") or not isinstance(channels, list):
        return None
    try:
        parsed = [int(channel) for channel in channels]
    except (TypeError, ValueError):
        return None
    if logic in ("and", "or"):
        if len(parsed) != 2:
            return None
        index = PAIR_INDEX.get((min(parsed), max(parsed)))
        if index is None:
            return None
        return index, None
    if len(parsed) != 1:
        return None
    channel = parsed[0]
    if logic == "even" and channel % 2 != 0:
        return None
    if logic == "odd" and channel % 2 != 1:
        return None
    for index, (low, high) in enumerate(HARDWARE_PAIRS):
        if channel in (low, high):
            return index, channel
    return None


def _configure_pyqtgraph() -> None:
    pg.setConfigOptions(
        useOpenGL=False,
        antialias=False,
        background="#0e1116",
        foreground="#d7dde6",
    )


def _monospace() -> QFont:
    font = QFont("monospace")
    font.setStyleHint(QFont.Monospace)
    return font


def _centered(widget: QWidget) -> QWidget:
    holder = QWidget()
    layout = QHBoxLayout(holder)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setAlignment(Qt.AlignCenter)
    layout.addWidget(widget)
    return holder


def _retain_items(box: QComboBox, allowed: Sequence[Any]) -> None:
    keep = set(allowed)
    for index in range(box.count() - 1, -1, -1):
        if box.itemData(index) not in keep:
            box.removeItem(index)


def _set_choice(box: QComboBox, value: Any, fallback: Any) -> None:
    if value is None:
        value = fallback
    index = box.findData(value)
    if index < 0:
        box.addItem(str(value), value)
        index = box.findData(value)
    if index >= 0:
        box.setCurrentIndex(index)


def _set_spin(box: QSpinBox, value: Any, default: int) -> None:
    number = _as_int(value, default)
    if number > box.maximum():
        box.setMaximum(number)
    if number < box.minimum():
        box.setMinimum(number)
    box.setValue(number)


def _set_double(box: QDoubleSpinBox, value: Any, default: float) -> None:
    number = _as_float(value, default)
    if number > box.maximum():
        box.setMaximum(number)
    if number < box.minimum():
        box.setMinimum(number)
    box.setValue(number)


@dataclass
class ChannelRow:
    index: int
    enable: QCheckBox
    polarity: QComboBox
    baseline: QSpinBox
    threshold: QSpinBox


@dataclass
class PairRow:
    index: int
    low: int
    high: int
    enabled: QCheckBox
    logic: QComboBox
    only: QComboBox


class MainWindow(QMainWindow):
    def __init__(
        self,
        config_path: Optional[str] = None,
        binary_hint: Optional[str] = None,
    ) -> None:
        super().__init__()
        _configure_pyqtgraph()
        self.setWindowTitle("ICXe DAQ")
        self.resize(1280, 840)
        self.setMinimumSize(960, 640)

        self._binary_hint = binary_hint
        self._config_path: Optional[Path] = None
        self._proc: Optional[QProcess] = None
        self._running = False
        self._running_mode: Optional[str] = None
        self._decoder = ScopeDecoder()
        self._text_buf = bytearray()
        self._frame_count = 0
        self._plotted_length = -1
        self._applying = False
        self._extra = _empty_extra()
        self._extra_pairs: List[Any] = []
        self._pair_extras: List[Dict[str, Any]] = [{} for _ in HARDWARE_PAIRS]
        self._channel_ranges: Dict[int, str] = {}
        self._channel_extras: List[Dict[str, Any]] = [{} for _ in range(N_CHANNELS)]

        self._build_ui()
        self.apply_config({})
        if config_path:
            self.load_path(Path(config_path))

    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)
        outer.setContentsMargins(8, 8, 8, 8)
        outer.setSpacing(6)
        outer.addLayout(self._build_toolbar())

        self.status_line = QLabel("Idle")
        self.status_line.setWordWrap(True)
        self.status_line.setTextInteractionFlags(Qt.TextSelectableByMouse)
        outer.addWidget(self.status_line)

        self.config_label = QLabel("Config: unsaved defaults")
        self.config_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.config_label.setStyleSheet("color: #555555;")
        outer.addWidget(self.config_label)

        splitter = QSplitter(Qt.Horizontal)
        splitter.addWidget(self._build_controls())
        splitter.addWidget(self._build_plot_panel())
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([540, 720])
        outer.addWidget(splitter, 1)

    def _build_toolbar(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.addWidget(QLabel("Mode"))
        self.mode_digitizer = QRadioButton("Digitizer")
        self.mode_scope = QRadioButton("Oscilloscope")
        self.mode_digitizer.setChecked(True)
        self._mode_group = QButtonGroup(self)
        self._mode_group.addButton(self.mode_digitizer)
        self._mode_group.addButton(self.mode_scope)
        self._mode_group.buttonToggled.connect(self._sync_mode_widgets)
        row.addWidget(self.mode_digitizer)
        row.addWidget(self.mode_scope)
        row.addSpacing(12)
        row.addWidget(QLabel("Scope channel"))
        self.scope_channel = QComboBox()
        for index in range(N_CHANNELS):
            self.scope_channel.addItem(str(index), index)
        self.scope_channel.currentIndexChanged.connect(self._sync_plot_title)
        row.addWidget(self.scope_channel)
        row.addStretch(1)

        self.load_btn = QPushButton("Load JSON")
        self.save_btn = QPushButton("Save JSON")
        self.load_btn.clicked.connect(self._load_dialog)
        self.save_btn.clicked.connect(self._save_dialog)
        row.addWidget(self.load_btn)
        row.addWidget(self.save_btn)

        self.start_btn = QPushButton("Start")
        self.stop_btn = QPushButton("Stop")
        self.start_btn.setStyleSheet(
            "QPushButton { background: #1f7a3a; color: white; padding: 6px 16px; "
            "font-weight: 600; }"
            "QPushButton:disabled { background: #c5d0c8; color: #666666; }"
        )
        self.stop_btn.setStyleSheet(
            "QPushButton { background: #a33b3b; color: white; padding: 6px 16px; "
            "font-weight: 600; }"
            "QPushButton:disabled { background: #e0d2d2; color: #666666; }"
        )
        self.start_btn.clicked.connect(self._start)
        self.stop_btn.clicked.connect(self._stop)
        self.stop_btn.setEnabled(False)
        row.addWidget(self.start_btn)
        row.addWidget(self.stop_btn)
        return row

    def _build_controls(self) -> QScrollArea:
        self.controls = QWidget()
        layout = QVBoxLayout(self.controls)
        layout.setContentsMargins(2, 2, 8, 2)
        layout.setSpacing(8)
        layout.addWidget(self._build_acquisition())
        layout.addWidget(self._build_trigger())
        layout.addWidget(self._build_channels())
        layout.addWidget(self._build_baseline())
        layout.addWidget(self._build_output())
        layout.addWidget(self._build_connection())
        layout.addStretch(1)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.NoFrame)
        scroll.setWidget(self.controls)
        scroll.setMinimumWidth(460)
        return scroll

    def _build_acquisition(self) -> QGroupBox:
        box = QGroupBox("Acquisition")
        form = QFormLayout(box)
        form.setContentsMargins(8, 16, 8, 8)

        self.record_length = QSpinBox()
        self.record_length.setRange(0, BUFFER_SAMPLES[1])
        self.record_length.setSingleStep(10)
        self.record_length.setToolTip("Samples per waveform. Must be a multiple of 10.")
        self.record_length.valueChanged.connect(self._refresh_timing_hints)

        self.pre_trigger = QSpinBox()
        self.pre_trigger.setRange(0, BUFFER_SAMPLES[1])
        self.pre_trigger.setToolTip("Samples kept before the trigger.")
        self.pre_trigger.valueChanged.connect(self._refresh_timing_hints)

        self.post_hint = QLabel("")
        self.post_hint.setWordWrap(True)

        self.dynamic_range = QComboBox()
        for label in ("2Vpp", "0.5Vpp"):
            self.dynamic_range.addItem(label, label)
        self.dynamic_range.currentIndexChanged.connect(self._on_dynamic_range)

        self.n_buffers = QComboBox()
        self.n_buffers.addItem("auto", 0)
        for number in BUFFER_ORDER:
            self.n_buffers.addItem(str(number), number)
        self.n_buffers.setToolTip(
            "Auto picks the largest buffer count whose slice still holds the record."
        )
        self.n_buffers.currentIndexChanged.connect(self._refresh_buffer_line)

        self.buffer_line = QLabel("")
        self.buffer_line.setWordWrap(True)
        self.buffer_line.setFont(_monospace())
        self.buffer_line.setTextInteractionFlags(Qt.TextSelectableByMouse)

        form.addRow("Record length", self.record_length)
        form.addRow("Pre-trigger samples", self.pre_trigger)
        form.addRow(self.post_hint)
        form.addRow("Dynamic range", self.dynamic_range)
        form.addRow("Number of buffers", self.n_buffers)
        form.addRow(self.buffer_line)
        return box

    def _build_trigger(self) -> QGroupBox:
        box = QGroupBox("Trigger")
        layout = QVBoxLayout(box)
        layout.setContentsMargins(8, 16, 8, 8)

        type_row = QWidget()
        type_layout = QHBoxLayout(type_row)
        type_layout.setContentsMargins(0, 0, 0, 0)
        type_layout.addWidget(QLabel("Type"))
        self.trigger_mode = QComboBox()
        for label, value in (
            ("External", "external"),
            ("Coincidence", "coincidence"),
            ("Rate", "rate"),
        ):
            self.trigger_mode.addItem(label, value)
        self.trigger_mode.currentIndexChanged.connect(self._sync_trigger_fields)
        type_layout.addWidget(self.trigger_mode, 1)
        layout.addWidget(type_row)

        self.io_level = QComboBox()
        self.io_level.addItem("NIM", "NIM")
        self.io_level.addItem("TTL", "TTL")
        self.io_row = self._labeled_row("I/O level", self.io_level)
        layout.addWidget(self.io_row)

        self.rate_hz = QDoubleSpinBox()
        self.rate_hz.setRange(0.01, 1_000_000)
        self.rate_hz.setDecimals(2)
        self.rate_hz.setSingleStep(1)
        self.rate_hz.setSuffix(" Hz")
        self.rate_row = self._labeled_row("Software rate", self.rate_hz)
        layout.addWidget(self.rate_row)

        self.coincidence_box = QGroupBox("Coincidence")
        coincidence = QVBoxLayout(self.coincidence_box)
        coincidence.setContentsMargins(8, 14, 8, 8)
        pair_form = QFormLayout()
        self.majority = QSpinBox()
        self.majority.setRange(0, 7)
        self.majority.setToolTip(
            "Trigger when the number of pair requests in the window is greater than this level."
        )
        self.window_ns = QSpinBox()
        self.window_ns.setRange(0, 100_000)
        self.window_ns.setSingleStep(8)
        self.window_ns.setSuffix(" ns")
        self.window_ns.setToolTip("Coincidence window. The digitizer steps this in 8 ns.")
        pair_form.addRow("Majority", self.majority)
        pair_form.addRow("Window", self.window_ns)
        coincidence.addLayout(pair_form)

        grid = QGridLayout()
        for column, title in enumerate(("Use", "Pair", "Logic", "ONLY channel")):
            grid.addWidget(QLabel(title), 0, column)
        self.pair_rows: List[PairRow] = []
        for index, (low, high) in enumerate(HARDWARE_PAIRS):
            enabled = QCheckBox()
            logic = QComboBox()
            for name in ("AND", "OR", "ONLY"):
                logic.addItem(name, name)
            only = QComboBox()
            only.addItem(str(low), low)
            only.addItem(str(high), high)
            only.setEnabled(False)
            logic.currentIndexChanged.connect(
                lambda _value, pair_index=index: self._on_pair_logic(pair_index)
            )
            grid.addWidget(enabled, index + 1, 0)
            grid.addWidget(QLabel("%d-%d" % (low, high)), index + 1, 1)
            grid.addWidget(logic, index + 1, 2)
            grid.addWidget(only, index + 1, 3)
            self.pair_rows.append(
                PairRow(index, low, high, enabled, logic, only)
            )
        coincidence.addLayout(grid)
        hint = QLabel(
            "Hardware pairs are adjacent channels. ONLY records the selected "
            "channel of that pair. Majority must be lower than the number of "
            "enabled pairs."
        )
        hint.setWordWrap(True)
        coincidence.addWidget(hint)
        layout.addWidget(self.coincidence_box)
        return box

    def _build_channels(self) -> QGroupBox:
        box = QGroupBox("Channels")
        layout = QVBoxLayout(box)
        layout.setContentsMargins(8, 16, 8, 8)
        hint = QLabel(
            "Changing polarity suggests a baseline of 10% for a positive pulse "
            "and 90% for a negative pulse. The percent stays editable."
        )
        hint.setWordWrap(True)
        layout.addWidget(hint)

        self.channel_table = QTableWidget(N_CHANNELS, 5)
        self.channel_table.setHorizontalHeaderLabels(
            ["Enable", "Ch", "Polarity", "Baseline %", "Threshold"]
        )
        header_item = self.channel_table.horizontalHeaderItem(4)
        if header_item is not None:
            header_item.setToolTip(
                "ADC counts relative to the baseline. Applied in coincidence mode."
            )
        self.channel_table.verticalHeader().setVisible(False)
        self.channel_table.verticalHeader().setDefaultSectionSize(30)
        self.channel_table.setSelectionMode(QAbstractItemView.NoSelection)
        self.channel_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        header = self.channel_table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(2, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(3, QHeaderView.Stretch)
        header.setSectionResizeMode(4, QHeaderView.Stretch)

        self.rows: List[ChannelRow] = []
        for index in range(N_CHANNELS):
            enable = QCheckBox()
            polarity = QComboBox()
            polarity.addItem("Positive", "positive")
            polarity.addItem("Negative", "negative")
            polarity.currentIndexChanged.connect(
                lambda _value, channel=index: self._on_polarity(channel)
            )
            baseline = QSpinBox()
            baseline.setRange(0, 100)
            baseline.setSuffix(" %")
            threshold = QSpinBox()
            threshold.setRange(0, 65535)
            item = QTableWidgetItem(str(index))
            item.setTextAlignment(Qt.AlignCenter)
            item.setFlags(Qt.ItemIsEnabled)
            self.channel_table.setItem(index, 1, item)
            self.channel_table.setCellWidget(index, 0, _centered(enable))
            self.channel_table.setCellWidget(index, 2, polarity)
            self.channel_table.setCellWidget(index, 3, baseline)
            self.channel_table.setCellWidget(index, 4, threshold)
            self.rows.append(ChannelRow(index, enable, polarity, baseline, threshold))

        header_height = max(self.channel_table.horizontalHeader().sizeHint().height(), 28)
        self.channel_table.setFixedHeight(header_height + N_CHANNELS * 30 + 4)
        layout.addWidget(self.channel_table)
        return box

    def _build_baseline(self) -> QGroupBox:
        box = QGroupBox("Baseline")
        form = QFormLayout(box)
        form.setContentsMargins(8, 16, 8, 8)
        self.tolerance = QDoubleSpinBox()
        self.tolerance.setRange(0, 10_000)
        self.tolerance.setDecimals(1)
        self.tolerance.setSingleStep(0.5)
        self.tolerance.setSuffix(" LSB")
        self.max_iterations = QSpinBox()
        self.max_iterations.setRange(1, 10_000)
        self.apply_on_start = QCheckBox("Calibrate DC offset when a run starts")
        form.addRow("Tolerance", self.tolerance)
        form.addRow("Max iterations", self.max_iterations)
        form.addRow(self.apply_on_start)
        return box

    def _build_output(self) -> QGroupBox:
        box = QGroupBox("Output")
        form = QFormLayout(box)
        form.setContentsMargins(8, 16, 8, 8)

        self.output_dir = QLineEdit()
        self.output_dir.setToolTip(
            "Relative paths are resolved from the ICXe-DAQ directory when a run starts."
        )
        browse = QPushButton("Browse...")
        browse.clicked.connect(self._browse_output)
        directory = QWidget()
        directory_row = QHBoxLayout(directory)
        directory_row.setContentsMargins(0, 0, 0, 0)
        directory_row.addWidget(self.output_dir, 1)
        directory_row.addWidget(browse)

        self.output_name = QLineEdit()
        self.waveforms_per_file = QSpinBox()
        self.waveforms_per_file.setRange(1, 1_000_000)
        self.max_events = QSpinBox()
        self.max_events.setRange(0, 2_000_000_000)
        self.max_events.setSpecialValueText("no limit")
        self.duration = QDoubleSpinBox()
        self.duration.setRange(0, 10_000_000)
        self.duration.setDecimals(1)
        self.duration.setSpecialValueText("no limit")
        self.duration.setSuffix(" s")

        form.addRow("Directory", directory)
        form.addRow("Name", self.output_name)
        form.addRow("Waveforms per file", self.waveforms_per_file)
        form.addRow("Max events", self.max_events)
        form.addRow("Duration", self.duration)
        return box

    def _build_connection(self) -> QGroupBox:
        box = QGroupBox("Connection")
        form = QFormLayout(box)
        form.setContentsMargins(8, 16, 8, 8)
        self.link_type = QLineEdit()
        self.link_num = QSpinBox()
        self.link_num.setRange(0, 15)
        self.conet_node = QSpinBox()
        self.conet_node.setRange(0, 15)
        self.vme_base = QLineEdit()
        self.vme_base.setToolTip(
            "Stored for a later bridge. The direct optical link opens at base 0."
        )
        self.use_vme = QCheckBox("Pass the VME base address to open")
        self.use_vme.setToolTip("Leave this off for the A5818 optical link.")
        form.addRow("Link type", self.link_type)
        form.addRow("Link number", self.link_num)
        form.addRow("CONET node", self.conet_node)
        form.addRow("VME base address", self.vme_base)
        form.addRow(self.use_vme)
        return box

    def _build_plot_panel(self) -> QWidget:
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(0, 0, 0, 0)
        header = QHBoxLayout()
        self.frame_label = QLabel("-")
        self.fit_btn = QPushButton("Fit")
        self.fit_btn.clicked.connect(self._fit_plot)
        header.addWidget(self.frame_label)
        header.addStretch(1)
        header.addWidget(self.fit_btn)
        layout.addLayout(header)

        stack = QWidget()
        grid = QGridLayout(stack)
        grid.setContentsMargins(0, 0, 0, 0)
        self.plot = pg.PlotWidget()
        self.plot.setLabel("bottom", "sample")
        self.plot.setLabel("left", "ADC counts")
        self.plot.showGrid(x=True, y=True, alpha=0.25)
        self.plot.setYRange(0, 16383, padding=0.02)
        self.plot.setXRange(0, 1024, padding=0)
        self.plot.disableAutoRange()
        self.plot.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.curve = self.plot.plot(pen=pg.mkPen("#37d67a", width=1))
        self.curve.setClipToView(True)
        self.curve.setDownsampling(auto=True, method="peak")
        self.plot_note = QLabel(
            "Digitizer mode writes waveform files.\nThis plot stays empty."
        )
        self.plot_note.setAlignment(Qt.AlignCenter)
        self.plot_note.setStyleSheet(
            "background-color: #0e1116; color: #d7dde6; font-size: 15px;"
        )
        grid.addWidget(self.plot, 0, 0)
        grid.addWidget(self.plot_note, 0, 0)
        layout.addWidget(stack, 1)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(200)
        self.log.setFixedHeight(150)
        self.log.setFont(_monospace())
        self.log.setPlaceholderText("Status from icxe-daq appears here.")
        layout.addWidget(self.log)
        return panel

    @staticmethod
    def _labeled_row(label: str, widget: QWidget) -> QWidget:
        row = QWidget()
        layout = QHBoxLayout(row)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(QLabel(label))
        layout.addWidget(widget, 1)
        return row

    def _run_mode(self) -> str:
        return "scope" if self.mode_scope.isChecked() else "run"

    def _sync_mode_widgets(self, *_args: Any) -> None:
        scope = self._run_mode() == "scope"
        self.scope_channel.setEnabled(scope and not self._running)
        self.mode_digitizer.setEnabled(not self._running)
        self.mode_scope.setEnabled(not self._running)
        self.plot_note.setVisible(not scope)
        self.fit_btn.setEnabled(scope)
        self._sync_plot_title()

    def _sync_plot_title(self, *_args: Any) -> None:
        if self._run_mode() == "scope":
            channel = self.scope_channel.currentData()
            self.plot.setTitle("Channel %s" % channel)
        else:
            self.plot.setTitle("Digitizer")

    def _sync_trigger_fields(self, *_args: Any) -> None:
        mode = self.trigger_mode.currentData()
        self.io_row.setVisible(mode in ("external", "rate"))
        self.rate_row.setVisible(mode == "rate")
        self.coincidence_box.setVisible(mode == "coincidence")
        for row in self.rows:
            row.threshold.setEnabled(mode == "coincidence")

    def _on_polarity(self, index: int) -> None:
        if self._applying:
            return
        row = self.rows[index]
        polarity = row.polarity.currentData()
        row.baseline.setValue(SUGGESTED_BASELINE.get(polarity, 10))

    def _on_pair_logic(self, index: int) -> None:
        row = self.pair_rows[index]
        row.only.setEnabled(row.logic.currentData() == "ONLY")

    def _refresh_timing_hints(self, *_args: Any) -> None:
        record = self.record_length.value()
        pre = self.pre_trigger.value()
        if record <= 0:
            self.post_hint.setText("Record length must be a positive multiple of 10.")
        elif pre > record:
            self.post_hint.setText("Pre-trigger is longer than the record.")
        else:
            post_percent = 100.0 * (record - pre) / float(record)
            text = (
                "Post-trigger ≈ %.0f%% (%d samples). The firmware stores a percent, "
                "so the sample count can step coarsely."
                % (post_percent, record - pre)
            )
            if record % 10 != 0:
                text += " Record length must be a multiple of 10."
            self.post_hint.setText(text)
        self._refresh_buffer_line()

    def _refresh_buffer_line(self, *_args: Any) -> None:
        requested = self.n_buffers.currentData()
        override = None if requested in (None, 0) else int(requested)
        text = buffer_description(self.record_length.value(), override)
        self.buffer_line.setText(text)
        bad = any(
            token in text
            for token in ("does not fit", "must be", "cannot", "not a power")
        )
        color = "#8a2b2b" if bad else "#16324f"
        self.buffer_line.setStyleSheet(
            "background: #f4f7fb; color: %s; padding: 6px; border: 1px solid #d0d7e2;"
            % color
        )

    def _set_running(self, running: bool) -> None:
        self._running = running
        self.start_btn.setEnabled(not running)
        self.stop_btn.setEnabled(running)
        self.controls.setEnabled(not running)
        self.load_btn.setEnabled(not running)
        self.save_btn.setEnabled(not running)
        self._sync_mode_widgets()

    def _set_status(self, text: str, error: bool = False) -> None:
        self.status_line.setText(text)
        self.status_line.setStyleSheet("color: #8a1c1c;" if error else "")

    def _publish_status(self, line: str) -> None:
        self.log.appendPlainText(line)
        self._set_status(line)

    def apply_config(self, cfg: Any) -> None:
        if not isinstance(cfg, dict):
            cfg = {}
        self._applying = True
        try:
            self._reset_widget_values()
            self._overlay_config(cfg)
        finally:
            self._applying = False
        self._refresh_timing_hints()
        self._sync_trigger_fields()
        self._sync_mode_widgets()
        for index in range(len(self.pair_rows)):
            self._on_pair_logic(index)

    def _reset_widget_values(self) -> None:
        self._extra = _empty_extra()
        self._extra_pairs = []
        self._pair_extras = [{} for _ in HARDWARE_PAIRS]
        self._channel_ranges = {}
        self._channel_extras = [{} for _ in range(N_CHANNELS)]

        self.link_type.setText("optical")
        self.link_num.setValue(0)
        self.conet_node.setValue(0)
        self.vme_base.setText("0x32140000")
        self.use_vme.setChecked(False)

        _retain_items(self.dynamic_range, ("2Vpp", "0.5Vpp"))
        _set_choice(self.dynamic_range, "2Vpp", "2Vpp")
        self.record_length.setValue(1020)
        self.pre_trigger.setValue(100)
        self._reset_n_buffers()

        _retain_items(self.trigger_mode, ("external", "coincidence", "rate"))
        _set_choice(self.trigger_mode, "external", "external")
        _retain_items(self.io_level, ("NIM", "TTL"))
        _set_choice(self.io_level, "NIM", "NIM")
        self.rate_hz.setValue(10)
        self.majority.setValue(0)
        self.window_ns.setValue(8)
        for row in self.pair_rows:
            row.enabled.setChecked(False)
            _set_choice(row.logic, "AND", "AND")
            _set_choice(row.only, row.low, row.low)

        for row in self.rows:
            row.enable.setChecked(row.index == 0)
            _set_choice(row.polarity, "positive", "positive")
            row.baseline.setValue(10)
            row.threshold.setValue(100)

        self.tolerance.setValue(2)
        self.max_iterations.setValue(20)
        self.apply_on_start.setChecked(True)
        self._triggers_per_step = None
        self.output_dir.setText("data")
        self.output_name.setText("run")
        self.waveforms_per_file.setValue(1000)
        self.max_events.setValue(0)
        self.duration.setValue(0)

    def _reset_n_buffers(self) -> None:
        allowed = set(BUFFER_SAMPLES)
        allowed.add(0)
        for index in range(self.n_buffers.count() - 1, -1, -1):
            if self.n_buffers.itemData(index) not in allowed:
                self.n_buffers.removeItem(index)
        auto = self.n_buffers.findData(0)
        if auto >= 0:
            self.n_buffers.setCurrentIndex(auto)

    def _overlay_config(self, cfg: Dict[str, Any]) -> None:
        self._extra["top"] = _unknown_fields(cfg, tuple(KNOWN_TOP))
        connection = _as_dict(cfg.get("connection"))
        self._extra["connection"] = _unknown_fields(connection, tuple(KNOWN_CONNECTION))
        if "link_type" in connection:
            self.link_type.setText(str(connection.get("link_type") or "optical"))
        if "link_num" in connection:
            _set_spin(self.link_num, connection.get("link_num"), 0)
        if "conet_node" in connection:
            _set_spin(self.conet_node, connection.get("conet_node"), 0)
        if "vme_base_address" in connection:
            self.vme_base.setText(str(connection.get("vme_base_address") or "0x32140000"))
        if "use_vme_base_address" in connection:
            self.use_vme.setChecked(
                _as_bool(connection.get("use_vme_base_address"), False)
            )

        self._apply_legacy_channel_enables(cfg.get("channels"))

        if "record_length" in cfg:
            _set_spin(self.record_length, cfg.get("record_length"), 1020)
        if "pre_trigger_samples" in cfg:
            _set_spin(self.pre_trigger, cfg.get("pre_trigger_samples"), 100)
        if "dynamic_range" in cfg:
            _set_choice(self.dynamic_range, cfg.get("dynamic_range"), "2Vpp")
        if "n_buffers" in cfg and cfg.get("n_buffers") not in (None, 0):
            self._set_n_buffers(cfg.get("n_buffers"))

        trigger = _as_dict(cfg.get("trigger"))
        self._extra["trigger"] = _unknown_fields(trigger, tuple(KNOWN_TRIGGER))
        trigger_type = trigger.get("type", trigger.get("mode"))
        if trigger_type is not None:
            _set_choice(self.trigger_mode, str(trigger_type).strip().lower(), "external")
        if "io_level" in trigger:
            _set_choice(self.io_level, trigger.get("io_level"), "NIM")
        if "rate_hz" in trigger:
            _set_double(self.rate_hz, trigger.get("rate_hz"), 10)
        thresholds, extra_thresholds = _split_channel_map(trigger.get("thresholds"))
        self._extra["thresholds"] = extra_thresholds
        for index, value in thresholds.items():
            _set_spin(self.rows[index].threshold, value, 100)

        coincidence = _as_dict(trigger.get("coincidence"))
        self._extra["coincidence"] = _unknown_fields(
            coincidence, tuple(KNOWN_COINCIDENCE)
        )
        if "majority" in coincidence:
            _set_spin(self.majority, coincidence.get("majority"), 0)
        if "window_ns" in coincidence:
            _set_spin(self.window_ns, coincidence.get("window_ns"), 8)
        if "pairs" in coincidence:
            self._apply_pairs(coincidence.get("pairs"))

        baseline = _as_dict(cfg.get("baseline"))
        self._extra["baseline"] = _unknown_fields(baseline, tuple(KNOWN_BASELINE))
        polarity, extra_polarity = _split_channel_map(baseline.get("polarity"))
        percent, extra_percent = _split_channel_map(baseline.get("target_percent"))
        self._extra["polarity"] = extra_polarity
        self._extra["target_percent"] = extra_percent
        for index, value in polarity.items():
            choice = value if value in ("positive", "negative") else "positive"
            _set_choice(self.rows[index].polarity, choice, "positive")
        for row in self.rows:
            if row.index in percent:
                _set_spin(row.baseline, percent[row.index], 10)
            elif row.polarity.currentData() == "negative":
                row.baseline.setValue(90)
        if "tolerance_lsb" in baseline:
            _set_double(self.tolerance, baseline.get("tolerance_lsb"), 2)
        if "max_iterations" in baseline:
            _set_spin(self.max_iterations, baseline.get("max_iterations"), 20)
        if "apply_on_start" in baseline:
            self.apply_on_start.setChecked(
                _as_bool(baseline.get("apply_on_start"), True)
            )
        if "triggers_per_step" in baseline:
            self._triggers_per_step = _as_int(baseline.get("triggers_per_step"), 8)

        self._apply_channel_objects(cfg.get("channels"))
        self._adopt_uniform_channel_range(cfg)

        output = _as_dict(cfg.get("output"))
        self._extra["output"] = _unknown_fields(output, tuple(KNOWN_OUTPUT))
        if "directory" in output:
            self.output_dir.setText(str(output.get("directory") or ""))
        elif "output_directory" in cfg:
            self.output_dir.setText(str(cfg.get("output_directory") or ""))
        if "name" in output:
            self.output_name.setText(str(output.get("name") or ""))
        elif "name" in cfg:
            self.output_name.setText(str(cfg.get("name") or ""))
        if "waveforms_per_file" in output:
            _set_spin(self.waveforms_per_file, output.get("waveforms_per_file"), 1000)
        elif "waveforms_per_file" in cfg:
            _set_spin(self.waveforms_per_file, cfg.get("waveforms_per_file"), 1000)
        if "max_events" in cfg and cfg.get("max_events") not in (None, 0):
            _set_spin(self.max_events, cfg.get("max_events"), 0)
        if "duration_s" in cfg and cfg.get("duration_s") not in (None, 0):
            _set_double(self.duration, cfg.get("duration_s"), 0)

    def _apply_legacy_channel_enables(self, channels: Any) -> None:
        if not isinstance(channels, list):
            return
        if any(isinstance(item, dict) for item in channels):
            return
        enabled = set()
        for item in channels:
            index = _as_int(item, -1)
            if 0 <= index < N_CHANNELS:
                enabled.add(index)
        for row in self.rows:
            row.enable.setChecked(row.index in enabled)

    def _apply_channel_objects(self, channels: Any) -> None:
        if not isinstance(channels, list):
            return
        objects = [item for item in channels if isinstance(item, dict)]
        if not objects:
            return
        mentioned = set()
        for item in objects:
            index = _as_int(item.get("index"), -1)
            if not 0 <= index < N_CHANNELS:
                continue
            mentioned.add(index)
            row = self.rows[index]
            if "enabled" in item:
                row.enable.setChecked(_as_bool(item.get("enabled"), True))
            else:
                row.enable.setChecked(True)
            if "polarity" in item:
                choice = item.get("polarity")
                if isinstance(choice, str):
                    choice = choice.strip().lower()
                if choice not in ("positive", "negative"):
                    choice = "positive"
                _set_choice(row.polarity, choice, "positive")
            if "target_percent" in item:
                _set_spin(row.baseline, item.get("target_percent"), 10)
            if "threshold" in item:
                _set_spin(row.threshold, item.get("threshold"), 100)
            if "dynamic_range" in item and isinstance(item.get("dynamic_range"), str):
                self._channel_ranges[index] = item["dynamic_range"]
            self._channel_extras[index] = _unknown_fields(item, tuple(KNOWN_CHANNEL))
        for row in self.rows:
            if row.index not in mentioned:
                row.enable.setChecked(False)

    def _adopt_uniform_channel_range(self, cfg: Dict[str, Any]) -> None:
        if not self._channel_ranges:
            return
        values = set(self._channel_ranges.values())
        if len(values) != 1:
            return
        only = next(iter(values))
        top = cfg.get("dynamic_range")
        if top is None:
            _set_choice(self.dynamic_range, only, "2Vpp")
            self._channel_ranges.clear()
        elif str(top) == only:
            self._channel_ranges.clear()

    def _on_dynamic_range(self, *_args: Any) -> None:
        if self._applying:
            return
        self._channel_ranges.clear()

    def _set_n_buffers(self, value: Any) -> None:
        number = _as_int(value, 0)
        if number == 0:
            self._reset_n_buffers()
            return
        index = self.n_buffers.findData(number)
        if index < 0:
            self.n_buffers.addItem(str(number), number)
            index = self.n_buffers.findData(number)
        if index >= 0:
            self.n_buffers.setCurrentIndex(index)

    def _apply_pairs(self, pairs: Any) -> None:
        self._extra_pairs = []
        self._pair_extras = [{} for _ in HARDWARE_PAIRS]
        for row in self.pair_rows:
            row.enabled.setChecked(False)
        if not isinstance(pairs, list):
            return
        used = set()
        for pair in pairs:
            matched = _match_pair(pair)
            if matched is None or matched[0] in used:
                self._extra_pairs.append(copy.deepcopy(pair))
                continue
            index, only_channel = matched
            used.add(index)
            row = self.pair_rows[index]
            row.enabled.setChecked(True)
            _set_choice(row.logic, _ui_pair_logic(pair.get("logic")), "AND")
            if only_channel is not None:
                _set_choice(row.only, only_channel, row.low)
            elif _pair_logic_key(pair.get("logic")) == "odd":
                _set_choice(row.only, row.high, row.low)
            self._pair_extras[index] = {
                key: copy.deepcopy(value)
                for key, value in pair.items()
                if key not in ("channels", "logic")
            }

    def collect_config(self) -> Dict[str, Any]:
        baseline: Dict[str, Any] = {
            "apply_on_start": self.apply_on_start.isChecked(),
            "tolerance_lsb": _json_number(float(self.tolerance.value())),
            "max_iterations": self.max_iterations.value(),
        }
        if self._triggers_per_step is not None:
            baseline["triggers_per_step"] = self._triggers_per_step
        buffers = self.n_buffers.currentData()
        config: Dict[str, Any] = {
            "name": self.output_name.text().strip(),
            "output_directory": self.output_dir.text().strip(),
            "duration_s": _json_number(float(self.duration.value())),
            "waveforms_per_file": self.waveforms_per_file.value(),
            "record_length": self.record_length.value(),
            "pre_trigger_samples": self.pre_trigger.value(),
            "n_buffers": None if buffers in (None, 0) else int(buffers),
            "dynamic_range": self.dynamic_range.currentData(),
            "connection": _with_extra(
                {
                    "link_type": self.link_type.text().strip() or "optical",
                    "link_num": self.link_num.value(),
                    "conet_node": self.conet_node.value(),
                    "vme_base_address": self.vme_base.text().strip() or "0x32140000",
                    "use_vme_base_address": self.use_vme.isChecked(),
                },
                self._extra.get("connection", {}),
            ),
            "baseline": _with_extra(baseline, self._extra.get("baseline", {})),
            "channels": self._collect_channels(),
            "trigger": _with_extra(
                self._collect_trigger(),
                self._extra.get("trigger", {}),
            ),
        }
        if self.max_events.value() > 0:
            config["max_events"] = self.max_events.value()
        leftover_output = self._extra.get("output") or {}
        if leftover_output:
            config["output"] = copy.deepcopy(leftover_output)
        for key, value in self._extra.get("top", {}).items():
            if key not in config:
                config[key] = copy.deepcopy(value)
        return config

    def _collect_channels(self) -> List[Dict[str, Any]]:
        global_range = self.dynamic_range.currentData()
        channels: List[Dict[str, Any]] = []
        for row in self.rows:
            override = self._channel_ranges.get(row.index)
            entry: Dict[str, Any] = {
                "index": row.index,
                "enabled": row.enable.isChecked(),
                "polarity": row.polarity.currentData(),
                "target_percent": row.baseline.value(),
                "threshold": row.threshold.value(),
                "dynamic_range": override if override else global_range,
            }
            for key, value in self._channel_extras[row.index].items():
                if key not in entry:
                    entry[key] = copy.deepcopy(value)
            channels.append(entry)
        return channels

    def _collect_trigger(self) -> Dict[str, Any]:
        trigger: Dict[str, Any] = {
            "type": self.trigger_mode.currentData(),
            "io_level": self.io_level.currentData(),
            "rate_hz": _json_number(float(self.rate_hz.value())),
        }
        pairs = self._collect_pairs()
        if pairs or self.trigger_mode.currentData() == "coincidence":
            trigger["coincidence"] = _with_extra(
                {
                    "majority": self.majority.value(),
                    "window_ns": self.window_ns.value(),
                    "pairs": pairs,
                },
                self._extra.get("coincidence", {}),
            )
        return trigger

    def _collect_pairs(self) -> List[Dict[str, Any]]:
        pairs: List[Dict[str, Any]] = []
        for row in self.pair_rows:
            if not row.enabled.isChecked():
                continue
            logic = row.logic.currentData()
            if logic == "ONLY":
                channel = int(row.only.currentData())
                schema = "even" if channel % 2 == 0 else "odd"
                channels = [channel]
            elif logic == "OR":
                schema = "or"
                channels = [row.low, row.high]
            else:
                schema = "and"
                channels = [row.low, row.high]
            pair: Dict[str, Any] = {"channels": channels, "logic": schema}
            for key, value in self._pair_extras[row.index].items():
                if key not in pair:
                    pair[key] = copy.deepcopy(value)
            pairs.append(pair)
        pairs.extend(copy.deepcopy(self._extra_pairs))
        return pairs

    def validate(self) -> List[str]:
        errors: List[str] = []
        record = self.record_length.value()
        pre = self.pre_trigger.value()
        if record <= 0 or record % 10 != 0:
            errors.append("Record length must be a positive multiple of 10.")
        elif largest_fitting_buffers(record) is None:
            errors.append(
                "Record length %d does not fit in one buffer (largest is %d samples)."
                % (record, BUFFER_SAMPLES[1])
            )
        requested = self.n_buffers.currentData()
        if requested not in (None, 0):
            if requested not in BUFFER_SAMPLES:
                errors.append(
                    "Number of buffers must be 1, 2, 4, ..., 1024, or auto."
                )
            elif BUFFER_SAMPLES[int(requested)] < record:
                errors.append(
                    "%d buffers hold %d samples, which cannot fit record length %d."
                    % (int(requested), BUFFER_SAMPLES[int(requested)], record)
                )
        if pre < 0 or pre > record:
            errors.append("Pre-trigger samples must be between 0 and the record length.")
        if not any(row.enable.isChecked() for row in self.rows):
            errors.append("Enable at least one channel.")
        if not self.output_dir.text().strip():
            errors.append("Choose an output directory.")
        if not self.output_name.text().strip():
            errors.append("Choose an output name.")
        if self.waveforms_per_file.value() < 1:
            errors.append("Waveforms per file must be at least 1.")
        if not self.link_type.text().strip():
            errors.append("Link type is required.")
        mode = self.trigger_mode.currentData()
        if mode == "rate" and self.rate_hz.value() <= 0:
            errors.append("Rate must be greater than 0 Hz.")
        pairs = self._collect_pairs()
        if mode == "coincidence" and not pairs:
            errors.append("Coincidence mode needs at least one pair.")
        if pairs:
            if self.majority.value() >= len(pairs):
                errors.append(
                    "Majority must be lower than the number of enabled pairs. "
                    "The digitizer triggers when the number of pair requests "
                    "in the window is greater than this level."
                )
            window = self.window_ns.value()
            if window < 0 or window > 120 or window % 8 != 0:
                errors.append(
                    "Coincidence window must be a multiple of 8 ns from 0 to 120."
                )
            enabled = {row.index for row in self.rows if row.enable.isChecked()}
            for pair in pairs:
                for channel in pair.get("channels", []):
                    if channel not in enabled:
                        errors.append(
                            "Coincidence pair lists channel %s, which is not enabled."
                            % channel
                        )
        return errors

    def load_path(self, path: Path) -> bool:
        try:
            text = Path(path).read_text(encoding="utf-8")
            data = json.loads(text)
            if not isinstance(data, dict):
                raise ValueError("configuration root must be a JSON object")
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
            QMessageBox.critical(self, "Could not load configuration", str(exc))
            return False
        self.apply_config(data)
        self._config_path = Path(path)
        self.config_label.setText("Config: %s" % path)
        self._set_status("Loaded %s" % path)
        return True

    def save_path(self, path: Path) -> bool:
        destination = Path(path).expanduser()
        payload = json.dumps(self.collect_config(), indent=2) + "\n"
        temporary = destination.with_name(
            destination.name + ".%d.tmp" % os.getpid()
        )
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary.write_text(payload, encoding="utf-8")
            temporary.replace(destination)
        except OSError as exc:
            QMessageBox.critical(self, "Could not save configuration", str(exc))
            return False
        self._config_path = destination
        self.config_label.setText("Config: %s" % destination)
        self._set_status("Saved %s" % destination)
        return True

    def _dialog_start(self) -> str:
        if self._config_path is not None:
            return str(self._config_path)
        default_file = ROOT / "config" / "default.json"
        if default_file.exists():
            return str(default_file)
        if (ROOT / "config").is_dir():
            return str(ROOT / "config")
        return str(ROOT)

    def _load_dialog(self) -> None:
        path, _selected = QFileDialog.getOpenFileName(
            self,
            "Load configuration",
            self._dialog_start(),
            "JSON (*.json)",
        )
        if path:
            self.load_path(Path(path))

    def _save_dialog(self) -> None:
        path, _selected = QFileDialog.getSaveFileName(
            self,
            "Save configuration",
            self._dialog_start(),
            "JSON (*.json)",
        )
        if path:
            self.save_path(Path(path))

    def _browse_output(self) -> None:
        current = self.output_dir.text().strip()
        start = current if current and Path(current).is_absolute() else str(ROOT)
        path = QFileDialog.getExistingDirectory(self, "Output directory", start)
        if path:
            self.output_dir.setText(path)

    def _start(self) -> None:
        if self._running:
            return
        errors = self.validate()
        if errors:
            QMessageBox.critical(self, "Check the configuration", "\n".join(errors))
            return
        mode = self._run_mode()
        if mode == "scope":
            channel = int(self.scope_channel.currentData())
            enabled = {row.index for row in self.rows if row.enable.isChecked()}
            if channel not in enabled:
                answer = QMessageBox.question(
                    self,
                    "Channel disabled",
                    "Channel %d is not enabled. The scope asks the digitizer for "
                    "that channel, and a disabled channel is left out of the "
                    "enable mask.\n\nStart anyway?" % channel,
                    QMessageBox.Yes | QMessageBox.No,
                    QMessageBox.No,
                )
                if answer != QMessageBox.Yes:
                    return
        located = locate_binary(self._binary_hint)
        if located.path is None:
            QMessageBox.critical(
                self,
                "icxe-daq not found",
                "Could not find the icxe-daq binary.\n\n"
                "The JSON editor and the buffer line do not need it.\n\n"
                + "\n".join(located.notes)
                + "\n\nBuild icxe-daq, pass --bin, or set ICXE_DAQ_BIN.",
            )
            self._set_status("icxe-daq was not found. Configuration is still editable.", error=True)
            return
        try:
            config_path = self._write_run_config()
        except OSError as exc:
            QMessageBox.critical(self, "Could not write the run configuration", str(exc))
            return

        if mode == "scope":
            args = [
                "scope",
                "--config",
                str(config_path),
                "--channel",
                str(int(self.scope_channel.currentData())),
            ]
            self.curve.setData([], [])
            self._plotted_length = -1
            self._frame_count = 0
            self.frame_label.setText("Frames: 0")
        else:
            args = ["run", "--config", str(config_path)]
            self.curve.clear()
            self._plotted_length = -1
            self.frame_label.setText("Digitizer mode does not plot waveforms.")

        command = " ".join(shlex.quote(part) for part in [str(located.path), *args])
        self._decoder = ScopeDecoder()
        self._text_buf = bytearray()
        self._retire_process()
        self._running_mode = mode
        self._proc = QProcess(self)
        self._proc.setProgram(str(located.path))
        self._proc.setArguments(args)
        self._proc.setWorkingDirectory(str(ROOT))
        self._proc.setProcessChannelMode(QProcess.SeparateChannels)
        self._proc.readyReadStandardOutput.connect(self._on_stdout)
        self._proc.readyReadStandardError.connect(self._on_stderr)
        self._proc.finished[int, QProcess.ExitStatus].connect(self._on_finished)
        self._proc.errorOccurred.connect(self._on_process_error)
        self._set_running(True)
        self._publish_status("$ " + command)
        self._proc.start()

    def _retire_process(self) -> None:
        proc = self._proc
        if proc is None:
            return
        for signal_obj, slot in (
            (proc.readyReadStandardOutput, self._on_stdout),
            (proc.readyReadStandardError, self._on_stderr),
            (proc.errorOccurred, self._on_process_error),
        ):
            try:
                signal_obj.disconnect(slot)
            except (RuntimeError, TypeError):
                pass
        try:
            proc.finished.disconnect(self._on_finished)
        except (RuntimeError, TypeError):
            pass
        if proc.state() != QProcess.NotRunning:
            proc.kill()
        proc.deleteLater()
        self._proc = None

    def _write_run_config(self) -> Path:
        path = Path(tempfile.gettempdir()) / ("icxe-daq-gui-%d.json" % os.getpid())
        path.write_text(
            json.dumps(self.collect_config(), indent=2) + "\n",
            encoding="utf-8",
        )
        return path

    def _stop(self) -> None:
        proc = self._proc
        if proc is None or proc.state() == QProcess.NotRunning:
            self._set_running(False)
            return
        self.stop_btn.setEnabled(False)
        self._set_status("Stopping...")
        pid = int(proc.processId())
        if pid > 0:
            try:
                os.kill(pid, signal.SIGINT)
            except OSError:
                proc.terminate()
        else:
            proc.terminate()
        QTimer.singleShot(3000, self._kill_if_running)

    def _kill_if_running(self) -> None:
        proc = self._proc
        if proc is not None and proc.state() != QProcess.NotRunning:
            proc.kill()

    def _on_stdout(self) -> None:
        proc = self.sender()
        if proc is None or proc is not self._proc:
            return
        data = bytes(proc.readAllStandardOutput())
        if self._running_mode == "scope":
            frames = self._decoder.feed(data)
            text = self._decoder.take_text()
            if text:
                self._handle_status_bytes(text.encode("utf-8"))
            if frames:
                self._frame_count += len(frames)
                self._show_frame(frames[-1])
        else:
            self._handle_status_bytes(data)

    def _on_stderr(self) -> None:
        proc = self.sender()
        if proc is None or proc is not self._proc:
            return
        self._handle_status_bytes(bytes(proc.readAllStandardError()))

    def _handle_status_bytes(self, data: bytes) -> None:
        if data:
            self._text_buf.extend(data)
        while True:
            newline = self._text_buf.find(b"\n")
            carriage = self._text_buf.find(b"\r")
            cuts = [index for index in (newline, carriage) if index >= 0]
            if not cuts:
                if len(self._text_buf) > 8192:
                    line = bytes(self._text_buf).decode("utf-8", "replace").strip()
                    self._text_buf.clear()
                    if line:
                        self._publish_status(line)
                break
            cut = min(cuts)
            raw = bytes(self._text_buf[:cut])
            drop = cut + 1
            if (
                cut < len(self._text_buf)
                and self._text_buf[cut] == 13
                and drop < len(self._text_buf)
                and self._text_buf[drop] == 10
            ):
                drop += 1
            del self._text_buf[:drop]
            line = raw.decode("utf-8", "replace").strip()
            if line:
                self._publish_status(line)

    def _flush_status_bytes(self) -> None:
        if self._text_buf:
            self._handle_status_bytes(b"\n")

    def _show_frame(self, samples: Any) -> None:
        array = np.asarray(samples, dtype=np.float32)
        count = int(array.shape[0])
        if count:
            self.curve.setData(array)
        else:
            self.curve.setData([], [])
        if count != self._plotted_length:
            self._plotted_length = count
            self.plot.autoRange()
            self.plot.disableAutoRange()
        self.frame_label.setText("Frames: %d    %d samples" % (self._frame_count, count))

    def _fit_plot(self) -> None:
        self.plot.autoRange()
        self.plot.disableAutoRange()

    def _on_finished(self, exit_code: int, exit_status: Any) -> None:
        if self.sender() is not self._proc:
            return
        self._on_stdout()
        self._on_stderr()
        if self._running_mode == "scope":
            self._decoder.flush()
            trailing = self._decoder.take_text()
            if trailing:
                self._handle_status_bytes(trailing.encode("utf-8"))
        self._flush_status_bytes()
        self._set_running(False)
        crashed = str(getattr(exit_status, "name", exit_status)) == "CrashExit"
        if crashed:
            message = "Stopped (crashed, code %d)" % exit_code
            error = True
        elif exit_code != 0:
            message = "Stopped (exit code %d)" % exit_code
            error = True
        else:
            message = "Stopped (exit code 0)"
            error = False
        self._set_status(message, error=error)
        self.log.appendPlainText(message)
        self._running_mode = None

    def _on_process_error(self, err: Any) -> None:
        if err != QProcess.FailedToStart:
            return
        detail = ""
        if self._proc is not None:
            detail = self._proc.errorString()
        self._set_running(False)
        self._running_mode = None
        self._set_status("Failed to start icxe-daq.", error=True)
        QMessageBox.critical(
            self,
            "Failed to start",
            "icxe-daq failed to start.\n%s" % detail,
        )

    def closeEvent(self, event: Any) -> None:
        proc = self._proc
        if proc is not None and proc.state() != QProcess.NotRunning:
            pid = int(proc.processId())
            if pid > 0:
                try:
                    os.kill(pid, signal.SIGINT)
                except OSError:
                    proc.terminate()
            else:
                proc.terminate()
            if not proc.waitForFinished(2000):
                proc.kill()
                proc.waitForFinished(1000)
        event.accept()


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ICXe DAQ control and oscilloscope")
    parser.add_argument(
        "--config",
        help="JSON configuration to load (for example ../config/default.json)",
    )
    parser.add_argument(
        "--bin",
        help="Path to the icxe-daq binary. Overrides ICXE_DAQ_BIN.",
    )
    return parser.parse_args(None if argv is None else list(argv))


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName("ICXe DAQ")
    app.setStyle("Fusion")
    window = MainWindow(config_path=args.config, binary_hint=args.bin)
    window.show()
    return int(app.exec())


if __name__ == "__main__":
    sys.exit(main())
