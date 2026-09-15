#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Reproduces complete bitmap rank timings with independent directory builds.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Compare pinned full bitmap rank implementations under the caller's resource lease."""

from snapshot import Snapshot

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--baseline", default="c2703db")
    parser.add_argument("--candidate", default="6f7a160")
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--queries", type=int, default=1048576)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    if args.trials < 1 or args.queries < 1:
        parser.error("positive trials and queries required")
    root = Path(__file__).resolve().parent.parent
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)

    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(root), *arguments])

    revisions = {key: git("rev-parse", value).decode().strip()
                 for key, value in (("baseline", args.baseline), ("candidate", args.candidate))}
    include = build / "candidate-headers"
    if include.exists():
        shutil.rmtree(include)
    hashes = {}
    candidate_snapshot = Snapshot(root, revisions["candidate"])
    for name in candidate_snapshot.paths:
        data = candidate_snapshot.read(name)
        path = include / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        hashes[name] = digest(data)
    baseline_snapshot = Snapshot(root, revisions["baseline"])
    old = baseline_snapshot.read("include/diet/rank.h")
    if b"#include <diet/" in old or old.count(b"namespace diet {") != 1:
        raise RuntimeError("review baseline namespace/dependency adapter")
    adapted = old.replace(b"namespace diet {", b"namespace baseline {")
    (build / "baseline_rank.h").write_bytes(adapted)
    source = build / "rank_spacers.cc"
    source.write_bytes((root / "bench/rank_spacers.cc").read_bytes())
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    flags += (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
              if args.sanitize else ["-O3", "-DNDEBUG"])
    executable = build / "rank_spacers"
    command = [args.cxx, *flags, "-I" + str(include / "include"), "-I" + str(build),
               str(source), "-o", str(executable)]
    subprocess.run(command, check=True)
    invocation = [str(executable), str(args.trials), str(args.queries)]
    if args.sanitize:
        invocation.append("check")
    metadata = {
        "revisions": revisions, "normalization": {"baseline": baseline_snapshot.metadata(), "candidate": candidate_snapshot.metadata()}, "candidate_headers_sha256": hashes,
        "baseline_rank_sha256": digest(old), "adapted_baseline_rank_sha256": digest(adapted),
        "source_sha256": digest(source.read_bytes()), "runner_sha256": digest(Path(__file__).read_bytes()),
        "executable_sha256": digest(executable.read_bytes()), "command": command, "invocation": invocation,
        "compiler": subprocess.check_output([args.cxx, "--version"], text=True),
        "platform": platform.platform(), "machine": platform.machine(),
        "trials": args.trials, "queries": args.queries, "sanitize": args.sanitize,
        "execution_order": "For each pattern and trial t: variant t%2 then (t+1)%2; baseline=0, candidate=1.",
        "apple_qos": "QOS_CLASS_USER_INITIATED checked; no CPU affinity",
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }
    result = subprocess.run(invocation, check=True, text=True, capture_output=True)
    metadata["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    metadata["stderr"] = result.stderr
    metadata["csv_sha256"] = digest(result.stdout.encode())
    (build / "results.csv").write_text(result.stdout)
    (build / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(result.stdout, end="")
    print(result.stderr, end="")


if __name__ == "__main__":
    main()
