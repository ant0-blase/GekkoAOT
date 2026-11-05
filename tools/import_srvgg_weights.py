#!/usr/bin/env python3
"""Import the pinned official Real-ESRGAN compact model; standard library only."""
import collections, hashlib, io, math, pickle, struct, zipfile
from pathlib import Path
EXPECTED='8dc7edb9ac80ccdc30c3a5dca6616509367f05fbc184ad95b731f05bece96292'
class Reader(pickle.Unpickler):
 def find_class(self,module,name):
  if (module,name)==('collections','OrderedDict'):return collections.OrderedDict
  if (module,name)==('torch','FloatStorage'):return 'float32'
  if (module,name)==('torch._utils','_rebuild_tensor_v2'):return lambda storage,offset,shape,stride,*args:(storage,offset,shape,stride)
  raise ValueError('unsupported pickle global '+module+'.'+name)
 def persistent_load(self,key):
  if len(key)!=5 or key[:2]!=('storage','float32'):raise ValueError('unsupported storage')
  return key[2],key[4]
def export(source,output):
 blob=Path(source).read_bytes()
 if hashlib.sha256(blob).hexdigest()!=EXPECTED:raise ValueError('unrecognized official model checksum')
 z=zipfile.ZipFile(io.BytesIO(blob));tensors=Reader(io.BytesIO(z.read('archive/data.pkl'))).load()['params']
 def values(name,shape):
  (key,count),offset,actual,stride=tensors.pop(name)
  if tuple(actual)!=tuple(shape) or offset!=0 or math.prod(shape)!=count:raise ValueError('unexpected tensor '+name)
  expected=[];n=1
  for d in reversed(shape):expected.append(n);n*=d
  if tuple(reversed(expected))!=tuple(stride):raise ValueError('noncontiguous tensor')
  v=struct.unpack('<'+str(count)+'f',z.read('archive/data/'+key))
  if not all(math.isfinite(x) for x in v):raise ValueError('nonfinite tensor')
  return v
 result=bytearray(b'GSRVGG01'+struct.pack('<I',34));params=0
 for layer in range(34):
  ic=3 if layer==0 else 64;oc=48 if layer==33 else 64;idx=layer*2
  w=values(f'body.{idx}.weight',(oc,ic,3,3));bias=values(f'body.{idx}.bias',(oc,));slope=values(f'body.{idx+1}.weight',(oc,)) if layer<33 else (1.,)*oc
  hwio=[w[((o*ic+i)*3+y)*3+x] for y in range(3) for x in range(3) for i in range(ic) for o in range(oc)]
  result+=struct.pack('<II',ic,oc)+struct.pack('<'+str(len(hwio)+len(bias)+len(slope))+'f',*(hwio+list(bias)+list(slope)));params+=len(w)+len(bias)+(len(slope) if layer<33 else 0)
 if tensors:raise ValueError('unconsumed model tensors')
 Path(output).parent.mkdir(parents=True,exist_ok=True);Path(output).write_bytes(result)
 print('SRVGG imported',params,'parameters',len(result),'bytes',hashlib.sha256(result).hexdigest())
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('source');p.add_argument('output');a=p.parse_args();export(a.source,a.output)
