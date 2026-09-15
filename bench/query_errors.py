#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Compares success-path query costs of exception layout and fail-stop checks.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Measure unchanged query checks with inline throws, cold throws or fail-stop helpers."""

from snapshot import Snapshot

import argparse
import csv
import datetime
import difflib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import statistics
import subprocess


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default="fea6afc")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--queries", type=int, default=4096)
    parser.add_argument("--records", type=int, default=4096)
    parser.add_argument("--larger-records", type=int, default=65536)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if min(args.queries, args.records, args.trials) < 1 or args.larger_records < 0:
        parser.error("positive records, queries and trials required")
    repo = Path(__file__).resolve().parent.parent
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(repo), *arguments])
    revision = git("rev-parse", args.baseline).decode().strip()
    snapshot = Snapshot(repo, revision)
    original = {p: snapshot.read(p) for p in snapshot.paths}
    closure = set()
    def visit(path):
        if path in closure:
            return
        closure.add(path)
        for child in re.findall(rb"#include <(diet/[^>]+)>", original[path]):
            visit("include/" + child.decode())
    visit("include/diet/query.h")
    expression = re.compile(r'throw\s+(std::[a-z_]+)\(("(?:[^"\\]|\\.)*"|truncated|overflow)\);')
    transformed = {}
    sites = {}
    for path in sorted(closure):
        source = original[path].decode()
        matches = list(expression.finditer(source))
        output = expression.sub(r'error_detail::raise<\1>(\2);', source)
        # Rethrows intentionally stay in place. Other throw expressions must
        # be reviewed explicitly before claiming closure-wide transformation.
        if re.search(r'\bthrow\s+[^;]', output):
            raise RuntimeError("unconverted throw expression in " + path)
        if not matches:
            continue
        sites[path] = [{"exception": m[1], "message": m[2]} for m in matches]
        output = output.replace("#pragma once\n", "#pragma once\n\n#include <diet/error_detail.h>\n", 1)
        transformed[path] = output.encode()
    preamble = '''/**
 * \\file
 * \\license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \\endlicense
 */
#pragma once
#include <cstdlib>
#include <stdexcept>
namespace diet::error_detail {
  template <class E> [[noreturn]]
#if defined(__GNUC__) || defined(__clang__)
  [[gnu::cold, gnu::noinline]]
#endif
'''
    helpers = {
        "outlined": preamble + '  inline void raise(char const * message) { throw E(message); }\n}\n',
        "fail_stop": preamble + '  inline void raise(char const *) noexcept { std::abort(); }\n}\n',
    }
    source = build / "query_compare.cc"
    source.write_bytes((repo / "bench/query_compare.cc").read_bytes())
    metadata = {
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "baseline_revision": revision, "normalization": snapshot.metadata(), "query_include_closure": sorted(closure),
        "transformed_sites": sites, "source_sha256": digest(source.read_bytes()),
        "runner_sha256": digest(Path(__file__).read_bytes()),
        "queries": args.queries, "records": args.records, "larger_records": args.larger_records,
        "trials": args.trials, "sanitize": args.sanitize, "variants": {}, "execution_order": [],
    }
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    metadata["compiler"] = subprocess.check_output([*compiler, "--version"], text=True)
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    flags += (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
              if args.sanitize else ["-O3", "-DNDEBUG"])
    variants = ["baseline", "outlined", "fail_stop"]
    executables = {}
    for variant in variants:
        headers = build / variant / "headers"
        if headers.exists():
            shutil.rmtree(headers)
        contents = dict(original)
        if variant != "baseline":
            contents.update(transformed)
            contents["include/diet/error_detail.h"] = helpers[variant].encode()
        patch = []
        for path, data in contents.items():
            destination = headers / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            if data != original.get(path, b""):
                patch.extend(difflib.unified_diff(original.get(path, b"").decode().splitlines(True),
                    data.decode().splitlines(True), fromfile="a/" + path if path in original else "/dev/null",
                    tofile="b/" + path))
        (build / (variant + ".patch")).write_text("".join(patch))
        executable = build / variant / "query_compare"
        executables[variant] = executable
        command = [*compiler, *flags, "-I" + str(headers / "include"), str(source), "-o", str(executable)]
        print("Building " + variant, flush=True)
        subprocess.run(command, check=True)
        size_output = subprocess.check_output(["size", "-m", str(executable)], text=True)
        sections = {name: int(value) for name, value in re.findall(r"Section (\S+): (\d+)", size_output)}
        metadata["variants"][variant] = {
            "headers_sha256": {p: digest(d) for p, d in contents.items()}, "command": command,
            "executable_sha256": digest(executable.read_bytes()), "file_bytes": executable.stat().st_size,
            "section_bytes": {p: sections.get(p, 0) for p in
                ["__text", "__gcc_except_tab", "__unwind_info", "__eh_frame"]},
            "size_output": size_output,
        }
    cases = [(args.records, 0), (args.records, 64)]
    if args.larger_records:
        cases.append((args.larger_records, 64))
    rows = []
    expected = {}
    invariant_fields = ["head_entries", "prefix_catalogs", "visited_catalogs", "matches", "checksum"]
    for records, prefix in cases:
        for trial in range(args.trials):
            order = variants[trial % 3:] + variants[:trial % 3]
            if trial % 2:
                order = list(reversed(order))
            for variant in order:
                invocation = [str(executables[variant]), str(records), str(prefix), str(args.queries), "1"]
                output = subprocess.check_output(invocation, text=True)
                measured = list(csv.DictReader(io.StringIO(output)))
                if len(measured) != 2 or {r["profile"] for r in measured} != {"byte", "bit"}:
                    raise RuntimeError("missing profile rows")
                metadata["execution_order"].append({"variant": variant, "records": records,
                    "prefix_bytes": prefix, "trial": trial})
                for row in measured:
                    key = (row["profile"], records, prefix)
                    invariant = tuple(row[f] for f in invariant_fields) + tuple(
                        row[f] for f in row if f.endswith("_bytes") and f != "prefix_bytes")
                    if key in expected and expected[key] != invariant:
                        raise RuntimeError("cross-variant query/array mismatch: " + repr(key))
                    expected[key] = invariant
                    rows.append({"variant": variant, "trial": trial, **row})
            print(f"Completed records={records}, prefix={prefix}, trial={trial + 1}", flush=True)
    metadata["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    with (build / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)
    (build / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("variant,profile,records,prefix,query_median_ns,query_min_ns,query_max_ns")
    for key in dict.fromkeys((r["variant"], r["profile"], r["base_records"], r["prefix_bytes"]) for r in rows):
        values = [float(r["query_ns"]) for r in rows
                  if (r["variant"], r["profile"], r["base_records"], r["prefix_bytes"]) == key]
        print(",".join(key) + f",{statistics.median(values):.3f},{min(values):.3f},{max(values):.3f}")


if __name__ == "__main__":
    main()
