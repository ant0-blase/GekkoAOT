// SPDX-License-Identifier: GPL-3.0-or-later
// Test double for the native renderer ABI, not a GPU implementation.
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>
#include "../../runtime/gx/native_command_stream.h"
#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif
namespace { std::vector<std::uint8_t> bytes; }
namespace { std::vector<std::uint32_t> write_pcs, write_sizes; }
namespace { std::atomic_uint32_t ready_xfb{0}; }
namespace { std::atomic_bool hold_writes{false}; }
namespace { std::uint32_t legacy_polls = 0, async_polls = 0; }
using Resolve = bool (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t,
                         std::uint32_t, const void**, std::uint32_t*);
EXPORT bool gekkoaot_native_gx_init(Resolve, void*);
EXPORT void gekkoaot_native_gx_write_burst(const std::uint8_t*,std::uint32_t,std::uint32_t);
EXPORT std::uint32_t gekkoaot_test_gx_write_count() { return write_pcs.size(); }
EXPORT std::uint32_t gekkoaot_test_gx_write_pc(std::uint32_t index) { return write_pcs.at(index); }
EXPORT std::uint32_t gekkoaot_test_gx_write_size(std::uint32_t index) { return write_sizes.at(index); }
EXPORT std::uint32_t gekkoaot_test_gx_legacy_polls() { return legacy_polls; }
EXPORT std::uint32_t gekkoaot_test_gx_async_polls() { return async_polls; }
EXPORT void gekkoaot_test_gx_hold_writes(bool hold) { hold_writes.store(hold); }
EXPORT void gekkoaot_test_gx_publish_frame(std::uint32_t xfb) { ready_xfb.store(xfb, std::memory_order_release); }
EXPORT bool gekkoaot_native_gx_consume_frame_ready(std::uint32_t* xfb) {
  ++legacy_polls;
  auto value = ready_xfb.exchange(0, std::memory_order_acq_rel);
  if (xfb) *xfb = value;
  return value != 0;
}
#ifndef GEKKOAOT_TEST_LEGACY_GX
EXPORT bool gekkoaot_native_gx_consume_frame_ready_async(std::uint32_t* xfb) {
  ++async_polls;
  auto value = ready_xfb.exchange(0, std::memory_order_acq_rel);
  if (xfb) *xfb = value;
  return value != 0;
}
// v300 ordered host commands: record them in the same log as FIFO bursts so
// tests can check that the GX worker executes them in submission order.
namespace { std::vector<std::uint32_t> command_log; }
EXPORT void gekkoaot_native_gx_sdk_array(std::uint32_t index,std::uint32_t base,
                                       std::uint32_t stride,std::uint32_t before) {
  command_log.insert(command_log.end(), {0x41525200u,index,base,stride,before});
}
EXPORT void gekkoaot_native_gx_write_commands(const std::uint8_t* data,std::uint32_t size,
    std::uint32_t pc,const void* metadata,std::uint32_t count) {
  command_log.insert(command_log.end(),{0x4e415400u,count});
  std::uint32_t offset=0;
  for(unsigned i=0;i<count;++i) {
    std::uint32_t c[4];std::memcpy(c,static_cast<const std::uint8_t*>(metadata)+16*i,16);
    if(c[0]>offset)gekkoaot_native_gx_write_burst(data+offset,c[0]-offset,pc);
    gekkoaot_native_gx_sdk_array(c[1],c[2],c[3],0);offset=c[0];
  }
  if(size>offset)gekkoaot_native_gx_write_burst(data+offset,size-offset,pc);
}
EXPORT void gekkoaot_native_gx_write_sdk_commands_v317(const std::uint8_t* data,std::uint32_t size,
    std::uint32_t pc,const void* metadata,std::uint32_t count) {
  using namespace GekkoAOT::GX;
  const auto* commands=static_cast<const NativeSdkCommand*>(metadata);
  command_log.insert(command_log.end(),{0x4e415400u,count});
  std::uint32_t offset=0;
  for(unsigned i=0;i<count;++i) {
    const auto& c=commands[i];
    if(c.offset>offset)gekkoaot_native_gx_write_burst(data+offset,c.offset-offset,pc);
    if(c.kind==NativeSdkKind::Array)gekkoaot_native_gx_sdk_array(c.a,c.b,c.c,0);
    else command_log.insert(command_log.end(),{0x4d415400u,c.a,c.b});
    offset=c.offset;
  }
  if(size>offset)gekkoaot_native_gx_write_burst(data+offset,size-offset,pc);
}
EXPORT void gekkoaot_native_gx_memory_write(std::uint32_t address, std::uint32_t size) {
  command_log.insert(command_log.end(), {0x4d454d00u, address, size});
}
EXPORT void gekkoaot_native_gx_present_xfb_async(std::uint32_t top, std::uint32_t bottom,
                                                 std::uint32_t width, std::uint32_t stride,
                                                 std::uint32_t field_height) {
  command_log.insert(command_log.end(), {0x50525300u, top, bottom, width, stride, field_height});
}
EXPORT std::uint32_t gekkoaot_test_gx_command_count() { return command_log.size(); }
EXPORT std::uint32_t gekkoaot_test_gx_command(std::uint32_t i) { return command_log.at(i); }
#endif
EXPORT void gekkoaot_native_gx_write(std::uint64_t value, std::uint8_t size, std::uint32_t) {
  for (unsigned i = 0; i < size; ++i) bytes.push_back(value >> ((size - i - 1) * 8));
}
EXPORT void gekkoaot_native_gx_write_burst(const std::uint8_t* data, std::uint32_t size, std::uint32_t pc) {
  while (hold_writes.load()) std::this_thread::yield();
  write_pcs.push_back(pc);
  write_sizes.push_back(size);
  bytes.insert(bytes.end(), data, data + size);
#ifndef GEKKOAOT_TEST_LEGACY_GX
  command_log.insert(command_log.end(), {0x42555200u, size});
#endif
}
EXPORT bool gekkoaot_native_gx_init(Resolve, void*) {
  bytes.clear(); write_pcs.clear(); write_sizes.clear(); ready_xfb.store(0); hold_writes.store(false);
  legacy_polls = async_polls = 0;
#ifndef GEKKOAOT_TEST_LEGACY_GX
  command_log.clear();
#endif
  return true;
}
EXPORT void gekkoaot_native_gx_present() {}
EXPORT void gekkoaot_native_gx_present_xfb(std::uint32_t, std::uint32_t) {}
EXPORT void gekkoaot_native_gx_shutdown() {}
EXPORT std::uint32_t gekkoaot_test_gx_bytes() { return bytes.size(); }
EXPORT std::uint8_t gekkoaot_test_gx_byte(std::uint32_t i) { return bytes.at(i); }

namespace { void (*test_pe_callback)(void*,std::uint32_t,std::uint32_t)=nullptr;void* test_pe_user=nullptr; }
EXPORT void gekkoaot_native_gx_set_pe_callback(void (*callback)(void*,std::uint32_t,std::uint32_t),void* user) {test_pe_callback=callback;test_pe_user=user;}
EXPORT void gekkoaot_test_gx_publish_pe(std::uint32_t reg,std::uint32_t value) {if(test_pe_callback)test_pe_callback(test_pe_user,reg,value);}
