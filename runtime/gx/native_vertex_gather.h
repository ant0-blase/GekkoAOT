// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace GekkoAOT::GX {
// Compile the indexed fields of a vertex format once. Build host-owned packed
// resources containing ONLY the elements referenced by this draw. Original
// RAM, direct attributes, matrix indices and attribute strides stay unchanged.
// Every draw re-reads the source: no guest cache invalidation assumption.
struct NativeVertexGather {
  static constexpr unsigned MaxFields = 4, MaxVertices = 64, MaxStride = 256;
  struct Field { std::uint16_t offset, element; };
  struct Source { const std::uint8_t* data; std::size_t available; unsigned stride; };
  struct Range { std::uint32_t offset, bytes, maximum; };
  struct Result {
    std::array<Range, MaxFields> ranges;
    std::size_t bytes = 0, source_window_bytes = 0;
  };
  struct Workspace {
    // Caller owns this workspace; results never retain pointers into RAM or
    // mapped GPU staging. No allocation or buffer growth in the draw path.
    std::array<std::uint8_t, MaxVertices * MaxStride> vertices;
    std::array<std::uint8_t, MaxFields * (MaxVertices * 255u + 4u)> storage;
  };
  std::array<Field, MaxFields> fields{};
  std::uint16_t stride = 0;
  std::uint8_t count = 0;

  bool Compile(unsigned vertex_stride, std::span<const Field> indexed) noexcept {
    count = 0;
    if (!vertex_stride || vertex_stride > MaxStride || indexed.empty() || indexed.size() > MaxFields)
      return false;
    for (unsigned f = 0; f < indexed.size(); ++f) {
      const auto field = indexed[f];
      if (!field.element || field.element > 255u || field.offset + 2u > vertex_stride) return false;
      for (unsigned previous = 0; previous < f; ++previous)
        if (field.offset < indexed[previous].offset + 2u && indexed[previous].offset < field.offset + 2u)
          return false;
      fields[f] = field;
    }
    stride = static_cast<std::uint16_t>(vertex_stride);
    count = static_cast<std::uint8_t>(indexed.size());
    return true;
  }

  // v336: elements are a few bytes (s16x3, f32x2, f32x3, colors); a libc
  // memcpy call per vertex was the top GX-worker hotspot (HPCOS). Two
  // possibly-overlapping fixed-width moves stay inside [0, size).
  static void CopyElement(std::uint8_t* target, const std::uint8_t* source, unsigned size) noexcept {
    if (size >= 8u && size <= 16u) {
      std::uint64_t head, tail;
      std::memcpy(&head, source, 8u);
      std::memcpy(&tail, source + size - 8u, 8u);
      std::memcpy(target, &head, 8u);
      std::memcpy(target + size - 8u, &tail, 8u);
    } else if (size >= 4u && size < 8u) {
      std::uint32_t head, tail;
      std::memcpy(&head, source, 4u);
      std::memcpy(&tail, source + size - 4u, 4u);
      std::memcpy(target, &head, 4u);
      std::memcpy(target + size - 4u, &tail, 4u);
    } else if (size < 4u) {
      for (unsigned i = 0; i < size; ++i) target[i] = source[i];
    } else {
      std::memcpy(target, source, size);
    }
  }

  bool Execute(std::span<const std::uint8_t> input, unsigned vertices,
               std::span<const Source> sources, Workspace& host, Result& out) const noexcept {
    out.bytes = out.source_window_bytes = 0;
    if (!count || sources.size() != count || !vertices || vertices > MaxVertices ||
        input.size() != std::size_t(vertices) * stride) return false;
    std::memcpy(host.vertices.data(), input.data(), input.size());
    for (unsigned f = 0; f < count; ++f) {
      const auto& source = sources[f];
      const auto& field = fields[f];
      if (!source.data || source.stride < field.element || source.stride > 255u) return false;
      const auto length = (vertices - 1u) * source.stride + field.element;
      const auto padded = (length + 3u) & ~3u;
      if (padded > host.storage.size() - out.bytes) return false;
      const auto start = out.bytes;
      unsigned minimum = 65535u, maximum = 0;
      for (unsigned v = 0; v < vertices; ++v) {
        const auto at = std::size_t(v) * stride + field.offset;
        const unsigned index = (unsigned(input[at]) << 8u) | input[at + 1u];
        const auto offset = std::size_t(index) * source.stride;
        if (offset > source.available || field.element > source.available - offset) return false;
        minimum = index < minimum ? index : minimum;
        maximum = index > maximum ? index : maximum;
        auto* target = host.storage.data() + start + std::size_t(v) * source.stride;
        CopyElement(target, source.data + offset, field.element);
        // GPU readers fetch aligned words; deterministic padding is captured
        // with the resource, without reading past the resolver-approved span.
        if (v + 1u != vertices && source.stride != field.element)
          std::memset(target + field.element, 0, source.stride - field.element);
        host.vertices[at] = 0;
        host.vertices[at + 1u] = static_cast<std::uint8_t>(v);
      }
      std::memset(host.storage.data() + start + length, 0, padded - length);
      out.ranges[f] = {static_cast<std::uint32_t>(start), length, maximum};
      out.bytes += padded;
      out.source_window_bytes += std::size_t(maximum) * source.stride + field.element -
                                ((std::size_t(minimum) * source.stride) & ~std::size_t{3});
    }
    return true;
  }
};
} // namespace GekkoAOT::GX
