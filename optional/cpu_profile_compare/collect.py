#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Freezes and collects paired CPU profile measurements with exact logical identities.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import platform
import shutil
import shlex
import subprocess
import sys
import time

MODES = ('raw-bit', 'raw-byte', 'typed-bit', 'typed-byte')


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from experiment import build_metadata


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build', type=Path, required=True)
    parser.add_argument('--candidate-build', type=Path, required=True)
    parser.add_argument('--baseline-revision', default='5868b32')
    parser.add_argument('--candidate-revision', default='a760695')
    parser.add_argument('--candidate-headers', type=Path, required=True)
    parser.add_argument('--reference-checks', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'raw').mkdir(); (output / 'work').mkdir()
    if command('git', '-C', str(root), 'status', '--porcelain'):
        raise RuntimeError('commit harness source checkpoint before collection')
    revisions = {name: command('git', '-C', str(root), 'rev-parse', getattr(args, name + '_revision'))
        for name in ('baseline', 'candidate')}
    header_roots = {'baseline': root / 'include', 'candidate': args.candidate_headers.resolve()}
    binaries = {name: getattr(args, name + '_build').resolve() / 'cpu-profile' for name in revisions}
    files = list((root / 'optional/cpu_profile_compare').glob('*.cc'))
    files += list((root / 'optional/cpu_profile_compare').glob('*.py'))
    files += [root / name for name in ('optional/cpu_profile_compare/CMakeLists.txt',
        'optional/fixed_kv_compare/cases.h', 'optional/fixed_gpu_merge/fixtures.h')]
    sources = {str(p.relative_to(root)): digest(p) for p in sorted(files)}
    manifest = {'harness_revision': command('git', '-C', str(root), 'rev-parse', 'HEAD'),
        'harness_sources': sources, 'builds': {}}
    for label, revision in revisions.items():
        names = command('git', '-C', str(root), 'ls-tree', '-r', '--name-only', revision, 'include/everett').splitlines()
        hashes = {}
        for name in names:
            path = header_roots[label] / Path(name).relative_to('include')
            encoded = subprocess.check_output(['git', '-C', str(root), 'show', revision + ':' + name])
            expected = hashlib.sha256(encoded).hexdigest()
            if digest(path) != expected:
                raise RuntimeError(label + ' headers differ from recorded revision: ' + name)
            hashes[name] = expected
            target = output / 'source' / label / name
            target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(encoded)
        if binaries[label].stat().st_mtime < (root / 'optional/cpu_profile_compare/prototype.cc').stat().st_mtime:
            raise RuntimeError(label + ' binary predates final host source')
        commands = json.loads((getattr(args, label + '_build') / 'compile_commands.json').read_text())
        entry, = [entry for entry in commands if Path(entry['file']).resolve() == (root / 'optional/cpu_profile_compare/prototype.cc').resolve()]
        tokens = entry.get('arguments') or shlex.split(entry['command'])
        includes = [Path(token[2:]).resolve() for token in tokens if token.startswith('-I')]
        if header_roots[label].resolve() not in includes:
            raise RuntimeError(label + ' build used a different include root')
        manifest['builds'][label] = {'header_revision': revision, 'headers': hashes,
            'binary_sha256': digest(binaries[label]), 'build': build_metadata(getattr(args, label + '_build'))}
        shutil.copyfile(binaries[label], output / ('cpu-profile-' + label))
    for name in sources:
        target = output / 'source/harness' / name
        target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(root / name, target)
    save(output / 'manifest.json', manifest)
    reference = {}
    for check in json.loads(args.reference_checks.read_text()):
        key = check['case']
        if reference.setdefault(key, check['logical_sha256']) != check['logical_sha256']:
            raise RuntimeError('reference logical identities disagree')
    schedule = []
    for process in range(3):
        for slot in range(12):
            case = (slot + process * 4) % 12
            rotation = (case + process) % len(MODES)
            for mode in MODES[rotation:] + MODES[:rotation]:
                order = ('baseline', 'candidate') if (case + MODES.index(mode) + process) % 2 == 0 else ('candidate', 'baseline')
                for revision in order:
                    schedule.append(dict(process=process, case=case, mode=mode, revision=revision))
    metadata = {'harness_revision': manifest['harness_revision'], 'header_revisions': revisions,
        'schedule': schedule, 'processes': len(schedule), 'trials_per_process': 3, 'warmups_per_process': 1,
        'timeout_seconds': args.timeout, 'started_unix': time.time(),
        'host': {'cpu': command('sysctl', '-n', 'machdep.cpu.brand_string'), 'os': command('sw_vers', '-productVersion'),
            'os_build': command('sw_vers', '-buildVersion'), 'platform': platform.platform()},
        'reference_checks_sha256': digest(args.reference_checks),
        'scope': 'Mapped inputs; native merge, output EF, fresh output mmap and section copy, body/header CRC32C, clipping, cleanup; no durable sync.'}
    save(output / 'metadata.json', metadata)
    rows, checks, identities, physical_identities = [], [], {}, {}
    try:
        for sequence, job in enumerate(schedule):
            case, mode, revision = job['case'], job['mode'], job['revision']
            tag = f'{sequence:03d}-case{case:02d}-{mode}-{revision}-process{job["process"]}'
            scratch = output / 'work' / tag; scratch.mkdir()
            started = time.time()
            try:
                run = subprocess.run([str(binaries[revision]), mode, str(scratch), str(case), '3'],
                    capture_output=True, text=True, timeout=args.timeout)
            except subprocess.TimeoutExpired as error:
                (output / 'raw' / (tag + '.stdout')).write_bytes(error.stdout or b'')
                (output / 'raw' / (tag + '.stderr')).write_bytes(error.stderr or b'')
                raise
            stdout, stderr = output / 'raw' / (tag + '.stdout'), output / 'raw' / (tag + '.stderr')
            stdout.write_text(run.stdout); stderr.write_text(run.stderr)
            if run.returncode:
                raise RuntimeError(tag + ' failed: ' + run.stderr)
            data = list(csv.DictReader(io.StringIO(run.stdout)))
            if [int(row['trial']) for row in data] != [-1, 0, 1, 2]:
                raise RuntimeError(tag + ' trial sequence')
            for row in data:
                for key in row:
                    if key not in ('mode', 'distribution'):
                        row[key] = float(row[key]) if key.endswith('_ms') else int(row[key])
                if row['case'] != case or row['mode'] != mode or row['cancelled'] or row['output_records'] != row['older'] + row['newer']:
                    raise RuntimeError(tag + ' count or mode mismatch')
                row.update(sequence=sequence, process=job['process'], revision=revision)
                rows.append(row)
            logical = {name: digest(scratch / ('logical-' + name + '.bin')) for name in ('a', 'b', 'output')}
            if identities.setdefault(case, logical) != logical or reference[case] != logical:
                raise RuntimeError(tag + ' differs from matched logical fixture')
            physical = {}
            for path in sorted(scratch.glob('*.kv')):
                with path.open('rb') as stream:
                    header = stream.read(256)
                record = {'bytes': path.stat().st_size, 'sha256': digest(path), 'first_256_bytes_hex': header.hex()}
                # Typed-byte transport deliberately changes its wire grammar.
                group = (case, mode, revision if mode == 'typed-byte' else '', path.name)
                if physical_identities.setdefault(group, (record['bytes'], record['sha256'])) != (record['bytes'], record['sha256']):
                    raise RuntimeError(tag + ' unexpected wire drift: ' + path.name)
                physical[path.name] = record
            checks.append({**job, 'sequence': sequence, 'tag': tag, 'seconds': time.time() - started,
                'logical_sha256': logical, 'physical': physical,
                'stdout_sha256': digest(stdout), 'stderr_sha256': digest(stderr)})
            save(output / 'results.json', rows); save(output / 'checks.json', checks)
            # All of these files were made solely for this completed process.
            # Keep exact hashes/header bytes, not hundreds of full scratch runs.
            for path in scratch.iterdir(): path.unlink()
            scratch.rmdir()
            print(f'{sequence + 1}/{len(schedule)} {tag} checked', flush=True)
        for name, expected in sources.items():
            if digest(root / name) != expected: raise RuntimeError('harness changed: ' + name)
        for label, build in manifest['builds'].items():
            if digest(binaries[label]) != build['binary_sha256']: raise RuntimeError('binary changed: ' + label)
            for name, expected in build['headers'].items():
                if digest(header_roots[label] / Path(name).relative_to('include')) != expected:
                    raise RuntimeError('headers changed: ' + label + ' ' + name)
        metadata['completed_unix'] = time.time()
        metadata['all_sources_and_artifacts_unchanged'] = True
        metadata['all_logical_identities_match_metal_fixtures'] = True
        metadata['raw_and_typed_bit_wire_identical_between_revisions'] = True
    finally:
        save(output / 'metadata.json', metadata)
    with (output / 'results.csv').open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)


if __name__ == '__main__':
    main()
