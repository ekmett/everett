#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Collects a frozen alternating comparison with exact logical identities.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import csv
import hashlib
import io
import json
import platform
from pathlib import Path
import shutil
import subprocess
import time


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def verify(root, build, manifest):
    for name, expected in manifest['sources'].items():
        if digest(root / name) != expected:
            raise RuntimeError('source changed: ' + name)
    for name, expected in manifest['artifacts'].items():
        if digest(build / name) != expected:
            raise RuntimeError('artifact changed: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build, output = args.build.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'raw').mkdir()
    (output / 'work').mkdir()
    manifest = json.loads((build / 'manifest.json').read_text())
    verify(root, build, manifest)
    revision = command('git', '-C', str(root), 'rev-parse', 'HEAD')
    if command('git', '-C', str(root), 'status', '--porcelain'):
        raise RuntimeError('commit source checkpoint before collecting')
    source_copy = output / 'source'
    for name in manifest['sources']:
        target = source_copy / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / name, target)
    for name in manifest['artifacts']:
        shutil.copyfile(build / name, output / name)
    save(output / 'manifest.json', manifest)
    # Fixed in advance: 3 fresh processes per format/case, each with one excluded
    # warmup and 3 measured iterations. Each case alternates format order across
    # rounds, necessarily giving one format the first slot twice and the other once.
    schedule = []
    for process in range(3):
        for slot in range(12):
            case = (slot + 4 * process) % 12
            order = ('fixed', 'kv') if (case + process) % 2 == 0 else ('kv', 'fixed')
            for mode in order:
                schedule.append({'process': process, 'case': case, 'mode': mode})
    metadata = {
        'source_revision': revision, 'host': {'platform': platform.platform(),
            'cpu': command('sysctl', '-n', 'machdep.cpu.brand_string'),
            'os': command('sw_vers', '-productVersion'),
            'os_build': command('sw_vers', '-buildVersion'),
            'compiler': command('xcrun', 'clang++', '--version')},
        'backend': 'Metal', 'processes': 72, 'trials_per_process': 3,
        'warmups_per_process': 1, 'schedule': schedule, 'timeout_seconds': args.timeout,
        'collector_sha256': digest(Path(__file__)), 'started_unix': time.time(),
        'scope': 'Fresh output map/import, complete merge, body/header CRC32C, clipping and cleanup; no durable sync.'}
    save(output / 'metadata.json', metadata)
    rows, checks, identities = [], [], {}
    try:
        for sequence, job in enumerate(schedule):
            case, mode, process = job['case'], job['mode'], job['process']
            tag = f'{sequence:02d}-case{case:02d}-{mode}-process{process}'
            scratch = output / 'work' / tag
            scratch.mkdir()
            start = time.time()
            try:
                result = subprocess.run([str(build / mode), str(build / (mode + '.metallib')),
                    str(scratch), str(case), '3'], capture_output=True, text=True, timeout=args.timeout)
            except subprocess.TimeoutExpired as error:
                (output / 'raw' / (tag + '.stdout')).write_bytes(error.stdout or b'')
                (output / 'raw' / (tag + '.stderr')).write_bytes(error.stderr or b'')
                raise
            stdout, stderr = output / 'raw' / (tag + '.stdout'), output / 'raw' / (tag + '.stderr')
            stdout.write_text(result.stdout); stderr.write_text(result.stderr)
            if result.returncode:
                raise RuntimeError(tag + ' failed: ' + result.stderr)
            parsed = list(csv.DictReader(io.StringIO(result.stdout)))
            if [int(row['trial']) for row in parsed] != [-1, 0, 1, 2]:
                raise RuntimeError(tag + ' trial sequence')
            for row in parsed:
                for key in row:
                    if key not in ('layout', 'distribution'):
                        row[key] = float(row[key]) if key.endswith('_ms') else int(row[key])
                if row['case'] != case or row['cancelled'] != 0 or row['output_records'] != row['older'] + row['newer']:
                    raise RuntimeError(tag + ' count mismatch')
                row.update(process=process, mode=mode, sequence=sequence)
                rows.append(row)
            logical = {name: digest(scratch / ('logical-' + name + '.bin')) for name in ('a', 'b', 'output')}
            if identities.setdefault(case, logical) != logical:
                raise RuntimeError(tag + ' logical identity mismatch')
            physical = {}
            for path in sorted(scratch.iterdir()):
                if path.name.startswith('logical-'):
                    continue
                with path.open('rb') as stream:
                    header = stream.read(256)
                physical[path.name] = {'bytes': path.stat().st_size, 'sha256': digest(path), 'first_256_bytes_hex': header.hex()}
            checks.append({**job, 'sequence': sequence, 'tag': tag, 'seconds': time.time() - start,
                'logical_sha256': logical, 'physical': physical,
                'stdout_sha256': digest(stdout), 'stderr_sha256': digest(stderr)})
            save(output / 'results.json', rows)
            save(output / 'checks.json', checks)
            print(f'{sequence + 1}/72 {tag} checked', flush=True)
        verify(root, build, manifest)
        metadata['completed_unix'] = time.time()
        metadata['all_sources_and_artifacts_unchanged'] = True
        metadata['all_logical_identities_equal'] = True
    finally:
        save(output / 'metadata.json', metadata)
    with (output / 'results.csv').open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)


if __name__ == '__main__':
    main()
