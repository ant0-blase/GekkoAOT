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
#define MAX_REL_SECTIONS 4096u
#define SECONDARY_ROUTER_CACHE_TAG "gekkoaot-secondary-router-v7-global-indirect"

typedef struct { uint32_t index, offset, size; int executable, bss; uint32_t linked; } RelSection;
typedef struct {
  char path[MAX_PATHBUF];
  uint32_t module_id, version, section_count, section_info, file_size;
  uint32_t bss_size, bss_alignment, fix_size, base;
  RelSection* sections;
} RelInfo;

typedef enum { CAND_REL, CAND_DOL, CAND_ELF } CandidateKind;
typedef struct { char path[MAX_PATHBUF]; CandidateKind kind; } Candidate;
typedef struct { Candidate* v; size_t n, cap; } CandidateVec;
typedef struct { RelInfo* v; size_t n, cap; } RelVec;
typedef struct { char** v; size_t n, cap; } StrVec;
typedef struct { uint32_t start, end; } Range;
typedef struct { Range* v; size_t n, cap; } RangeVec;

static uint16_t be16(const unsigned char* p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put_be32(unsigned char* p, uint32_t v) { p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v; }
static uint32_t align_up(uint32_t v, uint32_t a) { uint64_t x=((uint64_t)v+a-1u)/a*a; return x>0xffffffffu?0:(uint32_t)x; }
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

static void cv_push(CandidateVec* a,const char* p,CandidateKind k){ if(a->n==a->cap){a->cap=a->cap?a->cap*2:32;a->v=(Candidate*)xrealloc(a->v,a->cap*sizeof(*a->v));} snprintf(a->v[a->n].path,sizeof(a->v[a->n].path),"%s",p);a->v[a->n].kind=k;a->n++; }
static void sv_push(StrVec* a,const char* s){ if(a->n==a->cap){a->cap=a->cap?a->cap*2:16;a->v=(char**)xrealloc(a->v,a->cap*sizeof(*a->v));}a->v[a->n++]=xstrdup(s); }
static void sv_push_unique(StrVec* a,const char* s){ for(size_t i=0;i<a->n;i++)if(!strcmp(a->v[i],s))return;sv_push(a,s); }
static void rv_push(RangeVec* a,uint32_t s,uint32_t e){ if(s>=e)return; if(a->n==a->cap){a->cap=a->cap?a->cap*2:16;a->v=(Range*)xrealloc(a->v,a->cap*sizeof(*a->v));}a->v[a->n++]=(Range){s,e}; }
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

static int parse_rel(const char* p,RelInfo* r){ memset(r,0,sizeof(*r)); size_t n=0; unsigned char* d=read_file(p,&n); if(!d||n<0x40){free(d);return 0;}
  uint32_t id=be32(d), sc=be32(d+0x0c), si=be32(d+0x10), ver=be32(d+0x1c); if(!id||!sc||sc>MAX_REL_SECTIONS||si<0x40u||(uint64_t)si+sc*8ull>n){free(d);return 0;}
  RelSection* secs=(RelSection*)calloc(sc,sizeof(*secs)); if(!secs){free(d);return 0;} int have_exec=0;
  for(uint32_t i=0;i<sc;i++){uint32_t raw=be32(d+si+i*8),sz=be32(d+si+i*8+4),off=raw&~3u; int ex=(raw&1u)!=0,bss=off==0&&sz!=0; if(!bss&&sz&&(uint64_t)off+sz>n){free(secs);free(d);return 0;} secs[i]=(RelSection){i,off,sz,ex,bss,0}; if(ex&&sz&&!bss)have_exec=1;}
  if(!have_exec){free(secs);free(d);return 0;} snprintf(r->path,sizeof(r->path),"%s",p);r->module_id=id;r->version=ver;r->section_count=sc;r->section_info=si;r->file_size=(uint32_t)n;r->bss_size=be32(d+0x20);r->bss_alignment=n>=0x48?be32(d+0x44):4;r->fix_size=(ver>=3&&n>=0x4c)?be32(d+0x48):(uint32_t)n; if(!r->bss_alignment)r->bss_alignment=4; if(!r->fix_size||r->fix_size>n){free(secs);free(d);return 0;}r->sections=secs;free(d);return 1; }
static void free_rel(RelInfo* r){free(r->sections);r->sections=NULL;}
static void assign_rel_sections(RelInfo* r){ uint32_t bss=align_up(r->base+r->fix_size,r->bss_alignment); for(uint32_t i=0;i<r->section_count;i++){RelSection* s=&r->sections[i]; if(!s->size)continue; if(s->bss){s->linked=bss;bss=align_up(bss+s->size,r->bss_alignment);}else s->linked=r->base+s->offset;} }

static void discover_file(CandidateVec* out,const char* p){ const char* ext=extension(p); RelInfo r;
  if(ieq(ext,".rel") && parse_rel(p,&r)){free_rel(&r);cv_push(out,p,CAND_REL);return;}
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
static int run_cmd(const char* cmd){printf("$ %s\n",cmd);fflush(stdout);int rc=system(cmd);
#ifdef _WIN32
  return rc;
#else
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
  const unsigned inline_bytes=env_u32("GEKKOAOT_NATIVE_INLINE_BYTES",1024u,64u,4096u);
  const unsigned inline_hint=env_u32("GEKKOAOT_NATIVE_INLINE_HINT_BYTES",4096u,256u,4096u);
  const unsigned relaxed=env_u32("GEKKOAOT_RELAXED_CALL_GUARD_BYTES",512u,64u,4096u);
  char cache[MAX_PATHBUF],num[64];
  path_join(cache,sizeof(cache),output,"llvm-cache-v1");
  if(!mkdirs(cache))die("cannot create secondary LLVM cache");

  snprintf(cmd,cap,"cmake -E env");
  append_env(cmd,cap,"DOLRECOMP_LLVM_CACHE",cache);
  append_env(cmd,cap,"DOLRECOMP_DISPATCH_LOOKUP","indexed");
  snprintf(num,sizeof(num),"%u",chunk);append_env(cmd,cap,"DOLRECOMP_LLVM_CHUNK_INSTRUCTIONS",num);
  snprintf(num,sizeof(num),"%u",cycles);append_env(cmd,cap,"DOLRECOMP_GUARD_CYCLES",num);
  snprintf(num,sizeof(num),"%u",steps);append_env(cmd,cap,"DOLRECOMP_GUARD_STEPS",num);
  append_env(cmd,cap,"DOLRECOMP_LLVM_CODEGEN_LEVEL","3");
  snprintf(num,sizeof(num),"%u",inline_bytes);append_env(cmd,cap,"DOLRECOMP_NATIVE_INLINE_BYTES",num);
  snprintf(num,sizeof(num),"%u",inline_hint);append_env(cmd,cap,"DOLRECOMP_NATIVE_INLINE_HINT_BYTES",num);
  snprintf(num,sizeof(num),"%u",relaxed);append_env(cmd,cap,"DOLRECOMP_RELAXED_CALL_GUARD_BYTES",num);
  if(rel_mode)append_env(cmd,cap,"DOLRECOMP_REL_RUNTIME_TRANSLATE","1");
  qappend(cmd,cap,dolrecomp);

  char opts[512];
  snprintf(opts,sizeof(opts),
           " -j%d --gamecube --backend=llvm --cpu gekko --targets=host --semantics=exact --partition-instructions %u",
           jobs,chunk);
  strncat(cmd,opts,cap-strlen(cmd)-1);
  if(!rel_mode)
    strncat(cmd," --state-in-memory --native-abi=unrestricted",cap-strlen(cmd)-1);

  printf("GEKKOAOT_SECONDARY_LLVM_AOT=1 kind=%s chunk=%u cycles=%u steps=%u cache=\"%s\" rel-translate=%d\n",
         rel_mode?"rel":"fixed",chunk,cycles,steps,cache,rel_mode?1:0);
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

static int range_contains(const RangeVec* ranges,uint32_t address){for(size_t i=0;i<ranges->n;i++)if(address>=ranges->v[i].start&&address<ranges->v[i].end)return 1;return 0;}
static int collect_generated_chunks(const char* generated,const RangeVec* code,RangeVec* chunks){char header[MAX_PATHBUF];path_join(header,sizeof(header),generated,"generated.h");size_t n=0;unsigned char* data=read_file(header,&n);if(!data)return 0;char* cursor=(char*)data;while((cursor=strstr(cursor,"void func_"))!=NULL){unsigned int address=0;if(sscanf(cursor+10,"%8x",&address)==1&&range_contains(code,(uint32_t)address))rv_push(chunks,(uint32_t)address,(uint32_t)address+4u);cursor+=10;}free(data);if(!chunks->n)return 0;qsort(chunks->v,chunks->n,sizeof(Range),range_cmp);size_t w=0;for(size_t i=0;i<chunks->n;i++){if(w&&chunks->v[w-1].start==chunks->v[i].start)continue;chunks->v[w++]=chunks->v[i];}chunks->n=w;for(size_t i=0;i<chunks->n;i++){uint32_t end=0;for(size_t r=0;r<code->n;r++)if(chunks->v[i].start>=code->v[r].start&&chunks->v[i].start<code->v[r].end){end=code->v[r].end;break;}if(!end){free(chunks->v);memset(chunks,0,sizeof(*chunks));return 0;}if(i+1<chunks->n&&chunks->v[i+1].start>chunks->v[i].start&&chunks->v[i+1].start<end)end=chunks->v[i+1].start;chunks->v[i].end=end;}return 1;}

static int patch_rel_cpu(const char* cpu_src,const char* dst){ size_t n=0; unsigned char* d=read_file(cpu_src,&n);if(!d)return 0;const char* anchor="static u8* resolve_addr(CPUState* cpu, u32 addr, u32* avail) {";char* at=strstr((char*)d,anchor);if(!at){free(d);return 0;}size_t head=(size_t)(at-(char*)d)+strlen(anchor);const char* ins="\n    extern u32 gekkoaot_rel_translate_address(CPUState*, u32);\n    addr = gekkoaot_rel_translate_address(cpu, addr);";char dd[MAX_PATHBUF];dir_name(dd,sizeof(dd),dst);mkdirs(dd);FILE* f=fopen(dst,"wb");if(!f){free(d);return 0;}fwrite(d,1,head,f);fwrite(ins,1,strlen(ins),f);fwrite(d+head,1,n-head,f);free(d);return fclose(f)==0; }

static int generate_project(const char* project,const char* generated,const char* dolrecomp_src,const char* gekko_root,const char* out_dir,const char* output_name,const char* game_id,const RangeVec* code,const RelInfo* rel,const RangeVec* canonical_all,const char* fixed_dol){
  if(!mkdirs(project))return 0;
  RangeVec chunks={0};
  if(!collect_generated_chunks(generated,code,&chunks)){fprintf(stderr,"[secondary-build] error: cannot recover DolRecomp chunk table from %s/generated.h\n",generated);return 0;}
  char tables[MAX_PATHBUF],exportc[MAX_PATHBUF],cmake[MAX_PATHBUF],cpu[MAX_PATHBUF];path_join(tables,sizeof(tables),project,"secondary_tables.inc");path_join(exportc,sizeof(exportc),project,"secondary_module_export.c");path_join(cmake,sizeof(cmake),project,"CMakeLists.txt");path_join(cpu,sizeof(cpu),project,"rel_cpu.c");
  FILE* t=fopen(tables,"wb");if(!t){free(chunks.v);return 0;}write_ranges(t,"s_code_ranges",code);fprintf(t,"#define SECONDARY_CODE_COUNT %zuu\n",code->n);write_ranges(t,"s_smc_ranges",&(RangeVec){0});fprintf(t,"#define SECONDARY_SMC_COUNT 0u\n");write_ranges(t,"s_chunk_ranges",&chunks);fprintf(t,"#define SECONDARY_CHUNK_COUNT %zuu\nstatic const uint64_t s_chunk_hashes[] = {\n",chunks.n);uint64_t* chunk_hashes=(uint64_t*)calloc(chunks.n?chunks.n:1u,sizeof(uint64_t));if(!chunk_hashes){fclose(t);free(chunks.v);return 0;}if(!rel&&fixed_dol&&!fixed_chunk_hashes(fixed_dol,&chunks,chunk_hashes)){fprintf(stderr,"[secondary-build] error: cannot hash fixed executable chunks from %s\n",fixed_dol);free(chunk_hashes);fclose(t);free(chunks.v);return 0;}for(size_t i=0;i<chunks.n;i++)fprintf(t,"  0x%016llXull,\n",(unsigned long long)chunk_hashes[i]);free(chunk_hashes);fprintf(t,"};\n");
  if(rel){fprintf(t,"static const GekkoAOTRelSection s_rel_sections[] = {\n");uint32_t count=0;for(uint32_t i=0;i<rel->section_count;i++){const RelSection* s=&rel->sections[i];if(!s->size)continue;fprintf(t,"  {%uu,%uu,0x%08Xu,0x%08Xu},\n",rel->module_id,s->index,s->linked,s->size);count++;}fprintf(t,"};\nstatic const GekkoAOTRelModule s_rel_modules[] = {\n  {%uu,%uu,%uu,0x%Xu,0x%Xu,s_rel_sections,%uu},\n};\n",rel->module_id,rel->version,rel->section_count,rel->section_info,rel->file_size,count);write_ranges(t,"s_rel_address_ranges",canonical_all);fprintf(t,"#define SECONDARY_REL_ADDRESS_COUNT %zuu\n",canonical_all->n);} fclose(t);
  FILE* e=fopen(exportc,"wb");if(!e){free(chunks.v);return 0;}fprintf(e,"// SPDX-License-Identifier: GPL-3.0-or-later\n#include <stdio.h>\n#include \"generated.h\"\n#include \"core/module_abi.h\"\n\ntypedef void (*SecondaryChunkFn)(CPUState*);\nstatic SecondaryChunkFn secondary_find_chunk(uint32_t address){\n");for(size_t i=0;i<chunks.n;i++)fprintf(e,"  if(address>=0x%08Xu&&address<0x%08Xu&&((address-0x%08Xu)&3u)==0u)return func_%08X;\n",chunks.v[i].start,chunks.v[i].end,chunks.v[i].start,chunks.v[i].start);fprintf(e,"  return 0;\n}\nstatic int secondary_dispatch(CPUState* ctx,uint32_t address){if(dolrecomp_call(ctx,address))return 1;uint32_t canonical=address;if(address<ctx->ram_size)canonical=address|GC_RAM_BASE;SecondaryChunkFn fn=secondary_find_chunk(canonical);if(!fn)return 0;static unsigned recover_logs;if(recover_logs<8u){fprintf(stderr,\"GEKKOAOT_SECONDARY_DISPATCH_RECOVER=1 address=0x%%08x canonical=0x%%08x\\n\",address,canonical);recover_logs++;}ctx->pc=canonical;fn(ctx);return 1;}\n#define GEKKOAOT_GLOBAL_INDIRECT_QUERY 0xFFFFFFF9u\n#define GEKKOAOT_GLOBAL_INDIRECT_PENDING 0xFCu\nvoid dolrecomp_indirect_dispatch(CPUState* ctx,u32 address){if(secondary_dispatch(ctx,address))return;if(!ctx||!ctx->host_call)return;u32 sa=ctx->external_addr,sv=ctx->external_value;u8 sr=ctx->external_rid;ctx->external_addr=address;ctx->external_value=address;ctx->external_rid=GEKKOAOT_GLOBAL_INDIRECT_PENDING;(void)ctx->host_call(ctx,GEKKOAOT_GLOBAL_INDIRECT_QUERY);ctx->external_addr=sa;ctx->external_value=sv;ctx->external_rid=sr;}\nstatic void secondary_loaded(CPUState* ctx){ppc_fpscr_control_updated(ctx);}\n#include \"secondary_tables.inc\"\n");
  if(rel){fprintf(e,"#define GEKKOAOT_REL_ADDRESS_QUERY 0xFFFFFFFAu\n#define GEKKOAOT_REL_ADDRESS_PENDING 0xFDu\n#define GEKKOAOT_REL_ADDRESS_HANDLED 0xFEu\nu32 gekkoaot_rel_translate_address(CPUState* ctx,u32 address){int candidate=0;for(u32 i=0;i<SECONDARY_REL_ADDRESS_COUNT;i++){if(address>=s_rel_address_ranges[i].start&&address<s_rel_address_ranges[i].end){candidate=1;break;}}if(!candidate||!ctx||!ctx->host_call)return address;u32 sa=ctx->external_addr,sv=ctx->external_value;u8 sr=ctx->external_rid;ctx->external_addr=address;ctx->external_value=address;ctx->external_rid=GEKKOAOT_REL_ADDRESS_PENDING;(void)ctx->host_call(ctx,GEKKOAOT_REL_ADDRESS_QUERY);u32 translated=ctx->external_rid==GEKKOAOT_REL_ADDRESS_HANDLED?ctx->external_value:address;ctx->external_addr=sa;ctx->external_value=sv;ctx->external_rid=sr;return translated;}\n");}
  fprintf(e,"static const GekkoAOTModuleDesc s_desc={GEKKOAOT_MODULE_ABI_VERSION,GEKKOAOT_CPU_ABI_VERSION,(uint32_t)sizeof(CPUState),\"%s\",0x%08Xu,secondary_dispatch,secondary_loaded,s_code_ranges,SECONDARY_CODE_COUNT,s_smc_ranges,SECONDARY_SMC_COUNT,s_chunk_ranges,SECONDARY_CHUNK_COUNT,s_chunk_hashes,",game_id,code->n?code->v[0].start:0u);if(rel)fprintf(e,"s_rel_modules,1u};\n");else fprintf(e,"0,0u};\n");fprintf(e,"#if defined(_WIN32)\n#define GEKKOAOT_EXPORT __declspec(dllexport)\n#elif defined(__GNUC__) || defined(__clang__)\n#define GEKKOAOT_EXPORT __attribute__((visibility(\"default\")))\n#else\n#define GEKKOAOT_EXPORT\n#endif\nGEKKOAOT_EXPORT const GekkoAOTModuleDesc* gekkoaot_get_module(void){return &s_desc;}\n");fclose(e);
  char cpu_src[MAX_PATHBUF];path_join(cpu_src,sizeof(cpu_src),dolrecomp_src,"src/cpu/cpu.c");if(rel&&!patch_rel_cpu(cpu_src,cpu)){free(chunks.v);return 0;}
  FILE* c=fopen(cmake,"wb");if(!c){free(chunks.v);return 0;}
  fprintf(c,"cmake_minimum_required(VERSION 3.24)\n"
            "project(GekkoAOTSecondary C)\n"
            "set(CMAKE_C_STANDARD 11)\n"
            "set(CMAKE_POSITION_INDEPENDENT_CODE ON)\n"
            "set(GENERATED_DIR \"");
  cmake_path(c,generated);
  fprintf(c,"\")\n"
            "file(GLOB_RECURSE LLVM_OBJECTS CONFIGURE_DEPENDS \"${GENERATED_DIR}/*.o\" \"${GENERATED_DIR}/*.obj\")\n"
            "file(GLOB_RECURSE C_INPUTS CONFIGURE_DEPENDS \"${GENERATED_DIR}/*.c\")\n"
            "if(LLVM_OBJECTS)\n"
            "  set_source_files_properties(${LLVM_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)\n"
            "  set(DOL_INPUTS ${LLVM_OBJECTS})\n"
            "  set(GEKKOAOT_SECONDARY_BACKEND llvm)\n"
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
            "include(CheckIPOSupported)\n"
            "check_ipo_supported(RESULT GEKKOAOT_SECONDARY_IPO_OK OUTPUT GEKKOAOT_SECONDARY_IPO_ERROR LANGUAGES C)\n"
            "if(GEKKOAOT_SECONDARY_IPO_OK)\n"
            "  set_property(TARGET gekkoaot_secondary PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)\n"
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
            "endif()\n");
  // rel_cpu.c is a patched copy of DolRecomp/src/cpu/cpu.c. Once copied into
  // the generated side-module directory, quoted includes such as "cpu.h" no
  // longer inherit DolRecomp/src/cpu as their source-file directory. Keep the
  // original CPU include directory explicit for both REL and fixed modules.
  fprintf(c,"target_include_directories(gekkoaot_secondary PRIVATE \"");cmake_path(c,dolrecomp_src);fprintf(c,"/src/cpu\")\n");
  fclose(c);printf("GEKKOAOT_SECONDARY_ROUTER=1 chunks=%zu source=%s\n",chunks.n,generated);free(chunks.v);return 1;
}

static int build_project(const char* project,const char* build,int jobs){char cmd[16384];snprintf(cmd,sizeof(cmd),"cmake -S");qappend(cmd,sizeof(cmd),project);strcat(cmd," -B");qappend(cmd,sizeof(cmd),build);strcat(cmd," -G Ninja -DCMAKE_BUILD_TYPE=Release");if(run_cmd(cmd))return 0;snprintf(cmd,sizeof(cmd),"cmake --build");qappend(cmd,sizeof(cmd),build);char tail[64];snprintf(tail,sizeof(tail)," -j%d",jobs);strcat(cmd,tail);return run_cmd(cmd)==0;}

static int find_generated_for_rel(const StrVec* headers,uint32_t module_id,char* out,size_t cap){char suffix[64];snprintf(suffix,sizeof(suffix),"_%u",module_id);for(size_t i=0;i<headers->n;i++){char d[MAX_PATHBUF];dir_name(d,sizeof(d),headers->v[i]);const char* b=base_name(d);size_t bn=strlen(b),sn=strlen(suffix);if(bn>=sn&&!strcmp(b+bn-sn,suffix)){snprintf(out,cap,"%s",d);return 1;}}return 0;}
static void collect_dol_code(const char* p,RangeVec* code,uint32_t* entry){size_t n=0;unsigned char* d=read_file(p,&n);if(!d||n<0x100)die("invalid DOL while collecting code");*entry=be32(d+0xe0);for(int i=0;i<7;i++){uint32_t a=be32(d+0x48+i*4),z=be32(d+0x90+i*4);if(z)rv_push(code,a,a+z);}free(d);qsort(code->v,code->n,sizeof(Range),range_cmp);}

static int elf_to_dol(const char* src,const char* dst,RangeVec* code,uint32_t* entry){size_t n=0;unsigned char* d=read_file(src,&n);if(!d||n<0x34||memcmp(d,"\x7f""ELF",4)||d[4]!=1||d[5]!=2||be16(d+0x12)!=20||be16(d+0x10)!=2){free(d);return 0;}*entry=be32(d+0x18);typedef struct{uint32_t off,addr,size;} Seg;Seg seg[7];int ns=0;uint32_t shoff=be32(d+0x20);uint16_t shents=be16(d+0x2e),shnum=be16(d+0x30);if(shoff&&shents>=40&&(uint64_t)shoff+(uint64_t)shents*shnum<=n){for(uint16_t i=0;i<shnum;i++){const unsigned char* s=d+shoff+i*shents;uint32_t type=be32(s+4),flags=be32(s+8),addr=be32(s+12),off=be32(s+16),sz=be32(s+20);if(type==8||!sz||(flags&6)!=6)continue;if((uint64_t)off+sz>n||addr<0x80000000u||(uint64_t)addr+sz>0x81800000ull||ns>=7){free(d);return 0;}seg[ns++]=(Seg){off,addr,sz};}}
  if(!ns){uint32_t phoff=be32(d+0x1c);uint16_t phents=be16(d+0x2a),phnum=be16(d+0x2c);if(!phoff||phents<32||(uint64_t)phoff+(uint64_t)phents*phnum>n){free(d);return 0;}for(uint16_t i=0;i<phnum;i++){const unsigned char* s=d+phoff+i*phents;uint32_t type=be32(s),off=be32(s+4),v=be32(s+8),pa=be32(s+12),sz=be32(s+16),flags=be32(s+24),addr=pa?pa:v;if(type!=1||!sz||!(flags&1))continue;if((uint64_t)off+sz>n||addr<0x80000000u||(uint64_t)addr+sz>0x81800000ull||ns>=7){free(d);return 0;}seg[ns++]=(Seg){off,addr,sz};}}
  if(!ns){free(d);return 0;}unsigned char hdr[0x100];memset(hdr,0,sizeof(hdr));uint32_t fileoff=0x100;for(int i=0;i<ns;i++){fileoff=align_up(fileoff,0x20);put_be32(hdr+i*4,fileoff);put_be32(hdr+0x48+i*4,seg[i].addr);put_be32(hdr+0x90+i*4,seg[i].size);rv_push(code,seg[i].addr,seg[i].addr+seg[i].size);fileoff+=seg[i].size;}put_be32(hdr+0xe0,*entry);char dd[MAX_PATHBUF];dir_name(dd,sizeof(dd),dst);mkdirs(dd);FILE* f=fopen(dst,"wb");if(!f){free(d);return 0;}if(fwrite(hdr,1,sizeof(hdr),f)!=sizeof(hdr)){fclose(f);free(d);return 0;}uint32_t pos=0x100;for(int i=0;i<ns;i++){uint32_t aligned=align_up(pos,0x20);while(pos<aligned){fputc(0,f);pos++;}if(fwrite(d+seg[i].off,1,seg[i].size,f)!=seg[i].size){fclose(f);free(d);return 0;}pos+=seg[i].size;}fclose(f);free(d);qsort(code->v,code->n,sizeof(Range),range_cmp);return 1;}

static int build_rel_modules(const char* files,const char* dolrecomp,const char* dolsrc,const char* gekko,const char* output,const char* game,int jobs,CandidateVec* all,StrVec* built){RelVec rels={0};uint64_t h=1469598103934665603ull;h=fnv_bytes(h,"secondary-v4-turbo",16);h=fnv_bytes(h,SECONDARY_ROUTER_CACHE_TAG,sizeof(SECONDARY_ROUTER_CACHE_TAG)-1u);h=fnv_bytes(h,game,strlen(game));h=fnv_bytes(h,&(uint32_t){REL_BASE},4);for(size_t i=0;i<all->n;i++)if(all->v[i].kind==CAND_REL){RelInfo r;if(!parse_rel(all->v[i].path,&r))continue;if(rels.n==rels.cap){rels.cap=rels.cap?rels.cap*2:8;rels.v=(RelInfo*)xrealloc(rels.v,rels.cap*sizeof(*rels.v));}rels.v[rels.n++]=r;h=fnv_file(fnv_bytes(h,&r.module_id,4),r.path);}if(!rels.n)return 1;
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
    if(unique.n&&!build_rel_modules(files,dolrecomp,dolsrc,gekko,output,game,jobs,&unique,built)){free(unique.v);free(overlays.v);return 0;}
    for(size_t i=0;i<overlays.n;i++){printf("GEKKOAOT_REL_OVERLAY_BUILD=1 index=%zu total=%zu source=\"%s\"\n",i+1,overlays.n,overlays.v[i].path);printf("GEKKOAOT_SECONDARY_ITEM=1 kind=rel index=%zu total=%zu source=\"%s\"\n",i+1,overlays.n,overlays.v[i].path);CandidateVec one={&overlays.v[i],1,1};if(!build_rel_modules(files,dolrecomp,dolsrc,gekko,output,game,jobs,&one,built)){free(unique.v);free(overlays.v);return 0;}}
    free(unique.v);free(overlays.v);return 1;
  }
  uint32_t cursor=REL_BASE;RangeVec canonical={0};for(size_t i=0;i<rels.n;i++){RelInfo* r=&rels.v[i];r->base=cursor;assign_rel_sections(r);for(uint32_t j=0;j<r->section_count;j++)if(r->sections[j].size)rv_push(&canonical,r->sections[j].linked,r->sections[j].linked+r->sections[j].size);uint64_t end=(uint64_t)r->base+r->file_size+r->bss_size;if(end>0xffffffffu)die("REL canonical address overflow");cursor=align_up((uint32_t)end,REL_ALIGN);if(!cursor)die("REL canonical alignment overflow");}qsort(canonical.v,canonical.n,sizeof(Range),range_cmp);
  char suffix[16];module_suffix(suffix,sizeof(suffix));char modules[MAX_PATHBUF];path_join(modules,sizeof(modules),output,"modules");mkdirs(modules);char key[32];snprintf(key,sizeof(key),"%016llx",(unsigned long long)h);int cache=1;for(size_t i=0;i<rels.n;i++){char name[256],p[MAX_PATHBUF];snprintf(name,sizeof(name),"g%s_rel_%u_%s%s",game,rels.v[i].module_id,key,suffix);path_join(p,sizeof(p),modules,name);if(!is_file(p)){cache=0;break;}}if(cache){for(size_t i=0;i<rels.n;i++){char name[256],p[MAX_PATHBUF];snprintf(name,sizeof(name),"g%s_rel_%u_%s%s",game,rels.v[i].module_id,key,suffix);path_join(p,sizeof(p),modules,name);sv_push_unique(built,p);}printf("GEKKOAOT_REL_AOT_CACHE_HIT=1 modules=%zu key=%s\n",rels.n,key);goto done;}
  char work[MAX_PATHBUF],stage[MAX_PATHBUF],genroot[MAX_PATHBUF];char wn[128];snprintf(wn,sizeof(wn),"rel-all-%s",key);path_join(work,sizeof(work),output,wn);path_join(stage,sizeof(stage),work,"rel-input");path_join(genroot,sizeof(genroot),work,"dolrecomp");mkdirs(stage);mkdirs(genroot);for(size_t i=0;i<rels.n;i++){char n[256],dst[MAX_PATHBUF];snprintf(n,sizeof(n),"%04zu_%u.rel",i,rels.v[i].module_id);path_join(dst,sizeof(dst),stage,n);if(!copy_file(rels.v[i].path,dst))die("failed staging REL");}
  char cmd[16384];make_secondary_llvm_command(cmd,sizeof(cmd),dolrecomp,output,jobs,1);char opts[96];snprintf(opts,sizeof(opts)," --rel-base 0x%08X",REL_BASE);strncat(cmd,opts,sizeof(cmd)-strlen(cmd)-1);qappend(cmd,sizeof(cmd),stage);qappend(cmd,sizeof(cmd),genroot);if(run_cmd(cmd))die("DolRecomp LLVM REL batch failed");StrVec headers={0};find_named(&headers,genroot,"generated.h");if(headers.n!=rels.n)die("DolRecomp LLVM REL batch emitted unexpected generated.h count");
  for(size_t i=0;i<rels.n;i++){RelInfo* r=&rels.v[i];char generated[MAX_PATHBUF];if(!find_generated_for_rel(&headers,r->module_id,generated,sizeof(generated)))die("cannot map generated REL output to module id");RangeVec code={0};for(uint32_t j=0;j<r->section_count;j++){RelSection* s=&r->sections[j];if(s->size&&s->executable&&!s->bss)rv_push(&code,s->linked,s->linked+s->size);}qsort(code.v,code.n,sizeof(Range),range_cmp);char outname[256],module[MAX_PATHBUF],project[MAX_PATHBUF],build[MAX_PATHBUF];snprintf(outname,sizeof(outname),"g%s_rel_%u_%s",game,r->module_id,key);char file[300];snprintf(file,sizeof(file),"%s%s",outname,suffix);path_join(module,sizeof(module),modules,file);char pn[128];snprintf(pn,sizeof(pn),"module-%u",r->module_id);path_join(project,sizeof(project),work,pn);char bn[128];snprintf(bn,sizeof(bn),"build-%u",r->module_id);path_join(build,sizeof(build),work,bn);if(!generate_project(project,generated,dolsrc,gekko,modules,outname,game,&code,r,&canonical,NULL))die("failed generating REL side project");if(!build_project(project,build,jobs))die("failed building REL side module");if(!is_file(module))die("REL native side module output missing");sv_push_unique(built,module);free(code.v);}for(size_t i=0;i<headers.n;i++)free(headers.v[i]);free(headers.v);
 done: for(size_t i=0;i<rels.n;i++)free_rel(&rels.v[i]);free(rels.v);free(canonical.v);(void)files;return 1;}

static int build_fixed(const Candidate* c,const char* dolrecomp,const char* dolsrc,const char* gekko,const char* output,const char* game,int jobs,StrVec* built){uint64_t h=1469598103934665603ull;h=fnv_bytes(h,"secondary-v4-turbo",16);h=fnv_bytes(h,SECONDARY_ROUTER_CACHE_TAG,sizeof(SECONDARY_ROUTER_CACHE_TAG)-1u);h=fnv_bytes(h,game,strlen(game));h=fnv_bytes(h,&c->kind,sizeof(c->kind));h=fnv_file(h,c->path);char key[32];snprintf(key,sizeof(key),"%016llx",(unsigned long long)h);char suffix[16];module_suffix(suffix,sizeof(suffix));char modules[MAX_PATHBUF];path_join(modules,sizeof(modules),output,"modules");mkdirs(modules);char outname[256];snprintf(outname,sizeof(outname),"g%s_exec_%s",game,key);char file[300],module[MAX_PATHBUF];snprintf(file,sizeof(file),"%s%s",outname,suffix);path_join(module,sizeof(module),modules,file);if(is_file(module)){sv_push(built,module);printf("GEKKOAOT_SECONDARY_AOT_CACHE_HIT=1 source=\"%s\"\n",c->path);return 1;}
  char work[MAX_PATHBUF],genroot[MAX_PATHBUF],compile_input[MAX_PATHBUF];char wn[512],st[256];stem(st,sizeof(st),c->path);snprintf(wn,sizeof(wn),"exec-%s-%s",st,key);path_join(work,sizeof(work),output,wn);path_join(genroot,sizeof(genroot),work,"dolrecomp");mkdirs(genroot);RangeVec code={0};uint32_t entry=0;if(c->kind==CAND_ELF){path_join(compile_input,sizeof(compile_input),work,"converted.dol");if(!elf_to_dol(c->path,compile_input,&code,&entry)){fprintf(stderr,"GEKKOAOT_EXEC_SKIP path=\"%s\" reason=unsupported-elf-layout\n",c->path);free(code.v);return 1;}}else{snprintf(compile_input,sizeof(compile_input),"%s",c->path);collect_dol_code(c->path,&code,&entry);}char cmd[16384];make_secondary_llvm_command(cmd,sizeof(cmd),dolrecomp,output,jobs,0);qappend(cmd,sizeof(cmd),compile_input);qappend(cmd,sizeof(cmd),genroot);if(run_cmd(cmd))die("DolRecomp LLVM fixed side executable failed");StrVec headers={0};find_named(&headers,genroot,"generated.h");if(headers.n!=1)die("fixed executable generated.h count is not 1");char generated[MAX_PATHBUF];dir_name(generated,sizeof(generated),headers.v[0]);char project[MAX_PATHBUF],build[MAX_PATHBUF];path_join(project,sizeof(project),work,"module");path_join(build,sizeof(build),work,"build");if(!generate_project(project,generated,dolsrc,gekko,modules,outname,game,&code,NULL,NULL,compile_input))die("failed generating fixed side project");if(!build_project(project,build,jobs))die("failed building fixed side module");if(!is_file(module))die("fixed native side module output missing");sv_push(built,module);for(size_t i=0;i<headers.n;i++)free(headers.v[i]);free(headers.v);free(code.v);return 1;}

static const char* arg_value(int argc,char** argv,const char* name){for(int i=1;i+1<argc;i++)if(!strcmp(argv[i],name))return argv[i+1];return NULL;}
int main(int argc,char** argv){const char* root=arg_value(argc,argv,"--root"),*dol=arg_value(argc,argv,"--dolrecomp"),*dolsrc=arg_value(argc,argv,"--dolrecomp-src"),*gekko=arg_value(argc,argv,"--gekkoaot-root"),*output=arg_value(argc,argv,"--output"),*manifest=arg_value(argc,argv,"--manifest"),*game=arg_value(argc,argv,"--game-id"),*jobs_s=arg_value(argc,argv,"--jobs");if(!root||!dol||!dolsrc||!gekko||!output||!manifest||!game){fprintf(stderr,"usage: %s --root DISC_CACHE --dolrecomp EXE --dolrecomp-src DIR --gekkoaot-root DIR --output DIR --manifest FILE --game-id ID [--jobs N]\n",argv[0]);return 2;}int jobs=jobs_s?atoi(jobs_s):1;if(jobs<1)jobs=1;char files[MAX_PATHBUF];path_join(files,sizeof(files),root,"files");CandidateVec found={0};if(is_dir(files))scan_tree(&found,files);qsort(found.v,found.n,sizeof(Candidate),cand_cmp);size_t relc=0,elfc=0,dolc=0;for(size_t i=0;i<found.n;i++){if(found.v[i].kind==CAND_REL)relc++;else if(found.v[i].kind==CAND_ELF)elfc++;else dolc++;}printf("GEKKOAOT_SECONDARY_DISCOVERY=1 rel=%zu elf=%zu dol=%zu\n",relc,elfc,dolc);mkdirs(output);StrVec built={0};build_rel_modules(files,dol,dolsrc,gekko,output,game,jobs,&found,&built);size_t elfi=0,doli=0;for(size_t i=0;i<found.n;i++)if(found.v[i].kind!=CAND_REL){const char* kind=found.v[i].kind==CAND_ELF?"elf":"dol";size_t* current=found.v[i].kind==CAND_ELF?&elfi:&doli;size_t total=found.v[i].kind==CAND_ELF?elfc:dolc;(*current)++;printf("GEKKOAOT_SECONDARY_ITEM=1 kind=%s index=%zu total=%zu source=\"%s\"\n",kind,*current,total,found.v[i].path);build_fixed(&found.v[i],dol,dolsrc,gekko,output,game,jobs,&built);}char md[MAX_PATHBUF];dir_name(md,sizeof(md),manifest);mkdirs(md);FILE* mf=fopen(manifest,"wb");if(!mf)die("cannot write secondary manifest");for(size_t i=0;i<built.n;i++)fprintf(mf,"%s\n",built.v[i]);fclose(mf);printf("GEKKOAOT_SECONDARY_AOT=1 discovered=%zu built=%zu manifest=\"%s\"\n",found.n,built.n,manifest);for(size_t i=0;i<built.n;i++)free(built.v[i]);free(built.v);free(found.v);return 0;}
