#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Collect alternating fresh processes and verify identical fixture bytes/results."""
import argparse
import csv
import hashlib
import json
import pathlib
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from experiment import build_metadata


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=pathlib.Path)
    parser.add_argument("candidate", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--records", type=int, default=32768)
    parser.add_argument("--processes", type=int, default=3)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--loops", type=int, default=4)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = pathlib.Path(__file__).resolve().parents[2]
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    metadata = {
        "baseline_revision": "cb2b029a657d2bbcd7135265cb3060b2b11cd961",
        "candidate_revision": revision,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "records": args.records, "processes": args.processes,
        "trials": args.trials, "loops": args.loops,
        "binary_sha256": {name: digest(getattr(args, name)) for name in ("baseline", "candidate")},
        "source_sha256": {
            str(path.relative_to(source)): digest(path)
            for path in [source / "include/everett/sort_codec.h", source / "tests/sort_bit_reservoir.cc",
                         *sorted((source / "optional/bit_reservoir").glob("*"))] if path.is_file()
        },
        "builds": {name: build_metadata(getattr(args, name).parent) for name in ("baseline", "candidate")},
        "schedule": [],
    }
    results, expected_results, expected_files, checks = [], {}, {}, []
    for process in range(args.processes):
        order = ("baseline", "candidate") if process % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            tag = f"{process}-{variant}"
            fixtures = args.output / (tag + "-fixtures")
            raw = args.output / (tag + ".csv")
            command = [str(getattr(args, variant).resolve()), str(fixtures.resolve()),
                       str(args.records), str(args.trials), str(args.loops)]
            with raw.open("w") as output:
                subprocess.run(command, stdout=output, check=True)
            rows = list(csv.DictReader(raw.open()))
            assert len(rows) == 24 * (args.trials + 1)
            for row in rows:
                identity = (row["items"], row["bits"], row["loops"], row["checksum"])
                key = row["workload"]
                assert expected_results.setdefault(key, identity) == identity, (tag, key)
                results.append(dict(process=process, variant=variant, **row))
            files = {path.name: {"bytes": path.stat().st_size, "sha256": digest(path)}
                     for path in sorted(fixtures.glob("*.kv"))}
            assert len(files) == 12
            if expected_files:
                assert files == expected_files, tag
            else:
                expected_files = files
            checks.append({"process": process, "variant": variant, "rows": len(rows),
                           "fixture_files_equal": True, "raw_sha256": digest(raw)})
            metadata["schedule"].append({"process": process, "variant": variant})
            shutil.rmtree(fixtures)
    with (args.output / "results.csv").open("w") as output:
        writer = csv.DictWriter(output, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
    summary = []
    for workload in expected_results:
        entry = {"workload": workload, "items": int(expected_results[workload][0]),
                 "input_bits": int(expected_results[workload][1])}
        for variant in ("baseline", "candidate"):
            trials = [row for row in results if row["workload"] == workload and
                      row["variant"] == variant and row["phase"] == "timed"]
            medians = [statistics.median(float(row["ns_per_item"]) for row in trials if row["process"] == process)
                       for process in range(args.processes)]
            entry[variant] = {"process_medians": medians, "median_ns": statistics.median(medians),
                              "all_trial_min_ns": min(float(row["ns_per_item"]) for row in trials),
                              "all_trial_max_ns": max(float(row["ns_per_item"]) for row in trials)}
        entry["speedup"] = entry["baseline"]["median_ns"] / entry["candidate"]["median_ns"]
        entry["process_ranges_disjoint"] = (
            max(entry["candidate"]["process_medians"]) < min(entry["baseline"]["process_medians"]) or
            max(entry["baseline"]["process_medians"]) < min(entry["candidate"]["process_medians"]))
        summary.append(entry)
    for name, value in (("metadata.json", metadata), ("checks.json", checks),
                        ("fixtures.json", expected_files), ("summary.json", summary)):
        (args.output / name).write_text(json.dumps(value, indent=2) + "\n")
    manifest = {path.name: digest(path) for path in sorted(args.output.iterdir()) if path.is_file()}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for row in summary:
        print(f"{row['workload']:28} {row['baseline']['median_ns']:9.3f} "
              f"{row['candidate']['median_ns']:9.3f} {row['speedup']:6.3f}x")


if __name__ == "__main__":
    sys.exit(main())
