#!/usr/bin/env python3
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
"""Compare identical key/bit fixtures using a pinned baseline and current headers."""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import tarfile

BASE = "62ead3fab9d0ee5bda1b47780b7905a45aae1182"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--candidate", help="candidate Git revision; omit to use working-tree headers")
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--work", type=int, default=8388608)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if args.trials < 1 or args.work < 1:
        parser.error("trials and work must be positive")
    repo = Path(__file__).resolve().parent.parent
    build = (args.build_dir or repo / "build-key-bits-bench").resolve()
    build.mkdir(parents=True, exist_ok=True)
    def extract(reference, destination):
        archive = subprocess.check_output(["git", "-C", str(repo), "archive", reference, "include/"])
        with tarfile.open(fileobj=io.BytesIO(archive)) as data:
            for member in data:
                if member.isfile():
                    target = destination / member.name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data.extractfile(member).read())
        return destination / "include"

    baseline = extract(BASE, build / "baseline")
    # Isolate these changes: every dependency is the same pinned baseline in
    # both builds, even when the candidate revision contains unrelated work.
    headers = ["profile.h", "front.h", "key_detail.h"]
    candidate = build / "candidate-overlay"
    (candidate / "everett").mkdir(parents=True, exist_ok=True)
    for name in headers:
        source = "include/everett/" + name
        contents = subprocess.check_output(["git", "-C", str(repo), "show", args.candidate + ":" + source]) if args.candidate else (repo / source).read_bytes()
        (candidate / "everett" / name).write_bytes(contents)
    compiler = os.environ.get("CXX", "clang++")
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else ["-O3", "-DNDEBUG"]
    executables = {}
    for variant, headers in (("baseline", baseline), ("candidate", candidate)):
        executable = build / ("key_bits_" + variant + ("-sanitize" if args.sanitize else ""))
        subprocess.run([compiler, *flags, "-I" + str(headers), "-I" + str(baseline), str(repo / "bench/key_bits.cc"), "-o", str(executable)], check=True)
        executables[variant] = executable
    rows = []
    expected = {}
    for trial in range(args.trials):
        order = ("baseline", "candidate") if trial % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            output = subprocess.check_output([str(executables[variant]), str(args.work)], text=True)
            for row in csv.DictReader(io.StringIO(output)):
                key = tuple(row[k] for k in ("operation", "bits", "source_offset", "iterations"))
                answer = tuple(row[k] for k in ("checksum", "encoding"))
                if key in expected and answer != expected[key]:
                    raise RuntimeError("baseline/candidate answer or encoding differs: " + str(key))
                expected[key] = answer
                rows.append({"trial": trial, "variant": variant, **row})
    output = args.output or build / "results.csv"
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    headers = ["profile.h", "front.h", "key_detail.h"]
    hashes = {name: hashlib.sha256((candidate / "everett" / name).read_bytes()).hexdigest() for name in headers}
    metadata = {"baseline": BASE, "candidate_revision": subprocess.check_output(["git", "-C", str(repo), "rev-parse", args.candidate or "HEAD"], text=True).strip(),
                "candidate_working_tree": args.candidate is None, "dependency_revision": BASE, "candidate_overlay": headers,
                "candidate_header_sha256": hashes, "source_sha256": hashlib.sha256((repo / "bench/key_bits.cc").read_bytes()).hexdigest(),
                "compiler": subprocess.check_output([compiler, "--version"], text=True), "flags": flags,
                "platform": platform.platform(), "machine": platform.machine(), "trials": args.trials,
                "work": args.work, "sanitize": args.sanitize}
    output.with_suffix(".json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(output)


if __name__ == "__main__":
    main()

# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Runs pinned baseline/candidate key and bit comparisons with exact-answer checks.
