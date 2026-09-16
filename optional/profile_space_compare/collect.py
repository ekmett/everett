#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Retains complete-file space observations and exact fixture identities.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import csv
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1 << 20):
            result.update(block)
    return result.hexdigest()


def logical(path):
    keys = values = zeros = 0
    with path.open('rb') as stream:
        def number():
            return struct.unpack('<Q', stream.read(8))[0]
        count = number()
        for _ in range(count):
            size = number()
            key = stream.read(size)
            assert len(key) == size
            keys += size
            zeros += key.count(b'\0')
            size = number()
            value = stream.read(size)
            assert len(value) == size
            values += size
        assert not stream.read(1)
    return dict(records=count, logical_key_bytes=keys, logical_value_bytes=values,
                key_zero_bytes=zeros, sha256=digest(path))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--sizes', type=int, nargs='+', default=[1024, 8192, 32768, 131072])
    parser.add_argument('--keep-files', action='store_true')
    args = parser.parse_args()
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    raw = output / 'raw'
    raw.mkdir(exist_ok=True)
    rows, files, fixtures = [], [], {}
    modes = ['raw-byte', 'raw-bit', 'typed-byte', 'typed-bit']
    for count in args.sizes:
        for mode in modes:
            work = output / 'work' / f'{count}-{mode}'
            work.mkdir(parents=True, exist_ok=True)
            result = subprocess.run([str(binary), str(work), mode, str(count)], capture_output=True, text=True)
            prefix = raw / f'{count}-{mode}'
            prefix.with_suffix('.csv').write_text(result.stdout)
            prefix.with_suffix('.stderr').write_text(result.stderr)
            if result.returncode:
                raise RuntimeError(f'{count}/{mode} failed: {result.stderr}')
            current = list(csv.DictReader(result.stdout.splitlines()))
            assert len(current) >= 8 * 7
            for row in current:
                assert row['mode'] == mode and int(row['logical_records']) == count
                folder = f"{row['fixture']}-{count}-{mode}"
                path = work / folder / row['file']
                assert path.stat().st_size == int(row['file_bytes'])
                files.append(dict(fixture=row['fixture'], logical_records=count, mode=mode, file=row['file'],
                                  bytes=path.stat().st_size, sha256=digest(path)))
                rows.append(row)
            for path in work.glob('*/logical.bin'):
                name = path.parent.name.removesuffix(f'-{count}-{mode}')
                identity = logical(path)
                key = f'{name}/{count}'
                assert key not in fixtures or fixtures[key] == identity, f'logical mismatch {key}/{mode}'
                fixtures[key] = identity
            if not args.keep_files:
                shutil.rmtree(work)
            print(f'verified {count} rows, {mode}', flush=True)
    with (output / 'files.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    (output / 'file-sha256.json').write_text(json.dumps(files, separators=(',', ':')) + '\n')
    (output / 'fixtures.json').write_text(json.dumps(fixtures, indent=2, sort_keys=True) + '\n')
    root = pathlib.Path(__file__).resolve().parents[2]
    revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    source_paths = list((root / 'include').rglob('*.h')) + list(pathlib.Path(__file__).resolve().parent.glob('*.cc'))
    closure = {str(p.relative_to(root)): digest(p) for p in sorted(source_paths)}
    metadata = dict(source_revision=revision, binary_sha256=digest(binary), sizes=args.sizes, modes=modes,
                    timing=False, counts=dict(files=len(files), fixture_identities=len(fixtures), rows=len(rows)),
                    header_closure=closure)
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
