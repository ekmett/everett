#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds byte-profile shaders as validated SPIR-V and Metal.
#
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import os
from pathlib import Path
import subprocess

ENTRIES = ["scan_blocks", "scan_add", "byte_parse_input", "prefix_leaf", "prefix_reduce",
           "compressed_prefix", "compressed_cache", "merge_order", "merge_keep", "merge_compact",
           "byte_value_width", "byte_merge_sizes", "byte_merge_emit_words", "ef_output_sparse_count",
           "ef_output_low", "ef_output_high", "ef_output_samples", "ef_output_sparse"]

def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--build", type=Path, default=Path("build-byte-gpu"))
    p.add_argument("--dxc", default=os.environ.get("EVERETT_DXC", "dxc"))
    p.add_argument("--spirv-val", default=os.environ.get("EVERETT_SPIRV_VAL", "spirv-val"))
    p.add_argument("--spirv-cross", default=os.environ.get("EVERETT_SPIRV_CROSS", "spirv-cross"))
    p.add_argument("--sanitize", action="store_true")
    args = p.parse_args()
    source = Path(__file__).resolve().parent
    root = source.parents[1]
    output = (root / args.build).resolve()
    output.mkdir(parents=True, exist_ok=True)
    for entry in ENTRIES:
        stem = output / entry
        run("python3", source.parent / "gpu_merge/shader_compile.py", "--source", source / "kernels.hlsl",
            "--entry", entry, "--profile", "cs_6_0", "--spv", f"{stem}.spv", "--msl", f"{stem}.metal",
            "--msl-entry", entry, "--output-entry", entry, "--dxc", args.dxc,
            "--spirv-val", args.spirv_val, "--spirv-cross", args.spirv_cross)
        run("xcrun", "-sdk", "macosx", "metal", "-std=metal3.2", "-fno-fast-math",
            f"-fmodules-cache-path={output / 'metal-module-cache'}", "-c", f"{stem}.metal", "-o", f"{stem}.air")
    run("xcrun", "-sdk", "macosx", "metallib", *(output / f"{entry}.air" for entry in ENTRIES),
        "-o", output / "kernels.metallib")
    run("xcrun", "clang++", "-std=c++20", "-O2" if args.sanitize else "-O3", "-DNDEBUG",
        "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fobjc-arc", f"-I{root / 'include'}",
        *( ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else [] ),
        source / "prototype.mm", "-framework", "Foundation", "-framework", "Metal", "-o", output / "prototype")

if __name__ == "__main__":
    main()
