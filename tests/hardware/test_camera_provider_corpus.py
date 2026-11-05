"""Opt-in proof/mutation regression against the user's local executable corpus."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

@unittest.skipUnless(os.environ.get('GEKKOAOT_CAMERA_TEST_DOL'), 'local symbol/corpus validation is opt-in')
class CameraProviderCorpus(unittest.TestCase):
    def test_admission_and_mutations(self):
        runner=os.environ.get('GEKKOAOT_CAMERA_TEST_RUNNER','build/bin/gekkoaot-native-run')
        data=Path(os.environ['GEKKOAOT_CAMERA_TEST_DOL']).read_bytes()
        offsets=struct.unpack_from('>18I',data);addresses=struct.unpack_from('>18I',data,72);sizes=struct.unpack_from('>18I',data,144)
        def offset(address):
            for start,base,size in zip(offsets,addresses,sizes):
                if base<=address<base+size:return start+address-base
            raise AssertionError(hex(address))
        with tempfile.TemporaryDirectory(prefix='gekkoaot-camera-proof-') as temp:
            root=Path(temp)
            def resolve(blob,label):
                dol=root/(label+'.dol');manifest=root/(label+'.txt');dol.write_bytes(blob)
                r=subprocess.run([runner,'--dol',str(dol),'--emit-sdk-manifest',str(manifest)],capture_output=True,text=True,timeout=30)
                self.assertEqual(r.returncode,0,r.stderr[-2000:])
                return [line.split() for line in manifest.read_text().splitlines() if ' AngularCameraSetFov ' in line]
            hooks=resolve(data,'original');self.assertEqual(len(hooks),1)
            pc=int(hooks[0][0],16);body=offset(pc)
            # Relocated mathematical constants are part of the semantic proof.
            for upper,lower in ((16,24),(20,32),(28,40),(72,80),(124,128)):
                hi=struct.unpack_from('>I',data,body+upper)[0]<<16&0xffffffff
                lo=struct.unpack_from('>h',data,body+lower+2)[0]
                pos=offset((hi+lo)&0xffffffff);changed=bytearray(data)
                value=struct.unpack_from('>f',changed,pos)[0]
                struct.pack_into('>f',changed,pos,value+1)
                self.assertEqual(resolve(changed,'constant'+str(upper)),[])
            # A relocation-identical call to the wrong math helper is rejected.
            changed=bytearray(data);w=struct.unpack_from('>I',changed,body+68)[0]
            struct.pack_into('>I',changed,body+68,w+4)
            self.assertEqual(resolve(changed,'callee'),[])
            changed=bytearray(data);changed[body+48+3]^=2
            self.assertEqual(resolve(changed,'arithmetic'),[])
            print('CAMERA_PROVIDER_PROOF_PASSED original + 5 constants + callee + arithmetic mutations')

@unittest.skipUnless(os.environ.get('GEKKOAOT_TANGENT_TEST_DOL'),'local tangent camera corpus is opt-in')
class TangentProviderCorpus(unittest.TestCase):
    def test_full_consumer_proof(self):
        runner=os.environ.get('GEKKOAOT_CAMERA_TEST_RUNNER','build/bin/gekkoaot-native-run')
        data=Path(os.environ['GEKKOAOT_TANGENT_TEST_DOL']).read_bytes()
        with tempfile.TemporaryDirectory(prefix='gekkoaot-tangent-proof-') as temp:
            root=Path(temp)
            def resolve(blob,label):
                dol=root/(label+'.dol');manifest=root/(label+'.txt');dol.write_bytes(blob)
                r=subprocess.run([runner,'--dol',str(dol),'--emit-camera-manifest',str(manifest)],capture_output=True,text=True,timeout=30)
                self.assertEqual(r.returncode,0,r.stderr[-2000:])
                return [line.split() for line in manifest.read_text().splitlines() if ' TangentCameraFrustum ' in line]
            hooks=resolve(data,'original');self.assertEqual(len(hooks),2)
            addresses={int(h[0],16) for h in hooks}
            for pc in addresses:
                body=None
                for i in range(18):
                    offset=struct.unpack_from('>I',data,i*4)[0];base=struct.unpack_from('>I',data,72+i*4)[0];size=struct.unpack_from('>I',data,144+i*4)[0]
                    if base<=pc<base+size:body=offset+pc-base
                self.assertIsNotNone(body)
                length=508 if struct.unpack_from('>I',data,body)[0]==0x9421ffc0 else 1904
                # Entire projection/cache update and frustum consumer bodies
                # reject changed fields, branches, math, references and return.
                for index in (0x4f,0xa7,0x8b,0x2b,length-1):
                    changed=bytearray(data);changed[body+index]^=4
                    remaining=resolve(changed,'mutation'+str(pc)+'-'+str(index))
                    self.assertEqual({int(h[0],16) for h in remaining},addresses-{pc})
            print('TANGENT_CAMERA_PROOF_PASSED full consumer and cached-projection update + 10 semantic mutations')

if __name__=='__main__':unittest.main()
