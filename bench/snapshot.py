#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Normalize a Git benchmark snapshot to current Diet names, retaining both hashes.

Usage: snapshot.py REVISION DESTINATION [PATH ...]
Without paths, extract the complete header tree to DESTINATION/include/diet.
Paths use current names; explicitly listed fixtures are normalized the same way.
Names and wire identifiers are relabeled; API structure and algorithms remain
unchanged. The normalized encoding is not byte-identical to its historical
input. snapshot.json records original Git bytes and normalized build hashes
separately.
"""
import argparse
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

RENAME_REVISION = '990ba46933be83401daf1d3e0d49c183791d439c'


def git(repo, *arguments):
    return subprocess.check_output(['git', '-C', str(repo), *arguments])


@lru_cache(maxsize=None)
def replacements(repo):
    # Obtain source vocabulary from the actual semantic file/package renames.
    changes = git(repo, 'diff-tree', '-r', '-M', '--name-status',
                  RENAME_REVISION + '^', RENAME_REVISION, '--', 'include').decode()
    pairs = {}
    for line in changes.splitlines():
        fields = line.split('\t')
        if len(fields) != 3 or not fields[0].startswith('R'):
            continue
        before, after = Path(fields[1]), Path(fields[2])
        if before.parts[1] != after.parts[1]:
            pairs[before.parts[1]] = after.parts[1]
        if before.stem != after.stem:
            pairs[before.stem] = after.stem
    source_roots = {Path(line.split('\t')[1]).parts[1] for line in changes.splitlines()
                    if line.startswith('R') and len(line.split('\t')) == 3}
    if len(source_roots) == 1:
        framing = git(repo, 'show', RENAME_REVISION + '^:include/' + source_roots.pop() + '/file.h').decode()
        signature = re.search(r'case file_kind::native_blob: return \{"([A-Z]+)\.KV', framing)
        if signature: pairs[signature.group(1)] = 'DIET'
    if not pairs:
        raise RuntimeError('semantic rename metadata is unavailable')
    return sorted(pairs.items(), key=lambda item: -len(item[0]))


def normalize(repo, data):
    text = data.decode('utf-8')
    for old, new in replacements(str(Path(repo).resolve())):
        def replace(match):
            value = match.group()
            if value.isupper(): return new.upper()
            if value[:1].isupper(): return new.capitalize()
            return new
        pattern = re.escape(old) + (r'(?!wide)' if new == 'cola' else '')
        text = re.sub(pattern, replace, text, flags=re.IGNORECASE)
    return text.encode('utf-8')


class Snapshot:
    def __init__(self, repo, revision):
        self.repo = Path(repo).resolve()
        self.revision = git(self.repo, 'rev-parse', revision).decode().strip()
        original = git(self.repo, 'ls-tree', '-r', '--name-only', self.revision).decode().splitlines()
        self._paths = {}
        for path in original:
            current = normalize(self.repo, path.encode()).decode()
            if current in self._paths:
                raise RuntimeError('normalization collides at ' + current)
            self._paths[current] = path
        roots = {Path(path).parts[1] for path in original if path.startswith('include/') and len(Path(path).parts) >= 3}
        if len(roots) != 1:
            raise RuntimeError('snapshot must have one library include root')
        self.paths = tuple(sorted(path for path in self._paths if path.startswith('include/diet/')))
        if not self.paths:
            raise RuntimeError('snapshot has no library headers')
        self.files = {}

    def read(self, path):
        original_path = self._paths.get(path)
        if original_path is None:
            raise FileNotFoundError(path + ' is absent from ' + self.revision)
        original = git(self.repo, 'show', self.revision + ':' + original_path)
        current = normalize(self.repo, original)
        self.files[path] = {'original_sha256': hashlib.sha256(original).hexdigest(),
                            'normalized_sha256': hashlib.sha256(current).hexdigest()}
        return current

    def metadata(self):
        return {'revision': self.revision, 'normalization_revision': RENAME_REVISION,
                'hash_scope': 'original Git bytes and name-normalized build inputs',
                'files': self.files}

    def record(self, destination):
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(self.metadata(), indent=2) + '\n')

    def write(self, destination, paths=None):
        destination = Path(destination)
        if paths is None and (destination / 'include/diet').exists():
            shutil.rmtree(destination / 'include/diet')
        for path in self.paths if paths is None else paths:
            output = destination / path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(self.read(path))
        self.record(destination / 'snapshot.json')
        return self.metadata()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('revision')
    parser.add_argument('destination', type=Path)
    parser.add_argument('paths', nargs='*')
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    snapshot = Snapshot(args.repo, args.revision)
    snapshot.write(args.destination, args.paths or None)


if __name__ == '__main__':
    main()

# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Extracts name-normalized benchmark inputs with original and build hashes.
