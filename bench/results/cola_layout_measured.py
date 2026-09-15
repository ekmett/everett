#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Four-way whole-COLA-build control for comparator entry alignment.

Run under the host CPU/build-directory resource gate. Header snapshots are
fresh, and only the aligned variants receive the recorded declaration patch.
"""
import argparse
import csv
import datetime
import hashlib
import io
import json
from pathlib import Path
import platform
import shlex
import shutil
import statistics
import subprocess
import tempfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default="111a2d6")
    parser.add_argument("--candidate", default="f12d708")
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--compiler", default="c++")
    args = parser.parse_args()
    if args.trials < 1 or args.rounds < 1:
        parser.error("trials and rounds must be positive")
    repo = Path(__file__).resolve().parent.parent
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    git = lambda *a: subprocess.check_output(["git", "-C", str(repo), *a])
    source = build / "cola_layout.cc"
    source.write_bytes((repo / "bench/cola_layout.cc").read_bytes())
    declaration = "  inline bit_comparison compare_common_bits(bit_view a, bit_view b) {"
    patch = ("#if defined(__APPLE__) && defined(__aarch64__) && defined(__clang__)\n"
             "  // Reduce the out-of-line NEON loop's sensitivity to caller code layout.\n"
             "  [[gnu::aligned(64)]]\n#endif\n" + declaration)
    variants = ["baseline", "candidate", "baseline_aligned", "candidate_aligned"]
    compiler = shlex.split(args.compiler)
    metadata = {
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(), "machine": platform.machine(),
        "compiler": subprocess.check_output([*compiler, "--version"], text=True),
        "source_sha256": sha(source.read_bytes()),
        "runner_sha256": sha(Path(__file__).read_bytes()),
        "alignment_patch": patch, "inputs": {}, "commands": {},
        "trials": args.trials, "rounds": args.rounds, "execution_order": [],
        "wire_files_sha256": {}, "binaries_sha256": {},
    }
    for name in variants:
        revision = git("rev-parse", args.baseline if name.startswith("baseline") else args.candidate).decode().strip()
        headers = build / name / "headers"
        if headers.exists():
            shutil.rmtree(headers)
        for path in git("ls-tree", "-r", "--name-only", revision, "include/everett").decode().splitlines():
            output = headers / path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(git("show", revision + ":" + path))
        if name.endswith("_aligned"):
            profile = headers / "include/everett/profile.h"
            text = profile.read_text()
            assert text.count(declaration) == 1
            profile.write_text(text.replace(declaration, patch))
        metadata["inputs"][name] = {"revision": revision, "alignment64": name.endswith("_aligned"),
            "headers_sha256": {str(p.relative_to(headers)): sha(p.read_bytes())
                for p in sorted((headers / "include").rglob("*.h"))}}
        executable = build / name / "run"
        command = [*compiler, "-O3", "-DNDEBUG", "-std=c++20", "-I" + str(headers / "include"), str(source), "-o", str(executable)]
        metadata["commands"][name] = command
        subprocess.run(command, check=True)
        metadata["binaries_sha256"][name] = sha(executable.read_bytes())
        if platform.system() == "Darwin":
            for tool, suffix in ((["xcrun", "otool", "-tvV"], "asm.txt"), (["xcrun", "size", "-m"], "size.txt")):
                with (build / (name + "." + suffix)).open("w") as output:
                    subprocess.run([*tool, str(executable)], stdout=output, check=True)
    if platform.system() == "Darwin":
        metadata["hardware"] = {}
        for field in ("machdep.cpu.brand_string", "hw.memsize", "hw.ncpu"):
            probe = subprocess.run(["sysctl", "-n", field], text=True, capture_output=True)
            metadata["hardware"][field] = (probe.stdout.strip() if probe.returncode == 0 else
                {"unavailable": probe.stderr.strip(), "returncode": probe.returncode})
    print("Compilation complete. Timing begins.", flush=True)
    columns = "unit,k,prefix,round,items,ns,ns_per_item,bytes,crc".split(",")
    rows, expected = [], None
    for trial in range(args.trials):
        at = trial % len(variants)
        order = variants[at:] + variants[:at]
        if trial & 1:
            order.reverse()
        for name in order:
            with tempfile.TemporaryDirectory(prefix="everett-cola-layout-") as directory:
                command = [str(build / name / "run"), str(args.rounds), directory]
                output = subprocess.check_output(command, text=True)
                files = {p.name: p.read_bytes() for p in Path(directory).glob("*.index")}
                assert len(files) == 8
                if expected is None:
                    expected = files
                    metadata["wire_files_sha256"] = {name: sha(data) for name, data in files.items()}
                assert files == expected, (trial, name, "whole index bytes differ")
            metadata["execution_order"].append({"trial": trial, "variant": name})
            rows.extend({"variant": name, "trial": trial, **row}
                for row in csv.DictReader(io.StringIO(output), fieldnames=columns))
    print("Timing complete.", flush=True)
    with (build / "results.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=["variant", "trial", *columns])
        writer.writeheader()
        writer.writerows(rows)
    summary = []
    for identity in sorted({(r["unit"], int(r["k"]), int(r["prefix"])) for r in rows}):
        unit, k, prefix = identity
        group = [r for r in rows if (r["unit"], int(r["k"]), int(r["prefix"])) == identity]
        assert len({(r["items"], r["bytes"], r["crc"]) for r in group}) == 1
        row = {"unit": unit, "k": k, "prefix": prefix}
        for name in variants:
            values = [float(r["ns_per_item"]) for r in group if r["variant"] == name]
            row[name] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        summary.append(row)
    metadata["summary"] = summary
    metadata["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (build / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
