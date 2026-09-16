#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Collect checked merge timings and freeze a small device-specific header rule.

The selector's nine features use headers and file sizes only. Fixture labels
(prefix, overlap, requested value size, split) never enter the fitted rule.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import time

FEATURES = ["records", "payload_bytes", "file_bytes", "payload_bytes_per_record",
            "record_imbalance", "byte_imbalance", "terminal_key_bytes",
            "fixed_value_bytes", "known_value_widths"]


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as source:
        for part in iter(lambda: source.read(1 << 20), b""):
            value.update(part)
    return value.hexdigest()


def cpu_model():
    if platform.system() == "Darwin":
        try:
            result = subprocess.run(["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"],
                                    capture_output=True, text=True, check=True, timeout=2)
            return result.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    if platform.system() == "Linux":
        try:
            for line in Path("/proc/cpuinfo").read_text().splitlines():
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
        except OSError:
            pass
    return platform.processor() or "unavailable"


def collect(args):
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    command = json.loads(args.command_json)
    if not isinstance(command, list) or not command or not all(isinstance(x, str) for x in command):
        raise ValueError("command-json must be a JSON array of argument strings")
    artifacts = [{"path": str(Path(p).resolve()), "sha256": digest(p)} for p in args.artifact]
    environment = {"cpu_model": cpu_model(), "os_version": platform.platform(),
                   "driver": args.driver or "OS-provided; see os_version"}
    variant = hashlib.sha256(json.dumps({"environment": environment,
        "artifacts": [{"name": Path(a["path"]).name, "sha256": a["sha256"]} for a in artifacts]},
        sort_keys=True).encode()).hexdigest()
    order = list(range(50))
    random.Random(args.seed).shuffle(order)
    metadata = {"backend": args.backend, "variant": variant, "variant_label": args.variant_label,
                "artifacts": artifacts, "command_template": command, "seed": args.seed,
                "trials": args.trials, "case_order": order, "host": platform.uname()._asdict(),
                **environment, "logical_cpus": os.cpu_count(),
                "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "warmups_per_case": 1, "timeout_seconds": args.timeout, "runs": []}
    for case in order:
        directory = output / f"case-{case:02}"
        directory.mkdir()
        argv = [part.format(directory=str(directory), case=case, trials=args.trials) for part in command]
        start = time.perf_counter()
        timed_out = False
        try:
            result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, timeout=args.timeout)
            stdout, stderr, returncode = result.stdout, result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            timed_out = True
            def text(value):
                return value.decode(errors="replace") if isinstance(value, bytes) else value or ""
            stdout, stderr, returncode = text(error.stdout), text(error.stderr), -9
        (directory / "stdout.csv").write_text(stdout)
        (directory / "stderr.txt").write_text(stderr)
        metadata["runs"].append({"case": case, "command": argv, "returncode": returncode,
                                  "timed_out": timed_out, "wall_seconds": time.perf_counter() - start})
        (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        if returncode:
            reason = "timed out" if timed_out else "failed"
            raise RuntimeError(f"case {case} {reason}; partial logs preserved in {directory}")
    if any(digest(artifact["path"]) != artifact["sha256"] for artifact in artifacts):
        raise RuntimeError("calibration artifact changed during collection")
    metadata["artifacts_unchanged"] = True
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(output / "metadata.json")


def features(row):
    na, nb = int(row["a_records"]), int(row["b_records"])
    a, b = (int(row["a_payload_bits"]) + 7) // 8, (int(row["b_payload_bits"]) + 7) // 8
    widths = [int(row["a_common_value_bits"]), int(row["b_common_value_bits"])]
    return [na + nb, a + b, int(row["a_file_bytes"]) + int(row["b_file_bytes"]),
            (a + b) / (na + nb), max(na, nb) / min(na, nb), max(a, b) / min(a, b),
            (max(int(row["a_terminal_key_bits"]), int(row["b_terminal_key_bits"])) - 1) / 8,
            max(0, *widths) / 8, sum(width >= 0 for width in widths)]


def load(directory):
    directory = Path(directory)
    metadata = json.loads((directory / "metadata.json").read_text())
    if metadata["trials"] < 3 or not metadata.get("artifacts_unchanged") or len(metadata["runs"]) != 50 or any(run["returncode"] for run in metadata["runs"]):
        raise ValueError("calibration needs all 50 checked case processes")
    groups, devices = {}, set()
    for source in sorted(directory.glob("case-*/stdout.csv")):
        lines = source.read_text().splitlines()
        devices.update(line.split(",", 1)[1] for line in lines if line.startswith("cutover_device,"))
        headers = [line for line in lines if line.startswith("kind,case,split,")]
        if len(headers) != 1:
            raise ValueError(f"missing/ambiguous case CSV header: {source}")
        records = csv.DictReader(io.StringIO("\n".join(headers + [x for x in lines if x.startswith("cutover,")])))
        for row in records:
            groups.setdefault(int(row["case"]), []).append(row)
    if len(devices) != 1 or set(groups) != set(range(50)):
        raise ValueError("one exact device and every case are required")
    cases = []
    for case, rows in sorted(groups.items()):
        if len(rows) != metadata["trials"] or {int(row["trial"]) for row in rows} != set(range(metadata["trials"])):
            raise ValueError(f"incomplete repetitions in case {case}")
        x = features(rows[0])
        for row in rows:
            if features(row) != x or any(row[k] != rows[0][k] for k in ["split", "survivors", "output_bits", "output_bytes", "output_crc32c"]):
                raise ValueError(f"case {case} fixture or result changed between trials")
        cpu, gpu = [float(row["cpu_all_ms"]) for row in rows], [float(row["gpu_all_ms"]) for row in rows]
        if not all(math.isfinite(t) and t > 0 for t in cpu + gpu):
            raise ValueError("timings must be finite and positive")
        cases.append({"case": case, "split": rows[0]["split"], "features": x, "cpu_ms": cpu, "gpu_ms": gpu,
                      "robust_gpu": max(gpu) < 0.9 * min(cpu), "median_speedup": statistics.median(cpu) / statistics.median(gpu)})
    metadata["device"] = next(iter(devices))
    return metadata, cases


def fit(cases, depth=3):
    # Depth and leaf-size are fixed before seeing held-out measurements. A GPU
    # leaf must contain at least two different training cases, all robust wins.
    def node(rows, remaining):
        wins = sum(row["robust_gpu"] for row in rows)
        if len(rows) >= 2 and wins == len(rows):
            return {"gpu": True, "training_cases": [row["case"] for row in rows]}
        leaf = {"gpu": False, "training_cases": [row["case"] for row in rows]}
        if remaining == 0 or len(rows) < 4 or wins == 0:
            return leaf
        def impurity(part):
            yes = sum(row["robust_gpu"] for row in part)
            return yes * (len(part) - yes) / len(part)
        best = None
        parent = impurity(rows)
        for feature in range(len(FEATURES)):
            values = sorted({row["features"][feature] for row in rows})
            for lower, upper in zip(values, values[1:]):
                threshold = lower + (upper - lower) / 2
                left = [row for row in rows if row["features"][feature] <= threshold]
                right = [row for row in rows if row["features"][feature] > threshold]
                if min(len(left), len(right)) < 2:
                    continue
                gain = parent - impurity(left) - impurity(right)
                candidate = (gain, -feature, -threshold)
                if gain > 1e-12 and (best is None or candidate > best[0]):
                    best = (candidate, feature, threshold, left, right)
        if best is None:
            return leaf
        _, feature, threshold, left, right = best
        return {"feature": feature, "threshold": threshold,
                "left": node(left, remaining - 1), "right": node(right, remaining - 1)}
    return node(cases, depth)


def predict(tree, x):
    while "gpu" not in tree:
        tree = tree["left"] if x[tree["feature"]] <= tree["threshold"] else tree["right"]
    return tree["gpu"]


def analyze(args):
    metadata, cases = load(args.input)
    train = [row for row in cases if row["split"] == "train"]
    heldout = [row for row in cases if row["split"] == "heldout"]
    if len(train) != 34 or len(heldout) != 16:
        raise ValueError("frozen grid requires 34 training and 16 held-out cases")
    tree = fit(train)
    minimum = [min(row["features"][i] for row in train) for i in range(len(FEATURES))]
    maximum = [max(row["features"][i] for row in train) for i in range(len(FEATURES))]
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    frozen = {"feature_names": FEATURES, "minimum": minimum, "maximum": maximum, "tree": tree,
              "training_cases": [row["case"] for row in train]}
    (output / "training-rule.json").write_text(json.dumps(frozen, indent=2) + "\n")
    selected = []
    for row in cases:
        row["in_envelope"] = all(lo <= value <= hi for value, lo, hi in zip(row["features"], minimum, maximum))
        row["selected_gpu"] = row["in_envelope"] and predict(tree, row["features"])
        if row["split"] == "heldout" and row["selected_gpu"]:
            selected.append(row)
    passed = len(selected) >= 2 and all(row["robust_gpu"] for row in selected)
    # This file is the exact frozen rule: no held-out split or threshold tuning.
    model = {"identity": {k: metadata[k] for k in ["device", "backend", "variant"]},
             "feature_names": FEATURES, "minimum": minimum, "maximum": maximum,
             "tree": tree, "heldout_passed": passed, "margin": 0.1,
             "frozen_rule_sha256": digest(output / "training-rule.json"),
             "training_cases": [row["case"] for row in train], "heldout_cases": [row["case"] for row in heldout]}
    (output / "calibration.json").write_text(json.dumps(model, indent=2) + "\n")
    (output / "cases.json").write_text(json.dumps(cases, indent=2) + "\n")
    rules = []
    def describe(node, clauses):
        if "gpu" in node:
            if node["gpu"]:
                rules.append(" and ".join(clauses) or "every in-envelope input")
            return
        feature, threshold = FEATURES[node["feature"]], node["threshold"]
        describe(node["left"], clauses + [f"{feature} <= {threshold:.12g}"])
        describe(node["right"], clauses + [f"{feature} > {threshold:.12g}"])
    describe(tree, [])
    lines = ["# Header-only GPU cutover calibration", "", f"Device: {metadata['device']}; backend: {metadata['backend']}.",
             f"Variant: `{metadata['variant']}`.",
             f"CPU: {metadata['cpu_model']}; OS: {metadata['os_version']}; driver: {metadata['driver']}.",
             "The device token is observed for this run; stability across boots is not assumed.", "", f"Held-out validation: **{'passed' if passed else 'not sufficient; selector stays on CPU'}**.", "",
             "This is an empirical heuristic for this device, backend, and implementation. It is not a performance guarantee.",
             "Input generation and one verified warm-up of each implementation are excluded. Both measured totals include mapped output construction and CRC, without durability fsync barriers.",
             "The 34 training cases fit depth at most three; a GPU leaf needs two cases whose slowest GPU repetition is at least 10% faster than their fastest CPU repetition. The 16 held-out cases do not change the rule.",
             "The observed training envelope is checked first. Unknown device/backend/variant, inputs outside it, and failed held-out validation select CPU.", "", "## Frozen GPU regions", ""]
    lines += [f"- {rule}" for rule in rules] or ["No robust GPU region found."]
    lines += ["", "## Held-out decisions", "", "| Case | In envelope | Frozen rule GPU | CPU median ms | GPU median ms | Speedup | Robust 10% win |", "|---:|:---:|:---:|---:|---:|---:|:---:|"]
    for row in heldout:
        lines.append(f"| {row['case']} | {row['in_envelope']} | {row['selected_gpu']} | {statistics.median(row['cpu_ms']):.6g} | {statistics.median(row['gpu_ms']):.6g} | {row['median_speedup']:.3f}x | {row['robust_gpu']} |")
    lines += ["", "Terminal key length is a header feature, not an assumed maximum over keys. The execution backend retains all framing, ordering, resource, and output checks.", ""]
    (output / "report.md").write_text("\n".join(lines))
    flat = []
    def flatten(node):
        at = len(flat); flat.append(None)
        if "gpu" in node:
            flat[at] = [len(FEATURES), 0, 0, 0, node["gpu"]]
        else:
            left, right = flatten(node["left"]), flatten(node["right"])
            flat[at] = [node["feature"], left, right, node["threshold"], False]
        return at
    flatten(tree)
    identity = ", ".join(json.dumps(metadata[k], ensure_ascii=False) for k in ["device", "backend", "variant"])
    code = ['// Generated empirical calibration; see calibration.json and report.md.', '#pragma once', '#include "cutover.h"', 'namespace everett_gpu {',
            f'inline constexpr std::array<cutover_node, {len(flat)}> calibrated_nodes{{{{']
    code += ['  {' + ', '.join(str(x).lower() for x in row) + '},' for row in flat]
    code += ['}};', 'inline const cutover_calibration calibrated_cutover{', f'  {{{identity}}},',
             '  {' + ', '.join(repr(x) for x in minimum) + '},', '  {' + ', '.join(repr(x) for x in maximum) + '},',
             f'  calibrated_nodes, {str(passed).lower()}', '};', '}']
    (output / "calibration.h").write_text("\n".join(code) + "\n")
    print(output / "report.md")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    collect_parser = commands.add_parser("collect")
    collect_parser.add_argument("--output", required=True)
    collect_parser.add_argument("--command-json", required=True)
    collect_parser.add_argument("--backend", required=True)
    collect_parser.add_argument("--variant-label", required=True)
    collect_parser.add_argument("--driver", default="")
    collect_parser.add_argument("--artifact", action="append", required=True)
    collect_parser.add_argument("--seed", type=int, default=20260916)
    collect_parser.add_argument("--trials", type=int, default=3)
    collect_parser.add_argument("--timeout", type=float, default=180)
    analyze_parser = commands.add_parser("analyze")
    analyze_parser.add_argument("--input", required=True)
    analyze_parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.mode == "collect":
        if args.trials < 3:
            parser.error("calibration requires at least three repetitions")
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            parser.error("timeout must be finite and positive")
        collect(args)
    else:
        analyze(args)


if __name__ == "__main__":
    main()
