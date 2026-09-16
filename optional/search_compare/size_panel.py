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
parser.add_argument("--variants", nargs="+", choices=["base", "sub32", "packed", "direct32", "direct64"],
                    default=["base", "sub32", "packed", "direct32", "direct64"])
parser.add_argument("--binary-name", choices=["size_panel", "scheduled_size_panel"], default="size_panel")
parser.add_argument("--processes", type=int, default=2)
parser.add_argument("--trials", type=int, default=3)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
variants = args.variants
metadata_path = args.output / "metadata.json"
metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {"execution_order": []}
binaries = {v: hashlib.sha256((args.build / v / args.binary_name).read_bytes()).hexdigest() for v in variants}
if metadata_path.exists():
    if metadata.get("binary_name", "size_panel") != args.binary_name:
        raise RuntimeError("Output directory belongs to a different benchmark; use a new directory")
    for variant, fingerprint in binaries.items():
        previous = metadata.get("binaries", {}).get(variant)
        if previous is not None and previous != fingerprint:
            raise RuntimeError("Output directory has different executable hashes; use a new directory")
root = Path(__file__).resolve().parent
metadata["binary_name"] = args.binary_name
metadata.setdefault("binaries", {}).update(binaries)
sources = {n: hashlib.sha256((root / n).read_bytes()).hexdigest()
           for n in ["bench.cc", args.binary_name + ".cc", "size_panel.py", "prepare.py"]}
if metadata_path.exists() and metadata.get("sources") != sources:
    raise RuntimeError("Output directory has different source hashes; use a new directory")
metadata["sources"] = sources
# Freeze executable identity before writing the first raw result, so an
# interrupted collection can resume without silently relabeling old trials.
metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
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
                command = [str((args.build / variant / args.binary_name).resolve()), profile, str(n), str(width),
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
metadata["execution_order"] += order
metadata["scheduling"] = sorted({line for path in args.output.glob("[0-9]-*.stderr")
                                  for line in path.read_text().splitlines() if line.startswith("SCHEDULING,")})
metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
