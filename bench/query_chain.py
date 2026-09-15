#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Reproduces whole-chain query measurements with pinned headers and independent oracles.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Measure complete chain queries against pinned headers under the caller's resource gate."""

from snapshot import Snapshot
from fixture import write_fixture

import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", default="2bf591f")
    parser.add_argument("--build-dir", type=Path, default=Path("build-query-chain"))
    parser.add_argument("--records", type=int, default=4096)
    parser.add_argument("--queries", type=int, default=4096)
    parser.add_argument("--prefix", type=int, nargs="+", default=[0, 64])
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    revision = subprocess.check_output(["git", "-C", str(repo), "rev-parse", args.revision], text=True).strip()
    snapshot = Snapshot(repo, revision)
    paths = snapshot.paths
    headers = build / "headers"
    if headers.exists():
        shutil.rmtree(headers)
    hashes = {}
    for path in paths:
        data = snapshot.read(path)
        target = headers / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        hashes[path] = hashlib.sha256(data).hexdigest()
    source = build / "query_chain.cc"
    write_fixture(repo / "bench/query_chain.cc", source)
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    flags += (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
              if args.sanitize else ["-O3", "-DNDEBUG"])
    executable = build / "query_chain"
    command = [*compiler, *flags, "-I" + str(headers / "include"), str(source), "-o", str(executable)]
    subprocess.run(command, check=True)
    metadata = {
        "revision": revision, "headers_sha256": hashes, "normalization": snapshot.metadata(),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "platform": platform.platform(),
        "compiler": subprocess.check_output([*compiler, "--version"], text=True),
        "command": command, "records": args.records, "queries": args.queries,
        "prefixes": args.prefix, "trials": args.trials, "sanitize": args.sanitize,
    }
    (build / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    rows = []
    for prefix in args.prefix:
        output = subprocess.check_output([str(executable), str(args.records), str(prefix),
                                          str(args.queries), str(args.trials)], text=True)
        rows.extend(csv.DictReader(io.StringIO(output)))
    with (build / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print("profile,prefix_bytes,prepare_median_ns,query_median_ns,query_min_ns,query_max_ns")
    for profile, prefix in dict.fromkeys((row["profile"], row["prefix_bytes"]) for row in rows):
        selected = [row for row in rows if row["profile"] == profile and row["prefix_bytes"] == prefix]
        preparation = statistics.median(float(row["prepare_ns"]) for row in selected)
        queries = [float(row["query_ns"]) for row in selected]
        print(f"{profile},{prefix},{preparation:.3f},{statistics.median(queries):.3f},{min(queries):.3f},{max(queries):.3f}")
    print(f"Results: {build / 'results.csv'}")


if __name__ == "__main__":
    main()
