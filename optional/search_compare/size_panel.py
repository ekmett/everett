#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Run one bounded geometric-size panel; resume using existing raw files."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("build", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--sizes", nargs="+", type=int, default=[1024, 8192, 32768, 131072, 524288, 2097152])
parser.add_argument("--processes", type=int, default=2)
parser.add_argument("--trials", type=int, default=3)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
variants = ["base", "sub32", "packed", "direct32", "direct64"]
order = []
for n in args.sizes:
    for process in range(args.processes):
        for case, (profile, width, shape) in enumerate([("byte", 16, "structured"), ("bit", 128, "hash")]):
            rotation = (case + process + n.bit_length()) % len(variants)
            for variant in variants[rotation:] + variants[:rotation]:
                name = f"{process}-{variant}-{profile}-{n}-{width}-{shape}"
                out = args.output / f"{name}.csv"
                ineligible = args.output / f"{name}.ineligible.json"
                if out.exists() or ineligible.exists():
                    continue
                command = [str((args.build / variant / "size_panel").resolve()), profile, str(n), str(width),
                           shape, str(min(n, 65536)), str(args.trials)]
                begin = time.time()
                result = subprocess.run(command, capture_output=True, text=True)
                order.append({"name": name, "started": begin, "elapsed": time.time() - begin})
                if result.returncode:
                    if variant == "direct32" and "direct32 benchmark extent" in result.stderr:
                        ineligible.write_text(json.dumps({"reason": "residual universe exceeds UINT32_MAX",
                            "process": process, "profile": profile, "records": n, "key_bytes": width,
                            "distribution": shape}) + "\n")
                        continue
                    raise RuntimeError(f"{name} failed: {result.stderr}")
                out.write_text(result.stdout)
                (args.output / f"{name}.stderr").write_text(result.stderr)
                print(name, flush=True)
    # Each size is a complete checkpoint: callers may release the timing lane
    # between invocations without losing any completed observations.
observations, spaces = [], []
for path in sorted(args.output.glob("[0-9]-*.csv")):
    process, variant, profile, n, width, shape = path.stem.split("-")
    for row in csv.DictReader(path.read_text().splitlines()):
        observations.append({"process": process, "variant": variant, **row})
    stderr = path.with_suffix(".stderr").read_text().splitlines()
    s, = [line.split(",")[1:] for line in stderr if line.startswith("SPACE,")]
    q, = [line.split(",")[1:] for line in stderr if line.startswith("QUERIES,")]
    fields = ["file_bytes", "payload_bytes", "ef_payload_bytes", "ef_auxiliary_bytes", "rank_bytes", "cut_bytes",
              "flag_bytes", "native_build_ns", "index_build_ns", "payload_hash", "query_count", "distinct_queries",
              "query_logical_bytes", "query_object_bytes"]
    spaces.append(dict(process=process, variant=variant, profile=profile, records=n, key_bytes=width,
                       distribution=shape, **dict(zip(fields, s + q))))
for name, rows in [("queries.csv", observations), ("space.csv", spaces)]:
    with (args.output / name).open("w") as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
metadata_path = args.output / "metadata.json"
metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {"execution_order": []}
metadata["execution_order"] += order
metadata["binaries"] = {v: hashlib.sha256((args.build / v / "size_panel").read_bytes()).hexdigest() for v in variants}
root = Path(__file__).resolve().parent
metadata["sources"] = {n: hashlib.sha256((root / n).read_bytes()).hexdigest()
                       for n in ["bench.cc", "size_panel.cc", "size_panel.py", "prepare.py"]}
metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
