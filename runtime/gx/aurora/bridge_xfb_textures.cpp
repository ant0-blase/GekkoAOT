// SPDX-License-Identifier: GPL-3.0-or-later
// XFB scanout, RAM XFB uploads and texture mappings.
// Included by bridge.cpp, whose translation unit holds the bridge state; this
// file is not compiled on its own.

void DrainFifo();
void PollEvents();

std::uint8_t ClampByte(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void DecodeYuvyPair(const std::uint8_t* src, std::uint8_t* dst) {
  // GameCube XFB is YUVY 4:2:2: Y0 U Y1 V. BT.601 studio-range
  // conversion matches retail/Dolphin scanout semantics closely enough for a
  // host RGBA texture while keeping guest memory untouched.
  const int u = static_cast<int>(src[1]) - 128;
  const int v = static_cast<int>(src[3]) - 128;
  for (unsigned pixel = 0; pixel < 2u; ++pixel) {
    const int y = std::max(static_cast<int>(src[pixel * 2u]) - 16, 0);
    const int r = (298 * y + 409 * v + 128) >> 8;
    const int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
    const int b = (298 * y + 516 * u + 128) >> 8;
    dst[pixel * 4u + 0u] = ClampByte(r);
    dst[pixel * 4u + 1u] = ClampByte(g);
    dst[pixel * 4u + 2u] = ClampByte(b);
    dst[pixel * 4u + 3u] = 0xffu;
  }
}

std::uint64_t HashXfbRow(std::uint64_t hash, const std::uint8_t* data,
                         std::uint32_t bytes) {
  constexpr std::uint64_t kFnvPrime = 1099511628211ull;
  for (std::uint32_t i = 0; i < bytes; ++i) {
    hash ^= data[i];
    hash *= kFnvPrime;
  }
  return hash;
}

XfbZeroCopySlot* NewestXfbSlot() {
  XfbZeroCopySlot* newest = nullptr;
  for (auto& slot : g_xfb_slots) {
    if (!slot.valid) continue;
    if (!newest || slot.generation > newest->generation) newest = &slot;
  }
  return newest;
}

bool EnsureRamXfbTexture(std::uint32_t width, std::uint32_t height) {
  using namespace aurora::webgpu;
  if (g_ram_xfb_texture && g_ram_xfb_width == width && g_ram_xfb_height == height)
    return true;

  g_ram_xfb_texture = aurora::gfx::new_dynamic_texture_2d(
      width, height, 1u, GX_TF_RGBA8_PC, "GekkoAOT RAM XFB v57");
  if (!g_ram_xfb_texture || !g_ram_xfb_texture->texture ||
      !g_ram_xfb_texture->sampleTextureView) {
    g_ram_xfb_texture.reset();
    g_ram_xfb_present = {};
    g_ram_xfb_width = 0u;
    g_ram_xfb_height = 0u;
    return false;
  }

  g_ram_xfb_present = TextureWithSampler{
      .texture = g_ram_xfb_texture->texture,
      .view = g_ram_xfb_texture->sampleTextureView,
      .size = g_ram_xfb_texture->size,
      .format = g_ram_xfb_texture->format,
      .sampler = g_frameBuffer.sampler,
  };
  g_ram_xfb_width = width;
  g_ram_xfb_height = height;
  return true;
}

bool ResolveXfbField(std::uint32_t address, std::uint32_t width,
                     std::uint32_t stride_bytes, std::uint32_t rows,
                     const std::uint8_t** data) {
  if (!data || address == 0u || width == 0u || rows == 0u) return false;
  const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 2u;
  const std::uint64_t span = static_cast<std::uint64_t>(rows - 1u) * stride_bytes + row_bytes;
  if (span == 0u || span > std::numeric_limits<std::uint32_t>::max()) return false;
  const void* pointer = nullptr;
  std::uint32_t available = 0u;
  if (!Resolve(address, static_cast<std::uint32_t>(span), &pointer, &available) ||
      available < span || pointer == nullptr)
    return false;
  *data = static_cast<const std::uint8_t*>(pointer);
  return true;
}

bool PresentRamXfb(std::uint32_t top, std::uint32_t bottom,
                   std::uint32_t width, std::uint32_t stride_bytes,
                   std::uint32_t field_height) {
  if (top == 0u) return false;

  // VI geometry is authoritative when the v57 host ABI is available. Keep a
  // conservative fallback for old callers so this optional DSO symbol remains
  // useful during mixed-version testing.
  const XfbZeroCopySlot* recent = NewestXfbSlot();
  if (width == 0u && recent) width = recent->native_width;
  if (width == 0u) width = 640u;

  const std::uint32_t line_bytes = width * 2u;
  const bool split_origins = bottom != 0u && bottom != top;
  if (stride_bytes == 0u) {
    if (split_origins && bottom > top && bottom - top == line_bytes)
      stride_bytes = line_bytes * 2u;
    else
      stride_bytes = line_bytes;
  }

  // STD/WPL == 2 is the normal interlaced VI layout: top and bottom point at
  // adjacent scanlines, while each field advances by two scanlines.
  const bool interlaced = split_origins && stride_bytes >= line_bytes * 2u;
  if (field_height == 0u && recent && recent->native_height != 0u)
    field_height = interlaced ? std::max(recent->native_height / 2u, 1u)
                              : recent->native_height;
  if (field_height == 0u) field_height = interlaced ? 224u : 448u;

  const std::uint32_t output_height = interlaced ? field_height * 2u : field_height;
  if (width < 2u || (width & 1u) != 0u || width > 1024u ||
      field_height > 576u || output_height == 0u || output_height > 1152u ||
      stride_bytes < line_bytes)
    return false;

  const std::uint8_t* top_data = nullptr;
  const std::uint8_t* bottom_data = nullptr;
  if (!ResolveXfbField(top, width, stride_bytes, field_height, &top_data)) return false;
  if (interlaced &&
      !ResolveXfbField(bottom, width, stride_bytes, field_height, &bottom_data))
    return false;

  const std::size_t rgba_bytes =
      static_cast<std::size_t>(width) * output_height * 4u;
  g_ram_xfb_rgba.resize(rgba_bytes);
  std::uint64_t source_hash = 1469598103934665603ull;
  for (std::uint32_t field_row = 0u; field_row < field_height; ++field_row) {
    const auto decode_row = [&](const std::uint8_t* source, std::uint32_t output_row) {
      const std::uint8_t* src = source + static_cast<std::size_t>(field_row) * stride_bytes;
      source_hash = HashXfbRow(source_hash, src, line_bytes);
      std::uint8_t* dst = g_ram_xfb_rgba.data() +
                          static_cast<std::size_t>(output_row) * width * 4u;
      for (std::uint32_t x = 0u; x < width; x += 2u)
        DecodeYuvyPair(src + static_cast<std::size_t>(x) * 2u,
                       dst + static_cast<std::size_t>(x) * 4u);
    };
    if (interlaced) {
      decode_row(top_data, field_row * 2u);
      decode_row(bottom_data, field_row * 2u + 1u);
    } else {
      decode_row(top_data, field_row);
    }
  }

  // A VI cache miss is not sufficient evidence that guest RAM owns the XFB.
  // Aurora's zero-copy GX path deliberately keeps many display copies only on
  // the GPU; interpreting stale MEM1 at the same address as YUVY produces the
  // characteristic green/black screen seen on ordinary GX-rendered titles.
  //
  // Keep independent evidence for several rotating XFBs. Retail titles
  // commonly double/quad-buffer by changing the VI address every field. The
  // old single candidate forgot its hash history on every address change, so a
  // real RAM-owned XFB could remain forever at observations=1/changes=0 even
  // while the same addresses visibly changed on later visits.
  ++g_ram_xfb_candidate_epoch;
  RamXfbCandidate* candidate = nullptr;
  RamXfbCandidate* replacement = &g_ram_xfb_candidates[0];
  for (auto& slot : g_ram_xfb_candidates) {
    if (slot.valid && slot.top == top && slot.bottom == bottom &&
        slot.width == width && slot.stride == stride_bytes &&
        slot.field_height == field_height) {
      candidate = &slot;
      break;
    }
    if (!slot.valid || slot.last_seen < replacement->last_seen)
      replacement = &slot;
  }
  if (!candidate) {
    candidate = replacement;
    *candidate = {};
    candidate->top = top;
    candidate->bottom = bottom;
    candidate->width = width;
    candidate->stride = stride_bytes;
    candidate->field_height = field_height;
    candidate->valid = true;
  }
  candidate->last_seen = g_ram_xfb_candidate_epoch;

  const bool hash_changed = candidate->observations != 0u &&
                            candidate->last_hash != source_hash;
  if (hash_changed) ++candidate->changes;
  ++candidate->observations;
  candidate->last_hash = source_hash;
  if (!candidate->armed && candidate->observations >= 3u &&
      candidate->changes >= 2u) {
    candidate->armed = true;
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_RAM_XFB_V82=1 action=arm top=%08x bottom=%08x width=%u stride=%u field_height=%u observations=%u changes=%llu hash=%016llx policy=multi-xfb-changing-mem1 slots=%zu\n",
                 top, bottom, width, stride_bytes, field_height,
                 candidate->observations,
                 static_cast<unsigned long long>(candidate->changes),
                 static_cast<unsigned long long>(source_hash),
                 kRamXfbCandidateSlots);
  }
  if (!candidate->armed) {
    static unsigned probe_logs = 0u;
    if (probe_logs++ < 128u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_RAM_XFB_V82=0 action=hold top=%08x bottom=%08x width=%u stride=%u field_height=%u observations=%u changes=%llu hash=%016llx reason=awaiting-per-xfb-changing-mem1\n",
                   top, bottom, width, stride_bytes, field_height,
                   candidate->observations,
                   static_cast<unsigned long long>(candidate->changes),
                   static_cast<unsigned long long>(source_hash));
    return false;
  }

  if (!EnsureRamXfbTexture(width, output_height)) return false;
  const wgpu::TexelCopyTextureInfo destination{
      .texture = g_ram_xfb_texture->texture,
      .mipLevel = 0u,
      .origin = {0u, 0u, 0u},
      .aspect = wgpu::TextureAspect::All,
  };
  const wgpu::TexelCopyBufferLayout layout{
      .offset = 0u,
      .bytesPerRow = width * 4u,
      .rowsPerImage = output_height,
  };
  const wgpu::Extent3D upload_size{
      .width = width,
      .height = output_height,
      .depthOrArrayLayers = 1u,
  };

  // All queue mutations, including RAM-XFB uploads, belong to the render
  // worker. A producer-side WriteTexture raced the worker's staging/submit
  // operations in Dawn Vulkan (SIGSEGV in CopyFromStagingToTextureImpl).
  // Lease bounded immutable CPU packets by swapping vectors: no image copy,
  // no per-upload thread and no GPU fence or EFB recording break.
  if(aurora::gfx::render_worker::is_worker_thread()) {
    aurora::webgpu::g_queue.WriteTexture(&destination,g_ram_xfb_rgba.data(),rgba_bytes,&layout,&upload_size);
    PresentFinalTexture(g_ram_xfb_present);
  } else {
    RamXfbUploadSlot* packet=nullptr;
    {
      std::unique_lock lock(g_ram_xfb_upload_mutex);
      g_ram_xfb_upload_ready.wait(lock,[&]{
        for(auto& slot:g_ram_xfb_uploads)if(!slot.busy){packet=&slot;return true;}
        return false;
      });
      packet->busy=true;packet->bytes.swap(g_ram_xfb_rgba);
    }
    aurora::gfx::render_worker::enqueue_work([packet,destination,layout,upload_size,rgba_bytes,source=g_ram_xfb_present] {
      aurora::webgpu::g_queue.WriteTexture(&destination,packet->bytes.data(),rgba_bytes,&layout,&upload_size);
      PresentFinalTexture(source);
      {std::lock_guard lock(g_ram_xfb_upload_mutex);packet->busy=false;}
      g_ram_xfb_upload_ready.notify_one();
    });
  }

  ++g_ram_xfb_generation;
  static unsigned logs = 0u;
  if (logs++ < 128u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_RAM_XFB_V59=1 action=present top=%08x bottom=%08x width=%u stride=%u field_height=%u output_height=%u interlaced=%u generation=%llu hash=%016llx changed=%u changes=%llu observations=%u sample=%02x%02x%02x%02x%02x%02x%02x%02x source=mem1-yuvy\n",
                 top, bottom, width, stride_bytes, field_height, output_height,
                 interlaced ? 1u : 0u,
                 static_cast<unsigned long long>(g_ram_xfb_generation),
                 static_cast<unsigned long long>(source_hash),
                 hash_changed ? 1u : 0u,
                 static_cast<unsigned long long>(candidate->changes),
                 candidate->observations,
                 top_data[0], top_data[1], top_data[2], top_data[3],
                 top_data[4], top_data[5], top_data[6], top_data[7]);
  return true;
}

void PresentViXfb(std::uint32_t top, std::uint32_t bottom, bool address_keyed,
                  std::uint32_t width = 0u, std::uint32_t stride_bytes = 0u,
                  std::uint32_t field_height = 0u, bool poll_events = true) {
  if (!g_ready) return;
  FlushEfbPokes();
  DrainFifo();
  ++g_camera_vi_epoch;
  // The GX worker runs ordered scanouts; SDL events stay on the host
  // thread that owns the window (gekkoaot_native_gx_should_quit).
  if (poll_events) PollEvents();

  // Interlaced VI can expose TOP=0/BOTTOM=0x500 for an XFB whose base is
  // physical zero. Use whichever field origin is non-zero for the cache lookup.
  const std::uint32_t scanout_address = top != 0u ? top : bottom;

  std::uint32_t byte_offset = 0u;
  XfbZeroCopySlot* slot =
      address_keyed && scanout_address != 0u
          ? FindXfbSlotForScanout(scanout_address, &byte_offset)
          : nullptr;
  const auto* source = slot ? XfbPresentSource(*slot) : nullptr;

  if (address_keyed && scanout_address != 0u) {
    if (source == nullptr) {
      aurora::gfx::render_worker::enqueue_work([] { g_cached_scanout.Reset(); });
      if (PresentRamXfb(scanout_address,
                        bottom != 0u ? bottom : scanout_address,
                        width, stride_bytes, field_height))
        return;
      static unsigned misses = 0;
      if (misses++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_XFB_SCANOUT_V202=0 request=%08x top=%08x bottom=%08x reason=xfb-not-cached action=hold-last-scanout ram_fallback=failed\n",
                     scanout_address, top, bottom);
      return;
    }

    // Presentation is a pure consumer.  The producer frame containing
    // this immutable XFB was already submitted at GXCopyDisp.  Never end or
    // restart the live EFB here: VI can legally scan the same generation on
    // multiple field/retrace boundaries.
    // VI keeps observing RAM scanout and field changes independently of GX
    // production. Avoid GPU work only for the same immutable GX snapshot and
    // the same host view. Resize/aspect/resume must redraw even a static image.
    if (!aurora::window::is_presentable()) {
      aurora::gfx::render_worker::enqueue_work([] { g_cached_scanout.Reset(); });
      return;
    }
    const auto window_size = aurora::window::get_window_size();
    // The whole cached copy is presented whichever field VI selects, so
    // the field origins are not part of the image. Keying on them presented
    // every interlaced frame twice (30 fps titles showed 60 in MangoHud).
    const GekkoAOT::GX::CachedScanoutKey scanout{
        reinterpret_cast<std::uintptr_t>(source->texture.Get()), slot->generation,
        0u, 0u, window_size.native_fb_width, window_size.native_fb_height,
        slot->force_43 ? 0u : g_aspect_mode.load(std::memory_order_acquire)};
    // Cache state belongs exclusively to the render worker. A failed surface
    // acquisition/present is retried at the next VI field; only success allows
    // deduplication. This preserves pipelining without a new CPU/GPU wait.
    {
      static const bool trace = std::getenv("GEKKOAOT_XFB_COPY_TRACE") != nullptr;
      static unsigned logs = 0u;
      if (trace && slot->presented_generation != slot->generation && logs++ < 4000u)
        std::fprintf(stderr, "GEKKOAOT_XFB_SCANOUT_TRACE_V389 t_us=%lld xfb=%08x generation=%llu\n",
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count()),
                     slot->address, static_cast<unsigned long long>(slot->generation));
    }
    slot->presented_generation = slot->generation;
    aurora::gfx::render_worker::enqueue_work([retained = *source, scanout, force_43 = slot->force_43, captureFrame = slot->capture_frame] {
      GekkoAOT::Capture::FrameContext context(captureFrame);
      GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::XfbSelected, captureFrame, scanout.generation);
      if (g_cached_scanout.Duplicate(scanout)) {
        GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::ScanoutDuplicate, captureFrame);
        return;
      }
      const auto before = aurora::gfx::presentation_count();
      g_present_force_43 = force_43;
      PresentFinalTexture(retained);
      g_present_force_43 = false;
      if (aurora::gfx::presentation_count() != before)
        g_cached_scanout.Queued(scanout);
    });

    const std::uint32_t row_offset =
        slot->stride_bytes != 0u ? byte_offset / slot->stride_bytes : 0u;
    static unsigned logs = 0;
    if (logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_XFB_SCANOUT_V202=1 request=%08x top=%08x bottom=%08x base=%08x offset=%u row=%u native=%ux%u stride=%u generation=%llu source=vi-selected-aurora-xfb-cache\n",
                   scanout_address, top, bottom, slot->address, byte_offset, row_offset,
                   slot->native_width, slot->native_height, slot->stride_bytes,
                   static_cast<unsigned long long>(slot->generation));
    return;
  }

  // Legacy/boot path only, before VI has supplied a non-zero XFB address.
  aurora::gfx::render_worker::enqueue_work([] { g_cached_scanout.Reset(); });
  if (g_frame_open) {
    aurora_end_frame();
    g_frame_open = false;
    if (g_array_frame_refresh) RefreshIndexedArraySnapshots();
  }
  if (!g_quit && aurora_begin_frame()) g_frame_open = true;
}

void MapTextureImage3(std::uint8_t reg, std::uint32_t word) {
  using namespace aurora::gx;
  const unsigned map = (reg>=0xB4u ? 4u : 0u) + (reg & 3u);
  if(map>=MaxTextures) return;
  auto& slot=g_gxState.loadedTextures[map];
  const std::uint8_t bit=static_cast<std::uint8_t>(1u<<map);
  g_texture_image3_word[map] = word;
  g_texture_image3_written = static_cast<std::uint8_t>(g_texture_image3_written | bit);
  const std::uint32_t guest=(word & 0x00ffffffu)<<5;
  const std::size_t texture_bytes=aurora::gx::texture::texture_source_size(
      slot.format(),slot.width(),slot.height(),slot.mip_count());
  const std::uint32_t required=texture_bytes>std::numeric_limits<std::uint32_t>::max()
                                   ? 0u : static_cast<std::uint32_t>(texture_bytes);
  if(g_z16r_copy_trace && g_z16r_copy_span!=0u) {
    const std::uint64_t copy_begin=g_z16r_copy_dest;
    const std::uint64_t copy_end=copy_begin+g_z16r_copy_span;
    const std::uint64_t tex_begin=guest;
    const std::uint64_t tex_end=tex_begin+required;
    const bool overlap=required!=0u && tex_begin<copy_end && copy_begin<tex_end;
    const std::uint8_t bit=static_cast<std::uint8_t>(1u<<map);
    g_z16r_bound_maps=static_cast<std::uint8_t>(overlap ?
        (g_z16r_bound_maps|bit) : (g_z16r_bound_maps&~bit));
    if(overlap && g_z16r_bind_logs++<96u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_Z16R_TRACE_V1 event=bind map=%u guest=%08x bytes=%u fmt=%u size=%ux%u copy=%08x span=%u\n",
                   map,guest,required,unsigned(slot.format()),slot.width(),slot.height(),
                   g_z16r_copy_dest,g_z16r_copy_span);
  }
  const void* ptr=nullptr; std::uint32_t available=0;
  if(required==0u || !Resolve(guest,required,&ptr,&available) || available<required) {
    // Never retain the previous host pointer when the guest changes
    // IMAGE3 to an unmapped base. Reusing stale texture storage produces the
    // classic "bad VRAM/overclock" corruption pattern across otherwise valid
    // draws. Fail closed and force a fresh bind when a later address resolves.
    g_texture_image3_valid=static_cast<std::uint8_t>(g_texture_image3_valid & ~bit);
    g_texture_ui_source_hash_valid =
        static_cast<std::uint8_t>(g_texture_ui_source_hash_valid & ~bit);
    slot.data=nullptr;
    slot.texDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
    g_gxState.dirty |= DirtyTextures;
    aurora::gx::texture::invalidate_bindings();
    static unsigned bad_texture_logs=0;
    if(bad_texture_logs++<32u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_TEXTURE_BASE_V87=0 map=%u guest=%08x required=%u available=%u action=invalidate-stale-mapping\n",
                   map,guest,required,available);
    return;
  }

  static unsigned video_logs=0;
  if(g_trace_video && video_logs++<256u) {
    const auto* bytes=static_cast<const std::uint8_t*>(ptr);
    std::uint64_t hash=1469598103934665603ull;
    for(unsigned i=0;i<required;++i) hash=(hash^bytes[i])*1099511628211ull;
    std::fprintf(stderr,"GEKKOAOT_TRACE_VIDEO event=texture map=%u guest=%08x format=%u width=%u height=%u bytes=%u hash=%016llx\n",
                 map,guest,unsigned(slot.format()),slot.width(),slot.height(),required,
                 static_cast<unsigned long long>(hash));
  }

  // Games commonly re-emit the same IMAGE3 state for every draw. Treat that as
  // a binding, not a texture mutation. Aurora can then reuse its converted GPU
  // resource instead of new_static_texture_2d/convert/upload on every bind.
  const bool stable_bind=(g_texture_image3_valid&bit)!=0u &&
                         g_texture_mapped_image0[map]==slot.image0 &&
                         g_texture_mapped_mips[map]==slot.mip_count() && slot.data==ptr;
  // Small intensity/alpha and CI textures are the formats commonly used
  // by retail bitmap-font atlases. Terminator 3 rewrites some of those atlases
  // in place while retaining IMAGE3, so a pointer-only stable-bind cache misses
  // the mutation. v170 fixed that by versioning *every* such rebind, which also
  // turns Monkey Ball's bitmap-heavy UI into constant texture churn. Hash these
  // source bytes only on the low-bit stable-bind path and invalidate when their
  // contents actually changed. Normal material formats keep the zero-hash fast
  // path, while T3 still observes real in-place font updates.
  const auto source_format=slot.format();
  const bool font_ui_format=
      source_format==GX_TF_I4 || source_format==GX_TF_I8 ||
      source_format==GX_TF_IA4 || source_format==GX_TF_IA8 ||
      source_format==GX_TF_C4 || source_format==GX_TF_C8 ||
      source_format==GX_TF_C14X2;
  bool font_ui_changed=false;
  // Only hash low-bit/font/video textures when IMAGE3 is actually a
  // stable rebind.  The old v174 code hashed every I4/I8/IA/CI IMAGE3 write,
  // including ordinary texture switches and the three I8 Y/U/V planes used by
  // retail video paths.  That made MapTextureImage3 itself a double-digit CPU
  // hotspot.  A non-stable bind already bumps texDataVersion and uploads the
  // new source, so hashing it cannot improve coherency.  On the first stable
  // rebind we establish a baseline; later stable rebinds still detect genuine
  // in-place bitmap/font mutations exactly as v174 intended.
  const bool paranoid_texture_hash=g_paranoid_texture_hash;
  if(font_ui_format && (stable_bind || paranoid_texture_hash)) {
    const auto* bytes=static_cast<const std::uint8_t*>(ptr);
    std::uint64_t hash=1469598103934665603ull;
    for(std::uint32_t i=0;i<required;++i) hash=(hash^bytes[i])*1099511628211ull;
    const bool had_hash=(g_texture_ui_source_hash_valid&bit)!=0u;
    font_ui_changed=had_hash && g_texture_ui_source_hash[map]!=hash;
    g_texture_ui_source_hash[map]=hash;
    g_texture_ui_source_hash_valid=
        static_cast<std::uint8_t>(g_texture_ui_source_hash_valid|bit);
  } else if(font_ui_format) {
    // The pointer/descriptor changed, so the previous map hash belongs to a
    // different texture identity.  Avoid comparing it against the first stable
    // rebind of the new source.
    g_texture_ui_source_hash_valid=
        static_cast<std::uint8_t>(g_texture_ui_source_hash_valid&~bit);
  }
  if(stable_bind && g_texture_bind_cache && (!font_ui_format || !font_ui_changed)) return;

  g_texture_mapped_image0[map] = slot.image0;
  g_texture_mapped_mips[map] = slot.mip_count();
  g_texture_image3_valid=static_cast<std::uint8_t>(g_texture_image3_valid|bit);
  slot.data=ptr;
  // Keep object identity stable per hardware texture unit. With the retail
  // IMAGE3 cache disabled for correctness, assigning a fresh texObjId on every
  // bind makes Aurora's object cache grow by one entry per draw and can reach
  // several GiB in minutes. texDataVersion is the mutation/version signal; the
  // object ID is identity, not a generation counter.
  slot.texObjId=g_texture_source_ids && g_texture_version_reuse && !font_ui_format && required!=0u ?
      TextureSourceObjectId(guest,slot.image0,static_cast<std::uint8_t>(slot.mip_count())) :
      static_cast<std::uint32_t>(map+1u);
  const auto generation=static_cast<std::uint32_t>(++g_texture_generation);
  // Font/CI sources keep a fresh version per rebind (v174 in-place updates).
  slot.texDataVersion=font_ui_format ? generation :
      BindTextureSourceVersion(guest,slot.image0,static_cast<std::uint8_t>(slot.mip_count()),
                               required,generation);
  if(g_draw_census) {
    ++g_tex_census_binds; g_tex_census_bind_bytes+=required;
    if(font_ui_format) { ++g_tex_census_font; g_tex_census_font_bytes+=required; }
  }
  slot.flags &= ~0x80u;
  g_gxState.dirty |= DirtyTextures;
}

void RefreshTextureMappings() {
  for (unsigned map = 0; map < aurora::gx::MaxTextures; ++map) {
    const auto bit = static_cast<std::uint8_t>(1u << map);
    if ((g_texture_image3_written & bit) == 0) continue;
    const auto& slot = aurora::gx::g_gxState.loadedTextures[map];
    if ((g_texture_image3_valid & bit) == 0 ||
        g_texture_mapped_image0[map] != slot.image0 ||
        g_texture_mapped_mips[map] != slot.mip_count()) {
      // IMAGE0/MODE1 can be changed without writing IMAGE3. Validate the new
      // complete source span at draw time, after the descriptor is installed.
      MapTextureImage3(static_cast<std::uint8_t>((map < 4 ? 0x94 : 0xb4) + (map & 3)),
                       g_texture_image3_word[map]);
    }
  }
}

GekkoAOT::GX::TlutMemory g_tlut_memory;
std::array<std::uint64_t, 8> g_tlut_revisions{};
std::array<std::uint32_t, 8> g_tlut_bindings{};

void MapTlut() {
  using namespace aurora::gx;
  const auto load=g_gxState.bpRegCache[0x64];
  const auto trigger=g_gxState.bpRegCache[0x65];
  const std::uint32_t guest=GekkoAOT::GX::TlutMemory::SourceAddress(load);
  const auto required=GekkoAOT::GX::TlutMemory::UploadBytes(trigger);
  const void* ptr=nullptr; std::uint32_t available=0;
  const bool valid=required==0u ||
      (Resolve(guest,required,&ptr,&available) && available>=required);
  const bool copied=valid && g_tlut_memory.Load(trigger,
      {static_cast<const std::uint8_t*>(ptr),valid ? required : 0u});
  static unsigned logs=0;
  if(g_trace_font && logs++<128u)
    std::fprintf(stderr,"GEKKOAOT_TRACE_FONT event=tlut-load guest=%08x tmem=%05x bytes=%u success=%u revision=%llu\n",
                 guest,GekkoAOT::GX::TlutMemory::Offset(trigger),required,copied?1u:0u,
                 static_cast<unsigned long long>(g_tlut_memory.Revision()));
  if(!copied) { g_fifo_fault=true; g_quit=true; }
  else g_tlut_binding_scan_dirty=true;
}

void BindRawTluts() {
  using namespace aurora::gx;
  for(unsigned map=0;map<MaxTextures;++map) {
    auto& texture=g_gxState.loadedTextures[map];
    const auto format=texture.format();
    const unsigned entries=format==GX_TF_C4 ? 16u : format==GX_TF_C8 ? 256u :
                           format==GX_TF_C14X2 ? 16384u : 0u;
    if(!entries) continue;
    const unsigned reg=0x98u+(map&3u)+(map>=4u ? 0x20u : 0u);
    const auto binding=g_gxState.bpRegCache[reg]&0xfffu;
    // Include texture format: the same TMEM address can be rebound as CI4/CI8.
    const auto key=binding|(format<<16u);
    if(g_tlut_revisions[map]==g_tlut_memory.Revision()+1u && g_tlut_bindings[map]==key) continue;
    const auto bytes=g_tlut_memory.Palette(binding,entries);
    auto& palette=g_gxState.loadedTluts[map];
    palette.data=bytes.empty() ? nullptr : bytes.data();
    palette.numEntries=static_cast<std::uint16_t>(entries);
    palette.format=static_cast<GXTlutFmt>((binding>>10u)&3u);
    palette.tlutObjId=map+1u;
    palette.tlutDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
    texture.tlut=static_cast<GXTlut>(map);
    g_tlut_bindings[map]=key;
    g_tlut_revisions[map]=g_tlut_memory.Revision()+1u;
    g_gxState.dirty |= DirtyTextures;
    aurora::gx::texture::invalidate_bindings();
  }
}

void InvalidateTextureCacheV87() {
  using namespace aurora::gx;
  // Direct/uncached guest stores can mutate MEM1 without a dcbst/dcbf
  // publication callback. Invalidating TMEM must therefore make the next
  // sampled static texture check its source again. Otherwise Bink movies
  // retain their initial black frame. Coalesce the SDK's paired BP66 pulses
  // until the next draw; Aurora reuses its content cache if bytes are equal.
  // GPU-owned EFB copy textures retain their separate copy-texture identity.
  unsigned bound=0u;
  for(unsigned map=0;map<MaxTextures;++map) {
    auto& texture=g_gxState.loadedTextures[map];
    if(!texture.data) continue;
    ++bound;
  }
  if(g_invalidate_texture_sources) g_texture_source_invalidation_pending=true;
  ++g_texture_cache_only_invalidations;
  if(g_draw_census) ++g_tex_census_bp66;
  // With the host write watch, forget only the sources whose pages were
  // written since they were versioned (see HostBridge::EnableTextureWatch).
  bool watched=false;
  if(g_texture_watch_collect) {
    static std::vector<std::uint32_t> pages(8192u);
    const std::int32_t count=g_texture_watch_collect(g_texture_watch_user,pages.data(),
                                                     static_cast<std::uint32_t>(pages.size()));
    if(count>=0) {
      watched=true;
      for(std::int32_t i=0;i<count;++i) ForgetTextureSourceRange(pages[std::size_t(i)],4096u);
      ++g_tex_watch_collects; g_tex_watch_pages+=std::uint32_t(count);
    } else {
      ++g_tex_watch_fallbacks;
    }
  }
  g_texture_source_watched_invalidation=watched;
  if(!watched) ForgetTextureSourceVersions();
  texture::invalidate_bindings();
  g_gxState.dirty |= DirtyTextures;
  static unsigned logs=0;
  if(logs++<64u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_TEX_INVALIDATE_V237=1 reg=66 bound=%u source_recheck=%u policy=coalesce-until-draw\n",
                 bound,g_invalidate_texture_sources?1u:0u);
}

enum class DecodeResult : std::uint8_t { Complete, Incomplete, Invalid };

DecodeResult CommandLength(const std::uint8_t* p, std::size_t n,
                           std::size_t* len, std::size_t* need) {
  if(!p||!len||!need) return DecodeResult::Invalid;
  if(n<1u){*need=1u;return DecodeResult::Incomplete;}
  const std::uint8_t cmd=p[0], op=cmd & GX_OPCODE_MASK;
  std::size_t out=1u;
  if(op==kLoadBpOpcode) out=5u;
  else if(op==GX_LOAD_CP_REG) out=6u;
  else if(op==GX_LOAD_XF_REG) {
    if(n<5u){*need=5u;return DecodeResult::Incomplete;}
    out=5u+4u*(((Be32(p+1)>>16)&0xffffu)+1u);
  } else if(op==GX_LOAD_INDX_A||op==GX_LOAD_INDX_B||op==GX_LOAD_INDX_C||op==GX_LOAD_INDX_D) out=5u;
  else if(op==GX_CMD_CALL_DL) out=9u;
  else if(op==GX_CMD_INVL_VC||op==GX_NOP) out=1u;
  else if(op>=0x80u&&op<=0xB8u) {
    if(n<3u){*need=3u;return DecodeResult::Incomplete;}
    const auto fmt=static_cast<GXVtxFmt>(cmd&GX_VAT_MASK);
    const std::uint32_t vertex_size=VertexSize(fmt);
    if(vertex_size==0u) return DecodeResult::Invalid;
    out=3u+std::size_t(Be16(p+1))*vertex_size;
  } else {
    return DecodeResult::Invalid;
  }
  *len=out;
  if(n<out){*need=out;return DecodeResult::Incomplete;}
  *need=1u;
  return DecodeResult::Complete;
}

bool ProcessSpan(const std::uint8_t* data, std::size_t bytes, unsigned depth);
bool ProcessOne(const std::uint8_t* p, std::size_t len, unsigned depth);

// Draw-run coalescing. Retail engines (HPCOS: ~1.5M draws/s, 78% with
// <=4 vertices, 95% directly following the previous draw) emit long runs of
// tiny identical-format draws with no command in between. GX state cannot
// change inside such a run, so one draw over the concatenated vertices is
// equivalent: list primitives are concatenated, strips/fans become triangle
// lists using exactly Aurora's own vertex order (same winding/culling and
// rasterization order). The merged draw then needs one array window, one
// upload and one recorded/encoded draw instead of one per GXBegin.
// Members only need the same vertex format. A run of one list primitive
// (quads or triangles) stays that primitive and is a plain concatenation; any
// strip/fan member or a mix of primitives becomes one triangle list, using
// Aurora's prepare_idx_buffer order for every source primitive (quads as
// (0,1,2)(2,3,0)), so winding, culling and rasterization order are unchanged.
// Strips/fans are expanded vertex by vertex, which only pays for small ones;
// list members are copied as one block and may be larger.
constexpr std::uint32_t kDrawRunExpandMax = 64u;
constexpr std::uint32_t kDrawRunListMax = 256u;
constexpr std::uint32_t kDrawRunMaxVertices = 16384u;
bool g_draw_runs = true;
std::vector<std::uint8_t> g_draw_run_buffer;

std::uint32_t DrawRunTriangleVertices(std::uint8_t op, std::uint32_t count) {
  switch (op) {
  case GX_QUADS: return count / 4u * 6u;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN: return 3u * (count - 2u);
  default: return count;
  }
}
bool DrawRunMember(std::uint8_t op, std::uint32_t count) {
  if (count == 0u) return false;
  switch (op) {
  case GX_QUADS: return count <= kDrawRunListMax && count % 4u == 0u;
  case GX_TRIANGLES: return count <= kDrawRunListMax && count % 3u == 0u;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN: return count >= 3u && count <= kDrawRunExpandMax;
  default: return false;  // lines/points keep Aurora's dedicated paths
  }
}
bool DrawRunStateAllows() {
  using namespace aurora::gx;
  const auto& state = g_gxState;
  // A lone 4/6-vertex orthographic draw may be a screen fill whose
  // classification feeds uniforms; never fold one into a larger draw.
  return !(state.projType == GX_ORTHOGRAPHIC && state.numTexGens == 1u &&
           state.numTevStages <= 2u && state.blendMode == GX_BM_NONE);
}
// Copy one vertex of a run member. Strides are small and vary per VAT, so a
// libc memcpy call per expanded vertex dominated the run assembly; two
// overlapping fixed-size moves cover every stride from 4 to 64 bytes.
inline void CopyRunVertex(std::uint8_t* dst, const std::uint8_t* src, std::uint32_t stride) {
  const auto pair = [&]<std::size_t N>() {
    std::uint8_t head[N], tail[N];
    std::memcpy(head, src, N);
    std::memcpy(tail, src + stride - N, N);
    std::memcpy(dst, head, N);
    std::memcpy(dst + stride - N, tail, N);
  };
  if (stride >= 32u && stride <= 64u) pair.template operator()<32>();
  else if (stride >= 16u && stride < 32u) pair.template operator()<16>();
  else if (stride >= 8u && stride < 16u) pair.template operator()<8>();
  else if (stride >= 4u && stride < 8u) pair.template operator()<4>();
  else std::memcpy(dst, src, stride);
}
bool IsDrawCommand(std::uint8_t cmd) {
  const std::uint8_t op = cmd & GX_OPCODE_MASK;
  return op >= 0x80u && op <= 0xB8u;
}

// Append to `out` the merged command for the run of small draws in one
// vertex format starting at data[0..len) (bounded by `available`). Returns
// the number of members (0 when the first command does not start a run, with
// *miss the census reason) and the bytes consumed.
std::uint32_t AssembleDrawRun(const std::uint8_t* data, std::size_t available, std::size_t len,
                              std::uint32_t stride, std::vector<std::uint8_t>& out,
                              std::size_t* consumed, unsigned* miss) {
  using namespace aurora::gx;
  const std::uint8_t cmd = data[0], op = cmd & GX_OPCODE_MASK, vat = cmd & GX_VAT_MASK;
  const std::uint32_t first = Be16(data + 1);
  *consumed = len;
  if ((data[len] & GX_VAT_MASK) != vat) { *miss = 0u; return 0u; }
  if (op == GX_LINES || op == GX_LINESTRIP || op == GX_POINTS) { *miss = 3u; return 0u; }
  if (!DrawRunMember(op, first)) { *miss = 1u; return 0u; }
  if (stride == 0u || len != 3u + std::size_t(first) * stride) { *miss = 4u; return 0u; }
  std::size_t end = len;
  std::uint32_t members = 1u, triangles = DrawRunTriangleVertices(op, first), list = first;
  bool mixed = false;
  while (end + 3u <= available && IsDrawCommand(data[end]) &&
         (data[end] & GX_VAT_MASK) == vat) {
    const std::uint8_t member_op = data[end] & GX_OPCODE_MASK;
    const std::uint32_t count = Be16(data + end + 1);
    const std::size_t bytes = 3u + std::size_t(count) * stride;
    if (!DrawRunMember(member_op, count) || end + bytes > available) break;
    const std::uint32_t expanded = DrawRunTriangleVertices(member_op, count);
    if (triangles + expanded > kDrawRunMaxVertices) break;
    mixed |= member_op != op;
    triangles += expanded;
    list += count;
    end += bytes;
    ++members;
  }
  if (members == 1u) { *miss = 1u; return 0u; }

  const bool keep_list = !mixed && (op == GX_QUADS || op == GX_TRIANGLES);
  const std::uint32_t total = keep_list ? list : triangles;
  const std::size_t base = out.size();
  out.resize(base + 3u + std::size_t(total) * stride);
  out[base] = keep_list ? cmd : static_cast<std::uint8_t>(GX_TRIANGLES | vat);
  out[base + 1u] = static_cast<std::uint8_t>(total >> 8u);
  out[base + 2u] = static_cast<std::uint8_t>(total);
  std::uint8_t* dst = out.data() + base + 3u;
  for (std::size_t at = 0u; at < end;) {
    const std::uint8_t member_op = data[at] & GX_OPCODE_MASK;
    const std::uint32_t count = Be16(data + at + 1);
    const std::uint8_t* v = data + at + 3u;
    const auto put = [&](std::uint32_t index) {
      CopyRunVertex(dst, v + std::size_t(index) * stride, stride);
      dst += stride;
    };
    if (keep_list || member_op == GX_TRIANGLES) {
      std::memcpy(dst, v, std::size_t(count) * stride);
      dst += std::size_t(count) * stride;
    } else if (member_op == GX_QUADS) {
      // Identical to aurora::gx::prepare_idx_buffer's quad expansion.
      for (std::uint32_t q = 0u; q < count; q += 4u) {
        put(q); put(q + 1u); put(q + 2u);
        put(q + 2u); put(q + 3u); put(q);
      }
    } else {
      for (std::uint32_t i = 2u; i < count; ++i) {
        // Identical to aurora::gx::prepare_idx_buffer's strip/fan expansion.
        if (member_op == GX_TRIANGLEFAN) { put(0u); put(i - 1u); put(i); }
        else if (i == 2u || (i & 1u) == 0u) { put(i - 2u); put(i - 1u); put(i); }
        else { put(i - 1u); put(i - 2u); put(i); }
      }
    }
    at += 3u + std::size_t(count) * stride;
  }
  *consumed = end;
  return members;
}

// Process the framed command at data[0..len); if it starts a run of small
// draws in the same vertex format, consume the whole run (bounded by
// `available`). Same failure contract as ProcessOne.
bool ProcessCommandOrRun(const std::uint8_t* data, std::size_t available, std::size_t len,
                         unsigned depth, std::size_t* consumed) {
  *consumed = len;
  const std::uint8_t cmd = data[0];
  if (!g_draw_runs || !IsDrawCommand(cmd) || len < 3u || len + 3u > available ||
      !IsDrawCommand(data[len]))
    return ProcessOne(data, len, depth);
  if (!DrawRunStateAllows()) {
    if (g_draw_census) ++g_draw_run_misses[2u];
    return ProcessOne(data, len, depth);
  }
  auto& out = g_draw_run_buffer;
  out.clear();
  unsigned miss = 0u;
  const std::uint32_t members = AssembleDrawRun(data, available, len,
      VertexSize(static_cast<GXVtxFmt>(cmd & GX_VAT_MASK)), out, consumed, &miss);
  if (members == 0u) {
    if (g_draw_census && miss < 4u) ++g_draw_run_misses[miss];
    *consumed = len;
    return ProcessOne(data, len, depth);
  }
  ++g_draw_run_runs;
  g_draw_run_members += members;
  if (g_native_ir_profile) g_native_ir_stats.decoder_units += members - 1u;
  return ProcessOne(out.data(), out.size(), depth);
}

void RememberFifoWrite(std::uint64_t value, std::uint8_t size, std::uint64_t stream_offset,
                       std::uint32_t guest_pc) {
  if(!g_fifo_forensic_trace) return;
  g_fifo_write_trace[g_fifo_write_trace_head] = {value, stream_offset, guest_pc, size};
  static_assert((g_fifo_write_trace.size() & (g_fifo_write_trace.size() - 1u)) == 0u);
  g_fifo_write_trace_head =
      (g_fifo_write_trace_head + 1u) & (g_fifo_write_trace.size() - 1u);
  if (g_fifo_write_trace_count < g_fifo_write_trace.size()) ++g_fifo_write_trace_count;
}

void RememberDecodedCommand(const std::uint8_t* p, std::size_t len, std::uint64_t stream_offset) {
  if (!p || len == 0u) return;
  g_fifo_last_cmd = p[0];
  g_fifo_last_cmd_len = len;
  const std::uint8_t op = p[0] & GX_OPCODE_MASK;
  if (op >= 0x80u && op <= 0xB8u && len >= 3u) {
    g_fifo_last_draw_fmt = p[0] & GX_VAT_MASK;
    g_fifo_last_draw_count = Be16(p + 1);
    g_fifo_last_draw_stride = g_fifo_last_draw_count == 0u
                                ? 0u
                                : static_cast<std::uint32_t>((len - 3u) / g_fifo_last_draw_count);
  }
  if(!g_fifo_forensic_trace) return;
  FifoCommandTrace trace{};
  trace.stream_offset = stream_offset;
  trace.len = len;
  trace.cmd = p[0];
  trace.fmt = g_fifo_last_draw_fmt;
  trace.count = g_fifo_last_draw_count;
  trace.stride = g_fifo_last_draw_stride;
  g_fifo_command_trace[g_fifo_command_trace_head] = trace;
  static_assert((g_fifo_command_trace.size() & (g_fifo_command_trace.size() - 1u)) == 0u);
  g_fifo_command_trace_head =
      (g_fifo_command_trace_head + 1u) & (g_fifo_command_trace.size() - 1u);
  if (g_fifo_command_trace_count < g_fifo_command_trace.size()) ++g_fifo_command_trace_count;
}

void DumpFifoFault(std::size_t fault_offset) {
  if (g_fifo_fault_reported) return;
  g_fifo_fault_reported = true;

  const std::size_t fifo_size = g_fifo.size();
  const std::size_t begin = fault_offset > 48u ? fault_offset - 48u : 0u;
  const std::size_t end = std::min<std::size_t>(fifo_size, fault_offset + 64u);
  const unsigned bad = fault_offset < fifo_size ? unsigned(g_fifo[fault_offset]) : 0xffu;

  const std::uint64_t absolute_fault = g_fifo_stream_base + fault_offset;
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_V2=1 fault_offset=%zu absolute=%llu fifo_base=%llu fifo_size=%zu unread=%zu need=%zu bad=0x%02x last_cmd=0x%02x last_len=%zu last_draw_fmt=%u last_draw_count=%u last_draw_stride=%u\n",
               fault_offset, static_cast<unsigned long long>(absolute_fault),
               static_cast<unsigned long long>(g_fifo_stream_base), fifo_size,
               fault_offset <= fifo_size ? fifo_size - fault_offset : 0u,
               g_fifo_need, bad, unsigned(g_fifo_last_cmd), g_fifo_last_cmd_len,
               unsigned(g_fifo_last_draw_fmt), unsigned(g_fifo_last_draw_count),
               unsigned(g_fifo_last_draw_stride));

  std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_BYTES begin=%zu end=%zu data=", begin, end);
  for (std::size_t i = begin; i < end; ++i) {
    if (i == fault_offset) std::fprintf(stderr, "[");
    std::fprintf(stderr, "%02x", unsigned(g_fifo[i]));
    if (i == fault_offset) std::fprintf(stderr, "]");
    if (i + 1u < end) std::fprintf(stderr, " ");
  }
  std::fprintf(stderr, "\n");

  const std::size_t count = g_fifo_write_trace_count;
  const std::size_t first = (g_fifo_write_trace_head + g_fifo_write_trace.size() - count) %
                            g_fifo_write_trace.size();
  std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_WRITES count=%zu", count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& w = g_fifo_write_trace[(first + i) % g_fifo_write_trace.size()];
    std::fprintf(stderr, " @%llu/pc=%08x/s%u:0x%0*llx",
                 static_cast<unsigned long long>(w.stream_offset), unsigned(w.guest_pc),
                 unsigned(w.size), unsigned(w.size) * 2,
                 static_cast<unsigned long long>(w.value));
  }
  std::fprintf(stderr, "\n");

  const std::size_t command_count = g_fifo_command_trace_count;
  const std::size_t command_first =
      (g_fifo_command_trace_head + g_fifo_command_trace.size() - command_count) %
      g_fifo_command_trace.size();
  std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_COMMANDS count=%zu", command_count);
  for (std::size_t i = 0; i < command_count; ++i) {
    const auto& c = g_fifo_command_trace[(command_first + i) % g_fifo_command_trace.size()];
    std::fprintf(stderr, " @%llu:cmd=%02x,len=%zu",
                 static_cast<unsigned long long>(c.stream_offset), unsigned(c.cmd), c.len);
    if (c.fmt < 8u)
      std::fprintf(stderr, ",fmt=%u,count=%u,stride=%u", unsigned(c.fmt),
                   unsigned(c.count), unsigned(c.stride));
  }
  std::fprintf(stderr, "\n");

  if (g_fifo_last_draw_fmt < 8u) {
    using namespace aurora::gx;
    const unsigned f = g_fifo_last_draw_fmt;
    const bool vcd_lo = g_gxState.cpRegValid.test(0x50u);
    const bool vcd_hi = g_gxState.cpRegValid.test(0x60u);
    const bool vat_a = g_gxState.cpRegValid.test(0x70u + f);
    const bool vat_b = g_gxState.cpRegValid.test(0x80u + f);
    const bool vat_c = g_gxState.cpRegValid.test(0x90u + f);
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_CP fmt=%u vcd_lo=%u:0x%08x vcd_hi=%u:0x%08x vat_a=%u:0x%08x vat_b=%u:0x%08x vat_c=%u:0x%08x aurora_last_fmt=%u aurora_last_size=%u\n",
                 f, vcd_lo ? 1u : 0u, g_gxState.cpRegCache[0x50u],
                 vcd_hi ? 1u : 0u, g_gxState.cpRegCache[0x60u],
                 vat_a ? 1u : 0u, g_gxState.cpRegCache[0x70u + f],
                 vat_b ? 1u : 0u, g_gxState.cpRegCache[0x80u + f],
                 vat_c ? 1u : 0u, g_gxState.cpRegCache[0x90u + f],
                 unsigned(g_gxState.lastVtxFmt), unsigned(g_gxState.lastVtxSize));
  }
}

bool ApplyPixelOperation(std::uint32_t word, bool direct_state, std::span<const std::uint8_t> wire) {
    const std::uint8_t reg=static_cast<std::uint8_t>(word>>24);
    // BP 0xFE masks exactly one following BP write. Aurora applies that mask
    // in handle_bp(), but intercepted display copies never reach its parser.
    // Decode the same effective word before deciding which copy path to take.
    auto& bp_cache=aurora::gx::g_gxState.bpRegCache;
    const std::uint32_t bp_mask=bp_cache[0xFEu]&0x00ffffffu;
    const std::uint32_t effective_word=reg==0xFEu ? word :
        (std::uint32_t(reg)<<24u)|
        ((bp_cache[reg]&~bp_mask)|(word&bp_mask))&0x00ffffffu;
    // XF aliases can invalidate decoded BP state without changing its word.
    const bool bp_changed = reg==0xFEu || !aurora::gx::g_gxState.bpRegValid.test(reg) || bp_cache[reg]!=effective_word;
    // SDKs and retail render loops aggressively re-emit GX state. Aurora
    // quite reasonably treats a BP write as a state mutation, but feeding an
    // identical value through handle_bp() dirties pipelines/textures anyway.
    // Suppress only registers whose write has no command-like side effect.
    // Token/draw-done/copy/TLUT/cache/preload and BP-mask writes always pass.
    const bool bp_side_effect = reg==0x45u || reg==0x47u || reg==0x48u ||
                                reg==0x52u || (reg>=0x55u && reg<=0x57u) ||
                                reg==0x65u || reg==0x66u || reg==0xFEu ||
                                (reg>=0x60u && reg<=0x67u);
    if(!bp_changed && !bp_side_effect) {
      // Every non-mask BP command consumes the current write mask. The value is
      // already present in Aurora's cache, so no other state needs touching.
      bp_cache[0xFEu]=0x00ffffffu;
      return true;
    }

    // IMAGE0 and MODE1 can change dimensions/mip span without a new IMAGE3
    // address write. Track those register groups only on an actual mutation,
    // then skip the eight-slot scans on ordinary draws.
    if(bp_changed && ((reg>=0x84u&&reg<=0x8bu)||(reg>=0xa4u&&reg<=0xabu)))
      g_texture_mapping_scan_dirty=true;
    if(bp_changed && ((reg>=0x88u&&reg<=0x8bu)||(reg>=0xa8u&&reg<=0xabu)||
                      (reg>=0x98u&&reg<=0x9bu)||(reg>=0xb8u&&reg<=0xbbu)))
      g_tlut_binding_scan_dirty=true;
    if(g_z16r_copy_trace && reg==0x43u &&
       (bp_cache[reg]&7u)!=(effective_word&7u) && g_z16r_bp_logs++<96u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_Z16R_TRACE_V1 event=pe-mode before=%u after=%u bp_mask=%06x effective=%06x\n",
                   bp_cache[reg]&7u,effective_word&7u,bp_mask,effective_word&0x00ffffffu);
    if(g_ztexture_trace && (reg==0xF4u || reg==0xF5u) && g_ztexture_logs++<96u) {
      if(reg==0xF4u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE_V115 event=bias value=%06x mask=%06x\n",
                     effective_word&0x00ffffffu,bp_mask);
      else
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE_V115 event=control type=%u op=%u raw=%06x mask=%06x\n",
                     effective_word&3u,(effective_word>>2u)&3u,effective_word&0x00ffffffu,bp_mask);
    }
    const auto consume_intercepted_bp=[&] {
      bp_cache[reg]=effective_word;
      aurora::gx::g_gxState.bpRegValid.set(reg);
      bp_cache[0xFEu]=0x00ffffffu;
    };
    if (reg == 0x55u || reg == 0x56u) {
      consume_intercepted_bp();
      return GekkoAOT::GX::WriteNativeBoundingBox(reg, effective_word & 0x00ffffffu);
    }
    if(reg==0x65u) {
      consume_intercepted_bp();
      MapTlut();
      return !g_fifo_fault;
    }
    if(reg>=0x49u && reg<=0x4Eu) TrackRawCopyRegister(reg,effective_word);
    if(reg==0x52u && ((effective_word>>14)&1u)!=0u) {
      g_display_insets.EndFrame(g_display_copy_count);
      {
        // A pre-rendered backdrop switches the next frames to 4:3 at
        // once; three frames without one switch back.
        const bool was_43=g_frame_force_43.load(std::memory_order_relaxed);
        g_recorded_force_43=was_43;
        bool force_43=was_43;
        if (g_frame_prerendered) { force_43=true; g_prerendered_idle=0; }
        else if (++g_prerendered_idle>=3u) force_43=false;
        g_frame_prerendered=false;
        if (force_43!=was_43) {
          g_frame_force_43.store(force_43,std::memory_order_relaxed);
          aurora::gx::g_gxState.dirty|=aurora::gx::DirtyUniform;
          static unsigned prerendered_logs=0;
          if (prerendered_logs++<16u)
            std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_PRERENDERED_43_V363 copy=%llu mode=%s\n",
                         static_cast<unsigned long long>(g_display_copy_count),force_43 ? "4:3" : "widescreen");
        }
        const bool was_flat=g_flat_frames.Flat();
        g_flat_frames.EndFrame(g_frame_depth_draws);
        g_frame_depth_draws=0;
        static unsigned flat_logs=0;
        if (g_wide_perspective_ui && g_flat_frames.Flat()!=was_flat && flat_logs++<16u)
          std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_PERSPECTIVE_UI_V362 copy=%llu screen=%s\n",
                       static_cast<unsigned long long>(g_display_copy_count),
                       g_flat_frames.Flat() ? "2d-cards-ui-scissor" : "3d");
      }
      if(g_interp_probe_enabled) {
        auto& probe=g_interp_probe;
        std::unordered_multimap<std::uint64_t,std::uint64_t> previous;
        previous.reserve(probe.previous.size());
        for(const auto& [sig,mtx] : probe.previous) previous.emplace(sig,mtx);
        for(const auto& [sig,mtx] : probe.current) {
          ++probe.draws;
          const auto it=previous.find(sig);
          if(it==previous.end()) continue;
          ++probe.matched;
          if(it->second!=mtx) ++probe.moved;
          previous.erase(it);
        }
        probe.previous.swap(probe.current);
        probe.current.clear();
        if(++probe.frames%120u==0u) {
          std::fprintf(stderr,"GEKKOAOT_INTERP_PROBE frames=%llu draws_per_frame=%.0f matched=%.1f%% moved_of_matched=%.1f%%\n",
                       static_cast<unsigned long long>(probe.frames),double(probe.draws)/120.0,
                       probe.draws ? 100.0*double(probe.matched)/double(probe.draws) : 0.0,
                       probe.matched ? 100.0*double(probe.moved)/double(probe.matched) : 0.0);
          probe.draws=probe.matched=probe.moved=0;
        }
      }
      g_copy_provenance.BeginFrame();
      ++g_display_copy_count;
      g_camera_copy_epoch.store(g_display_copy_count,std::memory_order_release);
      consume_intercepted_bp();
      const bool clear=((effective_word>>11)&1u)!=0u;
      using namespace GekkoAOT::GX;
      OrderedRenderGraph graph(Resource(RenderResource::EfbColor) | Resource(RenderResource::EfbDepth));
      if (!graph.Add({RenderPass::DisplayCopy,
                      Resource(RenderResource::EfbColor) | Resource(RenderResource::EfbDepth),
                      Resource(RenderResource::DisplayCopy)})) return false;
      // Aurora snapshots indexed GX arrays into GPU storage and deliberately
      // reuses that cachedRange until GXInvalidateVtxCache. Retail games also
      // commonly stream/skin geometry by rewriting the same guest RAM between
      // display copies. GekkoAOT sees those CPU writes directly, while Aurora
      // cannot, so keeping the previous cachedRange can render stale vertices
      // with an otherwise perfectly valid FIFO. GXCopyDisp is the safest GX
      // frame boundary we have: the old geometry has already been consumed by
      // the EFB->XFB copy and the following frame may now reuse the same RAM.
      if (g_array_frame_refresh && !graph.Add({RenderPass::PublishArrayCache,
          Resource(RenderResource::DisplayCopy), Resource(RenderResource::GuestArrayCache)})) return false;
      const bool recorded = graph.Execute([&](RenderPass pass) {
        if (pass == RenderPass::DisplayCopy) return LatchDisplayCopyCached(effective_word, clear);
        RefreshIndexedArraySnapshots();
        return true;
      });
      if (recorded && g_native_ir_profile) ++g_native_ir_stats.graph_copies;
      return recorded;
    }
    const bool texture_copy = reg==0x52u;
    // EFB copies rewrite guest RAM. v389: forget only the sources that
    // overlap the destination (rows of 4-line tiles at the copy stride bound
    // every format's footprint; 8-line tile formats are covered twice).
    if(texture_copy) {
      const unsigned shift=(effective_word>>9u)&1u;
      const std::uint32_t height=std::max(1u,static_cast<unsigned>(std::max(0,int(aurora::gx::g_gxState.texCopySrc.height)))>>shift);
      const std::uint64_t span=std::uint64_t(g_copy_stride_bytes)*((height+3u)/4u);
      if(g_copy_range_forget && g_copy_dest_valid && span!=0u && span<=0x01000000u)
        ForgetTextureSourceRange(g_copy_dest_physical,static_cast<std::uint32_t>(span));
      else
        ForgetTextureSourceVersions();
    }
    if(texture_copy && !PrepareRawTextureCopy(effective_word)) {
      // Bad raw copy state is preferable to poisoning Aurora's copy-texture
      // cache with a null/stale destination. Continue guest execution and let
      // the next complete copy state recover naturally.
      consume_intercepted_bp();
      return true;
    }
    // Aurora records a GXCopyTex as a sealed pass whose FrameOp captures
    // its pass/upload pointers and staging high-water marks; passes, ops and
    // uploads live in std::deque, and encode_op touches only worker-owned
    // state. Later draws bind the copy through copyTextures, ordered on the
    // same frame encoder. Waiting for the render worker on both sides of every
    // copy (v78) only serialized the GX and render threads; keep it as an
    // opt-in diagnostic.
    static const bool copy_fence = EnvBool("GEKKOAOT_GX_COPY_FENCE", false);
    if(texture_copy) {
      if(copy_fence && !aurora::gfx::render_worker::is_worker_thread())
        aurora::gfx::render_worker::synchronize();
      static unsigned trace_before=0;
      if(trace_before++<64u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_EFB_COPY_FENCE_V78=1 phase=before dest_phys=%08x fmt=%u\n",
                     g_copy_dest_physical,unsigned(aurora::gx::g_gxState.texCopyFmt));
    }
    if(direct_state) aurora::gx::fifo::handle_bp(word);
    else (void)ProcessAuroraCompatibility(wire.data(),static_cast<std::uint32_t>(wire.size()));
    if(reg==0x66u) InvalidateTextureCacheV87();
    if(texture_copy) {
      if(copy_fence && !aurora::gfx::render_worker::is_worker_thread())
        aurora::gfx::render_worker::synchronize();
      static unsigned trace_after=0;
      if(trace_after++<64u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_EFB_COPY_FENCE_V78=1 phase=after dest_phys=%08x fmt=%u\n",
                     g_copy_dest_physical,unsigned(aurora::gx::g_gxState.texCopyFmt));
    }
    if((reg>=0x94u&&reg<=0x97u)||(reg>=0xB4u&&reg<=0xB7u)) MapTextureImage3(reg,effective_word);
    if(reg==0x65u) MapTlut();
    if(g_pe&&(reg==0x45u||reg==0x47u||reg==0x48u))
      {
        GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::PeCommandParsed, reg, effective_word&0x00ffffffu);
        g_pe(g_pe_user,reg,effective_word&0x00ffffffu);
      }
    return true;
}
