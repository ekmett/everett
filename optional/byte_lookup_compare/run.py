#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Collect adjacent byte/bit processes on identical logical lookup fixtures."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import random
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('build', type=Path)
parser.add_argument('headers', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--queries', type=int, default=4096)
parser.add_argument('--trials', type=int, default=3)
parser.add_argument('--loops', type=int, default=2)
parser.add_argument('--processes', type=int, default=3)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parent
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
identity = dict(binary_sha256=sha(args.build / 'bench'),
    sources={n: sha(root / n) for n in ['bench.cc', 'check.cc', 'run.py', 'CMakeLists.txt']},
    header_manifest_sha256=sha(args.headers / 'headers.json'),
    arguments={k: v for k, v in vars(args).items() if k not in ['build', 'headers', 'output']})
metadata_path = args.output / 'metadata.json'
if metadata_path.exists():
    metadata = json.loads(metadata_path.read_text())
    if metadata['identity'] != identity:
        raise RuntimeError('Output belongs to different source/executable/settings; use a new directory')
else:
    metadata = dict(identity=identity, execution_order=[], host=platform.platform(),
        compiler=subprocess.check_output(['c++', '--version'], text=True).splitlines()[0],
        header_revision=json.loads((args.headers / 'headers.json').read_text())['source_revision'],
        source_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip())
metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
fixtures = [(n, width, shape) for n in [8192, 131072] for width in [16, 128]
            for shape in ['structured', 'hash']] + [(524288, 128, 'hash')]
for process in range(args.processes):
    jobs = fixtures[:]
    random.Random(917 + process).shuffle(jobs)
    for case, (n, width, shape) in enumerate(jobs):
        profiles = ['byte', 'bit'] if (case + process) % 2 == 0 else ['bit', 'byte']
        for profile in profiles:
            name = f'{process}-{profile}-{n}-{width}-{shape}'
            path = args.output / (name + '.csv')
            if path.exists():
                continue
            command = [str((args.build / 'bench').resolve()), profile, str(n), str(width), shape,
                       str(args.queries), str(args.trials), str(args.loops)]
            started = time.time()
            result = subprocess.run(command, text=True, capture_output=True, check=True)
            path.write_text(result.stdout)
            path.with_suffix('.stderr').write_text(result.stderr)
            metadata['execution_order'].append(dict(name=name, started=started, elapsed=time.time() - started))
            metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
            print(name, flush=True)
rows, spaces, schedules = [], [], set()
for path in sorted(args.output.glob('[0-9]-*.csv')):
    process, profile, n, width, shape = path.stem.split('-')
    rows.extend(dict(process=process, **r) for r in csv.DictReader(path.read_text().splitlines()))
    stderr = path.with_suffix('.stderr').read_text().splitlines()
    s, = [x.split(',')[1:] for x in stderr if x.startswith('SPACE,')]
    f, = [x.split(',')[1:] for x in stderr if x.startswith('FIXTURE,')]
    schedules.update(x for x in stderr if x.startswith('SCHEDULING,'))
    fields = ['file_bytes', 'payload_bytes', 'ef_payload_bytes', 'ef_auxiliary_bytes', 'rank_bytes',
              'cut_bytes', 'flag_bytes', 'native_build_ns', 'index_build_ns', 'payload_hash',
              'logical_hash', 'query_hash', 'logical_count', 'query_count']
    spaces.append(dict(process=process, profile=profile, records=n, key_bytes=width,
                       distribution=shape, **dict(zip(fields, s + f))))
for name, data in [('queries.csv', rows), ('space.csv', spaces)]:
    with (args.output / name).open('w') as output:
        writer = csv.DictWriter(output, fieldnames=data[0], lineterminator='\n')
        writer.writeheader(); writer.writerows(data)
metadata['scheduling'] = sorted(schedules)
metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
