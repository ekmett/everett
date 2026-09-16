#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Summarize target-only statistical samples, retaining sanitized evidence."""
import argparse
from collections import Counter
import gzip
import json
from pathlib import Path
import re
from xml.etree import ElementTree as E

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--seconds", type=float, default=8)
args = parser.parse_args()
root = E.parse(args.input).getroot()
ids = {e.attrib["id"]: e for e in root.iter() if "id" in e.attrib}


def ref(e):
    return ids[e.attrib["ref"]] if "ref" in e.attrib else e


def classify(frame):
    name = frame["name"]
    # The frozen harness puts the inlined typed codec calls on these lines.
    # These locations recover samples whose outer symbol was linker-folded.
    if frame.get("file") == "bench.cc":
        if frame.get("line") == "182":
            return "value_materialization"
        if frame.get("line") in ["178", "179"]:
            return "query_encoding"
    if "everett::elias_fano_" in name:
        return "ef_select"
    if "everett::rank15_view::" in name or re.search(r"::project\(", name):
        return "rank_projection"
    if "everett::typed_detail::value<" in name:
        return "value_materialization"
    if "everett::typed_detail::key<" in name or "everett::sort_profile_query<" in name:
        return "query_encoding"
    if "everett::compare_common_bits(" in name or "everett::key_detail::common_" in name or (
            "everett::profile_query_context<" in name and re.search(r"::(advance|advance_parts)\(", name)):
        return "key_comparison"
    if "everett::profile_detail::read_" in name or "everett::profile_detail::load_bits(" in name or (
            ("everett::profile_view<" in name or "everett::sort_profile_view<" in name) and
            re.search(r"::(payload|parse_payload|parse_absolute|parse_relative|read_absolute|locate|start_block|encoded_at|next)[<(]", name)):
        return "frame_decode"
    return None


samples = []
for row in root.iter("row"):
    element = row.find("tagged-backtrace")
    if element is None:
        continue
    tagged = ref(element)
    if tagged.find("backtrace") is None:
        continue
    frames = [ref(x) for x in ref(tagged.find("backtrace"))]
    names = [x.attrib.get("name", "") for x in frames]
    # Linker-folded template frames can be named <deduplicated_symbol>, so
    # filtering by a particular function would systematically lose samples.
    # The time-profile table is target-only. Restrict to its main thread and
    # the last steady-state time window below; retain harness overhead in other.
    if "Main Thread" not in ref(row.find("thread")).attrib.get("fmt", ""):
        continue
    # A frame records only public relative source names; never retain process
    # identifiers, host paths, binaries or unrelated trace tables.
    evidence = []
    for frame, name in zip(frames, names):
        source = frame.find("source")
        entry = {"name": name}
        if source is not None and source.find("path") is not None:
            entry.update(file=Path(ref(source.find("path")).text).name, line=source.attrib.get("line"))
        evidence.append(entry)
    samples.append({"time_ns": int(ref(row.find("sample-time")).text),
                    "weight_ns": int(ref(row.find("weight")).text), "frames": evidence})
if not samples:
    raise RuntimeError("No attributable query samples")
# The benchmark repeats queries for a known duration immediately before exit.
# Select a strict suffix shorter than that duration, excluding setup/oracles.
last = max(x["time_ns"] for x in samples if any(
    f.get("file") == "bench.cc" and f.get("line") == "200" for f in x["frames"]))
samples = [x for x in samples if last - args.seconds * 1e9 <= x["time_ns"] <= last]
exclusive, inclusive, leaves = Counter(), Counter(), Counter()
for sample in samples:
    categories = [classify(f) for f in sample["frames"]]
    scopes = [x for x in categories if x]
    exclusive[scopes[0] if scopes else "routing_and_other"] += sample["weight_ns"]
    for scope in set(scopes):
        inclusive[scope] += sample["weight_ns"]
    leaves[sample["frames"][0]["name"]] += sample["weight_ns"]
total = sum(x["weight_ns"] for x in samples)
summary = {"samples": len(samples), "sample_weight_ns": total, "window_seconds": args.seconds,
           "exclusive_percent": {k: v * 100 / total for k, v in exclusive.items()},
           "inclusive_percent": {k: v * 100 / total for k, v in inclusive.items()},
           "exclusive_ns": dict(exclusive), "inclusive_ns": dict(inclusive),
           "leaf_weights": dict(leaves.most_common())}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.with_suffix(".json").write_text(json.dumps(summary, indent=2) + "\n")
with gzip.open(args.output.with_suffix(".samples.json.gz"), "wt") as out:
    json.dump(samples, out, separators=(",", ":"))
print(json.dumps({k: v for k, v in summary.items() if k != "leaf_weights"}, indent=2))
