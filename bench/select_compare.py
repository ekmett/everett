#!/usr/bin/env python3
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
"""Compile a comparison with baseline62ead3f, then run it under the caller's resource gate."""
from snapshot import Snapshot

import argparse
import json
import os
import platform
from pathlib import Path
import subprocess
BASE = '62ead3fab9d0ee5bda1b47780b7905a45aae1182'
CANDIDATE = 'bc24f443cdee7d12492ac8a6f48e8587de0fc147'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir', type=Path)
p.add_argument('--prototype', action='store_true')
p.add_argument('--arch', choices=('arm64','x86_64'), default='arm64')
p.add_argument('--bmi2', action='store_true')
p.add_argument('--entries', type=int, default=4096)
p.add_argument('--trials', type=int, default=5)
p.add_argument('--queries', type=int, default=1048576)
p.add_argument('--pattern', choices=('dense','zero','sparse'), default='dense')
p.add_argument('--output', type=Path)
a=p.parse_args();root=Path(__file__).resolve().parent.parent
build=(a.build_dir or root/'build-select').resolve();build.mkdir(parents=True,exist_ok=True)
baseline_snapshot=Snapshot(root,BASE)
candidate_snapshot=Snapshot(root,CANDIDATE)
for name in ('select_groups','select15'):
    (build/('baseline_'+name+'.h')).write_bytes(baseline_snapshot.read('include/diet/'+name+'.h'))
stage1=build/'stage1'
if a.prototype:
    (stage1/'include/diet').mkdir(parents=True,exist_ok=True)
    for name in ('select_groups','select15'):
        (stage1/('include/diet/'+name+'.h')).write_bytes((build/('baseline_'+name+'.h')).read_bytes())
    subprocess.run(['patch','--silent','-p1','-d',str(stage1),'-i',str(root/'bench/select_stage1.patch')],check=True)
candidate=build/'candidate/diet'
candidate.mkdir(parents=True,exist_ok=True)
for name in ('select_groups','select15'):
    (candidate/(name+'.h')).write_bytes(candidate_snapshot.read('include/diet/'+name+'.h'))
baseline_snapshot.record(build/'baseline-snapshot.json')
candidate_snapshot.record(build/'candidate-snapshot.json')
flags=['-std=c++20','-O3','-DNDEBUG','-Wall','-Wextra','-Wpedantic','-Werror']
if a.arch=='x86_64':
    if platform.system()=='Darwin': flags+=['-arch','x86_64']
    flags+=['-mavx2','-mno-avx512f',('-mbmi2' if a.bmi2 else '-mno-bmi2')]
exe=build/('select-'+a.arch+('-bmi2' if a.bmi2 else ''))
command=[os.environ.get('CXX','clang++'),*flags,'-I'+str(candidate.parent),
 '-DSELECT_BASELINE_GROUPS="'+str(build/'baseline_select_groups.h')+'"',
 '-DSELECT_BASELINE_FIXED="'+str(build/'baseline_select15.h')+'"']
if a.prototype:
    prototype=build/'prototype_select_groups.h'
    prototype.write_bytes((stage1/'include/diet/select_groups.h').read_bytes())
    subprocess.run(['patch','--silent',str(prototype),str(root/'bench/select_simd_prototype.patch')],check=True)
    command+=['-DSELECT_PROTOTYPE="'+str(prototype)+'"']
command += [str(root/'bench/select_compare.cc'),'-o',str(exe)]
subprocess.run(command,check=True)
(build/('commands-'+a.arch+('-bmi2' if a.bmi2 else '')+'.json')).write_text(json.dumps(command,indent=2)+'\n')
run=([ 'arch','-x86_64'] if a.arch=='x86_64' and platform.system()=='Darwin' else [])+[str(exe),str(a.entries),str(a.trials),str(a.queries),a.pattern]
if a.output:
 a.output.parent.mkdir(parents=True,exist_ok=True)
 with a.output.open('w') as out:subprocess.run(run,stdout=out,check=True)
else:subprocess.run(run,check=True)

# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Reproduces pinned Elias-Fano query and construction comparisons.
