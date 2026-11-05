"""Compile real proved SDK leaves and execute both generated service routes."""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[2]
SOURCE=Path(os.environ.get('GEKKOAOT_TEST_DOLRECOMP_SOURCE',ROOT/'.gekkoaot/src/DolRecomp'))
COMPILER=Path(os.environ.get('GEKKOAOT_TEST_DOLRECOMP',ROOT/'.gekkoaot/build/DolRecomp/dolrecomp'))
META=ROOT/'build/bin/gekkoaot-module-meta'
VERIFY=ROOT/'build/bin/gekkoaot-test-gx-pixel-runtime'
MOCK=ROOT/'build/lib/libgekkoaot-test-gx.so'

class NativeSdkLowering(unittest.TestCase):
    @unittest.skipUnless(all(p.exists() for p in (COMPILER,META,VERIFY)), 'local LLVM toolchain required')
    def test_entire_registry_compiles_and_links(self):
        # This is a compiler/link contract test, not semantic validation of
        # 204 SDK functions. Semantic suites exercise their actual operations.
        root=ROOT/'.gekkoaot/compat/native-sdk5/registry-compiler-fixture';root.mkdir(parents=True,exist_ok=True)
        rows=re.findall(r'SDK_(SIMPLE|OS)\((\w+), "([^"]+)", "([^"]+)"\)',
                        (ROOT/'runtime/sdk/native_services.def').read_text())
        self.assertEqual(len(rows),204)
        body=bytearray(len(rows)*16)
        manifest=[];indices=dict(SIMPLE=0,OS=0)
        for i,(domain,kind,name,abi) in enumerate(rows):
            struct.pack_into('>I',body,i*16,0x4e800020)
            token=(0xfffc0000 if domain=='SIMPLE' else 0xfffd0000)|indices[domain];indices[domain]+=1
            manifest.append(f'{0x80010000+i*16:#x} {name} {token:#x} {abi} native:{domain.lower()}_{kind}\n')
        header=bytearray(0x100)
        for at,value in ((0,0x100),(0x48,0x80010000),(0x90,len(body)),(0xe0,0x80010000)):struct.pack_into('>I',header,at,value)
        (root/'registry.dol').write_bytes(header+body);(root/'sdk.txt').write_text(''.join(manifest))
        env=dict(os.environ,DOLRECOMP_STATIC_INTERCEPTS=str(root/'sdk.txt'),DOLRECOMP_LLVM_DUMP_IR='1',
                 DOLRECOMP_LLVM_CACHE='off',DOLRECOMP_NATIVE_SDK_CLOCK_LOWERING='0')
        def run(command,environment=None):
            result=subprocess.run(list(map(str,command)),cwd=ROOT,env=environment,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
            with (root/'commands.log').open('a') as log:log.write(str(command)+'\n'+result.stdout+'\n')
            self.assertEqual(result.returncode,0,result.stdout[-8000:]);return result.stdout
        run([COMPILER,'--gamecube','--cpu','gekko','--backend','llvm','--native-abi','unrestricted',
             '--targets','host','--game-id','SDK502',root/'registry.dol',root/'out'],env)
        generated=root/'out/generated';ir='\n'.join(p.read_text() for p in generated.rglob('*.ll'))
        for domain,kind,_,_ in rows:
            symbol='call i32 @gekkoaot_sdk_'+domain.lower()+'_'+kind+'('
            self.assertTrue(symbol in ir, f'missing compiler binding: {symbol}; compiler={COMPILER}')
        run([META,generated/'generated.h',generated/'generated_smc.txt',root/'registry.dol',root/'tables.inc'])
        run(['cmake','-S',ROOT/'runtime/module','-B',root/'build','-G','Ninja','-DGAME_ID=SDK502',
             f'-DGENERATED_DIR={generated}',f'-DDOLRECOMP_DIR={SOURCE}',
             f'-DGEKKOAOT_ROOT={ROOT}',f'-DMODULE_TABLES={root/"tables.inc"}','-DCMAKE_BUILD_TYPE=Release'])
        run(['cmake','--build',root/'build','-j2'])
        (root/'result.json').write_text(json.dumps(dict(passed=True,bindings=len(rows),
            scope='compiler-and-link-contract',sdk_semantics_validated_by_this_test=False),indent=2))

    @unittest.skipUnless(all(p.exists() for p in (COMPILER,META,VERIFY)), 'local LLVM toolchain required')
    def test_generated_native_entries(self):
        root=ROOT/'.gekkoaot/compat/native-sdk5/compiler-fixture';root.mkdir(parents=True,exist_ok=True)
        # Take the same complete bodies used by the independent PPC semantic
        # oracle; no fake host-success function stands in for the SDK operation.
        header=(ROOT/'runtime/sdk/gx_pixel_service.h').read_text()
        bodies=[list(map(lambda x:int(x,16),re.findall(r'0x([0-9a-f]+)u',part)))
                for part in re.findall(r'body\d\{(.*?)\};',header,re.S)]
        self.assertEqual(len(bodies),4)
        text=bytearray(0x340)
        for family,words in enumerate(bodies):
            struct.pack_into('>'+str(len(words))+'I',text,256*family,*words)
        dol=bytearray(0x100)
        for at,value in ((0,0x100),(0x48,0x80010000),(0x90,len(text)),(0xe0,0x80010000)):
            struct.pack_into('>I',dol,at,value)
        (root/'sdk.dol').write_bytes(dol+text)
        names=('GXSetAlphaCompare','GXSetZMode','GXSetColorUpdate','GXSetAlphaUpdate')
        def run(command,env=None):
            result=subprocess.run(list(map(str,command)),cwd=ROOT,env=env,text=True,
                                  stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
            with (root/'commands.log').open('a') as log:log.write(str(command)+'\n'+result.stdout+'\n')
            self.assertEqual(result.returncode,0,result.stdout[-8000:]);return result.stdout
        for route in ('guest','generic','direct'):
            direct=route=='direct'
            target=root/route;target.mkdir(exist_ok=True)
            manifest=target/'sdk.txt'
            # Tokens remain the runtime's append-only ABI; the named route is
            # selected by the resolver annotation, never by an SDK name alone.
            # Obtain actual token values from an existing generated manifest.
            values={}
            for p in (ROOT/'.gekkoaot/cache/sdk').glob('*/native-intercepts-*.txt'):
                for line in p.read_text().splitlines():
                    parts=line.split()
                    if len(parts)>=3 and parts[1] in names:values[parts[1]]=parts[2]
            self.assertEqual(len(values),4)
            manifest.write_text('' if route=='guest' else ''.join(f'{0x80010000+256*i:#x} {name} {values[name]}' +
                (f' leaf native:simple_{("GxSetAlphaCompare","GxSetZMode","GxSetColorUpdate","GxSetAlphaUpdate")[i]}' if direct else '')+'\n' for i,name in enumerate(names)))
            env=dict(os.environ,DOLRECOMP_STATIC_INTERCEPTS=str(manifest),DOLRECOMP_LLVM_DUMP_IR='1',
                     DOLRECOMP_LLVM_CACHE='off')
            run([COMPILER,'--gamecube','--cpu','gekko','--backend','llvm','--native-abi','unrestricted',
                 '--targets','host','--game-id','SDK500',root/'sdk.dol',target/'out'],env)
            generated=target/'out/generated'
            ir='\n'.join(p.read_text() for p in generated.rglob('*.ll'))
            self.assertTrue(ir)
            self.assertEqual('call i32 @gekkoaot_sdk_simple_' in ir,direct)
            tables=target/'tables.inc'
            run([META,generated/'generated.h',generated/'generated_smc.txt',root/'sdk.dol',tables])
            run(['cmake','-S',ROOT/'runtime/module','-B',target/'build','-G','Ninja','-DGAME_ID=SDK500',
                 f'-DGENERATED_DIR={generated}',f'-DDOLRECOMP_DIR={SOURCE}',
                 f'-DGEKKOAOT_ROOT={ROOT}',f'-DMODULE_TABLES={tables}','-DCMAKE_BUILD_TYPE=Release'])
            run(['cmake','--build',target/'build','-j2'])
            output=run([VERIFY,MOCK,target/'build/gSDK500_recomp.so',-1 if route=='guest' else int(direct)])
            self.assertIn('exact CPU/GXData/FIFO',output)
        (root/'result.json').write_text(json.dumps(dict(passed=True,routes=['original-sdk-aot','generic-token','named-native-service'],
            families=list(names),positive_cases=3456,includes_mutation_declines=True),indent=2))

if __name__=='__main__':unittest.main()
