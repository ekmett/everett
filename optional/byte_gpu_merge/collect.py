#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Retains complete byte-profile CPU/Metal merge observations and provenance.
#
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--processes", type=int, default=3)
    p.add_argument("--trials", type=int, default=5)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    subprocess.run(["git", "diff", "--quiet", "HEAD"], cwd=root, check=True)
    build = args.build.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    raw = output / "raw"
    raw.mkdir()
    metadata = {
        "source_revision": revision, "platform": platform.platform(),
        "python": sys.version.split()[0], "processes": args.processes, "trials": args.trials,
        "warmup_trial": -1, "qos": "USER_INITIATED requested and verified on benchmark thread",
        "binary_sha256": digest(build / "prototype"),
        "metallib_sha256": digest(build / "kernels.metallib"),
        "compiler": subprocess.check_output(["xcrun", "clang++", "--version"], text=True).splitlines()[0],
    }
    files = subprocess.check_output(["git", "ls-files", "include/everett", "optional/byte_gpu_merge", "optional/gpu_merge"], cwd=root, text=True).splitlines()
    closure = {name: digest(root / name) for name in files if Path(name).suffix in {".h", ".inc", ".hlsl", ".mm", ".py", ".txt"}}
    metadata["source_files"] = closure
    metadata["spirv_sha256"] = {path.name: digest(path) for path in sorted(build.glob("*.spv"))}
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    rows = []
    identities = []
    for process in range(args.processes):
        work = args.work.resolve() / str(process)
        with (raw / f"process-{process}.csv").open("w") as stdout, (raw / f"process-{process}.stderr").open("w") as stderr:
            subprocess.run([str(build / "prototype"), str(build / "kernels.metallib"),
                str(work), "bench", str(args.trials)], stdout=stdout, stderr=stderr, check=True)
        with (raw / f"process-{process}.csv").open() as stream:
            for row in csv.DictReader(stream):
                row["process"] = process
                rows.append(row)
        for case in sorted(work.iterdir()):
            if not case.is_dir():
                continue
            sums = {name: digest(case / f"{name}.kv") for name in ("older", "newer", "oracle", "cpu", "gpu")}
            assert sums["oracle"] == sums["cpu"] == sums["gpu"]
            identities.append({"case": case.name, "process": process, "sha256": sums,
                "bytes": {name: (case / f"{name}.kv").stat().st_size for name in sums}})
    with (output / "observations.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (output / "identities.json").write_text(json.dumps(identities, indent=2, sort_keys=True) + "\n")
    manifest = {str(path.relative_to(output)): digest(path) for path in sorted(output.rglob("*")) if path.is_file()}
    (output / "sha256.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"Retained {len(rows)} observations (including warmups), {len(identities)} complete-file identities.")

if __name__ == "__main__":
    main()
