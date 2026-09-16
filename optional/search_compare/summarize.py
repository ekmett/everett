#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Pair process medians with complete serialized fixture-file sizes."""
import argparse
from collections import defaultdict
import csv
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--phase", required=True)
args = parser.parse_args()
dimensions = ["profile", "distribution", "records", "key_bytes", "query_kind", "access"]
raw = list(csv.DictReader((args.input / "queries.csv").open()))
space = list(csv.DictReader((args.input / "space.csv").open()))
processes = defaultdict(list)
checksums = defaultdict(set)
for r in raw:
    key = tuple(r[k] for k in dimensions)
    processes[(key, r["variant"], r["process"])].append(float(r["ns_per_query"]))
    checksums[key].add(r["checksum"])
if any(len(c) != 1 for c in checksums.values()):
    raise RuntimeError("Query checksums differ between variants/processes")
medians = defaultdict(list)
for (key, variant, process), values in processes.items():
    medians[(key, variant)].append(statistics.median(values))
footprints = {}
payload_hashes = defaultdict(set)
construction = defaultdict(list)
for r in space:
    key = ("typed-" + r["profile"], r["distribution"], r["records"], r["key_bytes"])
    payload_hashes[key].add(r["payload_hash"])
    footprint = tuple(int(r[k]) for k in ["file_bytes", "payload_bytes", "ef_payload_bytes", "ef_auxiliary_bytes"])
    if (key, r["variant"]) in footprints and footprints[(key, r["variant"]) ] != footprint:
        raise RuntimeError("Space changed across processes")
    footprints[(key, r["variant"])] = footprint
    construction[(key, r["variant"])].append(float(r["native_build_ns"]) + float(r["index_build_ns"]))
if any(len(c) != 1 for c in payload_hashes.values()):
    raise RuntimeError("Encoded record-stream fingerprints differ")
rows = []
for (key, variant), values in sorted(medians.items()):
    base = medians[(key, "base")]
    baseline, actual = statistics.median(base), statistics.median(values)
    bs = footprints[(key[:4], "base")]; cs = footprints[(key[:4], variant)]
    rows.append(dict(phase=args.phase, **dict(zip(dimensions, key)), variant=variant,
        processes=len(values), baseline_ns=baseline, query_ns=actual, speedup=baseline / actual,
        baseline_min_ns=min(base), baseline_max_ns=max(base), candidate_min_ns=min(values), candidate_max_ns=max(values),
        process_ranges="faster" if max(values) < min(base) else "slower" if min(values) > max(base) else "overlap",
        baseline_file_bytes=bs[0], file_bytes=cs[0], file_growth_percent=100 * (cs[0] / bs[0] - 1),
        offset_bytes=cs[2] + cs[3], offset_file_percent=100 * (cs[2] + cs[3]) / cs[0],
        offset_auxiliary_bytes=cs[3], preparation_ns=statistics.median(construction[(key[:4], variant)])))
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open("w") as out:
    writer = csv.DictWriter(out, fieldnames=rows[0]); writer.writeheader(); writer.writerows(rows)
print(len(rows), "paired summary rows")
