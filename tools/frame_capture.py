#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Arm an owned runner's gameplay capture and collect Linux per-thread CPU deltas.

Requires a reviewed local screenshot; never labels a boot/cutscene as gameplay.
The trace's WSI success boundary is not physical monitor scanout.
"""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import time


def snapshot(pid):
    def read_stat(path):
        parts=path.read_text().rsplit(')',1)
        f=parts[1].split()
        return {'name':parts[0].split('(',1)[1], 'ticks':int(f[11])+int(f[12]),
                'start_ticks':int(f[19])}
    root=Path('/proc')/str(pid)
    threads={}
    for item in (root/'task').iterdir():
        try: threads[item.name]=read_stat(item/'stat')
        except FileNotFoundError: pass
    return {'wall_ns':time.monotonic_ns(),'process':read_stat(root/'stat'),'threads':threads}


def cpu_delta(before,after,tick_hz=None):
    elapsed=(after['wall_ns']-before['wall_ns'])/1e9
    hz=tick_hz if tick_hz is not None else os.sysconf('SC_CLK_TCK')
    result={'seconds':elapsed,'tick_hz':hz,'threads':{},'incomplete_threads':[],
            'created_during_window':sorted(set(after['threads'])-set(before['threads']))}
    for tid,b in before['threads'].items():
        a=after['threads'].get(tid)
        if a is None or a['start_ticks']!=b['start_ticks']:
            result['incomplete_threads'].append(tid);continue
        seconds=(a['ticks']-b['ticks'])/hz
        result['threads'][tid]={'name':a['name'],'cpu_seconds':seconds,'percent_one_core':100*seconds/elapsed}
    result['process_cpu_seconds']=(after['process']['ticks']-before['process']['ticks'])/hz
    result['process_percent_one_core']=100*result['process_cpu_seconds']/elapsed
    return result


def quantiles(values):
    if not values:return None
    s=sorted(values)
    return {name:s[max(0,math.ceil(len(s)*p)-1)] for name,p in
            [('median',.5),('p95',.95),('p99',.99),('p999',.999)]}


def analyze(prefix):
    metadata=json.loads(Path(str(prefix)+'.json').read_text())
    rows=[]
    with Path(str(prefix)+'.events.csv').open() as f:
        for row in csv.DictReader(f):
            rows.append({k:(v if k=='kind' else int(v)) for k,v in row.items()})
    rows.sort(key=lambda r:r['start_ns'])
    kinds={}
    for r in rows:kinds.setdefault(r['kind'],[]).append(r)
    frames={}
    for k in ('frame_begin','frame_end','xfb_publish','xfb_selected','wsi_success','gpu_submit','gpu_complete'):
        for r in kinds.get(k,[]):frames.setdefault(str(r['id']),{}).setdefault(k,[]).append(r['start_ns'])
    wsi=[r['start_ns'] for r in kinds.get('wsi_success',[])]
    intervals=[(b-a)/1e6 for a,b in zip(wsi,wsi[1:])]
    seconds=(metadata['end_ns']-metadata['start_ns'])/1e9
    output={'schema':1,'seconds':seconds,'physical_scanout_measured':False,
            'complete_trace':metadata['dropped_trace_events']==0,
            'wsi_success_count':len(wsi),'wsi_success_hz':len(wsi)/seconds,
            'wsi_interval_ms':quantiles(intervals),
            'stage_counts':{k:len(v) for k,v in kinds.items()},
            'repeat_present_ids':sum(max(0,len(f.get('wsi_success',[]))-1) for f in frames.values()),
            'published_without_present_in_window':sum('xfb_publish' in f and 'wsi_success' not in f for f in frames.values()),
            'note':'Unpresented in this bounded window is not proof of a lost frame. PE parsed/delivered events are not GPU retirement. Nested duration sums are inclusive; do not add them as exclusive CPU time.',
            'frames':frames}
    Path(str(prefix)+'.analysis.json').write_text(json.dumps(output,indent=2)+'\n')
    return output


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('prefix',type=Path)
    p.add_argument('--pid',type=int)
    p.add_argument('--proof',type=Path,help='reviewed gameplay-proof.json naming a capture in its own directory')
    p.add_argument('--seconds',type=float,default=20)
    p.add_argument('--analyze',action='store_true')
    args=p.parse_args();prefix=args.prefix.resolve()
    if args.analyze:
        result=analyze(prefix);result.pop('frames');print(json.dumps(result,indent=2));return
    if not args.pid or not args.proof:p.error('--pid and --proof required when arming')
    proof=json.loads(args.proof.read_text())
    shot=(args.proof.parent/proof['capture']).resolve()
    if (proof.get('stage')!='gameplay' or proof.get('reviewed') is not True or
            shot.parent!=args.proof.resolve().parent or not shot.is_file()):
        p.error('proof must identify a reviewed gameplay screenshot in its directory')
    root=Path('/proc')/str(args.pid)
    env=dict(x.split(b'=',1) for x in (root/'environ').read_bytes().split(b'\0') if b'=' in x)
    cmd=(root/'cmdline').read_bytes().split(b'\0')
    expected=os.fsencode(str(prefix))
    if (Path(os.fsdecode(cmd[0])).name!='gekkoaot-native-run' or
            env.get(b'GEKKOAOT_PERF_CAPTURE')!=expected or b'--stop-file' not in cmd):
        p.error('PID does not match an owned, armed diagnostic runner')
    if Path(str(prefix)+'.start').exists():p.error('capture already started; choose a new prefix')
    before=snapshot(args.pid)
    Path(str(prefix)+'.start').touch()
    deadline=time.monotonic()+args.seconds+4
    while time.monotonic()<deadline and not Path(str(prefix)+'.window-end').exists():time.sleep(.02)
    after=snapshot(args.pid)
    payload={'schema':1,'pid':args.pid,'proof':proof,'before':before,'after':after,
             'cpu':cpu_delta(before,after),
             'scope':'Linux tick accounting; controller arming and flush margin included. No perf samples mixed in.'}
    Path(str(prefix)+'.cpu.json').write_text(json.dumps(payload,indent=2)+'\n')
    deadline=time.monotonic()+30
    while time.monotonic()<deadline and not Path(str(prefix)+'.json').exists():time.sleep(.1)
    if not Path(str(prefix)+'.json').exists():raise RuntimeError('capture failed to flush before deadline')
    result=analyze(prefix);result.pop('frames');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
