#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds matched-format wrappers without changing either merge implementation.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kv-metallib', type=Path, required=True)
    parser.add_argument('--fixed-metallib', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parent.parent
    out = args.build.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for mode in ('kv', 'fixed'):
        source_library = getattr(args, mode + '_metallib').resolve()
        shutil.copyfile(source_library, out / (mode + '.metallib'))
        subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O3', '-DNDEBUG', '-Wall', '-Wextra',
            '-Wpedantic', '-Werror', '-fobjc-arc', '-I' + str(root / 'include'), str(here / (mode + '.mm')),
            '-framework', 'Foundation', '-framework', 'Metal', '-o', str(out / mode)], check=True)
    sources = set((root / 'include/everett').rglob('*.h')) | set((root / 'include/everett').rglob('*.inc'))
    for folder in ('gpu_merge', 'fixed_gpu_merge', 'fixed_kv_compare'):
        for suffix in ('*.h', '*.hlsl', '*.mm', '*.py', 'CMakeLists.txt'):
            sources.update((root / 'optional' / folder).glob(suffix))
    record = {'sources': {str(p.relative_to(root)): digest(p) for p in sorted(sources)},
        'artifacts': {name: digest(out / name) for name in ('kv', 'fixed', 'kv.metallib', 'fixed.metallib')}}
    (out / 'manifest.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
