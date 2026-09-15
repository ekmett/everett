#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Run under cpu-heavy; snapshot exact Diet revisions and preserve full wire oracles."""
from snapshot import Snapshot
from fixture import write_fixture

import argparse, concurrent.futures, csv, datetime, hashlib, io, json, os
from pathlib import Path
import platform, shlex, statistics, subprocess, shutil

def sha(data): return hashlib.sha256(data).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',required=True)
    candidate=p.add_mutually_exclusive_group(required=True)
    candidate.add_argument('--candidate');candidate.add_argument('--candidate-patch',type=Path)
    p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--trials',type=int,default=5)
    p.add_argument('--rounds',type=int,default=3);p.add_argument('--compiler',default=os.environ.get('CXX','clang++'))
    a=p.parse_args();repo=Path(__file__).resolve().parent.parent;b=a.build_dir.resolve();b.mkdir(parents=True,exist_ok=True)
    git=lambda *args:subprocess.check_output(['git','-C',str(repo),*args])
    source=b/'cola_payload.cc';write_fixture(repo/'bench/cola_payload.cc', source)
    compiler=shlex.split(a.compiler)
    meta={'started':datetime.datetime.now(datetime.timezone.utc).isoformat(),'platform':platform.platform(),'compiler':subprocess.check_output([*compiler,'--version'],text=True),'source_sha256':sha(source.read_bytes()),'runner_sha256':sha(Path(__file__).read_bytes()),'trials':a.trials,'rounds':a.rounds,'variants':{},'execution_order':[],'wires':{}}
    commands=[]
    for name,ref in [('baseline',a.baseline),('candidate',a.candidate or a.baseline)]:
        rev=git('rev-parse',ref).decode().strip();dest=b/name;dest.mkdir(exist_ok=True)
        if (dest/'include').exists(): shutil.rmtree(dest/'include')
        snapshot=Snapshot(repo,rev);snapshot.write(dest)
        if name=='candidate' and a.candidate_patch:
            patch=a.candidate_patch.resolve()
            subprocess.run(['git','apply','--unsafe-paths','--directory='+str(dest),str(patch)],cwd=repo,check=True)
            meta['candidate_patch_sha256']=sha(patch.read_bytes())
        hashes={str(f.relative_to(dest)):sha(f.read_bytes()) for f in sorted((dest/'include').rglob('*')) if f.is_file()}
        if name=='candidate' and a.candidate_patch:
            recorded=json.loads((repo/'bench/results/cola_payload/results.json').read_text())
            expected=recorded['variants']['candidate']['headers_sha256']
            # Unchanged headers may acquire current names/signatures. Changed
            # headers still require the recorded original hash: this patch edits
            # an already normalized source, not its package or wire labels.
            expected={path:(snapshot.files[path]['normalized_sha256']
                      if path in snapshot.files and value==snapshot.files[path]['original_sha256']
                      else value) for path,value in expected.items()}
            if hashes!=expected:
                raise RuntimeError('reconstructed candidate headers differ from measured source or normalization')
            rev='patch applied to '+rev
        meta['variants'][name]={'revision':rev,'headers_sha256':hashes,'normalization':snapshot.metadata(),'commands':[]}
        for suffix,options in [('',[]),('-alloc',['-DDIET_BENCH_ALLOCATIONS'])]:
            command=[*compiler,'-std=c++20','-O3','-DNDEBUG','-Wall','-Wextra','-Wpedantic','-Werror','-I'+str(dest/'include'),str(source),'-o',str(dest/('run'+suffix)),*options]
            if name=='candidate':command+=['-DDIET_REUSE']
            meta['variants'][name]['commands'].append(command);commands.append(command)
    def compile(command):subprocess.run(command,check=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(compile,commands))
    sanitized=[*compiler,'-std=c++20','-O1','-g','-Wall','-Wextra','-Wpedantic','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DDIET_REUSE','-I'+str(b/'candidate/include'),str(source),'-o',str(b/'sanitized')]
    subprocess.run(sanitized,check=True);meta['sanitizer_command']=sanitized
    with (b/'sanitizer.csv').open('w') as out:subprocess.run([str(b/'sanitized'),'1',str(b/'sanitizer-wire')],check=True,stdout=out,env=dict(os.environ,ASAN_OPTIONS='halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
    fields='case,operation,mode,round,items,ns,calls,requested,peak,live,checksum'.split(',');rows=[];allocs=[];expected=None
    def run(name,trial,allocation=False):
        nonlocal expected
        suffix='-alloc' if allocation else '';dump=b/f'{name}-{trial}{suffix}'
        executable_name='candidate' if name=='candidate_reuse' else name
        command=[str(b/executable_name/('run'+suffix)),str(1 if allocation else a.rounds),str(dump),'0' if allocation or name=='candidate_reuse' else '1']
        output=subprocess.check_output(command,text=True)
        wires={f.name:f.read_bytes() for f in sorted(dump.glob('*.index'))}
        if len(wires)!=8 or (expected is not None and wires!=expected):raise RuntimeError('baseline/candidate full wire mismatch')
        expected=wires;meta['wires'][dump.name]={k:sha(v) for k,v in wires.items()}
        result=[{'variant':name,'trial':trial,**row} for row in csv.DictReader(io.StringIO(output),fieldnames=fields)]
        (allocs if allocation else rows).extend(result)
    print('Sanitizer and compilation passed; timing starts.',flush=True)
    for trial in range(a.trials):
        order=['baseline','candidate'] if not trial&1 else ['candidate','baseline'];meta['execution_order'].append(order)
        for name in order:run(name,trial);print('Completed',trial,name,flush=True)
    print('Matched default-only comparisons finished; separate rotating-mode reuse timing.',flush=True)
    for trial in range(a.trials):run('candidate_reuse',trial);print('Reuse trial',trial,flush=True)
    for name in ['baseline','candidate']:run(name,0,True)
    for name,data in [('results.csv',rows),('allocations.csv',allocs)]:
        with (b/name).open('w',newline='') as f:
            w=csv.DictWriter(f,fieldnames=['variant','trial',*fields]);w.writeheader();w.writerows(data)
        meta[name+'_sha256']=sha((b/name).read_bytes())
    meta['summary']=[]
    for case,operation in sorted({(r['case'],r['operation']) for r in rows}):
        item={'case':case,'operation':operation}
        for variant,mode in [('baseline','0'),('candidate','0'),('candidate_reuse','0'),('candidate_reuse','1'),('candidate_reuse','2'),('candidate_reuse','3')]:
            ns=[float(r['ns'])/int(r['items']) for r in rows if r['case']==case and r['operation']==operation and r['variant']==variant and r['mode']==mode]
            if ns:item[variant+mode]={'min':min(ns),'median':statistics.median(ns),'max':max(ns)}
        meta['summary'].append(item)
    for name in ['baseline','candidate']:
        meta['variants'][name]['binary_sha256']={x:sha((b/name/x).read_bytes()) for x in ['run','run-alloc']}
    meta['completed']=datetime.datetime.now(datetime.timezone.utc).isoformat();(b/'results.json').write_text(json.dumps(meta,indent=2)+'\n')
    print(json.dumps(meta['summary'],indent=2),flush=True)
if __name__=='__main__':main()
