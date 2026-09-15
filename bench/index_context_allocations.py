#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Count allocations against the two frozen header trees from sample_frontier.py."""
from fixture import write_fixture

import argparse
import csv
import hashlib
import io
import json
import os
import platform
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark-build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build = args.benchmark_build.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = output / 'index_context_allocations.cc'
    write_fixture(Path(__file__).with_suffix('.cc'), source)
    frozen = json.loads((build / 'metadata.json').read_text())
    compiler = shlex.split(os.environ.get('CXX', 'clang++'))
    metadata = {'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'compiler': subprocess.check_output(compiler + ['--version'], text=True),
                'platform': platform.platform(),
                'variants': {}}
    all_rows = []
    for variant in ('baseline', 'candidate'):
        include = build / variant / 'headers' / 'include'
        for relative, expected in frozen['variants'][variant]['headers_sha256'].items():
            path = build / variant / 'headers' / relative
            if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                raise RuntimeError('frozen header changed: ' + relative)
        binary = output / variant
        command = compiler + ['-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                              '-O3', '-DNDEBUG', '-I' + str(include), str(source), '-o', str(binary)]
        subprocess.run(command, check=True)
        raw = subprocess.check_output([str(binary)], text=True)
        (output / (variant + '.csv')).write_text(raw)
        rows = list(csv.DictReader(io.StringIO(raw)))
        if len(rows) != 8:
            raise RuntimeError('allocation probe row count')
        for row in rows:
            row['variant'] = variant
        all_rows.extend(rows)
        metadata['variants'][variant] = dict(frozen['variants'][variant], command=command)
    baseline = {tuple(r[k] for k in ('profile', 'prefix_bytes', 'input')): r for r in all_rows if r['variant'] == 'baseline'}
    for row in all_rows:
        if row['variant'] == 'candidate':
            old = baseline[tuple(row[k] for k in ('profile', 'prefix_bytes', 'input'))]
            if any(old[k] != row[k] for k in ('records', 'checksum')):
                raise RuntimeError('allocation probe exact output mismatch')
    with (output / 'results.csv').open('w') as handle:
        writer = csv.DictWriter(handle, fieldnames=list(all_rows[0]))
        writer.writeheader()
        writer.writerows(all_rows)
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')


if __name__ == '__main__':
    main()
