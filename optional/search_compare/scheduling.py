#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Retain per-process wall/CPU variation without discarding timed trials."""
import argparse
from collections import defaultdict
import csv
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
dimensions = ["profile", "distribution", "records", "key_bytes", "access", "variant", "process"]
groups = defaultdict(list)
for row in csv.DictReader(args.input.open()):
    groups[tuple(row[k] for k in dimensions)].append(row)
rows = []
for key, trials in sorted(groups.items()):
    row = dict(zip(dimensions, key), trials=len(trials))
    for field, prefix in [("ns_per_query", "wall"), ("cpu_ns_per_query", "cpu")]:
        values = [float(t[field]) for t in trials]
        row.update({prefix + "_min_ns": min(values), prefix + "_median_ns": statistics.median(values),
                    prefix + "_max_ns": max(values), prefix + "_max_min_ratio": max(values) / min(values)})
    ratios = [float(t["cpu_ns_per_query"]) / float(t["ns_per_query"]) for t in trials]
    row.update(cpu_wall_min=min(ratios), cpu_wall_median=statistics.median(ratios), cpu_wall_max=max(ratios))
    rows.append(row)
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open("w") as output:
    writer = csv.DictWriter(output, fieldnames=rows[0], lineterminator="\n")
    writer.writeheader(); writer.writerows(rows)
print(len(rows), "process/access groups;")
print("largest within-process wall trial ratio:", max(r["wall_max_min_ratio"] for r in rows))
print("largest within-process CPU trial ratio:", max(r["cpu_max_min_ratio"] for r in rows))
print("smallest CPU/wall ratio:", min(r["cpu_wall_min"] for r in rows))
