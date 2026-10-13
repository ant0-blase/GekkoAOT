"""Check relocation, unfamiliar code and cross-title evidence exclusion."""
from pathlib import Path
import importlib.util
import json
import struct
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
TOOL=ROOT/'tools/crossgame_db.py'
spec=importlib.util.spec_from_file_location('crossgame_db',TOOL)
db=importlib.util.module_from_spec(spec);sys.modules[spec.name]=db;spec.loader.exec_module(db)
OUT=ROOT/'.gekkoaot/diagnostics/runtime-opt/crossgame-probe'

def dol(path,address,words):
    blob=bytearray(256)
    for offset,value in ((0,256),(0x48,address),(0x90,len(words)*4),(0xe0,address)):
        struct.pack_into('>I',blob,offset,value)
    path.write_bytes(blob+struct.pack('>'+str(len(words))+'I',*words))

def match(path,game):
    subprocess.run([sys.executable,str(TOOL),'match','--dol',str(path),'--db',str(OUT/'db.json'),
                    '--env',str(OUT/'match.env'),'--symbol-order',str(OUT/'hot.txt'),
                    '--compile-chunk','1024','--chunk-locked','--exact-only',
                    '--exclude-game',game],check=True)
    env=dict(line.split('=',1) for line in (OUT/'match.env').read_text().splitlines())
    assert env['GEKKOAOT_CROSSGAME_CHUNK']=='1024'
    return (OUT/'hot.txt').read_text()

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    words=[0x80640000,0x38630001,0x90640000]*3+[0x38630002,0x38630003,0x4e800020]
    region=db.Region(0x80010000,0x80010000+4*len(words),tuple(words))
    entry=db.fingerprint(region);entry.update(games=['SOURCE'],samples=3,confidence=.85,policy='balanced')
    data=db.empty_db();data['entries']=[entry]
    (OUT/'db.json').write_text(json.dumps(data))
    path=OUT/'relocated.dol';dol(path,0x80020000,words)
    assert match(path,'TARGET')=='func_80020000_budget\n'
    assert match(path,'SOURCE')=='', 'own-title evidence leaked'
    unfamiliar=[0x60000000]*11+[0x4e800020]
    dol(path,0x80020000,unfamiliar)
    assert match(path,'TARGET')=='', 'unfamiliar code received hot hints'
    # Even an identical simhash must not bypass the exact-only consumer gate.
    dol(path,0x80020000,words);entry['fingerprint']='0'*64
    (OUT/'db.json').write_text(json.dumps(data))
    assert match(path,'TARGET')=='', 'fuzzy evidence entered the exact-only prior'
    print('CrossGameDB matching: relocation / self-exclusion / unknown / fuzzy gates passed')

if __name__=='__main__': main()
