#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds the optional GPU experiment through validated HLSL translation.
#
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Standalone opt-in build; the installed header-only library is unchanged."""

import argparse
import os
from pathlib import Path
import subprocess


def run(*command):
    subprocess.run([str(value) for value in command], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-gpu-merge")
    parser.add_argument("--dxc", default=os.environ.get("EVERETT_DXC", "dxc"))
    parser.add_argument("--spirv-val", default=os.environ.get("EVERETT_SPIRV_VAL", "spirv-val"))
    parser.add_argument("--spirv-cross", default=os.environ.get("EVERETT_SPIRV_CROSS", "spirv-cross"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = (root / args.build).resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = Path(__file__).resolve().with_name("shader_compile.py")
    entries = [
        "scan_blocks", "scan_add", "rank_count", "rank_finish", "rank15_count",
        "rank15_finish", "probe_copy", "merge_order", "merge_keep", "merge_compact",
        "merge_sizes", "merge_emit", "rank15_classes", "parse_input", "parse_ef_probe",
        "ef_output_sparse_count", "ef_output_low", "ef_output_high", "ef_output_samples", "ef_output_sparse",
        "index_rank_classes", "index_rank_blocks",
    ]
    jobs = [(entry, entry, False, False) for entry in entries]
    for entry in ["prefix_leaf", "prefix_reduce", "compressed_prefix", "compressed_probe"]:
        jobs.append((entry, entry, True, False))
    for entry in ["merge_order", "merge_keep", "merge_sizes", "merge_emit"]:
        jobs.append((entry, "compressed_" + entry, True, False))
    jobs.append(("compressed_merge_emit_words", "compressed_merge_emit_words", True, False))
    jobs.append(("word_emit_probe", "word_emit_probe", True, False))
    jobs.append(("compressed_cache", "compressed_cache", True, True))
    for entry in ["merge_order", "merge_keep", "merge_sizes"]:
        jobs.append((entry, "cached_" + entry, True, True))
    jobs = [(entry, name, compressed, cached, False) for entry, name, compressed, cached in jobs]
    for entry in ["collision_clear", "collision_mark", "collision_mark_tiled", "collision_rank512_count",
                  "collision_rank512_finish", "collision_rank2048_finish", "collision_rank_probe",
                  "collision_scatter_a", "collision_scatter_a_tiled", "collision_scatter_b",
                  "collision_compact_a", "collision_merge_compact"]:
        jobs.append((entry, entry, True, True, False))
    for entry in ["clear", "rank_probe", "scatter_a", "scatter_a_tiled", "scatter_b", "compact_a"]:
        name = "collision_rank2048_" + ("probe" if entry == "rank_probe" else entry)
        jobs.append(("collision_" + entry, name, True, True, True))
    for entry, name, compressed, cached, rank2048 in jobs:
        stem = output / name
        run("python3", compiler, "--source", root / "optional/gpu_merge/kernels.hlsl",
            "--entry", entry, "--profile", "cs_6_0", "--spv", f"{stem}.spv",
            "--msl", f"{stem}.metal", "--msl-entry", name, "--output-entry", name, "--dxc", args.dxc,
            "--spirv-val", args.spirv_val, "--spirv-cross", args.spirv_cross,
            *(["--define", "COMPRESSED_INPUT=1"] if compressed else []),
            *(["--define", "PREFIX_CACHE=1"] if cached else []),
            *(["--define", "COLLISION_RANK2048=1"] if rank2048 else []))
        run("xcrun", "-sdk", "macosx", "metal", "-std=metal3.2",
            f"-fmodules-cache-path={output / 'metal-module-cache'}", "-fno-fast-math",
            "-c", f"{stem}.metal", "-o", f"{stem}.air")
    run("xcrun", "-sdk", "macosx", "metallib",
        *(f"{output / name}.air" for _, name, _, _, _ in jobs), "-o", output / "kernels.metallib")
    run("xcrun", "clang++", "-std=c++20", "-O3", "-DNDEBUG", "-Wall", "-Wextra",
        "-Wpedantic", "-Werror", "-fobjc-arc", f"-I{root / 'include'}",
        root / "optional/gpu_merge/prototype.mm", "-framework", "Foundation",
        "-framework", "Metal", "-o", output / "prototype")


if __name__ == "__main__":
    main()
