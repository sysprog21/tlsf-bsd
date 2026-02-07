#!/usr/bin/env python3
"""
WCET analysis plots for TLSF allocator.

Reads raw sample data from 'wcet -r' output and generates latency
distribution plots.  Falls back to text summary when matplotlib is
unavailable (e.g., CI environments).

Usage:
    build/wcet -r samples.csv && python3 scripts/wcet_plot.py samples.csv
    build/wcet -r samples.csv && python3 scripts/wcet_plot.py samples.csv -o build/wcet

The output prefix (-o) controls where PNG files are written.  Two plots
are generated:
    {prefix}_boxplot.png     - Box plot of all scenarios per size class
    {prefix}_histogram.png   - Latency histograms per scenario
"""

import argparse
import csv
import sys
from collections import defaultdict

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False


def read_raw_csv(path):
    """Read raw sample CSV: scenario,size,[unit,]value -> (data, unit)."""
    data = defaultdict(lambda: defaultdict(list))
    unit = None
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            scenario = row["scenario"]
            size = int(row["size"])
            value = int(row["value"])
            data[scenario][size].append(value)
            if unit is None and "unit" in row:
                unit = row["unit"]
    return data, unit


def percentile(sorted_data, p):
    """Return the p-th percentile from pre-sorted data."""
    if not sorted_data:
        return 0
    idx = int(len(sorted_data) * p / 100.0)
    if idx >= len(sorted_data):
        idx = len(sorted_data) - 1
    return sorted_data[idx]


def text_report(data):
    """Print text summary to stdout."""
    scenarios = sorted(data.keys())
    for scenario in scenarios:
        print(f"\n  {scenario}")
        print(
            f"    {'Size':>6s} {'Min':>8s} {'P50':>8s} {'P90':>8s} "
            f"{'P99':>8s} {'P99.9':>8s} {'Max':>8s}"
        )
        for size in sorted(data[scenario].keys()):
            v = sorted(data[scenario][size])
            print(
                f"    {size:>6d} {v[0]:>8d} {percentile(v, 50):>8d} "
                f"{percentile(v, 90):>8d} {percentile(v, 99):>8d} "
                f"{percentile(v, 99.9):>8d} {v[-1]:>8d}"
            )


def detect_unit(data, csv_unit=None):
    """Return the tick unit, preferring the CSV-embedded value."""
    if csv_unit:
        return csv_unit
    # Fallback heuristic for legacy CSV files without a unit column
    all_vals = []
    for scenario in data.values():
        for size_vals in scenario.values():
            all_vals.extend(size_vals[:100])
    if not all_vals:
        return "ticks"
    median = sorted(all_vals)[len(all_vals) // 2]
    if median > 100000:
        return "ns"
    if median < 30:
        return "ticks"
    return "cycles"


def plot_boxplot(data, output_path, csv_unit=None):
    """Box plot: all scenarios side by side for each allocation size."""
    scenarios = sorted(data.keys())
    sizes = sorted(next(iter(data.values())).keys())
    unit = detect_unit(data, csv_unit)

    n_sizes = len(sizes)
    n_scenarios = len(scenarios)

    fig, axes = plt.subplots(1, n_sizes, figsize=(3.5 * n_sizes, 5), sharey=False)
    if n_sizes == 1:
        axes = [axes]

    colors = {
        "malloc_worst": "#e74c3c",
        "malloc_best": "#3498db",
        "free_worst": "#e67e22",
        "free_best": "#2ecc71",
    }

    for ax, size in zip(axes, sizes):
        box_data = []
        labels = []
        box_colors = []
        for scenario in scenarios:
            vals = data[scenario].get(size, [])
            box_data.append(vals)
            label = scenario.replace("malloc_", "m/").replace("free_", "f/")
            labels.append(label)
            box_colors.append(colors.get(scenario, "#95a5a6"))

        bp = ax.boxplot(
            box_data,
            labels=labels,
            showfliers=False,
            patch_artist=True,
            widths=0.6,
            medianprops={"color": "black", "linewidth": 1.5},
        )
        for patch, color in zip(bp["boxes"], box_colors):
            patch.set_facecolor(color)
            patch.set_alpha(0.7)

        ax.set_title(f"{size}B", fontweight="bold")
        ax.set_ylabel(f"Latency ({unit})" if ax == axes[0] else "")
        ax.grid(True, alpha=0.3, axis="y")
        ax.tick_params(axis="x", rotation=30)

    fig.suptitle("TLSF WCET: Per-Operation Latency", fontsize=13, y=1.02)
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches="tight")
    print(f"  Box plot: {output_path}")


def plot_histogram(data, output_path, csv_unit=None):
    """Latency histograms: one subplot per scenario, all sizes overlaid."""
    scenarios = sorted(data.keys())
    sizes = sorted(next(iter(data.values())).keys())
    unit = detect_unit(data, csv_unit)

    n_scenarios = len(scenarios)
    fig, axes = plt.subplots(
        n_scenarios, 1, figsize=(10, 3 * n_scenarios), sharex=False
    )
    if n_scenarios == 1:
        axes = [axes]

    size_colors = plt.cm.viridis(
        [i / max(len(sizes) - 1, 1) for i in range(len(sizes))]
    )

    for ax, scenario in zip(axes, scenarios):
        for si, size in enumerate(sizes):
            vals = data[scenario].get(size, [])
            if not vals:
                continue
            # Clip outliers beyond p99.5 for cleaner histograms
            sv = sorted(vals)
            clip = sv[min(int(len(sv) * 0.995), len(sv) - 1)]
            clipped = [v for v in vals if v <= clip]
            ax.hist(
                clipped,
                bins=50,
                alpha=0.5,
                label=f"{size}B",
                color=size_colors[si],
                density=True,
            )

        ax.set_title(scenario, fontweight="bold")
        ax.set_ylabel("Density")
        ax.legend(fontsize=8, loc="upper right")
        ax.grid(True, alpha=0.3, axis="y")

    axes[-1].set_xlabel(f"Latency ({unit})")
    fig.suptitle("TLSF WCET: Latency Distributions", fontsize=13, y=1.01)
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches="tight")
    print(f"  Histogram: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Generate WCET analysis plots from raw sample data."
    )
    parser.add_argument("input", help="Raw CSV file from 'wcet -r'")
    parser.add_argument(
        "-o",
        "--output",
        default="wcet",
        help="Output file prefix (default: wcet)",
    )
    args = parser.parse_args()

    data, csv_unit = read_raw_csv(args.input)
    if not data:
        print("No data found in input file", file=sys.stderr)
        return 1

    # Always print text summary
    print("WCET Summary:")
    text_report(data)
    print()

    if HAS_MATPLOTLIB:
        plot_boxplot(data, f"{args.output}_boxplot.png", csv_unit)
        plot_histogram(data, f"{args.output}_histogram.png", csv_unit)
    else:
        print("Note: matplotlib not available, skipping plot generation.")
        print("Install with: pip install matplotlib")

    return 0


if __name__ == "__main__":
    sys.exit(main())
