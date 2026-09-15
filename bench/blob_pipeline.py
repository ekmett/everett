#!/usr/bin/env python3
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Compare pinned headers on complete blob builds, pipelines and window queries."""

from snapshot import Snapshot

import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import statistics
import subprocess


BASELINE = "62ead3fab9d0ee5bda1b47780b7905a45aae1182"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default=BASELINE)
    parser.add_argument("--candidate", default="working-tree")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--records", type=int, default=4096)
    parser.add_argument("--prefix", type=int, default=64)
    parser.add_argument("--queries", type=int, default=4096)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if args.trials < 1:
        parser.error("trials must be positive")
    repo = Path(__file__).resolve().parent.parent
    build = (args.build_dir or repo / "build-blob-pipeline").resolve()
    build.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    flags += (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
              if args.sanitize else ["-O3", "-DNDEBUG"])
    # Capture the harness too: an edit while the two variants compile must not
    # silently give the baseline and candidate different benchmark bodies.
    source = build / "blob_pipeline.cc"
    source.write_bytes((repo / "bench/blob_pipeline.cc").read_bytes())
    metadata = {
        "compiler": subprocess.check_output([*compiler, "--version"], text=True),
        "flags": flags,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "records": args.records, "prefix_bytes": args.prefix,
        "queries": args.queries, "rounds": args.rounds, "trials": args.trials,
        "sanitize": args.sanitize, "variants": {},
    }
    executables = {}
    for name, revision in (("baseline", args.baseline), ("candidate", args.candidate)):
        root = build / name
        headers = root / "include"
        if headers.exists():
            shutil.rmtree(headers)
        headers.mkdir(parents=True)
        normalization = None
        if revision == "working-tree":
            shutil.copytree(repo / "include/diet", headers / "diet")
            commit = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
        else:
            commit = subprocess.check_output(["git", "-C", str(repo), "rev-parse", revision], text=True).strip()
            snapshot = Snapshot(repo, commit)
            for path in snapshot.paths:
                destination = headers / Path(path).relative_to("include")
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(snapshot.read(path))
            normalization = snapshot.metadata()
        metadata["variants"][name] = {
            "selection": revision, "commit": commit, "normalization": normalization,
            "headers_sha256": {str(p.relative_to(headers)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in sorted(headers.rglob("*")) if p.is_file()},
        }
        executable = root / "blob_pipeline"
        subprocess.run([*compiler, *flags, "-I" + str(headers), str(source), "-o", str(executable)], check=True)
        executables[name] = executable
    metadata_file = build / "metadata.json"
    metadata_file.write_text(json.dumps(metadata, indent=2) + "\n")
    rows = []
    expected = {}
    dimensions = list(map(str, (args.records, args.prefix, args.queries, args.rounds)))
    for trial in range(args.trials):
        order = ("baseline", "candidate") if trial % 2 == 0 else ("candidate", "baseline")
        for name in order:
            output = subprocess.check_output([str(executables[name]), *dimensions], text=True)
            (build / f"{name}-{trial}.csv").write_text(output)
            for row in csv.DictReader(io.StringIO(output)):
                key = (row["profile"], row["operation"])
                identity = (row["encoding_digest"], row["digest_bytes"], row["checksum"])
                if key in expected and expected[key] != identity:
                    raise RuntimeError(f"output differs from baseline for {key}: {identity} != {expected[key]}")
                expected[key] = identity
                rows.append({"variant": name, "trial": trial, **row})
    output = args.output or build / "results.csv"
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print("profile,operation,baseline_median_ns,candidate_median_ns,speedup")
    for profile, operation in expected:
        times = {}
        for name in executables:
            times[name] = statistics.median(float(row["ns_per_operation"]) for row in rows
                                            if row["variant"] == name and row["profile"] == profile
                                            and row["operation"] == operation)
        print(f"{profile},{operation},{times['baseline']:.3f},{times['candidate']:.3f},{times['baseline'] / times['candidate']:.3f}")
    print(f"Results: {output}\nMetadata: {metadata_file}")


if __name__ == "__main__":
    main()

# \file
# \author Edward Kmett
# \brief Reproduces whole-blob and pipeline comparisons using pinned header snapshots.
