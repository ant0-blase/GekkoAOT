"""Opt-in cold/warm shader startup and lossless async GPU validation."""
import os
from pathlib import Path
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

@unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS')=='1' and shutil.which('gamescope'),
                     'opt-in Gamescope GPU regression')
class ShaderCompilationGPU(unittest.TestCase):
    def test_exact_indexed_caches(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-array-cache-') as temp:
                for script in ('gx_array_probe.py','gx_compact_array_probe.py'):
                    self.run_probe(backend,script,Path(temp),'exact-caches',script[:-3])

    def test_capture_continuation(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-capture-continuation-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_CAPTURE_CONTINUATION='1'):
                output,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'capture-continuation','capture-continuation')
                self.assertIn('CAPTURE_CONTINUATION_PIXELS_PASSED',output)

    def test_capture_lifetime(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-capture-lifetime-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_CAPTURE_LIFETIME='1'):
                output,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'capture-lifetime','capture-lifetime')
                self.assertIn('CAPTURE_LIFETIME_PIXELS_PASSED',output)

    def test_particle_lights(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-particle-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_PARTICLE_LIGHTS='1',GEKKOAOT_NATIVE_GX_VERTEX_GATHER='1'):
                output,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'particle-lights','particle-lights')
                self.assertIn('PARTICLE_LIGHT_PIXELS_PASSED',output)
                self.assertRegex(output,r'PARTICLE_LIGHTS candidates=[1-9][0-9]? matched=[1-9][0-9]?')
                self.assertIn('ALPHA_FIRE_PIXELS_PASSED',output)
                self.assertIn('PARTICLE_CONTINUATION_PASSED',output)
                self.assertIn('PARTICLE_RADIANCE_CACHE_PASSED',output)
                self.assertIn('DECODED_PARTICLE_SOURCE_PASSED',output)
                self.assertRegex(output,r'known_sources=[1-9]')
                self.assertIn('PARTICLE_LIGHT_REACH_PASSED',output)

    def test_depth_of_field(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-dof-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_DOF='1'):
                output,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'dof','dof')
                self.assertIn('DEPTH_OF_FIELD_PIXELS_PASSED',output)

    def test_camera_background_upscale(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-sky-geometry-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_SKY_GEOMETRY='1'):
                output,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'sky-geometry','sky-cold')
                self.assertIn('SKY_GEOMETRY_UPSCALE_PASSED',output)
                self.assertIn('method=srvgg-native-v4 role=sky',output)
                self.assertRegex(output,r'HD_TEXTURE_BOUND original=64x64 gpu=256x256 .*role=sky')
                warm,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'sky-geometry','sky-warm')
                self.assertRegex(warm,r'HD_TEXTURES requested=2 cache_hits=[1-9]\d* generated=0')

    def test_blue_background_upscale(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-sky-upscale-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_SKY_UPSCALE='1'):
                output,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'sky-upscale','sky-cold')
                self.assertIn('BLUE_BACKGROUND_UPSCALE_PASSED',output)
                self.assertIn('method=srvgg-native-v4',output)
                self.assertIn('HD_ADMISSION source=256x256 stable_observations=1',output)
                self.assertIn('HD_TEXTURE_BOUND original=256x256 gpu=1024x1024',output)
                warm,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'sky-upscale','sky-warm')
                self.assertRegex(warm,r'HD_TEXTURES requested=2 cache_hits=[1-9]\d* generated=0')

    def test_streaming_video_upscale(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-video-upscale-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_STREAMING_VIDEO='1'):
                output,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'video-upscale','video-upscale')
                self.assertIn('STREAMING_VIDEO_UPSCALE_PASSED',output)
                self.assertIn('GEKKOAOT_VIDEO_RECONSTRUCTION method=edge-directed-gpu neural=0',output)
                # Only initial unclassified frames can enqueue a static job.
                count=re.search(r'HD_TEXTURES requested=(\d+)',output)
                self.assertIsNotNone(count)
                self.assertLessEqual(int(count[1]),2)

    def test_orthographic_font_upscale(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-font-upscale-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_2D_UPSCALE='1'):
                output,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'font-upscale','font-upscale')
                self.assertIn('ORTHOGRAPHIC_FONT_UPSCALE_PASSED',output)
                self.assertIn('method=coverage-contour-v3',output)
                self.assertIn('HD_TEXTURE_BOUND original=16x16 gpu=64x64',output)
                warm,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'font-upscale','font-warm')
                self.assertRegex(warm,r'HD_TEXTURES requested=2 cache_hits=[1-9]\d* generated=0')

    def test_surface_displacement(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-displacement-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_DISPLACEMENT='1'):
                output,_=self.run_probe(backend,'gx_material_probe.py',Path(temp),'displacement','displacement')
                self.assertIn('GX_DISPLACEMENT_PIXELS_PASSED',output)
                self.assertIn('GEKKOAOT_DISPLACEMENT_APPLIED',output)

    def test_compact_texture_model(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-compact-sr-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_NATIVE_CNN='1',GEKKOAOT_TEST_SR_BACKEND='2'):
                root=Path(temp)
                cold,_=self.run_probe(backend,'gx_material_probe.py',root,'compact-sr','compact-cold')
                self.assertIn('method=srvgg-native-v4',cold)
                self.assertIn('GEKKOAOT_COMPACT_GPU_VALIDATION independent_reference=1',cold)
                self.assertIn('cpu_convolutions=0',cold)
                self.assertNotIn('GEKKOAOT_COMPACT_GPU_ERROR',cold)
                self.assertNotIn('fallback=native-cpu',cold)
                self.assertRegex(cold,r'NATIVE_CNN generated=2 ')
                for size in (128,256):self.assertIn(f'HD_TEXTURE_BOUND original=64x64 gpu={size}x{size}',cold)
                warm,_=self.run_probe(backend,'gx_material_probe.py',root,'compact-sr','compact-warm')
                self.assertRegex(warm,r'HD_TEXTURES requested=2 cache_hits=[1-9]\d* generated=0')

    def test_native_texture_model(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-native-cnn-') as temp,patch.dict(os.environ,GEKKOAOT_TEST_NATIVE_CNN='1'):
                root=Path(temp)
                cold,_=self.run_probe(backend,'gx_material_probe.py',root,'native-cnn','cnn-cold')
                self.assertIn('method=espcn-native-v5',cold)
                self.assertRegex(cold,r'NATIVE_CNN generated=2 ')
                for size in (128,256):self.assertIn(f'HD_TEXTURE_BOUND original=64x64 gpu={size}x{size}',cold)
                warm,_=self.run_probe(backend,'gx_material_probe.py',root,'native-cnn','cnn-warm')
                self.assertRegex(warm,r'HD_TEXTURES requested=2 cache_hits=[1-9]\d* generated=0')
                self.assertRegex(warm,r'NATIVE_CNN generated=0 ')

    def test_mesh_scene_compatibility(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-mesh-scene-') as temp,patch.dict(os.environ,GEKKOAOT_GEOMETRY_REFINEMENT='2'):
                for script in ('gx_enhanced_probe.py','gx_sun_shadow_probe.py','gx_gi_ssr_probe.py'):
                    self.run_probe(backend,script,Path(temp),'mesh-'+script,script[:-3])

    def test_mesh_silhouettes(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-mesh-') as temp:
                output,_=self.run_probe(backend,'gx_mesh_probe.py',Path(temp),'mesh-silhouette','mesh')
                self.assertIn('original_triangles=8 refined_triangles=32/128',output)
                self.assertRegex(output,r'MESH_CACHE built=[1-9]\d* .*hits=[1-9]\d*')
                self.assertRegex(output,r'MESH_POSE_STATS updates=[1-9]\d*')

    def test_original_material_maps(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-material-cache-') as temp:
                root=Path(temp)
                cold,_=self.run_probe(backend,'gx_material_probe.py',root,'material-original','material-cold')
                self.assertIn('method=material-structure-v2',cold)
                self.assertRegex(cold,r'HD_TEXTURES requested=1 cache_hits=\d+ generated=1')
                warm,_=self.run_probe(backend,'gx_material_probe.py',root,'material-original','material-warm')
                self.assertRegex(warm,r'HD_TEXTURES requested=1 cache_hits=[1-9]\d* generated=0')

    def test_coverage_measurements(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory() as temp,patch.dict(os.environ,
                    GEKKOAOT_ENHANCED_MEASURE='1',GEKKOAOT_ENHANCED_DIAGNOSTICS='1'):
                output,_=self.run_probe(backend,'gx_enhanced_probe.py',Path(temp),'measured-scene','measured')
                rows=[dict(re.findall(r'(\w+)=([^\s]+)',line)) for line in output.splitlines()
                      if line.startswith('GEKKOAOT_ENHANCED_MEASUREMENT ') and 'samples=' in line]
                self.assertGreaterEqual(len(rows),5)
                self.assertTrue(all(int(r['samples'])==4096 for r in rows))
                self.assertTrue(any(int(r['valid_depth'])>3500 and int(r['valid_geometric_normal'])>3500 for r in rows))
                self.assertTrue(any(int(r['ao_changed'])>0 and int(r['output_changed'])>0 for r in rows))
                self.assertTrue(all(int(r['output_changed'])==0 for r in rows[:4]))
                for row in rows:
                    if row['timings_valid']=='1':self.assertTrue(all(int(n)>0 for n in row['gpu_ns'].split(',')))

    def run_probe(self, backend, script, data, game, label, async_compile='1'):
        directory=Path(__file__).parent
        library=os.environ['GEKKOAOT_NATIVE_GX_TEST_LIBRARY']
        env={**os.environ,'SDL_VIDEODRIVER':'x11','SDL_AUDIODRIVER':'dummy','EGL_PLATFORM':'x11',
             'GEKKOAOT_GRAPHICS_BACKEND':backend,'XDG_DATA_HOME':str(data),'GEKKOAOT_GAME':game,
             'GEKKOAOT_ASYNC_SHADER_COMPILATION':async_compile,'GEKKOAOT_STARTUP_SHADER_COMPILATION':'1',
             'GEKKOAOT_NATIVE_GX_TEST_LIBRARY':library,'GEKKOAOT_TEST_GX_LIBRARY':library}
        for key in ('DISPLAY','WAYLAND_DISPLAY','XAUTHORITY','GEKKOAOT_ENHANCED_RENDERER',
                    'GEKKOAOT_GPU_CAPTURE_DIR','GEKKOAOT_FOV_Y'):
            env.pop(key,None)
        if logs:=os.environ.get('GEKKOAOT_NATIVE_GX_TEST_LOG_DIR'):
            target=Path(logs);target.mkdir(parents=True,exist_ok=True)
            if script in ('gx_material_probe.py','gx_enhanced_probe.py','gx_fov_probe.py','gx_shader_variants_probe.py','gx_fragment_lighting_probe.py','gx_copy_alias_probe.py','gx_environment_probe.py','gx_sun_shadow_probe.py','gx_gi_ssr_probe.py'):
                captures=target/(backend+'-captures');captures.mkdir(exist_ok=True)
                env['GEKKOAOT_GPU_CAPTURE_DIR']=str(captures)
        child=[sys.executable,str(directory/script)]
        if script=='gx_compact_array_probe.py':child+=['--bridge',library]
        process=subprocess.Popen(['gamescope','--backend','headless','-W','640','-H','480','-w','640','-h','480','--',
            *child],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
            text=True,start_new_session=True)
        try:output,_=process.communicate(timeout=55)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid,signal.SIGKILL);output,_=process.communicate()
            self.fail('GPU shader probe timeout: '+output[-6000:])
        if logs:(Path(logs)/(backend+'-'+label+'.log')).write_text(output)
        # Gamescope can exit zero even if its child aborts: require both the
        # pixel oracle and orderly renderer shutdown, and reject device errors.
        marker={'gx_array_probe.py':'GX_ARRAY_PIXELS_PASSED','gx_compact_array_probe.py':'GX_COMPACT_ARRAY_PIXELS_PASSED','gx_mesh_probe.py':'GX_MESH_PIXELS_PASSED','gx_material_probe.py':'GX_MATERIAL_PIXELS_PASSED','gx_fov_probe.py':'GX_FOV_PIXELS_PASSED','gx_enhanced_probe.py':'GX_ENHANCED_PIXELS_PASSED','gx_shader_variants_probe.py':'GX_SHADER_VARIANTS_PIXELS_PASSED','gx_resource_history_probe.py':'GX_RESOURCE_HISTORY_PIXELS_PASSED','gx_fragment_lighting_probe.py':'GX_FRAGMENT_LIGHTING_PIXELS_PASSED','gx_copy_alias_probe.py':'GX_COPY_ALIAS_PIXELS_PASSED','gx_environment_probe.py':'GX_ENVIRONMENT_PIXELS_PASSED','gx_sun_shadow_probe.py':'GX_SUN_SHADOW_PIXELS_PASSED','gx_gi_ssr_probe.py':'GX_GI_SSR_PIXELS_PASSED','gx_shadow_replay_probe.py':'GX_SHADOW_REPLAY_PIXELS_PASSED'}[script]
        self.assertEqual(process.returncode,0,output[-6000:])
        self.assertIn(marker,output,output[-6000:])
        self.assertIn('GEKKOAOT_SHADER_COMPILATION_STATS',output,output[-6000:])
        for bad in ('Traceback','WebGPU error ','GPU Validation Error','SHADER_COMPILATION_ERROR'):
            self.assertNotIn(bad,output,output[-6000:])
        begin=re.search(r'SHADER_STARTUP begin known=(\d+) variants=(\d+)',output)
        end=re.search(r'SHADER_STARTUP complete known=(\d+) variants=(\d+)',output)
        self.assertIsNotNone(begin,output[-6000:]);self.assertIsNotNone(end,output[-6000:])
        self.assertEqual(begin.groups(),end.groups())
        return output,tuple(map(int,end.groups()))

    def test_startup_async_and_enhanced_pixels(self):
        for backend in os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS','Vulkan,OGL').split(','):
            with self.subTest(backend=backend),tempfile.TemporaryDirectory(prefix='gekkoaot-shader-cache-') as temp:
                root=Path(temp)
                cold,counts=self.run_probe(backend,'gx_shader_variants_probe.py',root,'game-one','cold')
                self.assertEqual(counts,(0,0))
                deferred=re.search(r'SHADER_COMPILATION_STATS deferred=(\d+)',cold)
                self.assertGreater(int(deferred[1]),0)
                _,counts=self.run_probe(backend,'gx_shader_variants_probe.py',root,'game-one','warm')
                self.assertGreater(counts[0],32);self.assertGreaterEqual(counts[1],counts[0])
                # Keep valid row sizes/versions while corrupting their actual
                # descriptions. Startup must reject them before typed field
                # access, then rebuild every required pipeline from live draws.
                databases=list(root.rglob('pipeline_cache.db'))
                self.assertEqual(len(databases),1)
                with sqlite3.connect(databases[0]) as database:
                    rows=database.execute('SELECT rowid,config FROM pipeline_cache').fetchall()
                    self.assertGreater(len(rows),32)
                    for rowid,config in rows:
                        changed=bytearray(config);changed[-1]^=0x80
                        database.execute('UPDATE pipeline_cache SET config=? WHERE rowid=?',(bytes(changed),rowid))
                rebuilt,counts=self.run_probe(backend,'gx_shader_variants_probe.py',root,'game-one','corrupt-rebuild')
                self.assertEqual(counts,(0,0))
                self.assertIn('GEKKOAOT_SHADER_CACHE_REJECT reason=content-key-mismatch',rebuilt)
                _,counts=self.run_probe(backend,'gx_shader_variants_probe.py',root,'game-one','repaired-warm')
                self.assertGreater(counts[0],32)
                _,counts=self.run_probe(backend,'gx_shader_variants_probe.py',root,'game-two','other-game')
                self.assertEqual(counts,(0,0),'Startup shaders must belong to the selected game')
                self.run_probe(backend,'gx_enhanced_probe.py',root,'enhanced-scene','enhanced')
                # A scene composited through a mid-frame texture copy keeps
                # its effects; perspective users and CPU color peeks do not.
                self.run_probe(backend,'gx_copy_alias_probe.py',root,'copy-alias-scene','copy-alias')
                self.run_probe(backend,'gx_fragment_lighting_probe.py',root,'fragment-lighting-scene','fragment-lighting')
                # World-stable sun/sky relight: game light, Custom world frame, Off exact.
                self.run_probe(backend,'gx_environment_probe.py',root,'environment-scene','environment')
                # Sun cascades of replayed geometry shade only the occluded ground.
                self.run_probe(backend,'gx_sun_shadow_probe.py',root,'sun-shadow-scene','sun-shadow')
                # One-bounce indirect light and Fresnel reflections of a red wall.
                self.run_probe(backend,'gx_gi_ssr_probe.py',root,'gi-ssr-scene','gi-ssr')
                self.run_probe(backend,'gx_shadow_replay_probe.py',root,'shadow-replay-scene','shadow-replay')
                history,_=self.run_probe(backend,'gx_resource_history_probe.py',root,'resource-scene','resource-history')
                edges=re.findall(r'copy_resource=(\d+) copy_revision=(\d+).*?reads=(.*)',history)
                self.assertGreaterEqual(len({int(revision) for resource,revision,reads in edges if int(resource)>0}),6)
                # Two sampling draws, no false third edge for the white draw
                # that has the copy bound but does not sample it in its shader.
                self.assertRegex(history,r'reads=\d+@\d+<-[1-9]\d*:maps01:draws2,')
                self.run_probe(backend,'gx_fov_probe.py',root,'fov-scene','fov')

if __name__=='__main__':unittest.main()
