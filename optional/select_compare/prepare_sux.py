#!/usr/bin/env python3
"""Prepare an isolated, explicitly patched comparator from a pinned external Sux checkout."""
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

REVISION = "568903f1b7957ef03620ebad65d6ef68e031fef9"
parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
revision = subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip()
if revision != REVISION:
    raise SystemExit("Unexpected external Sux revision")
if subprocess.check_output(["git", "-C", str(args.source), "status", "--porcelain"], text=True).strip():
    raise SystemExit("External checkout must be clean")
source = args.source / "sux/bits/SimpleSelectHalf.hpp"
original = source.read_text()
before = "if (span > (1 << 16)) inventory[inventory_index]"
after = "if (span >= (1 << 16)) inventory[inventory_index]"
if original.count(before) != 1:
    raise SystemExit("Upstream boundary patch no longer applies exactly")
patched = original.replace(before, after)
# Name/path adaptations allow original and corrected classes in one binary.
patched = patched.replace("SimpleSelectHalf", "SimpleSelectHalfFixed")
patched = patched.replace('"../support/common.hpp"', "<sux/support/common.hpp>")
patched = patched.replace('"../util/Vector.hpp"', "<sux/util/Vector.hpp>")
patched = patched.replace('"Select.hpp"', "<sux/bits/Select.hpp>")
args.output.mkdir(parents=True, exist_ok=True)
destination = args.output / "everett_optional_sux_half_fixed.hpp"
destination.write_text(patched)
manifest = {
    "repository": "https://github.com/vigna/sux", "revision": revision,
    "original_file": "sux/bits/SimpleSelectHalf.hpp",
    "original_sha256": hashlib.sha256(original.encode()).hexdigest(),
    "patched_sha256": hashlib.sha256(patched.encode()).hexdigest(),
    "algorithm_change": {"before": before, "after": after},
    "name_only_change": "SimpleSelectHalf -> SimpleSelectHalfFixed",
    "include_path_only_change": "relative headers -> sux/... headers",
    "license": "External source retains its GPLv3 + GCC Runtime Library Exception notice",
}
(args.output / "sux-patch-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
