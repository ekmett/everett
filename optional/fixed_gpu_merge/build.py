#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds the isolated fixed-key complete-merge Metal experiment.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def run(*args):
    subprocess.run([str(x) for x in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build-fixed-gpu')
    for name in ('dxc', 'spirv-val', 'spirv-cross'):
        parser.add_argument('--' + name, default=os.environ.get('EVERETT_' + name.upper().replace('-', '_'), name))
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parent.parent
    out = (root / args.build).resolve()
    out.mkdir(parents=True, exist_ok=True)
    entries = ['keep_initialize', 'cancel_due', 'decode_values', 'compact_a',
               'merge_order', 'merge_lengths', 'emit_keys', 'emit_values',
               'ef_sparse_count', 'ef_low', 'ef_high', 'ef_samples', 'ef_sparse']
    jobs = [(name, name, here / 'kernels.hlsl', []) for name in entries]
    jobs += [(name, name, here.parent / 'gpu_merge/kernels.hlsl', []) for name in ('scan_blocks', 'scan_add')]
    jobs.append(('emit_values_word', 'emit_values', here / 'kernels.hlsl', ['--define', 'FV_WORDS=1']))
    for name, entry, source, defines in jobs:
        stem = out / name
        run('python3', here.parent / 'gpu_merge/shader_compile.py', '--source', source,
            '--entry', entry, '--profile', 'cs_6_0', '--spv', f'{stem}.spv',
            '--msl', f'{stem}.metal', '--msl-entry', name, '--output-entry', name,
            '--dxc', args.dxc, '--spirv-val', args.spirv_val, '--spirv-cross', args.spirv_cross, *defines)
        run('xcrun', '-sdk', 'macosx', 'metal', '-std=metal3.2', '-fno-fast-math',
            f'-fmodules-cache-path={out / "module-cache"}', '-c', f'{stem}.metal', '-o', f'{stem}.air')
    run('xcrun', '-sdk', 'macosx', 'metallib', *(out / (name + '.air') for name, _, _, _ in jobs),
        '-o', out / 'kernels.metallib')
    run('xcrun', 'clang++', '-std=c++20', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Wpedantic',
        '-Werror', '-fobjc-arc', f'-I{root / "include"}', here / 'prototype.mm',
        '-framework', 'Foundation', '-framework', 'Metal', '-o', out / 'prototype')
    sources = list(here.glob('*.py')) + list(here.glob('*.mm')) + list(here.glob('*.hlsl'))
    sources += list((here.parent / 'gpu_merge').glob('*.hlsl'))
    sources += [here.parent / 'gpu_merge/shader_compile.py']
    sources += [root / 'include/everett' / name for name in ('elias_fano.h', 'word_view.h', 'error_detail.h')]
    hashes = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    hashes['kernels.metallib'] = hashlib.sha256((out / 'kernels.metallib').read_bytes()).hexdigest()
    hashes['prototype'] = hashlib.sha256((out / 'prototype').read_bytes()).hexdigest()
    (out / 'source-hashes.json').write_text(json.dumps(hashes, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
