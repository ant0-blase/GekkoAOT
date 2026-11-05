#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare generic/native generated modules on the same real native GX service.

Run test_native_sdk_lowering.py first. This CPU fixture does not measure game
frame time, input latency or GPU rendering performance.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--samples',type=int,default=9);ap.add_argument('--calls',type=int,default=300000);ap.add_argument('--transport',choices=['gpu','recording'],default='gpu');ap.add_argument('-o','--output',type=Path,required=True);args=ap.parse_args()
    if args.samples<3 or args.calls<1:ap.error('at least 3 samples and 1 call required')
    fixture=ROOT/'.gekkoaot/compat/native-sdk5/compiler-fixture'
    result=dict(scope='CPU-only generated module + real GX service; not gameplay frame timing',transport=args.transport,samples={'guest':[],'generic':[],'direct':[]})
    for sample in range(args.samples):
        for route in (('guest','generic','direct') if sample%2==0 else ('direct','generic','guest')):
            process=subprocess.run([str(ROOT/'build/bin/gekkoaot-bench-native-sdk'),str(ROOT/'build/lib/libgekkoaot-test-gx.so'),str(fixture/route/'build/gSDK500_recomp.so'),str(args.calls),args.transport],text=True,capture_output=True,check=True)
            record=json.loads(process.stdout.strip().splitlines()[-1])
            expected=args.calls+4096 if route=='direct' else 0
            if record['named_calls_including_warmup']!=expected:raise RuntimeError('benchmark did not execute the expected native service route')
            result['samples'][route].append(record)
    result['median_ns_per_call']={r:statistics.median(x['wall_ns_per_call'] for x in rows) for r,rows in result['samples'].items()}
    before,after=(result['median_ns_per_call'][r] for r in ('generic','direct'));result['dispatch_change_pct']=(after-before)/before*100
    guest=result['median_ns_per_call']['guest'];result['change_vs_original_sdk_pct']=(after-guest)/guest*100
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='samples'},indent=2))
if __name__=='__main__':main()
