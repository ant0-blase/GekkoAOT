"""Coverage must preserve aliases and distinguish declarations from execution."""
import importlib.util
import json
import struct
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('ledger',ROOT/'tools/sdk_coverage_ledger.py')
ledger=importlib.util.module_from_spec(spec);spec.loader.exec_module(ledger)

class CoverageLedger(unittest.TestCase):
    def test_direct_call_and_data_reference_scan_is_bounded(self):
        with tempfile.TemporaryDirectory() as temp:
            image=bytearray(0x128)
            for at,value in ((0,0x100),(0x48,0x80001000),(0x90,16),
                             (28,0x120),(0x64,0x80003000),(0xac,8)):
                struct.pack_into('>I',image,at,value)
            # BL +32 reaches the SDK; a plain branch is not a call site.
            struct.pack_into('>4I',image,0x100,0x48000021,0x4800001c,0x4e800020,0)
            struct.pack_into('>2I',image,0x120,0x80001020,0x80001024)
            binary=Path(temp)/'fixture.dol';binary.write_bytes(image)
            entries=[dict(address=0x80001020),dict(address=0x80001024)]
            calls,refs=ledger.binary_references(binary,entries)
            self.assertEqual(calls,{0x80001020:['0x80001000'],0x80001024:[]})
            self.assertEqual(refs,{0x80001020:['0x80003000'],0x80001024:['0x80003004']})
            struct.pack_into('>I',image,0xac,0x100);binary.write_bytes(image)
            with self.assertRaises(ValueError):ledger.binary_references(binary,entries)

    def test_no_native_or_test_claim_from_manifest_or_source_mentions(self):
        entries=[dict(address=0x80001000,size=60,name='GXLoadPosMtxImm'),
                 dict(address=0x80002000,size=40,name='OSGetTime'),
                 dict(address=0x80003000,size=16,name='unknown'),
                 dict(address=0x80004000,size=32,name='unknown')]
        direct={0x80001000:dict(name='GXLoadPosMtxImm',token='0xfffc001d',abi='leaf')}
        names=ledger.classify_entries(entries,direct,set(),{}, {'GXLoadPosMtxImm'})
        self.assertEqual(len(entries),4);self.assertEqual(len(names),3)
        self.assertEqual([e['path'] for e in entries],['UNVERIFIED','AOT_GUEST','UNVERIFIED','UNVERIFIED'])
        self.assertTrue(entries[0]['test_referenced']);self.assertFalse(entries[0]['tested'])
        self.assertTrue(all(e['runtime_native_calls'] is None and e['exclusive_cpu_ns'] is None for e in entries))
        self.assertEqual(len({e['entry_id'] for e in entries}),4)

    def test_validation_and_measurements_bound_to_input_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);mapping=root/'map';mapping.write_text('0x80001000 60 GXLoadPosMtxImm\n')
            manifest=root/'manifest';manifest.write_text('0x80001000 GXLoadPosMtxImm 0xfffc001d\n')
            evidence=root/'evidence.json'
            artifact=root/'passed-fixture.json';artifact.write_text('{"passed":true}')
            record=dict(address='0x80001000',name='GXLoadPosMtxImm',status='NATIVEGX_IR',
                        validation='passed',implementation='runtime/native/runtime.cpp',
                        lowering_method='static-token-and-typed-matrix',validation_artifact='passed-fixture.json',
                        validation_artifact_sha256=ledger.source_identity(artifact),
                        native_calls=25,guest_calls=0,exclusive_cpu_ns=700,guest_instructions=0)
            value=dict(schema=1,service_map_sha256=ledger.source_identity(mapping),
                       manifest_sha256=ledger.source_identity(manifest),entries=[record])
            def write():evidence.write_text(json.dumps(value))
            write();observations=ledger.verified_observations(evidence,mapping,manifest)
            entries=ledger.read_map(mapping);direct,support=ledger.read_manifest(manifest)
            ledger.classify_entries(entries,direct,support,observations,set())
            self.assertEqual(entries[0]['path'],'NATIVEGX_IR');self.assertTrue(entries[0]['tested'])
            self.assertEqual(entries[0]['runtime_native_calls'],25)
            self.assertEqual(entries[0]['runtime_guest_calls'],0)
            artifact.write_text('{"passed":false}')
            with self.assertRaises(ValueError):ledger.verified_observations(evidence,mapping,manifest)
            artifact.write_text('{"passed":true}')
            record['validation']='unverified';write()
            with self.assertRaises(ValueError):ledger.verified_observations(evidence,mapping,manifest)
            record['validation']='passed';record['native_calls']=-1;write()
            with self.assertRaises(ValueError):ledger.verified_observations(evidence,mapping,manifest)
            record['native_calls']=25;write();manifest.write_text(manifest.read_text()+'# changed\n')
            with self.assertRaises(ValueError):ledger.verified_observations(evidence,mapping,manifest)

    def test_conflicting_manifest_cannot_validate_another_function(self):
        entries=[dict(address=0x80001000,size=60,name='GXLoadPosMtxImm')]
        direct={0x80001000:dict(name='GXLoadTexMtxImm',token='0xfffc001e',abi='leaf')}
        ledger.classify_entries(entries,direct,set(),{},set())
        self.assertFalse(entries[0]['declaration_matches'])
        self.assertEqual(entries[0]['path'],'UNVERIFIED')
        with self.assertRaises(ValueError):
            ledger.classify_entries(entries,direct,set(),{0x80001000:dict(name='GXLoadPosMtxImm',status='NATIVEGX_IR')},set())

if __name__=='__main__':unittest.main()
