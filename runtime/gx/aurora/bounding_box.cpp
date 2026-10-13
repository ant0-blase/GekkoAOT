// SPDX-License-Identifier: GPL-3.0-or-later
// A small native GPU reduction, only used between GXClearBoundingBox and a
// PE bounding-box read. Writes enter the same encoder as the surrounding GX
// draws; CPU reads use an ordered 16-byte readback, never an EFB screenshot.
#include "bounding_box.h"
#include "gx/gx.hpp"
#include "gfx/recording.hpp"
#include "webgpu/gpu.hpp"
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>

namespace {
wgpu::Buffer g_bounds;
aurora::gfx::EncoderTaskId g_write_task = aurora::gfx::InvalidEncoderTask;
std::array<std::uint16_t, 4> g_cached{};
bool g_active = false;
bool g_valid = true;
struct PairWrite { std::uint32_t index, first, second; };

void EncodePair(const aurora::gfx::EncoderTaskContext& ctx,
                const wgpu::CommandEncoder& encoder, const void* payload,
                std::size_t size, void*) {
  if (size != sizeof(PairWrite)) return;
  PairWrite write;
  std::memcpy(&write, payload, size);
  const wgpu::BufferDescriptor descriptor{
      .label = "GX bounding box pair upload", .usage = wgpu::BufferUsage::CopySrc,
      .size = 8, .mappedAtCreation = true};
  auto upload = ctx.device.CreateBuffer(&descriptor);
  const std::array values{write.first, write.second};
  std::memcpy(upload.GetMappedRange(), values.data(), sizeof(values));
  upload.Unmap();
  encoder.CopyBufferToBuffer(upload, 0, g_bounds, write.index * 4u, sizeof(values));
}
}

// Internal Aurora overlay hooks, resolved by the standalone GX DSO.
extern "C" const wgpu::Buffer& gekkoaot_bbox_buffer() {
  if (!g_bounds) {
    const wgpu::BufferDescriptor descriptor{
        .label = "GX bounding box atomic bounds",
        .usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst,
        .size = 16, .mappedAtCreation = true};
    g_bounds = aurora::webgpu::g_device.CreateBuffer(&descriptor);
    std::memset(g_bounds.GetMappedRange(), 0, 16);
    g_bounds.Unmap();
  }
  return g_bounds;
}
extern "C" bool gekkoaot_bbox_active() { return g_active; }

namespace GekkoAOT::GX {
bool NativeBoundingBoxActive() { return g_active; }

bool WriteNativeBoundingBox(std::uint32_t reg, std::uint32_t value) {
  if (reg != 0x55u && reg != 0x56u) return false;
  (void)gekkoaot_bbox_buffer();
  if (g_write_task == aurora::gfx::InvalidEncoderTask) {
    g_write_task = aurora::gfx::register_encoder_task_type({
        .label = "GX bounding box pair reset", .callback = EncodePair});
  }
  const PairWrite write{reg == 0x55u ? 0u : 2u, value & 0x3ffu, (value >> 10u) & 0x3ffu};
  if (!aurora::gfx::push_encoder_task(g_write_task, &write, sizeof(write))) return false;
  g_active = true;
  g_valid = false;
  aurora::gx::g_gxState.dirty |= aurora::gx::DirtyPipeline | aurora::gx::DirtyUniform;
  static const bool trace = [] {
    const auto* value = std::getenv("GEKKOAOT_TRACE_BBOX");
    return value && value[0] == '1';
  }();
  static unsigned logs = 0;
  if (trace && logs++ < 128)
    std::fprintf(stderr, "GEKKOAOT_TRACE_BBOX event=reset reg=%02x first=%u second=%u\n",
                 reg, write.first, write.second);
  return true;
}

bool ReadNativeBoundingBox(std::uint16_t* bounds) {
  if (!bounds) return false;
  // Hardware stops bounding-box accumulation on any PE bbox read. The four
  // sequential GXReadBoundingBox loads must observe one coherent result.
  g_active = false;
  aurora::gx::g_gxState.dirty |= aurora::gx::DirtyPipeline;
  if (!g_valid) {
    const wgpu::BufferDescriptor descriptor{
        .label = "GX bounding box CPU readback",
        .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead, .size = 16};
    auto readback = aurora::webgpu::g_device.CreateBuffer(&descriptor);
    auto encoder = aurora::webgpu::g_device.CreateCommandEncoder();
    encoder.CopyBufferToBuffer(gekkoaot_bbox_buffer(), 0, readback, 0, 16);
    auto command = encoder.Finish();
    aurora::webgpu::g_queue.Submit(1, &command);
    auto mapped = std::make_shared<std::atomic_bool>(false);
    const auto future = readback.MapAsync(wgpu::MapMode::Read, 0, 16,
        wgpu::CallbackMode::WaitAnyOnly,
        [mapped](wgpu::MapAsyncStatus status, wgpu::StringView) {
          mapped->store(status == wgpu::MapAsyncStatus::Success, std::memory_order_release);
        });
    const auto wait = aurora::webgpu::g_instance.WaitAny(future, 5000000000ull);
    if (wait != wgpu::WaitStatus::Success || !mapped->load(std::memory_order_acquire)) {
      std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_BBOX error=readback-failed\n");
      return false;
    }
    std::array<std::uint32_t, 4> values;
    std::memcpy(values.data(), readback.GetConstMappedRange(), sizeof(values));
    readback.Unmap();
    for (unsigned i = 0; i < values.size(); ++i) g_cached[i] = static_cast<std::uint16_t>(values[i]);
    g_valid = true;
    static unsigned logs = 0;
    if (logs++ < 32)
      std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_BBOX event=read bounds=%u,%u,%u,%u backend=gpu-fragment-reduction\n",
                   g_cached[0], g_cached[1], g_cached[2], g_cached[3]);
  }
  std::memcpy(bounds, g_cached.data(), sizeof(g_cached));
  return true;
}

void ShutdownNativeBoundingBox() {
  if (g_write_task != aurora::gfx::InvalidEncoderTask)
    aurora::gfx::unregister_encoder_task_type(g_write_task);
  g_write_task = aurora::gfx::InvalidEncoderTask;
  g_bounds = {};
  g_cached = {};
  g_active = false;
  g_valid = true;
}
}
