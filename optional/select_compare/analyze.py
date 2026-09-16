#!/usr/bin/env python3
"""Validate complete result groups and summarize paired offset-access measurements."""
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Reports exact sequence space and median-of-process-median timings.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics as stats
import sys

root = Path(sys.argv[1])
manifest = json.loads((root / "manifest.json").read_text())
sequences = {x["sequence"]: x for x in json.loads((root / "sequences.json").read_text())}
raw_hashes = json.loads((root / "raw-sha256.json").read_text())
for name, expected in raw_hashes.items():
    assert hashlib.sha256((root / "raw" / name).read_bytes()).hexdigest() == expected, name
candidates = ["ef-current", "ef-trusted-control", "direct64", "direct32", "packed-absolute", "ef-high-direct64", "ef-sub32", "ef-sux-simple1", "ef-sux-simple2", "ef-sux-half", "ef-sux-half-fixed"]
accesses = ["random-throughput", "clustered-throughput", "sequential-throughput", "dependent-latency", "lower-bound"]
measurements, constructions, spaces, checksums, failures, unavailable = {}, {}, {}, {}, [], []
process_count, row_count = 0, 0
for path in sorted((root / "raw").glob("*.p*.csv")):
    process = int(path.stem.rsplit(".p", 1)[1])
    process_count += 1
    seen = set()
    with path.open() as stream:
        rows = list(csv.DictReader(stream))
    assert rows, path
    sequence = rows[0]["sequence"]
    meta = sequences[sequence]
    for row in rows:
        row_count += 1
        assert row["sequence"] == sequence
        candidate, access, trial = row["candidate"], row["access"], int(row["trial"])
        assert candidate in candidates
        key = candidate, access, trial
        assert key not in seen, (path, key)
        seen.add(key)
        assert int(row["count"]) == meta["count"] and int(row["universe"]) == meta["universe"]
        if row["status"] != "ok":
            if row["status"] == "ineligible-width":
                assert candidate == "direct32" and meta["universe"] > 2**32 - 1
                unavailable.append({"sequence": sequence, "candidate": candidate, "process": process})
            else:
                assert candidate == "ef-sux-half" and row["status"] == "failed-verification"
                failures.append({"sequence": sequence, "candidate": candidate, "process": process, "status": row["status"]})
            continue
        assert int(row["low_width"]) == meta["low_width"] and int(row["high_bits"]) == meta["high_bits"]
        space = {x: int(row[x]) for x in ("payload_bytes", "auxiliary_bytes", "allocated_bytes", "object_bytes")}
        space["array_bytes"] = space["payload_bytes"] + space["auxiliary_bytes"]
        space["bytes_per_offset"] = space["array_bytes"] / meta["count"]
        space["resident_bytes"] = space["allocated_bytes"] + space["object_bytes"]
        space_key = sequence, candidate
        assert space_key not in spaces or spaces[space_key] == space
        spaces[space_key] = space
        checksum_key = sequence, access, trial
        checksum = (int(row["checksum"]), int(row["select_calls"]))
        assert checksum_key not in checksums or checksums[checksum_key] == checksum, (path, checksum_key)
        checksums[checksum_key] = checksum
        assert checksum[1] > 0 and float(row["time_ns"]) > 0
        if trial >= 0:
            measurements.setdefault((sequence, candidate, access), {}).setdefault(process, []).append(float(row["time_ns"]) / checksum[1])
        build_key = sequence, candidate, process, trial
        construction = [float(row["whole_build_ns"]), float(row["index_build_ns"])]
        assert build_key not in constructions or constructions[build_key] == construction
        constructions[build_key] = construction
    for candidate in candidates:
        if candidate == "direct32" and meta["universe"] > 2**32 - 1:
            assert (candidate, "invalid", 0) in seen
            continue
        if (candidate, "invalid", 0) in seen:
            continue
        for access in accesses + (["forward-cursor", "dependency-loop-control"] if candidate == "ef-current" else []):
            assert all((candidate, access, trial) in seen for trial in range(-1, 3)), (path, candidate, access)
assert process_count == manifest["completed_processes"]
summary = []
for (sequence, candidate, access), processes in sorted(measurements.items()):
    assert set(processes) == {0, 1, 2} and all(len(v) == 3 for v in processes.values())
    medians = [stats.median(processes[p]) for p in range(3)]
    whole = [stats.median(constructions[(sequence, candidate, p, t)][0] for t in range(3)) for p in range(3)]
    index = [stats.median(constructions[(sequence, candidate, p, t)][1] for t in range(3)) for p in range(3)]
    summary.append({"sequence": sequence, "candidate": candidate, "access": access,
        "ns_per_call": stats.median(medians), "process_medians": medians,
        "trial_min": min(min(x) for x in processes.values()), "trial_max": max(max(x) for x in processes.values()),
        "whole_build_ns": stats.median(whole), "index_build_ns": stats.median(index),
        **spaces[(sequence, candidate)]})
(root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
(root / "checks.json").write_text(json.dumps({"processes": process_count, "rows": row_count, "summary_rows": len(summary),
    "same_sequence_checksums": len(checksums), "failures": failures, "unavailable": unavailable,
    "validation": "All raw hashes, result groups, sequence extents, per-candidate spaces, checksums and query counts verified"}, indent=2) + "\n")
lookup = {(x["sequence"], x["candidate"], x["access"]): x for x in summary}
actual = sorted({x["sequence"] for x in summary if sequences[x["sequence"]]["kind"] == "actual-library-encoded"})
def geomean(values):
    return math.exp(stats.fmean(math.log(x) for x in values))
def ratios(candidate, access, pool=actual):
    return [lookup[(s, "ef-current", access)]["ns_per_call"] / lookup[(s, candidate, access)]["ns_per_call"]
            for s in pool if (s, candidate, access) in lookup]
report = ["Select alternatives on the same offsets", "======================================", "",
    f"Measured source `{manifest['source_revision']}`; {manifest['machine']}, {manifest.get('machdep.cpu.brand_string', '')}.",
    f"{process_count} fresh processes; one excluded warmup plus three trials each. Every valid candidate matches the same sequence and query checksums.",
    "Numbers below are medians of three process medians. The raw CSV files, immutable-array byte counts, construction times, and per-process results are retained alongside this report.", "",
    "Scope", "-----", "", "These are offset-access measurements, not complete lookup or merge benchmarks. Library-built native and fractional-index fixtures supply 96 real encoded directories. Six larger cases replay their observed gaps; three synthetic cases probe an upstream boundary. All access data are resident and warmed by qualification/construction. No filesystem work is timed. The production baseline remains unchanged.", "",
    "The trusted control also bypasses mapped/native view accessors: its difference cannot be assigned entirely to validation. Sux and other directory alternatives trust their high-select structures while checking ordinal and decoded offset. Direct arrays and packed positions expect valid ordinals.", "",
    "Across the 96 encoded fixture directories", "---------------------------------------", "",
    "Paired geometric-mean speedup over checked production EF; each sequence has equal weight. Parentheses show the minimum and maximum per-sequence speedup. This aggregates differently sized directories, so use the detailed rows for a particular workload.", "",
    "| Candidate | Random throughput | Dependent latency | Offset binary search |", "| --- | ---: | ---: | ---: |"]
for candidate in candidates[1:]:
    cells = []
    for access in ("random-throughput", "dependent-latency", "lower-bound"):
        rr = ratios(candidate, access)
        cells.append(f"{geomean(rr):.2f}× ({min(rr):.2f}–{max(rr):.2f})")
    report.append("| " + candidate + " | " + " | ".join(cells) + " |")
report.extend(["", "Same-data space example", "-----------------------", ""])
for sequence in ("case-2.raw-byte.native-output", "case-11.typed-bit.index-main"):
    meta = sequences[sequence]
    report.extend([f"`{sequence}`: {meta['count']} offsets, inclusive universe {meta['universe']}, EF low width {meta['low_width']}; $U/n={meta['universe']/meta['count']:.3f}$ and high-bit density $n/H={meta['count']/meta['high_bits']:.3f}$.", "",
      "| Candidate | Payload bytes | Auxiliary bytes | Total array bytes | Bytes/offset | Resident bytes |", "| --- | ---: | ---: | ---: | ---: | ---: |"])
    for candidate in candidates:
        x = spaces[(sequence, candidate)]
        report.append(f"| {candidate} | {x['payload_bytes']} | {x['auxiliary_bytes']} | {x['array_bytes']} | {x['bytes_per_offset']:.3f} | {x['resident_bytes']} |")
    report.append("")
report.extend(["Totals count array contents and padding, excluding constant file metadata. Resident bytes additionally count retained capacity and C++ objects, excluding allocator headers. Sux array bytes are not a persisted file-format size. High bits are counted once; auxiliary percentages must not be confused with total representation percentages.", "",
    "Cache-scale replay", "------------------", "", "These are derived synthetic sequences. Random-throughput nanoseconds per selected offset:", "",
    "| Sequence | Current | Trusted | Sub32 | Simple2 | Half fixed | Direct32 |", "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"])
for sequence in sorted(s for s, m in sequences.items() if m["kind"] == "synthetic-replayed-observed-gaps"):
    cells = []
    for candidate in ("ef-current", "ef-trusted-control", "ef-sub32", "ef-sux-simple2", "ef-sux-half-fixed", "direct32"):
        x = lookup.get((sequence, candidate, "random-throughput"))
        cells.append(f"{x['ns_per_call']:.2f}" if x else "ineligible")
    report.append("| " + sequence + " | " + " | ".join(cells) + " |")
report.extend(["", "Construction and scanning", "-------------------------", "",
    "`summary.json` reports whole construction and alternative index-only construction separately, in nanoseconds. The whole builder includes low/high arrays; the isolated alternative builder does not build unused production samples. Batch destruction is outside construction timing. Production EF has no isolated-directory builder entry point, represented by -1 rather than an invented comparable number.", "",
    "| Example | Current build ns/offset | Simple2 build ns/offset | Simple2 index ns/offset | Current forward ns/offset | Current indexed scan ns/offset |", "| --- | ---: | ---: | ---: | ---: | ---: |"])
for sequence in ("case-2.raw-byte.native-output", "case-11.typed-bit.index-main", "replay-case-2.raw-byte.native-output-22"):
    n = sequences[sequence]["count"]
    a = lookup[(sequence,"ef-current","random-throughput")]
    b = lookup[(sequence,"ef-sux-simple2","random-throughput")]
    report.append(f"| {sequence} | {a['whole_build_ns']/n:.2f} | {b['whole_build_ns']/n:.2f} | {b['index_build_ns']/n:.2f} | {lookup[(sequence,'ef-current','forward-cursor')]['ns_per_call']:.2f} | {lookup[(sequence,'ef-current','sequential-throughput')]['ns_per_call']:.2f} |")
report.extend(["", "Correctness boundary and limitations", "------------------------------------", "",
    f"The unmodified pinned Sux Half generated {len(failures)} explicit failed-verification rows (one per failing sequence/process). Those rows have no performance claim. See the README for the exact 65536-span reproducer and separately labeled correction. The full SimpleSelect candidates remain unmodified.", "",
    "Results are from one Apple Silicon host. Neither Pasta nor SPIDER was measured. The experiment establishes no deployment lookup/merge crossover and no x86/PDEP speedup. The source, binary, external source, generated patch, sequence, and raw-output hashes are in the manifest files.", ""])
(root / "report.md").write_text("\n".join(report))
print(f"Validated {process_count} processes, {row_count} rows; wrote summary.json, checks.json, report.md")
