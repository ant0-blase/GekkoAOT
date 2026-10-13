// SPDX-License-Identifier: GPL-3.0-or-later
// Compile the production helpers; all inputs below are independently authored.
#define main secondary_cli_main
#include "../../tools/secondary_build.c"
#undef main
#include <time.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif
#define CHECK(x) do { if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);exit(1);} } while(0)
static void write_text(const char* path,const char* text){
  char parent[MAX_PATHBUF];dir_name(parent,sizeof(parent),path);CHECK(mkdirs(parent));
  FILE* f=fopen(path,"wb");CHECK(f);CHECK(fputs(text,f)>=0);CHECK(!fclose(f));
}
static void write_dol_fixture(const char* path,uint32_t size,uint32_t entry,int change_code,int change_padding){
  unsigned char bytes[0x120]={0};
  put_be32(bytes,0x100u);
  put_be32(bytes+0x48,0x80003100u);
  put_be32(bytes+0x90,size);
  put_be32(bytes+0xe0,entry);
  put_be32(bytes+0x100,change_code?0x4e800420u:0x4e800020u);
  if(change_padding)bytes[0x104]=1u;
  FILE* f=fopen(path,"wb");CHECK(f);CHECK(fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes));CHECK(!fclose(f));
}
static double seconds(void){struct timespec now;timespec_get(&now,TIME_UTC);return now.tv_sec+now.tv_nsec*1e-9;}
int main(int argc,char** argv){
  CHECK(argc==2);char work[MAX_PATHBUF];
  snprintf(work,sizeof(work),"%s-%llu",argv[1],(unsigned long long)time(NULL));CHECK(mkdirs(work));
  char path[MAX_PATHBUF],build[MAX_PATHBUF],project[MAX_PATHBUF];
  path_join(path,sizeof(path),work,"generated.c");write_text(path,"first\n");
#ifdef _WIN32
  struct _utimbuf old_time={123456789,123456789};CHECK(!_utime(path,&old_time));
#else
  struct utimbuf old_time={123456789,123456789};CHECK(!utime(path,&old_time));
#endif
  FILE* f=tmpfile();CHECK(f);fputs("first\n",f);CHECK(finish_generated_file(f,path));
  struct stat st;CHECK(!stat(path,&st));CHECK(st.st_mtime==old_time.modtime);
  f=tmpfile();CHECK(f);fputs("second\n",f);CHECK(finish_generated_file(f,path));
  size_t n;unsigned char* data=read_file(path,&n);CHECK(data&&n==7&&!memcmp(data,"second\n",7));free(data);

  char compiler[MAX_PATHBUF],source[MAX_PATHBUF],root[MAX_PATHBUF];
  path_join(compiler,sizeof(compiler),work,"compiler");write_text(compiler,"compiler-version-1");
  char probe_build[MAX_PATHBUF],probe_cache[MAX_PATHBUF],probe_text[3*MAX_PATHBUF+256];
  path_join(probe_build,sizeof(probe_build),work,"cache-types");CHECK(mkdirs(probe_build));
  path_join(probe_cache,sizeof(probe_cache),probe_build,"CMakeCache.txt");
  for(unsigned types=0;types<8;types++){
    snprintf(probe_text,sizeof(probe_text),
             "CMAKE_C_COMPILER:%s=%s\r\nCMAKE_MAKE_PROGRAM:%s=%s\r\nCMAKE_COMMAND:%s=%s\r\n",
             types&1?"STRING":"FILEPATH",compiler,
             types&2?"STRING":"FILEPATH",compiler,
             types&4?"STRING":"INTERNAL",compiler);
    write_text(probe_cache,probe_text);CHECK(reusable_build_tree(probe_build));
  }
  write_text(probe_cache,"CMAKE_C_COMPILER:STRING=/missing/compiler\n");
  CHECK(!reusable_build_tree(probe_build));
  path_join(source,sizeof(source),work,"src");path_join(root,sizeof(root),work,"root");
  const char* inputs[]={"src/cpu/cpu.c","src/cpu/cpu.h","src/common/types.h"};
  for(size_t i=0;i<3;i++){path_join(path,sizeof(path),source,inputs[i]);write_text(path,"abi1");}
  path_join(path,sizeof(path),root,"runtime/core/module_abi.h");write_text(path,"abi1");
  const uint64_t first=secondary_toolchain_hash(compiler,source,root);
  CHECK(first==secondary_toolchain_hash(compiler,source,root));
  write_text(path,"abi2");CHECK(first!=secondary_toolchain_hash(compiler,source,root));
  write_text(path,"abi1");write_text(compiler,"compiler-version-2");
  CHECK(first!=secondary_toolchain_hash(compiler,source,root));
  write_text(compiler,"compiler-version-1");
  // The SDK restart trailer is executable despite having no FST .dol file.
  // Stage only the trailer; malformed lengths and RAM overflow fail closed.
  unsigned char apploader[0x80]={0};
  put_be32(apploader+0x10,0x81200000u);
  put_be32(apploader+0x14,0x20u);
  put_be32(apploader+0x18,0x40u);
  put_be32(apploader+0x40,0x48000005u);
  put_be32(apploader+0x44,0x4e800020u);
  path_join(path,sizeof(path),root,"sys/apploader.img");
  char parent[MAX_PATHBUF];dir_name(parent,sizeof(parent),path);CHECK(mkdirs(parent));
  f=fopen(path,"wb");CHECK(f);CHECK(fwrite(apploader,1,sizeof(apploader),f)==sizeof(apploader));CHECK(!fclose(f));
  CandidateVec reboot={0};CHECK(discover_apploader_reboot(&reboot,root,work));CHECK(reboot.n==1);
  data=read_file(reboot.v[0].path,&n);CHECK(data&&n==0x160u);
  CHECK(be32(data+0xe0)==0x81300000u&&be32(data+0x48)==0x81200000u);
  CHECK(be32(data+0x4c)==0x81300000u&&be32(data+4)==0x120u);
  CHECK(!memcmp(data+0x100,apploader+0x20,0x60));free(data);free(reboot.v);
  const uint32_t invalid_reboot_sizes[]={0u,0x41u,0x500004u,0xfffffffcu};
  for(size_t i=0;i<sizeof(invalid_reboot_sizes)/sizeof(invalid_reboot_sizes[0]);++i){
    put_be32(apploader+0x18,invalid_reboot_sizes[i]);
    f=fopen(path,"wb");CHECK(f);CHECK(fwrite(apploader,1,sizeof(apploader),f)==sizeof(apploader));CHECK(!fclose(f));
    reboot=(CandidateVec){0};CHECK(discover_apploader_reboot(&reboot,root,work));CHECK(!reboot.n);free(reboot.v);
  }
  put_be32(apploader+0x18,0x40u);put_be32(apploader+0x14,0xfffffffcu);
  f=fopen(path,"wb");CHECK(f);CHECK(fwrite(apploader,1,sizeof(apploader),f)==sizeof(apploader));CHECK(!fclose(f));
  reboot=(CandidateVec){0};CHECK(discover_apploader_reboot(&reboot,root,work));CHECK(!reboot.n);free(reboot.v);
  char short_dol[MAX_PATHBUF],padded_dol[MAX_PATHBUF];
  path_join(short_dol,sizeof(short_dol),work,"short.dol");
  path_join(padded_dol,sizeof(padded_dol),work,"padded.dol");
  write_dol_fixture(short_dol,4u,0x80003100u,0,0);
  write_dol_fixture(padded_dol,32u,0x80003100u,0,0);
  CHECK(dol_text_equivalent(short_dol,padded_dol));
  write_dol_fixture(padded_dol,32u,0x80003100u,0,1);
  CHECK(!dol_text_equivalent(short_dol,padded_dol));
  // Ordinary resource containers can resemble a REL section table. A version
  // zero header (as in Luigi's room data) must never enter the AOT compiler.
  path_join(path,sizeof(path),work,"resource.bin");
  unsigned char rel_bytes[0x90]={0};
  put_be32(rel_bytes,1u);put_be32(rel_bytes+0x0c,2u);
  put_be32(rel_bytes+0x10,0x40u);
  put_be32(rel_bytes+0x48,0x81u);put_be32(rel_bytes+0x4c,4u);
  f=fopen(path,"wb");CHECK(f);CHECK(fwrite(rel_bytes,1,sizeof(rel_bytes),f)==sizeof(rel_bytes));CHECK(!fclose(f));
  RelInfo rel={0};CHECK(!parse_rel(path,&rel));
  for(unsigned version=1;version<=3;version++){
    put_be32(rel_bytes+0x1c,version);
    f=fopen(path,"wb");CHECK(f);CHECK(fwrite(rel_bytes,1,sizeof(rel_bytes),f)==sizeof(rel_bytes));CHECK(!fclose(f));
    CHECK(parse_rel(path,&rel));CHECK(rel.sections[1].executable);free_rel(&rel);
  }
  // Compressed executable overlays must be discovered just like plain RELs.
  unsigned char compressed[16+sizeof(rel_bytes)+sizeof(rel_bytes)/8]={0};
  memcpy(compressed,"Yaz0",4);put_be32(compressed+4,sizeof(rel_bytes));
  size_t compressed_size=16;
  for(size_t i=0;i<sizeof(rel_bytes);i+=8){
    compressed[compressed_size++]=0xff;
    memcpy(compressed+compressed_size,rel_bytes+i,8);compressed_size+=8;
  }
  size_t decoded_size=0;data=decode_yaz0(compressed,compressed_size,&decoded_size);
  CHECK(data&&decoded_size==sizeof(rel_bytes)&&!memcmp(data,rel_bytes,decoded_size));free(data);
  CHECK(!decode_yaz0(compressed,compressed_size-1,&decoded_size));
  path_join(path,sizeof(path),work,"compressed.rel");
  f=fopen(path,"wb");CHECK(f);CHECK(fwrite(compressed,1,compressed_size,f)==compressed_size);CHECK(!fclose(f));
  path_join(g_decoded_rel_root,sizeof(g_decoded_rel_root),work,"decoded-rel");
  CandidateVec candidates={0};discover_file(&candidates,path);
  CHECK(candidates.n==1&&candidates.v[0].kind==CAND_REL);
  CHECK(parse_rel(candidates.v[0].path,&rel));CHECK(rel.module_id==1);free_rel(&rel);
  discover_file(&candidates,path);CHECK(candidates.n==1);free(candidates.v);
  // Both short and extended backreferences permit forward-overlapping copies.
  unsigned char repeated[22]={0};memcpy(repeated,"Yaz0",4);put_be32(repeated+4,24);
  repeated[16]=0x80;repeated[17]='a';repeated[18]=0x30;repeated[19]=0;
  repeated[20]=0;repeated[21]=0; // missing extended length
  CHECK(!decode_yaz0(repeated,sizeof(repeated),&decoded_size));
  unsigned char extended[23];memcpy(extended,repeated,sizeof(repeated));extended[22]=0;
  data=decode_yaz0(extended,sizeof(extended),&decoded_size);CHECK(data&&decoded_size==24);
  for(size_t i=0;i<decoded_size;i++)CHECK(data[i]=='a');free(data);
  extended[19]=1;CHECK(!decode_yaz0(extended,sizeof(extended),&decoded_size));
  extended[19]=0;put_be32(extended+4,23);CHECK(!decode_yaz0(extended,sizeof(extended),&decoded_size));
  // A RARC archive can carry the initial system REL as well as compressed
  // actor modules. Directory entries are metadata, not executable files.
  unsigned char archive[0x100+sizeof(rel_bytes)]={0};
  memcpy(archive,"RARC",4);put_be32(archive+4,sizeof(archive));
  put_be32(archive+8,0x20);put_be32(archive+12,0xe0);
  put_be32(archive+0x28,2);put_be32(archive+0x2c,0x20);
  put_be32(archive+0x30,16);put_be32(archive+0x34,0x60);
  put_be32(archive+0x44,0x02000000); // directory entry, safely ignored
  put_be32(archive+0x58,0x11000000); // file entry, name offset zero
  put_be32(archive+0x60,sizeof(rel_bytes));
  memcpy(archive+0x80,"system.rel",11);
  memcpy(archive+0x100,rel_bytes,sizeof(rel_bytes));
  path_join(path,sizeof(path),work,"RELS.arc");
  f=fopen(path,"wb");CHECK(f);CHECK(fwrite(archive,1,sizeof(archive),f)==sizeof(archive));CHECK(!fclose(f));
  candidates=(CandidateVec){0};discover_file(&candidates,path);CHECK(candidates.n==1);
  CHECK(parse_rel(candidates.v[0].path,&rel));CHECK(rel.module_id==1);free_rel(&rel);free(candidates.v);
  unsigned char packed_archive[16+sizeof(archive)+sizeof(archive)/8]={0};
  memcpy(packed_archive,"Yaz0",4);put_be32(packed_archive+4,sizeof(archive));
  size_t packed_size=16;
  for(size_t i=0;i<sizeof(archive);i+=8){
    packed_archive[packed_size++]=0xff;
    memcpy(packed_archive+packed_size,archive+i,8);packed_size+=8;
  }
  path_join(path,sizeof(path),work,"RELS.szs");
  f=fopen(path,"wb");CHECK(f);CHECK(fwrite(packed_archive,1,packed_size,f)==packed_size);CHECK(!fclose(f));
  candidates=(CandidateVec){0};discover_file(&candidates,path);CHECK(candidates.n==1);free(candidates.v);
  path_join(path,sizeof(path),work,"RELS.arc");
  put_be32(archive+0x5c,sizeof(archive)); // out-of-bounds file payload
  f=fopen(path,"wb");CHECK(f);CHECK(fwrite(archive,1,sizeof(archive),f)==sizeof(archive));CHECK(!fclose(f));
  candidates=(CandidateVec){0};discover_file(&candidates,path);CHECK(candidates.n==0);free(candidates.v);
  put_be32(rel_bytes+0x4c,3u); // executable sections contain complete instructions
  f=fopen(path,"wb");CHECK(f);CHECK(fwrite(rel_bytes,1,sizeof(rel_bytes),f)==sizeof(rel_bytes));CHECK(!fclose(f));
  CHECK(!parse_rel(path,&rel));
  write_dol_fixture(padded_dol,32u,0x80003100u,1,0);
  CHECK(!dol_text_equivalent(short_dol,padded_dol));
  write_dol_fixture(padded_dol,32u,0x80003104u,0,0);
  CHECK(!dol_text_equivalent(short_dol,padded_dol));

  // Module-0 REL relocations into DOL text are external native entry points.
  // This models Tales of Symphonia's battle REL -> main.dol transition.
  char entry_dol[MAX_PATHBUF],entry_rel[MAX_PATHBUF],entry_manifest[MAX_PATHBUF];
  path_join(entry_dol,sizeof(entry_dol),work,"entry.dol");
  write_dol_fixture(entry_dol,32u,0x80003100u,0,0);
  unsigned char linked_rel[0xa0]={0};
  put_be32(linked_rel,1u);                         // module id
  put_be32(linked_rel+0x0c,2u);                  // section count
  put_be32(linked_rel+0x10,0x50u);               // section table
  put_be32(linked_rel+0x1c,3u);                  // version
  put_be32(linked_rel+0x28,0x60u);               // import table
  put_be32(linked_rel+0x2c,8u);                  // one import
  put_be32(linked_rel+0x44,4u);put_be32(linked_rel+0x48,0x84u);
  put_be32(linked_rel+0x58,0x81u);put_be32(linked_rel+0x5c,4u); // exec section 1
  put_be32(linked_rel+0x60,0u);put_be32(linked_rel+0x64,0x68u); // module 0
  linked_rel[0x6a]=202u;linked_rel[0x6b]=1u;     // SECTION 1
  linked_rel[0x72]=10u;linked_rel[0x73]=2u;      // REL24 -> DOL text
  put_be32(linked_rel+0x74,0x80003110u);
  linked_rel[0x7a]=1u;linked_rel[0x7b]=2u;       // ADDR32 -> DOL text
  put_be32(linked_rel+0x7c,0x80003114u);
  linked_rel[0x82]=10u;linked_rel[0x83]=2u;      // outside DOL text, ignore
  put_be32(linked_rel+0x84,0x80400000u);
  linked_rel[0x8a]=203u;                         // END
  path_join(entry_rel,sizeof(entry_rel),work,"linked.rel");
  f=fopen(entry_rel,"wb");CHECK(f);
  CHECK(fwrite(linked_rel,1,sizeof(linked_rel),f)==sizeof(linked_rel));CHECK(!fclose(f));
  CHECK(parse_rel(entry_rel,&rel));free_rel(&rel);
  Candidate entry_candidate={0};snprintf(entry_candidate.path,sizeof(entry_candidate.path),"%s",entry_rel);
  entry_candidate.kind=CAND_REL;CandidateVec entry_candidates={&entry_candidate,1u,1u};
  path_join(entry_manifest,sizeof(entry_manifest),work,"entrypoints.txt");
  CHECK(write_main_entrypoints_manifest(&entry_candidates,entry_dol,entry_manifest));
  data=read_file(entry_manifest,&n);CHECK(data);
  CHECK(strstr((char*)data,"80003110\n"));CHECK(strstr((char*)data,"80003114\n"));
  CHECK(!strstr((char*)data,"80400000"));free(data);
#ifdef _WIN32
  _putenv_s("GEKKOAOT_AOT_CHUNK_INSTRUCTIONS","2048");
#else
  setenv("GEKKOAOT_AOT_CHUNK_INSTRUCTIONS","2048",1);
#endif
  CHECK(first!=secondary_toolchain_hash(compiler,source,root));
  const uint64_t tuned=secondary_toolchain_hash(compiler,source,root);
#ifdef _WIN32
  _putenv_s("GEKKOAOT_LLVM_COALESCE_LIMIT","16");
#else
  setenv("GEKKOAOT_LLVM_COALESCE_LIMIT","16",1);
#endif
  CHECK(tuned!=secondary_toolchain_hash(compiler,source,root));
  char command[16384];
  make_secondary_llvm_command(command,sizeof(command),compiler,work,1,1);
  CHECK(strstr(command,"DOLRECOMP_LLVM_COALESCE_LIMIT=16"));
  CHECK(strstr(command,"DOLRECOMP_REL_RUNTIME_TRANSLATE=1"));
  CHECK(strstr(command,"DOLRECOMP_NATIVE_ADDRESS_LOWERING=1"));
  CHECK(strstr(command,"DOLRECOMP_FP_REGION_COALESCE=1"));
  CHECK(strstr(command,"fpregion292-1"));
  const uint64_t fp_region_identity=secondary_toolchain_hash(compiler,source,root);
#ifdef _WIN32
  _putenv_s("GEKKOAOT_FP_REGION_COALESCE","0");
#else
  setenv("GEKKOAOT_FP_REGION_COALESCE","0",1);
#endif
  CHECK(fp_region_identity!=secondary_toolchain_hash(compiler,source,root));
  make_secondary_llvm_command(command,sizeof(command),compiler,work,1,1);
  CHECK(strstr(command,"DOLRECOMP_FP_REGION_COALESCE=0"));
  CHECK(strstr(command,"fpregion292-0"));
#ifdef _WIN32
  _putenv_s("GEKKOAOT_FP_REGION_COALESCE","");
#else
  unsetenv("GEKKOAOT_FP_REGION_COALESCE");
#endif
  // A fast path toggle must change both compiled object and module identity.
  make_secondary_llvm_command(command,sizeof(command),compiler,work,1,1);
  CHECK(strstr(command,"DOLRECOMP_NATIVE_FLAT_VMEM=1"));
  CHECK(strstr(command,"flat304-1"));
  const uint64_t flat_identity=secondary_toolchain_hash(compiler,source,root);
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_FLAT_VMEM","0");
#else
  setenv("GEKKOAOT_NATIVE_FLAT_VMEM","0",1);
#endif
  CHECK(flat_identity!=secondary_toolchain_hash(compiler,source,root));
  make_secondary_llvm_command(command,sizeof(command),compiler,work,1,1);
  CHECK(strstr(command,"DOLRECOMP_NATIVE_FLAT_VMEM=0"));
  CHECK(strstr(command,"flat304-0"));
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_FLAT_VMEM","");
#else
  unsetenv("GEKKOAOT_NATIVE_FLAT_VMEM");
#endif
  make_secondary_llvm_command(command,sizeof(command),compiler,work,1,1);
  CHECK(strstr(command,"DOLRECOMP_NATIVE_LOCKED_CACHE=1"));
  CHECK(strstr(command,"lc308-1"));
  const uint64_t lc_identity=secondary_toolchain_hash(compiler,source,root);
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_LOCKED_CACHE","0");
#else
  setenv("GEKKOAOT_NATIVE_LOCKED_CACHE","0",1);
#endif
  CHECK(lc_identity!=secondary_toolchain_hash(compiler,source,root));
  make_secondary_llvm_command(command,sizeof(command),compiler,work,1,1);
  CHECK(strstr(command,"DOLRECOMP_NATIVE_LOCKED_CACHE=0"));
  CHECK(strstr(command,"lc308-0"));
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_LOCKED_CACHE","");
#else
  unsetenv("GEKKOAOT_NATIVE_LOCKED_CACHE");
#endif
  const uint64_t address_identity=secondary_toolchain_hash(compiler,source,root);
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_ADDRESS_LOWERING","0");
#else
  setenv("GEKKOAOT_NATIVE_ADDRESS_LOWERING","0",1);
#endif
  CHECK(address_identity!=secondary_toolchain_hash(compiler,source,root));
  make_secondary_llvm_command(command,sizeof(command),compiler,work,1,1);
  CHECK(strstr(command,"DOLRECOMP_NATIVE_ADDRESS_LOWERING=0"));
  CHECK(strstr(command,"base0"));
#ifdef _WIN32
  _putenv_s("GEKKOAOT_NATIVE_ADDRESS_LOWERING","");
#else
  unsetenv("GEKKOAOT_NATIVE_ADDRESS_LOWERING");
#endif

  // Use a real CMake/Ninja build. A second build must retain its object and
  // a stale cached compiler path must still force the portable rebuild path.
  path_join(project,sizeof(project),work,"project");CHECK(mkdirs(project));
  path_join(path,sizeof(path),project,"CMakeLists.txt");
  write_text(path,"cmake_minimum_required(VERSION 3.25)\nproject(SideCache C)\nadd_executable(fixture fixture.c)\n"
                  "add_executable(capability-modern capabilities.c)\n"
                  "target_compile_definitions(capability-modern PRIVATE GEKKOAOT_AOT_RESTARTABLE_MEMORY=1 EXPECT_CAP=1)\n"
                  "add_executable(capability-legacy capabilities.c)\n"
                  "target_compile_definitions(capability-legacy PRIVATE EXPECT_CAP=0)\n"
                  "add_executable(capability-live capabilities.c)\n"
                  "target_compile_definitions(capability-live PRIVATE GEKKOAOT_AOT_RESTARTABLE_MEMORY=1 GEKKOAOT_AOT_REL_LIVE_GPRS=1 EXPECT_CAP=3)\n"
                  "add_executable(published-pointer published.c)\n"
                  "add_executable(direct-dispatch direct.c)\n");
  // Compile the actual generated secondary superchain against independent
  // wrappers and compare its direct and legacy dispatch modes in separate
  // processes. IRQ/host epochs must stop before another wrapper executes.
  Range fixture_range={0x80003100u,0x80003110u};RangeVec fixture_routes={&fixture_range,1,1};
  FILE* glue=tmpfile();CHECK(glue&&write_secondary_dispatch_glue(glue,&fixture_routes));
  CHECK(!fseek(glue,0,SEEK_END));long glue_size=ftell(glue);CHECK(glue_size>0);
  CHECK(!fseek(glue,0,SEEK_SET));char* glue_text=calloc((size_t)glue_size+1,1);CHECK(glue_text);
  CHECK(fread(glue_text,1,(size_t)glue_size,glue)==(size_t)glue_size);CHECK(!fclose(glue));
  char* glue_begin=strstr(glue_text,"typedef void (*SecondaryChunkFn)");
  char* glue_end=strstr(glue_text,"#define GEKKOAOT_GLOBAL_INDIRECT_QUERY");CHECK(glue_begin&&glue_end);
  path_join(path,sizeof(path),project,"direct.c");f=fopen(path,"wb");CHECK(f);
  fprintf(f,"#include <stdint.h>\n#include <stdlib.h>\n#include <stdio.h>\n#include <string.h>\n#include \"%s/module/secondary_dispatch_cache.h\"\n",GEKKOAOT_TEST_RUNTIME_DIR);
  fputs("typedef uint32_t u32;\n#define SECONDARY_LIKELY(x) (x)\n#define SECONDARY_UNLIKELY(x) (x)\n#define GC_RAM_BASE 0x80000000u\n"
        "struct CPUState {uint32_t pc,ram_size,exception;int64_t downcount,cycle_budget;uint8_t external_read_count,external_write_count,external_rid;uint32_t value;};\n"
        "static unsigned selected_calls;static int event;\n"
        "static void func_80003100(CPUState*c){c->value++;if(!(event&32)){c->pc=(event&128)?0x80003100:c->pc+4;c->downcount=(event&64)?INT64_MIN:c->downcount-1;}if((event&1)&&c->external_write_count!=255)c->external_write_count++;if((event&2)&&c->external_read_count!=255)c->external_read_count++;if(event&4)c->exception=1;if(event&8)c->external_rid=0xFD;if(event&16)c->pc=0;}\n"
        "static int dolrecomp_call(CPUState*c,u32 a){selected_calls++;if(a>=0x80003100&&a<0x80003110&&!(a&3)){c->pc=a;func_80003100(c);return 1;}return 0;}\n",f);
  CHECK(fwrite(glue_begin,1,(size_t)(glue_end-glue_begin),f)==(size_t)(glue_end-glue_begin));free(glue_text);
  fputs("int main(int argc,char**argv){if(argc!=4)return 1;\n",f);
#ifdef _WIN32
  fputs("_putenv_s(\"GEKKOAOT_SECONDARY_DIRECT_DISPATCH\",argv[1]);_putenv_s(\"GEKKOAOT_SECONDARY_INSTRUCTION_DISPATCH\",argv[2]);_putenv_s(\"GEKKOAOT_SECONDARY_BOUNDARY_HOIST\",argv[3]);\n",f);
#else
  fputs("setenv(\"GEKKOAOT_SECONDARY_DIRECT_DISPATCH\",argv[1],1);setenv(\"GEKKOAOT_SECONDARY_INSTRUCTION_DISPATCH\",argv[2],1);setenv(\"GEKKOAOT_SECONDARY_BOUNDARY_HOIST\",argv[3],1);\n",f);
#endif
  fputs("CPUState c={.pc=0x80003100,.ram_size=0x2000000,.cycle_budget=1000};event=1;"
        "if(!secondary_dispatch(&c,c.pc)||c.pc!=0x80003104||c.value!=1||c.downcount!=-1)return 2;"
        "event=0;c.external_write_count=255;if(!secondary_dispatch(&c,c.pc)||c.pc!=0x80003108||c.value!=2)return 3;"
        "c.external_write_count=0;if(!secondary_dispatch(&c,c.pc)||c.pc!=0x80003110||c.value!=4)return 4;"
        "for(unsigned a=0x800030f0;a<0x80003120;a++){c.pc=a;c.cycle_budget=0;int expected=a>=0x80003100&&a<0x80003110&&!(a&3);"
        "if(secondary_dispatch(&c,a)!=expected)return 5;}"
        "selected_calls=0;c.pc=0x80003100;if(!secondary_dispatch(&c,c.pc))return 6;"
        "if((argv[1][0]=='0')!=(selected_calls!=0))return 7;"
        "unsigned rng=0x294296;for(unsigned i=0;i<100000;i++){rng=rng*1664525u+1013904223u;CPUState x={0};x.pc=0x80003100+((rng>>28)&31);x.ram_size=0x2000000;x.cycle_budget=(i%19)?1+(rng&4095):0;x.downcount=-(int64_t)(rng&4095);x.external_read_count=(rng>>8)&255;x.external_write_count=(rng>>16)&255;event=(rng>>24)&127;CPUState y=x;int a=secondary_chain_legacy(&x,x.pc),b=secondary_dispatch(&y,y.pc);if(a!=b||memcmp(&x,&y,sizeof(x)))return 8;}"
        "for(unsigned i=0;i<8;i++){CPUState x={.pc=0x80003100,.cycle_budget=INT64_MAX};x.external_read_count=i&1?255:0;x.external_write_count=i&2?255:0;event=i&4?32:128;CPUState y=x;int a=secondary_chain_legacy(&x,x.pc),b=secondary_dispatch(&y,y.pc);if(a!=b||memcmp(&x,&y,sizeof(x)))return 9;}return 0;}\n",f);
  CHECK(!fclose(f));
  path_join(path,sizeof(path),project,"CMakeLists.txt");f=fopen(path,"ab");CHECK(f);
  fprintf(f,"target_include_directories(direct-dispatch PRIVATE \"%s\")\n",GEKKOAOT_TEST_RUNTIME_DIR);
  CHECK(!fclose(f));
  path_join(path,sizeof(path),project,"fixture.c");write_text(path,"int main(void){return 0;}\n");
  // Exercise the capability query emitted by the production exporter with
  // modern and legacy generated headers. Missing exports fence real overlays
  // out of paging-enabled dispatch even when their compiler supports restart.
  char generated[MAX_PATHBUF],side_project[MAX_PATHBUF];
  path_join(generated,sizeof(generated),work,"generated");CHECK(mkdirs(generated));
  path_join(path,sizeof(path),generated,"generated.h");
  write_text(path,"void func_80003100(CPUState*);\n");
  path_join(side_project,sizeof(side_project),work,"side-project");
  Range code_range={0x80003100u,0x80003104u};RangeVec code={&code_range,1u,1u};
  CHECK(generate_project(side_project,generated,source,root,work,"side","TEST01",&code,NULL,NULL,short_dol));
  path_join(path,sizeof(path),side_project,"secondary_module_export.c");
  data=read_file(path,&n);CHECK(data);
  char* query=strstr((char*)data,"GEKKOAOT_EXPORT uint32_t gekkoaot_module_capabilities(void)");
  CHECK(query);char* end=strstr(query,"}\n");CHECK(end);
  path_join(path,sizeof(path),project,"capabilities.c");f=fopen(path,"wb");CHECK(f);
  fputs("#include <stdint.h>\n#define GEKKOAOT_EXPORT\n#define GEKKOAOT_MODULE_CAP_MEMORY_RESTART 1u\n#define GEKKOAOT_MODULE_CAP_REL_LIVE_GPRS 2u\n",f);
  CHECK(fwrite(query,1,(size_t)(end+2-query),f)==(size_t)(end+2-query));
  fputs("int main(void){return gekkoaot_module_capabilities()!=EXPECT_CAP;}\n",f);
  CHECK(!fclose(f));free(data);
  // Run the production REL publishing bridge with independent code/data
  // addresses. Callback publication must translate; an internal store must
  // retain its canonical value, as must unrelated scalar values.
  RelSection sections[2]={{.index=1,.size=4,.executable=1,.linked=0x80510000u},
                          {.index=2,.size=4,.linked=0x80520000u}};
  RelInfo publishing_rel={.module_id=1,.version=3,.section_count=2,.sections=sections};
  snprintf(publishing_rel.path,sizeof(publishing_rel.path),"%s",entry_rel);
  Range address_ranges[2]={{0x80510000u,0x80510004u},{0x80520000u,0x80520004u}};
  RangeVec addresses={address_ranges,2,2};
  path_join(path,sizeof(path),source,"src/cpu/cpu.c");
  write_text(path,"static u8* resolve_addr(CPUState* cpu, u32 addr, u32* avail) {\nreturn 0;\n}\n");
  CHECK(generate_project(side_project,generated,source,root,work,"side","TEST01",
                         &code,&publishing_rel,&addresses,NULL));
  path_join(path,sizeof(path),side_project,"secondary_module_export.c");
  data=read_file(path,&n);CHECK(data);
  char* bridge=strstr((char*)data,"#define GEKKOAOT_REL_ADDRESS_QUERY");
  end=bridge?strstr(bridge,"static const GekkoAOTModuleDesc s_desc"):NULL;
  CHECK(bridge&&end);
  path_join(path,sizeof(path),project,"published.c");f=fopen(path,"wb");CHECK(f);
  fputs("#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
        "typedef uint32_t u32;typedef uint8_t u8;\n"
        "typedef struct CPUState {u32 external_addr,external_value;u8 external_rid;int(*host_call)(struct CPUState*,u32);} CPUState;\n"
        "static const struct {u32 linked_start,size;} s_rel_sections[]={{0x80510000,4},{0x80520000,4}};\n"
        "static const struct {u32 start,end;} s_rel_address_ranges[]={{0x80510000,0x80510004},{0x80520000,0x80520004}};\n"
        "static const struct {u32 start,end;} s_rel_own_data_ranges[]={{0x80520000,0x80520004}};\n"
        "#define SECONDARY_REL_ADDRESS_COUNT 2\n#define SECONDARY_REL_OWN_DATA_COUNT 1\n",f);
  CHECK(fwrite(bridge,1,(size_t)(end-bridge),f)==(size_t)(end-bridge));free(data);
  fputs("static int query(CPUState*c,u32 a){if(a!=0xfffffffa||c->external_rid!=0xfd)abort();c->external_value=c->external_addr+0x00d00020;c->external_rid=0xfe;return 1;}\n"
        "int main(void){CPUState c={.host_call=query};\n"
        "if(gekkoaot_rel_translate_published_pointer(&c,0x80350000,0x80510000)!=0x81210020)return 1;\n"
        "if(gekkoaot_rel_translate_published_pointer(&c,0x80350000,0x80520000)!=0x81220020)return 2;\n"
        "if(gekkoaot_rel_translate_published_pointer(&c,0x80520000,0x80510000)!=0x80510000)return 3;\n"
        "if(gekkoaot_rel_translate_published_pointer(&c,0xffffffff,0x80510000)!=0x80510000)return 4;\n"
        "if(gekkoaot_rel_translate_published_pointer(&c,0x80350000,0x80530000)!=0x80530000)return 5;return 0;}\n",f);
  CHECK(!fclose(f));
  // Relocation evidence distinguishes pointer materialization from an equal
  // integer literal, and memory-displacement relocations from loaded values.
  unsigned char pointer_rel[0x180]={0};
  put_be32(pointer_rel+0x28,0x130);put_be32(pointer_rel+0x2c,8);
  put_be32(pointer_rel+0x130,7);put_be32(pointer_rel+0x134,0x140);
  put_be32(pointer_rel+0x100,0x3c60805a); // lis r3
  put_be32(pointer_rel+0x104,0x3b833c18); // relocated addi r28,r3
  put_be32(pointer_rel+0x108,0x80a33c18); // relocated lwz r5,disp(r3)
  put_be32(pointer_rel+0x10c,0x60633c18); // relocated ori r3,r3
  put_be32(pointer_rel+0x110,0x3b833c18); // identical unrelocated scalar
  pointer_rel[0x142]=202;pointer_rel[0x143]=1;
  pointer_rel[0x149]=6;pointer_rel[0x14a]=4;pointer_rel[0x14b]=1;
  pointer_rel[0x151]=4;pointer_rel[0x152]=4;pointer_rel[0x153]=1;
  pointer_rel[0x159]=4;pointer_rel[0x15a]=4;pointer_rel[0x15b]=1;
  pointer_rel[0x162]=203;
  path_join(path,sizeof(path),work,"pointer-relocation.rel");
  f=fopen(path,"wb");CHECK(f);CHECK(fwrite(pointer_rel,1,sizeof(pointer_rel),f)==sizeof(pointer_rel));CHECK(!fclose(f));
  RelSection pointer_sections[2]={{0},{.offset=0x100,.size=0x20,.executable=1,.linked=0x80510100}};
  RelInfo pointer_info={.module_id=7,.section_count=2,.sections=pointer_sections};
  snprintf(pointer_info.path,sizeof(pointer_info.path),"%s",path);
  RelVec pointer_batch={&pointer_info,1,1};
  path_join(path,sizeof(path),work,"pointer-relocations.txt");
  CHECK(write_rel_pointer_manifest(&pointer_batch,path));
  data=read_file(path,&n);CHECK(data);
  CHECK(strstr((char*)data,"80510104 p\n"));
  CHECK(strstr((char*)data,"80510108 a\n"));
  CHECK(strstr((char*)data,"8051010c p\n"));
  CHECK(!strstr((char*)data,"80510110"));free(data);
  f=tmpfile();CHECK(f);CHECK(write_rel_import_proofs(f,&pointer_info));
  CHECK(!fseek(f,0,SEEK_SET));char proof_text[512]={0};
  CHECK(fread(proof_text,1,sizeof(proof_text)-1,f)>0);CHECK(!fclose(f));
  CHECK(strstr(proof_text,"{7u,40u,"));
  CHECK(strstr(proof_text,"SECONDARY_REL_IMPORT_PROOF_COUNT 1u"));
  path_join(build,sizeof(build),work,"build");double start=seconds();CHECK(build_project(project,build,1));
  double cold=seconds()-start;
  if(!reusable_build_tree(build)){
    path_join(path,sizeof(path),build,"CMakeCache.txt");data=read_file(path,&n);CHECK(data);
    for(char* line=strtok((char*)data,"\n");line;line=strtok(NULL,"\n"))
      if(!strncmp(line,"CMAKE_C_COMPILER:",17)||!strncmp(line,"CMAKE_MAKE_PROGRAM:",19)||!strncmp(line,"CMAKE_COMMAND:",14))
        fprintf(stderr,"Rejected build cache: %s\n",line);
    free(data);
  }
  CHECK(reusable_build_tree(build));
  for(unsigned i=0;i<3;i++){
#ifdef _WIN32
    const char* executable=i==2?"capability-live.exe":i?"capability-legacy.exe":"capability-modern.exe";
#else
    const char* executable=i==2?"capability-live":i?"capability-legacy":"capability-modern";
#endif
    path_join(path,sizeof(path),build,executable);command[0]=0;qappend(command,sizeof(command),path);
    CHECK(run_cmd(command)==0);
  }
#ifdef _WIN32
  path_join(path,sizeof(path),build,"published-pointer.exe");
#else
  path_join(path,sizeof(path),build,"published-pointer");
#endif
  command[0]=0;qappend(command,sizeof(command),path);CHECK(run_cmd(command)==0);
  path_join(path,sizeof(path),build,
#ifdef _WIN32
            "direct-dispatch.exe"
#else
            "direct-dispatch"
#endif
            );
  for(unsigned i=0;i<2;i++)for(unsigned image=0;image<2;image++)for(unsigned hoist=0;hoist<2;hoist++){command[0]=0;qappend(command,sizeof(command),path);qappend(command,sizeof(command),i?"1":"0");qappend(command,sizeof(command),image?"1":"0");qappend(command,sizeof(command),hoist?"1":"0");CHECK(run_cmd(command)==0);}
  char sentinel[MAX_PATHBUF];path_join(sentinel,sizeof(sentinel),build,"retained");write_text(sentinel,"retained");
  start=seconds();CHECK(build_project(project,build,1));double warm=seconds()-start;CHECK(is_file(sentinel));
  path_join(path,sizeof(path),build,"CMakeCache.txt");write_text(path,"CMAKE_C_COMPILER:FILEPATH=/expired/AppImage/compiler\n");
  CHECK(!reusable_build_tree(build));CHECK(build_project(project,build,1));CHECK(!is_file(sentinel));
  printf("Secondary build regression passed; cold=%.3fs incremental=%.3fs\n",cold,warm);
  return 0;
}
