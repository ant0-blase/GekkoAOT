#!/usr/bin/env python3
"""Observe every game in original and widescreen modes, without assuming correctness."""
import argparse
import html
import json
import os
from pathlib import Path
import sys
import time
import compat_lab as lab


def pair_result(original, wide):
    """Successful mode launches are necessary evidence, not a widescreen oracle."""
    rows = [r for r in (original, wide) if r]
    return dict(original_status=original.get('status') if original else 'NOT_TESTED',
                widescreen_status=wide.get('status') if wide else 'NOT_TESTED',
                gameplay_both=len(rows)==2 and all(r.get('acceptance',{}).get('gameplay_verified') for r in rows),
                enhanced_both=len(rows)==2 and all(r.get('acceptance',{}).get('enhanced_verified') for r in rows),
                widescreen_correctness='UNVERIFIED',
                required_review=['matched gameplay scenes', 'world proportions and horizontal field of view',
                                 'HUD proportions and placement', 'edge geometry and shadow culling during camera movement',
                                 'Enhanced composition at both aspects'])


def publish(directory, rows, current, phase):
    report=dict(schema=1, phase=phase, pid=os.getpid(), current=current, updated=time.time(), games=rows,
                policy='4:3 / 16:9 observations do not certify gameplay or widescreen without scene review')
    lab.boot.atomic_json(directory/'aspect-results.json', report)
    table=''
    for row in rows:
        comparison=row.get('comparison',{})
        values=[row['name'], comparison.get('original_status','NOT_TESTED'), comparison.get('widescreen_status','NOT_TESTED'), comparison.get('widescreen_correctness','UNVERIFIED')]
        table+='<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in values)+'</tr>'
    page='<!doctype html><meta charset="utf-8"><title>GekkoAOT gameplay / widescreen campaign</title><style>body{font:16px system-ui;margin:2rem}td,th{padding:.5rem;border:1px solid #bbb}table{border-collapse:collapse}</style>'
    page+='<h1>Gameplay + Enhanced + widescreen</h1><p>'+html.escape(phase+' — '+str(current))+'</p><p>Each mode has independent captures and logs. A widescreen window or successful boot never proves correct geometry, HUD or culling.</p>'
    page+='<table><tr><th>Game<th>4:3<th>16:9<th>Widescreen review</tr>'+table+'</table>'
    (directory/'index.html').write_text(page)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus',type=Path,default=lab.CORPUS)
    parser.add_argument('--results',type=Path,default=lab.ROOT/'.gekkoaot/compat/playability-aspects')
    parser.add_argument('--seconds',type=int,default=180)
    parser.add_argument('--input-seconds',type=int,default=70)
    parser.add_argument('--jobs',type=int,default=6)
    parser.add_argument('--compile-timeout',type=int,default=3600)
    parser.add_argument('--fixtures',type=Path)
    args=parser.parse_args()
    directory=args.results.resolve()
    if directory==args.corpus.resolve() or args.corpus.resolve() in directory.parents:parser.error('results must be outside corpus')
    directory.mkdir(parents=True,exist_ok=True)
    rows=lab.discover(args.corpus)
    failed=False
    for row in rows:
        if not row['supported']:continue
        observations={}
        for aspect,slug in (('4:3','original'),('16:9','widescreen')):
            target=directory/'games'/row['id']/slug
            target.mkdir(parents=True,exist_ok=True)
            current=dict(id=row['id'],game=row['name'],aspect=aspect)
            publish(directory,rows,current,'running')
            command=[sys.executable,str(lab.ROOT/'tools/compat_lab.py'),'--corpus',str(args.corpus.resolve()),
                     '--profile','full','--entry-id',row['id'],'--gameplay','--enhanced','--aspect',aspect,
                     '--seconds',str(args.seconds),'--input-seconds',str(args.input_seconds),'--jobs',str(args.jobs),
                     '--compile-timeout',str(args.compile_timeout),'--incremental','--results',str(target)]
            if args.fixtures:command+=['--fixtures',str(args.fixtures.resolve())]
            outcome=lab.bounded(command,dict(os.environ),target/'supervisor.log',args.compile_timeout+args.seconds+180)
            report=target/'results.json'
            if report.exists():
                found=next((r for r in json.loads(report.read_text())['games'] if r['id']==row['id']),None)
                observations[slug]=found
            else:observations[slug]=dict(status='NOT_TESTED',reason='Child campaign did not publish results')
            row.setdefault('modes',{})[slug]=dict(result=str(report),exit=outcome,observation=observations[slug])
            row['comparison']=pair_result(observations.get('original'),observations.get('widescreen'))
            publish(directory,rows,current,'running')
        failed |= not (row['comparison']['gameplay_both'] and row['comparison']['enhanced_both'])
    publish(directory,rows,None,'observations-finished-review-and-fixes-required' if failed else 'observations-finished-widescreen-review-required')
    return 2


if __name__=='__main__':raise SystemExit(main())
