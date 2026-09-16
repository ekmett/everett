#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Reproduces bounded-rank measurements with a pinned cached-total baseline.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
"""Run under the caller's resource gate; current checkout versus fixed baseline."""
from snapshot import Snapshot

import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--trials', type=int, default=5)
parser.add_argument('--queries', type=int, default=1048576)
parser.add_argument('--cxx', default='clang++')
parser.add_argument('--candidate', default='working-tree', help='candidate Git revision or working-tree')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
base = 'c31f339321eebd68aa9319db8c31f788572f2538'
baseline_snapshot = Snapshot(root, base)
normalization = {'baseline': baseline_snapshot.metadata()}
if args.candidate == 'working-tree':
    include = root / 'include'
    candidate_revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
else:
    candidate_snapshot = Snapshot(root, args.candidate)
    candidate_snapshot.write(build / 'candidate')
    include = build / 'candidate/include'
    candidate_revision = candidate_snapshot.revision
    normalization['candidate'] = candidate_snapshot.metadata()
for name in ['rank', 'rank15', 'rank_groups']:
    data = baseline_snapshot.read(f'include/everett/{name}.h').decode()
    data = data.replace('namespace everett {', 'namespace old {\n  using everett::word_view;')
    (build / f'old_{name}.h').write_text(data)
flags = ['-std=c++20', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
exe = build / 'rank_bounds'
command = [args.cxx, *flags, '-I'+str(include), '-I'+str(build), str(root/'bench/rank_bounds.cc'), '-o', str(exe)]
subprocess.run(command, check=True)
paths = [root/'bench/rank_bounds.cc', *(include/'everett'/f'{name}.h' for name in ['rank', 'rank15', 'rank_groups', 'word_view'])]
metadata = {'baseline': base, 'current_revision': candidate_revision, 'selection': args.candidate, 'normalization': normalization,
            'working_tree_changes': args.candidate == 'working-tree' and subprocess.run(['git', 'diff', '--quiet'], cwd=root).returncode != 0,
            'hashes': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
            'compiler': subprocess.check_output([args.cxx, '--version'], text=True), 'flags': flags,
            'platform': platform.platform(), 'trials': args.trials, 'queries': args.queries,
            'apple_qos': 'QOS_CLASS_USER_INITIATED; no CPU affinity'}
(build/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
with (build/'results.csv').open('w') as output:
    subprocess.run([str(exe), str(args.trials), str(args.queries)], stdout=output, check=True)
print((build/'results.csv').read_text(), end='')
