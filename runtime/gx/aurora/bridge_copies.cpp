// SPDX-License-Identifier: GPL-3.0-or-later
// Guest memory notifications and raw EFB copy tracking.
// Included by bridge.cpp, whose translation unit holds the bridge state; this
// file is not compiled on its own.

// Reserve before uploading any compact array. The shader receives a wrapping
// u32 bias, never a host pointer/offset or a WebGPU binding range. Clear these
// one-draw ranges immediately after submission; frame rotation and storage
// rollover must never interpret the bias as a persistent storage allocation.
struct CompactArrayDraw {
  std::uint32_t mask=0;
  void Clear() {
    g_replay_compact_mask=0;
    if(!mask) return;
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)
      if((mask&(1u<<attr))!=0u) aurora::gx::g_gxState.arrays[attr].cachedRange={};
    aurora::gx::g_gxState.dirty|=aurora::gx::DirtyImmediates;
    mask=0;
  }
  ~CompactArrayDraw() { Clear(); }

  void Prepare(const CompactArrayPlan& plan,const GekkoAOT::GX::NativeAttributePlan& bindings) {
    using namespace aurora::gx;
    auto selected=SelectCompactArrayMask(plan,bindings);
    PrimePersistentIndexedArraysV161H(selected,bindings);
    if(!selected && !g_native_array_snapshots) return;
    const auto projected=[&](std::uint32_t candidate) {
      std::uint64_t end=aurora::gfx::storage_size();
      const auto alignment=aurora::gfx::storage_alignment();
      const auto append=[&](std::uint64_t bytes) {
        if(!bytes) return;
        // Conservative for every upload order, including compact arrays first.
        end+=(alignment ? alignment-1u : 0u)+bytes;
      };
      bindings.ForEach(g_native_attribute_plan,[&](unsigned attr) {
        const auto& array=g_gxState.arrays[attr];
        const auto desc=g_gxState.vtxDesc[attr];
        if((desc!=GX_INDEX8 && desc!=GX_INDEX16) || array.cachedRange.size) return;
        append((candidate&(1u<<attr))!=0u ? plan[attr].bytes : array.size);
      });
      // Aurora may build/change its fog LUT while choosing the shader/pipeline.
      // Its exact maximum payload is one f32 per render-target pixel column.
      append(std::uint64_t(aurora::gfx::get_render_target_size().x)*sizeof(float));
      return end;
    };
    auto required=projected(selected);
    if(required>aurora::gfx::storage_capacity()) {
      if(!aurora::gfx::rollover_storage_pass(static_cast<std::size_t>(required))) {
        ++g_compact_array_fallbacks;
        PrimePersistentIndexedArraysV161H(0,bindings);
        return;
      }
      ++g_compact_array_rollovers;
      selected=SelectCompactArrayMask(plan,bindings);
      PrimePersistentIndexedArraysV161H(selected,bindings);
      required=projected(selected);
      if(required>aurora::gfx::storage_capacity()) {
        ++g_compact_array_fallbacks;
        PrimePersistentIndexedArraysV161H(0,bindings);
        return;
      }
    }
    bool prepared=false;
    bool failed=false;
    bindings.ForEach(g_native_attribute_plan,[&](unsigned attr) {
      if(failed) return;
      const bool compact=(selected&(1u<<attr))!=0u;
      auto& array=g_gxState.arrays[attr];
      const auto desc=g_gxState.vtxDesc[attr];
      // Short full arrays (including INDEX8 and low INDEX16 indices)
      // need the same immutable resource identity as compact suffixes. Stage
      // them before draw selection so native batching sees validated bases.
      if(!compact && (!g_native_array_snapshots || array.cachedRange.size || !array.data ||
          !array.size || array.size>8u*1024u || (desc!=GX_INDEX8 && desc!=GX_INDEX16))) return;
      const auto window=compact ? plan[attr] : GekkoAOT::GX::CompactArrayWindow{0,array.size};
      const auto range=aurora::gfx::push_storage_snapshot(
          static_cast<const std::uint8_t*>(array.data)+window.prefix,window.bytes,!compact);
      if(range.size!=window.bytes) {
        ++g_compact_array_fallbacks;
        Clear();
        PrimePersistentIndexedArraysV161H(0,bindings);
        failed=true;
        return;
      }
      array.cachedRange={GekkoAOT::GX::CompactArrayShaderBase(range.offset,window.prefix),
                         window.bytes};
      if(window.prefix!=0u) { g_replay_compact_prefix[attr]=window.prefix; g_replay_compact_mask|=1u<<attr; }
      else g_replay_compact_mask&=~(1u<<attr);
      prepared=true;
      if(compact) {
        mask|=1u<<attr;
        ++g_compact_array_uploads;
        g_compact_array_bytes+=window.bytes;
        g_compact_array_prefix_bytes+=array.size;
      } else {
        ++g_native_array_snapshot_stages;
      }
    });
    if(prepared) g_gxState.dirty|=DirtyImmediates;
  }
};

// One native resource allocation/copy for all indexed attributes of a
// short draw, without min/max window uploads or per-attribute snapshot lookup.
// Rewritten indices live exclusively in host memory. CP state, guest arrays
// and display-list bytes keep their original representation.
struct GatheredArrayDraw {
  const std::uint8_t* vertices=nullptr;
  std::uint32_t mask=0;
  ~GatheredArrayDraw() {
    if(!mask) return;
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)
      if(mask&(1u<<attr)) aurora::gx::g_gxState.arrays[attr].cachedRange={};
    aurora::gx::g_gxState.dirty|=aurora::gx::DirtyImmediates;
  }
  bool Prepare(const std::uint8_t* p,std::size_t len,GXVtxFmt fmt,unsigned count) {
    using namespace aurora::gx;
    if(!g_native_vertex_gather ||
       count>GekkoAOT::GX::NativeVertexGather::MaxVertices) return false;
    const auto* layout=GetIndexScanLayout(fmt);
    if(!layout || !layout->gather.count) return false;
    std::array<GekkoAOT::GX::NativeVertexGather::Source,4> sources;
    for(unsigned f=0;f<layout->field_count;++f) {
      const auto attr=layout->fields[f].attr;
      const auto& array=g_gxState.arrays[attr];
      sources[f]={static_cast<const std::uint8_t*>(array.data),g_array_available[attr],array.stride};
    }
    GekkoAOT::GX::NativeVertexGather::Result gathered;
    if(!layout->gather.Execute({p+3,len-3},count,{sources.data(),layout->field_count},
                              g_native_gather_workspace,gathered)) return false;
    const auto alignment=aurora::gfx::storage_alignment();
    const auto padding=alignment ? alignment-1u : 0u;
    const auto required=aurora::gfx::storage_size()+padding+
        gathered.bytes+padding+
        std::size_t(aurora::gfx::get_render_target_size().x)*sizeof(float);
    if(required>aurora::gfx::storage_capacity() &&
       !aurora::gfx::rollover_storage_pass(required)) return false;
    const auto uploaded=aurora::gfx::push_storage(g_native_gather_workspace.storage.data(),gathered.bytes);
    if(uploaded.size!=gathered.bytes) return false;
    for(unsigned f=0;f<layout->field_count;++f) {
      const auto attr=layout->fields[f].attr;
      auto& array=g_gxState.arrays[attr];
      const auto& range=gathered.ranges[f];
      array.size=std::max(array.size,range.maximum*array.stride+layout->fields[f].element_size);
      array.cachedRange={uploaded.offset+range.offset,range.bytes};
      mask|=1u<<attr;
    }
    vertices=g_native_gather_workspace.vertices.data();
    g_gxState.dirty|=DirtyImmediates;
    ++g_native_gather_draws;
    g_native_gather_bytes+=gathered.bytes;
    g_native_gather_windows+=gathered.source_window_bytes;
    return true;
  }
};

void NotifyGuestMemoryWriteRange(std::uint32_t address, std::uint32_t size) {
  using namespace aurora::gx;
  if(size==0u) return;
  ++g_guest_memory_write_notifications;
  ForgetTextureSourceRange(address,size);

  unsigned invalidated_arrays=0u;
  for(int i=GX_VA_POS;i<GX_VA_MAX_ATTR;++i) {
    auto& array=g_gxState.arrays[i];
    if(array.cachedRange.size==0u || array.size==0u || array.data==nullptr) continue;
    if(!GuestRangeOverlaps(address,size,g_array_guest_base[i],array.size)) continue;
    array.cachedRange={};
    ++invalidated_arrays;
    ++g_array_cache_invalidations;
  }
  if(invalidated_arrays!=0u)
    g_gxState.dirty |= DirtyImmediates;

  unsigned invalidated_textures=0u;
  for(unsigned map=0;map<MaxTextures;++map) {
    const auto bit=static_cast<std::uint8_t>(1u<<map);
    if((g_texture_image3_valid&bit)==0u) continue;
    auto& slot=g_gxState.loadedTextures[map];
    if(slot.data==nullptr) continue;
    const std::uint32_t guest=(g_texture_image3_word[map]&0x00ffffffu)<<5u;
    const auto format=static_cast<std::uint32_t>(slot.format());
    const auto width=static_cast<std::uint32_t>(slot.width());
    const auto height=static_cast<std::uint32_t>(slot.height());
    const auto mips=static_cast<std::uint32_t>(slot.mip_count());
    auto& footprint=g_texture_write_footprints[map];
    if(!footprint.valid || footprint.format!=format || footprint.width!=width ||
       footprint.height!=height || footprint.mips!=mips) {
      footprint={format,width,height,mips,
                 texture::texture_source_size(slot.format(),slot.width(),slot.height(),slot.mip_count()),true};
      ++g_texture_write_span_misses;
    } else {
      ++g_texture_write_span_hits;
    }
    if(footprint.bytes==0u || !GuestRangeOverlaps(address,size,guest,footprint.bytes)) continue;
    slot.texDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
    ++invalidated_textures;
    ++g_texture_memory_invalidations;
  }
  if(invalidated_textures!=0u) {
    texture::invalidate_bindings();
    g_gxState.dirty |= DirtyTextures;
  }

  if(invalidated_arrays==0u && invalidated_textures==0u) ++g_guest_memory_write_noops;
  if((invalidated_arrays!=0u || invalidated_textures!=0u) &&
     g_array_cache_control_logs++<64u) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_MEMORY_COHERENCY_V159=1 ea=%08x bytes=%u arrays=%u textures=%u\n",
                 address,size,invalidated_arrays,invalidated_textures);
  }
}

void NotifyGuestVertexCacheControl(std::uint8_t operation, std::uint32_t address) {
  // 0/1/2 are dcbst/dcbf/dcbi in HostRuntime. GekkoAOT's RAM is host-coherent,
  // but these guest cache-management instructions are the publication signal
  // for CPU-authored GX arrays and textures. Invalidate only the touched line.
  if(operation>2u) return;
  ++g_array_cache_control_events;
  NotifyGuestMemoryWriteRange(address&~31u,32u);
}

void RefreshIndexedArraySnapshots() {
  using namespace aurora::gx;
  bool any=false;
  for(int i=GX_VA_POS;i<=GX_VA_TEX7;++i) {
    auto& array=g_gxState.arrays[i];
    if(array.data && array.cachedRange.size!=0u) {
      array.cachedRange={};
      any=true;
    }
  }
  if(any) g_gxState.dirty |= DirtyImmediates;
}

bool DecodeRawCopyFormat(std::uint32_t word, GXTexFmt* out) {
  using namespace aurora::gx;
  if(!out) return false;
  // BP 0x52 stores the 4-bit EFB-copy format rotated left by one. Dolphin's
  // hardware model expresses the inverse as fmt/2 + (fmt&1)*8.
  const std::uint32_t encoded=Bits(word,3u,4u);
  const std::uint32_t real=(encoded>>1u)|((encoded&1u)<<3u);
  const bool intensity=Bits(word,15u,1u)!=0u;
  const bool auto_conv=Bits(word,16u,1u)!=0u;
  const bool depth=g_gxState.pixelFmt==GX_PF_Z24;

  if(depth) {
    switch(real) {
    case 0x0u: *out=GX_CTF_Z4; return true;
    case 0x1u: *out=GX_TF_Z8; return true;
    case 0x3u: *out=GX_TF_Z16; return true;
    case 0x6u: *out=GX_TF_Z24X8; return true;
    case 0x9u: *out=GX_CTF_Z8M; return true;
    case 0xAu: *out=GX_CTF_Z8L; return true;
    case 0xBu: *out=static_cast<GXTexFmt>(0x3Bu); return true; // Z16R = 0x0B | ZTF | CTF
    case 0xCu: *out=GX_CTF_Z16L; return true;
    default: return false;
    }
  }
  if(intensity) {
    switch(real) {
    case 0x0u: *out=GX_TF_I4; return true;
    case 0x1u: *out=GX_TF_I8; return true;
    case 0x2u: *out=GX_TF_IA4; return true;
    case 0x3u: *out=GX_TF_IA8; return true;
    default: return false;
    }
  }
  switch(real) {
  case 0x0u: *out=GX_CTF_R4; return true;
  case 0x2u: *out=GX_CTF_RA4; return true;
  case 0x3u: *out=GX_CTF_RA8; return true;
  case 0x4u: *out=GX_TF_RGB565; return true;
  case 0x5u: *out=GX_TF_RGB5A3; return true;
  case 0x6u: *out=auto_conv ? GX_CTF_YUVA8 : GX_TF_RGBA8; return true;
  case 0x7u: *out=GX_CTF_A8; return true;
  case 0x8u: *out=GX_CTF_R8; return true;
  case 0x9u: *out=GX_CTF_G8; return true;
  case 0xAu: *out=GX_CTF_B8; return true;
  case 0xBu: *out=GX_CTF_RG8; return true;
  case 0xCu: *out=GX_CTF_GB8; return true;
  case 0xEu: *out=GX_TF_CMPR; return true;
  default: return false;
  }
}

void TrackRawCopyRegister(std::uint8_t reg, std::uint32_t word) {
  using namespace aurora::gx;
  const std::uint32_t value=word&0x00ffffffu;
  switch(reg) {
  case 0x49u: {
    const auto left=static_cast<std::int32_t>(Bits(value,0u,10u));
    const auto top=static_cast<std::int32_t>(Bits(value,10u,10u));
    g_gxState.texCopySrc.x=left;
    g_gxState.texCopySrc.y=top;
    break;
  }
  case 0x4Au: {
    const auto width=Bits(value,0u,10u)+1u;
    const auto height=Bits(value,10u,10u)+1u;
    g_gxState.texCopySrc.width=static_cast<std::int32_t>(width);
    g_gxState.texCopySrc.height=static_cast<std::int32_t>(height);
    // A texture copy has no vertical scaling. The retail BP stream does not
    // carry the high-level GXSetTexCopyDst dimensions directly; for raw FIFO
    // use the exact source rectangle dimensions (the common SDK contract) and
    // retain stride separately for diagnostics.
    g_gxState.texCopyDstWidth=width;
    g_gxState.texCopyDstHeight=height;
    g_gxState.texCopyDstWide=true;
    g_copy_src_valid=true;
    break;
  }
  case 0x4Bu: {
    const std::uint32_t physical=(value&0x001fffffu)<<5u;
    const void* ptr=nullptr; std::uint32_t available=0;
    if(Resolve(physical,1u,&ptr,&available)) {
      g_gxState.texCopyDest=ptr;
      g_copy_dest_physical=physical;
      g_copy_dest_valid=true;
    } else {
      g_gxState.texCopyDest=nullptr;
      g_copy_dest_physical=physical;
      g_copy_dest_valid=false;
    }
    break;
  }
  case 0x4Du:
    g_copy_stride_bytes=(value&0x3ffu)<<5u;
    break;
  case 0x4Eu:
    g_copy_yscale_raw=value&0x1ffu;
    break;
  default:
    break;
  }
}

GekkoAOT::GX::TextureViewportRoles g_texture_viewport_roles;

bool PrepareRawTextureCopy(std::uint32_t word) {
  using namespace aurora::gx;
  GXTexFmt format{};
  const bool format_valid=DecodeRawCopyFormat(word,&format);
  if(g_z16r_copy_trace && Bits(word,3u,4u)==0x7u) {
    const std::uint32_t rows=g_gxState.texCopySrc.height>0 ?
        (static_cast<std::uint32_t>(g_gxState.texCopySrc.height)+3u)/4u : 0u;
    const std::uint64_t span=std::uint64_t(g_copy_stride_bytes)*rows;
    g_z16r_copy_dest=g_copy_dest_physical;
    g_z16r_copy_span=static_cast<std::uint32_t>(std::min<std::uint64_t>(span,0xffffffffu));
    g_z16r_bound_maps=0;
    for(unsigned map=0;map<MaxTextures;++map) {
      if((g_texture_image3_valid&(1u<<map))==0u) continue;
      const std::uint64_t base=std::uint64_t(g_texture_image3_word[map]&0x00ffffffu)<<5u;
      if(base>=g_z16r_copy_dest && base<std::uint64_t(g_z16r_copy_dest)+g_z16r_copy_span)
        g_z16r_bound_maps=static_cast<std::uint8_t>(g_z16r_bound_maps|(1u<<map));
    }
    if(g_z16r_copy_logs++<96u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_Z16R_TRACE_V1 event=copy pe_ctrl=%06x pixfmt=%u ctrl=%06x src=%d,%d,%dx%d dest=%08x span=%u stride=%u format=%u valid=%u\n",
                   g_gxState.bpRegCache[0x43u]&0x00ffffffu,
                   unsigned(g_gxState.pixelFmt),word&0x00ffffffu,
                   g_gxState.texCopySrc.x,g_gxState.texCopySrc.y,
                   g_gxState.texCopySrc.width,g_gxState.texCopySrc.height,
                   g_z16r_copy_dest,g_z16r_copy_span,g_copy_stride_bytes,
                   unsigned(format),format_valid?1u:0u);
  }
  if(!g_copy_src_valid || !g_copy_dest_valid || !g_gxState.texCopyDest || !format_valid) {
    static unsigned failures=0;
    if(failures++<16u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_EFB_COPY_V2=0 src=%u dest=%u phys=%08x ctrl=%06x action=skip-invalid-copy\n",
                   g_copy_src_valid?1u:0u,g_copy_dest_valid?1u:0u,
                   g_copy_dest_physical,word&0x00ffffffu);
    return false;
  }
  const unsigned shift=Bits(word,9u,1u);
  g_gxState.texCopyDstWidth=std::max(1u,static_cast<unsigned>(g_gxState.texCopySrc.width)>>shift);
  g_gxState.texCopyDstHeight=std::max(1u,static_cast<unsigned>(g_gxState.texCopySrc.height)>>shift);
  g_gxState.texCopyFmt=format;
  const auto display=aurora::vi::configured_fb_size();
  const auto& view=g_gxState.logicalViewport;
  const bool texture_role_changed=g_texture_viewport_roles.Observe(view.left,view.top,view.width,view.height,
      g_gxState.texCopySrc.x,g_gxState.texCopySrc.y,g_gxState.texCopySrc.width,
      g_gxState.texCopySrc.height,float(display.x),float(display.y),
      g_camera_copy_epoch.load(std::memory_order_relaxed));
  // A copy establishes the viewport's role after its first recording. Cached
  // uniforms and enhancement coverage must follow that evidence next draw.
  if(texture_role_changed)g_gxState.dirty|=DirtyUniform|DirtyPipeline;
  g_display_insets.NoteCopy(float(g_gxState.texCopySrc.x),float(g_gxState.texCopySrc.width),
                            g_display_copy_count);
  if(g_replay_capture_on.load(std::memory_order_relaxed))
    g_replay_building.copies.push_back({static_cast<std::uint32_t>(g_replay_building.blocks.size()),
        {float(g_gxState.texCopySrc.x),float(g_gxState.texCopySrc.y),
         float(g_gxState.texCopySrc.width),float(g_gxState.texCopySrc.height)}});
  g_copy_provenance.NoteCopy(g_gxState.texCopyDest,float(g_gxState.texCopySrc.x),float(g_gxState.texCopySrc.y),
                             float(g_gxState.texCopySrc.width),float(g_gxState.texCopySrc.height),
                             DisplayCanvasWidth(),float(aurora::vi::configured_fb_size().y));
  static unsigned copy_logs=0;
  if(g_trace_gx_copy && copy_logs++<128u)
    std::fprintf(stderr,"GEKKOAOT_TRACE_GX_COPY event=texture-copy width=%u height=%u half=%u format=%u\n",
                 g_gxState.texCopyDstWidth,g_gxState.texCopyDstHeight,shift,unsigned(format));
  static unsigned logs=0;
  if(logs++<24u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_EFB_COPY_V2=1 src=%d,%d %dx%d dest_phys=%08x stride=%u fmt=%u depth=%u\n",
                 g_gxState.texCopySrc.x,g_gxState.texCopySrc.y,
                 g_gxState.texCopySrc.width,g_gxState.texCopySrc.height,
                 g_copy_dest_physical,g_copy_stride_bytes,unsigned(format),
                 g_gxState.pixelFmt==GX_PF_Z24?1u:0u);
  return true;
}


XfbZeroCopySlot* FindXfbSlot(std::uint32_t address) {
  for (auto& slot : g_xfb_slots) {
    if (slot.valid && slot.address == address) return &slot;
  }
  return nullptr;
}

// VI field origins are addresses inside XFB memory, not necessarily the exact
// base used by GXCopyDisp.  A common interlaced layout is BOTTOM = TOP + one
// 640x2-byte scanline (0x500).  Resolve such addresses against the latched XFB
// memory range instead of treating them as unrelated textures.
XfbZeroCopySlot* FindXfbSlotForScanout(std::uint32_t address,
                                      std::uint32_t* byte_offset) {
  if (byte_offset) *byte_offset = 0u;
  if (auto* exact = FindXfbSlot(address)) return exact;

  XfbZeroCopySlot* newest = nullptr;
  std::uint32_t newest_offset = 0u;
  for (auto& slot : g_xfb_slots) {
    if (!slot.valid || slot.span_bytes == 0u || address < slot.address) continue;
    const std::uint64_t offset = static_cast<std::uint64_t>(address - slot.address);
    if (offset >= slot.span_bytes) continue;
    if (slot.stride_bytes != 0u && (offset % slot.stride_bytes) != 0u) continue;
    if (!newest || slot.generation > newest->generation) {
      newest = &slot;
      newest_offset = static_cast<std::uint32_t>(offset);
    }
  }
  if (newest && byte_offset) *byte_offset = newest_offset;
  return newest;
}

XfbZeroCopySlot* AcquireXfbSlot(std::uint32_t address) {
  if (auto* existing = FindXfbSlot(address)) return existing;
  for (auto& slot : g_xfb_slots) {
    if (!slot.valid) {
      slot.address = address;
      return &slot;
    }
  }
  auto* oldest = &g_xfb_slots[0];
  for (auto& slot : g_xfb_slots) {
    if (slot.generation < oldest->generation) oldest = &slot;
  }
  oldest->address = address;
  oldest->stride_bytes = 0u;
  oldest->native_width = 0u;
  oldest->native_height = 0u;
  oldest->span_bytes = 0u;
  oldest->generation = 0u;
  oldest->snapshot.reset();
  oldest->frame_buffer = {};
  oldest->resolved = {};
  oldest->valid = false;
  return oldest;
}

bool RenderTextureReusable(const aurora::webgpu::TextureWithSampler& texture,
                           const wgpu::Extent3D& size, wgpu::TextureFormat format) {
  return texture.texture && texture.view && texture.size.width == size.width &&
         texture.size.height == size.height && texture.format == format;
}

const aurora::webgpu::TextureWithSampler* XfbPresentSource(const XfbZeroCopySlot& slot) {
  if (!slot.valid) return nullptr;
  // V12 XFB cache entries are always independent single-sample Aurora copy
  // textures.  Do not select the old MSAA-resolved rotation slot based on the
  // live EFB sample count; that would make every cached XFB invisible whenever
  // host MSAA is enabled.
  if (slot.snapshot)
    return slot.frame_buffer.texture ? &slot.frame_buffer : nullptr;
  if (aurora::webgpu::g_graphicsConfig.msaaSamples > 1)
    return slot.resolved.texture ? &slot.resolved : nullptr;
  return slot.frame_buffer.texture ? &slot.frame_buffer : nullptr;
}

// The uniform blocks of the frame this display copy ends.
std::shared_ptr<ReplayCapture> FinishReplayCapture(const XfbZeroCopySlot& slot) {
  auto capture=std::make_shared<ReplayCapture>(std::move(g_replay_building));
  g_replay_building={};
  g_replay_building.blocks.reserve(capture->blocks.size());
  g_replay_building.bytes.reserve(capture->bytes.size());
  g_replay_building.copies.reserve(capture->copies.size());
  g_replay_occurrences.clear();
  capture->frame_id=aurora::gfx::gekkoaot_recording_frame_id();
  capture->produced=std::chrono::steady_clock::now();
  if (const auto* source=XfbPresentSource(slot)) capture->present=*source;
  // Bounding-box draws feed values the guest reads back. (A frame that
  // loads the previous EFB is restored by Aurora before each replay, v360.)
  capture->eligible=capture->frame_id!=0 && capture->present.texture && !capture->blocks.empty() &&
                    !g_replay_frame_bbox;
  g_replay_frame_bbox=false;
  g_replay_captures.fetch_add(1,std::memory_order_relaxed);
  if (capture->eligible) g_replay_eligible.fetch_add(1,std::memory_order_relaxed);
  static unsigned capture_logs=0;
  if (capture_logs++<4u)
    std::fprintf(stderr,"GEKKOAOT_FRAME_REPLAY_CAPTURE_V358 frame=%llu blocks=%zu bytes=%zu present=%u eligible=%u\n",
                 static_cast<unsigned long long>(capture->frame_id),capture->blocks.size(),capture->bytes.size(),
                 capture->present.texture ? 1u : 0u,capture->eligible ? 1u : 0u);
  // Draws rendering into a small region the frame then copies to a texture
  // (shadow maps, light masks) stay exactly as in the real frame: what
  // samples that texture (projective texture matrices) is not interpolated,
  // so a moved copy would shake against its receivers. Full-screen copies
  // (the scene itself, post effects) still interpolate.
  {
    const auto efb=aurora::vi::configured_fb_size();
    const float efb_area=std::max(1.f,float(efb.x)*float(efb.y));
    std::uint32_t start=0;
    for (const auto& copy : capture->copies) {
      const float* r=copy.rect;
      if (r[2]*r[3]<=0.4f*efb_area)
        for (std::uint32_t i=start;i<copy.blocks && i<capture->blocks.size();++i) {
          auto& block=capture->blocks[i];
          const float* v=block.view;
          if (v[0]>=r[0]-2.f && v[1]>=r[1]-2.f && v[0]+v[2]<=r[0]+r[2]+2.f && v[1]+v[3]<=r[1]+r[3]+2.f)
            block.producer=true;
        }
      start=copy.blocks;
    }
  }
  capture->index.reserve(capture->blocks.size());
  for (std::uint32_t i=0;i<capture->blocks.size();++i) {
    capture->index.emplace(capture->blocks[i].key,i);
    capture->groups[capture->blocks[i].sig].push_back(i);
  }
  g_replay_array_last.clear();
  for (const auto& array : capture->arrays) g_replay_array_last[array.key]=array.hash;
  g_replay_array_seen.clear();
  return capture;
}
// Runs on the render worker right after the frame was submitted there:
// replays only ever see frames the worker retains.
std::int64_t ReplayClockNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Render worker: times the work submitted so far, from `start` to the moment
// the GPU finished it. A replay is the frame's passes again without its uploads,
// so while no replay runs the real frame's cost keeps the estimate current and
// the unlock comes back by itself once the scene gets lighter.
void TimeSubmittedWork(std::int64_t start,bool replay) {
  g_replay_gpu_start_ns.store(start,std::memory_order_release);
  aurora::webgpu::g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
    [start,replay](wgpu::QueueWorkDoneStatus,wgpu::StringView) {
      // Completion is only seen at the next poll, half a host tick late on average.
      const std::int64_t cost=std::max<std::int64_t>(
          ReplayClockNs()-start-g_replay_tick_ns.load(std::memory_order_relaxed)/2,0);
      const auto blend=[cost](std::atomic<std::int64_t>& value) {
        const std::int64_t old=value.load(std::memory_order_relaxed);
        value.store(old ? (old*7+cost)/8 : cost,std::memory_order_relaxed);
      };
      if (!replay) blend(g_replay_frame_cost_ns);
      if (replay || g_replay_direct.load(std::memory_order_relaxed)) blend(g_replay_cost_ns);
      std::int64_t expected=start;
      g_replay_gpu_start_ns.compare_exchange_strong(expected,0,std::memory_order_acq_rel);
    });
}

// Render worker: smoothed time between guest frames, whichever way they are shown.
void NoteGuestFrame() {
  static std::int64_t last=0;
  const std::int64_t now=ReplayClockNs();
  const std::int64_t interval=last ? now-last : 0;
  last=now;
  if (interval<=0 || interval>250000000) return;
  const std::int64_t old=g_replay_period_ns.load(std::memory_order_relaxed);
  g_replay_period_ns.store(old ? (old*7+interval)/8 : interval,std::memory_order_relaxed);
}

void PublishReplayCapture(std::shared_ptr<ReplayCapture> capture) {
  NoteGuestFrame();
  {
    std::lock_guard lock{g_replay_mutex};
    g_replay_clock.OnFrame(ReplaySeconds(capture->produced));
    g_replay_last_frame_s.store(ReplaySeconds(std::chrono::steady_clock::now()),std::memory_order_relaxed);
    g_replay_previous=std::move(g_replay_current);
    g_replay_current=std::move(capture);
  }
  TimeSubmittedWork(ReplayClockNs(),false);
}
