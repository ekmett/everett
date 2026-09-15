#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Reproduces packed group and bitmap rank comparisons from pinned headers.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
"""Build and run under the caller's resource lease; no host paths are embedded."""
from snapshot import Snapshot

import argparse
import hashlib
import json
import pathlib
import platform
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', required=True, type=pathlib.Path)
parser.add_argument('--mode', choices=['hot', 'large', 'all', 'check'], default='hot')
parser.add_argument('--trials', type=int, default=5)
parser.add_argument('--queries', type=int, default=1048576)
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--cxx', default='c++')
parser.add_argument('--cxx-flag', action='append', default=[])
parser.add_argument('--candidate-revision', default='5817e02b2672ed92a01bf13b5f4934341bded64a')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
baseline = '62ead3fab9d0ee5bda1b47780b7905a45aae1182'
candidate = subprocess.check_output(['git', 'rev-parse', args.candidate_revision], cwd=root, text=True).strip()
include = build / 'candidate-include' / 'diet'
include.mkdir(parents=True, exist_ok=True)
header_hashes = {}
candidate_snapshot = Snapshot(root, candidate)
baseline_snapshot = Snapshot(root, baseline)
for name in ['rank', 'rank_groups', 'rank15']:
    data = candidate_snapshot.read(f'include/diet/{name}.h')
    (include / f'{name}.h').write_bytes(data)
    header_hashes[name] = hashlib.sha256(data).hexdigest()
if any('word_view.h' in (include / f'{name}.h').read_text() for name in ['rank', 'rank_groups', 'rank15']):
    data = candidate_snapshot.read('include/diet/word_view.h')
    (include / 'word_view.h').write_bytes(data)
    header_hashes['word_view'] = hashlib.sha256(data).hexdigest()
for name in ['rank', 'rank15', 'rank_groups']:
    text = baseline_snapshot.read(f'include/diet/{name}.h').decode()
    text = text.replace('namespace diet {', 'namespace baseline {')
    (build / f'baseline_{name}.h').write_text(text)
neon = (include / 'rank_groups.h').read_text()
aliases = 'namespace rank_neon {\n  using diet::rank15_view;'
if 'word_view' in neon:
    aliases += '\n  using diet::word_view;'
neon = neon.replace('namespace diet {', aliases).replace('if constexpr (K != 3)', 'if constexpr (true)')
(build / 'neon_rank_groups.h').write_text(neon)
exe = build / 'other_rank'
flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else ['-O3', '-DNDEBUG']
source = root / 'bench/other_rank.cc'
subprocess.run([args.cxx, '-std=c++20', *flags, *args.cxx_flag, '-I', str(include.parent), '-I', str(build), str(source), '-o', str(exe)], check=True)
metadata = {
    'baseline_revision': baseline,
    'normalization': {'baseline': baseline_snapshot.metadata(), 'candidate': candidate_snapshot.metadata()},
    'candidate_revision': candidate,
    'candidate_headers_sha256': header_hashes,
    'neon_candidate_sha256': hashlib.sha256(neon.encode()).hexdigest(),
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'compiler': subprocess.check_output([args.cxx, '--version'], text=True).strip(),
    'flags': ['-std=c++20', *flags, *args.cxx_flag],
    'platform': platform.platform(),
    'mode': args.mode,
    'trials': args.trials,
    'queries': args.queries,
    'sanitize': args.sanitize,
}
(build / 'other_rank_metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
subprocess.run([str(exe), args.mode, str(args.trials), str(args.queries)], check=True)
