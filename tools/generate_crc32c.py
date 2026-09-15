#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Regenerates Diet's pinned CRC32C backends.
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Regenerate the pinned Corsix CRC32C backends; consumers need not run this."""

import argparse
import hashlib
import pathlib
import re
import subprocess
import tempfile

REVISION = "13f5289ceb6065d014f9e1e64f161ea7a926c3b7"
SOURCE_SHA256 = "d6980f19f0d814ffb9cf2a48692c13b2ae36d04325ff93f411498e5aa2329bf1"
VARIANTS = {
    "portable": ("none", "s1"),
    "arm_scalar": ("neon", "s1"),
    "arm_pmull": ("neon", "v12_v1"),
    "arm_eor3": ("neon_eor3", "v9s3x2_s3"),
    "x86_scalar": ("sse", "s1"),
    "x86_pclmul": ("sse", "v8s3x3"),
    "x86_avx512": ("avx512", "v9s3x4"),
    "x86_vpclmul": ("avx512_vpclmulqdq", "v4s5x3"),
}


def adapt(source, name, isa, algorithm):
    """Only C++ linkage, alias-safe loads, literals and macro scope are changed."""
    source = re.sub(r"^/\*.*?\*/\n", "", source, flags=re.MULTILINE)
    source = re.sub(r"^#include <[^>]+>\n", "", source, flags=re.MULTILINE)
    source = source.replace("#define CRC_EXPORT extern", "#define CRC_EXPORT inline")
    source = source.replace("CRC_AINLINE static __forceinline", "CRC_AINLINE __forceinline")
    source = source.replace("CRC_AINLINE static __inline", "CRC_AINLINE inline")
    source = source.replace("static const uint32_t g_crc_table", "inline constexpr uint32_t g_crc_table")
    source = source.replace("static uint32_t xnmodp", "inline uint32_t xnmodp")
    source = source.replace("(uint64x2_t){", "uint64x2_t{")
    source = re.sub(r"\*\(const (uint(?:8|32|64)_t)\*\)(\([^()]*\)|[a-zA-Z_]\w*)",
                    r"load_little<\1>(\2)", source)
    if re.search(r"\*\(const uint(?:8|32|64)_t\*\)", source):
        raise RuntimeError("unhandled generated scalar load")
    macros = sorted(set(re.findall(r"^#define (\w+)", source, re.MULTILINE)))
    for macro in macros:
        source = re.sub(r"\b" + macro + r"\b", "DIET_GENERATED_" + macro, source)
    notice = (
        "/*\n"
        " * SPDX-FileCopyrightText: 2023 Peter Cawley\n"
        " * SPDX-License-Identifier: MIT OR Zlib\n"
        " * Generated from corsix/fast-crc32 " + REVISION + ".\n"
        " * Parameters: -i " + isa + " -p crc32c -a " + algorithm + ".\n"
        " * C++ adaptation: tools/generate_crc32c.py; do not edit by hand.\n"
        " * The upstream notices are in third_party/fast-crc32/LICENSE*.md\n"
        " * and the installed share/diet/third_party/fast-crc32 directory.\n"
        " */\n\n"
    )
    return notice + "namespace diet::crc32c_detail::" + name + " {\n" + source.strip() + (
        "\n\n" + "".join("#undef DIET_GENERATED_" + macro + "\n" for macro in macros) + "}\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc", help="host C compiler")
    parser.add_argument("--source", type=pathlib.Path,
                        help="override pinned vendored generate.c path")
    parser.add_argument("--check", action="store_true", help="fail if output differs")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    source = args.source or root / "third_party/fast-crc32/generate.c"
    if hashlib.sha256(source.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise SystemExit("generator does not match the pinned upstream source")
    target = root / "include/diet/detail"
    target.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="diet-crc32c-") as directory:
        executable = pathlib.Path(directory) / "generate"
        subprocess.run([args.cc, "-O2", str(source), "-o", str(executable)], check=True)
        for name, (isa, algorithm) in VARIANTS.items():
            generated = subprocess.check_output(
                [str(executable), "-i", isa, "-p", "crc32c", "-a", algorithm], text=True)
            result = adapt(generated, name, isa, algorithm)
            path = target / ("crc32c_" + name + ".inc")
            if args.check:
                if path.read_text() != result:
                    raise SystemExit("stale generated backend: " + str(path))
            else:
                path.write_text(result)


if __name__ == "__main__":
    main()
