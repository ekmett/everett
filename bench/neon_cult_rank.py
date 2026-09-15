#!/usr/bin/env python3
# \file
# \author Edward Kmett
# \brief Runs the pinned NEON/Cult rank comparison with an external source header.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
"""Reproduce NEON rank15/Cult rank checks without copying the external header."""
from snapshot import Snapshot

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

BASE = "aeaaa9d9896db9aec1a003a0a5ba7b0b6174bd06"
CULT_SHA256 = "96773e2dbaa8dacd92c22173f97e63bb5bbc2604f1194541cfefe83eb1f79ed0"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=("check", "hot", "large", "all"))
    parser.add_argument("--cult-header", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--queries", type=int, default=1048576)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    build = (args.build_dir or repo / "build-neon-cult-rank").resolve()
    build.mkdir(parents=True, exist_ok=True)
    cult = args.cult_header.resolve()
    if hashlib.sha256(cult.read_bytes()).hexdigest() != CULT_SHA256:
        raise SystemExit("external Cult header differs from the reviewed source hash")
    snapshot = Snapshot(repo, BASE)
    base = snapshot.read("include/diet/rank15.h")
    baseline_dir = build / "baseline/diet"
    baseline_dir.mkdir(parents=True, exist_ok=True)
    (baseline_dir / "rank15.h").write_bytes(base)
    text = base.decode()
    old = "return vaddlvq_u8(vaddq_u8(vaddq_u8(a, b), vaddq_u8(c, d)));"
    new = "auto pairs = vaddq_u8(vaddq_u8(a, b), vaddq_u8(c, d));\n      return sum_bytes(vaddvq_u64(vreinterpretq_u64_u8(pairs)));"
    if text.count(old) != 1:
        raise SystemExit("pinned NEON implementation does not match expected reduction")
    candidate = build / "neon_qword_rank15.h"
    candidate.write_text(text.replace(old, new))
    compiler = os.environ.get("CXX", "clang++")
    executable = build / ("neon_cult_rank_sanitize" if args.sanitize else "neon_cult_rank")
    flags = ["-std=c++20", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else ["-std=c++20", "-O3", "-DNDEBUG"]
    command = [compiler, *flags, "-Wall", "-Wextra", "-Werror", "-I" + str(baseline_dir.parent),
               '-DDIET_NEON_QWORD_HEADER="' + str(candidate) + '"',
               '-DDIET_EXTERNAL_CULT_RANK_HEADER="' + str(cult) + '"',
               str(repo / "bench/neon_cult_rank.cc"), "-o", str(executable)]
    subprocess.run(command, check=True)
    cult_revision = subprocess.check_output(["git", "-C", str(cult.parent), "rev-parse", "HEAD"], text=True).strip()
    metadata = {"diet_revision": BASE, "normalization": snapshot.metadata(), "diet_rank_sha256": hashlib.sha256(base).hexdigest(),
                "candidate_sha256": hashlib.sha256(candidate.read_bytes()).hexdigest(),
                "cult_revision": cult_revision, "cult_rank_sha256": CULT_SHA256,
                "source_sha256": hashlib.sha256((repo / "bench/neon_cult_rank.cc").read_bytes()).hexdigest(),
                "compiler": subprocess.check_output([compiler, "--version"], text=True),
                "flags": flags + ["-Wall", "-Wextra", "-Werror"], "case": args.case,
                "trials": args.trials, "queries": args.queries, "sanitize": args.sanitize}
    (build / (args.case + ("-sanitize" if args.sanitize else "") + "-metadata.json")).write_text(json.dumps(metadata, indent=2) + "\n")
    run = [str(executable), args.case, str(args.trials), str(args.queries)]
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w") as stream:
            subprocess.run(run, stdout=stream, check=True)
    else:
        subprocess.run(run, check=True)


if __name__ == "__main__":
    main()
