#!/usr/bin/env python3
"""
analyze_plot.py — EMA wake-up delay analysis and result plotting.

Reads CSVs produced by client.c (via run_experiments.sh) and generates:
  1. RTT vs message size  — PS on vs off, per proc_delay
  2. RTT CDF              — all conditions overlaid
  3. Wake-up delay time series with EMA
  4. EMA prediction accuracy (predicted vs actual next delay)
  5. RTT box plots by inter-message gap (PS on vs off)

Usage:
  python3 analyze_plot.py --results-dir results [--alpha 0.3] [--out-dir results/plots]
"""

import os
import glob
import argparse
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.lines import Line2D

# ── plot style ────────────────────────────────────────────────────────────────
plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.titlesize": 11,
    "axes.labelsize": 10,
    "legend.fontsize": 9,
    "figure.dpi": 150,
    "lines.linewidth": 1.4,
    "axes.grid": True,
    "grid.alpha": 0.35,
})

COLORS = {
    "off": "#2166ac",   # blue  — PS off (baseline)
    "on":  "#d6604d",   # red   — PS on
}
SIZE_COLORS = plt.cm.viridis(np.linspace(0.15, 0.85, 5))


# ── EMA helper ────────────────────────────────────────────────────────────────
def ema(series: np.ndarray, alpha: float) -> np.ndarray:
    """Exponential moving average.  ema[i] = α·x[i] + (1−α)·ema[i-1]"""
    out = np.empty_like(series, dtype=float)
    out[0] = series[0]
    for i in range(1, len(series)):
        out[i] = alpha * series[i] + (1 - alpha) * out[i - 1]
    return out


def ema_prediction_error(delays: np.ndarray, alpha: float) -> tuple:
    """
    Treat EMA[i] as the prediction for delay[i+1].
    Returns (predicted, actual, abs_error) arrays.
    """
    e = ema(delays, alpha)
    predicted = e[:-1]           # ema[i] predicts delay[i+1]
    actual    = delays[1:]
    err       = np.abs(actual - predicted)
    return predicted, actual, err


# ── data loading ──────────────────────────────────────────────────────────────
def load_data(results_dir: str) -> pd.DataFrame:
    """Load all per-run CSVs and concatenate."""
    pattern = os.path.join(results_dir, "csv", "*.csv")
    files = sorted(glob.glob(pattern))
    if not files:
        raise FileNotFoundError(f"No CSV files found in {pattern}")

    frames = []
    for f in files:
        try:
            df = pd.read_csv(f)
            frames.append(df)
        except Exception as e:
            print(f"  WARNING: could not read {f}: {e}")
    if not frames:
        raise ValueError("No valid CSV files loaded.")

    data = pd.concat(frames, ignore_index=True)
    data["power_save"] = data["power_save"].astype(int)
    data["ps_label"]   = data["power_save"].map({0: "off", 1: "on"})
    return data


# ── wake-up delay extraction ──────────────────────────────────────────────────
def compute_wakeup_delay(df: pd.DataFrame) -> pd.DataFrame:
    """
    Estimate per-message wake-up delay:
        wakeup_delay = RTT − processing_delay − estimated_tx_overhead
    The transmission overhead is estimated from the PS-off median RTT for
    the same (msg_size, proc_delay) combination.
    """
    baseline = (df[df["ps_label"] == "off"]
                .groupby(["msg_size_bytes", "processing_delay_ms"])["rtt_ms"]
                .median()
                .rename("baseline_rtt_ms")
                .reset_index())

    merged = df.merge(baseline, on=["msg_size_bytes", "processing_delay_ms"], how="left")
    merged["wakeup_delay_ms"] = (merged["rtt_ms"] - merged["baseline_rtt_ms"]).clip(lower=0)
    return merged


# ── Figure 1: RTT vs message size ─────────────────────────────────────────────
def plot_rtt_vs_size(df: pd.DataFrame, out_dir: str):
    proc_delays = sorted(df["processing_delay_ms"].unique())
    n = len(proc_delays)
    fig, axes = plt.subplots(1, n, figsize=(4.5 * n, 4.2), sharey=False)
    if n == 1:
        axes = [axes]

    for ax, pd_val in zip(axes, proc_delays):
        sub = df[df["processing_delay_ms"] == pd_val]
        for ps_label, grp in sub.groupby("ps_label"):
            stats = grp.groupby("msg_size_bytes")["rtt_ms"].agg(
                median="median", p25=lambda x: np.percentile(x, 25),
                p75=lambda x: np.percentile(x, 75))
            sizes = stats.index.values
            ax.plot(sizes, stats["median"], marker="o", color=COLORS[ps_label],
                    label=f"PS {ps_label}")
            ax.fill_between(sizes, stats["p25"], stats["p75"],
                            color=COLORS[ps_label], alpha=0.2)

        ax.set_xscale("log", base=2)
        ax.set_xlabel("Message size (bytes)")
        ax.set_ylabel("RTT (ms)")
        ax.set_title(f"proc_delay = {int(pd_val)} ms")
        ax.legend()

    fig.suptitle("RTT vs Message Size — median ± IQR", fontweight="bold")
    fig.tight_layout()
    path = os.path.join(out_dir, "fig1_rtt_vs_size.png")
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved {path}")


# ── Figure 2: RTT CDF ─────────────────────────────────────────────────────────
def plot_cdf(df: pd.DataFrame, out_dir: str):
    proc_delays = sorted(df["processing_delay_ms"].unique())
    n = len(proc_delays)
    fig, axes = plt.subplots(1, n, figsize=(4.5 * n, 4.0))
    if n == 1:
        axes = [axes]

    for ax, pd_val in zip(axes, proc_delays):
        sub = df[df["processing_delay_ms"] == pd_val]
        for ps_label, grp in sub.groupby("ps_label"):
            rtts = np.sort(grp["rtt_ms"].values)
            cdf  = np.arange(1, len(rtts) + 1) / len(rtts)
            ax.plot(rtts, cdf, color=COLORS[ps_label], label=f"PS {ps_label}")

        ax.set_xlabel("RTT (ms)")
        ax.set_ylabel("CDF")
        ax.set_title(f"proc_delay = {int(pd_val)} ms")
        ax.set_ylim(0, 1.02)
        ax.legend()

    fig.suptitle("RTT CDF by Power-Save Mode", fontweight="bold")
    fig.tight_layout()
    path = os.path.join(out_dir, "fig2_rtt_cdf.png")
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved {path}")


# ── Figure 3: Wake-up delay + EMA time series ─────────────────────────────────
def plot_wakeup_ema(df: pd.DataFrame, alpha: float, out_dir: str):
    """One panel per (proc_delay, inter_gap) for PS-on data."""
    ps_df = df[(df["ps_label"] == "on")].copy()
    if ps_df.empty:
        print("  WARNING: no PS-on data for wake-up delay plot")
        return

    groups = sorted(ps_df.groupby(["processing_delay_ms", "inter_gap_ms"]).groups.keys())
    n = len(groups)
    cols = min(n, 3)
    rows = (n + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(5 * cols, 3.5 * rows))
    axes_flat = np.array(axes).flatten() if n > 1 else [axes]

    for ax, (pd_val, gap_val) in zip(axes_flat, groups):
        grp = ps_df[(ps_df["processing_delay_ms"] == pd_val) &
                    (ps_df["inter_gap_ms"] == gap_val)].sort_values("msg_index")
        delays = grp["wakeup_delay_ms"].values
        e = ema(delays, alpha)
        x = np.arange(len(delays))

        ax.plot(x, delays, color="#aaaaaa", linewidth=0.6, label="raw delay", zorder=1)
        ax.plot(x, e,      color=COLORS["on"], linewidth=1.6, label=f"EMA α={alpha:.2f}", zorder=2)
        ax.set_xlabel("Message index")
        ax.set_ylabel("Wake-up delay (ms)")
        ax.set_title(f"proc={int(pd_val)}ms  gap={int(gap_val)}ms")
        ax.legend(fontsize=8)

    # hide unused axes
    for ax in axes_flat[len(groups):]:
        ax.set_visible(False)

    fig.suptitle(f"Wake-up Delay & EMA (α={alpha})  —  PS on", fontweight="bold")
    fig.tight_layout()
    path = os.path.join(out_dir, "fig3_wakeup_ema.png")
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved {path}")


# ── Figure 4: EMA prediction accuracy ────────────────────────────────────────
def plot_ema_accuracy(df: pd.DataFrame, alpha: float, out_dir: str):
    """
    For each (proc_delay, ps_mode), plot EMA prediction error distribution.
    Also sweeps alpha to show sensitivity.
    """
    alphas = [0.1, 0.2, 0.3, 0.5, 0.7]
    ps_df = df[df["ps_label"] == "on"].copy()
    if ps_df.empty:
        return

    proc_delays = sorted(ps_df["processing_delay_ms"].unique())
    n = len(proc_delays)
    fig, axes = plt.subplots(1, n, figsize=(4.5 * n, 4.0))
    if n == 1:
        axes = [axes]

    for ax, pd_val in zip(axes, proc_delays):
        sub = ps_df[ps_df["processing_delay_ms"] == pd_val]
        delays = sub.sort_values("msg_index")["wakeup_delay_ms"].values
        if len(delays) < 10:
            continue

        mae_vals = []
        for a in alphas:
            _, _, err = ema_prediction_error(delays, a)
            mae_vals.append(np.mean(err))
            # plot predicted vs actual scatter for the chosen alpha only
            if abs(a - alpha) < 0.01:
                pred, actual, _ = ema_prediction_error(delays, a)
                ax.scatter(actual, pred, s=6, alpha=0.4, color=COLORS["on"],
                           label=f"α={a:.2f}  MAE={np.mean(err):.2f}ms")

        # reference line
        lim_max = max(ax.get_xlim()[1], ax.get_ylim()[1]) if ax.get_xlim()[1] > 0 else 100
        ax.plot([0, lim_max], [0, lim_max], "k--", linewidth=0.8, label="perfect")
        ax.set_xlabel("Actual wake-up delay (ms)")
        ax.set_ylabel("EMA-predicted delay (ms)")
        ax.set_title(f"proc_delay = {int(pd_val)} ms")
        ax.legend()

    fig.suptitle(f"EMA Prediction Accuracy (α={alpha})  —  PS on", fontweight="bold")
    fig.tight_layout()
    path = os.path.join(out_dir, "fig4_ema_accuracy.png")
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved {path}")

    # secondary: MAE vs alpha bar chart (first proc_delay only)
    if proc_delays:
        pd_val = proc_delays[0]
        sub    = ps_df[ps_df["processing_delay_ms"] == pd_val]
        delays = sub.sort_values("msg_index")["wakeup_delay_ms"].values
        mae_vals = []
        for a in alphas:
            _, _, err = ema_prediction_error(delays, a)
            mae_vals.append(np.mean(err))

        fig2, ax2 = plt.subplots(figsize=(5, 3.5))
        bars = ax2.bar([str(a) for a in alphas], mae_vals, color=COLORS["on"], alpha=0.8)
        ax2.bar_label(bars, fmt="%.2f", fontsize=8)
        ax2.set_xlabel("EMA alpha")
        ax2.set_ylabel("MAE (ms)")
        ax2.set_title(f"EMA prediction MAE vs α  (proc={int(pd_val)}ms)")
        fig2.tight_layout()
        path2 = os.path.join(out_dir, "fig4b_mae_vs_alpha.png")
        fig2.savefig(path2, bbox_inches="tight")
        plt.close(fig2)
        print(f"  saved {path2}")


# ── Figure 5: RTT box plots by inter-gap ─────────────────────────────────────
def plot_rtt_by_gap(df: pd.DataFrame, out_dir: str):
    proc_delays = sorted(df["processing_delay_ms"].unique())
    n = len(proc_delays)
    fig, axes = plt.subplots(1, n, figsize=(4.5 * n, 4.2))
    if n == 1:
        axes = [axes]

    for ax, pd_val in zip(axes, proc_delays):
        sub  = df[df["processing_delay_ms"] == pd_val]
        gaps = sorted(sub["inter_gap_ms"].unique())
        positions_off = np.arange(len(gaps)) * 3
        positions_on  = positions_off + 1.1

        for positions, ps_label in [(positions_off, "off"), (positions_on, "on")]:
            data_groups = [
                sub[(sub["inter_gap_ms"] == g) & (sub["ps_label"] == ps_label)]["rtt_ms"].values
                for g in gaps
            ]
            bp = ax.boxplot(data_groups, positions=positions, widths=0.9,
                            patch_artist=True, showfliers=False,
                            medianprops=dict(color="white", linewidth=2))
            for patch in bp["boxes"]:
                patch.set_facecolor(COLORS[ps_label])
                patch.set_alpha(0.75)

        ax.set_xticks(positions_off + 0.55)
        ax.set_xticklabels([f"{int(g)} ms" for g in gaps])
        ax.set_xlabel("Inter-message gap")
        ax.set_ylabel("RTT (ms)")
        ax.set_title(f"proc_delay = {int(pd_val)} ms")

    legend_els = [Line2D([0], [0], color=COLORS["off"], lw=8, alpha=0.75, label="PS off"),
                  Line2D([0], [0], color=COLORS["on"],  lw=8, alpha=0.75, label="PS on")]
    axes[-1].legend(handles=legend_els, loc="upper right")

    fig.suptitle("RTT vs Inter-Message Gap — PS on vs off", fontweight="bold")
    fig.tight_layout()
    path = os.path.join(out_dir, "fig5_rtt_by_gap.png")
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved {path}")


# ── Figure 6: summary statistics table (saved as PNG) ────────────────────────
def save_summary_table(df: pd.DataFrame, out_dir: str):
    summary = (df.groupby(["processing_delay_ms", "ps_label", "inter_gap_ms"])["rtt_ms"]
               .agg(count="count", mean="mean", median="median",
                    p95=lambda x: np.percentile(x, 95),
                    std="std")
               .round(2)
               .reset_index())
    path = os.path.join(out_dir, "summary_stats.csv")
    summary.to_csv(path, index=False)
    print(f"  saved {path}")


# ── main ──────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Analyze PS experiment results")
    parser.add_argument("--results-dir", default="results")
    parser.add_argument("--alpha",       type=float, default=0.3,
                        help="EMA smoothing factor (0 < alpha ≤ 1, default 0.3)")
    parser.add_argument("--out-dir",     default=None,
                        help="Output directory for plots (default: <results-dir>/plots)")
    args = parser.parse_args()

    out_dir = args.out_dir or os.path.join(args.results_dir, "plots")
    os.makedirs(out_dir, exist_ok=True)

    print(f"Loading data from {args.results_dir}/csv/ ...")
    df = load_data(args.results_dir)
    print(f"  {len(df)} records from {df['run_id'].nunique()} runs")
    print(f"  PS modes : {sorted(df['ps_label'].unique())}")
    print(f"  proc delays: {sorted(df['processing_delay_ms'].unique())}")
    print(f"  msg sizes  : {sorted(df['msg_size_bytes'].unique())}")

    print("Computing wake-up delays ...")
    df = compute_wakeup_delay(df)

    print(f"Generating plots (alpha={args.alpha}) ...")
    plot_rtt_vs_size(df, out_dir)
    plot_cdf(df, out_dir)
    plot_wakeup_ema(df, args.alpha, out_dir)
    plot_ema_accuracy(df, args.alpha, out_dir)
    plot_rtt_by_gap(df, out_dir)
    save_summary_table(df, out_dir)

    print(f"\nAll plots written to: {out_dir}/")


if __name__ == "__main__":
    main()
