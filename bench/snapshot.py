#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Extracts name-normalized benchmark inputs with original and build hashes.
#
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Normalize a Git benchmark snapshot to Everett names, retaining both hashes.

Usage: snapshot.py REVISION DESTINATION [PATH ...]
Without paths, extract the complete header tree to DESTINATION/include/everett.
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
    # Invert the historical file/package rename. Deriving its vocabulary keeps
    # old checkout names out of the public runner and handles both naming eras.
    changes = git(repo, 'diff-tree', '-r', '-M', '--name-status',
                  RENAME_REVISION + '^', RENAME_REVISION, '--', 'include').decode()
    pairs = {}
    for line in changes.splitlines():
        fields = line.split('\t')
        if len(fields) != 3 or not fields[0].startswith('R'):
            continue
        before, after = Path(fields[1]), Path(fields[2])
        if before.parts[1] != after.parts[1]:
            pairs[after.parts[1]] = before.parts[1]
        if before.stem != after.stem:
            pairs[after.stem] = before.stem
    roots = {(Path(line.split('\t')[1]).parts[1], Path(line.split('\t')[2]).parts[1])
             for line in changes.splitlines()
             if line.startswith('R') and len(line.split('\t')) == 3}
    if len(roots) != 1 or pairs.get('cola') != 'world':
        raise RuntimeError('semantic rename metadata is unavailable')
    before, after = roots.pop()
    framing = git(repo, 'show', RENAME_REVISION + '^:include/' + before + '/file.h').decode()
    signature = re.search(r'case file_kind::native_blob: return \{"([A-Z]+)\.KV', framing)
    if not signature or len(signature.group(1)) != 4 or len(after) != 4:
        raise RuntimeError('historical four-byte file signature is unavailable')
    # The package rename and its later wire-signature rename were separate
    # commits. Early snapshots already use the target signature unchanged.
    return pairs, (signature.group(1), after.upper())


def spelling(value, replacement):
    if value.isupper(): return replacement.upper()
    if value[:1].isupper(): return replacement.capitalize()
    return replacement


def wire_names(text, before, after):
    # The outer signature has four brand bytes plus four kind/version bytes.
    # Expanding it to the full project name would collide at the fixed width.
    text = re.sub(re.escape(after) + r'(?=\.(?:KV|IX|RC|RB))', before, text)
    atoms = [r"(?:std::byte\s*\{\s*)?'" + c + r"'(?:\s*\})?" for c in after]
    pattern = r'\s*,\s*'.join(atoms)
    pattern += r"(?=\s*,\s*(?:std::byte\s*\{\s*)?'[.R]')"
    def split_signature(match):
        letters = iter(before)
        return re.sub(r"'[A-Z]'", lambda _: "'" + next(letters) + "'", match.group())
    text = re.sub(pattern, split_signature, text)

    # Explicitly extracted file tests also contain a canonical 96-byte golden
    # header. Relabel only a valid header and repair its independent CRC32C;
    # malformed-byte fixtures and body checksums must remain untouched.
    def crc32c(data):
        crc = 0xffffffff
        for byte in data:
            crc ^= byte
            for _ in range(8):
                crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
        return crc ^ 0xffffffff

    def golden_header(match):
        chunks = re.findall(r'"([0-9a-fA-F]+)"', match.group())
        joined = ''.join(chunks)
        if len(joined) != 192:
            return match.group()
        header = bytearray.fromhex(joined)
        if header[:8] not in (after.encode() + b'.KV\0', after.encode() + b'.IX\0'):
            return match.group()
        if header[8:12] != b'\1\0\x60\0':
            return match.group()
        checksum = int.from_bytes(header[68:72], 'little')
        header[68:72] = bytes(4)
        if crc32c(header) != checksum:
            return match.group()
        header[:4] = before.encode()
        header[68:72] = crc32c(header).to_bytes(4, 'little')
        encoded, at = header.hex(), 0
        def chunk(part):
            nonlocal at
            end = at + len(part.group(1))
            result = '"' + encoded[at:end] + '"'
            at = end
            return result
        return re.sub(r'"([0-9a-fA-F]+)"', chunk, match.group())

    return re.sub(r'(?:"[0-9a-fA-F]+"\s*)+', golden_header, text)


def normalize(repo, data, path=None):
    text = data.decode('utf-8')
    reference_world = re.search(r'\bstruct\s+(?:reference_cola|cola_record)\b', text)
    pairs, signatures = replacements(str(Path(repo).resolve()))
    text = wire_names(text, *signatures)
    if path is not None and Path(path).as_posix() == 'tests/replacement_rebuild.cc':
        # This fixture checks just the first byte of the RB envelope. Keep the
        # rewrite local to that assertion; ordinary character payloads do not
        # acquire the file signature's new spelling.
        first_byte = (r"(\bbytes\s*\[\s*0\s*\]\s*==\s*std::byte\s*\{\s*)'" +
                      re.escape(signatures[1][0]) + r"'(\s*\})")
        text = re.sub(first_byte,
                      lambda m: m.group(1) + "'" + signatures[0][0] + "'" + m.group(2), text)
    for old, new in pairs.items():
        if old == 'cola':
            continue
        text = re.sub(re.escape(old), lambda m: spelling(m.group(), new), text, flags=re.IGNORECASE)

    # COLA is still the physical algorithm. Only the logical state vocabulary
    # changes; cola_index, cola_runtime, mapped_cola and their relatives stay.
    semantic = r'(?:reference|typed|stored|replacement|saved)_cola(?:_(?:metadata|type))?'
    semantic += r'|cola_(?:record|edit|run|apply_result|batch|import_limits|type)'
    semantic += r'|u64_cola_codec'
    text = re.sub(r'\b(?:' + semantic + r')\b',
                  lambda m: m.group().replace('cola', 'world'), text)
    if reference_world:
        text = re.sub(r'\bcola_detail\b', 'world_detail', text)
    text = re.sub(r'\b(?:cola|Cola|colas|Colas)\b',
                  lambda m: spelling(m.group(), 'worlds' if m.group().endswith('s') else 'world'), text)
    # Underscores delimit the public session vocabulary in compound symbols.
    text = re.sub(r'(?<![A-Za-z0-9])taps?(?![A-Za-z0-9])',
                  lambda m: spelling(m.group(), 'sessions' if m.group().lower().endswith('s') else 'session'),
                  text, flags=re.IGNORECASE)
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
        self.paths = tuple(sorted(path for path in self._paths if path.startswith('include/everett/')))
        if not self.paths:
            raise RuntimeError('snapshot has no library headers')
        self.files = {}

    def read(self, path):
        original_path = self._paths.get(path)
        if original_path is None:
            raise FileNotFoundError(path + ' is absent from ' + self.revision)
        original = git(self.repo, 'show', self.revision + ':' + original_path)
        current = normalize(self.repo, original, path=path)
        self.files[path] = {'original_sha256': hashlib.sha256(original).hexdigest(),
                            'normalized_sha256': hashlib.sha256(current).hexdigest()}
        return current

    def metadata(self):
        return {'revision': self.revision, 'normalization_revision': RENAME_REVISION,
                'target_names': 'everett/world/multiverse/session', 'normalization_version': 2,
                'hash_scope': 'original Git bytes and name-normalized build inputs',
                'files': self.files}

    def record(self, destination):
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(self.metadata(), indent=2) + '\n')

    def write(self, destination, paths=None):
        destination = Path(destination)
        if paths is None and (destination / 'include/everett').exists():
            shutil.rmtree(destination / 'include/everett')
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
