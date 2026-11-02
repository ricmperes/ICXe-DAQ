#!/usr/bin/env python3
"""Model-independent LED occupancy, SPE mean and gain from a noise run and an LED run.

Both runs are truncated to the smaller event count N. The LED window charge of
every event is histogrammed with the same bins for both runs. For each
threshold the occupancy is l = -ln(As/Ab), where As and Ab count the LED and
noise events below it; the first threshold with l_err/l under --max-rel-err
sets l. The SPE mean and variance then follow from the moments of the two
charge distributions, and the gain from the SPE mean.

A tag is a unique substring of a run directory name under data/.

    python led_occupancy.py NOISE_TAG LED_TAG --data /data/PMT_on_flex/3inch --led-window 640 720 --amplification 10
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from read_waveforms import load_waveforms

BASELINE_SAMPLES = 200
LED_WINDOW = (580, 620)
CHARGE_BOUNDS = (-500.0, 3000.0)
BIN_WIDTH = 1.0
PLOT_BIN_WIDTH = 20.0
MAX_REL_ERR = 0.02
IMPEDANCE = 50.0
E_CHARGE = 1.6021766208e-19


def default_data_dir() -> Path:
    return Path(__file__).resolve().parents[1] / "data"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("noise_tag", help="unique substring of the noise (LED off) run directory name")
    parser.add_argument("led_tag", help="unique substring of the LED run directory name")
    parser.add_argument("--data", type=Path, default="/data/PMT_on_flex/3inch", help="run directory root")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[1] / "plots" / "led_occupancy", help="directory for the png and json files")
    parser.add_argument(
        "--led-window",
        type=int,
        nargs=2,
        default=LED_WINDOW,
        metavar=("START", "STOP"),
        help="LED sample window, applied to both runs (default 580 620)",
    )
    parser.add_argument("--baseline-samples", type=int, default=BASELINE_SAMPLES)
    parser.add_argument(
        "--charge-bounds",
        type=float,
        nargs=2,
        default=CHARGE_BOUNDS,
        metavar=("LO", "HI"),
        help="charge histogram range in pVs (default -500 3000); charges outside are not histogrammed",
    )
    parser.add_argument("--bin-width", type=float, default=BIN_WIDTH, help="charge bin width in pVs (default 1)")
    parser.add_argument(
        "--thr-range",
        type=float,
        nargs=2,
        default=None,
        metavar=("LO", "HI"),
        help="thresholds to scan in pVs (default the full charge range)",
    )
    parser.add_argument(
        "--max-rel-err",
        type=float,
        default=MAX_REL_ERR,
        help="the first threshold with l_err/l below this sets the occupancy (default 0.02)",
    )
    parser.add_argument("--impedance", type=float, default=IMPEDANCE, help="digitizer input impedance in ohm (default 50)")
    parser.add_argument("--amplification", type=float, default=1.0, help="amplifier gain in front of the digitizer (default 1)")
    parser.add_argument(
        "--plot-bin-width",
        type=float,
        default=PLOT_BIN_WIDTH,
        help="charge bin width of the spectrum plot in pVs (default 20)",
    )
    parser.add_argument(
        "--scale-noise",
        action="store_true",
        help="draw the noise spectrum scaled by As/Ab = exp(-l) at the chosen threshold so its pedestal matches the LED pedestal",
    )
    args = parser.parse_args(argv)

    led_window = _window(args.led_window, "--led-window")
    charge_bounds = _pair(args.charge_bounds, "--charge-bounds")
    thr_range = _pair(args.thr_range, "--thr-range") if args.thr_range is not None else None
    if args.bin_width <= 0:
        raise SystemExit("--bin-width must be positive")
    if args.max_rel_err <= 0:
        raise SystemExit("--max-rel-err must be positive")
    if args.impedance <= 0 or args.amplification <= 0:
        raise SystemExit("--impedance and --amplification must be positive")

    data_dir = args.data or default_data_dir()
    out_dir = args.out or Path(__file__).resolve().parents[1] / "plots" / "led_occupancy"
    out_dir.mkdir(parents=True, exist_ok=True)

    noise_dir = _resolve_tag(data_dir, args.noise_tag)
    led_dir = _resolve_tag(data_dir, args.led_tag)
    n_events = min(_event_count(noise_dir), _event_count(led_dir))
    if n_events <= 0:
        raise SystemExit("a run has no events")
    print("using N = %d events from each run; LED window %d:%d" % (n_events, led_window[0], led_window[1]))

    bins = _charge_bins(charge_bounds, args.bin_width)
    off = _charges(noise_dir, n_events, args.baseline_samples, led_window, bins)
    on = _charges(led_dir, n_events, args.baseline_samples, led_window, bins)
    if _mean_var(on["moments"])[0] <= _mean_var(off["moments"])[0]:
        raise SystemExit(
            "the LED run %s has a charge mean no larger than the noise run %s; "
            "give the noise tag first and the LED tag second" % (led_dir.name, noise_dir.name)
        )

    occupancy, occupancy_err, thr, tot_entries_b, f_array = compute_occupancy(on["counts"], off["counts"], bins, thr_range)
    l_threshold_idx, l_threshold_value = find_lambda(occupancy, occupancy_err, thr, args.max_rel_err)

    calc = {
        "occupancy": occupancy.tolist(),
        "occupancy_err": occupancy_err.tolist(),
        "thr": thr.tolist(),
        "tot_entries_b": float(tot_entries_b),
        "scaling_factor": None,
        "set_occupancy": None,
        "set_occupancy_err": None,
        "l_threshold_idx": None,
        "l_threshold_value": None,
        "EPsi": None,
        "EPsi_stat_err": None,
        "EPsi_sys_err": None,
        "VPsi": None,
        "gain": None,
        "gain_err": None,
        "n_events": n_events,
        "noise_run": noise_dir.name,
        "led_run": led_dir.name,
        "led_window": list(led_window),
        "baseline_samples": args.baseline_samples,
        "charge_bounds": list(charge_bounds),
        "bin_width": args.bin_width,
        "max_rel_err": args.max_rel_err,
        "impedance": args.impedance,
        "amplification": args.amplification,
    }
    if l_threshold_idx is not None:
        set_occupancy = occupancy[l_threshold_idx]
        set_occupancy_err = occupancy_err[l_threshold_idx]
        f = f_array[l_threshold_idx]
        EPsi, VPsi, EPsi_stat_err, EPsi_sys_err = calculate_spe(
            on["moments"], off["moments"], set_occupancy, tot_entries_b, f, set_occupancy_err
        )
        gain = calc_gain(EPsi, args.impedance, args.amplification)
        gain_err = np.hypot(
            calc_gain(EPsi_stat_err, args.impedance, args.amplification),
            calc_gain(EPsi_sys_err, args.impedance, args.amplification),
        )
        calc.update(
            scaling_factor=float(f),
            set_occupancy=float(set_occupancy),
            set_occupancy_err=float(set_occupancy_err),
            l_threshold_idx=int(l_threshold_idx),
            l_threshold_value=float(l_threshold_value),
            EPsi=float(EPsi),
            EPsi_stat_err=float(EPsi_stat_err),
            EPsi_sys_err=float(EPsi_sys_err),
            VPsi=float(VPsi),
            gain=float(gain),
            gain_err=float(gain_err),
        )

    stem = "%s__noise_%s" % (led_dir.name, args.noise_tag)
    calc_path = out_dir / f"{stem}_calc.json"
    calc_path.write_text(json.dumps(calc, indent=2) + "\n", encoding="utf-8")
    print(calc_path)
    _save_charge(on["counts"], off["counts"], bins, calc, args.plot_bin_width, out_dir / f"{stem}_charge.png", args.scale_noise)
    _save_occupancy(calc, out_dir / f"{stem}_occupancy.png")

    if l_threshold_idx is None:
        good = np.flatnonzero(occupancy > 0)
        if good.size:
            rel = occupancy_err[good] / occupancy[good]
            best = good[np.argmin(rel)]
            print(
                "no threshold reaches l_err/l < %g; the smallest is %.4g at %.4g pVs (l = %.4g +- %.4g)"
                % (args.max_rel_err, rel.min(), thr[best], occupancy[best], occupancy_err[best])
            )
        else:
            print("no threshold gives a positive occupancy")
        return 1

    print("N = %d events per run" % n_events)
    print("threshold %.4g pVs (index %d), f = %.4g" % (calc["l_threshold_value"], calc["l_threshold_idx"], calc["scaling_factor"]))
    print("occupancy %.4f +- %.4f" % (calc["set_occupancy"], calc["set_occupancy_err"]))
    print(
        "SPE mean %.2f +- %.2f stat +- %.2f sys pVs; variance %.4g pVs^2"
        % (calc["EPsi"], calc["EPsi_stat_err"], calc["EPsi_sys_err"], calc["VPsi"])
    )
    print("gain %.4e +- %.2e (impedance %g ohm, amplification %g)" % (calc["gain"], calc["gain_err"], args.impedance, args.amplification))
    return 0


def compute_occupancy(hist_on, hist_off, bins, thr_range=None):
    """Occupancy l, its error, and f = Ab/N at each bin edge used as threshold."""
    Ab = np.cumsum(hist_off, dtype=np.float64)
    As = np.cumsum(hist_on, dtype=np.float64)
    thr = np.asarray(bins[1:], dtype=np.float64)
    keep = (Ab > 0) & (As > 0)
    if thr_range is not None:
        keep &= (thr >= thr_range[0]) & (thr <= thr_range[1])
    Ab, As, thr = Ab[keep], As[keep], thr[keep]
    tot_entries_b = float(np.sum(hist_off))
    f = Ab / tot_entries_b
    occupancy = -np.log(As / Ab)
    occupancy_err = np.sqrt((np.exp(occupancy) + 1.0 - 2.0 * f) / Ab)
    return occupancy, occupancy_err, thr, tot_entries_b, f


def find_lambda(occupancy, occupancy_err, thr, max_rel_err=MAX_REL_ERR):
    passing = np.flatnonzero((occupancy > 0) & (occupancy_err < max_rel_err * occupancy))
    if passing.size == 0:
        return None, None
    idx = int(passing[0])
    return idx, float(thr[idx])


def calculate_spe(moments_on, moments_off, set_occupancy, tot_entries_b, f, set_occupancy_err):
    """SPE mean and variance with the statistical and systematic errors of the mean.

    moments_* are (n, sum q, sum q^2) of the LED window charge.
    """
    E_s, V_s = _mean_var(moments_on)
    E_b, V_b = _mean_var(moments_off)
    EPsi = (E_s - E_b) / set_occupancy
    VPsi = (V_s - V_b) / set_occupancy - EPsi**2
    EPsi_stat_err = np.sqrt(
        (set_occupancy * (EPsi**2 + VPsi) + 2.0 * V_b) / (tot_entries_b * set_occupancy**2)
        + (EPsi * EPsi * (np.exp(set_occupancy) + 1.0 - 2.0 * f)) / (f * tot_entries_b * set_occupancy**2)
    )
    EPsi_sys_err = (E_s - E_b) * set_occupancy_err / (set_occupancy**2)
    return EPsi, VPsi, EPsi_stat_err, EPsi_sys_err


def calc_gain(EPsi, impedance=IMPEDANCE, amplification=1.0):
    """Gain from an SPE mean in pVs (mV x ns)."""
    return EPsi * 1e-12 / (impedance * amplification * E_CHARGE)


def _mean_var(moments):
    n, s1, s2 = moments
    mean = s1 / n
    return mean, s2 / n - mean**2


def _pair(value, name: str):
    try:
        lo, hi = float(value[0]), float(value[1])
    except (TypeError, ValueError, IndexError):
        raise SystemExit("%s must be two numbers" % name) from None
    if hi <= lo:
        raise SystemExit("%s HI must be greater than LO" % name)
    return (lo, hi)


def _window(value, name: str):
    try:
        start, stop = int(value[0]), int(value[1])
    except (TypeError, ValueError, IndexError):
        raise SystemExit("%s must be two integers" % name) from None
    if start < 0 or stop <= start:
        raise SystemExit("%s stop must be greater than start, and start must be non-negative" % name)
    return (start, stop)


def _charge_bins(bounds, width: float) -> np.ndarray:
    n_bins = int(np.ceil((bounds[1] - bounds[0]) / width - 1e-9))
    return bounds[0] + np.arange(n_bins + 1) * width


def _resolve_tag(data_dir: Path, tag: str) -> Path:
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
    print("%s -> %s" % (tag, matches[0].name))
    return matches[0]


def _metadata(run_dir: Path) -> dict:
    return json.loads((run_dir / "metadata.json").read_text(encoding="utf-8"))


def _event_count(run_dir: Path) -> int:
    written = _metadata(run_dir).get("events_written")
    if written is not None:
        return int(written)
    return sum(load_waveforms(str(path))[0].shape[0] for path in sorted(run_dir.glob("wf_*.bin")))


def _charges(run_dir: Path, n_events: int, baseline_samples: int, led_window, bins) -> dict:
    metadata = _metadata(run_dir)
    scale = _millivolt_scale(metadata)
    dt = float(metadata.get("sample_period_ns") or 2)
    files = sorted(run_dir.glob("wf_*.bin"))
    if not files:
        raise SystemExit("no wf_*.bin in %s" % run_dir)

    counts = np.zeros(len(bins) - 1, dtype=np.float64)
    s1 = 0.0
    s2 = 0.0
    taken = 0
    for path in files:
        if taken >= n_events:
            break
        waveforms, _ = load_waveforms(str(path))
        if waveforms.shape[2] < led_window[1]:
            raise SystemExit("LED window stop %d exceeds %d samples in %s" % (led_window[1], waveforms.shape[2], path))
        samples = np.asarray(waveforms[: n_events - taken, 0, :], dtype=np.float32)
        del waveforms
        baseline = samples[:, :baseline_samples].mean(axis=1)
        samples -= baseline[:, None]
        samples *= scale
        charge = samples[:, led_window[0]:led_window[1]].sum(axis=1, dtype=np.float64) * dt
        del samples
        counts += np.histogram(charge, bins=bins)[0]
        s1 += float(np.sum(charge))
        s2 += float(np.sum(charge**2))
        taken += charge.size
    if taken < n_events:
        raise SystemExit("%s has only %d events in its wf_*.bin files, expected %d" % (run_dir.name, taken, n_events))
    print("  %s: %d events, charge mean %.4g pVs" % (run_dir.name, taken, s1 / taken))
    return {"name": run_dir.name, "counts": counts, "moments": (taken, s1, s2), "dt": dt}


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


def _rebin(counts, bins, width: float):
    factor = max(1, int(round(width / (bins[1] - bins[0]))))
    starts = np.arange(0, len(counts), factor)
    return np.add.reduceat(counts, starts), np.append(bins[starts], bins[-1])


def _save_charge(hist_on, hist_off, bins, calc: dict, plot_bin_width: float, path: Path, scale_noise: bool = False) -> None:
    fig, ax = plt.subplots(figsize=(8, 4.5))
    noise_label = "noise"
    if scale_noise and calc["set_occupancy"] is not None:
        noise_scale = np.exp(-calc["set_occupancy"])
        hist_off = hist_off * noise_scale
        noise_label = "noise x %.4g" % noise_scale
    for label, counts in (("LED", hist_on), (noise_label, hist_off)):
        heights, edges = _rebin(counts, bins, plot_bin_width)
        heights = heights.astype(float)
        heights[heights <= 0] = np.nan
        ax.stairs(heights, edges, label=label)
    ax.set_yscale("log")
    ax.set_xlabel("Charge [pVs]")
    ax.set_ylabel("Counts / %g pVs" % (edges[1] - edges[0]))

    occupancy = np.asarray(calc["occupancy"])
    occupancy_err = np.asarray(calc["occupancy_err"])
    thr = np.asarray(calc["thr"])
    positive = occupancy > 0
    rel = ax.twinx()
    rel.plot(thr[positive], occupancy_err[positive] / occupancy[positive], color="k", linewidth=0.8, label="l_err / l")
    rel.axhline(calc["max_rel_err"], color="0.5", ls="--", label="%g%% error" % (100 * calc["max_rel_err"]))
    if calc["l_threshold_value"] is not None:
        rel.axvline(calc["l_threshold_value"], color="0.5", ls=":", label="threshold %.4g pVs" % calc["l_threshold_value"])
    rel.set_yscale("log")
    rel.set_ylabel("Relative occupancy error")
    handles, labels = ax.get_legend_handles_labels()
    rel_handles, rel_labels = rel.get_legend_handles_labels()
    ax.legend(handles + rel_handles, labels + rel_labels)
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    plt.close(fig)
    print(path)


def _save_occupancy(calc: dict, path: Path) -> None:
    occupancy = np.asarray(calc["occupancy"])
    occupancy_err = np.asarray(calc["occupancy_err"])
    thr = np.asarray(calc["thr"])
    step = max(1, len(thr) // 100)
    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax.errorbar(thr[::step], occupancy[::step], yerr=occupancy_err[::step], fmt="o", markersize=3, capsize=2)
    if calc["l_threshold_value"] is not None:
        ax.axvline(
            calc["l_threshold_value"],
            color="0.5",
            ls="--",
            label="threshold %.4g pVs, l = %.4f +- %.4f"
            % (calc["l_threshold_value"], calc["set_occupancy"], calc["set_occupancy_err"]),
        )
        ax.legend()
    ax.set_xlabel("Charge threshold [pVs]")
    ax.set_ylabel("Occupancy")
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    plt.close(fig)
    print(path)


if __name__ == "__main__":
    sys.exit(main())
