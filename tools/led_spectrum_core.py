"""LED spectrum accumulation: config, runs, and histograms."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from scipy.signal import butter, sosfiltfilt

from read_waveforms import load_waveforms

BASELINE_SAMPLES = 200
LED_WINDOW = (580, 620)
AMP_HI_REF = 10.0
N_AMP_BINS = 100
N_CHARGE_BINS = 300
CHARGE_SPAN = (-40.0, 250.0)
FIT_SPAN = (-50.0, 250.0)
MU_SPAN = (40.0, 60.0)
CUTOFF_MHZ = 10
_CONFIG_KEYS = {
    "amp_bounds",
    "baseline_samples",
    "charge_bounds",
    "cmap",
    "cutoff_mhz",
    "data",
    "fit_range",
    "led_window",
    "mu_bounds",
    "n_gaussians",
    "out",
    "runs",
    "softfilter",
}
_RUN_KEYS = {"tag", "label", "led_window", "amp_bounds", "charge_bounds"}

def _load_config(path: Path | None) -> dict:
    if path is None:
        return {}
    if not path.is_file():
        raise SystemExit("config file does not exist: %s" % path)
    cfg = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(cfg, dict):
        raise SystemExit("config must be a JSON object")
    unknown = set(cfg) - _CONFIG_KEYS
    if unknown:
        raise SystemExit("unknown config setting %s" % ", ".join(sorted(unknown)))
    if "cmap" in cfg and not isinstance(cfg["cmap"], bool):
        raise SystemExit("cmap must be true or false")
    if "softfilter" in cfg and not isinstance(cfg["softfilter"], bool):
        raise SystemExit("softfilter must be true or false")
    if "cutoff_mhz" in cfg and not _positive_number(cfg["cutoff_mhz"]):
        raise SystemExit("cutoff_mhz must be a positive number")
    return cfg

def _run_list(args, cfg: dict) -> list:
    cfg_runs = cfg.get("runs", [])
    if not isinstance(cfg_runs, list):
        raise SystemExit("config runs must be a list")
    if args.tags and cfg_runs:
        raise SystemExit("pass run tags on the command line or in config runs, not both")
    if not args.tags and not cfg_runs:
        raise SystemExit("provide run tags or --config with runs")

    runs = []
    if args.tags:
        runs = [{"tag": tag} for tag in args.tags]
    else:
        for entry in cfg_runs:
            if not isinstance(entry, dict) or "tag" not in entry:
                raise SystemExit("each config run needs a tag")
            unknown = set(entry) - _RUN_KEYS
            if unknown:
                raise SystemExit("unknown run setting %s" % ", ".join(sorted(unknown)))
            runs.append(dict(entry, tag=str(entry["tag"])))

    if args.label is None:
        present = ["label" in run and run["label"] is not None for run in runs]
        if any(present) and not all(present):
            raise SystemExit("provide a label for every run or for none")
    elif len(args.label) != len(runs):
        raise SystemExit(
            "--label needs one entry per run (got %d label(s) for %d run(s))"
            % (len(args.label), len(runs))
        )
    else:
        for run, label in zip(runs, args.label):
            run["label"] = label

    shared = args.led_window if args.led_window is not None else cfg.get("led_window", LED_WINDOW)
    shared = _window(shared, "led_window")
    shared_amp = _pair(
        args.amp_bounds if args.amp_bounds is not None else cfg.get("amp_bounds", (0.0, AMP_HI_REF)),
        "amp_bounds",
    )
    if args.charge_bounds is not None:
        shared_charge = _pair(args.charge_bounds, "charge_bounds")
    elif "charge_bounds" in cfg:
        shared_charge = _pair(cfg["charge_bounds"], "charge_bounds")
    else:
        shared_charge = None
    for run in runs:
        if run.get("led_window") is None:
            run["led_window"] = shared
        else:
            run["led_window"] = _window(run["led_window"], "led_window for %s" % run["tag"])
        run["noise_window"] = _noise_window(run["led_window"])
        if run.get("amp_bounds") is None:
            run["amp_bounds"] = shared_amp
        else:
            run["amp_bounds"] = _pair(run["amp_bounds"], "amp_bounds for %s" % run["tag"])
        if run.get("charge_bounds") is None:
            run["charge_bounds"] = shared_charge
        else:
            run["charge_bounds"] = _pair(run["charge_bounds"], "charge_bounds for %s" % run["tag"])
    return runs

def _pair(value, name: str):
    try:
        lo, hi = float(value[0]), float(value[1])
    except (TypeError, ValueError, IndexError):
        raise SystemExit("%s must be two numbers" % name) from None
    if hi <= lo:
        raise SystemExit("%s HI must be greater than LO" % name)
    return (lo, hi)

def _spectrum_edges(amp_bounds, charge_bounds):
    amp_lo, amp_hi = amp_bounds
    scale = amp_hi / AMP_HI_REF
    charge_span = charge_bounds if charge_bounds is not None else tuple(edge * scale for edge in CHARGE_SPAN)
    zoom_hi = charge_span[1] / 2.0
    if zoom_hi <= charge_span[0]:
        raise SystemExit("charge zoom upper bound HI/2 must be greater than LO")
    return {
        "amp_bins": np.linspace(amp_lo, amp_hi, N_AMP_BINS),
        "charge_bins": np.linspace(charge_span[0], charge_span[1], N_CHARGE_BINS),
        "zoom_bins": np.linspace(charge_span[0], zoom_hi, N_CHARGE_BINS),
        "scale": scale,
    }

def _window(value, name: str):
    try:
        start, stop = int(value[0]), int(value[1])
    except (TypeError, ValueError, IndexError):
        raise SystemExit("%s must be two integers" % name) from None
    if stop <= start:
        raise SystemExit("%s stop must be greater than start" % name)
    return (start, stop)

def _noise_window(led_window):
    return (0, led_window[1] - led_window[0])

def _span(cli, configured, default, name):
    chosen = cli if cli is not None else configured
    if chosen is None:
        return default
    try:
        return (float(chosen[0]), float(chosen[1]))
    except (TypeError, ValueError, IndexError):
        raise SystemExit("%s must be two numbers" % name) from None

def _resolve_tag(data_dir: Path, tag: str, led_window) -> Path:
    if not data_dir.is_dir():
        raise SystemExit("data directory does not exist: %s" % data_dir)
    matches = sorted(
        path
        for path in data_dir.iterdir()
        if path.is_dir() and tag in path.name and (path / "metadata.json").is_file()
    )
    if not matches:
        raise SystemExit("no run directory contains %r" % tag)
    if len(matches) > 1:
        names = "\n".join(path.name for path in matches)
        raise SystemExit("tag %r matches more than one run:\n%s" % (tag, names))
    print("%s -> %s  LED %d:%d" % (tag, matches[0].name, led_window[0], led_window[1]))
    return matches[0]

def _output_stem(base: str, softfilter: bool) -> str:
    return f"{base}_softfiltered" if softfilter else base

def _positive_number(value) -> bool:
    return not isinstance(value, bool) and isinstance(value, (int, float)) and value > 0

def _softfilter(samples: np.ndarray, dt_ns: float, cutoff_mhz: float = CUTOFF_MHZ) -> np.ndarray:
    """Zero-phase 4th-order Butterworth low-pass at cutoff_mhz along the sample axis."""
    fs = 1e9 / float(dt_ns)
    sos = butter(4, float(cutoff_mhz) * 1e6, btype="low", fs=fs, output="sos")
    return np.asarray(sosfiltfilt(sos, samples, axis=1), dtype=np.float32)

def _spectrum(
    run_dir: Path,
    baseline_samples: int,
    led_window,
    noise_window,
    keep_charge: bool,
    amp_bins,
    charge_bins,
    zoom_bins,
    softfilter: bool = False,
    cutoff_mhz: float = CUTOFF_MHZ,
) -> dict:
    metadata = json.loads((run_dir / "metadata.json").read_text(encoding="utf-8"))
    scale = _millivolt_scale(metadata)
    dt = float(metadata.get("sample_period_ns") or 2)
    files = sorted(run_dir.glob("wf_*.bin"))
    if not files:
        raise SystemExit("no wf_*.bin in %s" % run_dir)

    amplitude_counts = np.zeros(len(amp_bins) - 1, dtype=np.float64)
    led_counts = np.zeros(len(charge_bins) - 1, dtype=np.float64)
    noise_counts = np.zeros(len(charge_bins) - 1, dtype=np.float64)
    zoom_counts = np.zeros(len(zoom_bins) - 1, dtype=np.float64)
    noise_zoom_counts = np.zeros(len(zoom_bins) - 1, dtype=np.float64)
    average = None
    n_events = 0
    led_parts = []
    filter_check = None

    for path in files:
        waveforms, _ = load_waveforms(str(path))
        samples = np.asarray(waveforms[:, 0, :], dtype=np.float32)
        del waveforms
        baseline = samples[:, :baseline_samples].mean(axis=1)
        samples -= baseline[:, None]
        samples *= scale
        if softfilter:
            keep_check = filter_check is None
            if keep_check:
                if samples.shape[0] < 2:
                    raise SystemExit("softfilter waveform check needs at least 2 events in %s" % path.name)
                indexes = np.sort(np.random.default_rng().choice(samples.shape[0], size=2, replace=False))
                before = samples[indexes].copy()
            samples = _softfilter(samples, dt, cutoff_mhz)
            if keep_check:
                filter_check = {"before": before, "after": samples[indexes].copy(), "dt": dt}
        amplitude = samples.max(axis=1)
        led = samples[:, led_window[0]:led_window[1]].sum(axis=1) * dt
        noise = samples[:, noise_window[0]:noise_window[1]].sum(axis=1) * dt
        file_average = samples.mean(axis=0)
        n = samples.shape[0]
        del samples

        amplitude_counts += np.histogram(amplitude, bins=amp_bins)[0]
        led_counts += np.histogram(led, bins=charge_bins)[0]
        noise_counts += np.histogram(noise, bins=charge_bins)[0]
        zoom_counts += np.histogram(led, bins=zoom_bins)[0]
        noise_zoom_counts += np.histogram(noise, bins=zoom_bins)[0]
        average = file_average * n if average is None else average + file_average * n
        n_events += n
        if keep_charge:
            led_parts.append(np.asarray(led, dtype=np.float32))

    result = {
        "name": run_dir.name,
        "amplitude_counts": amplitude_counts,
        "led_counts": led_counts,
        "noise_counts": noise_counts,
        "led_zoom_counts": zoom_counts,
        "noise_zoom_counts": noise_zoom_counts,
        "average": average / n_events,
        "dt": dt,
        "n_events": n_events,
        "led": np.concatenate(led_parts) if led_parts else None,
    }
    if filter_check is not None:
        result["filter_check"] = filter_check
    return result

def _millivolt_scale(metadata: dict) -> float:
    text = None
    for channel in metadata.get("channels") or []:
        if channel.get("index") == 0 and channel.get("dynamic_range"):
            text = channel["dynamic_range"]
            break
    if text is None:
        text = metadata.get("dynamic_range") or "2Vpp"
    volts = float(str(text).lower().replace("vpp", ""))
    return -volts * 1000.0 / 2**14
