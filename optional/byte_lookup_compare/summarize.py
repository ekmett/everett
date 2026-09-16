#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Compare matched process medians and complete-file sizes; retain variation."""
import argparse
from collections import defaultdict
import csv
import math
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument('input', type=Path)
args = parser.parse_args()
rows = list(csv.DictReader((args.input / 'queries.csv').open()))
space = list(csv.DictReader((args.input / 'space.csv').open()))
dimensions = ['records', 'key_bytes', 'distribution', 'query_kind', 'access']
groups, checksums, logic, sizes = defaultdict(list), defaultdict(set), defaultdict(set), defaultdict(set)
for r in rows:
    key = tuple(r[k] for k in dimensions)
    groups[(key, r['profile'], r['process'])].append(r)
    checksums[key].add((r['queries'], r['checksum']))
for r in space:
    key = tuple(r[k] for k in dimensions[:3])
    logic[key].add(tuple(r[k] for k in ['logical_hash', 'query_hash', 'logical_count', 'query_count']))
    sizes[(key, 'typed-' + r['profile'])].add(int(r['file_bytes']))
assert all(len(v) == 1 for v in checksums.values()), 'Query checksums/counts differ'
assert all(len(v) == 1 for v in logic.values()), 'Logical inputs/query sets differ'
assert all(len(v) == 1 for v in sizes.values()), 'File sizes change across processes'
medians, diagnostics = defaultdict(list), []
for (key, profile, process), trials in sorted(groups.items()):
    wall = [float(r['ns_per_query']) for r in trials]
    cpu = [float(r['cpu_ns_per_query']) for r in trials]
    ratios = [c / w for c, w in zip(cpu, wall)]
    medians[(key, profile)].append(statistics.median(wall))
    diagnostics.append(dict(**dict(zip(dimensions, key)), profile=profile, process=process,
        wall_min_ns=min(wall), wall_median_ns=statistics.median(wall), wall_max_ns=max(wall),
        wall_max_min_ratio=max(wall)/min(wall), cpu_max_min_ratio=max(cpu)/min(cpu),
        cpu_wall_min=min(ratios), cpu_wall_median=statistics.median(ratios), cpu_wall_max=max(ratios)))
summary = []
for key in sorted(checksums):
    byte = medians[(key, 'typed-byte')]; bit = medians[(key, 'typed-bit')]
    b, t = statistics.median(byte), statistics.median(bit)
    bs, = sizes[(key[:3], 'typed-byte')]; ts, = sizes[(key[:3], 'typed-bit')]
    summary.append(dict(**dict(zip(dimensions, key)), processes=len(byte), byte_ns=b, bit_ns=t,
        byte_throughput_ratio=t/b, byte_min_ns=min(byte), byte_max_ns=max(byte),
        bit_min_ns=min(bit), bit_max_ns=max(bit), byte_process_ratio=max(byte)/min(byte),
        bit_process_ratio=max(bit)/min(bit), process_ranges='byte_faster' if max(byte)<min(bit) else
            'bit_faster' if max(bit)<min(byte) else 'overlap',
        byte_file_bytes=bs, bit_file_bytes=ts, bit_space_saving_percent=100*(1-ts/bs)))
for name, data in [('summary.csv', summary), ('diagnostics.csv', diagnostics)]:
    with (args.input / name).open('w') as output:
        writer = csv.DictWriter(output, fieldnames=data[0], lineterminator='\n')
        writer.writeheader(); writer.writerows(data)
print(len(rows), 'observations;', len(summary), 'matched cases;', len(diagnostics), 'process/access groups')
print('byte throughput geometric mean:', math.exp(statistics.mean(math.log(r['byte_throughput_ratio']) for r in summary)))
print('range:', min(r['byte_throughput_ratio'] for r in summary), max(r['byte_throughput_ratio'] for r in summary))
