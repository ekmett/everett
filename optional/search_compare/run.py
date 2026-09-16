#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Rotate independent processes and preserve whole-query observations."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import random
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("build", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--processes", type=int, default=3)
parser.add_argument("--queries", type=int, default=2048)
parser.add_argument("--loops", type=int, default=2)
parser.add_argument("--trials", type=int, default=3)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
jobs = [(profile, n, width, shape) for profile in ["byte", "bit"]
        for n in [8192, 131072] for width, shape in
        [(16, "structured"), (128, "structured"), (16, "hash"), (128, "hash")]]
observations, footprints, order = [], [], []
for process in range(args.processes):
    random.Random(917 + process).shuffle(jobs)
    for case, dimensions in enumerate(jobs):
        variants = ["base", "sub32", "direct32"]
        rotation = (case + process) % 3
        for variant in variants[rotation:] + variants[:rotation]:
            name = f"{process}-{variant}-{'-'.join(map(str, dimensions))}"
            command = [str((args.build / variant / "bench").resolve()), *map(str, dimensions),
                       str(args.queries), str(args.trials), str(args.loops), "0", variant]
            begin = time.time()
            result = subprocess.run(command, text=True, capture_output=True, check=True)
            order.append({"name": name, "started": begin, "elapsed": time.time() - begin})
            (args.output / f"{name}.csv").write_text(result.stdout)
            (args.output / f"{name}.stderr").write_text(result.stderr)
            for row in csv.DictReader(result.stdout.splitlines()):
                observations.append({"process": process, "variant": variant, **row})
            line, = [x for x in result.stderr.splitlines() if x.startswith("SPACE,")]
            names = ["file_bytes", "payload_bytes", "ef_payload_bytes", "ef_auxiliary_bytes", "rank_bytes",
                     "cut_bytes", "flag_bytes", "native_build_ns", "index_build_ns", "payload_hash"]
            footprints.append(dict(process=process, variant=variant, profile=dimensions[0],
                records=dimensions[1], key_bytes=dimensions[2], distribution=dimensions[3],
                **dict(zip(names, line.split(",")[1:]))))
            print(name, flush=True)
for name, rows in [("queries.csv", observations), ("space.csv", footprints)]:
    with (args.output / name).open("w") as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
root = Path(__file__).resolve().parents[2]
sources = [root / "optional/search_compare" / n for n in ["bench.cc", "prepare.py", "telemetry.h", "run.py"]]
metadata = {"platform": platform.platform(), "machine": platform.machine(),
    "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
    "arguments": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()
                  if k not in ["build", "output"]},
    "sources": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
    "binaries": {v: hashlib.sha256((args.build / v / "bench").read_bytes()).hexdigest()
                 for v in ["base", "sub32", "direct32"]}, "execution_order": order}
(args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
