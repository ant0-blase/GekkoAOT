"""Diagnostics cannot promote configuration or execution into visual proof."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from renderer_coverage_report import report

class CoverageReport(unittest.TestCase):
    def test_no_false_visual_success(self):
        self.assertEqual(report([])['outcome'],'NOT_EXECUTED')
        self.assertEqual(report(['GEKKOAOT_ENHANCED_STATS graphs=0 missing=4'])['outcome'],'NO_VALID_SCENE')
        r=report(['GEKKOAOT_ENHANCED_GRAPH graph=300 passes=5 views=1'])
        self.assertIsNone(r['outcome']);self.assertIsNone(r['gpu_timing_ns'])
        self.assertIsNone(r['graph_measurement']['valid_depth_fraction'])

    def test_measurement_needs_matching_successful_normal_presentation(self):
        sample='GEKKOAOT_ENHANCED_MEASUREMENT graph=120 samples=4096 valid_depth=2048 valid_geometric_normal=2000 ao_changed=32 contact_changed=2 output_changed=30 timings_valid=1 gpu_ns=10,20,30,40,50'
        lines=['GEKKOAOT_ENHANCED_GRAPH graph=120',sample]
        self.assertIsNone(report(lines)['outcome'])
        for presentation in ('graph=119 view=0 wsi_success=1','graph=120 view=2 wsi_success=1','graph=120 view=0 wsi_success=0'):
            r=report(lines+['GEKKOAOT_ENHANCED_PRESENT '+presentation])
            self.assertEqual(r['graph_measurement']['presented_changed_samples'],0)
        lines+=['GEKKOAOT_ENHANCED_PRESENT graph=120 view=0 wsi_success=1']
        r=report(lines)
        self.assertEqual(r['graph_measurement']['presented_changed_samples'],30)
        self.assertIsNone(r['outcome']) # still requires visual review
        self.assertEqual(r['gpu_timing_ns']['ao_and_contact']['median'],20)
        review=dict(outcome='VALID_EFFECT',rationale='Reviewed receiver/HUD comparison',captures=['a.png','b.png'])
        self.assertEqual(report(lines,review=review)['outcome'],'VALID_EFFECT')
        with self.assertRaises(ValueError): report(lines[:2],review=review)

if __name__=='__main__':unittest.main()
