#!/usr/bin/env python3
"""Read-only catalog of icxe-daq run directories.

Scans a data directory for metadata.json and builds a pandas table. Run
directories and their files are never written.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pandas as pd

# Baseline, rms, and threshold refer to this channel when it is enabled.
# In these runs that is the PMT; the partner channel sits near 15% full scale.
SIGNAL_CHANNEL = 0

_VOLTAGE_KV = re.compile(r"(\d+)_(\d+)kV")

COLUMNS = [
    "path",
    "name",
    "trigger",
    "voltage_kV",
    "started",
    "ended",
    "duration_s",
    "events",
    "baseline",
    "baseline_rms",
    "baseline_within_tolerance",
    "threshold",
    "threshold_adc",
    "channel",
    "channel_thresholds",
    "polarity",
    "dynamic_range",
    "target_percent",
    "dac",
    "channel_mask",
    "enabled_channels",
    "record_length",
    "pre_trigger_samples",
    "post_trigger_samples",
    "sample_period_ns",
    "waveforms_per_file",
    "n_files",
    "stop_reason",
    "io_level",
    "majority",
    "coincidence_window_ns",
    "pair_logic",
    "board_model",
    "board_serial",
    "adc_bits",
]

MINIMAL_COLUMNS = [
    "path",
    "trigger",
    "voltage_kV",
    "started",
    "ended",
    "events",
    "baseline",
    "baseline_rms",
    "threshold",
]


def default_data_dir() -> Path:
    return Path(__file__).resolve().parents[1] / "data"


def load_catalog(data_dir: Path | str | None = None) -> pd.DataFrame:
    """Return one row per run directory, with every catalog field filled in."""
    root = Path(data_dir) if data_dir is not None else default_data_dir()
    rows = [_row_from_run(path) for path in _run_dirs(root)]
    frame = pd.DataFrame(rows, columns=COLUMNS)
    if frame.empty:
        return frame
    frame["started"] = pd.to_datetime(frame["started"], utc=True)
    frame["ended"] = pd.to_datetime(frame["ended"], utc=True)
    return frame.sort_values(["started", "name"], kind="mergesort").reset_index(drop=True)


def format_catalog(catalog: pd.DataFrame, data_dir: Path | str | None = None) -> str:
    """Short text table: path, trigger, voltage, times, events, baseline, threshold."""
    root = Path(data_dir) if data_dir is not None else default_data_dir()
    view = catalog.loc[:, MINIMAL_COLUMNS].copy()
    view["path"] = [
        _short_path(path, root) for path in catalog["path"]
    ] if len(catalog) else view["path"]
    view["started"] = _format_time(view["started"])
    view["ended"] = _format_time(view["ended"])
    view["voltage_kV"] = view["voltage_kV"].map(_format_voltage)
    view["events"] = view["events"].map(_format_int)
    view["baseline"] = view["baseline"].map(lambda value: _format_float(value, 1))
    view["baseline_rms"] = view["baseline_rms"].map(lambda value: _format_float(value, 2))
    view["threshold"] = view["threshold"].map(_format_int)
    return view.to_string(index=False)


def _run_dirs(data_dir: Path):
    if not data_dir.is_dir():
        raise FileNotFoundError("data directory does not exist: %s" % data_dir)
    return sorted(
        path
        for path in data_dir.iterdir()
        if path.is_dir() and (path / "metadata.json").is_file()
    )


def _row_from_run(run_dir: Path) -> dict:
    metadata = _read_json(run_dir / "metadata.json")
    config_path = run_dir / "config.json"
    config = _read_json(config_path) if config_path.is_file() else {}
    name = config.get("name") or run_dir.name
    channel = _signal_channel(metadata)
    trigger = metadata.get("trigger") or {}
    pairs = trigger.get("pairs") or []
    board = metadata.get("board") or {}
    started = metadata.get("started_utc")
    ended = metadata.get("ended_utc")
    return {
        "path": str(run_dir.resolve()),
        "name": name,
        "trigger": trigger.get("type"),
        "voltage_kV": _voltage_kV(name),
        "started": started,
        "ended": ended,
        "duration_s": _duration_s(started, ended),
        "events": metadata.get("events_written"),
        "baseline": channel.get("baseline_mean"),
        "baseline_rms": channel.get("baseline_rms"),
        "baseline_within_tolerance": channel.get("baseline_within_tolerance"),
        "threshold": channel.get("threshold_counts"),
        "threshold_adc": channel.get("threshold_adc"),
        "channel": channel.get("index"),
        "channel_thresholds": _channel_thresholds(metadata),
        "polarity": channel.get("polarity"),
        "dynamic_range": channel.get("dynamic_range") or metadata.get("dynamic_range"),
        "target_percent": channel.get("target_percent"),
        "dac": channel.get("dac"),
        "channel_mask": metadata.get("channel_mask"),
        "enabled_channels": ",".join(str(ch) for ch in metadata.get("enabled_channels") or []),
        "record_length": metadata.get("record_length"),
        "pre_trigger_samples": metadata.get("pre_trigger_samples_actual"),
        "post_trigger_samples": metadata.get("post_trigger_samples_actual"),
        "sample_period_ns": metadata.get("sample_period_ns"),
        "waveforms_per_file": metadata.get("waveforms_per_file"),
        "n_files": len(metadata.get("files") or []),
        "stop_reason": metadata.get("stop_reason"),
        "io_level": trigger.get("io_level"),
        "majority": trigger.get("majority"),
        "coincidence_window_ns": trigger.get("window_ns"),
        "pair_logic": ",".join(pair.get("logic", "") for pair in pairs) or None,
        "board_model": board.get("model"),
        "board_serial": board.get("serial"),
        "adc_bits": board.get("adc_bits"),
    }


def _signal_channel(metadata: dict) -> dict:
    channels = metadata.get("channels") or []
    for channel in channels:
        if channel.get("enabled") and channel.get("index") == SIGNAL_CHANNEL:
            return channel
    enabled = [channel for channel in channels if channel.get("enabled")]
    return enabled[0] if enabled else {}


def _channel_thresholds(metadata: dict) -> str:
    parts = []
    for channel in metadata.get("channels") or []:
        if not channel.get("enabled"):
            continue
        counts = channel.get("threshold_counts")
        if counts is None:
            continue
        parts.append("%s:%s" % (channel.get("index"), counts))
    return ",".join(parts)


def _voltage_kV(name: str):
    match = _VOLTAGE_KV.search(name)
    if match is None:
        return None
    return float("%s.%s" % (match.group(1), match.group(2)))


def _duration_s(started, ended):
    if not started or not ended:
        return None
    delta = pd.Timestamp(ended) - pd.Timestamp(started)
    return delta.total_seconds()


def _read_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def _short_path(path: str, data_dir: Path) -> str:
    try:
        return str(Path(path).relative_to(data_dir.parent))
    except ValueError:
        return Path(path).name


def _format_time(values: pd.Series) -> pd.Series:
    stamps = pd.to_datetime(values, utc=True)
    return stamps.dt.strftime("%Y-%m-%d %H:%M:%S").mask(stamps.isna(), "")


def _format_voltage(value) -> str:
    if pd.isna(value):
        return ""
    return "%.1f" % value


def _format_float(value, digits: int) -> str:
    if pd.isna(value):
        return ""
    return "%.*f" % (digits, value)


def _format_int(value) -> str:
    if pd.isna(value):
        return ""
    return "%d" % value


def main() -> None:
    catalog = load_catalog()
    print(format_catalog(catalog))


if __name__ == "__main__":
    main()
