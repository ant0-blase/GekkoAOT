"""Validate new overlays against the locally composed pinned Aurora baseline.
Set GEKKOAOT_AURORA_TEST_SOURCE to the patched checkout. No checkout is modified.
"""
import ast
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
SOURCE=os.environ.get('GEKKOAOT_AURORA_TEST_SOURCE')

def canonical_markers(source):
    # Idempotent reconfiguration can apply an independent new overlay after
    # older ones. The order of their leading marker comments has no semantics.
    lines=source.splitlines(keepends=True);markers=[]
    while lines and lines[0].startswith('// GEKKOAOT_'):markers.append(lines.pop(0))
    return ''.join(sorted(markers)+lines)

def operations(script):
    tree=ast.parse(script.read_text());collected={}
    def collect(name,changes):
        if callable(changes):
            pairs=[]
            def replace(old,new,count=1):pairs.append((old,new,count))
            changes(replace)
        else:pairs=[(change[0],change[1],change[2] if len(change)>2 else 1) for change in changes]
        collected[name]=pairs
    # Execute only this repository's transformation declarations, using our
    # collector in place of edit. Exclude the final filesystem write loop.
    tree.body=[node for node in tree.body if not
               (isinstance(node,ast.FunctionDef) and node.name=='edit') and not isinstance(node,ast.For)]
    with patch.object(sys,'argv',[str(script),SOURCE or '/unused']):
        exec(compile(tree,str(script),'exec'),{'edit':collect})
    return collected

@unittest.skipUnless(SOURCE,'requires the locally composed Aurora checkout')
class EnhancedOverlays(unittest.TestCase):
    def test_reapply_and_failed_preflight(self):
        overlays=(
            ('aurora_enhanced_renderer.py','GEKKOAOT_ENHANCED_RENDERER_V1'),
            ('aurora_shader_compilation.py','GEKKOAOT_SHADER_COMPILATION_V1'),
            ('aurora_layered_capture.py','GEKKOAOT_LAYERED_SCENE_CAPTURE_V1'),
            ('aurora_contribution_capture.py','GEKKOAOT_CONTRIBUTION_CAPTURE_V1'),
            ('aurora_resource_history.py','GEKKOAOT_SCENE_RESOURCE_HISTORY_V1'),
            ('aurora_pipeline_integrity.py','GEKKOAOT_PIPELINE_INTEGRITY_V1'),
            ('aurora_draw_provenance.py','GEKKOAOT_DRAW_PROVENANCE_V1'),
            ('aurora_fragment_lighting.py','GEKKOAOT_FRAGMENT_LIGHTING_V1'),
            ('aurora_shadow_replay.py','GEKKOAOT_SHADOW_REPLAY_V1'),
            ('aurora_shadow_projection.py','GEKKOAOT_SHADOW_PROJECTION_V2'),
            ('aurora_material_capture.py','GEKKOAOT_MATERIAL_CAPTURE_V1'),
            ('aurora_capture_coverage.py','GEKKOAOT_CAPTURE_COVERAGE_V1'),
            ('aurora_capture_diagnostics.py','GEKKOAOT_CAPTURE_DIAGNOSTICS_V1'),
            ('aurora_environment_lighting.py','GEKKOAOT_ENVIRONMENT_LIGHTING_V1'),
            ('aurora_enhanced_measurement.py','GEKKOAOT_ENHANCED_MEASUREMENT_V1'))
        for index,(filename,marker) in enumerate(overlays):
            with self.subTest(overlay=filename),tempfile.TemporaryDirectory() as temp:
                target=Path(temp);script=ROOT/'runtime/gx/aurora'/filename
                transforms=operations(script);expected={};baseline={}
                later=[(laterMarker,operations(ROOT/'runtime/gx/aurora'/laterFile))
                       for laterFile,laterMarker in reversed(overlays[index+1:])
                       if laterFile not in ('aurora_enhanced_renderer.py','aurora_shader_compilation.py')]
                for name,pairs in transforms.items():
                    text=(Path(SOURCE)/name).read_text()
                    for laterMarker,changes in later:
                      if name in changes:
                        text=text.replace('// '+laterMarker+'\n','',1)
                        for old,new,count in reversed(changes[name]):
                            self.assertEqual(text.count(new),count,(name,new[:80]))
                            text=text.replace(new,old)
                    expected[name]=text
                    self.assertIn('// '+marker+'\n',text)
                    text=text.replace('// '+marker+'\n','',1)
                    for old,new,count in reversed(pairs):
                        self.assertEqual(text.count(new),count,(name,new[:80]))
                        text=text.replace(new,old)
                    baseline[name]=text
                    path=target/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
                result=subprocess.run([sys.executable,str(script),str(target)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual({name:canonical_markers((target/name).read_text()) for name in expected},
                                 {name:canonical_markers(text) for name,text in expected.items()})
                result=subprocess.run([sys.executable,str(script),str(target)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual({name:canonical_markers((target/name).read_text()) for name in expected},
                                 {name:canonical_markers(text) for name,text in expected.items()})
                for name,text in baseline.items():(target/name).write_text(text)
                # Break the last file so earlier files have passed their entire
                # preflight. The failed overlay must leave every file intact.
                name=next(reversed(transforms));old=transforms[name][0][0]
                (target/name).write_text(baseline[name].replace(old,'unsupported-anchor'))
                before={name:(target/name).read_bytes() for name in expected}
                result=subprocess.run([sys.executable,str(script),str(target)],capture_output=True,text=True)
                self.assertNotEqual(result.returncode,0)
                self.assertEqual({name:(target/name).read_bytes() for name in expected},before)

    def test_fov_projection_preflight(self):
        script=ROOT/'runtime/gx/aurora/aurora_projection_fov.py'
        constants={}
        for node in ast.walk(ast.parse(script.read_text())):
            if isinstance(node,ast.Assign) and isinstance(node.targets[0],ast.Name) and node.targets[0].id in ('old','new','anchor'):
                constants[node.targets[0].id]=ast.literal_eval(node.value)
        expected=(Path(SOURCE)/'lib/gx/shader_info.cpp').read_text()
        baseline=expected.replace(constants['new'],constants['old']).replace('\nextern "C" float gekkoaot_native_gx_fov_scale(float vertical) noexcept;','')
        self.assertNotIn('GEKKOAOT_PROJECTION_FOV_V1',baseline)
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'lib/gx/shader_info.cpp';path.parent.mkdir(parents=True);path.write_text(baseline)
            for _ in range(2):
                result=subprocess.run([sys.executable,str(script),temp],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(path.read_text(),expected)
            path.write_text(baseline.replace(constants['old'],'unsupported-anchor'))
            before=path.read_bytes();result=subprocess.run([sys.executable,str(script),temp],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0);self.assertEqual(path.read_bytes(),before)

if __name__=='__main__':unittest.main()
