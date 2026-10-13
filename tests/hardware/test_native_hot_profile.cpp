// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_hot_profile.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) std::abort(); } while (false)
int main() {
 const std::string header="# gekkoaot-native-hot-v285 dol-fnv64=123456789abcdef0\n";
 std::istringstream good(header+"# evidence\n80001000\n80002000\n");
 auto values=GekkoAOT::Build::ReadNativeHotProfile(good,"123456789abcdef0");
 CHECK(values.size()==2 && values[0]==0x80001000 && values[1]==0x80002000);
 for (const std::string& body : {"", "80001001\n", "80002000\n80001000\n", "80001000\n80001000\n", "80001000 garbage\n", "100000000\n", "-80001000\n"}) {
   std::istringstream bad(header+body);bool rejected=false;
   try { (void)GekkoAOT::Build::ReadNativeHotProfile(bad,"123456789abcdef0"); } catch(const std::runtime_error&) { rejected=true; }
   CHECK(rejected);
 }
 std::istringstream wrong(header+"80001000\n");bool rejected=false;
 try { (void)GekkoAOT::Build::ReadNativeHotProfile(wrong,"ffffffffffffffff"); } catch(const std::runtime_error&) { rejected=true; }
 CHECK(rejected);
 std::cout << "native hot profile: source identity, strict address validation and independent input passed\n";
}
