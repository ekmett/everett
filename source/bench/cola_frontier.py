#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Compare complete COLA builds with exact frozen header revisions.

Run under the host CPU resource gate. Uses the shared cola_layout.cc fixture.
"""
from snapshot import Snapshot
from fixture import write_fixture

import argparse
import csv
import datetime
import hashlib
import io
import json
from pathlib import Path
import platform
import shlex
import statistics
import subprocess


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--compiler", default="c++")
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    if args.trials < 1 or args.rounds < 1:
        parser.error("trials and rounds must be positive")
    repo = Path(__file__).resolve().parent.parent
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=False)
    git = lambda *a: subprocess.check_output(["git", "-C", str(repo), *a])
    source = build / "cola_layout.cc"
    write_fixture(repo / "bench/cola_layout.cc", source)
    compiler = shlex.split(args.compiler)
    metadata = {
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(), "machine": platform.machine(),
        "compiler": subprocess.check_output([*compiler, "--version"], text=True),
        "source_sha256": sha(source.read_bytes()),
        "runner_sha256": sha(Path(__file__).read_bytes()),
        "trials": args.trials, "rounds": args.rounds, "variants": {},
        "execution_order": [], "wire_sha256": {},
    }
    for name, ref in (("baseline", args.baseline), ("candidate", args.candidate)):
        revision = git("rev-parse", ref).decode().strip()
        headers = build / name / "include"
        hashes = {}
        snapshot = Snapshot(repo, revision)
        for path in snapshot.paths:
            target = build / name / path
            target.parent.mkdir(parents=True, exist_ok=True)
            data = snapshot.read(path)
            target.write_bytes(data)
            hashes[path] = sha(data)
        executable = build / name / "run"
        command = [*compiler, "-O3", "-DNDEBUG", "-std=c++20", "-I" + str(headers), str(source), "-o", str(executable)]
        subprocess.run(command, check=True)
        metadata["variants"][name] = {"revision": revision, "headers_sha256": hashes, "normalization": snapshot.metadata(),
            "command": command, "binary_sha256": sha(executable.read_bytes())}
    print("Compilation complete; timing both variants.", flush=True)
    fields = "unit,k,prefix,round,items,ns,ns_per_item,bytes,crc".split(",")
    rows, expected = [], None
    for trial in range(args.trials):
        order = ["baseline", "candidate"] if not trial & 1 else ["candidate", "baseline"]
        metadata["execution_order"].append(order)
        for name in order:
            dump = build / (name + "-" + str(trial))
            output = subprocess.check_output([str(build / name / "run"), str(args.rounds), str(dump)], text=True)
            data = {p.name: p.read_bytes() for p in sorted(dump.glob("*.index"))}
            if len(data) != 8 or (expected is not None and data != expected):
                raise RuntimeError("whole IX03 output differs between processes")
            expected = data
            metadata["wire_sha256"][name + "-" + str(trial)] = {k: sha(v) for k, v in data.items()}
            for row in csv.DictReader(io.StringIO(output), fieldnames=fields):
                rows.append({"variant": name, "trial": trial, **row})
            print(f"Completed trial {trial + 1}: {name}", flush=True)
    output = build / "results.csv"
    with output.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["variant", "trial", *fields])
        writer.writeheader()
        writer.writerows(rows)
    metadata["csv_sha256"] = sha(output.read_bytes())
    metadata["completed_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    metadata["summary"] = []
    for unit, k, prefix in sorted({(r["unit"], int(r["k"]), int(r["prefix"])) for r in rows}):
        case = {"unit": unit, "k": k, "prefix": prefix}
        for name in ("baseline", "candidate"):
            values = [float(r["ns_per_item"]) for r in rows if r["variant"] == name and
                (r["unit"], int(r["k"]), int(r["prefix"])) == (unit, k, prefix)]
            case[name] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        case["median_change_percent"] = 100 * (case["candidate"]["median"] / case["baseline"]["median"] - 1)
        metadata["summary"].append(case)
    (build / "results.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata["summary"], indent=2))


if __name__ == "__main__":
    main()
