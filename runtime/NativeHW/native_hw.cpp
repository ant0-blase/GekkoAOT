// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeHW/native_hw.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace GekkoAOT::NativeHW {
namespace {
bool Flag(const char* name, bool fallback) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
         std::strcmp(value, "off") != 0 && std::strcmp(value, "no") != 0;
}
}

Service::Service() {
  // Match the launcher's fresh-install policy for direct/CLI launches too.
  // An explicit umbrella or per-device opt-out still selects the fallback.
  config_.enabled = Flag("GEKKOAOT_NATIVE_HW", true);
  config_.si = Flag("GEKKOAOT_NATIVE_HW_SI", true);
  config_.di = Flag("GEKKOAOT_NATIVE_HW_DI", true);
  config_.exi = Flag("GEKKOAOT_NATIVE_HW_EXI", true);
  config_.vi = Flag("GEKKOAOT_NATIVE_HW_VI", true);
  config_.ai = Flag("GEKKOAOT_NATIVE_HW_AI", true);
  config_.pi = Flag("GEKKOAOT_NATIVE_HW_PI", true);
  config_.mi = Flag("GEKKOAOT_NATIVE_HW_MI", true);
  config_.cp = Flag("GEKKOAOT_NATIVE_HW_CP", true);
  config_.dsp = Flag("GEKKOAOT_NATIVE_HW_DSP", true);
  config_.strict = Flag("GEKKOAOT_NATIVE_HW_STRICT", false);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_HW_1_0_SAFE=%u full=%u components=%u/9 strict=%u "
               "si=%u di=%u exi=%u vi=%u ai=%u pi=%u mi=%u cp=%u dsp=%u "
               "scheduler=v4-preserved cp-breakpoints=preserved cp-batch=4k "
               "policy=host-services+guest-register-facades+exact-fallback\n",
               config_.enabled ? 1u : 0u, FullNative() ? 1u : 0u,
               ActiveComponentCount(), Strict() ? 1u : 0u,
               SI() ? 1u : 0u, DI() ? 1u : 0u, EXI() ? 1u : 0u,
               VI() ? 1u : 0u, AI() ? 1u : 0u, PI() ? 1u : 0u,
               MI() ? 1u : 0u, CP() ? 1u : 0u, DSP() ? 1u : 0u);
}
} // namespace GekkoAOT::NativeHW
