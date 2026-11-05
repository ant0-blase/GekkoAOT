"""Exercise every generated binding and reject stale/malformed service tables."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
class NativeSdkAbi(unittest.TestCase):
    def test_entire_generated_binding_table(self):
        source=r'''
#include <assert.h>
#include <stdint.h>
#if defined(_WIN32)
#define GEKKOAOT_EXPORT __declspec(dllexport)
#else
#define GEKKOAOT_EXPORT __attribute__((visibility("default")))
#endif
#include "module/native_sdk_shims.h"
struct CPUState {unsigned calls,owner;};
#define SDK_SIMPLE(kind,name,abi) static uint32_t service_simple_##kind(CPUState* cpu){++cpu->calls;return GekkoAOTSdkSimple_##kind+1+cpu->owner;}
#define SDK_OS(kind,name,abi) static uint32_t service_os_##kind(CPUState* cpu){++cpu->calls;return GekkoAOTSdkOs_##kind+1+cpu->owner;}
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
static GekkoAOTNativeSdk table={2,sizeof(GekkoAOTNativeSdk),GekkoAOTNativeSdkCount,0,0,{
#define SDK_SIMPLE(kind,name,abi) service_simple_##kind,
#define SDK_OS(kind,name,abi) service_os_##kind,
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
}};
int main(void){
 CPUState a={0,0},b={0,1000};
#define SDK_SIMPLE(kind,name,abi) assert(!gekkoaot_sdk_simple_##kind(&a));
#define SDK_OS(kind,name,abi) assert(!gekkoaot_sdk_os_##kind(&a));
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
 assert(!gekkoaot_bind_native_sdk(0));assert(!gekkoaot_bind_native_sdk(&table));
 table.registry_hash=gekkoaot_native_sdk_registry_hash();
 ++table.version;assert(!gekkoaot_bind_native_sdk(&table));--table.version;
 --table.size;assert(!gekkoaot_bind_native_sdk(&table));++table.size;
 --table.count;assert(!gekkoaot_bind_native_sdk(&table));++table.count;
 table.reserved=1;assert(!gekkoaot_bind_native_sdk(&table));table.reserved=0;
 GekkoAOTNativeSdkService saved=table.entries[0];table.entries[0]=0;
 assert(!gekkoaot_bind_native_sdk(&table));table.entries[0]=saved;
 assert(gekkoaot_bind_native_sdk(&table));
#define SDK_SIMPLE(kind,name,abi) assert(gekkoaot_sdk_simple_##kind(&a)==GekkoAOTSdkSimple_##kind+1);assert(gekkoaot_sdk_simple_##kind(&b)==GekkoAOTSdkSimple_##kind+1001);
#define SDK_OS(kind,name,abi) assert(gekkoaot_sdk_os_##kind(&a)==GekkoAOTSdkOs_##kind+1);assert(gekkoaot_sdk_os_##kind(&b)==GekkoAOTSdkOs_##kind+1001);
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
 assert(a.calls==GekkoAOTNativeSdkCount && b.calls==GekkoAOTNativeSdkCount);
 // Failed rebinding cannot poison an already loaded game module.
 table.registry_hash^=1;assert(!gekkoaot_bind_native_sdk(&table));
 assert(gekkoaot_sdk_simple_Memcpy(&a)==GekkoAOTSdkSimple_Memcpy+1);
 return 0;
}'''
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp);(p/'abi.c').write_text(source)
            compilers=list(dict.fromkeys(filter(None,(shutil.which(os.environ.get('CC','cc')),shutil.which('clang')))))
            self.assertTrue(compilers)
            for index,cc in enumerate(compilers):
                subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'runtime'),str(p/'abi.c'),'-o',str(p/f'abi{index}')],check=True,capture_output=True)
                subprocess.run([str(p/f'abi{index}')],check=True)
            cross=shutil.which('x86_64-w64-mingw32-gcc')
            if cross:
                subprocess.run([cross,'-std=c11','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'runtime'),str(p/'abi.c'),'-c','-o',str(p/'windows-abi.o')],check=True,capture_output=True)
if __name__=='__main__':unittest.main()
