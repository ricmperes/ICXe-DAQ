#!/usr/bin/env python3
"""Build the LED charge spectrum from one or more run-name tags.

One tag loads that run, writes the amplitude and charge spectra, the average
waveform, and the SPE fits. Several tags draw one spectrum line per run and
do not fit.

A tag is a unique substring of a run directory name under data/. The same
settings can be read from a JSON file; a run entry may set its own led_window,
amp_bounds, and charge_bounds. The top-level softfilter flag low-passes each
waveform at cutoff_mhz (default 10 MHz) before the spectra are built. The
top-level cmap flag colors compare-plot lines with the coolwarm colormap.

    python led_spectrum.py --config led_spectrum.example.json
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

from led_spectrum_core import (
    BASELINE_SAMPLES,
    CUTOFF_MHZ,
    FIT_SPAN,
    MU_SPAN,
    _load_config,
    _output_stem,
    _resolve_tag,
    _run_list,
    _span,
    _spectrum,
    _spectrum_edges,
)
from led_spectrum_plots import (
    _fit_and_save,
    _save_average,
    _save_hist,
    _save_waveforms,
)


def default_data_dir() -> Path:
    return Path(__file__).resolve().parents[1] / "data"

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tags", nargs="*", help="unique substring of a run directory name")
    parser.add_argument("--config", type=Path, default=None, help="JSON file of runs and shared settings; flags override it")
    parser.add_argument("--data", type=Path, default=None, help="run directory root")
    parser.add_argument("--out", type=Path, default=None, help="directory for the png files")
    parser.add_argument(
        "--amp-bounds",
        type=float,
        nargs=2,
        default=None,
        metavar=("LO", "HI"),
        help="amplitude histogram range in mV (default 0 10); a config run can override it. The default charge window, fit range, and mu bounds scale by HI/10",
    )
    parser.add_argument(
        "--charge-bounds",
        type=float,
        nargs=2,
        default=None,
        metavar=("LO", "HI"),
        help="charge histogram range in pVs (default -40 250, scaled with the amplitude bound); a config run can override it. The zoom runs from LO to HI/2",
    )
    parser.add_argument(
        "--fit-range",
        type=float,
        nargs=2,
        default=None,
        metavar=("LO", "HI"),
        help="charge fit range in pVs (default -50 250, scaled with --amp-bounds)",
    )
    parser.add_argument(
        "--mu-bounds",
        type=float,
        nargs=2,
        default=None,
        metavar=("LO", "HI"),
        help="mu bounds in pVs (default 40 60, scaled with --amp-bounds)",
    )
    parser.add_argument("--n-gaussians", type=int, default=None)
    parser.add_argument("--baseline-samples", type=int, default=None)
    parser.add_argument(
        "--led-window",
        type=int,
        nargs=2,
        default=None,
        metavar=("START", "STOP"),
        help="shared LED sample window (default 580 620); a config run can override it",
    )
    parser.add_argument(
        "--label",
        action="append",
        default=None,
        metavar="TEXT",
        help="legend label for one run, in tag order; repeat once per run, or omit to use the directory names",
    )
    args = parser.parse_args(argv)
    cfg = _load_config(args.config)
    runs = _run_list(args, cfg)

    data_dir = args.data or (Path(cfg["data"]) if "data" in cfg else default_data_dir())
    out_dir = args.out or (
        Path(cfg["out"]) if "out" in cfg else Path(__file__).resolve().parents[1] / "plots" / "led_spectrum"
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    baseline_samples = args.baseline_samples if args.baseline_samples is not None else cfg.get("baseline_samples", BASELINE_SAMPLES)
    n_gaussians = args.n_gaussians if args.n_gaussians is not None else cfg.get("n_gaussians", 3)
    softfilter = cfg.get("softfilter", False)
    cutoff_mhz = cfg.get("cutoff_mhz", CUTOFF_MHZ)
    cmap = cfg.get("cmap", False)

    spectra = []
    for run in runs:
        edges = _spectrum_edges(run["amp_bounds"], run["charge_bounds"])
        path = _resolve_tag(data_dir, run["tag"], run["led_window"])
        print(
            "  amplitude %.4g to %.4g mV; charge %.4g to %.4g pVs"
            % (edges["amp_bins"][0], edges["amp_bins"][-1], edges["charge_bins"][0], edges["charge_bins"][-1])
        )
        item = _spectrum(
            path,
            baseline_samples=baseline_samples,
            led_window=run["led_window"],
            noise_window=run["noise_window"],
            keep_charge=len(runs) == 1,
            amp_bins=edges["amp_bins"],
            charge_bins=edges["charge_bins"],
            zoom_bins=edges["zoom_bins"],
            softfilter=softfilter,
            cutoff_mhz=cutoff_mhz,
        )
        item["label"] = run.get("label") or item["name"]
        item["led_window"] = run["led_window"]
        item["noise_window"] = run["noise_window"]
        item["amp_bins"] = edges["amp_bins"]
        item["charge_bins"] = edges["charge_bins"]
        item["zoom_bins"] = edges["zoom_bins"]
        item["scale"] = edges["scale"]
        spectra.append(item)
    single = len(spectra) == 1
    base = spectra[0]["name"] if single else "compare_" + "__".join(run["tag"] for run in runs)
    stem = _output_stem(base, softfilter)
    compare_cmap = cmap and not single

    _save_hist(
        [(item["label"], item["amplitude_counts"], item["amp_bins"]) for item in spectra],
        out_dir / f"{stem}_amplitude.png",
        "Amplitude [mV]",
        "Counts",
        cmap=compare_cmap,
    )
    charge_series = [(item["label"], item["led_counts"], item["charge_bins"]) for item in spectra]
    zoom_series = [(item["label"], item["led_zoom_counts"], item["zoom_bins"]) for item in spectra]
    if single:
        charge_series.append((spectra[0]["label"] + " noise", spectra[0]["noise_counts"], spectra[0]["charge_bins"]))
        zoom_series.append((spectra[0]["label"] + " noise", spectra[0]["noise_zoom_counts"], spectra[0]["zoom_bins"]))
    _save_hist(charge_series, out_dir / f"{stem}_charge.png", "Charge [pVs]", "Counts", cmap=compare_cmap)
    _save_hist(zoom_series, out_dir / f"{stem}_charge_zoom.png", "Charge [pVs]", "Counts", ylim=(100, None), cmap=compare_cmap)
    _save_average(spectra, out_dir / f"{stem}_average.png", cmap=compare_cmap)
    if softfilter:
        check = spectra[0]["filter_check"]
        _save_waveforms(check["before"], check["after"], check["dt"], out_dir / f"{stem}_waveforms.png")

    if single:
        scale = spectra[0]["scale"]
        fit_range = _span(args.fit_range, cfg.get("fit_range"), tuple(edge * scale for edge in FIT_SPAN), "fit_range")
        mu_bounds = _span(args.mu_bounds, cfg.get("mu_bounds"), tuple(edge * scale for edge in MU_SPAN), "mu_bounds")
        charge_bins = spectra[0]["charge_bins"]
        _fit_and_save(
            spectra[0]["led"],
            out_dir,
            stem,
            fit_range=fit_range,
            mu_bounds=mu_bounds,
            n_gaussians=n_gaussians,
            bins=charge_bins,
            xlim=(float(charge_bins[0]), float(charge_bins[-1])),
        )
    print("wrote %s" % out_dir)
    return 0

if __name__ == "__main__":
    sys.exit(main())
