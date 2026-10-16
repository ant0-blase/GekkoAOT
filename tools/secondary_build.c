// SPDX-License-Identifier: GPL-3.0-or-later
// Native, dependency-free secondary executable builder for GekkoAOT.
// Disc files are scanned for GameCube RELs and fixed-address PPC ELF/DOL code.
// RELs are compiled together so DolRecomp can resolve inter-module imports.
#define _CRT_SECURE_NO_WARNINGS
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define PATH_SEP '\\'
#define MKDIR(p) _mkdir(p)
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <dirent.h>
#include <unistd.h>
#include <strings.h>
#include <sys/wait.h>
#define PATH_SEP '/'
#define MKDIR(p) mkdir((p), 0777)
#endif

#define MAX_PATHBUF 4096
#define REL_BASE 0x80500000u
#define REL_ALIGN 0x00010000u
#define REL24_WINDOW_SIZE 0x02000000u
#define REL24_WINDOW_GUARD REL_ALIGN
/* v364: when 64 KiB slots cannot keep every REL within a REL24 branch of the
   DOL (Twilight Princess: 757 RELs, 10 MiB of images, 50 MiB of slots), the
   canonical layout packs the images at one shared alignment (the largest REL
   section/BSS alignment, at least 0x20), each ending at its last section.
   DolRecomp's REL batch places them identically from DOLRECOMP_REL_ALIGN.
   Titles that already fit keep the 64 KiB layout, so their REL batches are
   unchanged. */
#define REL_DENSE_ALIGN 0x20u
static int g_rel_dense_layout=0;
static uint32_t g_rel_dense_align=REL_DENSE_ALIGN;
#define MAX_REL_SECTIONS 4096u
#define SECONDARY_ROUTER_CACHE_TAG "gekkoaot-secondary-router-v20-lc-v308"
#define REL_CODEGEN_CACHE_TAG "gekkoaot-rel-published-pointer-v220-own-code-data-cross-module-xmodgpr207-cfgaddr221-materialize222"
#ifndef GEKKOAOT_SECONDARY_SOURCE_HASH
#define GEKKOAOT_SECONDARY_SOURCE_HASH "secondary-v6-v277"
#endif

typedef struct { uint32_t index, offset, size; int executable, bss; uint32_t linked; } RelSection;
typedef struct {
  char path[MAX_PATHBUF];
  uint32_t module_id, version, section_count, section_info, file_size;
  uint32_t bss_size, bss_alignment, fix_size, base, alignment;
  RelSection* sections;
} RelInfo;

typedef enum { CAND_REL, CAND_DOL, CAND_ELF } CandidateKind;
typedef struct { char path[MAX_PATHBUF]; CandidateKind kind; } Candidate;
typedef struct { Candidate* v; size_t n, cap; } CandidateVec;
typedef struct { RelInfo* v; size_t n, cap; } RelVec;
typedef struct { char** v; size_t n, cap; } StrVec;
typedef struct { uint32_t start, end; } Range;
typedef struct { Range* v; size_t n, cap; } RangeVec;
typedef struct { uint32_t* v; size_t n, cap; } U32Vec;

static uint16_t be16(const unsigned char* p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put_be32(unsigned char* p, uint32_t v) { p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v; }
static uint32_t align_up(uint32_t v, uint32_t a) { uint64_t x=((uint64_t)v+a-1u)/a*a; return x>0xffffffffu?0:(uint32_t)x; }
static uint32_t align_down(uint32_t v, uint32_t a) { return a ? (v/a)*a : v; }
static int is_file(const char* p) { struct stat s; return stat(p,&s)==0 && S_ISREG(s.st_mode); }
static int is_dir(const char* p) { struct stat s; return stat(p,&s)==0 && S_ISDIR(s.st_mode); }

static void die(const char* msg) { fprintf(stderr,"[secondary-build] error: %s\n",msg); exit(1); }
static void* xrealloc(void* p,size_t n){ void* q=realloc(p,n); if(!q)die("out of memory"); return q; }
static char* xstrdup(const char* s){ size_t n=strlen(s)+1; char* p=(char*)malloc(n); if(!p)die("out of memory"); memcpy(p,s,n); return p; }

static void path_join(char* out,size_t cap,const char* a,const char* b){
  const size_t an=strlen(a),bn=strlen(b);
  const int sep=an && a[an-1]!='/' && a[an-1]!='\\';
  const size_t need=an+(sep?1u:0u)+bn+1u;
  if(!cap)return;
  if(need>cap){out[0]=0;return;}
  memcpy(out,a,an);
  size_t pos=an;
  if(sep)out[pos++]='/';
  memcpy(out+pos,b,bn);
  out[pos+bn]=0;
}
static const char* base_name(const char* p){ const char* a=strrchr(p,'/'); const char* b=strrchr(p,'\\'); const char* x=a>b?a:b; return x?x+1:p; }
static void dir_name(char* out,size_t cap,const char* p){ snprintf(out,cap,"%s",p); char* a=strrchr(out,'/'); char* b=strrchr(out,'\\'); char* x=a>b?a:b; if(x)*x=0; else snprintf(out,cap,"."); }
static const char* extension(const char* p){ const char* b=base_name(p); const char* d=strrchr(b,'.'); return d?d:""; }
static int ieq(const char* a,const char* b){ while(*a&&*b){ if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++))return 0; } return *a==0&&*b==0; }
static void stem(char* out,size_t cap,const char* p){ snprintf(out,cap,"%s",base_name(p)); char* d=strrchr(out,'.'); if(d)*d=0; }

static int mkdirs(const char* path){
  char tmp[MAX_PATHBUF]; size_t n=strlen(path); if(n>=sizeof(tmp))return 0; memcpy(tmp,path,n+1);
  for(size_t i=1;i<n;i++) if(tmp[i]=='/'||tmp[i]=='\\'){
#ifdef _WIN32
    if(i==2 && tmp[1]==':') continue;
#endif
    char c=tmp[i]; tmp[i]=0; if(*tmp && !is_dir(tmp) && MKDIR(tmp)!=0 && errno!=EEXIST)return 0; tmp[i]=c;
  }
  if(!is_dir(tmp) && MKDIR(tmp)!=0 && errno!=EEXIST)return 0;
  return 1;
}
static int copy_file(const char* src,const char* dst){
  FILE* a=fopen(src,"rb"); if(!a)return 0; char dd[MAX_PATHBUF]; dir_name(dd,sizeof(dd),dst); mkdirs(dd);
  FILE* b=fopen(dst,"wb"); if(!b){fclose(a);return 0;} unsigned char buf[65536]; size_t n; int ok=1;
  while((n=fread(buf,1,sizeof(buf),a))){ if(fwrite(buf,1,n,b)!=n){ok=0;break;} } if(ferror(a))ok=0; fclose(a); if(fclose(b)!=0)ok=0; return ok;
}
static unsigned char* read_file(const char* p,size_t* n){ FILE* f=fopen(p,"rb"); if(!f)return NULL; fseek(f,0,SEEK_END); long z=ftell(f); if(z<0){fclose(f);return NULL;} rewind(f); unsigned char* d=(unsigned char*)malloc((size_t)z+1); if(!d){fclose(f);return NULL;} *n=fread(d,1,(size_t)z,f); fclose(f); if(*n!=(size_t)z){free(d);return NULL;} d[*n]=0; return d; }

// Preserve generated-source timestamps on identical output so Ninja can reuse
// the compiled glue when only a finished side module has been removed.
static int finish_generated_file(FILE* generated,const char* path){
  if(ferror(generated)||fflush(generated)){fclose(generated);return 0;}
  rewind(generated);
  FILE* old=fopen(path,"rb");int same=old!=NULL;
  unsigned char a[65536],b[65536];size_t n;
  while(same&&(n=fread(a,1,sizeof(a),generated))){
    if(fread(b,1,n,old)!=n||memcmp(a,b,n))same=0;
  }
  if(same&&(fgetc(old)!=EOF||ferror(old)||ferror(generated)))same=0;
  if(old)fclose(old);
  if(same){fclose(generated);return 1;}
  rewind(generated);FILE* out=fopen(path,"wb");
  if(!out){fclose(generated);return 0;}
  int ok=1;
  while((n=fread(a,1,sizeof(a),generated)))if(fwrite(a,1,n,out)!=n){ok=0;break;}
  if(ferror(generated))ok=0;
  if(fclose(out))ok=0;
  fclose(generated);return ok;
}

static void cv_push(CandidateVec* a,const char* p,CandidateKind k){ if(a->n==a->cap){a->cap=a->cap?a->cap*2:32;a->v=(Candidate*)xrealloc(a->v,a->cap*sizeof(*a->v));} snprintf(a->v[a->n].path,sizeof(a->v[a->n].path),"%s",p);a->v[a->n].kind=k;a->n++; }
static void sv_push(StrVec* a,const char* s){ if(a->n==a->cap){a->cap=a->cap?a->cap*2:16;a->v=(char**)xrealloc(a->v,a->cap*sizeof(*a->v));}a->v[a->n++]=xstrdup(s); }
static void sv_push_unique(StrVec* a,const char* s){ for(size_t i=0;i<a->n;i++)if(!strcmp(a->v[i],s))return;sv_push(a,s); }
static void rv_push(RangeVec* a,uint32_t s,uint32_t e){ if(s>=e)return; if(a->n==a->cap){a->cap=a->cap?a->cap*2:16;a->v=(Range*)xrealloc(a->v,a->cap*sizeof(*a->v));}a->v[a->n++]=(Range){s,e}; }
static void u32v_push(U32Vec* a,uint32_t value){ if(a->n==a->cap){a->cap=a->cap?a->cap*2:64;a->v=(uint32_t*)xrealloc(a->v,a->cap*sizeof(*a->v));}a->v[a->n++]=value; }
static int u32_cmp(const void* A,const void* B){ const uint32_t a=*(const uint32_t*)A,b=*(const uint32_t*)B; return a<b?-1:a>b; }
static int range_cmp(const void* A,const void* B){ const Range* a=(const Range*)A,*b=(const Range*)B; return a->start<b->start?-1:a->start>b->start?1:a->end<b->end?-1:a->end>b->end; }
static int cand_cmp(const void* A,const void* B){ const Candidate* a=(const Candidate*)A,*b=(const Candidate*)B;
#ifdef _WIN32
  return _stricmp(a->path,b->path);
#else
  return strcasecmp(a->path,b->path);
#endif
}

static uint64_t fnv_bytes(uint64_t h,const void* data,size_t n){ const unsigned char* p=(const unsigned char*)data; for(size_t i=0;i<n;i++){h^=p[i];h*=1099511628211ull;} return h; }
static uint64_t fnv_file(uint64_t h,const char* p){ FILE* f=fopen(p,"rb"); if(!f)return fnv_bytes(h,p,strlen(p)); unsigned char b[65536]; size_t n; while((n=fread(b,1,sizeof(b),f)))h=fnv_bytes(h,b,n);fclose(f);return h; }
static int fixed_chunk_hashes(const char* dol_path,const RangeVec* chunks,uint64_t* hashes){
  if(!dol_path||!chunks||!hashes)return 0;
  size_t n=0;unsigned char* d=read_file(dol_path,&n);if(!d||n<0x100){free(d);return 0;}
  for(size_t c=0;c<chunks->n;c++){
    const Range r=chunks->v[c];int found=0;
    for(int i=0;i<7;i++){
      const uint32_t off=be32(d+i*4),addr=be32(d+0x48+i*4),sz=be32(d+0x90+i*4);
      const uint64_t end=(uint64_t)addr+sz;
      if(!off||!addr||!sz||r.start<addr||(uint64_t)r.end>end)continue;
      const size_t roff=(size_t)off+(size_t)(r.start-addr),rsize=(size_t)(r.end-r.start);
      if(roff>n||rsize>n-roff){free(d);return 0;}
      hashes[c]=fnv_bytes(0xcbf29ce484222325ull,d+roff,rsize);found=1;break;
    }
    if(!found){free(d);return 0;}
  }
  free(d);return 1;
}

static int valid_dol(const char* p){ size_t n=0; unsigned char* d=read_file(p,&n); if(!d||n<0x100){free(d);return 0;} uint32_t entry=be32(d+0xe0); int used=0,hit=0;
  for(int i=0;i<18;i++){uint32_t off=be32(d+i*4),addr=be32(d+0x48+i*4),sz=be32(d+0x90+i*4); if(!sz)continue;used++; if(off<0x100u||(uint64_t)off+sz>n||addr<0x80000000u||(uint64_t)addr+sz>0x81800000ull){free(d);return 0;} if(i<7&&entry>=addr&&entry<addr+sz)hit=1;} free(d); return used&&hit; }
static int valid_ppc_elf(const char* p){ size_t n=0; unsigned char* d=read_file(p,&n); int ok=d&&n>=0x34&&!memcmp(d,"\x7f""ELF",4)&&d[4]==1&&d[5]==2&&be16(d+0x12)==20&&be16(d+0x10)==2;free(d);return ok; }

// __OSReboot reads the apploader trailer after header.size + 0x20 and jumps
// to 0x81300000. It is another fixed executable, even though it has no .dol
// header or FST entry. The trailer then reloads/calls the loader at 0x81200000.
// Wrap both sets of unchanged disc bytes for the normal AOT
// pipeline; live instruction hashes still gate execution in the runtime.
static int discover_apploader_reboot(CandidateVec* out,const char* root,const char* output){
  char source[MAX_PATHBUF];path_join(source,sizeof(source),root,"sys/apploader.img");
  size_t n=0;unsigned char* data=read_file(source,&n);
  if(!data)return 1;
  if(n<0x20u){free(data);return 1;}
  const uint32_t entry=be32(data+0x10),loader_size=be32(data+0x14),reboot_size=be32(data+0x18);
  const uint64_t offset=0x20ull+loader_size;
  if(!loader_size||!reboot_size||(reboot_size&3u)||
     entry<0x81200000u||(uint64_t)entry>=0x81200000ull+loader_size||
     offset>n||reboot_size>n-offset||reboot_size>0x00500000u||
     (loader_size&3u)||loader_size>0x00100000u){free(data);return 1;}
  const uint64_t hash=fnv_bytes(1469598103934665603ull,data+0x20,loader_size+reboot_size);
  char directory[MAX_PATHBUF],name[80],staged[MAX_PATHBUF];
  path_join(directory,sizeof(directory),output,"apploader-reboot");
  if(!mkdirs(directory)){free(data);return 0;}
  snprintf(name,sizeof(name),"reboot-%016llx.dol",(unsigned long long)hash);
  path_join(staged,sizeof(staged),directory,name);
  unsigned char header[0x100]={0};
  put_be32(header,0x100u);put_be32(header+0x48,0x81200000u);
  put_be32(header+0x90,loader_size);
  put_be32(header+4,0x100u+loader_size);put_be32(header+0x4c,0x81300000u);
  put_be32(header+0x94,reboot_size);put_be32(header+0xe0,0x81300000u);
  FILE* f=tmpfile();
  if(!f){free(data);return 0;}
  const int written=fwrite(header,1,sizeof(header),f)==sizeof(header)&&
      fwrite(data+0x20,1,loader_size+reboot_size,f)==loader_size+reboot_size;
  free(data);
  if(!written){fclose(f);return 0;}
  if(!finish_generated_file(f,staged)||!valid_dol(staged))return 0;
  cv_push(out,staged,CAND_DOL);
  printf("GEKKOAOT_APPLOADER_REBOOT_AOT_V249=1 source=\"%s\" loader_bytes=%u reboot_bytes=%u addresses=81200000,81300000 policy=disc-bytes+fixed-image-proof\n",source,loader_size,reboot_size);
  return 1;
}

static int dol_rel24_bounds(const char* p,uint32_t* code_low,uint32_t* occupied_end){
  size_t n=0;unsigned char* d=read_file(p,&n);if(!d||n<0x100){free(d);return 0;}
  uint32_t low=0xffffffffu,high=0u;
  for(int i=0;i<18;i++){
    const uint32_t addr=be32(d+0x48+i*4),size=be32(d+0x90+i*4);
    if(!size)continue;
    const uint64_t end=(uint64_t)addr+size;
    if(addr<0x80000000u||end>0xffffffffull){free(d);return 0;}
    if(i<7&&addr<low)low=addr;
    if(end>high)high=(uint32_t)end;
  }
  const uint32_t bss_addr=be32(d+0xd8),bss_size=be32(d+0xdc);
  if(bss_size){
    const uint64_t bss_end=(uint64_t)bss_addr+bss_size;
    if(bss_addr<0x80000000u||bss_end>0xffffffffull){free(d);return 0;}
    if(bss_end>high)high=(uint32_t)bss_end;
  }
  free(d);if(low==0xffffffffu||!high)return 0;*code_low=low;*occupied_end=high;return 1;
}

/* Shared dense alignment of a REL set, or 0 when one REL cannot be packed. */
static uint32_t rel_dense_alignment(const RelVec* rels){
  uint32_t align=REL_DENSE_ALIGN;
  for(size_t i=0;i<rels->n;i++){
    const RelInfo* r=&rels->v[i];
    if(r->alignment>align)align=r->alignment;
    if(r->bss_alignment&(r->bss_alignment-1u)||r->bss_alignment>REL_ALIGN)return 0;
    if(r->bss_alignment>align)align=r->bss_alignment;
  }
  return align;
}
/* The cursor after a REL placed at `base`: the end of its file image and BSS
   or of its last section (BSS follows fix_size at its own alignment, as in
   assign_rel_sections), whichever is later, at the shared alignment. This is
   DolRecomp's next_rel_base with DOLRECOMP_REL_ALIGN. */
static uint32_t rel_dense_end(const RelInfo* r,uint32_t base){
  uint64_t end=(uint64_t)base+r->file_size+r->bss_size;
  uint32_t bss=align_up(base+r->fix_size,r->bss_alignment);
  for(uint32_t i=0;i<r->section_count;i++){
    const RelSection* s=&r->sections[i];
    if(!s->size)continue;
    const uint64_t at=s->bss?bss:(uint64_t)base+s->offset;
    if(s->bss)bss=align_up(bss+s->size,r->bss_alignment);
    if(at+s->size>end)end=at+s->size;
  }
  if(end>0xffffffffull)return 0;
  return align_up((uint32_t)end,g_rel_dense_align);
}
static uint32_t rel_layout_end_dense(const RelVec* rels,uint32_t base,int dense){
  uint32_t cursor=base;
  for(size_t i=0;i<rels->n;i++){
    const RelInfo* r=&rels->v[i];
    if(!dense){
      const uint64_t end=(uint64_t)cursor+r->file_size+r->bss_size;
      if(end>0xffffffffull)return 0;
      cursor=align_up((uint32_t)end,REL_ALIGN);
    } else {
      cursor=rel_dense_end(r,cursor);
    }
    if(!cursor)return 0;
  }
  return cursor;
}
static uint32_t rel_layout_end_at(const RelVec* rels,uint32_t base){
  return rel_layout_end_dense(rels,base,g_rel_dense_layout);
}

static uint32_t choose_rel_canonical_base_mode(const RelVec* rels,const char* boot_dol,int* fits);
static uint32_t choose_rel_canonical_base(const RelVec* rels,const char* boot_dol){
  int fits=0;
  g_rel_dense_layout=0;
  const uint32_t base=choose_rel_canonical_base_mode(rels,boot_dol,&fits);
  if(fits)return base;
  g_rel_dense_align=rel_dense_alignment(rels);
  if(!g_rel_dense_align)return base;
  g_rel_dense_layout=1;
  const uint32_t dense=choose_rel_canonical_base_mode(rels,boot_dol,&fits);
  if(fits){
    printf("GEKKOAOT_REL24_DENSE_LAYOUT_V364=1 rel_base=0x%08X rel_end=0x%08X rels=%zu align=0x%X policy=shared-alignment-packing\n",
           dense,rel_layout_end_at(rels,dense),rels->n,g_rel_dense_align);
    return dense;
  }
  g_rel_dense_layout=0;
  return base;
}
static uint32_t choose_rel_canonical_base_mode(const RelVec* rels,const char* boot_dol,int* fits){
  *fits=0;
  const uint32_t default_end=rel_layout_end_at(rels,REL_BASE);
  if(!default_end)die("REL canonical address overflow");
  uint32_t dol_code_low=0u,dol_occupied_end=0u;
  if(!boot_dol||!dol_rel24_bounds(boot_dol,&dol_code_low,&dol_occupied_end)){*fits=1;return REL_BASE;}

  const uint32_t min_base=align_up(dol_occupied_end,REL_ALIGN);
  if(!min_base)die("REL canonical minimum base overflow");
  const uint64_t rel_span=(uint64_t)default_end-REL_BASE;
  const uint64_t window_limit=(uint64_t)dol_code_low+REL24_WINDOW_SIZE-REL24_WINDOW_GUARD;
  if(window_limit<=rel_span){
    fprintf(stderr,"GEKKOAOT_REL24_AUTO_LAYOUT=0 reason=span-too-large dol_code_low=%08x rel_span=%llu\n",
            dol_code_low,(unsigned long long)rel_span);
    return REL_BASE;
  }

  const uint32_t max_base=align_down((uint32_t)(window_limit-rel_span),REL_ALIGN);
  if(max_base<min_base){
    fprintf(stderr,"GEKKOAOT_REL24_AUTO_LAYOUT=0 reason=no-safe-window dol_code_low=%08x dol_end=%08x min_base=%08x max_base=%08x rel_span=%llu\n",
            dol_code_low,dol_occupied_end,min_base,max_base,(unsigned long long)rel_span);
    return REL_BASE;
  }

  uint32_t chosen=REL_BASE;
  if(chosen<min_base)chosen=min_base;
  if(chosen>max_base)chosen=max_base;
  const uint32_t chosen_end=rel_layout_end_at(rels,chosen);
  if(!chosen_end||(uint64_t)chosen_end>window_limit){
    fprintf(stderr,"GEKKOAOT_REL24_AUTO_LAYOUT=0 reason=verification-failed base=%08x end=%08x limit=%08llx\n",
            chosen,chosen_end,(unsigned long long)window_limit);
    return REL_BASE;
  }
  if(chosen!=REL_BASE)
    printf("GEKKOAOT_REL24_AUTO_LAYOUT=1 rel_base=0x%08X default=0x%08X dol_code_low=0x%08X dol_end=0x%08X rel_end=0x%08X policy=fit-dol+rel-in-rel24-window\n",
           chosen,REL_BASE,dol_code_low,dol_occupied_end,chosen_end);
  *fits=1;
  return chosen;
}

static int parse_rel(const char* p,RelInfo* r){ memset(r,0,sizeof(*r)); size_t n=0; unsigned char* d=read_file(p,&n); if(!d||n<0x40){free(d);return 0;}
  uint32_t id=be32(d), sc=be32(d+0x0c), si=be32(d+0x10), ver=be32(d+0x1c); if(!id||!sc||sc>MAX_REL_SECTIONS||ver<1u||ver>3u||si<0x40u||(si&3u)||(uint64_t)si+sc*8ull>n){free(d);return 0;}
  RelSection* secs=(RelSection*)calloc(sc,sizeof(*secs)); if(!secs){free(d);return 0;} int have_exec=0;
  for(uint32_t i=0;i<sc;i++){uint32_t raw=be32(d+si+i*8),sz=be32(d+si+i*8+4),off=raw&~1u; int ex=(raw&1u)!=0,bss=off==0&&sz!=0; if((!bss&&sz&&(uint64_t)off+sz>n)||(ex&&sz&&((off&3u)||(sz&3u)||bss))){free(secs);free(d);return 0;} secs[i]=(RelSection){i,off,sz,ex,bss,0}; if(ex&&sz&&!bss)have_exec=1;}
  if(!have_exec){free(secs);free(d);return 0;} snprintf(r->path,sizeof(r->path),"%s",p);r->module_id=id;r->version=ver;r->section_count=sc;r->section_info=si;r->file_size=(uint32_t)n;r->bss_size=be32(d+0x20);r->bss_alignment=n>=0x48?be32(d+0x44):4;r->fix_size=(ver>=3&&n>=0x4c)?be32(d+0x48):(uint32_t)n; if(!r->bss_alignment)r->bss_alignment=4; r->alignment=(ver>=2&&n>=0x44)?be32(d+0x40):4u; if(!r->alignment||(r->alignment&(r->alignment-1u))||r->alignment>REL_ALIGN)r->alignment=REL_ALIGN; if(!r->fix_size||r->fix_size>n){free(secs);free(d);return 0;}r->sections=secs;free(d);return 1; }
static void free_rel(RelInfo* r){free(r->sections);r->sections=NULL;}
static void assign_rel_sections(RelInfo* r){ uint32_t bss=align_up(r->base+r->fix_size,r->bss_alignment); for(uint32_t i=0;i<r->section_count;i++){RelSection* s=&r->sections[i]; if(!s->size)continue; if(s->bss){s->linked=bss;bss=align_up(bss+s->size,r->bss_alignment);}else s->linked=r->base+s->offset;} }

static unsigned char* decode_yaz0(const unsigned char* source,size_t size,size_t* decoded_size){
  if(size<16||memcmp(source,"Yaz0",4))return NULL;
  const size_t target=be32(source+4);
  if(!target||target>64u*1024u*1024u)return NULL;
  unsigned char* decoded=(unsigned char*)malloc(target);
  if(!decoded)return NULL;
  size_t input=16,output=0;
  while(output<target){
    if(input>=size)goto invalid;
    const unsigned control=source[input++];
    for(unsigned bit=0;bit<8&&output<target;bit++){
      if(control&(0x80u>>bit)){
        if(input>=size)goto invalid;
        decoded[output++]=source[input++];
      }else{
        if(size-input<2)goto invalid;
        const unsigned first=source[input++],second=source[input++];
        const size_t distance=((first&15u)<<8|second)+1u;
        size_t length=first>>4;
        if(length)length+=2;
        else{if(input>=size)goto invalid;length=source[input++]+18u;}
        if(distance>output||length>target-output)goto invalid;
        // Overlapping copies deliberately read bytes just produced.
        for(size_t j=0;j<length;j++){decoded[output]=decoded[output-distance];output++;}
      }
    }
  }
  *decoded_size=target;return decoded;
 invalid:free(decoded);return NULL;
}

static char g_decoded_rel_root[MAX_PATHBUF];
static int discover_compressed_rel(CandidateVec* out,const char* path){
  if(!g_decoded_rel_root[0])return 0;
  unsigned char signature[4];FILE* probe=fopen(path,"rb");if(!probe)return 0;
  const int compressed=fread(signature,1,4,probe)==4&&!memcmp(signature,"Yaz0",4);fclose(probe);
  if(!compressed)return 0;
  size_t size=0,decoded_size=0;unsigned char* source=read_file(path,&size);
  if(!source)return 0;
  unsigned char* decoded=decode_yaz0(source,size,&decoded_size);
  const uint64_t hash=fnv_bytes(1469598103934665603ull,source,size);free(source);
  if(!decoded)return 0;
  char key[32],directory[MAX_PATHBUF],staged[MAX_PATHBUF];
  snprintf(key,sizeof(key),"%016llx",(unsigned long long)hash);
  path_join(directory,sizeof(directory),g_decoded_rel_root,key);
  if(!mkdirs(directory)){free(decoded);die("cannot create decoded REL cache");}
  path_join(staged,sizeof(staged),directory,"module.rel");
  for(size_t i=0;i<out->n;i++)if(!strcmp(out->v[i].path,staged)){free(decoded);return 1;}
  FILE* file=tmpfile();if(!file){free(decoded);die("cannot stage decoded REL");}
  const int written=fwrite(decoded,1,decoded_size,file)==decoded_size;free(decoded);
  if(!written){fclose(file);die("cannot write decoded REL");}
  if(!finish_generated_file(file,staged))die("cannot finish decoded REL");
  RelInfo rel;if(!parse_rel(staged,&rel))return 0;
  free_rel(&rel);cv_push(out,staged,CAND_REL);
  printf("GEKKOAOT_COMPRESSED_REL=1 format=Yaz0 bytes=%zu source=\"%s\"\n",decoded_size,path);
  return 1;
}

static void discover_file(CandidateVec* out,const char* p);
static void discover_rel_archive(CandidateVec* out,const char* path){
  if(!g_decoded_rel_root[0])return;
  size_t size=0;unsigned char* data=read_file(path,&size);if(!data)return;
  if(size>=4&&!memcmp(data,"Yaz0",4)){
    size_t decoded_size=0;unsigned char* decoded=decode_yaz0(data,size,&decoded_size);
    free(data);data=decoded;size=decoded_size;if(!data)return;
  }
  if(size<0x40||memcmp(data,"RARC",4)){free(data);return;}
  const uint64_t info=be32(data+8);
  if(info>size||size-info<0x20){free(data);return;}
  const uint64_t count=be32(data+info+8),entries=info+be32(data+info+12);
  const uint64_t string_size=be32(data+info+16),strings=info+be32(data+info+20);
  const uint64_t payload=info+be32(data+12);
  if(entries>size||count>(size-entries)/20||strings>size||string_size>size-strings||payload>size){free(data);return;}
  const uint64_t hash=fnv_bytes(1469598103934665603ull,data,size);
  char key[64],directory[MAX_PATHBUF];snprintf(key,sizeof(key),"archive-%016llx",(unsigned long long)hash);
  path_join(directory,sizeof(directory),g_decoded_rel_root,key);
  for(uint64_t i=0;i<count;i++){
    const unsigned char* entry=data+entries+i*20;
    const uint32_t flags_name=be32(entry+4);if((flags_name>>24)&2u)continue;
    const uint32_t name_offset=flags_name&0xffffffu;
    if(name_offset>=string_size)continue;
    const char* name=(const char*)data+strings+name_offset;
    if(!memchr(name,0,(size_t)(string_size-name_offset)))continue;
    const char* ext=extension(name);
    if(!ieq(ext,".rel")&&!ieq(ext,".bin")&&!ieq(ext,".dol")&&!ieq(ext,".elf"))continue;
    const uint64_t offset=payload+be32(entry+8),length=be32(entry+12);
    if(offset>size||length>size-offset)continue;
    if(!mkdirs(directory))die("cannot create REL archive cache");
    char file_name[64],staged[MAX_PATHBUF];
    snprintf(file_name,sizeof(file_name),"%llu%s",(unsigned long long)i,ext);
    path_join(staged,sizeof(staged),directory,file_name);
    FILE* file=tmpfile();if(!file)die("cannot stage archive executable");
    if(fwrite(data+offset,1,(size_t)length,file)!=(size_t)length){fclose(file);die("cannot write archive executable");}
    if(!finish_generated_file(file,staged))die("cannot finish archive executable");
    const size_t before=out->n;discover_file(out,staged);
    if(out->n>before)printf("GEKKOAOT_ARCHIVE_EXECUTABLE=1 format=RARC name=\"%s\" source=\"%s\"\n",name,path);
  }
  free(data);
}

static void discover_file(CandidateVec* out,const char* p){ const char* ext=extension(p); RelInfo r;
  if(ieq(ext,".arc")||ieq(ext,".szs")){discover_rel_archive(out,p);return;}
  if(ieq(ext,".rel") && parse_rel(p,&r)){free_rel(&r);cv_push(out,p,CAND_REL);return;}
  if((ieq(ext,".rel")||ieq(ext,".bin"))&&discover_compressed_rel(out,p))return;
  if(ieq(ext,".elf") && valid_ppc_elf(p)){cv_push(out,p,CAND_ELF);return;}
  if(ieq(ext,".dol") && valid_dol(p)){cv_push(out,p,CAND_DOL);return;}
  if(ieq(ext,".bin")){if(valid_ppc_elf(p)){cv_push(out,p,CAND_ELF);return;} if(valid_dol(p)){cv_push(out,p,CAND_DOL);return;} if(parse_rel(p,&r)){free_rel(&r);cv_push(out,p,CAND_REL);return;}}
}
static void scan_tree(CandidateVec* out,const char* root){
#ifdef _WIN32
  char pat[MAX_PATHBUF];path_join(pat,sizeof(pat),root,"*");WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA(pat,&fd);if(h==INVALID_HANDLE_VALUE)return;do{if(!strcmp(fd.cFileName,".")||!strcmp(fd.cFileName,".."))continue;char p[MAX_PATHBUF];path_join(p,sizeof(p),root,fd.cFileName);if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)scan_tree(out,p);else discover_file(out,p);}while(FindNextFileA(h,&fd));FindClose(h);
#else
  DIR* d=opendir(root);if(!d)return;struct dirent* e;while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char p[MAX_PATHBUF];path_join(p,sizeof(p),root,e->d_name);struct stat st;if(stat(p,&st))continue;if(S_ISDIR(st.st_mode))scan_tree(out,p);else if(S_ISREG(st.st_mode))discover_file(out,p);}closedir(d);
#endif
}
static void find_named(StrVec* out,const char* root,const char* name){
#ifdef _WIN32
  char pat[MAX_PATHBUF];path_join(pat,sizeof(pat),root,"*");WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA(pat,&fd);if(h==INVALID_HANDLE_VALUE)return;do{if(!strcmp(fd.cFileName,".")||!strcmp(fd.cFileName,".."))continue;char p[MAX_PATHBUF];path_join(p,sizeof(p),root,fd.cFileName);if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)find_named(out,p,name);else if(!strcmp(fd.cFileName,name))sv_push(out,p);}while(FindNextFileA(h,&fd));FindClose(h);
#else
  DIR* d=opendir(root);if(!d)return;struct dirent* e;while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char p[MAX_PATHBUF];path_join(p,sizeof(p),root,e->d_name);struct stat st;if(stat(p,&st))continue;if(S_ISDIR(st.st_mode))find_named(out,p,name);else if(S_ISREG(st.st_mode)&&!strcmp(e->d_name,name))sv_push(out,p);}closedir(d);
#endif
}

static void qappend(char* out,size_t cap,const char* p){ size_t n=strlen(out); if(n+4>=cap)die("command too long"); out[n++]=' ';out[n++]='"';for(const char* s=p;*s&&n+3<cap;s++){if(*s=='"')out[n++]='\\';out[n++]=*s;}out[n++]='"';out[n]=0; }
static int run_cmd(const char* cmd){printf("$ %s\n",cmd);fflush(stdout);
#ifdef _WIN32
  // cmd /c strips the first and last quotes when a command starts with a
  // quoted executable and also has quoted arguments. Supply an outer pair
  // so the executable and argument quotes survive that shell processing.
  const char* first=cmd;while(isspace((unsigned char)*first))first++;
  char* wrapped=NULL;
  if(*first=='"'){
    size_t length=strlen(first);wrapped=(char*)malloc(length+3);
    if(!wrapped)return 127;
    wrapped[0]='"';memcpy(wrapped+1,first,length);wrapped[length+1]='"';wrapped[length+2]=0;
  }
  int rc=system(wrapped?wrapped:cmd);free(wrapped);
  return rc;
#else
  int rc=system(cmd);
  if(rc==-1)return 127;
  if(WIFEXITED(rc))return WEXITSTATUS(rc);
  if(WIFSIGNALED(rc))return 128+WTERMSIG(rc);
  return rc;
#endif
}

static unsigned env_u32(const char* name,unsigned fallback,unsigned minv,unsigned maxv){
  const char* text=getenv(name);if(!text||!*text)return fallback;
  char* end=NULL;errno=0;unsigned long value=strtoul(text,&end,10);
  if(errno||!end||*end||value<minv||value>maxv)return fallback;
  return (unsigned)value;
}

// Hash once per invocation, not once per REL. Cached finished modules must
// describe the compiler and ABI that produced them as well as disc bytes.
static uint64_t secondary_toolchain_hash(const char* compiler,const char* dolsrc,const char* root){
  uint64_t h=fnv_bytes(1469598103934665603ull,GEKKOAOT_SECONDARY_SOURCE_HASH,
                       strlen(GEKKOAOT_SECONDARY_SOURCE_HASH));
  h=fnv_file(h,compiler);
  const char* sources[]={"src/cpu/cpu.c","src/cpu/cpu.h","src/common/types.h"};
  char path[MAX_PATHBUF];
  for(size_t i=0;i<sizeof(sources)/sizeof(sources[0]);++i){
    path_join(path,sizeof(path),dolsrc,sources[i]);h=fnv_file(h,path);
  }
  path_join(path,sizeof(path),root,"runtime/core/module_abi.h");h=fnv_file(h,path);
  path_join(path,sizeof(path),root,"runtime/module/secondary_dispatch_cache.h");h=fnv_file(h,path);
  path_join(path,sizeof(path),root,"runtime/core/native_hardware.h");h=fnv_file(h,path);
  const unsigned options[]={
    env_u32("GEKKOAOT_AOT_CHUNK_INSTRUCTIONS",1024,64,4096),
    env_u32("GEKKOAOT_AOT_GUARD_CYCLES",65536,256,1048576),
    env_u32("GEKKOAOT_AOT_GUARD_STEPS",262144,2048,1048576),
    env_u32("GEKKOAOT_AOT_POLL_GUARD_CYCLES",128,16,8192),
    env_u32("GEKKOAOT_NATIVE_INLINE_BYTES",1024,64,4096),
    env_u32("GEKKOAOT_NATIVE_INLINE_HINT_BYTES",4096,256,4096),
    env_u32("GEKKOAOT_RELAXED_CALL_GUARD_BYTES",512,64,4096),
    env_u32("GEKKOAOT_LLVM_CODEGEN_LEVEL",2,0,3),
    env_u32("GEKKOAOT_SECONDARY_LLVM_IR_LEVEL",2,0,3),
    env_u32("GEKKOAOT_LLVM_WORKER_BATCH",4,1,64),
    env_u32("GEKKOAOT_LLVM_COALESCE_SIZE",100,1,4096),
    env_u32("GEKKOAOT_LLVM_COALESCE_LIMIT",32,1,4096),
    env_u32("GEKKOAOT_LLVM_SLP_MAX_INSTRUCTIONS",8192,64,1048576),
    env_u32("GEKKOAOT_NATIVE_ADDRESS_LOWERING",1,0,1),
    env_u32("GEKKOAOT_NATIVE_FLAT_VMEM",1,0,1),
    env_u32("GEKKOAOT_NATIVE_LOCKED_CACHE",1,0,1),
    env_u32("GEKKOAOT_FP_REGION_COALESCE",1,0,1),
    sizeof(void*)
  };
  h=fnv_bytes(h,options,sizeof(options));
  const char* vars[]={"CC","CFLAGS","LDFLAGS","DOLRECOMP_LLVM_PGO",
                      "DOLRECOMP_LLVM_PROFILE","DOLRECOMP_LLVM_TARGET",
                      "DOLRECOMP_LLVM_CPU","DOLRECOMP_LLVM_FEATURES","CMAKE_TOOLCHAIN_FILE"};
  for(size_t i=0;i<sizeof(vars)/sizeof(vars[0]);++i){
    const char* value=getenv(vars[i]);
    h=fnv_bytes(h,vars[i],strlen(vars[i])+1);
    if(value)h=fnv_bytes(h,value,strlen(value)+1);
  }
  const char* profile=getenv("DOLRECOMP_LLVM_PROFILE");
  if(profile&&*profile)h=fnv_file(h,profile);
  // v274: the probe-intercept manifest decides which entries keep runtime
  // admission probes, so its contents are part of the module identity.
  const char* intercepts=getenv("GEKKOAOT_SECONDARY_STATIC_INTERCEPTS");
  if(intercepts&&*intercepts){
    h=fnv_bytes(h,"probe-intercepts-v274",sizeof("probe-intercepts-v274"));
    h=fnv_file(h,intercepts);
  }
  return h;
}
static uint64_t g_secondary_toolchain_hash;
static char g_rel_pointer_manifest[MAX_PATHBUF];

static void append_env(char* cmd,size_t cap,const char* name,const char* value){
  char item[MAX_PATHBUF+128];
  if(snprintf(item,sizeof(item),"%s=%s",name,value)>=(int)sizeof(item))die("environment item too long");
  qappend(cmd,cap,item);
}

static void make_secondary_llvm_command(char* cmd,size_t cap,const char* dolrecomp,
                                        const char* output,int jobs,int rel_mode){
  const unsigned chunk=env_u32("GEKKOAOT_AOT_CHUNK_INSTRUCTIONS",1024u,64u,4096u);
  const unsigned cycles=env_u32("GEKKOAOT_AOT_GUARD_CYCLES",65536u,256u,1048576u);
  const unsigned steps=env_u32("GEKKOAOT_AOT_GUARD_STEPS",262144u,2048u,1048576u);
  const unsigned poll_cycles=env_u32("GEKKOAOT_AOT_POLL_GUARD_CYCLES",128u,16u,8192u);
  const unsigned inline_bytes=env_u32("GEKKOAOT_NATIVE_INLINE_BYTES",1024u,64u,4096u);
  const unsigned inline_hint=env_u32("GEKKOAOT_NATIVE_INLINE_HINT_BYTES",4096u,256u,4096u);
  const unsigned relaxed=env_u32("GEKKOAOT_RELAXED_CALL_GUARD_BYTES",512u,64u,4096u);
  const unsigned ir_level=env_u32("GEKKOAOT_SECONDARY_LLVM_IR_LEVEL",2u,0u,3u);
  const unsigned worker_batch=env_u32("GEKKOAOT_LLVM_WORKER_BATCH",4u,1u,64u);
  char cache[MAX_PATHBUF],num[64];
  // The compiler binary fingerprints finished modules, but the LLVM object
  // cache also needs a new namespace when IR semantics change.
  const char* cache_name=rel_mode ? "llvm-cache-v14-lc308-flat304-address287-hostfilter278-materialize222-cfgaddr221-xmodgpr207-halfwordreverse45" : "llvm-cache-v7-lc308-flat304-address287-hostfilter278-halfwordreverse45";
  // v274: DolRecomp's per-object cache key does not cover the interception
  // manifest, so objects built with a different manifest get their own cache.
  const char* probe_intercepts=getenv("GEKKOAOT_SECONDARY_STATIC_INTERCEPTS");
  char cache_dir_name[256];
  if(probe_intercepts&&*probe_intercepts){
    snprintf(cache_dir_name,sizeof(cache_dir_name),"%s-base%u-flat304-%u-lc308-%u-fpregion292-%u-probe274-%016llx",cache_name,
             env_u32("GEKKOAOT_NATIVE_ADDRESS_LOWERING",1,0,1),
             env_u32("GEKKOAOT_NATIVE_FLAT_VMEM",1,0,1),
             env_u32("GEKKOAOT_NATIVE_LOCKED_CACHE",1,0,1),
             env_u32("GEKKOAOT_FP_REGION_COALESCE",1,0,1),
             (unsigned long long)fnv_file(1469598103934665603ull,probe_intercepts));
  } else {
    snprintf(cache_dir_name,sizeof(cache_dir_name),"%s-base%u-flat304-%u-lc308-%u-fpregion292-%u",cache_name,
             env_u32("GEKKOAOT_NATIVE_ADDRESS_LOWERING",1,0,1),
             env_u32("GEKKOAOT_NATIVE_FLAT_VMEM",1,0,1),
             env_u32("GEKKOAOT_NATIVE_LOCKED_CACHE",1,0,1),
             env_u32("GEKKOAOT_FP_REGION_COALESCE",1,0,1));
  }
  path_join(cache,sizeof(cache),output,cache_dir_name);
  if(!mkdirs(cache))die("cannot create secondary LLVM cache");

  snprintf(cmd,cap,"cmake -E env");
  append_env(cmd,cap,"DOLRECOMP_LLVM_CACHE",cache);
  if(rel_mode&&g_rel_pointer_manifest[0])
    append_env(cmd,cap,"DOLRECOMP_REL_POINTER_MANIFEST",g_rel_pointer_manifest);
  if(rel_mode&&g_rel_dense_layout){
    snprintf(num,sizeof(num),"0x%X",g_rel_dense_align);
    append_env(cmd,cap,"DOLRECOMP_REL_ALIGN",num);
  }
  append_env(cmd,cap,"DOLRECOMP_DISPATCH_LOOKUP","indexed");
  append_env(cmd,cap,"DOLRECOMP_FP_REGION_COALESCE",
             env_u32("GEKKOAOT_FP_REGION_COALESCE",1,0,1) ? "1" : "0");
  append_env(cmd,cap,"DOLRECOMP_NATIVE_ADDRESS_LOWERING",
             env_u32("GEKKOAOT_NATIVE_ADDRESS_LOWERING",1,0,1) ? "1" : "0");
  append_env(cmd,cap,"DOLRECOMP_NATIVE_FLAT_VMEM",
             env_u32("GEKKOAOT_NATIVE_FLAT_VMEM",1,0,1) ? "1" : "0");
  append_env(cmd,cap,"DOLRECOMP_NATIVE_LOCKED_CACHE",
             env_u32("GEKKOAOT_NATIVE_LOCKED_CACHE",1,0,1) ? "1" : "0");
  // v274: address-only interception manifest (see gekkoaotctl
  // BuildSecondaryModules); entries without an intercept skip the runtime
  // admission probe that DolRecomp emits when no manifest is present.
  if(probe_intercepts&&*probe_intercepts)
    append_env(cmd,cap,"DOLRECOMP_STATIC_INTERCEPTS",probe_intercepts);
  // v238 currently proves SDK clock identities in the primary DOL manifest.
  // Secondary modules retain interception until they have their own proof.
  append_env(cmd,cap,"DOLRECOMP_NATIVE_SDK_CLOCK_LOWERING","0");
  append_env(cmd,cap,"DOLRECOMP_NATIVE_HARDWARE_LOWERING","0");
  append_env(cmd,cap,"DOLRECOMP_NATIVE_WGPIPE_TRY_LOWERING","0");
  snprintf(num,sizeof(num),"%u",chunk);append_env(cmd,cap,"DOLRECOMP_LLVM_CHUNK_INSTRUCTIONS",num);
  snprintf(num,sizeof(num),"%u",cycles);append_env(cmd,cap,"DOLRECOMP_GUARD_CYCLES",num);
  snprintf(num,sizeof(num),"%u",steps);append_env(cmd,cap,"DOLRECOMP_GUARD_STEPS",num);
  snprintf(num,sizeof(num),"%u",poll_cycles);append_env(cmd,cap,"DOLRECOMP_POLL_GUARD_CYCLES",num);
  snprintf(num,sizeof(num),"%u",env_u32("GEKKOAOT_LLVM_CODEGEN_LEVEL",2u,0u,3u));
  append_env(cmd,cap,"DOLRECOMP_LLVM_CODEGEN_LEVEL",num);
  snprintf(num,sizeof(num),"%u",ir_level);append_env(cmd,cap,"DOLRECOMP_LLVM_IR_LEVEL",num);
  snprintf(num,sizeof(num),"%u",worker_batch);append_env(cmd,cap,"DOLRECOMP_LLVM_WORKER_BATCH",num);
  append_env(cmd,cap,"DOLRECOMP_LLVM_EMIT_THINLTO","0");
  append_env(cmd,cap,"DOLRECOMP_FALLBACK_AUDIT","0");
  snprintf(num,sizeof(num),"%u",env_u32("GEKKOAOT_LLVM_COALESCE_SIZE",100u,1u,4096u));
  append_env(cmd,cap,"DOLRECOMP_LLVM_COALESCE_SIZE",num);
  snprintf(num,sizeof(num),"%u",env_u32("GEKKOAOT_LLVM_COALESCE_LIMIT",32u,1u,4096u));
  append_env(cmd,cap,"DOLRECOMP_LLVM_COALESCE_LIMIT",num);
  snprintf(num,sizeof(num),"%u",env_u32("GEKKOAOT_LLVM_SLP_MAX_INSTRUCTIONS",8192u,64u,1048576u));
  append_env(cmd,cap,"DOLRECOMP_LLVM_SLP_MAX_INSTRUCTIONS",num);
  snprintf(num,sizeof(num),"%u",inline_bytes);append_env(cmd,cap,"DOLRECOMP_NATIVE_INLINE_BYTES",num);
  snprintf(num,sizeof(num),"%u",inline_hint);append_env(cmd,cap,"DOLRECOMP_NATIVE_INLINE_HINT_BYTES",num);
  snprintf(num,sizeof(num),"%u",relaxed);append_env(cmd,cap,"DOLRECOMP_RELAXED_CALL_GUARD_BYTES",num);
  if(rel_mode){
    append_env(cmd,cap,"DOLRECOMP_REL_RUNTIME_TRANSLATE","1");
    // REL code is runtime-relocated and may be entered through function pointers
    // or jump-table targets that are not discoverable from static control flow.
    // Ask DolRecomp to retain a native entry label for every guest instruction
    // only for REL side modules; the main DOL keeps its optimized region shape.
    append_env(cmd,cap,"DOLRECOMP_REL_ALL_INSTRUCTION_ENTRYPOINTS","1");
  }
  qappend(cmd,cap,dolrecomp);

  char opts[512];
  snprintf(opts,sizeof(opts),
           " -j%d --gamecube --backend=llvm --cpu gekko --targets=host --semantics=exact --partition-instructions %u",
           jobs,chunk);
  strncat(cmd,opts,cap-strlen(cmd)-1);
  if(!rel_mode)
    strncat(cmd," --state-in-memory --native-abi=unrestricted",cap-strlen(cmd)-1);

  printf("GEKKOAOT_SECONDARY_LLVM_AOT=1 kind=%s chunk=%u cycles=%u steps=%u cache=\"%s\" rel-translate=%d rel-all-entrypoints=%d\n",
         rel_mode?"rel":"fixed",chunk,cycles,steps,cache,rel_mode?1:0,rel_mode?1:0);
  printf("GEKKOAOT_SECONDARY_LLVM_TURBO_V1=1 ir=%u worker_batch=%u thinlto=0 fallback_audit=0 target_machine_cache=1\n",
         ir_level,worker_batch);
}

static void module_suffix(char* out,size_t cap){
#ifdef _WIN32
 snprintf(out,cap,".dll");
#elif defined(__APPLE__)
 snprintf(out,cap,".dylib");
#else
 snprintf(out,cap,".so");
#endif
}
static void cmake_path(FILE* f,const char* s){ for(;*s;s++) fputc(*s=='\\'?'/' : *s,f); }
static void write_ranges(FILE* f,const char* name,const RangeVec* ranges){fprintf(f,"static const GekkoAOTRange %s[] = {\n",name);if(!ranges->n)fprintf(f,"  {0u,0u},\n");for(size_t i=0;i<ranges->n;i++)fprintf(f,"  {0x%08Xu,0x%08Xu},\n",ranges->v[i].start,ranges->v[i].end);fprintf(f,"};\n");}

static int write_secondary_dispatch_glue(FILE* e,const RangeVec* chunks){
  if(!e||!chunks||!chunks->n)return 0;
  fprintf(e,
    "// SPDX-License-Identifier: GPL-3.0-or-later\n"
    "#include <stddef.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
    "#include \"generated.h\"\n#include \"core/module_abi.h\"\n"
    "#include \"module/secondary_dispatch_cache.h\"\n"
    "#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))\n"
    "#include <cpuid.h>\n#endif\n\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "#define SECONDARY_LIKELY(x) __builtin_expect(!!(x),1)\n"
    "#define SECONDARY_UNLIKELY(x) __builtin_expect(!!(x),0)\n"
    "#else\n#define SECONDARY_LIKELY(x) (!!(x))\n#define SECONDARY_UNLIKELY(x) (!!(x))\n#endif\n\n"
    "typedef void (*SecondaryChunkFn)(CPUState*);\n"
    "typedef GekkoAOTChunkRoute SecondaryChunkRoute;\n"
    "static const SecondaryChunkRoute s_secondary_chunk_routes[] = {\n");
  for(size_t i=0;i<chunks->n;i++)
    fprintf(e,"  {0x%08Xu,0x%08Xu,func_%08X},\n",chunks->v[i].start,chunks->v[i].end,chunks->v[i].start);
  fprintf(e,"};\n");
  // A bounded dense text image trades <=8 MiB read-only storage for direct
  // instruction-to-native-chunk lookup. Sparse/large/unaligned tables retain
  // the existing page/cache paths; no guest code identity check is bypassed.
  const uint32_t instruction_base=chunks->v[0].start;
  const uint64_t instruction_span=(uint64_t)chunks->v[chunks->n-1].end-instruction_base;
  int instruction_image=chunks->n<=65535u && instruction_span<=16u*1024u*1024u &&
                        !(instruction_span&3u) && !(instruction_base&3u);
  for(size_t i=0;instruction_image && i<chunks->n;i++)
    if((chunks->v[i].start&3u)||(chunks->v[i].end&3u)||
       (i && chunks->v[i-1].end>chunks->v[i].start))instruction_image=0;
  const uint32_t instruction_words=instruction_image?(uint32_t)(instruction_span/4u):0u;
  fprintf(e,"#define SECONDARY_INSTRUCTION_BASE 0x%08Xu\n#define SECONDARY_INSTRUCTION_WORDS %uu\n",instruction_base,instruction_words);
  fprintf(e,"static const uint16_t s_secondary_instruction_routes[] = {\n");
  if(instruction_image){
    size_t route=0;
    for(uint32_t word=0;word<instruction_words;word++){
      const uint32_t address=instruction_base+word*4u;
      while(route<chunks->n && chunks->v[route].end<=address)++route;
      const unsigned index=route<chunks->n && chunks->v[route].start<=address ? (unsigned)route+1u : 0u;
      fprintf(e,"%u,%s",index,(word&31u)==31u?"\n":"");
    }
  }else fputs("0,",e);
  fprintf(e,"\n};\n");
  // Immutable 4 KiB page -> small route interval. Unlike an instruction-sized
  // map this costs eight bytes per guest text page, not four bytes per opcode.
  const uint32_t first_page=chunks->v[0].start>>12;
  const uint32_t page_count=((chunks->v[chunks->n-1].end-1u)>>12)-first_page+1u;
  const int page_index=page_count<=16384u;
  fprintf(e,"#define SECONDARY_FIRST_PAGE %uu\n#define SECONDARY_PAGE_COUNT %uu\n",first_page,page_index?page_count:0u);
  fprintf(e,"static const GekkoAOTChunkPage s_secondary_chunk_pages[] = {\n");
  if(page_index){
    size_t lo=0,hi=0;
    for(uint32_t page=0;page<page_count;page++){
      const uint64_t start=(uint64_t)(first_page+page)<<12, end=start+4096u;
      while(lo<chunks->n && chunks->v[lo].end<=start)++lo;
      if(hi<lo)hi=lo;
      while(hi<chunks->n && chunks->v[hi].start<end)++hi;
      fprintf(e,"  {%zuu,%zuu},\n",lo,hi);
    }
  }else fprintf(e,"  {0u,0u},\n");
  fprintf(e,
    "};\n#define SECONDARY_CHUNK_ROUTE_COUNT ((uint32_t)(sizeof(s_secondary_chunk_routes)/sizeof(s_secondary_chunk_routes[0])))\n\n"
    "static inline SecondaryChunkFn secondary_find_chunk(uint32_t address){\n"
    "  uint32_t lo=0u,hi=SECONDARY_CHUNK_ROUTE_COUNT;\n"
    "  while(lo<hi){uint32_t mid=lo+((hi-lo)>>1u);if(s_secondary_chunk_routes[mid].start<=address)lo=mid+1u;else hi=mid;}\n"
    "  if(lo==0u)return 0;\n  const SecondaryChunkRoute* r=&s_secondary_chunk_routes[lo-1u];\n"
    "  return address<r->end&&((address-r->start)&3u)==0u?r->fn:0;\n}\n\n"
    "#if defined(DOLRECOMP_MODULE_HAVE_X86_64_V3)\n"
    "static int secondary_host_has_x86_64_v3(void){\n"
    "#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))\n"
    "  unsigned eax,ebx,ecx,edx;if(!__get_cpuid(1,&eax,&ebx,&ecx,&edx))return 0;\n"
    "  const unsigned req=(1u<<12)|(1u<<22)|(1u<<27)|(1u<<28)|(1u<<29);if((ecx&req)!=req)return 0;\n"
    "  unsigned xlo,xhi;__asm__ volatile(\"xgetbv\":\"=a\"(xlo),\"=d\"(xhi):\"c\"(0));(void)xhi;if((xlo&6u)!=6u)return 0;\n"
    "  if(!__get_cpuid_count(7,0,&eax,&ebx,&ecx,&edx))return 0;\n  if((ebx&((1u<<3)|(1u<<5)|(1u<<8)))!=((1u<<3)|(1u<<5)|(1u<<8)))return 0;\n"
    "  return __get_cpuid(0x80000001u,&eax,&ebx,&ecx,&edx)&&(ecx&(1u<<5));\n"
    "#else\n  return 0;\n#endif\n}\n#endif\n\n"
    "static inline int secondary_selected_dispatch(CPUState* ctx,uint32_t address){\n"
    "#if defined(DOLRECOMP_MODULE_HAVE_X86_64_V3)\n"
    "  static int cached_v3=-1;if(SECONDARY_UNLIKELY(cached_v3<0))cached_v3=secondary_host_has_x86_64_v3();\n"
    "  if(SECONDARY_LIKELY(cached_v3!=0))return dolrecomp_call__x86_64_v3(ctx,address);\n"
    "#endif\n  return dolrecomp_call(ctx,address);\n}\n\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "__attribute__((noinline,cold))\n"
    "#endif\n"
    "static int secondary_legacy_dispatch(CPUState* ctx,uint32_t address){\n"
    "  if(secondary_selected_dispatch(ctx,address))return 1;\n"
    "  uint32_t canonical=address;if(address<ctx->ram_size)canonical=address|GC_RAM_BASE;\n"
    "  SecondaryChunkFn fn=secondary_find_chunk(canonical);if(!fn)return 0;\n"
    "  static unsigned recover_logs;if(recover_logs<8u){fprintf(stderr,\"GEKKOAOT_SECONDARY_DISPATCH_RECOVER_V277=1 address=0x%%08x canonical=0x%%08x\\n\",address,canonical);recover_logs++;}\n"
    "  ctx->pc=canonical;fn(ctx);return 1;\n}\n\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "__attribute__((always_inline))\n"
    "#endif\n"
    "static inline int secondary_configured_mode(void){\n"
    "#if !defined(DOLRECOMP_MODULE_HAVE_X86_64_V3)\n"
    "  static _Atomic(int) configured_mode;int mode=atomic_load_explicit(&configured_mode,memory_order_relaxed);\n"
    "  if(SECONDARY_UNLIKELY(!mode)){const char* v=getenv(\"GEKKOAOT_SECONDARY_DIRECT_DISPATCH\");const char* p=getenv(\"GEKKOAOT_SECONDARY_PAGE_DISPATCH\");const char* i=getenv(\"GEKKOAOT_SECONDARY_INSTRUCTION_DISPATCH\");mode=(v&&v[0]=='0'&&v[1]==0)?-1:(SECONDARY_INSTRUCTION_WORDS && !(i&&i[0]=='0'&&i[1]==0))?3:(p&&p[0]=='0'&&p[1]==0)?2:1;atomic_store_explicit(&configured_mode,mode,memory_order_relaxed);}\n"
    "  return mode;\n#else\n  return -1;\n#endif\n}\n\n"
    "static inline int secondary_dispatch_mode(CPUState* ctx,uint32_t address,int mode){\n"
    "#if !defined(DOLRECOMP_MODULE_HAVE_X86_64_V3)\n"
    "  static const GekkoAOTInstructionRoutes image={s_secondary_chunk_routes,s_secondary_instruction_routes,SECONDARY_INSTRUCTION_BASE,SECONDARY_INSTRUCTION_WORDS,SECONDARY_CHUNK_ROUTE_COUNT};\n"
    "  static GekkoAOTSharedChunkCache indexed={s_secondary_chunk_routes,{0},SECONDARY_PAGE_COUNT?s_secondary_chunk_pages:0,SECONDARY_FIRST_PAGE,SECONDARY_PAGE_COUNT};\n"
    "  static GekkoAOTSharedChunkCache unindexed={s_secondary_chunk_routes,{0},0,0,0};\n"
    "  if(SECONDARY_LIKELY(mode>0)){SecondaryChunkFn direct=mode==3?gekkoaot_find_instruction_chunk(&image,address):gekkoaot_find_shared_chunk(mode==1?&indexed:&unindexed,SECONDARY_CHUNK_ROUTE_COUNT,address);\n"
    "    if(SECONDARY_LIKELY(direct!=0)){ctx->pc=address;direct(ctx);return 1;}}\n"
    "#endif\n"
    "  return secondary_legacy_dispatch(ctx,address);\n}\n\n"
    "static inline int secondary_dispatch_once(CPUState* ctx,uint32_t address){return secondary_dispatch_mode(ctx,address,secondary_configured_mode());}\n\n"
    "#define GEKKOAOT_AOT_POLL_PARK_MARKER 0xFDu\n"
    "static inline uint64_t secondary_charged_cycles(const CPUState* ctx){return (!ctx||ctx->downcount>=0)?0u:(uint64_t)(-(ctx->downcount+1))+1u;}\n"
    "static int secondary_chain_legacy(CPUState* ctx,uint32_t address){\n"
    "  if(SECONDARY_UNLIKELY(!ctx))return 0;\n"
    "  if(SECONDARY_LIKELY(ctx->cycle_budget<=0))return secondary_dispatch_once(ctx,address);\n"
    "  const uint64_t horizon=(uint64_t)ctx->cycle_budget;enum{kMaxSecondaryChain=4096};\n"
    "  uint32_t pc=address;unsigned hops=0;int progressed=0;\n"
    "  while(hops++<kMaxSecondaryChain){\n"
    "    const uint16_t host_epoch=((uint16_t)ctx->external_read_count<<8)|ctx->external_write_count;\n"
    "    const int64_t before_downcount=ctx->downcount;const uint32_t before_pc=ctx->pc;\n"
    "    if(!secondary_dispatch_once(ctx,pc))return progressed?1:0;\n    progressed=1;\n"
    "    if(ctx->exception!=0u||ctx->pc==0u)break;\n"
    "    if(ctx->external_rid==GEKKOAOT_AOT_POLL_PARK_MARKER)break;\n"
    "    if(ctx->external_read_count==255u||ctx->external_write_count==255u)break;\n"
    "    if((((uint16_t)ctx->external_read_count<<8)|ctx->external_write_count)!=host_epoch)break;\n"
    "    if(secondary_charged_cycles(ctx)>=horizon)break;\n"
    "    if(ctx->pc==before_pc&&ctx->downcount==before_downcount)break;\n    pc=ctx->pc;\n"
    "  }return progressed;\n}\n"
    "// v296: mode is immutable after initialization; saturated epochs execute\n"
    "// one chunk. For all other entries a changed epoch already catches every\n"
    "// newly saturated byte, so two checks per hop can be removed exactly.\n"
    "static inline int secondary_boundary_hoist_enabled(void){\n"
    "  static _Atomic(int) enabled;int v=atomic_load_explicit(&enabled,memory_order_relaxed);\n"
    "  if(SECONDARY_UNLIKELY(!v)){const char* p=getenv(\"GEKKOAOT_SECONDARY_BOUNDARY_HOIST\");v=(p&&p[0]=='0'&&p[1]==0)?-1:1;atomic_store_explicit(&enabled,v,memory_order_relaxed);}return v>0;\n}\n"
    "static int secondary_dispatch(CPUState* ctx,uint32_t address){\n"
    "  if(SECONDARY_UNLIKELY(!ctx))return 0;\n"
    "  if(SECONDARY_LIKELY(ctx->cycle_budget<=0))return secondary_dispatch_once(ctx,address);\n"
    "  if(SECONDARY_UNLIKELY(!secondary_boundary_hoist_enabled()))return secondary_chain_legacy(ctx,address);\n"
    "  if(SECONDARY_UNLIKELY(ctx->external_read_count==255u||ctx->external_write_count==255u))return secondary_dispatch_once(ctx,address);\n"
    "  const int mode=secondary_configured_mode();const uint64_t horizon=(uint64_t)ctx->cycle_budget;enum{kMaxSecondaryChain=4096};\n"
    "  uint32_t pc=address;unsigned hops=0;int progressed=0;\n"
    "  while(hops++<kMaxSecondaryChain){\n"
    "    const uint16_t host_epoch=((uint16_t)ctx->external_read_count<<8)|ctx->external_write_count;\n"
    "    const int64_t before_downcount=ctx->downcount;const uint32_t before_pc=ctx->pc;\n"
    "    if(!secondary_dispatch_mode(ctx,pc,mode))return progressed?1:0;\n    progressed=1;\n"
    "    if(ctx->exception!=0u||ctx->pc==0u)break;\n"
    "    if(ctx->external_rid==GEKKOAOT_AOT_POLL_PARK_MARKER)break;\n"
    "    if((((uint16_t)ctx->external_read_count<<8)|ctx->external_write_count)!=host_epoch)break;\n"
    "    if(secondary_charged_cycles(ctx)>=horizon)break;\n"
    "    if(ctx->pc==before_pc&&ctx->downcount==before_downcount)break;\n    pc=ctx->pc;\n"
    "  }return progressed;\n}\n"
    "#define GEKKOAOT_GLOBAL_INDIRECT_QUERY 0xFFFFFFF9u\n"
    "#define GEKKOAOT_GLOBAL_INDIRECT_PENDING 0xFCu\n"
    "void dolrecomp_indirect_dispatch(CPUState* ctx,u32 address){if(secondary_dispatch(ctx,address))return;if(!ctx||!ctx->host_call)return;u32 sa=ctx->external_addr,sv=ctx->external_value;u8 sr=ctx->external_rid;ctx->external_addr=address;ctx->external_value=address;ctx->external_rid=GEKKOAOT_GLOBAL_INDIRECT_PENDING;(void)ctx->host_call(ctx,GEKKOAOT_GLOBAL_INDIRECT_QUERY);ctx->external_addr=sa;ctx->external_value=sv;ctx->external_rid=sr;}\n"
    "static void secondary_loaded(CPUState* ctx){ppc_fpscr_control_updated(ctx);}\n"
    "#include \"secondary_tables.inc\"\n");
  return 1;
}

static int range_contains(const RangeVec* ranges,uint32_t address){for(size_t i=0;i<ranges->n;i++)if(address>=ranges->v[i].start&&address<ranges->v[i].end)return 1;return 0;}
static int collect_generated_chunks(const char* generated,const RangeVec* code,RangeVec* chunks){char header[MAX_PATHBUF];path_join(header,sizeof(header),generated,"generated.h");size_t n=0;unsigned char* data=read_file(header,&n);if(!data)return 0;char* cursor=(char*)data;while((cursor=strstr(cursor,"void func_"))!=NULL){unsigned int address=0;if(sscanf(cursor+10,"%8x",&address)==1&&range_contains(code,(uint32_t)address))rv_push(chunks,(uint32_t)address,(uint32_t)address+4u);cursor+=10;}free(data);if(!chunks->n)return 0;qsort(chunks->v,chunks->n,sizeof(Range),range_cmp);size_t w=0;for(size_t i=0;i<chunks->n;i++){if(w&&chunks->v[w-1].start==chunks->v[i].start)continue;chunks->v[w++]=chunks->v[i];}chunks->n=w;for(size_t i=0;i<chunks->n;i++){uint32_t end=0;for(size_t r=0;r<code->n;r++)if(chunks->v[i].start>=code->v[r].start&&chunks->v[i].start<code->v[r].end){end=code->v[r].end;break;}if(!end){free(chunks->v);memset(chunks,0,sizeof(*chunks));return 0;}if(i+1<chunks->n&&chunks->v[i+1].start>chunks->v[i].start&&chunks->v[i+1].start<end)end=chunks->v[i+1].start;chunks->v[i].end=end;}return 1;}

static int patch_rel_cpu(const char* cpu_src,const char* dst){ size_t n=0; unsigned char* d=read_file(cpu_src,&n);if(!d)return 0;const char* anchor="static u8* resolve_addr(CPUState* cpu, u32 addr, u32* avail) {";char* at=strstr((char*)d,anchor);if(!at){free(d);return 0;}size_t head=(size_t)(at-(char*)d)+strlen(anchor);const char* ins="\n    extern u32 gekkoaot_rel_translate_address(CPUState*, u32);\n    addr = gekkoaot_rel_translate_address(cpu, addr);";char dd[MAX_PATHBUF];dir_name(dd,sizeof(dd),dst);mkdirs(dd);FILE* f=tmpfile();if(!f){free(d);return 0;}fwrite(d,1,head,f);fwrite(ins,1,strlen(ins),f);fwrite(d+head,1,n-head,f);free(d);return finish_generated_file(f,dst); }

static int write_rel_import_proofs(FILE* out,const RelInfo* rel){
  size_t size=0;unsigned char* data=read_file(rel->path,&size);
  if(!data||size<0x30){free(data);return 0;}
  uint32_t table=be32(data+0x28),bytes=be32(data+0x2c);
  if((bytes&7u)||table>size||bytes>size-table){free(data);return 0;}
  fprintf(out,"static const GekkoAOTRelImportProof s_rel_import_proofs[] = {\n");
  for(uint32_t entry=table;entry<table+bytes;entry+=8){
    uint32_t start=be32(data+entry+4),cursor=start;int ended=0;
    while((uint64_t)cursor+8<=size){
      uint32_t type=data[cursor+2];cursor+=8;
      if(type==203){ended=1;break;}
    }
    if(!ended){free(data);return 0;}
    uint64_t hash=fnv_bytes(0xcbf29ce484222325ull,data+start,cursor-start);
    fprintf(out,"  {%uu,%uu,0x%016llXull},\n",be32(data+entry),cursor-start,(unsigned long long)hash);
  }
  fprintf(out,"};\n#define SECONDARY_REL_IMPORT_PROOF_COUNT %uu\n",bytes/8u);
  free(data);return 1;
}

static int generate_project(const char* project,const char* generated,const char* dolrecomp_src,const char* gekko_root,const char* out_dir,const char* output_name,const char* game_id,const RangeVec* code,const RelInfo* rel,const RangeVec* canonical_all,const char* fixed_dol){
  if(!mkdirs(project))return 0;
  RangeVec chunks={0};
  if(!collect_generated_chunks(generated,code,&chunks)){fprintf(stderr,"[secondary-build] error: cannot recover DolRecomp chunk table from %s/generated.h\n",generated);return 0;}
  char tables[MAX_PATHBUF],exportc[MAX_PATHBUF],cmake[MAX_PATHBUF],cpu[MAX_PATHBUF];path_join(tables,sizeof(tables),project,"secondary_tables.inc");path_join(exportc,sizeof(exportc),project,"secondary_module_export.c");path_join(cmake,sizeof(cmake),project,"CMakeLists.txt");path_join(cpu,sizeof(cpu),project,"rel_cpu.c");
  FILE* t=tmpfile();if(!t){free(chunks.v);return 0;}write_ranges(t,"s_code_ranges",code);fprintf(t,"#define SECONDARY_CODE_COUNT %zuu\n",code->n);write_ranges(t,"s_smc_ranges",&(RangeVec){0});fprintf(t,"#define SECONDARY_SMC_COUNT 0u\n");write_ranges(t,"s_chunk_ranges",&chunks);fprintf(t,"#define SECONDARY_CHUNK_COUNT %zuu\nstatic const uint64_t s_chunk_hashes[] = {\n",chunks.n);uint64_t* chunk_hashes=(uint64_t*)calloc(chunks.n?chunks.n:1u,sizeof(uint64_t));if(!chunk_hashes){fclose(t);free(chunks.v);return 0;}if(!rel&&fixed_dol&&!fixed_chunk_hashes(fixed_dol,&chunks,chunk_hashes)){fprintf(stderr,"[secondary-build] error: cannot hash fixed executable chunks from %s\n",fixed_dol);free(chunk_hashes);fclose(t);free(chunks.v);return 0;}for(size_t i=0;i<chunks.n;i++)fprintf(t,"  0x%016llXull,\n",(unsigned long long)chunk_hashes[i]);free(chunk_hashes);fprintf(t,"};\n");
  if(rel){
    if(!write_rel_import_proofs(t,rel)){fclose(t);free(chunks.v);return 0;}
    fprintf(t,"static const GekkoAOTRelSection s_rel_sections[] = {\n");
    uint32_t count=0;
    RangeVec own_data={0};
    for(uint32_t i=0;i<rel->section_count;i++){
      const RelSection* s=&rel->sections[i];
      if(!s->size)continue;
      fprintf(t,"  {%uu,%uu,0x%08Xu,0x%08Xu},\n",
              rel->module_id,s->index,s->linked,s->size);
      count++;
      // v199: a value can only be a publishable self-REL data pointer if it
      // lands in a non-executable section owned by this exact module.
      if(!s->executable)rv_push(&own_data,s->linked,s->linked+s->size);
    }
    fprintf(t,"};\nstatic const GekkoAOTRelModule s_rel_modules[] = {\n"
              "  {%uu,%uu,%uu,0x%Xu,0x%Xu,s_rel_sections,%uu},\n};\n",
            rel->module_id,rel->version,rel->section_count,rel->section_info,
            rel->file_size,count);
    write_ranges(t,"s_rel_own_data_ranges",&own_data);
    fprintf(t,"#define SECONDARY_REL_OWN_DATA_COUNT %zuu\n",own_data.n);
    free(own_data.v);
    write_ranges(t,"s_rel_address_ranges",canonical_all);
    fprintf(t,"#define SECONDARY_REL_ADDRESS_COUNT %zuu\n",canonical_all->n);
  }
  if(!finish_generated_file(t,tables)){free(chunks.v);return 0;}
  FILE* e=tmpfile();if(!e){free(chunks.v);return 0;}
  if(!write_secondary_dispatch_glue(e,&chunks)){fclose(e);free(chunks.v);return 0;}
  if(rel){
    fprintf(e,
      "#define GEKKOAOT_REL_ADDRESS_QUERY 0xFFFFFFFAu\n"
      "#define GEKKOAOT_REL_ADDRESS_PENDING 0xFDu\n"
      "#define GEKKOAOT_REL_ADDRESS_HANDLED 0xFEu\n"
      "u32 gekkoaot_rel_translate_address(CPUState* ctx,u32 address){"
      "int candidate=0;"
      "for(u32 i=0;i<SECONDARY_REL_ADDRESS_COUNT;i++){"
      "if(address>=s_rel_address_ranges[i].start&&address<s_rel_address_ranges[i].end){candidate=1;break;}}"
      "if(!candidate||!ctx||!ctx->host_call)return address;"
      "u32 sa=ctx->external_addr,sv=ctx->external_value;u8 sr=ctx->external_rid;"
      "ctx->external_addr=address;ctx->external_value=address;"
      "ctx->external_rid=GEKKOAOT_REL_ADDRESS_PENDING;"
      "(void)ctx->host_call(ctx,GEKKOAOT_REL_ADDRESS_QUERY);"
      "u32 translated=ctx->external_rid==GEKKOAOT_REL_ADDRESS_HANDLED?ctx->external_value:address;"
      "ctx->external_addr=sa;ctx->external_value=sv;ctx->external_rid=sr;"
      "return translated;}\n"
      // v199: v195 was too broad (any known MEM1-looking constant), while
      // v197 was too strict and missed the classic REL prolog sequence
      // lis/addi/stw because constant folding erased relocation provenance.
      // v220: exported callbacks need the same live-address publication as
      // data pointers. Different REL variants reuse the synthetic code window;
      // publishing a canonical callback can execute an unloaded variant.
      // Admit only a pointer into this module that is stored outside it.
      // This captures exported interface/data pointers without rewriting arbitrary integers
      // or internal REL data stores (the SMB2 regression).
      "static int gekkoaot_rel_own_section(u32 address){"
      "for(u32 i=0;i<(u32)(sizeof(s_rel_sections)/sizeof(s_rel_sections[0]));i++){"
      "u32 start=s_rel_sections[i].linked_start;"
      "u32 end=start+s_rel_sections[i].size;"
      "if(address>=start&&address<end)return 1;}return 0;}\n"
      "static int gekkoaot_rel_own_data(u32 address){"
      "for(u32 i=0;i<SECONDARY_REL_OWN_DATA_COUNT;i++){"
      "if(address>=s_rel_own_data_ranges[i].start&&address<s_rel_own_data_ranges[i].end)return 1;}"
      "return 0;}\n"
      "u32 gekkoaot_rel_translate_published_pointer(CPUState* ctx,u32 destination,u32 value){"
      "if(destination==0xffffffffu||!gekkoaot_rel_own_section(value)||gekkoaot_rel_own_section(destination))return value;"
      "u32 translated=gekkoaot_rel_translate_address(ctx,value);"
      "if(translated!=value){static unsigned logs=0;"
      "if(logs++<32u)fprintf(stderr,\"GEKKOAOT_REL_PUBLISH_V220=1 dst=%%08x canonical=%%08x runtime=%%08x action=translate-own-code-data-cross-module\\n\",destination,value,translated);}"
      "return translated;}\n");
  }
  fprintf(e,"static const GekkoAOTModuleDesc s_desc={GEKKOAOT_MODULE_ABI_VERSION,GEKKOAOT_CPU_ABI_VERSION,(uint32_t)sizeof(CPUState),\"%s\",0x%08Xu,secondary_dispatch,secondary_loaded,s_code_ranges,SECONDARY_CODE_COUNT,s_smc_ranges,SECONDARY_SMC_COUNT,s_chunk_ranges,SECONDARY_CHUNK_COUNT,s_chunk_hashes,",game_id,code->n?code->v[0].start:0u);if(rel)fprintf(e,"s_rel_modules,1u};\n");else fprintf(e,"0,0u};\n");fprintf(e,"#if defined(_WIN32)\n#define GEKKOAOT_EXPORT __declspec(dllexport)\n#elif defined(__GNUC__) || defined(__clang__)\n#define GEKKOAOT_EXPORT __attribute__((visibility(\"default\")))\n#else\n#define GEKKOAOT_EXPORT\n#endif\nGEKKOAOT_EXPORT const GekkoAOTModuleDesc* gekkoaot_get_module(void){return &s_desc;}\n");
  // Match the main module export: paging admission must reflect the compiler
  // marker, including for fixed ELF/DOL overlays and REL modules.
  fprintf(e,"GEKKOAOT_EXPORT uint32_t gekkoaot_module_capabilities(void){\n"
            "  uint32_t caps = 0u;\n"
            "#if defined(GEKKOAOT_AOT_REL_LIVE_GPRS) && GEKKOAOT_AOT_REL_LIVE_GPRS\n"
            "  caps |= GEKKOAOT_MODULE_CAP_REL_LIVE_GPRS;\n"
            "#endif\n"
            "#if defined(GEKKOAOT_AOT_RESTARTABLE_MEMORY) && GEKKOAOT_AOT_RESTARTABLE_MEMORY\n"
            "  caps |= GEKKOAOT_MODULE_CAP_MEMORY_RESTART;\n"
            "#endif\n"
            "  return caps;\n"
            "}\n");
  // v287: native address lowering calls gekkoaot_native_address_base() from
  // every lowered body, so secondary modules must export the same binding as
  // the main module. An unbound window returns NULL (biased RAM fallback).
  fprintf(e,"#include <stdatomic.h>\n#include \"core/native_hardware.h\"\n"
            "static _Atomic(GekkoAOTNativeAddressBase) g_secondary_address_base;\n"
            "GEKKOAOT_EXPORT int gekkoaot_bind_native_address_window(const GekkoAOTNativeAddressWindow* services){\n"
            "  if(!services||services->version!=1u||services->size!=sizeof(*services)||!services->get_base)return 0;\n"
            "  atomic_store_explicit(&g_secondary_address_base,services->get_base,memory_order_release);return 1;\n"
            "}\n"
            "GEKKOAOT_EXPORT uint8_t* gekkoaot_native_address_base(CPUState* cpu){\n"
            "  GekkoAOTNativeAddressBase fn=atomic_load_explicit(&g_secondary_address_base,memory_order_acquire);\n"
            "  return fn?fn(cpu):NULL;\n"
            "}\n");
  // v304: secondary LLVM bodies use the same revocable owner view. Keep
  // the symbol local to each DSO so an unbound old runtime fails closed.
  fprintf(e,"static _Atomic(GekkoAOTGetFlatVmemView) g_secondary_flat_vmem;\n"
            "GEKKOAOT_EXPORT int gekkoaot_bind_native_flat_vmem(const GekkoAOTNativeFlatVmem* services){\n"
            "  if(!services||services->version!=1u||services->size!=sizeof(*services)||!services->get_view)return 0;\n"
            "  atomic_store_explicit(&g_secondary_flat_vmem,services->get_view,memory_order_release);return 1;\n"
            "}\n"
            "GEKKOAOT_EXPORT const GekkoAOTFlatVmemView* gekkoaot_native_flat_vmem_view(CPUState* cpu){\n"
            "  static const GekkoAOTFlatVmemView disabled={NULL,0u};\n"
            "  GekkoAOTGetFlatVmemView fn=atomic_load_explicit(&g_secondary_flat_vmem,memory_order_acquire);\n"
            "  const GekkoAOTFlatVmemView* view=fn?fn(cpu):NULL;return view?view:&disabled;\n"
            "}\n");
  fprintf(e,"static _Atomic(GekkoAOTNativeAddressBase) g_secondary_locked_cache;\n"
            "GEKKOAOT_EXPORT int gekkoaot_bind_native_locked_cache(const GekkoAOTNativeLockedCache* services){\n"
            "  if(!services||services->version!=1u||services->size!=sizeof(*services)||!services->get_base)return 0;\n"
            "  atomic_store_explicit(&g_secondary_locked_cache,services->get_base,memory_order_release);return 1;\n"
            "}\n"
            "GEKKOAOT_EXPORT uint8_t* gekkoaot_native_locked_cache_base(CPUState* cpu){\n"
            "  GekkoAOTNativeAddressBase fn=atomic_load_explicit(&g_secondary_locked_cache,memory_order_acquire);\n"
            "  return fn?fn(cpu):NULL;\n"
            "}\n");
  if(rel)fprintf(e,"GEKKOAOT_EXPORT const GekkoAOTRelImportProof* gekkoaot_rel_import_proofs(uint32_t id,uint32_t* count){if(id!=%uu){*count=0u;return 0;}*count=SECONDARY_REL_IMPORT_PROOF_COUNT;return s_rel_import_proofs;}\n",rel->module_id);
  if(!finish_generated_file(e,exportc)){free(chunks.v);return 0;}
  char cpu_src[MAX_PATHBUF];path_join(cpu_src,sizeof(cpu_src),dolrecomp_src,"src/cpu/cpu.c");if(rel&&!patch_rel_cpu(cpu_src,cpu)){free(chunks.v);return 0;}
  FILE* c=tmpfile();if(!c){free(chunks.v);return 0;}
  fprintf(c,"cmake_minimum_required(VERSION 3.24)\n"
            "project(GekkoAOTSecondary C)\n"
            "set(CMAKE_C_STANDARD 11)\n"
            "set(CMAKE_POSITION_INDEPENDENT_CODE ON)\n"
            "set(GENERATED_DIR \"");
  cmake_path(c,generated);
  fprintf(c,"\")\n"
            "file(GLOB_RECURSE LLVM_OBJECTS \"${GENERATED_DIR}/*.o\" \"${GENERATED_DIR}/*.obj\")\n"
            "file(GLOB_RECURSE C_INPUTS \"${GENERATED_DIR}/*.c\")\n"
            "if(LLVM_OBJECTS)\n"
            "  set_source_files_properties(${LLVM_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)\n"
            "  add_library(gekkoaot_secondary_chunks STATIC ${LLVM_OBJECTS})\n"
            "  set_target_properties(gekkoaot_secondary_chunks PROPERTIES LINKER_LANGUAGE C)\n"
            "  set(DOL_INPUTS)\n"
            "  set(GEKKOAOT_SECONDARY_BACKEND llvm-archive)\n"
            "elseif(C_INPUTS)\n"
            "  set_source_files_properties(${C_INPUTS} PROPERTIES GENERATED TRUE)\n"
            "  set(DOL_INPUTS ${C_INPUTS})\n"
            "  set(GEKKOAOT_SECONDARY_BACKEND c)\n"
            "else()\n"
            "  message(FATAL_ERROR \"No DolRecomp LLVM objects or C sources below ${GENERATED_DIR}\")\n"
            "endif()\n"
            "message(STATUS \"GekkoAOT secondary module backend: ${GEKKOAOT_SECONDARY_BACKEND}\")\n"
            "add_library(gekkoaot_secondary SHARED secondary_module_export.c\n  \"");
  cmake_path(c,rel?cpu:cpu_src);
  fprintf(c,"\"\n  ${DOL_INPUTS})\n"
            "if(TARGET gekkoaot_secondary_chunks)\n"
            "  target_link_libraries(gekkoaot_secondary PRIVATE gekkoaot_secondary_chunks)\n"
            "endif()\n"
            "find_program(GEKKOAOT_CCACHE_PROGRAM ccache)\n"
            "if(GEKKOAOT_CCACHE_PROGRAM)\n"
            "  set_property(TARGET gekkoaot_secondary PROPERTY C_COMPILER_LAUNCHER \"${GEKKOAOT_CCACHE_PROGRAM}\")\n"
            "endif()\n"
            "target_include_directories(gekkoaot_secondary PRIVATE \"${GENERATED_DIR}\" \"");
  cmake_path(c,dolrecomp_src);
  fprintf(c,"/src\" \"");
  cmake_path(c,gekko_root);
  fprintf(c,"/runtime\" \"");
  cmake_path(c,project);
  fprintf(c,"\")\n"
            "set_target_properties(gekkoaot_secondary PROPERTIES PREFIX \"\" OUTPUT_NAME \"%s\" LIBRARY_OUTPUT_DIRECTORY \"",output_name);
  cmake_path(c,out_dir);
  fprintf(c,"\" RUNTIME_OUTPUT_DIRECTORY \"");
  cmake_path(c,out_dir);
  fprintf(c,"\" C_VISIBILITY_PRESET hidden)\n"
            "file(READ \"${GENERATED_DIR}/generated.h\" GENERATED_HEADER_TEXT)\n"
            "if(GENERATED_HEADER_TEXT MATCHES \"dolrecomp_call__x86_64_v3\")\n"
            "  target_compile_definitions(gekkoaot_secondary PRIVATE DOLRECOMP_MODULE_HAVE_X86_64_V3=1)\n"
            "endif()\n"
            "option(GEKKOAOT_SECONDARY_IPO \"Enable IPO/LTO for secondary glue\" ON)\n"
            "if(GEKKOAOT_SECONDARY_IPO)\n"
            "  include(CheckIPOSupported)\n"
            "  check_ipo_supported(RESULT GEKKOAOT_SECONDARY_IPO_OK OUTPUT GEKKOAOT_SECONDARY_IPO_ERROR LANGUAGES C)\n"
            "  if(GEKKOAOT_SECONDARY_IPO_OK)\n"
            "    set_property(TARGET gekkoaot_secondary PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)\n"
            "  endif()\n"
            "endif()\n"
            "if(NOT WIN32)\n"
            "  target_link_libraries(gekkoaot_secondary PRIVATE m)\n"
            "endif()\n"
            "if(MSVC)\n"
            "  target_compile_options(gekkoaot_secondary PRIVATE /O2 /fp:strict)\n"
            "else()\n"
            "  target_compile_options(gekkoaot_secondary PRIVATE -O3 -ffp-contract=off -fno-fast-math -fvisibility=hidden -fno-semantic-interposition -fomit-frame-pointer)\n"
            "  if(NOT APPLE)\n"
            "    target_compile_options(gekkoaot_secondary PRIVATE -fno-plt)\n"
            "  endif()\n"
            "  if(NOT APPLE AND (CMAKE_SYSTEM_PROCESSOR MATCHES \"x86_64|AMD64|amd64|aarch64|arm64\"))\n"
            "    target_compile_options(gekkoaot_secondary PRIVATE -march=native -mtune=native)\n"
            "  endif()\n"
            "endif()\n");
  // rel_cpu.c is a patched copy of DolRecomp/src/cpu/cpu.c. Once copied into
  // the generated side-module directory, quoted includes such as "cpu.h" no
  // longer inherit DolRecomp/src/cpu as their source-file directory. Keep the
  // original CPU include directory explicit for both REL and fixed modules.
  fprintf(c,"target_include_directories(gekkoaot_secondary PRIVATE \"");cmake_path(c,dolrecomp_src);fprintf(c,"/src/cpu\")\n");
  if(!finish_generated_file(c,cmake)){free(chunks.v);return 0;}printf("GEKKOAOT_SECONDARY_ROUTER=1 chunks=%zu source=%s\n",chunks.n,generated);free(chunks.v);return 1;
}

static int reusable_build_tree(const char* build){
  char cache[MAX_PATHBUF];path_join(cache,sizeof(cache),build,"CMakeCache.txt");
  size_t size=0;unsigned char* data=read_file(cache,&size);if(!data)return 0;
  // CMake may cache a tool as STRING rather than FILEPATH (notably with
  // Windows LLVM). Validate the executable path independently of that type.
  const char* keys[]={"CMAKE_C_COMPILER:","CMAKE_MAKE_PROGRAM:",
                      "CMAKE_COMMAND:"};
  int valid=1;
  for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i){
    char* value=strstr((char*)data,keys[i]);
    if(!value){valid=0;break;}value+=strlen(keys[i]);
    char* equals=strchr(value,'=');char* newline=strchr(value,'\n');
    if(!equals||(newline&&equals>newline)){valid=0;break;}value=equals+1;
    char* end=strchr(value,'\n');if(end)*end=0;
    size_t n=strlen(value);char* cr=n&&value[n-1]=='\r'?value+n-1:NULL;
    if(cr)*cr=0;
    if(!is_file(value))valid=0;
    // Restore both CR and LF: leaving a NUL at CR truncated the cache before
    // the remaining tool entries on Windows.
    if(cr)*cr='\r';
    if(end)*end='\n';
  }
  free(data);return valid;
}

static int build_project(const char* project,const char* build,int jobs){char cmd[16384];
  // AppImages mount at a different /tmp/.mount_* path on every launch. CMake
  // caches absolute compiler/toolchain/Ninja paths, so a side-module build tree
  // from an older launch becomes invalid even though its LLVM objects are still
  // reusable. Keep valid build trees so a relink does not recompile CPU glue.
  if(!reusable_build_tree(build)){
    snprintf(cmd,sizeof(cmd),"cmake -E rm -rf");qappend(cmd,sizeof(cmd),build);if(run_cmd(cmd))return 0;
  }
  snprintf(cmd,sizeof(cmd),"cmake -S");qappend(cmd,sizeof(cmd),project);strcat(cmd," -B");qappend(cmd,sizeof(cmd),build);strcat(cmd," -G Ninja -DCMAKE_BUILD_TYPE=Release");if(run_cmd(cmd))return 0;
  snprintf(cmd,sizeof(cmd),"cmake --build");qappend(cmd,sizeof(cmd),build);char tail[64];snprintf(tail,sizeof(tail)," -j%d",jobs);strcat(cmd,tail);return run_cmd(cmd)==0;}

// REL native wrappers are independent after DolRecomp has emitted the batch.
// Building them serially turns a title with hundreds of RELs into hundreds of
// CMake configure/link round trips.  Keep the codegen batch intact for import
// resolution, but fan out the cheap native wrapper/link phase on POSIX hosts.
typedef struct {
  char module[MAX_PATHBUF];
  char project[MAX_PATHBUF];
  char build[MAX_PATHBUF];
  uint32_t module_id;
  int needs_build;
} RelNativeJob;

static int build_rel_native_jobs(RelNativeJob* work,size_t count,int jobs){
  size_t pending=0;for(size_t i=0;i<count;i++)if(work[i].needs_build)pending++;
  if(!pending)return 1;
#ifdef _WIN32
  printf("GEKKOAOT_REL_NATIVE_PARALLEL=0 reason=windows-fallback modules=%zu\n",pending);
  for(size_t i=0;i<count;i++)if(work[i].needs_build){
    if(!build_project(work[i].project,work[i].build,jobs))return 0;
    if(!is_file(work[i].module))return 0;
  }
  return 1;
#else
  unsigned workers=(unsigned)(jobs>0?jobs:1);if(workers>pending)workers=(unsigned)pending;
  printf("GEKKOAOT_REL_NATIVE_PARALLEL=1 modules=%zu workers=%u inner_jobs=1\n",pending,workers);
  pid_t* pids=(pid_t*)calloc(count,sizeof(*pids));if(!pids)return 0;
  size_t next=0,active=0,completed=0;int failed=0;
  while(completed<pending){
    while(!failed&&active<workers){
      while(next<count&&!work[next].needs_build)next++;
      if(next>=count)break;
      const size_t index=next++;
      fflush(NULL);
      pid_t pid=fork();
      if(pid==0){
        const int ok=build_project(work[index].project,work[index].build,1)&&is_file(work[index].module);
        fflush(NULL);_exit(ok?0:1);
      }
      if(pid<0){
        fprintf(stderr,"GEKKOAOT_REL_NATIVE_PARALLEL_FALLBACK=1 module=%u reason=fork-failed\n",work[index].module_id);
        if(!build_project(work[index].project,work[index].build,1)||!is_file(work[index].module))failed=1;
        work[index].needs_build=0;completed++;
        continue;
      }
      pids[index]=pid;active++;
    }
    if(!active){if(failed||completed>=pending)break;continue;}
    int status=0;const pid_t pid=wait(&status);
    if(pid<0){if(errno==EINTR)continue;failed=1;break;}
    size_t index=count;for(size_t i=0;i<count;i++)if(pids[i]==pid){index=i;break;}
    if(index<count){pids[index]=0;active--;completed++;
      if(!WIFEXITED(status)||WEXITSTATUS(status)!=0||!is_file(work[index].module)){
        fprintf(stderr,"[secondary-build] error: REL native module %u build failed\n",work[index].module_id);failed=1;
      }
    }
  }
  while(active){int status=0;const pid_t pid=wait(&status);if(pid<0){if(errno==EINTR)continue;break;}for(size_t i=0;i<count;i++)if(pids[i]==pid){pids[i]=0;active--;break;}}
  free(pids);if(failed)return 0;
  for(size_t i=0;i<count;i++)if(!is_file(work[i].module))return 0;
  return 1;
#endif
}

static int find_generated_for_rel(const StrVec* headers,uint32_t module_id,char* out,size_t cap){char suffix[64];snprintf(suffix,sizeof(suffix),"_%u",module_id);for(size_t i=0;i<headers->n;i++){char d[MAX_PATHBUF];dir_name(d,sizeof(d),headers->v[i]);const char* b=base_name(d);size_t bn=strlen(b),sn=strlen(suffix);if(bn>=sn&&!strcmp(b+bn-sn,suffix)){snprintf(out,cap,"%s",d);return 1;}}return 0;}
static void collect_dol_code(const char* p,RangeVec* code,uint32_t* entry){size_t n=0;unsigned char* d=read_file(p,&n);if(!d||n<0x100)die("invalid DOL while collecting code");*entry=be32(d+0xe0);for(int i=0;i<7;i++){uint32_t a=be32(d+0x48+i*4),z=be32(d+0x90+i*4);if(z)rv_push(code,a,a+z);}free(d);qsort(code->v,code->n,sizeof(Range),range_cmp);}

static int address_in_ranges(const RangeVec* ranges,uint32_t address){
  if((address&3u)!=0u)return 0;
  for(size_t i=0;i<ranges->n;i++)
    if(address>=ranges->v[i].start&&address<ranges->v[i].end)return 1;
  return 0;
}

/*
 * Nintendo REL imports with module id 0 resolve against the main executable.
 * The addend is therefore an absolute DOL address.  Any relocation that points
 * into executable DOL text is a real cross-module entry candidate: direct
 * REL24 calls, function pointers (ADDR32), split-address materialization, etc.
 *
 * GekkoAOT feeds these addresses to DolRecomp's existing --map entry-point
 * collector so LLVM keeps a native region leader at each target.  This avoids
 * the old REL -> DOL "instruction fallback required" path without making every
 * instruction in the main DOL externally enterable.
 */
/*
 * v275: a disc can ship REL files linked against a different main executable
 * (development/debug/other-region builds left next to the real modules).  Their
 * module-0 relocations are not entries into *this* DOL; feeding them to the
 * partitioner splits ordinary DOL functions at arbitrary instructions and turns
 * every crossing into a host boundary.  A REL that really links against this
 * DOL calls function starts with its REL24 relocations, so validate each REL by
 * that property and ignore every module-0 target of a REL that fails it.
 */
typedef struct {
  unsigned char* bytes; size_t size;
  uint32_t off[7],addr[7],len[7]; uint32_t entry;
  U32Vec calls; /* sorted DOL-internal bl targets */
} DolImage;

static int dol_word(const DolImage* dol,uint32_t address,uint32_t* word){
  for(int i=0;i<7;i++)
    if(dol->len[i]&&address>=dol->addr[i]&&address-dol->addr[i]+4u<=dol->len[i]){
      *word=be32(dol->bytes+dol->off[i]+(address-dol->addr[i]));return 1;
    }
  return 0;
}

static int dol_load_image(const char* path,DolImage* dol){
  memset(dol,0,sizeof(*dol));
  dol->bytes=read_file(path,&dol->size);
  if(!dol->bytes||dol->size<0x100)return 0;
  dol->entry=be32(dol->bytes+0xe0);
  for(int i=0;i<7;i++){
    dol->off[i]=be32(dol->bytes+i*4);dol->addr[i]=be32(dol->bytes+0x48+i*4);dol->len[i]=be32(dol->bytes+0x90+i*4);
    if((uint64_t)dol->off[i]+dol->len[i]>dol->size)dol->len[i]=0;
  }
  for(int i=0;i<7;i++)
    for(uint32_t o=0;o+4u<=dol->len[i];o+=4u){
      const uint32_t w=be32(dol->bytes+dol->off[i]+o);
      if((w>>26)!=18u||!(w&1u))continue;
      int32_t disp=(int32_t)(w&0x03fffffcu);if(disp&0x02000000)disp-=0x04000000;
      u32v_push(&dol->calls,((w&2u)?0u:dol->addr[i]+o)+(uint32_t)disp);
    }
  qsort(dol->calls.v,dol->calls.n,sizeof(*dol->calls.v),u32_cmp);
  return 1;
}

static int dol_plausible_function_start(const DolImage* dol,uint32_t target){
  if(target==dol->entry)return 1;
  if(dol->calls.n&&bsearch(&target,dol->calls.v,dol->calls.n,sizeof(*dol->calls.v),u32_cmp))return 1;
  for(int i=0;i<7;i++)if(dol->len[i]&&target==dol->addr[i])return 1;
  uint32_t prev=0;
  if(!dol_word(dol,target-4u,&prev))return 0;
  /* blr, bctr, rfi, padding, or an unconditional non-linking b. */
  return prev==0x4E800020u||prev==0x4E800420u||prev==0x4C000064u||prev==0u||
         ((prev>>26)==18u&&!(prev&1u));
}

static int collect_rel_main_entrypoints(const char* path,const RangeVec* dol_code,const DolImage* dol,
                                        U32Vec* targets,int* mismatched){
  size_t n=0;unsigned char* d=read_file(path,&n);
  *mismatched=0;
  if(!d||n<0x30u){free(d);return 0;}
  const uint32_t imports=be32(d+0x28),import_size=be32(d+0x2c);
  if(!import_size){free(d);return 1;}
  if((import_size&7u)||imports>n||import_size>n-imports){free(d);return 0;}

  U32Vec local={0};
  size_t calls=0,plausible=0;
  for(uint32_t off=imports;off<imports+import_size;off+=8u){
    const uint32_t module_id=be32(d+off);
    uint32_t reloc=be32(d+off+4u);
    if(module_id!=0u)continue;
    if(reloc>=n){free(local.v);free(d);return 0;}

    int ended=0;
    size_t guard=0;
    while(reloc+8u<=n&&guard++<=n/8u+1u){
      const uint8_t type=d[reloc+2u];
      const uint32_t addend=be32(d+reloc+4u);
      reloc+=8u;
      if(type==203u){ended=1;break;} /* R_DOLPHIN_END */
      if(type==201u||type==202u||type==204u)continue; /* NOP/SECTION/MRKREF */
      if(!address_in_ranges(dol_code,addend))continue;
      u32v_push(&local,addend);
      if(type==10u){ /* R_PPC_REL24: a direct call/branch into the DOL */
        calls++;
        if(dol&&dol_plausible_function_start(dol,addend))plausible++;
      }
    }
    if(!ended){free(local.v);free(d);return 0;}
  }
  free(d);
  if(dol&&calls>=4u&&plausible*2u<calls){
    *mismatched=1;
    printf("GEKKOAOT_REL_TO_DOL_MISMATCH_V275=1 rel=\"%s\" rel24=%zu function_starts=%zu action=ignore-module0-targets\n",
           path,calls,plausible);
  } else {
    for(size_t i=0;i<local.n;i++)u32v_push(targets,local.v[i]);
  }
  free(local.v);
  return 1;
}

static int write_main_entrypoints_manifest(const CandidateVec* candidates,const char* boot_dol,const char* path){
  RangeVec dol_code={0};uint32_t entry=0;collect_dol_code(boot_dol,&dol_code,&entry);
  /* Opt-in until measured: GEKKOAOT_REL_LINKED_DOL_CHECK=1 ignores RELs that
     are not linked against this DOL. */
  const char* check=getenv("GEKKOAOT_REL_LINKED_DOL_CHECK");
  DolImage image;memset(&image,0,sizeof(image));
  const int have_image=(check&&!strcmp(check,"1"))&&dol_load_image(boot_dol,&image);
  U32Vec targets={0};size_t rels=0,ignored=0;
  for(size_t i=0;i<candidates->n;i++){
    if(candidates->v[i].kind!=CAND_REL)continue;
    rels++;
    int mismatched=0;
    if(!collect_rel_main_entrypoints(candidates->v[i].path,&dol_code,have_image?&image:NULL,&targets,&mismatched)){
      fprintf(stderr,"[secondary-build] warning: cannot scan REL imports: %s\n",candidates->v[i].path);
    }
    ignored+=(size_t)mismatched;
  }
  free(image.bytes);free(image.calls.v);
  qsort(targets.v,targets.n,sizeof(*targets.v),u32_cmp);
  size_t unique=0;
  for(size_t i=0;i<targets.n;i++)
    if(!unique||targets.v[i]!=targets.v[unique-1u])targets.v[unique++]=targets.v[i];

  char parent[MAX_PATHBUF];dir_name(parent,sizeof(parent),path);
  if(!mkdirs(parent)){free(dol_code.v);free(targets.v);return 0;}
  FILE* out=fopen(path,"wb");
  if(!out){free(dol_code.v);free(targets.v);return 0;}
  for(size_t i=0;i<unique;i++)fprintf(out,"%08x\n",targets.v[i]);
  const int ok=fclose(out)==0;
  printf("GEKKOAOT_REL_TO_DOL_ENTRYPOINTS_V206=1 rel=%zu targets=%zu ignored_mismatched_rel=%zu manifest=\"%s\" policy=module0-relocations-in-dol-text+v275-linked-dol-check\n",
         rels,unique,ignored,path);
  free(dol_code.v);free(targets.v);return ok;
}



typedef struct { uint32_t off, addr, size; } DolTextSegment;
static int parse_dol_text_segments(const unsigned char* d,size_t n,DolTextSegment out[7],size_t* count,uint32_t* entry){
  if(!d||n<0x100)return 0;
  *count=0;*entry=be32(d+0xe0);
  for(int i=0;i<7;i++){
    uint32_t off=be32(d+i*4),addr=be32(d+0x48+i*4),size=be32(d+0x90+i*4);
    if(!size)continue;
    if(addr<0x80000000u||(uint64_t)addr+size>0x81800000ull||(uint64_t)off+size>n)return 0;
    out[(*count)++]=(DolTextSegment){off,addr,size};
  }
  return *count!=0;
}
static int dol_text_lookup(const unsigned char* d,const DolTextSegment* segs,size_t count,uint32_t addr,const unsigned char** ptr,uint32_t* avail){
  for(size_t i=0;i<count;i++){
    if(addr>=segs[i].addr&&addr<segs[i].addr+segs[i].size){
      uint32_t delta=addr-segs[i].addr;*ptr=d+segs[i].off+delta;*avail=segs[i].size-delta;return 1;
    }
  }
  return 0;
}
static int zero_section_padding(const unsigned char* bytes,uint32_t size){
  if(size>31u)return 0;
  for(uint32_t i=0;i<size;i++)if(bytes[i])return 0;
  return 1;
}
static int dol_text_equivalent(const char* a_path,const char* b_path){
  size_t an=0,bn=0;unsigned char* a=read_file(a_path,&an);unsigned char* b=read_file(b_path,&bn);
  if(!a||!b){free(a);free(b);return 0;}
  DolTextSegment as[7],bs[7];size_t ac=0,bc=0;uint32_t ae=0,be=0;
  int ok=parse_dol_text_segments(a,an,as,&ac,&ae)&&parse_dol_text_segments(b,bn,bs,&bc,&be)&&ae==be;
  for(size_t i=0;ok&&i<ac;i++){
    uint32_t addr=as[i].addr,left=as[i].size,src_off=0;
    const unsigned char* first=NULL;uint32_t first_avail=0;if(!dol_text_lookup(b,bs,bc,addr,&first,&first_avail)){ok=0;break;}
    while(left){const unsigned char* bp=NULL;uint32_t bavail=0;if(!dol_text_lookup(b,bs,bc,addr,&bp,&bavail)){ok=zero_section_padding(a+as[i].off+src_off,left);break;}uint32_t chunk=left<bavail?left:bavail;if(memcmp(a+as[i].off+src_off,bp,chunk)){ok=0;break;}addr+=chunk;src_off+=chunk;left-=chunk;}
  }
  for(size_t i=0;ok&&i<bc;i++){
    uint32_t addr=bs[i].addr,left=bs[i].size,src_off=0;
    const unsigned char* first=NULL;uint32_t first_avail=0;if(!dol_text_lookup(a,as,ac,addr,&first,&first_avail)){ok=0;break;}
    while(left){const unsigned char* ap=NULL;uint32_t aavail=0;if(!dol_text_lookup(a,as,ac,addr,&ap,&aavail)){ok=zero_section_padding(b+bs[i].off+src_off,left);break;}uint32_t chunk=left<aavail?left:aavail;if(memcmp(b+bs[i].off+src_off,ap,chunk)){ok=0;break;}addr+=chunk;src_off+=chunk;left-=chunk;}
  }
  free(a);free(b);return ok;
}

static int elf_to_dol(const char* src,const char* dst,RangeVec* code,uint32_t* entry){size_t n=0;unsigned char* d=read_file(src,&n);if(!d||n<0x34||memcmp(d,"\x7f""ELF",4)||d[4]!=1||d[5]!=2||be16(d+0x12)!=20||be16(d+0x10)!=2){free(d);return 0;}*entry=be32(d+0x18);typedef struct{uint32_t off,addr,size;} Seg;Seg seg[7];int ns=0;uint32_t shoff=be32(d+0x20);uint16_t shents=be16(d+0x2e),shnum=be16(d+0x30);if(shoff&&shents>=40&&(uint64_t)shoff+(uint64_t)shents*shnum<=n){for(uint16_t i=0;i<shnum;i++){const unsigned char* s=d+shoff+i*shents;uint32_t type=be32(s+4),flags=be32(s+8),addr=be32(s+12),off=be32(s+16),sz=be32(s+20);if(type==8||!sz||(flags&6)!=6)continue;if((uint64_t)off+sz>n||addr<0x80000000u||(uint64_t)addr+sz>0x81800000ull||ns>=7){free(d);return 0;}seg[ns++]=(Seg){off,addr,sz};}}
  if(!ns){uint32_t phoff=be32(d+0x1c);uint16_t phents=be16(d+0x2a),phnum=be16(d+0x2c);if(!phoff||phents<32||(uint64_t)phoff+(uint64_t)phents*phnum>n){free(d);return 0;}for(uint16_t i=0;i<phnum;i++){const unsigned char* s=d+phoff+i*phents;uint32_t type=be32(s),off=be32(s+4),v=be32(s+8),pa=be32(s+12),sz=be32(s+16),flags=be32(s+24),addr=pa?pa:v;if(type!=1||!sz||!(flags&1))continue;if((uint64_t)off+sz>n||addr<0x80000000u||(uint64_t)addr+sz>0x81800000ull||ns>=7){free(d);return 0;}seg[ns++]=(Seg){off,addr,sz};}}
  if(!ns){free(d);return 0;}unsigned char hdr[0x100];memset(hdr,0,sizeof(hdr));uint32_t fileoff=0x100;for(int i=0;i<ns;i++){fileoff=align_up(fileoff,0x20);put_be32(hdr+i*4,fileoff);put_be32(hdr+0x48+i*4,seg[i].addr);put_be32(hdr+0x90+i*4,seg[i].size);rv_push(code,seg[i].addr,seg[i].addr+seg[i].size);fileoff+=seg[i].size;}put_be32(hdr+0xe0,*entry);char dd[MAX_PATHBUF];dir_name(dd,sizeof(dd),dst);mkdirs(dd);FILE* f=fopen(dst,"wb");if(!f){free(d);return 0;}if(fwrite(hdr,1,sizeof(hdr),f)!=sizeof(hdr)){fclose(f);free(d);return 0;}uint32_t pos=0x100;for(int i=0;i<ns;i++){uint32_t aligned=align_up(pos,0x20);while(pos<aligned){fputc(0,f);pos++;}if(fwrite(d+seg[i].off,1,seg[i].size,f)!=seg[i].size){fclose(f);free(d);return 0;}pos+=seg[i].size;}fclose(f);free(d);qsort(code->v,code->n,sizeof(Range),range_cmp);return 1;}

static int write_rel_pointer_manifest(const RelVec* rels,const char* path){
  FILE* out=fopen(path,"wb");if(!out)return 0;
  int ok=1;size_t marked=0;
  for(size_t i=0;i<rels->n&&ok;i++){
    const RelInfo* r=&rels->v[i];size_t size=0;unsigned char* data=read_file(r->path,&size);
    if(!data||size<0x30){free(data);ok=0;break;}
    uint32_t imports=be32(data+0x28),bytes=be32(data+0x2c);
    if((bytes&7u)||imports>size||bytes>size-imports){free(data);ok=0;break;}
    for(uint32_t imp=imports;imp<imports+bytes&&ok;imp+=8){
      if(be32(data+imp)!=r->module_id)continue;
      uint32_t cursor=be32(data+imp+4),section=UINT32_MAX,offset=0;int ended=0;
      while((uint64_t)cursor+8<=size){
        uint32_t delta=be16(data+cursor),type=data[cursor+2],symbol=data[cursor+3];
        uint32_t addend=be32(data+cursor+4);cursor+=8;
        if(type==203){ended=1;break;}
        if(type==202){section=symbol;offset=0;continue;}
        if(offset>UINT32_MAX-delta){ok=0;break;}offset+=delta;
        if(type!=3&&type!=4&&type!=5&&type!=6)continue;
        if(section>=r->section_count||symbol>=r->section_count){ok=0;break;}
        const RelSection* s=&r->sections[section];
        if(!s->executable)continue;
        uint32_t word=offset&~3u;
        if((offset&3u)!=2u||(uint64_t)word+4>s->size||
           (uint64_t)s->offset+word+4>size){ok=0;break;}
        uint32_t raw=be32(data+s->offset+word),op=raw>>26;
        char kind=0;
        if(type==3||type==4){
          if(op==14||op==24)kind='p';
          else if(op>=32&&op<=55)kind='a';
        }else if(op==15&&((raw>>16)&31u)==0u&&
                 ((r->sections[symbol].linked+addend)&0xffffu)==0u)kind='p';
        if(kind){fprintf(out,"%08x %c\n",s->linked+word,kind);marked++;}
      }
      if(!ended)ok=0;
    }
    free(data);
  }
  if(fclose(out))ok=0;
  printf("GEKKOAOT_REL_POINTER_MATERIALIZATION_V222=1 sites=%zu proof=self-relocation-stream manifest=\"%s\"\n",marked,path);
  return ok;
}

static int build_rel_modules(const char* files,const char* boot_dol,const char* dolrecomp,const char* dolsrc,const char* gekko,const char* output,const char* game,int jobs,CandidateVec* all,StrVec* built){RelVec rels={0};uint64_t h=g_secondary_toolchain_hash;h=fnv_bytes(h,SECONDARY_ROUTER_CACHE_TAG,sizeof(SECONDARY_ROUTER_CACHE_TAG)-1u);h=fnv_bytes(h,REL_CODEGEN_CACHE_TAG,sizeof(REL_CODEGEN_CACHE_TAG)-1u);h=fnv_bytes(h,game,strlen(game));for(size_t i=0;i<all->n;i++)if(all->v[i].kind==CAND_REL){RelInfo r;if(!parse_rel(all->v[i].path,&r))continue;if(rels.n==rels.cap){rels.cap=rels.cap?rels.cap*2:8;rels.v=(RelInfo*)xrealloc(rels.v,rels.cap*sizeof(*rels.v));}rels.v[rels.n++]=r;h=fnv_file(fnv_bytes(h,&r.module_id,4),r.path);}if(!rels.n)return 1;
  // A disc may contain mutually-exclusive REL overlays that intentionally reuse
  // the same module id. They cannot be handed to DolRecomp in one REL batch,
  // because module id is the batch identity. Keep genuinely unique ids batched
  // (for inter-REL import resolution) and compile colliding overlays per-file.
  int duplicate_ids=0;
  for(size_t i=0;i<rels.n;i++)for(size_t j=i+1;j<rels.n;j++)if(rels.v[i].module_id==rels.v[j].module_id){duplicate_ids=1;break;}
  if(duplicate_ids){
    CandidateVec unique={0},overlays={0};
    size_t duplicate_variants=0;
    for(size_t i=0;i<rels.n;i++){
      size_t count=0;
      for(size_t j=0;j<rels.n;j++)if(rels.v[i].module_id==rels.v[j].module_id)count++;
      if(count==1){cv_push(&unique,rels.v[i].path,CAND_REL);continue;}
      cv_push(&overlays,rels.v[i].path,CAND_REL);
      duplicate_variants++;
      int first=1;for(size_t j=0;j<i;j++)if(rels.v[j].module_id==rels.v[i].module_id){first=0;break;}
      if(first)printf("GEKKOAOT_REL_OVERLAY_ID=1 module_id=%u variants=%zu\n",rels.v[i].module_id,count);
    }
    printf("GEKKOAOT_REL_OVERLAY_MODE=1 rel=%zu duplicate_variants=%zu unique=%zu policy=duplicate-id-per-file-aot\n",rels.n,duplicate_variants,unique.n);
    for(size_t i=0;i<rels.n;i++) free_rel(&rels.v[i]);
    free(rels.v);
    if(unique.n&&!build_rel_modules(files,boot_dol,dolrecomp,dolsrc,gekko,output,game,jobs,&unique,built)){free(unique.v);free(overlays.v);return 0;}
    for(size_t i=0;i<overlays.n;i++){printf("GEKKOAOT_REL_OVERLAY_BUILD=1 index=%zu total=%zu source=\"%s\"\n",i+1,overlays.n,overlays.v[i].path);printf("GEKKOAOT_SECONDARY_ITEM=1 kind=rel index=%zu total=%zu source=\"%s\"\n",i+1,overlays.n,overlays.v[i].path);CandidateVec one={&overlays.v[i],1,1};if(!build_rel_modules(files,boot_dol,dolrecomp,dolsrc,gekko,output,game,jobs,&one,built)){free(unique.v);free(overlays.v);return 0;}}
    free(unique.v);free(overlays.v);return 1;
  }
  const uint32_t rel_base=choose_rel_canonical_base(&rels,boot_dol);h=fnv_bytes(h,&rel_base,4);if(g_rel_dense_layout){h=fnv_bytes(h,"dense-v364b",11);h=fnv_bytes(h,&g_rel_dense_align,4);}uint32_t cursor=rel_base;RangeVec canonical={0};for(size_t i=0;i<rels.n;i++){RelInfo* r=&rels.v[i];r->base=cursor;assign_rel_sections(r);for(uint32_t j=0;j<r->section_count;j++)if(r->sections[j].size)rv_push(&canonical,r->sections[j].linked,r->sections[j].linked+r->sections[j].size);if(g_rel_dense_layout){cursor=rel_dense_end(r,r->base);if(!cursor)die("REL canonical address overflow");continue;}uint64_t end=(uint64_t)r->base+r->file_size+r->bss_size;if(end>0xffffffffu)die("REL canonical address overflow");cursor=align_up((uint32_t)end,REL_ALIGN);if(!cursor)die("REL canonical alignment overflow");}qsort(canonical.v,canonical.n,sizeof(Range),range_cmp);
  char suffix[16];module_suffix(suffix,sizeof(suffix));char modules[MAX_PATHBUF];path_join(modules,sizeof(modules),output,"modules");mkdirs(modules);char key[32];snprintf(key,sizeof(key),"%016llx",(unsigned long long)h);int cache=1;for(size_t i=0;i<rels.n;i++){char name[256],p[MAX_PATHBUF];snprintf(name,sizeof(name),"g%s_rel_%u_%s%s",game,rels.v[i].module_id,key,suffix);path_join(p,sizeof(p),modules,name);if(!is_file(p)){cache=0;break;}}if(cache){for(size_t i=0;i<rels.n;i++){char name[256],p[MAX_PATHBUF];snprintf(name,sizeof(name),"g%s_rel_%u_%s%s",game,rels.v[i].module_id,key,suffix);path_join(p,sizeof(p),modules,name);sv_push_unique(built,p);}printf("GEKKOAOT_REL_AOT_CACHE_HIT=1 modules=%zu key=%s\n",rels.n,key);goto done;}
  char work[MAX_PATHBUF],stage[MAX_PATHBUF],genroot[MAX_PATHBUF];char wn[128];snprintf(wn,sizeof(wn),"rel-all-%s",key);path_join(work,sizeof(work),output,wn);path_join(stage,sizeof(stage),work,"rel-input");path_join(genroot,sizeof(genroot),work,"dolrecomp");mkdirs(stage);mkdirs(genroot);for(size_t i=0;i<rels.n;i++){char n[256],dst[MAX_PATHBUF];snprintf(n,sizeof(n),"%04zu_%u.rel",i,rels.v[i].module_id);path_join(dst,sizeof(dst),stage,n);if(!copy_file(rels.v[i].path,dst))die("failed staging REL");}
  path_join(g_rel_pointer_manifest,sizeof(g_rel_pointer_manifest),work,"rel-pointer-materializations-v222.txt");
  if(!write_rel_pointer_manifest(&rels,g_rel_pointer_manifest))die("invalid REL pointer relocation manifest");
  char cmd[16384];make_secondary_llvm_command(cmd,sizeof(cmd),dolrecomp,output,jobs,1);char opts[96];snprintf(opts,sizeof(opts)," --rel-base 0x%08X",rel_base);strncat(cmd,opts,sizeof(cmd)-strlen(cmd)-1);qappend(cmd,sizeof(cmd),stage);qappend(cmd,sizeof(cmd),genroot);if(run_cmd(cmd))die("DolRecomp LLVM REL batch failed");StrVec headers={0};find_named(&headers,genroot,"generated.h");if(headers.n!=rels.n)die("DolRecomp LLVM REL batch emitted unexpected generated.h count");
  RelNativeJob* native=(RelNativeJob*)calloc(rels.n,sizeof(*native));if(!native)die("out of memory");
  size_t partial_hits=0;
  for(size_t i=0;i<rels.n;i++){
    RelInfo* r=&rels.v[i];RelNativeJob* job=&native[i];job->module_id=r->module_id;
    char outname[256],file[300],pn[128],bn[128];
    snprintf(outname,sizeof(outname),"g%s_rel_%u_%s",game,r->module_id,key);
    snprintf(file,sizeof(file),"%s%s",outname,suffix);path_join(job->module,sizeof(job->module),modules,file);
    snprintf(pn,sizeof(pn),"module-%u",r->module_id);path_join(job->project,sizeof(job->project),work,pn);
    snprintf(bn,sizeof(bn),"build-%u",r->module_id);path_join(job->build,sizeof(job->build),work,bn);
    if(is_file(job->module)){partial_hits++;continue;}
    char generated[MAX_PATHBUF];if(!find_generated_for_rel(&headers,r->module_id,generated,sizeof(generated)))die("cannot map generated REL output to module id");
    RangeVec code={0};for(uint32_t j=0;j<r->section_count;j++){RelSection* section=&r->sections[j];if(section->size&&section->executable&&!section->bss)rv_push(&code,section->linked,section->linked+section->size);}qsort(code.v,code.n,sizeof(Range),range_cmp);
    if(!generate_project(job->project,generated,dolsrc,gekko,modules,outname,game,&code,r,&canonical,NULL)){free(code.v);free(native);die("failed generating REL side project");}
    free(code.v);job->needs_build=1;
  }
  if(partial_hits)printf("GEKKOAOT_REL_PARTIAL_CACHE_HIT=1 modules=%zu total=%zu\n",partial_hits,rels.n);
  if(!build_rel_native_jobs(native,rels.n,jobs)){free(native);die("failed building REL side modules");}
  for(size_t i=0;i<rels.n;i++){if(!is_file(native[i].module)){free(native);die("REL native side module output missing");}sv_push_unique(built,native[i].module);}free(native);
  for(size_t i=0;i<headers.n;i++)free(headers.v[i]);
  free(headers.v);
 done: for(size_t i=0;i<rels.n;i++)free_rel(&rels.v[i]);free(rels.v);free(canonical.v);(void)files;return 1;}

static int build_fixed(const Candidate* c,const char* boot_dol,const char* dolrecomp,const char* dolsrc,const char* gekko,const char* output,const char* game,int jobs,StrVec* built){uint64_t h=g_secondary_toolchain_hash;h=fnv_bytes(h,SECONDARY_ROUTER_CACHE_TAG,sizeof(SECONDARY_ROUTER_CACHE_TAG)-1u);h=fnv_bytes(h,game,strlen(game));h=fnv_bytes(h,&c->kind,sizeof(c->kind));h=fnv_file(h,c->path);char key[32];snprintf(key,sizeof(key),"%016llx",(unsigned long long)h);char suffix[16];module_suffix(suffix,sizeof(suffix));char modules[MAX_PATHBUF];path_join(modules,sizeof(modules),output,"modules");mkdirs(modules);char outname[256];snprintf(outname,sizeof(outname),"g%s_exec_%s",game,key);char file[300],module[MAX_PATHBUF];snprintf(file,sizeof(file),"%s%s",outname,suffix);path_join(module,sizeof(module),modules,file);
  char work[MAX_PATHBUF],genroot[MAX_PATHBUF],compile_input[MAX_PATHBUF];char wn[512],st[256];stem(st,sizeof(st),c->path);snprintf(wn,sizeof(wn),"exec-%s-%s",st,key);path_join(work,sizeof(work),output,wn);path_join(genroot,sizeof(genroot),work,"dolrecomp");mkdirs(genroot);RangeVec code={0};uint32_t entry=0;if(c->kind==CAND_ELF){path_join(compile_input,sizeof(compile_input),work,"converted.dol");if(!elf_to_dol(c->path,compile_input,&code,&entry)){fprintf(stderr,"GEKKOAOT_EXEC_SKIP path=\"%s\" reason=unsupported-elf-layout\n",c->path);free(code.v);return 1;}}else{snprintf(compile_input,sizeof(compile_input),"%s",c->path);collect_dol_code(c->path,&code,&entry);}
  if(boot_dol&&is_file(boot_dol)&&dol_text_equivalent(compile_input,boot_dol)){printf("GEKKOAOT_EXEC_BOOT_DUPLICATE_V164=1 action=skip kind=%s source=\"%s\" boot=\"%s\" policy=entry+virtual-text-byte-equality\n",c->kind==CAND_ELF?"elf":"dol",c->path,boot_dol);free(code.v);return 1;}
  if(is_file(module)){sv_push(built,module);printf("GEKKOAOT_SECONDARY_AOT_CACHE_HIT=1 source=\"%s\"\n",c->path);free(code.v);return 1;}
  char cmd[16384];make_secondary_llvm_command(cmd,sizeof(cmd),dolrecomp,output,jobs,0);qappend(cmd,sizeof(cmd),compile_input);qappend(cmd,sizeof(cmd),genroot);if(run_cmd(cmd))die("DolRecomp LLVM fixed side executable failed");StrVec headers={0};find_named(&headers,genroot,"generated.h");if(headers.n!=1)die("fixed executable generated.h count is not 1");char generated[MAX_PATHBUF];dir_name(generated,sizeof(generated),headers.v[0]);char project[MAX_PATHBUF],build[MAX_PATHBUF];path_join(project,sizeof(project),work,"module");path_join(build,sizeof(build),work,"build");if(!generate_project(project,generated,dolsrc,gekko,modules,outname,game,&code,NULL,NULL,compile_input))die("failed generating fixed side project");if(!build_project(project,build,jobs))die("failed building fixed side module");if(!is_file(module))die("fixed native side module output missing");sv_push(built,module);for(size_t i=0;i<headers.n;i++)free(headers.v[i]);free(headers.v);free(code.v);return 1;}

static const char* arg_value(int argc,char** argv,const char* name){for(int i=1;i+1<argc;i++)if(!strcmp(argv[i],name))return argv[i+1];return NULL;}
int main(int argc,char** argv){
  const char* root=arg_value(argc,argv,"--root");
  const char* output=arg_value(argc,argv,"--output");
  const char* entrypoints_manifest=arg_value(argc,argv,"--entrypoints-manifest");

  /*
   * Cheap pre-pass used before main.dol recompilation.  It deliberately reuses
   * the production discovery path, so plain RELs, Yaz0 RELs and RARC-contained
   * RELs all contribute the same module-0 targets later used by side-module AOT.
   */
  if(entrypoints_manifest){
    if(!root||!output){
      fprintf(stderr,"usage: %s --root DISC_CACHE --output CACHE --entrypoints-manifest FILE\n",argv[0]);
      return 2;
    }
    char files[MAX_PATHBUF],boot_dol[MAX_PATHBUF];
    path_join(files,sizeof(files),root,"files");
    path_join(boot_dol,sizeof(boot_dol),root,"sys/main.dol");
    mkdirs(output);
    path_join(g_decoded_rel_root,sizeof(g_decoded_rel_root),output,"decoded-rel-entrypoints");
    CandidateVec found={0};
    if(is_dir(files))scan_tree(&found,files);
    qsort(found.v,found.n,sizeof(Candidate),cand_cmp);
    const int ok=write_main_entrypoints_manifest(&found,boot_dol,entrypoints_manifest);
    free(found.v);
    return ok?0:1;
  }

  const char* dol=arg_value(argc,argv,"--dolrecomp");
  const char* dolsrc=arg_value(argc,argv,"--dolrecomp-src");
  const char* gekko=arg_value(argc,argv,"--gekkoaot-root");
  const char* manifest=arg_value(argc,argv,"--manifest");
  const char* game=arg_value(argc,argv,"--game-id");
  const char* jobs_s=arg_value(argc,argv,"--jobs");
  if(!root||!dol||!dolsrc||!gekko||!output||!manifest||!game){
    fprintf(stderr,"usage: %s --root DISC_CACHE --dolrecomp EXE --dolrecomp-src DIR --gekkoaot-root DIR --output DIR --manifest FILE --game-id ID [--jobs N]\n",argv[0]);
    return 2;
  }
  int jobs=jobs_s?atoi(jobs_s):1;if(jobs<1)jobs=1;
  char files[MAX_PATHBUF],boot_dol[MAX_PATHBUF];
  path_join(files,sizeof(files),root,"files");
  path_join(boot_dol,sizeof(boot_dol),root,"sys/main.dol");
  mkdirs(output);
  path_join(g_decoded_rel_root,sizeof(g_decoded_rel_root),output,"decoded-rel");
  CandidateVec found={0};
  if(is_dir(files))scan_tree(&found,files);
  if(!discover_apploader_reboot(&found,root,output))die("cannot stage apploader reboot executable");
  qsort(found.v,found.n,sizeof(Candidate),cand_cmp);
  size_t relc=0,elfc=0,dolc=0;
  for(size_t i=0;i<found.n;i++){
    if(found.v[i].kind==CAND_REL)relc++;
    else if(found.v[i].kind==CAND_ELF)elfc++;
    else dolc++;
  }
  printf("GEKKOAOT_SECONDARY_DISCOVERY=1 rel=%zu elf=%zu dol=%zu\n",relc,elfc,dolc);
  StrVec built={0};
  if(found.n)g_secondary_toolchain_hash=secondary_toolchain_hash(dol,dolsrc,gekko);
  build_rel_modules(files,boot_dol,dol,dolsrc,gekko,output,game,jobs,&found,&built);
  size_t elfi=0,doli=0;
  for(size_t i=0;i<found.n;i++)if(found.v[i].kind!=CAND_REL){
    const char* kind=found.v[i].kind==CAND_ELF?"elf":"dol";
    size_t* current=found.v[i].kind==CAND_ELF?&elfi:&doli;
    size_t total=found.v[i].kind==CAND_ELF?elfc:dolc;
    (*current)++;
    printf("GEKKOAOT_SECONDARY_ITEM=1 kind=%s index=%zu total=%zu source=\"%s\"\n",
           kind,*current,total,found.v[i].path);
    build_fixed(&found.v[i],boot_dol,dol,dolsrc,gekko,output,game,jobs,&built);
  }
  char md[MAX_PATHBUF];dir_name(md,sizeof(md),manifest);mkdirs(md);
  FILE* mf=fopen(manifest,"wb");if(!mf)die("cannot write secondary manifest");
  for(size_t i=0;i<built.n;i++)fprintf(mf,"%s\n",built.v[i]);
  fclose(mf);
  printf("GEKKOAOT_SECONDARY_AOT=1 discovered=%zu built=%zu manifest=\"%s\"\n",
         found.n,built.n,manifest);
  for(size_t i=0;i<built.n;i++)free(built.v[i]);
  free(built.v);free(found.v);return 0;
}
