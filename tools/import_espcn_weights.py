from pathlib import Path
import struct

def fields(data):
 pos=0
 def integer():
  nonlocal pos
  n=0
  for shift in range(0,70,7):
   b=data[pos];pos+=1;n|=(b&127)<<shift
   if b<128:return n
  raise ValueError('oversize protobuf integer')
 while pos<len(data):
  tag=integer();kind=tag&7;field=tag>>3
  if kind==0:value=integer()
  elif kind==2:
   n=integer();value=data[pos:pos+n];pos+=n
  elif kind==5:value=data[pos:pos+4];pos+=4
  elif kind==1:value=data[pos:pos+8];pos+=8
  else:raise ValueError('protobuf wire type')
  if pos>len(data):raise ValueError('short protobuf')
  yield field,value

def parse(path):
 result={}
 for tag,node in fields(Path(path).read_bytes()):
  if tag!=1:continue
  data=list(fields(node));name=next(v.decode() for k,v in data if k==1);op=next(v.decode() for k,v in data if k==2)
  if op!='Const' and op not in {'Placeholder','Conv2D','Add','Relu','DepthToSpace','Tanh','Transpose'}:raise ValueError('unsupported graph operation '+op)
  for k,attr in data:
   if k!=5:continue
   a=dict(fields(attr))
   if a[1]!=b'value':continue
   t=dict(fields(dict(fields(a[2]))[8]))
   if t[1]!=1:continue
   shape=[dict(fields(dim))[1] for k,dim in fields(t[2]) if k==2]
   values=struct.unpack('<'+str(len(t[4])//4)+'f',t[4]);result[name]=(shape,values)
 return result

def export(directory,output):
 import hashlib,json
 directory=Path(directory)
 metadata={"repository":"https://github.com/fannymonori/TF-ESPCN","revision":"5c628eca82028161a53e1265cc3a5b571ab8625f","license":"Apache-2.0","models":{}}
 lines=['// SPDX-License-Identifier: Apache-2.0','// Pretrained TF-ESPCN constants; see assets/models/espcn/SOURCE.md.','#pragma once','#include <cstdint>','namespace GekkoAOT::GX::TinySR::Weights {']
 expected={2:'59f77351e1d7c0057bf6fe088b4a8a07e42c468c8c8aebb674a6b4ea1823221d',4:'e403f06309229cf36009cd8fb0da032ba7643fae9f15cf94fe562e8edf8fef47'}
 for scale in (2,4):
  path=directory/f'ESPCN_x{scale}.pb';digest=hashlib.sha256(path.read_bytes()).hexdigest()
  if digest!=expected[scale]:raise ValueError('unrecognized pretrained graph checksum')
  tensors=parse(path);shapes={'f1':[5,5,1,64],'f2':[3,3,64,32],'f3':[3,3,32,scale*scale],'b1':[64],'b2':[32],'b3':[scale*scale]}
  if tensors.keys()!=shapes.keys():raise ValueError('unexpected graph constants')
  count=0
  for name,shape in shapes.items():
   actual,values=tensors[name]
   if actual!=shape:raise ValueError('unexpected tensor dimensions')
   count+=len(values);lines.append(f'alignas(16) inline constexpr float X{scale}{name.upper()}[] = {{')
   for i in range(0,len(values),8):
    def literal(v):
     import math
     if not math.isfinite(v):raise ValueError('nonfinite model coefficient')
     t=format(v,'.9g');return t+('.0' if not any(c in t for c in '.eE') else '')+'f'
    lines.append('  '+','.join(literal(v) for v in values[i:i+8])+',')
   lines.append('};')
  lines.append(f'inline constexpr std::uint64_t X{scale}Fingerprint=0x{digest[:16]}ull;')
  metadata['models'][str(scale)]={"upstream_sha256":digest,"parameters":count,"float32_bytes":count*4}
 lines.append('}')
 Path(output).write_text('\n'.join(lines)+'\n')
 Path('assets/models/espcn/manifest.json').write_text(json.dumps(metadata,indent=2)+'\n')
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser(description='Import pinned ESPCN frozen constants with Python stdlib only; no inference framework.')
 p.add_argument('graphs',help='Directory containing ESPCN_x2.pb and ESPCN_x4.pb')
 p.add_argument('--output',default='runtime/gx/espcn_weights.h')
 a=p.parse_args();export(a.graphs,a.output)
