from scipy.optimize import curve_fit
import numpy as np
import matplotlib.pyplot as plt

def fit_spe(amplitudes, n_gaussians=3, linear=True, plot=True, print_fit=True,
            fit_range=(1.0, 10.0), bins=None, mu_bounds=(1.8, 4.0),
            pedestal=False, n_bins=200, sigma_over_mu_bounds=(0.02, 1.0),
            ylim=(0.5, 2e4), shared_sigma=True, xlabel="Amplitude [mV]"):
    """Fit photoelectron Gaussians at mu, 2*mu, 3*mu, ...

    shared_sigma fits one sigma/mu for every peak. Set it False to fit each
    peak width on its own. pedestal adds an independent Gaussian fixed at zero.
    Both are off except shared_sigma. When bins is omitted, the histogram stays
    on 0..12 as long as fit_range lies inside it; a wider fit_range is
    histogrammed over that range instead.
    """
    if n_gaussians < 1:
        raise ValueError("n_gaussians must be at least 1")
    if bins is None:
        bins = np.linspace(0, 12, n_bins)
        if fit_range[0] < bins[0] or fit_range[1] > bins[-1]:
            bins = np.linspace(fit_range[0], fit_range[1], n_bins)

    def model(x, *params):
        mu = params[1]
        amps = (params[0],) + tuple(params[3:2 + n_gaussians])
        if shared_sigma:
            sigmas = [params[2] * k * mu for k in range(1, n_gaussians + 1)]
            cursor = 2 + n_gaussians
        else:
            sigmas = [params[2], *params[2 + n_gaussians:1 + 2 * n_gaussians]]
            cursor = 1 + 2 * n_gaussians
        y = np.zeros_like(x, dtype=float)
        for k, (amp, sigma) in enumerate(zip(amps, sigmas), start=1):
            y = y + amp * np.exp(-0.5 * ((x - k * mu) / sigma) ** 2)
        if pedestal:
            a0, sigma0 = params[cursor], params[cursor + 1]
            y = y + a0 * np.exp(-0.5 * (x / sigma0) ** 2)
            cursor += 2
        if linear:
            y = y + params[cursor] * x + params[cursor + 1]
        return y

    def model_log(x, *params):
        return np.log(np.maximum(model(x, *params), 1e-12))

    counts, edges = np.histogram(amplitudes, bins=bins)
    centers = 0.5 * (edges[:-1] + edges[1:])
    mask = (centers >= fit_range[0]) & (centers <= fit_range[1]) & (counts > 0)
    x = centers[mask]
    y = counts[mask].astype(float)

    if len(y) == 0:
        raise ValueError("no histogram bins with counts inside fit_range")
    mu_window = (x >= mu_bounds[0]) & (x <= mu_bounds[1])
    a1_0 = y[mu_window].max() if np.any(mu_window) else y.max()
    mu_guess = 2.5 if mu_bounds[0] < 2.5 < mu_bounds[1] else 0.5 * (mu_bounds[0] + mu_bounds[1])
    mu0 = float(np.clip(mu_guess, mu_bounds[0] + 1e-3, mu_bounds[1] - 1e-3))
    sigma_ratio_0 = float(np.clip(0.16, sigma_over_mu_bounds[0] * 1.01, sigma_over_mu_bounds[1] * 0.99))
    extra_amps = [a1_0 / (5 * k) for k in range(1, n_gaussians)]
    if shared_sigma:
        p0 = [a1_0, mu0, sigma_ratio_0] + extra_amps
        lower = [0.0, mu_bounds[0], sigma_over_mu_bounds[0]] + [0.0] * (n_gaussians - 1)
        upper = [np.inf, mu_bounds[1], sigma_over_mu_bounds[1]] + [np.inf] * (n_gaussians - 1)
    else:
        sigma_guess = [sigma_ratio_0 * k * mu0 for k in range(1, n_gaussians + 1)]
        sigma_lo = [sigma_over_mu_bounds[0] * k * mu0 for k in range(1, n_gaussians + 1)]
        sigma_hi = [sigma_over_mu_bounds[1] * k * mu0 for k in range(1, n_gaussians + 1)]
        p0 = [a1_0, mu0, sigma_guess[0]] + extra_amps + sigma_guess[1:]
        lower = [0.0, mu_bounds[0], sigma_lo[0]] + [0.0] * (n_gaussians - 1) + sigma_lo[1:]
        upper = [np.inf, mu_bounds[1], sigma_hi[0]] + [np.inf] * (n_gaussians - 1) + sigma_hi[1:]
    if pedestal:
        span = float(fit_range[1] - fit_range[0])
        near_zero = np.abs(x) <= max(span * 0.05, 1e-6)
        a0_0 = float(y[near_zero].max() if np.any(near_zero) else max(y.max() * 0.1, 1.0))
        sigma0_0 = span * 0.02
        p0 += [a0_0, sigma0_0]
        lower += [0.0, span * 1e-3]
        upper += [np.inf, span * 0.5]
    if linear:
        p0 += [0.0, max(y.min(), 1.0)]
        lower += [-np.inf, 0.0]
        upper += [np.inf, np.inf]

    popt, pcov = curve_fit(
        model_log, x, np.log(y), p0=p0, sigma=1.0 / np.sqrt(y),
        absolute_sigma=True, bounds=(lower, upper), maxfev=20000,
    )
    perr = np.sqrt(np.diag(pcov))
    residual = (model_log(x, *popt) - np.log(y)) * np.sqrt(y)
    chi2 = float(np.sum(residual ** 2))
    ndf = len(y) - len(popt)
    chi2_red = chi2 / ndf
    mu = popt[1]
    amps = np.array([popt[0], *popt[3:2 + n_gaussians]])
    amp_errs = np.array([perr[0], *perr[3:2 + n_gaussians]])
    if shared_sigma:
        sigma_over_mu = popt[2]
        sigma_over_mu_err = perr[2]
        sigmas = sigma_over_mu * np.arange(1, n_gaussians + 1) * mu
        sigma_errs = None
        cursor = 2 + n_gaussians
    else:
        sigma_over_mu = sigma_over_mu_err = None
        sigmas = np.array([popt[2], *popt[2 + n_gaussians:1 + 2 * n_gaussians]])
        sigma_errs = np.array([perr[2], *perr[2 + n_gaussians:1 + 2 * n_gaussians]])
        cursor = 1 + 2 * n_gaussians
    a0 = a0_err = sigma0 = sigma0_err = None
    if pedestal:
        a0, sigma0 = popt[cursor], popt[cursor + 1]
        a0_err, sigma0_err = perr[cursor], perr[cursor + 1]
        cursor += 2
    slope = intercept = slope_err = intercept_err = None
    if linear:
        slope, intercept = popt[cursor], popt[cursor + 1]
        slope_err, intercept_err = perr[cursor], perr[cursor + 1]

    fit_values = {
        "mu": mu,
        "mu_err": perr[1],
        "sigma_over_mu": sigma_over_mu,
        "sigma_over_mu_err": sigma_over_mu_err,
        "amplitudes": amps,
        "amplitude_errs": amp_errs,
        "sigmas": sigmas,
        "sigma_errs": sigma_errs,
        "pedestal_amplitude": a0,
        "pedestal_amplitude_err": a0_err,
        "pedestal_sigma": sigma0,
        "pedestal_sigma_err": sigma0_err,
        "slope": slope,
        "slope_err": slope_err,
        "intercept": intercept,
        "intercept_err": intercept_err,
        "popt": popt,
        "perr": perr,
        "chi2": chi2,
        "ndf": ndf,
        "chi2_red": chi2_red,
    }

    if print_fit:
        print(f"mu = {mu:.3f} ± {perr[1]:.3f}")
        print(", ".join(f"{k}*mu = {k * mu:.3f}" for k in range(2, n_gaussians + 1)))
        if shared_sigma:
            print(f"sigma/mu = {sigma_over_mu:.3f} ± {sigma_over_mu_err:.3f}")
            print("sigmas = " + ", ".join(f"{s:.3f}" for s in sigmas))
        else:
            print("sigmas = " + ", ".join(f"{s:.3f} ± {e:.3f}" for s, e in zip(sigmas, sigma_errs)))
        print("amplitudes = " + ", ".join(f"{a:.1f} ± {e:.1f}" for a, e in zip(amps, amp_errs)))
        if pedestal:
            print(f"pedestal = {a0:.1f} ± {a0_err:.1f}, sigma0 = {sigma0:.3f} ± {sigma0_err:.3f}")
        if linear:
            print(f"slope = {slope:.2f} ± {slope_err:.2f}, intercept = {intercept:.1f} ± {intercept_err:.1f}")
        print(f"chi2_red = {chi2_red:.3f} ({chi2:.1f} / {ndf})")

    if not plot:
        return fit_values

    yerr = np.sqrt(counts)
    x_step = np.repeat(edges, 2)[1:-1]
    y_step = np.repeat(counts.astype(float), 2)
    e_step = np.repeat(yerr, 2)
    visible = y_step > 0
    lower_band = np.where(visible, np.maximum(y_step - e_step, 1e-3), np.nan)
    upper_band = np.where(visible, y_step + e_step, np.nan)
    xx = np.linspace(fit_range[0], fit_range[1], 400)

    fig, ax = plt.subplots()
    ax.fill_between(x_step, lower_band, upper_band, color="0.75", linewidth=0)
    ax.plot(x_step, np.where(visible, y_step, np.nan), color="black", lw=1, label="data")
    ax.plot(
        xx,
        model(xx, *popt),
        label=f"fit\nmu = {mu:.3f} ± {perr[1]:.3f}\nchi2_red = {chi2_red:.3f}",
    )
    for k, (amp, sigma) in enumerate(zip(amps, sigmas), start=1):
        ax.plot(xx, amp * np.exp(-0.5 * ((xx - k * mu) / sigma) ** 2), "--", label=f"{k} PE")
    if pedestal:
        ax.plot(xx, a0 * np.exp(-0.5 * (xx / sigma0) ** 2), "--", label="pedestal")
    if linear:
        ax.plot(xx, np.maximum(slope * xx + intercept, 1e-12), ":", label="linear")
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Counts")
    ax.set_yscale("log")
    in_fit = (centers >= fit_range[0]) & (centers <= fit_range[1])
    peak = counts[in_fit].max() if np.any(in_fit) else 0
    top = None if ylim is None else ylim[1]
    if top is not None and peak > top:
        top = peak * 1.3
    bottom = 0.5 if ylim is None else ylim[0]
    ax.set_ylim(bottom, top)
    ax.legend()
    return fit_values, fig, ax


def plot_region_waveforms(regions, amplitudes, wfs, baselines=None, which=0,
                          yscale="mV", dynamic_range=2):
    """Plot one waveform from each amplitude region.

    regions : dict of name -> array
        Either the cut amplitude values (as in amplitudes_spe) or a boolean
        mask of the same length as amplitudes.
    amplitudes : array aligned with wfs, the one the regions were cut from.
    which : int position inside each region, or "random".
    Returns the chosen wfs index for each region, plus fig and axes.
    """
    if not isinstance(regions, dict):
        regions = {"region": regions}
    amplitudes = np.asarray(amplitudes)

    def indices_for(region):
        region = np.asarray(region)
        if region.dtype == bool:
            return np.flatnonzero(region)
        return np.flatnonzero(np.isin(amplitudes, region))

    fig, axes = plt.subplots(len(regions), 1, sharex=True, figsize=(8, 2.4 * len(regions)), squeeze=False)
    chosen = {}
    for ax, (name, region) in zip(axes[:, 0], regions.items()):
        matches = indices_for(region)
        if len(matches) == 0:
            raise ValueError(f"no waveform in region {name!r}")
        i = int(np.random.choice(matches)) if which == "random" else int(matches[which])
        chosen[name] = i
        wf = wfs[i].astype(float)
        if baselines is not None:
            wf = wf - baselines[i]
        if yscale == "mV":
            wf = wf * dynamic_range / 2**14 * 1000
            ax.set_ylabel("mV")
        else:
            ax.set_ylabel("ADC counts")
        ax.plot(wf, color="black", lw=1)
        ax.set_title(f"{name}   wfs[{i}]   amplitude {amplitudes[i]:.2f}   ({len(matches)} events)")
    axes[-1, 0].set_xlabel("Samples")
    fig.tight_layout()
    return chosen, fig, axes