#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
namespace GekkoAOT::Native {
// Durable same-directory replacement; never delete the previous save on error.
bool ReplaceFile(const std::filesystem::path &, std::span<const std::uint8_t>,
                 const std::atomic<bool> *cancel = nullptr);
} // namespace GekkoAOT::Native
