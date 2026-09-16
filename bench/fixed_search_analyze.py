#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Validate and summarize three fixed-search benchmark processes."""

import collections
import csv
import math
import pathlib
import statistics
import sys

root = pathlib.Path(sys.argv[1])
cases = collections.defaultdict(lambda: collections.defaultdict(list))
identities = {}
for process in range(3):
    with (root / f"process-{process}.csv").open() as source:
        rows = list(csv.DictReader(source))
    assert len(rows) == 7128
    groups = collections.defaultdict(list)
    for row in rows:
        case = (int(row["key_bits"]), int(row["count"]), row["shape"],
                int(row["windows"]), row["pattern"])
        assert case[0] in (32, 64, 128)
        assert case[1] in (*range(1, 33), 63)
        assert case[2] in ("random", "shared") and case[3] in (1, 8192)
        assert case[4] in ("independent", "dependent")
        assert row["method"] in ("binary", "simd", "automatic")
        identity = (int(row["window_bytes"]), int(row["iterations"]), int(row["checksum"]))
        assert identity[1] == 65536
        assert identities.setdefault(case, identity) == identity
        groups[case, row["method"]].append((int(row["repeat"]), float(row["ns_per_lookup"])))
    assert len(groups) == 2376
    for (case, method), values in groups.items():
        assert sorted(repeat for repeat, _ in values) == [0, 1, 2]
        cases[case][method].append(statistics.median(value for _, value in values))
assert len(cases) == 792

output = []
for case, methods in sorted(cases.items()):
    row = dict(zip(("key_bits", "count", "shape", "windows", "pattern"), case))
    for method in ("binary", "simd", "automatic"):
        values = methods[method]
        assert len(values) == 3
        row[method + "_ns"] = statistics.median(values)
        row[method + "_min"] = min(values)
        row[method + "_max"] = max(values)
    for method in ("simd", "automatic"):
        row[method + "_ratio"] = row["binary_ns"] / row[method + "_ns"]
        row[method + "_disjoint_win"] = row[method + "_max"] < row["binary_min"]
        row[method + "_disjoint_loss"] = row[method + "_min"] > row["binary_max"]
    row["checksum"] = identities[case][2]
    output.append(row)
with (root / "cases.csv").open("w") as target:
    writer = csv.DictWriter(target, output[0].keys())
    writer.writeheader()
    writer.writerows(output)

summary = []
for width in (32, 64, 128):
    for count in (*range(1, 33), 63):
        selected = [row for row in output if row["key_bits"] == width and row["count"] == count]
        assert len(selected) == 8
        row = {"key_bits": width, "count": count}
        for method in ("simd", "automatic"):
            ratios = [entry[method + "_ratio"] for entry in selected]
            row[method + "_geomean"] = math.exp(statistics.mean(map(math.log, ratios)))
            row[method + "_min_ratio"] = min(ratios)
            row[method + "_max_ratio"] = max(ratios)
            for outcome in ("win", "loss"):
                row[method + "_disjoint_" + outcome] = sum(entry[method + "_disjoint_" + outcome] for entry in selected)
        summary.append(row)
with (root / "summary.csv").open("w") as target:
    writer = csv.DictWriter(target, summary[0].keys())
    writer.writeheader()
    writer.writerows(summary)
print(f"Validated 21384 timed rows and {len(output)} cases in {root.name}.")
