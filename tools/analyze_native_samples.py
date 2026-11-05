#!/usr/bin/env python3
"""Resolve GekkoAOT native CPU timer samples, including remapped huge AOT text.
Requires pyelftools only for this offline diagnostic, never for the runtime.
"""
import argparse
import bisect
from collections import Counter
import csv
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile


def analyze(prefix, tid=None):
    prefix=Path(prefix)
    meta=json.loads(Path(str(prefix)+'.samples.json').read_text())
    mappings=[]
    for line in Path(str(prefix)+'.maps.end').read_text().splitlines():
        fields=line.split(maxsplit=5)
        if len(fields)!=6 or 'x' not in fields[1]:continue
        low,high=(int(v,16) for v in fields[0].split('-'))
        mappings.append((low,high,int(fields[2],16),fields[5].removesuffix(' (deleted)')))
    jit=[]
    perfmap=Path(str(prefix)+'.perf-map')
    if perfmap.exists():
        for line in perfmap.read_text().splitlines():
            fields=line.split(maxsplit=2)
            if len(fields)==3:jit.append((int(fields[0],16),int(fields[1],16),fields[2]))
    jit.sort();jit_starts=[v[0] for v in jit]
    libraries={}
    def symbols(path):
        if path in libraries:return libraries[path]
        result=[]
        try:
            with Path(path).open('rb') as stream:
                elf=ELFFile(stream)
                for name in ('.symtab','.dynsym'):
                    table=elf.get_section_by_name(name)
                    if table:
                        result.extend((int(s['st_value']),int(s['st_size']),s.name) for s in table.iter_symbols()
                                      if s['st_info']['type']=='STT_FUNC' and s['st_shndx']!='SHN_UNDEF' and s.name)
                segments=[(int(s['p_offset']),int(s['p_filesz']),int(s['p_vaddr'])) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        except (OSError,ValueError):segments=[]
        result=sorted(set(result));libraries[path]=(result,[v[0] for v in result],segments)
        return libraries[path]
    resolved={}
    def resolve(pc):
        if pc in resolved:return resolved[pc]
        i=bisect.bisect_right(jit_starts,pc)-1
        if i>=0 and pc<jit[i][0]+jit[i][1]:answer=('AOT huge text',jit[i][2])
        else:
            answer=('unmapped',hex(pc))
            for low,high,offset,path in mappings:
                if not low<=pc<high:continue
                table,starts,segments=symbols(path);address=pc-low+offset
                for file_start,size,virtual_start in segments:
                    if file_start<=address<file_start+size:address+=virtual_start-file_start;break
                at=bisect.bisect_right(starts,address)-1
                name=hex(address)
                if at>=0 and (not table[at][1] or address<table[at][0]+table[at][1]):name=table[at][2]
                answer=(path,name);break
        resolved[pc]=answer;return answer
    counts=Counter();threads=Counter()
    with Path(str(prefix)+'.samples.csv').open() as stream:
        for row in csv.DictReader(stream):
            thread=int(row['tid'])
            if tid is not None and thread!=tid:continue
            counts[resolve(int(row['pc'],16))]+=1;threads[thread]+=1
    total=sum(counts.values())
    return dict(schema=1,metadata=meta,selected_tid=tid,samples=total,thread_samples=dict(threads),
                note='Exclusive sampled instruction pointers; no call-stack attribution or GPU-time claim.',
                symbols=[dict(library=lib,symbol=name,samples=count,percent=100*count/max(1,total)) for (lib,name),count in counts.most_common()])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix',type=Path);parser.add_argument('--tid',type=int);parser.add_argument('--output',type=Path)
    args=parser.parse_args();result=analyze(args.prefix,args.tid)
    if args.output:args.output.write_text(json.dumps(result,indent=2)+'\n')
    for row in result['symbols'][:30]:print(f"{row['percent']:6.2f}% {row['samples']:6d} {Path(row['library']).name}: {row['symbol']}")
    print('samples:',result['samples'],'threads:',result['thread_samples'])


if __name__=='__main__':main()
