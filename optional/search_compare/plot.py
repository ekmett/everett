#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Plot whole-search speed against complete-file growth from retained CSV rows."""

import argparse
import csv
import hashlib
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import PercentFormatter


def read(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("primary", type=Path)
    parser.add_argument("alternatives", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    rows = read(args.primary) + read(args.alternatives)
    colors = {"sub32": "#8b8e98", "packed": "#147d92",
              "direct32": "#c46717", "direct64": "#7455a0"}
    names = {"sub32": "EF + sub32", "packed": "Packed absolute",
             "direct32": "Direct 32-bit", "direct64": "Direct 64-bit"}
    markers = {"sub32": "s", "packed": "o", "direct32": "^", "direct64": "D"}
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "svg.hashsalt": "everett-offset-search"})
    fig, axes = plt.subplots(1, 2, figsize=(12, 5.4), sharey=True)
    for ax, profile, label in zip(axes, ("typed-byte", "typed-bit"),
                                  ("Byte KV02", "Bit KV03")):
        selected = [row for row in rows if row["profile"] == profile]
        if not selected:
            raise ValueError("No rows for " + profile)
        for variant in colors:
            points = [row for row in selected if row["variant"] == variant]
            if not points:
                raise ValueError("No rows for " + profile + "/" + variant)
            ax.scatter([float(row["file_growth_percent"]) for row in points],
                       [(float(row["speedup"]) - 1) * 100 for row in points],
                       label=names[variant], color=colors[variant],
                       marker=markers[variant], s=27, alpha=.58, linewidths=0)
        ax.axhline(0, color="#40454d", linewidth=.8)
        ax.set_title(label, loc="left", weight="bold")
        ax.set_xlabel("Growth in complete .kv + .index bytes")
        ax.xaxis.set_major_formatter(PercentFormatter(xmax=100, decimals=1))
        ax.yaxis.set_major_formatter(PercentFormatter(xmax=100, decimals=0))
        ax.grid(alpha=.15)
        ax.set_axisbelow(True)
    axes[0].set_ylabel("Whole-query throughput gain over Elias–Fano")
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(.5, .105),
               ncol=4, frameon=False, markerscale=1.4)
    fig.suptitle("Faster offsets cost a small fraction of the complete files",
                 fontsize=15, weight="bold", x=.07, ha="left")
    fig.text(.07, .875, "Apple M2 Max · resident host CPU lookups · 8K and 128K base records",
             color="#555b65")
    fig.text(.07, .035,
             "Each point is one workload median: 3 processes × 3 trials. "
             "1,024-key warm query set; hits, misses and mixed queries.\n"
             "Packed/direct64 use their own interleaved EF baseline. "
             "No production dispatch, cold-file or GPU performance is inferred.",
             fontsize=9, color="#555b65")
    fig.subplots_adjust(left=.07, right=.98, top=.81, bottom=.29, wspace=.12)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for extension in ("png", "svg"):
        fig.savefig(args.output.with_suffix("." + extension), dpi=180,
                    metadata={"Date": None} if extension == "svg" else {})
    metadata = {"inputs": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                            for path in (args.primary, args.alternatives)},
                "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "matplotlib": matplotlib.__version__,
                "aggregation": "One point per retained workload median; no pooled latencies."}
    args.output.with_suffix(".json").write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
