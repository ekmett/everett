#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Generate isolated benchmark overlays; never change production headers."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--candidate", action="store_true")
parser.add_argument("--direct32", action="store_true")
parser.add_argument("--direct64", action="store_true")
parser.add_argument("--packed", action="store_true")
parser.add_argument("--audit", action="store_true")
args = parser.parse_args()
target = args.output.resolve()
target.mkdir(parents=True, exist_ok=True)
shutil.copytree(root / "include", target / "include", dirs_exist_ok=True)
hashes = {}


def body(name, declaration, replacement):
    path = target / "include/everett" / name
    source = path.read_text()
    if source.count(declaration) != 1:
        raise RuntimeError(f"{name}: expected one {declaration!r}")
    start = source.index(declaration) + len(declaration)
    depth, end = 1, start
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    edit(name, [(source[start:end-1], "\n" + replacement + "\n    ", 1)])


def edit(name, replacements):
    path = target / "include/everett" / name
    source = path.read_text()
    original = source
    for old, new, count in replacements:
        if source.count(old) != count:
            raise RuntimeError(f"{name}: expected {count} instances of {old!r}")
        source = source.replace(old, new)
    path.write_text(source)
    hashes[name] = {"original": hashes.get(name, {}).get("original", hashlib.sha256(original.encode()).hexdigest()),
                    "generated": hashlib.sha256(source.encode()).hexdigest()}


if args.candidate:
    edit("elias_fano.h", [
        ("samples.size() != ((entries + 255) >> 8)", "samples.size() != (((entries + 255) >> 8) * 2)", 1),
        ("auto sample = samples_[ordinal >> 8];", "auto sample = samples_[(ordinal >> 8) * 2];", 1),
        ("auto word = sample.first >> 6;\n      auto value = high_[word] & (~std::uint64_t{0} << (sample.first & 63));",
         "auto sub = samples_[(ordinal >> 8) * 2 + 1];\n"
         "      auto packed = remaining < 128 ? sub.first : sub.sparse;\n"
         "      auto start = sample.first + ((packed >> (((remaining >> 5) & 3) * 16)) & 65535);\n"
         "      remaining &= 31;\n"
         "      if (start >= high_bits_ || start - sample.first >= 4096)\n"
         "        error_detail::raise<std::invalid_argument>(\"invalid benchmark EF subentry\");\n"
         "      auto word = start >> 6;\n"
         "      auto value = high_[word] & (~std::uint64_t{0} << (start & 63));", 1),
        ("auto sample = source_.samples_[ordinal_ >> 8];", "auto sample = source_.samples_[(ordinal_ >> 8) * 2];", 1),
        ("result.samples.push_back({first, sparse});",
         "result.samples.push_back({first, sparse});\n"
         "        elias_fano_sample sub{};\n"
         "        if (sparse == std::numeric_limits<std::uint64_t>::max()) {\n"
         "          for (auto i = begin; i < end; i += 32) {\n"
         "            auto relative = (residuals[i] >> result.low_width) + i - first;\n"
         "            auto lane = (i - begin) >> 5;\n"
         "            (lane < 4 ? sub.first : sub.sparse) |= relative << ((lane & 3) * 16);\n"
         "          }\n"
         "        }\n"
         "        result.samples.push_back(sub);", 1),
    ])

if args.direct32:
    if args.candidate:
        raise RuntimeError("direct32 and sub32 are different candidates")
    edit("elias_fano.h", [
        ("high.size() != elias_fano_detail::words(high_bits_) ||\n          samples.size() != ((entries + 255) >> 8)",
         "!high.empty() || !samples.empty() || !sparse.empty() || low_width != 32", 1),
        ("return decode(ordinal, select_high(ordinal));",
         "return (low_[ordinal >> 1] >> ((ordinal & 1) * 32)) & 0xffffffffull;", 1),
    ])
    body("elias_fano.h", "std::uint64_t next() {", """      if (done()) error_detail::raise<std::out_of_range>("direct32 cursor end");
      return source_.select(ordinal_++);""")
    edit("elias_fano.h", [("    elias_fano_sample sample_{};\n    std::uint64_t ordinal_ = 0, word_ = 0, remaining_ = 0;",
                           "    std::uint64_t ordinal_ = 0;", 1)])
    body("elias_fano.h", "static elias_fano build(std::span<std::uint64_t const> residuals) {", """      if (residuals.empty()) return {};
      if (!elias_fano_detail::monotone(residuals) || residuals.back() > 0xffffffffull)
        error_detail::raise<std::invalid_argument>("direct32 benchmark extent");
      elias_fano result;
      result.entry_count = residuals.size(); result.universe = residuals.back(); result.low_width = 32;
      result.low.assign((residuals.size() + 1) >> 1, 0);
      for (std::size_t i = 0; i < residuals.size(); ++i) result.low[i >> 1] |= residuals[i] << ((i & 1) * 32);
      return result;""")

if args.direct64 or args.packed:
    if sum([args.candidate, args.direct32, args.direct64, args.packed]) != 1:
        raise RuntimeError("choose one representation")
    body("elias_fano.h", "entry_count_(entry_count), universe_(universe), low_width_(low_width) {", """      if (low_width > 64 || !high.empty() || !samples.empty() || !sparse.empty() ||
          low.size() != elias_fano_detail::words(entry_count * low_width))
        error_detail::raise<std::invalid_argument>("invalid absolute offset shape");""")
    edit("elias_fano.h", [("return decode(ordinal, select_high(ordinal));", """if (!low_width_) return 0;
      if (low_width_ == 64) return low_[ordinal];
      auto bit = ordinal * low_width_;
      auto word = bit >> 6; auto shift = unsigned(bit & 63);
      auto value = low_[word] >> shift;
      if (shift + low_width_ > 64) value |= low_[word + 1] << (64 - shift);
      return value & ((std::uint64_t{1} << low_width_) - 1);""", 1)])
    if args.direct64:
        edit("elias_fano.h", [("""if (!low_width_) return 0;
      if (low_width_ == 64) return low_[ordinal];
      auto bit = ordinal * low_width_;
      auto word = bit >> 6; auto shift = unsigned(bit & 63);
      auto value = low_[word] >> shift;
      if (shift + low_width_ > 64) value |= low_[word + 1] << (64 - shift);
      return value & ((std::uint64_t{1} << low_width_) - 1);""", "return low_[ordinal];", 1)])
    body("elias_fano.h", "std::uint64_t next() {", """      if (done()) error_detail::raise<std::out_of_range>("absolute offset cursor end");
      return source_.select(ordinal_++);""")
    edit("elias_fano.h", [("    elias_fano_sample sample_{};\n    std::uint64_t ordinal_ = 0, word_ = 0, remaining_ = 0;",
                           "    std::uint64_t ordinal_ = 0;", 1)])
    width = "64" if args.direct64 else "unsigned(std::bit_width(residuals.back()))"
    body("elias_fano.h", "static elias_fano build(std::span<std::uint64_t const> residuals) {", """      if (residuals.empty()) return {};
      if (!elias_fano_detail::monotone(residuals))
        error_detail::raise<std::invalid_argument>("absolute offsets benchmark order");
      elias_fano result;
      result.entry_count = residuals.size(); result.universe = residuals.back();
      result.low_width = """ + width + """;
      result.low.assign(elias_fano_detail::words(residuals.size() * result.low_width), 0);
      if (result.low_width == 64) std::copy(residuals.begin(), residuals.end(), result.low.begin());
      else elias_fano_detail::pack_low(residuals, result.low, result.low_width);
      return result;""")

if args.audit:
    shutil.copy(root / "optional/search_compare/telemetry.h", target / "include/everett/search_telemetry.h")
    marks = {
        "elias_fano.h": [("std::uint64_t select(std::uint64_t ordinal) const {", "ef_select")],
        "rank15.h": [("std::uint64_t rank(std::uint64_t group) const {", "rank")],
        "cola_index.h": [("cola_window project(std::uint64_t group) const {", "project")],
        "profile.h": [("auto parse_payload(std::uint64_t at, std::uint64_t retained) const {", "byte_frame"),
                      ("inline bit_comparison compare_common_bits(bit_view a, bit_view b) {", "compare")],
        "sort_profile.h": [("sort_profile_frame payload(std::uint64_t at, std::uint64_t retained, sort_profile_frame result, bool same, bool restart = false) const {", "bit_frame")],
        "typed_world.h": [("template <class P, class S> arrow_t<S> value(bit_view encoded) {", "materialize")],
    }
    for name, entries in marks.items():
        changes = [("#pragma once", '#pragma once\n#include <everett/search_telemetry.h>', 1)]
        for declaration, operation in entries:
            # project also has a one-line forwarding method; mark only the body
            # on its own line, once.
            old = declaration + "\n"
            changes.append((old, old + f"      search_audit::hit(search_audit::{operation});\n", 1))
        edit(name, changes)

all_headers = {str(p.relative_to(target / "include")): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted((target / "include").rglob("*.h"))}
(target / "headers.json").write_text(json.dumps({"candidate": args.candidate, "direct32": args.direct32,
    "direct64": args.direct64, "packed": args.packed, "audit": args.audit,
    "modified": hashes, "headers": all_headers}, indent=2) + "\n")
