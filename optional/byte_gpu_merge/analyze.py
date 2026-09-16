#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Summarizes retained byte-profile CPU/Metal merge observations.
#
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import csv
import hashlib
import json
from pathlib import Path
from statistics import median


def main():
    p = argparse.ArgumentParser()
    p.add_argument("directory", type=Path)
    args = p.parse_args()
    manifest_path = args.directory / "sha256.json"
    for name, expected in json.loads(manifest_path.read_text()).items():
        assert hashlib.sha256((args.directory / name).read_bytes()).hexdigest() == expected, name
    with (args.directory / "observations.csv").open() as source:
        rows = list(csv.DictReader(source))
    metadata = json.loads((args.directory / "metadata.json").read_text())
    summary = []
    for name in sorted({row["case"] for row in rows}):
        trial = [row for row in rows if row["case"] == name and int(row["trial"]) >= 0]
        assert len(trial) == metadata["processes"] * metadata["trials"]
        for process in range(metadata["processes"]):
            assert sorted(int(row["trial"]) for row in trial if int(row["process"]) == process) == list(range(metadata["trials"]))
        assert all(row["cpu_width_pass"] == "0" for row in trial)
        item = {"case": name, "observations": len(trial)}
        for field in ("older", "newer", "output_records", "input_a_bytes", "input_b_bytes", "output_bytes", "common_value_bytes"):
            values = {int(row[field]) for row in trial}
            assert len(values) == 1
            item[field] = values.pop()
        for metric in ("cpu_ms", "gpu_complete_ms", "gpu_device_ms", "parse_ms", "order_ms", "size_ms", "emit_ms", "assembly_ms"):
            process = [median(float(row[metric]) for row in trial if int(row["process"]) == i)
                       for i in range(metadata["processes"])]
            item[metric] = {"median": median(process), "process_medians": process,
                "trial_min": min(float(row[metric]) for row in trial), "trial_max": max(float(row[metric]) for row in trial)}
        item["cpu_over_gpu"] = item["cpu_ms"]["median"] / item["gpu_complete_ms"]["median"]
        summary.append(item)
    (args.directory / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    manifest = {str(path.relative_to(args.directory)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(args.directory.rglob("*")) if path.is_file() and path != manifest_path}
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print("case,cpu_ms,gpu_ms,cpu_over_gpu")
    for item in summary:
        print(f'{item["case"]},{item["cpu_ms"]["median"]:.6f},{item["gpu_complete_ms"]["median"]:.6f},{item["cpu_over_gpu"]:.3f}')

if __name__ == "__main__":
    main()
