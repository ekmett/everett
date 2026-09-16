#!/usr/bin/env python3
"""Collect repeatable same-sequence offset access measurements."""
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Records pinned source, exact sequences, and rotated process trials.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import platform
import struct
import subprocess
import time


def command(args, cwd=None):
    return subprocess.check_output([str(x) for x in args], cwd=cwd, text=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True, type=Path)
parser.add_argument("--sux", required=True, type=Path)
parser.add_argument("--output", required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
binary, sux, output = args.binary.resolve(), args.sux.resolve(), args.output.resolve()
if command(["git", "status", "--porcelain"], repo).strip():
    raise SystemExit("Commit the experiment before collecting timings")
if output.exists():
    raise SystemExit("Output directory already exists")
output.mkdir(parents=True)
sequences = output / "sequences"
raw = output / "raw"
sequences.mkdir()
raw.mkdir()
revision = command(["git", "rev-parse", "HEAD"], repo).strip()
source_files = sorted(set((repo / "include/everett").rglob("*.h")) | set((repo / "optional/select_compare").glob("*")) | {
    repo / "optional/fixed_kv_compare/cases.h", repo / "optional/fixed_gpu_merge/fixtures.h"})
source_hashes = {str(p.relative_to(repo)): digest(p) for p in source_files if p.is_file()}
external_files = command(["git", "ls-files"], sux).splitlines()
external_hashes = {name: digest(sux / name) for name in external_files if (sux / name).is_file()}
metadata = {
    "source_revision": revision, "binary_sha256": digest(binary), "source_sha256": source_hashes,
    "external_revision": command(["git", "rev-parse", "HEAD"], sux).strip(),
    "external_clean": not command(["git", "status", "--porcelain"], sux).strip(),
    "external_sha256": external_hashes, "platform": platform.platform(), "machine": platform.machine(),
    "compiler": command(["clang++", "--version"]), "processes": 3, "trials": 3,
    "queries_actual": 65536, "queries_supplemental": 262144,
    "build_flags": "Release C++20, CMake defaults (-O3 -DNDEBUG), no architecture override",
    "scope": "offset access on exact library-built directories plus explicitly derived synthetic sequences",
}
for key in ("hw.model", "hw.memsize", "machdep.cpu.brand_string"):
    try:
        metadata[key] = command(["sysctl", "-n", key]).strip()
    except subprocess.CalledProcessError:
        pass
patch_manifest = binary.parent / "external/sux-patch-manifest.json"
metadata["sux_patch"] = json.loads(patch_manifest.read_text())
write_json(output / "manifest.json", metadata)
(output / "qualification.txt").write_text(command([binary, "check"]))
actual = []
for case in range(12):
    log = command([binary, "export", sequences, case])
    (raw / f"export-{case}.csv").write_text(log)
    actual.extend(dict(row, kind="actual-library-encoded", recipe=f"export case {case}") for row in csv.DictReader(io.StringIO(log)))
    print(f"exported case {case}", flush=True)
recipes = []
for source, exponent in [
    ("case-2.raw-byte.native-output", 20), ("case-2.raw-byte.native-output", 22),
    ("case-8.raw-bit.native-output", 22), ("case-5.typed-byte.native-output", 22),
    ("case-11.typed-bit.native-output", 22), ("case-11.typed-bit.index-main", 22),
]:
    name = f"replay-{source}-{exponent}"
    command([binary, "replay", sequences / f"{source}.seq", sequences / f"{name}.seq", 1 << exponent])
    recipes.append({"sequence": name, "kind": "synthetic-replayed-observed-gaps", "source": source,
                    "recipe": f"repeat consecutive source gaps to {1 << exponent} offsets, starting at zero"})
for span in (65535, 65536, 65537):
    name = f"synthetic-half-boundary-{span}"
    values = [0] * 1024 + [span - 1024] * (40000 - 1024)
    (sequences / f"{name}.seq").write_bytes(struct.pack("<QQ", len(values), values[-1]) + struct.pack("<40000Q", *values))
    recipes.append({"sequence": name, "kind": "synthetic-boundary", "recipe": f"1024 zeros followed by 38976 copies of {span - 1024}"})
sequence_metadata = actual + recipes
for item in sequence_metadata:
    path = sequences / (item["sequence"] + ".seq")
    count, universe = struct.unpack("<QQ", path.read_bytes()[:16])
    width = (universe // count).bit_length() - 1 if universe // count else 0
    item.update(sha256=digest(path), count=count, universe=universe, low_width=width,
                high_bits=(universe >> width) + count, file_bytes=path.stat().st_size)
write_json(output / "sequences.json", sequence_metadata)
selected = [x for x in actual if x["sequence"].endswith(("native-output", "index-main"))] + recipes
start = time.time()
for process in range(3):
    # Reverse between processes as well as rotating candidate order.
    schedule = selected if process != 1 else list(reversed(selected))
    for number, item in enumerate(schedule):
        name = item["sequence"]
        queries = 65536 if item["kind"] == "actual-library-encoded" else 262144
        result = subprocess.run([str(binary), "bench", str(sequences / f"{name}.seq"), str(queries), "3", str(process * 4)], text=True, capture_output=True)
        (raw / f"{name}.p{process}.csv").write_text(result.stdout)
        (raw / f"{name}.p{process}.stderr").write_text(result.stderr)
        if result.returncode:
            raise SystemExit(f"Failed {name} process {process}: {result.stderr}")
        print(f"process {process + 1}/3 sequence {number + 1}/{len(selected)} {name}", flush=True)
metadata["collection_seconds"] = time.time() - start
metadata["timed_sequences"] = len(selected)
metadata["completed_processes"] = len(selected) * 3
write_json(output / "manifest.json", metadata)
write_json(output / "raw-sha256.json", {p.name: digest(p) for p in sorted(raw.iterdir())})
print(f"Complete: {len(selected) * 3} processes", flush=True)
