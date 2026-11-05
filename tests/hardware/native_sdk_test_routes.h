// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside HardwareTestAccess: run the same behavioral assertions
// through the generated constant-kind implementation, without bypassing or
// weakening the production entry-admission tests elsewhere.
static bool NativeSdkTestCall(HostRuntime& r,SimpleHleKind kind,std::uint32_t pc) {
  if(!std::getenv("GEKKOAOT_TEST_NATIVE_SPECIALIZATIONS"))return r.DispatchSimpleHle(kind,pc);
  switch(kind) {
#define SDK_SIMPLE(id, name, abi) case SimpleHleKind::id: return r.DispatchSimpleHleNative<static_cast<int>(SimpleHleKind::id)>(kind,pc);
#define SDK_OS(id, name, abi)
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
  default:return false;
  }
}
