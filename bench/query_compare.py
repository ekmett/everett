#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Compares pinned complete query implementations with one shared oracle.
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Compare complete queries using pinned headers and one shared oracle harness.

Invoke under the host's exclusive CPU/build-directory resource gate. The runner
never acquires another gate. Every fresh process performs its own warm oracle
pass before one measured trial, followed by another exact result verification.
"""

from snapshot import Snapshot

import argparse
import csv
import datetime
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
import time


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default="71a92a9")
    parser.add_argument("--candidate", default="2d58df0")
    parser.add_argument("--build-dir", type=Path, default=Path("build-query-compare"))
    parser.add_argument("--records", type=int, default=4096)
    parser.add_argument("--queries", type=int, default=4096)
    parser.add_argument("--prefix", type=int, nargs="+", default=[0, 64])
    parser.add_argument("--larger-records", type=int, default=65536)
    parser.add_argument("--larger-prefix", type=int, default=64)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--w16", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if args.trials < 1 or args.queries < 1:
        parser.error("trials and queries must be positive")
    repo = Path(__file__).resolve().parent.parent
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)

    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(repo), *arguments])

    revisions = {name: git("rev-parse", ref).decode().strip()
                 for name, ref in (("baseline", args.baseline), ("candidate", args.candidate))}
    headers = {}
    header_hashes = {}
    normalizations = {}
    for name, revision in revisions.items():
        headers[name] = build / (name + "-headers")
        if headers[name].exists():
            shutil.rmtree(headers[name])
        hashes = {}
        snapshot = Snapshot(repo, revision)
        for path in snapshot.paths:
            data = snapshot.read(path)
            destination = headers[name] / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            hashes[path] = digest(data)
        header_hashes[name] = hashes
        normalizations[name] = snapshot.metadata()
    source = build / "query_compare.cc"
    source.write_bytes((repo / "bench/query_compare.cc").read_bytes())
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    flags += (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
              if args.sanitize else ["-O3", "-DNDEBUG"])
    variants = [("baseline_w15", "baseline", []), ("candidate_w15", "candidate", [])]
    if args.w16:
        variants.append(("candidate_w16", "candidate", ["-DDIET_QUERY_COMPARE_W16=1"]))
    commands = {}
    executables = {}
    for name, revision, definitions in variants:
        executables[name] = build / name
        commands[name] = [*compiler, *flags, *definitions, "-I" + str(headers[revision] / "include"),
                          str(source), "-o", str(executables[name])]
        print("Building " + name, flush=True)
        subprocess.run(commands[name], check=True)
    cases = [(args.records, prefix, args.queries) for prefix in args.prefix]
    if args.larger_records:
        cases.append((args.larger_records, args.larger_prefix, args.queries))
    metadata = {
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revisions": revisions, "headers_sha256": header_hashes, "normalization": normalizations,
        "source_sha256": digest(source.read_bytes()),
        "runner_sha256": digest(Path(__file__).read_bytes()),
        "platform": platform.platform(), "machine": platform.machine(),
        "compiler": subprocess.check_output([*compiler, "--version"], text=True),
        "commands": commands, "cases": cases, "trials": args.trials,
        "sanitize": args.sanitize, "execution_order": [],
        "footprint_scope": "Logical live payload and auxiliary-array bytes; excludes object headers, capacities, allocators, query scratch, and duplicate pins. Counts each shared native array once.",
    }
    if platform.system() == "Darwin":
        metadata["hardware"] = {}
        for field in ("machdep.cpu.brand_string", "hw.memsize", "hw.ncpu"):
            probe = subprocess.run(["sysctl", "-n", field], text=True, capture_output=True)
            metadata["hardware"][field] = (probe.stdout.strip() if probe.returncode == 0 else
                {"unavailable": probe.stderr.strip(), "returncode": probe.returncode})
    rows = []
    expected = {}
    sizes = {}
    invariant_fields = ("head_entries", "prefix_catalogs", "visited_catalogs", "matches", "checksum")
    print("Beginning alternating oracle-checked trials", flush=True)
    for records, prefix, queries in cases:
        for trial in range(args.trials):
            order = variants if trial % 2 == 0 else list(reversed(variants))
            for name, revision, _ in order:
                invocation = [str(executables[name]), str(records), str(prefix), str(queries), "1"]
                start = time.monotonic()
                output = subprocess.check_output(invocation, text=True)
                metadata["execution_order"].append({"variant": name, "records": records, "prefix": prefix,
                    "queries": queries, "trial": trial, "elapsed_seconds": time.monotonic() - start})
                measured = list(csv.DictReader(io.StringIO(output)))
                if len(measured) != 2 or {row["profile"] for row in measured} != {"byte", "bit"}:
                    raise RuntimeError("harness did not emit both profiles")
                for row in measured:
                    identity = (row["profile"], records, prefix, queries)
                    invariant = tuple(row[field] for field in invariant_fields)
                    if identity in expected and expected[identity] != invariant:
                        raise RuntimeError("cross-revision semantic/catalog mismatch: " + repr(identity))
                    expected[identity] = invariant
                    footprint = tuple(row[field] for field in row if field.endswith("_bytes") and field != "prefix_bytes")
                    size_identity = (name, *identity)
                    if size_identity in sizes and sizes[size_identity] != footprint:
                        raise RuntimeError("backing-array sizes changed across trials")
                    sizes[size_identity] = footprint
                    row["trial"] = str(trial)
                    rows.append({"variant": name, "revision": revisions[revision], **row})
            print(f"Completed records={records}, prefix={prefix}, trial={trial + 1}", flush=True)
    metadata["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    with (build / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (build / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("variant,profile,records,prefix,prepare_median_ns,query_median_ns,query_min_ns,query_max_ns,total_array_bytes")
    for identity in dict.fromkeys((r["variant"], r["profile"], r["base_records"], r["prefix_bytes"]) for r in rows):
        group = [r for r in rows if (r["variant"], r["profile"], r["base_records"], r["prefix_bytes"]) == identity]
        query = [float(r["query_ns"]) for r in group]
        preparation = statistics.median(float(r["prepare_ns"]) for r in group)
        print(",".join(identity) + f",{preparation:.3f},{statistics.median(query):.3f},{min(query):.3f},{max(query):.3f},{group[0]['total_array_bytes']}")
    print("Results: " + str(build / "results.csv"))


if __name__ == "__main__":
    main()
