#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

path=Path(__file__).resolve().parents[2]/'tools/frame_capture.py'
spec=importlib.util.spec_from_file_location('frame_capture',path)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class CaptureAnalysis(unittest.TestCase):
    def test_frame_domains_and_partial_window(self):
        with tempfile.TemporaryDirectory() as temp:
            prefix=Path(temp)/'capture'
            Path(str(prefix)+'.json').write_text(json.dumps({'start_ns':0,'end_ns':1000000000,'dropped_trace_events':0}))
            rows=[('xfb_publish',1,10),('wsi_success',1,20),('wsi_success',1,30),
                  ('xfb_publish',2,40),('pe_command_parsed',0x45,50),('vi_field_observed',1,60)]
            with Path(str(prefix)+'.events.csv').open('w') as f:
                writer=csv.writer(f);writer.writerow(['kind','start_ns','end_ns','id','value','tid'])
                for kind,identity,t in rows:writer.writerow([kind,t,t,identity,0,1])
            result=module.analyze(prefix)
            self.assertEqual(result['wsi_success_count'],2)
            self.assertEqual(result['repeat_present_ids'],1)
            self.assertEqual(result['published_without_present_in_window'],1)
            self.assertFalse(result['physical_scanout_measured'])
            self.assertEqual(result['wsi_interval_ms']['median'],.00001)
            self.assertNotIn('gpu_complete',result['stage_counts'])
            self.assertTrue(result['complete_trace'])
            Path(str(prefix)+'.json').write_text(json.dumps({'start_ns':0,'end_ns':1000000000,'dropped_trace_events':1}))
            self.assertFalse(module.analyze(prefix)['complete_trace'])

    def test_cpu_accounting_keeps_thread_exit_explicit(self):
        proc={'ticks':0,'start_ticks':1,'name':'main'}
        before={'wall_ns':0,'process':proc,'threads':{'1':proc,'2':proc}}
        after={'wall_ns':1000000000,'process':dict(proc,ticks=10),
               'threads':{'1':dict(proc,ticks=10)}}
        result=module.cpu_delta(before,after,tick_hz=100)
        self.assertEqual(result['incomplete_threads'],['2'])
        self.assertEqual(result['threads']['1']['cpu_seconds'],10/result['tick_hz'])

if __name__=='__main__':unittest.main()
