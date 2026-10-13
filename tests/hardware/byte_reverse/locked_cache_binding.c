// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu/cpu.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define GEKKOAOT_EXPORT
#include "module/native_hardware_shims.inc"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);return 1;}}while(0)
static uint8_t* owner_base(CPUState* c){return c?(uint8_t*)c->external_user_data:NULL;}
int main(void){
  CPUState first={0},second={0},before={0};uint8_t a[32]={0},b[32]={0};
  first.external_user_data=a;second.external_user_data=b;
  CHECK(!gekkoaot_native_locked_cache_base(&first));
  CHECK(!gekkoaot_bind_native_locked_cache(NULL));
  GekkoAOTNativeLockedCache s={1,sizeof(s),NULL};
  CHECK(!gekkoaot_bind_native_locked_cache(&s));
  s.get_base=owner_base;s.version=2;CHECK(!gekkoaot_bind_native_locked_cache(&s));
  s.version=1;s.size--;CHECK(!gekkoaot_bind_native_locked_cache(&s));
  s.size=sizeof(s);CHECK(gekkoaot_bind_native_locked_cache(&s));
  for(unsigned i=0;i<131072;++i){CPUState* c=(i&1)?&first:&second;
    memcpy(&before,c,sizeof(before));
    CHECK(gekkoaot_native_locked_cache_base(c)==((i&1)?a:b));
    CHECK(!memcmp(&before,c,sizeof(before)));
  }
  // Invalid rebinding must not revoke a valid binding or change the old ABI.
  s.version=0;CHECK(!gekkoaot_bind_native_locked_cache(&s));
  CHECK(gekkoaot_native_locked_cache_base(&first)==a);
  CHECK(!gekkoaot_native_locked_cache_base(NULL));
  puts("LC module binding: absent, invalid version/size/function, valid publication, two owners and unchanged CPU state passed");
  return 0;
}
