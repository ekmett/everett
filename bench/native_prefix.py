#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Runs alternating shared-harness native-writer comparisons.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Compare pinned native writers with a shared wire/key-checked harness."""

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


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default="fdcad44")
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--records", type=int, default=16384)
    parser.add_argument("--prefix", type=int, nargs="+", default=[0, 64, 4096])
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if min(args.records, args.rounds, args.trials) < 1 or min(args.prefix) < 0:
        parser.error("positive records, rounds, trials and nonnegative prefixes required")
    repo = Path(__file__).resolve().parent.parent
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    flags += (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
              if args.sanitize else ["-O3", "-DNDEBUG"])
    source = build / "native_prefix.cc"
    source.write_bytes((repo / "bench/native_prefix.cc").read_bytes())
    metadata = {
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "source_sha256": digest(source.read_bytes()),
        "runner_sha256": digest(Path(__file__).read_bytes()),
        "compiler": subprocess.check_output([*compiler, "--version"], text=True),
        "platform": platform.platform(), "machine": platform.machine(),
        "records": args.records, "prefix_bytes": args.prefix, "rounds": args.rounds,
        "trials": args.trials, "sanitize": args.sanitize, "variants": {}, "execution_order": [],
    }
    variants = {}
    for name, ref in (("baseline", args.baseline), ("candidate", args.candidate)):
        revision = subprocess.check_output(["git", "-C", str(repo), "rev-parse", ref], text=True).strip()
        snapshot = Snapshot(repo, revision)
        paths = snapshot.paths
        headers = build / name / "headers"
        if headers.exists():
            shutil.rmtree(headers)
        hashes = {}
        for path in paths:
            data = snapshot.read(path)
            destination = headers / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            hashes[path] = digest(data)
        executable = build / name / "native_prefix"
        command = [*compiler, *flags, "-I" + str(headers / "include"), str(source), "-o", str(executable)]
        metadata["variants"][name] = {"revision": revision, "headers_sha256": hashes, "normalization": snapshot.metadata(), "command": command}
        variants[name] = executable
        print("Building " + name, flush=True)
        subprocess.run(command, check=True)
    rows = []
    expected = {}
    for prefix in args.prefix:
        for trial in range(args.trials):
            order = ("baseline", "candidate") if trial % 2 == 0 else ("candidate", "baseline")
            for name in order:
                invocation = [str(variants[name]), str(args.records), str(prefix), str(args.rounds)]
                output = subprocess.check_output(invocation, text=True)
                measured = list(csv.DictReader(io.StringIO(output)))
                if len(measured) != 4 * args.rounds:
                    raise RuntimeError("missing harness results")
                metadata["execution_order"].append({"variant": name, "prefix_bytes": prefix, "trial": trial})
                for row in measured:
                    identity = (row["profile"], row["records"], row["prefix_bytes"])
                    invariant = (row["payload_bytes"], row["wire_digest"])
                    if identity in expected and expected[identity] != invariant:
                        raise RuntimeError("cross-variant wire mismatch: " + repr(identity))
                    expected[identity] = invariant
                    rows.append({"variant": name, "revision": metadata["variants"][name]["revision"],
                        "trial": trial, **row})
            print(f"Completed prefix={prefix}, trial={trial + 1}", flush=True)
    metadata["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    with (build / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)
    (build / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("variant,profile,prefix_bytes,median_ns_per_record,min,max")
    for identity in dict.fromkeys((r["variant"], r["profile"], r["prefix_bytes"]) for r in rows):
        group = [float(r["record_ns"]) for r in rows
                 if (r["variant"], r["profile"], r["prefix_bytes"]) == identity]
        print(",".join(identity) + f",{statistics.median(group):.3f},{min(group):.3f},{max(group):.3f}")


if __name__ == "__main__":
    main()
