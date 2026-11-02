"""LED spectrum figures."""

from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import ListedColormap

from spe_fit_funcs import fit_spe

def _coolwarm_colors(n: int) -> np.ndarray:
    colors = plt.cm.coolwarm(np.arange(plt.cm.coolwarm.N))
    colors[:, 0:3] *= 0.85
    coolwarm = ListedColormap(colors)
    return coolwarm(np.linspace(0, 1, n))

def _save_hist(series, path: Path, xlabel: str, ylabel: str, ylim: tuple[float, float] = None, cmap: bool = False) -> None:
    fig, ax = plt.subplots(figsize=(8, 4.5))
    colors = _coolwarm_colors(len(series)) if cmap and len(series) > 1 else None
    for index, (label, counts, edges) in enumerate(series):
        heights = counts.astype(float)
        heights[heights <= 0] = np.nan
        if colors is None:
            ax.stairs(heights, edges, label=label)
        else:
            ax.stairs(heights, edges, label=label, color=colors[index])
    if any(np.any(counts > 0) for _label, counts, _edges in series):
        ax.set_yscale("log")
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    if ylim is not None:
        ax.set_ylim(ylim)
    ax.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    plt.close(fig)
    print(path)

def _save_average(spectra, path: Path, cmap: bool = False) -> None:
    fig, ax = plt.subplots(figsize=(8, 4.5))
    lines = []
    for item in spectra:
        time = np.arange(len(item["average"])) * item["dt"]
        line, = ax.plot(time, item["average"], label=item["label"])
        lines.append(line)
    dt = spectra[0]["dt"]
    windows = [tuple(item["led_window"]) for item in spectra]
    if len(spectra) == 1:
        ax.axvspan(windows[0][0] * dt, windows[0][1] * dt, color="C0", alpha=0.15, label="LED window")
        noise_window = spectra[0]["noise_window"]
        ax.axvspan(noise_window[0] * dt, noise_window[1] * dt, color="C1", alpha=0.15, label="noise window")
    elif len(set(windows)) > 1:
        for item, line in zip(spectra, lines):
            ax.axvspan(item["led_window"][0] * item["dt"], item["led_window"][1] * item["dt"], color=line.get_color(), alpha=0.15)
    if cmap and len(spectra) > 1:
        for line, color in zip(lines, _coolwarm_colors(len(spectra))):
            line.set_color(color)
    ax.axhline(0, color="0.5", linewidth=0.8)
    ax.set_xlabel("Time [ns]")
    ax.set_ylabel("mV")
    sample_axis = ax.secondary_xaxis("top", functions=(lambda time: time / dt, lambda sample: sample * dt))
    sample_axis.set_xlabel("Samples")
    ax.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    plt.close(fig)
    print(path)

def _save_waveforms(before: np.ndarray, after: np.ndarray, dt: float, path: Path) -> None:
    fig, axes = plt.subplots(2, 1, sharex=True, figsize=(8, 8))
    time = np.arange(before.shape[1]) * dt
    for ax, raw, filtered in zip(axes, before, after):
        ax.plot(time, raw, label="unfiltered")
        ax.plot(time, filtered, label="filtered")
        ax.axhline(0, color="0.5", linewidth=0.8)
        ax.set_ylabel("mV")
        ax.legend()
    axes[-1].set_xlabel("Time [ns]")
    sample_axis = axes[0].secondary_xaxis("top", functions=(lambda time: time / dt, lambda sample: sample * dt))
    sample_axis.set_xlabel("Samples")
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    plt.close(fig)
    print(path)

def _fit_and_save(charge, out_dir: Path, name: str, fit_range, mu_bounds, n_gaussians: int, bins, xlim) -> None:
    for linear, suffix in ((False, "fit"), (True, "fit_linear")):
        print("fit linear=%s" % linear)
        fit_values, fig, ax = fit_spe(
            charge,
            n_gaussians=n_gaussians,
            linear=linear,
            plot=True,
            print_fit=True,
            fit_range=fit_range,
            mu_bounds=mu_bounds,
            pedestal=True,
            shared_sigma=False,
            xlabel="Charge [pVs]",
            bins=bins,
        )
        ax.set_xlim(*xlim)
        path = out_dir / f"{name}_{suffix}.png"
        fig.savefig(path, dpi=120)
        plt.close(fig)
        print(path)
