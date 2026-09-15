#!/usr/bin/env python3
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
"""Run under the caller's resource gate; current checkout versus fixed baseline."""
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
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
base = 'c31f339321eebd68aa9319db8c31f788572f2538'
for name in ['rank', 'rank15', 'rank_groups']:
    data = subprocess.check_output(['git', 'show', f'{base}:include/everett/{name}.h'], cwd=root, text=True)
    data = data.replace('namespace everett {', 'namespace old {\n  using everett::word_view;')
    (build / f'old_{name}.h').write_text(data)
flags = ['-std=c++20', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
exe = build / 'rank_bounds'
command = [args.cxx, *flags, '-I'+str(root/'include'), '-I'+str(build), str(root/'bench/rank_bounds.cc'), '-o', str(exe)]
subprocess.run(command, check=True)
paths = [root/'bench/rank_bounds.cc', *(root/'include/everett'/f'{name}.h' for name in ['rank', 'rank15', 'rank_groups', 'word_view'])]
metadata = {'baseline': base, 'current_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
            'working_tree_changes': subprocess.run(['git', 'diff', '--quiet'], cwd=root).returncode != 0,
            'hashes': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
            'compiler': subprocess.check_output([args.cxx, '--version'], text=True), 'flags': flags,
            'platform': platform.platform(), 'trials': args.trials, 'queries': args.queries,
            'apple_qos': 'QOS_CLASS_USER_INITIATED; no CPU affinity'}
(build/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
with (build/'results.csv').open('w') as output:
    subprocess.run([str(exe), str(args.trials), str(args.queries)], stdout=output, check=True)
print((build/'results.csv').read_text(), end='')
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Reproduces bounded-rank measurements with a pinned cached-total baseline.
