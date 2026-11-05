// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/address_space.h"
#include <cstdlib>
#include <iostream>
#if defined(__linux__)
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace GekkoAOT::Native;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
int main() {
#if defined(__linux__)
  unsetenv("GEKKOAOT_NATIVE_ADDRESS_WINDOW");
  AddressSpace first(true), second;
  auto* base = first.NativeAddressBase();
  CHECK(base && second.NativeAddressBase() && base != second.NativeAddressBase());
  CHECK(first.Mem1().data() == base);
  CHECK(first.Mem2().data() == base + 0x10000000u);
  const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  unsigned char resident = 1;
  CHECK(mincore(base + 0x10000u, page, &resident) == 0 && !(resident & 1));
  // Independent expected aliases and widths: writes through real host views,
  // not through the same address decoder being tested.
  for (uint32_t offset : {0u, 4095u, 4096u, 0x017fffffu, 0x01ffffffu}) {
    base[0x80000000ull + offset] = 0x5a;
    CHECK(base[offset] == 0x5a && base[0xc0000000ull + offset] == 0x5a);
    CHECK(second.Mem1()[offset] == 0);
    base[0xc0000000ull + offset] = 0xa5;
    uint8_t value = 0; CHECK(first.Read8(offset, &value) && value == 0xa5);
  }
  base[0x93ffffffull] = 0x67;
  CHECK(base[0x13ffffffull] == 0x67 && base[0xd3ffffffull] == 0x67);
  CHECK(first.Write32(0x80004000u, 0x12345678u));
  CHECK(base[0xc0004000ull] == 0x12 && base[0xc0004003ull] == 0x78);
  base[0x7fffffffull] = 0x89;
  CHECK(first.FakeVmem().back() == 0x89);
  first.SetPagedVmem(true);
  CHECK(!first.Resolve(0x7e001000u, 1) && first.FakeVmem().empty());
  CHECK(!first.Resolve(0xcc000000u, 4) && !first.Resolve(0x82000000u, 1));
  for (uint32_t hole : {0x02000000u, 0x40000000u, 0x82000000u, 0xcc000000u, 0xffffffffu}) {
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
      rlimit no_core{0, 0}; setrlimit(RLIMIT_CORE, &no_core);
      volatile uint8_t byte = base[hole]; (void)byte; _exit(0);
    }
    int status = 0; CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
  }
  first.Clear();
  CHECK(first.NativeAddressBase() == base);
  CHECK(base[0xc1ffffffull] == 0 && base[0x93ffffffull] == 0 && base[0x7fffffffull] == 0);
  base[0xc0004000ull] = 0x42; CHECK(first.Mem1()[0x4000] == 0x42);
  first.Clear(); CHECK(base[0x80004000ull] == 0);
  // Unsupported/custom layouts and explicit opt-out retain the checked path.
  AddressSpace odd(4097, 0); CHECK(!odd.NativeAddressBase());
  CHECK(odd.Write8(0xc0001000u, 0xee)); odd.Clear();
  uint8_t value = 1; CHECK(odd.Read8(0x80001000u, &value) && !value);
  setenv("GEKKOAOT_NATIVE_ADDRESS_WINDOW", "0", 1);
  AddressSpace disabled; CHECK(!disabled.NativeAddressBase());
  CHECK(disabled.Write32(0xc0000010u, 0xabcdef01u));
  uint32_t word = 0; CHECK(disabled.Read32(0x80000010u, &word) && word == 0xabcdef01u);
  unsetenv("GEKKOAOT_NATIVE_ADDRESS_WINDOW");
#endif
  std::cout << "Guest address window aliases/guard holes/reset/isolation/fallback passed\n";
}
