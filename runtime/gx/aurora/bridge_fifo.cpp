// SPDX-License-Identifier: GPL-3.0-or-later
// Native draw processing, FIFO parsing and draining.
// Included by bridge.cpp, whose translation unit holds the bridge state; this
// file is not compiled on its own.

// Both FIFO and compiled primitive lists enter the same draw semantics. The
// native list supplies topology/count/format directly; no command framing or
// opcode dispatch is repeated on a cache hit. Array content/lifetimes remain
// validated below before Aurora records its immutable resource ranges.
bool ProcessNativeDraw(const GekkoAOT::GX::NativeDraw& draw,const std::uint8_t* p) {
  PhaseScope phase_scope(kPhDraw);
  ++g_trace_native_draws;
    const auto cmd=std::uint8_t(draw.topology)|draw.format;
    const auto fmt=static_cast<GXVtxFmt>(draw.format);
    const auto prim=static_cast<GXPrimitive>(draw.topology);
    const auto count=draw.count;
    const std::size_t len=draw.bytes;
    static const bool trace_xf=EnvBool("GEKKOAOT_TRACE_GX_STATE",false);
    NoteDraw(count);
    if(g_texture_mapping_scan_dirty) {
      RefreshTextureMappings();
      g_texture_mapping_scan_dirty=false;
    }
    if(g_tlut_binding_scan_dirty) {
      BindRawTluts();
      g_tlut_binding_scan_dirty=false;
    }
    using aurora::gx::g_gxState;
    // GEKKOAOT_LOGIC_TRACE=1: logic-op draws per op (Aurora snapshots the
    // framebuffer per primitive for these), reported every 600 frames.
    static const bool logic_trace=EnvBool("GEKKOAOT_LOGIC_TRACE",false);
    if(logic_trace && g_gxState.blendMode==GX_BM_LOGIC) {
      static std::array<std::uint64_t,16> draws{}, vertices{};
      static std::uint64_t last_copy=0;
      ++draws[unsigned(g_gxState.blendOp)&15u]; vertices[unsigned(g_gxState.blendOp)&15u]+=count;
      if(g_display_copy_count>=last_copy+600u) {
        last_copy=g_display_copy_count;
        std::fprintf(stderr,"GEKKOAOT_LOGIC_TRACE copies=%llu",static_cast<unsigned long long>(g_display_copy_count));
        for(unsigned op=0;op<16u;++op) if(draws[op])
          std::fprintf(stderr," op%u=%llu/%llu",op,static_cast<unsigned long long>(draws[op]),static_cast<unsigned long long>(vertices[op]));
        std::fprintf(stderr," prim=%u color=%u alpha=%u\n",unsigned(prim),g_gxState.colorUpdate?1u:0u,g_gxState.alphaUpdate?1u:0u);
      }
    }
    if(g_texture_source_invalidation_pending && count!=0u) {
      for(unsigned map=0;map<aurora::gx::MaxTextures;++map) {
        auto& texture=g_gxState.loadedTextures[map];
        if(!texture.data) continue;
        const auto span=aurora::gx::texture::texture_source_size(
            texture.format(),texture.width(),texture.height(),texture.mip_count());
        if(g_texture_source_watched_invalidation && (g_texture_image3_valid&(1u<<map))!=0u) {
          const std::uint32_t guest=(g_texture_image3_word[map]&0x00ffffffu)<<5u;
          const auto& entry=TextureSourceSlot(guest,texture.image0,static_cast<std::uint8_t>(texture.mip_count()));
          if(entry.version!=0u && entry.version==texture.texDataVersion && entry.guest==guest &&
             entry.image0==texture.image0 && entry.mips==texture.mip_count() && entry.span==span)
            continue;
        }
        texture.texDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
        if((g_texture_image3_valid&(1u<<map))!=0u && span<=0xffffffffu)
          RememberTextureSourceVersion((g_texture_image3_word[map]&0x00ffffffu)<<5u,texture.image0,
                                       static_cast<std::uint8_t>(texture.mip_count()),
                                       static_cast<std::uint32_t>(span),texture.texDataVersion);
        if(g_draw_census) { ++g_tex_census_forced; g_tex_census_forced_bytes+=span; }
      }
      g_texture_source_invalidation_pending=false;
    }
    if(g_trace_font) {
      static std::array<std::uint64_t,128> font_draw_keys{};
      static unsigned font_draw_samples=0;
      const auto& tex=g_gxState.loadedTextures[0];
      const std::uint64_t key=(std::uint64_t(g_gxState.bpRegCache[0x94])<<32u)|
          (std::uint64_t(tex.image0)<<8u)|g_gxState.numTexGens;
      bool seen=false;
      for(unsigned i=0;i<font_draw_samples;++i) seen|=font_draw_keys[i]==key;
      if(!seen && font_draw_samples<font_draw_keys.size()) {
        font_draw_keys[font_draw_samples++]=key;
        std::fprintf(stderr,
          "GEKKOAOT_T3_FONT_DIAG prim=%u vertices=%u format=%u size=%ux%u image0=%06x image3=%06x tlut0=%06x tlut_src=%06x tlut_load=%06x tev_color=%06x tev_alpha=%06x alpha_compare=%06x blend=%06x texgens=%u\n",
          unsigned(prim),unsigned(count),unsigned(tex.format()),tex.width(),tex.height(),
          g_gxState.bpRegCache[0x88]&0xffffffu,
          g_gxState.bpRegCache[0x94]&0xffffffu,g_gxState.bpRegCache[0x98]&0xffffffu,
          g_gxState.bpRegCache[0x64]&0xffffffu,g_gxState.bpRegCache[0x65]&0xffffffu,
          g_gxState.bpRegCache[0xc0]&0xffffffu,g_gxState.bpRegCache[0xc1]&0xffffffu,
          g_gxState.bpRegCache[0xf3]&0xffffffu,g_gxState.bpRegCache[0x41]&0xffffffu,
          unsigned(g_gxState.numTexGens));
      }
    }
    if(trace_xf) {
      using namespace aurora::gx;
      static std::vector<std::uint64_t> traced_draw_states;
      std::uint64_t signature=1469598103934665603ull;
      for(const auto reg : {0x00,0x28,0x41,0xc0,0xc1,0xf3})
        signature=(signature^g_gxState.bpRegCache[reg])*1099511628211ull;
      signature=(signature^g_gxState.loadedTextures[0].image0)*1099511628211ull;
      signature=(signature^unsigned(g_gxState.projType))*1099511628211ull;
      if(traced_draw_states.size()<256u && std::find(traced_draw_states.begin(),traced_draw_states.end(),signature)==traced_draw_states.end()) {
        traced_draw_states.push_back(signature);
        const auto& tcg=g_gxState.tcgs[0];
        std::fprintf(stderr,"GEKKOAOT_TRACE_GX_STATE event=draw vertices=%u pn=%u texgens=%u tev=%u tex0=%ux%u:%u tref=%06x color=%06x alpha=%06x compare=%06x blend=%06x texgen=%u:%u mtx=%u post=%u normalize=%u dual=%u viewport=%.2f,%.2f,%.2f,%.2f\n",
                     count,g_gxState.currentPnMtx,g_gxState.numTexGens,g_gxState.numTevStages,
                     g_gxState.loadedTextures[0].width(),g_gxState.loadedTextures[0].height(),unsigned(g_gxState.loadedTextures[0].format()),
                     g_gxState.bpRegCache[0x28]&0xffffffu,g_gxState.bpRegCache[0xc0]&0xffffffu,g_gxState.bpRegCache[0xc1]&0xffffffu,
                     g_gxState.bpRegCache[0xf3]&0xffffffu,g_gxState.bpRegCache[0x41]&0xffffffu,
                     unsigned(tcg.type),unsigned(tcg.src),unsigned(tcg.mtx),unsigned(tcg.postMtx),unsigned(tcg.normalize),
                     g_gxState.xfRegCache[0x12],g_gxState.logicalViewport.left,g_gxState.logicalViewport.top,
                     g_gxState.logicalViewport.width,g_gxState.logicalViewport.height);
        const auto& tracedTex=g_gxState.loadedTextures[0];
        std::fprintf(stderr,"GEKKOAOT_TRACE_GX_TRANSFORM proj=%u copy=%u data=%p matrix=",unsigned(g_gxState.projType),unsigned(g_gxState.copyTextures.contains(tracedTex.data)),static_cast<const void*>(tracedTex.data));
        const auto row=g_gxState.currentPnMtx;
        for(unsigned r=0;r<3 && row+r<g_gxState.xfPosRows.size();++r)
          for(unsigned c=0;c<4;++c)std::fprintf(stderr,"%.5g,",g_gxState.xfPosRows[row+r][c]);
        std::fprintf(stderr," projection=");
        for(const auto* r : {&g_gxState.proj.m0,&g_gxState.proj.m1,&g_gxState.proj.m2,&g_gxState.proj.m3})
          for(unsigned c=0;c<4;++c)std::fprintf(stderr,"%.5g,",(*r)[c]);
        std::fprintf(stderr," scissor=%d,%d,%d,%d\n",g_gxState.logicalScissor.x,g_gxState.logicalScissor.y,g_gxState.logicalScissor.width,g_gxState.logicalScissor.height);
        std::fprintf(stderr,"GEKKOAOT_TRACE_GX_TINT prev=%.3f,%.3f,%.3f,%.3f reg0=%.3f,%.3f,%.3f,%.3f material=%.3f,%.3f,%.3f,%.3f source=%u lighting=%u depth=%u,%u,%u cull=%u\n",
          g_gxState.colorRegs[0][0],g_gxState.colorRegs[0][1],g_gxState.colorRegs[0][2],g_gxState.colorRegs[0][3],
          g_gxState.colorRegs[1][0],g_gxState.colorRegs[1][1],g_gxState.colorRegs[1][2],g_gxState.colorRegs[1][3],
          g_gxState.colorChannelState[0].matColor[0],g_gxState.colorChannelState[0].matColor[1],g_gxState.colorChannelState[0].matColor[2],g_gxState.colorChannelState[0].matColor[3],
          unsigned(g_gxState.colorChannelConfig[0].matSrc),unsigned(g_gxState.colorChannelConfig[0].lightingEnabled),
          unsigned(g_gxState.depthCompare),unsigned(g_gxState.depthUpdate),unsigned(g_gxState.depthFunc),unsigned(g_gxState.cullMode));
        const auto& uvfmt=g_gxState.vtxFmts[fmt].attrs[GX_VA_TEX0];
        std::fprintf(stderr,"GEKKOAOT_TRACE_GX_UV scale=%u,%u mode=%u type=%u frac=%u stride=%zu raw=",
          unsigned(g_gxState.texCoordScales[0].scaleS)+1,unsigned(g_gxState.texCoordScales[0].scaleT)+1,
          unsigned(g_gxState.vtxDesc[GX_VA_TEX0]),unsigned(uvfmt.type),unsigned(uvfmt.frac),count?(len-3)/count:0);
        for(unsigned byte=3;byte<std::min<std::size_t>(len,67);++byte)std::fprintf(stderr,"%02x",p[byte]);
        std::fprintf(stderr,"\n");
      }
    }
    CaptureDraw(cmd,p,len);

    // GXBegin with zero vertices is a legal no-op in retail code.  The framed
    // command is only the 3-byte opcode/count header; do not send it through
    // the raw-draw helper, whose contract intentionally requires vertex data.
    // Treating this as a FIFO fault made harmless zero-count triangle strips
    // (notably opcode 0x98) terminate the whole native runtime.
    if(count==0u) {
      static bool zero_draw_logged=false;
      if(!zero_draw_logged) {
        zero_draw_logged=true;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ZERO_DRAW_V13_1=1 opcode=0x%02x action=noop\n",
                     unsigned(cmd));
      }
      return true;
    }

    // GX_CULL_ALL is a legal state (used by Luigi's Mansion). WebGPU has no
    // equivalent and Aurora rejects it when constructing a triangle pipeline.
    // Consume the framed vertices without submitting culled polygons.
    if(g_gxState.cullMode == GX_CULL_ALL && prim != GX_POINTS &&
       prim != GX_LINES && prim != GX_LINESTRIP) return true;

    std::optional<PhaseScope> prep_scope(std::in_place,kPhDrawPrep);
    GatheredArrayDraw gathered_draw;
    std::optional<PhaseScope> sub_scope(std::in_place,kPhPGather);
    const bool gathered=gathered_draw.Prepare(p,len,fmt,count);
    sub_scope.reset(); sub_scope.emplace(kPhPWindows);
    CompactArrayPlan compact_plan{};
    const GekkoAOT::GX::NativeAttributePlan* bindings=nullptr;
    if(!gathered && !PrepareIndexedArrayWindows(p,len,g_compact_array_windows ? &compact_plan : nullptr,&bindings)) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR opcode=0x%02x action=reject-array-window\n",
                   unsigned(cmd));
      return false;
    }
    CompactArrayDraw compact_draw;
    sub_scope.reset(); sub_scope.emplace(kPhPCompact);
    if(!gathered) {
      if(g_compact_array_windows || g_native_array_snapshots) compact_draw.Prepare(compact_plan,*bindings);
      else PrimePersistentIndexedArraysV161H(0,*bindings);
    }
    sub_scope.reset();
    prep_scope.reset();
    const bool screen_fill=ClassifyScreenFill(p,len,prim,fmt,count);
    const bool previous_perspective_ui=g_draw_perspective_ui;
    if(g_gxState.projType==GX_PERSPECTIVE) {
      if(g_gxState.depthCompare && g_gxState.depthUpdate) ++g_frame_depth_draws;
      g_draw_perspective_ui=g_wide_perspective_ui && !g_gxState.depthUpdate && g_flat_frames.Flat();
    } else {
      g_draw_perspective_ui=false;
    }
    if(previous_perspective_ui!=g_draw_perspective_ui)g_gxState.dirty|=aurora::gx::DirtyUniform;
    if (g_native_ir_profile)
      g_scene_evidence.Observe({g_gxState.projType == GX_PERSPECTIVE,
          g_gxState.depthCompare, g_gxState.depthUpdate,
          g_gxState.blendMode != GX_BM_NONE, aurora::gfx::is_offscreen(),
          screen_fill, g_draw_perspective_ui});
    if(g_interp_probe_enabled) {
      std::uint64_t sig=1469598103934665603ull, mtx=1469598103934665603ull;
      const auto mix=[](std::uint64_t& h,std::uint64_t v){ h=(h^v)*1099511628211ull; };
      mix(sig,unsigned(prim)); mix(sig,count); mix(sig,unsigned(fmt)); mix(sig,unsigned(g_gxState.projType));
      mix(sig,g_gxState.numTevStages); mix(sig,g_gxState.numTexGens);
      for(unsigned stage=0;stage<g_gxState.numTevStages && stage<aurora::gx::MaxTevStages;++stage) {
        const auto map=g_gxState.tevStages[stage].texMapId;
        mix(sig,map==GX_TEXMAP_NULL ? 0u : reinterpret_cast<std::uintptr_t>(g_gxState.loadedTextures[unsigned(map)&7u].data));
      }
      mix(sig,reinterpret_cast<std::uintptr_t>(g_gxState.arrays[GX_VA_POS].data));
      const unsigned pn=g_gxState.currentPnMtx;
      for(unsigned r=0;r<3u && pn+r<g_gxState.xfPosRows.size();++r)
        for(unsigned c=0;c<4u;++c) { std::uint32_t b; std::memcpy(&b,&g_gxState.xfPosRows[pn+r][c],4); mix(mtx,b); }
      for(unsigned c=0;c<4u;++c) { std::uint32_t b; std::memcpy(&b,&g_gxState.proj.m0[c],4); mix(mtx,b); }
      g_interp_probe.current.emplace_back(sig,mtx);
    }
    if(g_replay_capture_on.load(std::memory_order_relaxed)) {
      // Stable across frames: what is drawn and with which textures. Vertex
      // array addresses are left out (CPU-skinned meshes double-buffer them).
      std::uint64_t sig=1469598103934665603ull;
      const auto mix=[](std::uint64_t& h,std::uint64_t v){ h=(h^v)*1099511628211ull; };
      mix(sig,unsigned(prim)); mix(sig,count); mix(sig,unsigned(fmt)); mix(sig,unsigned(g_gxState.projType));
      mix(sig,g_gxState.numTevStages); mix(sig,g_gxState.numTexGens); mix(sig,g_gxState.currentPnMtx);
      for(unsigned stage=0;stage<g_gxState.numTevStages && stage<aurora::gx::MaxTevStages;++stage) {
        const auto map=g_gxState.tevStages[stage].texMapId;
        mix(sig,map==GX_TEXMAP_NULL ? 0u : reinterpret_cast<std::uintptr_t>(g_gxState.loadedTextures[unsigned(map)&7u].data));
      }
      g_replay_draw_sig=sig;
      g_replay_draw_pnmtx=g_gxState.currentPnMtx;
      if(GekkoAOT::GX::NativeBoundingBoxActive() && GekkoAOT::GX::NativeBoundingBoxRead()) g_replay_frame_bbox=true;
    }
    if(g_ui_insets && RecordAspectMode()==1u && !aurora::gfx::is_offscreen()) {
      const float canvas=DisplayCanvasWidth();
      const auto& view=g_gxState.logicalViewport;
      const auto& clip=g_gxState.logicalScissor;
      if(GekkoAOT::GX::IsInsetSpan(view.left,view.width,canvas))
        g_display_insets.NoteDraw(view.left,view.width,g_display_copy_count);
      if(GekkoAOT::GX::IsInsetSpan(float(clip.x),float(clip.width),canvas))
        g_display_insets.NoteDraw(float(clip.x),float(clip.width),g_display_copy_count);
    }
    if(g_wide_span_trace && g_gxState.projType==GX_ORTHOGRAPHIC) {
      // Diagnostic: textures sampled by orthographic draws and their EFB-copy provenance.
      static std::vector<std::uint64_t> seen_ortho;
      for(unsigned stage=0;stage<g_gxState.numTevStages && stage<aurora::gx::MaxTevStages;++stage) {
        const auto map=g_gxState.tevStages[stage].texMapId;
        if(map==GX_TEXMAP_NULL) continue;
        const auto& slot=g_gxState.loadedTextures[unsigned(map)&7u];
        const auto copy=g_gxState.copyTextures.find(slot.data);
        const bool is_copy=slot.data && copy!=g_gxState.copyTextures.end();
        const int sw=is_copy?copy->second.guestSource.width:0, sh=is_copy?copy->second.guestSource.height:0;
        const std::uint64_t key=(std::uint64_t(slot.width())<<40)^(std::uint64_t(slot.height())<<28)^
            (std::uint64_t(is_copy)<<27)^(std::uint64_t(sw)<<14)^std::uint64_t(sh);
        if(seen_ortho.size()<64u && std::find(seen_ortho.begin(),seen_ortho.end(),key)==seen_ortho.end()) {
          seen_ortho.push_back(key);
          std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_ORTHO_TEX_TRACE tex=%ux%u fmt=%u copy=%u source=%dx%d view=%.0fx%.0f count=%u\n",
                       slot.width(),slot.height(),slot.raw_format(),is_copy?1u:0u,sw,sh,
                       g_gxState.logicalViewport.width,g_gxState.logicalViewport.height,unsigned(count));
        }
      }
    }
    if (screen_fill!=g_screen_fill_draw) {
      g_screen_fill_draw=screen_fill;
      g_gxState.dirty|=aurora::gx::DirtyUniform;
    }

    if(g_z16r_copy_trace && g_z16r_bound_maps!=0u && g_z16r_draw_logs<96u) {
      std::uint8_t sampled_maps=0;
      const auto& state=aurora::gx::g_gxState;
      for(unsigned stage=0;stage<state.numTevStages && stage<aurora::gx::MaxTevStages;++stage) {
        const unsigned map=unsigned(state.tevStages[stage].texMapId);
        if(map<aurora::gx::MaxTextures)
          sampled_maps=static_cast<std::uint8_t>(sampled_maps|(1u<<map));
      }
      if((sampled_maps&g_z16r_bound_maps)!=0u) {
        ++g_z16r_draw_logs;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_Z16R_TRACE_V1 event=draw-consumer bound=%02x sampled=%02x opcode=%02x count=%u\n",
                     g_z16r_bound_maps,sampled_maps,unsigned(cmd),unsigned(count));
      }
    }
    if(g_array_base_trace) {
      ++g_array_base_trace_draws;
      for(int i=GX_VA_POS;i<=GX_VA_TEX7;++i) {
        const auto desc=aurora::gx::g_gxState.vtxDesc[i];
        if(desc!=GX_INDEX8 && desc!=GX_INDEX16) continue;
        const auto& array=aurora::gx::g_gxState.arrays[i];
        if(array.cachedRange.size==0u && array.size!=0u) {
          ++g_array_base_trace_uploads;
          g_array_base_trace_upload_bytes+=array.size;
        }
      }
      if((g_array_base_trace_draws&0x7fffu)==0u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ARRAY_DRAW_TRACE_V1 draws=%llu uploads=%llu upload_bytes=%llu gx_invalidate=%llu window_growth=%llu window_cached=%llu window_bytes=%llu eager_growth=%llu\n",
                     static_cast<unsigned long long>(g_array_base_trace_draws),
                     static_cast<unsigned long long>(g_array_base_trace_uploads),
                     static_cast<unsigned long long>(g_array_base_trace_upload_bytes),
                     static_cast<unsigned long long>(g_array_base_trace_invalidate),
                     static_cast<unsigned long long>(g_array_base_trace_window_growth),
                     static_cast<unsigned long long>(g_array_base_trace_window_cached),
                     static_cast<unsigned long long>(g_array_base_trace_window_bytes),
                     static_cast<unsigned long long>(g_array_base_trace_eager_growth));
    }
    ObservePerspectiveCamera(count);
    {
      aurora::gfx::enhanced::GeometrySourceScope original_geometry({p+3,len-3u});
      PhaseScope submit_scope(kPhDrawSubmit);
      if(aurora::gx::fifo::submit_raw_draw(prim,fmt,count,gathered ? gathered_draw.vertices : p+3,
                                           static_cast<std::uint32_t>(len-3u)))
        return true;
    }

    // Accepted layouts have the exact same byte count as the raw draw. Keep
    // the original semantic path if the renderer rejects a gathered payload.
    if(gathered) {
      for(unsigned f=0;f<GetIndexScanLayout(fmt)->field_count;++f)
        g_gxState.arrays[GetIndexScanLayout(fmt)->fields[f].attr].cachedRange={};
      const GekkoAOT::GX::NativeAttributePlan* original_bindings=nullptr;
      if(!PrepareIndexedArrayWindows(p,len,&compact_plan,&original_bindings)) return false;
      compact_draw.Prepare(compact_plan,*original_bindings);
    }

    // The direct path is an optimisation, never a compatibility requirement.
    // If its stricter framing contract rejects a non-empty draw, preserve the
    // pre-v4 behaviour by giving the complete command to Aurora's normal FIFO
    // decoder.  Only fail closed when even that decoder cannot consume it.
    const auto parsed=ProcessAuroraCompatibility(p,static_cast<std::uint32_t>(len));
    if(parsed.bytesProcessed==len) {
      static bool fallback_logged=false;
      if(!fallback_logged) {
        fallback_logged=true;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_RAW_DRAW_FALLBACK_V13_1=1 policy=aurora-parser\n");
      }
      return true;
    }
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_FIFO_ERROR opcode=0x%02x action=raw-draw-and-parser-reject\n",
                 unsigned(cmd));
    return false;
}

bool ProcessOne(const std::uint8_t* p, std::size_t len, unsigned depth) {
  PhaseScope phase_scope(kPhState);
  const std::uint8_t cmd=p[0], op=cmd & GX_OPCODE_MASK;
  if (g_phase_profile && depth == 0u) {
    if (op == kLoadBpOpcode && len >= 2u) ++g_fifo_census[256u + p[1]];
    else if (op == GX_LOAD_XF_REG && len >= 5u) ++g_fifo_census[128u + std::min<unsigned>((Be32(p + 1) & 0xffffu) >> 8u, 127u)];
    else ++g_fifo_census[op >> 3u];
  }
  if (g_native_ir_profile) ++g_native_ir_stats.decoder_units;
  struct ExecutionRoute {
    std::uint64_t before = g_native_ir_profile ? g_native_ir_stats.compatibility : 0;
    ~ExecutionRoute() {
      if (g_native_ir_profile && before == g_native_ir_stats.compatibility)
        g_native_ir_stats.in_compatibility = false;
    }
  } route;
  // GekkoAOT has already framed and validated this complete command.
  // Enter Aurora's native state handlers directly instead of asking its FIFO
  // interpreter to decode the same opcode/header a second time. Keep BP copy,
  // mask, token, texture provenance and CP array handling below authoritative.
  static const bool direct_state=EnvBool("GEKKOAOT_NATIVE_GX_DIRECT_STATE",true);
  static const bool trace_xf=EnvBool("GEKKOAOT_TRACE_GX_STATE",false);
  if(trace_xf && op==GX_LOAD_XF_REG) {
    const auto header=Be32(p+1);
    const unsigned address=header&0xffffu, count=(header>>16)+1u;
    static unsigned xf_logs=0;
    if(address>=0x101au && address<=0x1026u && xf_logs++<128u)
      std::fprintf(stderr,"GEKKOAOT_TRACE_GX_STATE event=xf-projection-viewport address=%04x words=%u first=%08x\n",
                   address,count,Be32(p+5));
  }
  if(g_array_base_trace && op==GX_CMD_INVL_VC) ++g_array_base_trace_invalidate;
  if(g_draw_census) {
    if(op==GX_CMD_CALL_DL) NoteDrawGap(256u);
    else if(op==GX_LOAD_CP_REG) NoteDrawGap(p[1]>=0xA0u ? 8u : 16u);
    else if(op==GX_LOAD_XF_REG) NoteDrawGap((Be32(p+1)&0xffffu)<0x1000u ? 32u : 64u);
    else if(op==GX_LOAD_INDX_A||op==GX_LOAD_INDX_B||op==GX_LOAD_INDX_C||op==GX_LOAD_INDX_D) NoteDrawGap(128u);
    else if(op==kLoadBpOpcode) {
      const std::uint8_t reg=p[1];
      if(reg==0x52u) NoteDrawGap(512u);
      else if((reg>=0x80u&&reg<=0xBBu)||reg==0x64u||reg==0x65u) NoteDrawGap(1u);
      else if((reg>=0xC0u&&reg<=0xFDu)||(reg>=0x28u&&reg<=0x2Fu)||(reg>=0x40u&&reg<=0x42u)) NoteDrawGap(2u);
      else if(reg!=0x45u&&reg!=0x47u&&reg!=0x48u) NoteDrawGap(4u);
    }
  }
  if(op==GX_CMD_CALL_DL) {
    // The retail CP has one display-list call level. A stream may call one DL,
    // but a DL cannot recursively call another DL. The old host recursion cap
    // of 16 accepted command streams the hardware never could have executed.
    if(depth>=1u) {
      static unsigned dl_depth_logs=0;
      if(dl_depth_logs++<32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_DL_DEPTH_V87=0 depth=%u max=1 action=reject-nested-display-list\n",
                     depth);
      return false;
    }
    const std::uint32_t addr=Be32(p+1), bytes=Be32(p+5);
    const void* data=nullptr; std::uint32_t available=0;
    if(!Resolve(addr,bytes,&data,&available) || available<bytes)return false;
    std::span<const std::uint8_t> source{static_cast<const std::uint8_t*>(data),bytes};
    // One immutable copy per call; lists cannot nest (depth<=1), so a single
    // reused buffer replaces a heap allocation for every uncached list.
    static std::vector<std::uint8_t> snapshot;
    if(g_fifo_native_ir && direct_state && bytes) {
      const auto plan=g_native_display_lists.Find(addr,source);
      if(!plan.source.empty())source=plan.source;
      if(plan.operations) {
        if(g_native_ir_profile)g_native_ir_stats.decoder_units+=plan.commands;
        FlushEfbPokes();
        for(const auto& operation:*plan.operations)
          if(!SubmitNativeOperation(operation,GekkoAOT::GX::NativeOrigin::Fifo,true))return false;
        if(g_native_ir_profile) {
          g_native_ir_stats.dl_operations_executed+=plan.operations->size();
          if(plan.cached)g_native_ir_stats.dl_commands_reused+=plan.commands;
        }
        return true;
      }
    }
    if(g_fifo_native_ir && direct_state && bytes) {
      GekkoAOT::GX::NativeDrawLayout layout;
      const auto& state=aurora::gx::g_gxState;
      for(unsigned reg=0x50;reg<=0x97;++reg)
        if(state.cpRegValid.test(reg))layout.Write(reg,state.cpRegCache[reg]);
      const auto plan=[&]{ PhaseScope lookup_scope(kPhListCompile); return g_native_draw_lists.Find(addr,source,layout); }();
      if(!plan.source.empty())source=plan.source;
      if(plan.operations){
        PhaseScope list_scope(kPhListExec);
        FlushEfbPokes();
        const auto& ops=*plan.operations;
        // Draws adjacent in the plan and contiguous in the list are the
        // same back-to-back commands the FIFO path merges into runs (HP
        // titles: ~33k tiny draws per frame inside cached lists). The merged
        // commands are assembled once per cached plan; whether the renderer
        // state allows merging is decided at every execution.
        auto* runs=g_draw_runs ? plan.runs : nullptr;
        if(g_phase_profile && plan.runs) {
          // How often a cached list starts from the same GX state as its
          // previous execution (matrices and array contents excluded).
          const auto& st=aurora::gx::g_gxState;
          std::uint64_t parts[5];
          parts[0]=XXH3_64bits(st.bpRegCache.data(),sizeof st.bpRegCache);
          parts[1]=XXH3_64bits(st.cpRegCache.data(),sizeof st.cpRegCache);
          parts[2]=XXH3_64bits(st.xfRegCache.data(),sizeof st.xfRegCache);
          std::uint64_t ids[2*aurora::gx::MaxTextures+8]{};
          for(unsigned m=0;m<aurora::gx::MaxTextures;++m) {
            ids[2*m]=reinterpret_cast<std::uintptr_t>(st.loadedTextures[m].data);
            ids[2*m+1]=st.loadedTextures[m].texDataVersion;
          }
          parts[3]=XXH3_64bits(ids,2*aurora::gx::MaxTextures*8);
          for(int a=GX_VA_POS;a<=GX_VA_TEX7;++a) ids[a-GX_VA_POS]=reinterpret_cast<std::uintptr_t>(st.arrays[a].data);
          parts[4]=XXH3_64bits(ids,8*8);
          const std::uint64_t sigma=XXH3_64bits(parts,sizeof parts);
          ++g_replay_census_calls;
          if(plan.runs->last_sigma==sigma) ++g_replay_census_same;
          for(unsigned k=0;k<5u;++k) if(plan.runs->last_parts[k]!=parts[k]) ++g_replay_census_diff[k];
          std::memcpy(plan.runs->last_parts,parts,sizeof parts);
          plan.runs->last_sigma=sigma;
        }
        if(runs && !runs->built) {
          runs->built=true;
          for(std::size_t i=0;i<ops.size();) {
            const auto* draw=std::get_if<GekkoAOT::GX::NativeDraw>(&ops[i]);
            if(!draw) { ++i; continue; }
            std::size_t j=i+1u;
            std::uint32_t end=draw->offset+draw->bytes;
            while(j<ops.size()) {
              const auto* next=std::get_if<GekkoAOT::GX::NativeDraw>(&ops[j]);
              if(!next || next->offset!=end) break;
              end+=next->bytes; ++j;
            }
            while(i+1u<j) {
              const auto& head=std::get<GekkoAOT::GX::NativeDraw>(ops[i]);
              const std::size_t base=runs->bytes.size();
              std::size_t consumed=0u; unsigned miss=0u;
              const auto members=AssembleDrawRun(source.data()+head.offset,end-head.offset,head.bytes,head.stride,
                                                 runs->bytes,&consumed,&miss);
              if(members<2u) { ++i; continue; }
              const auto* merged=runs->bytes.data()+base;
              const std::uint32_t count=(std::uint32_t(merged[1])<<8u)|merged[2];
              GekkoAOT::GX::NativeDrawRun run;
              run.first=static_cast<std::uint32_t>(i);
              run.end=static_cast<std::uint32_t>(i+members);
              run.merged=GekkoAOT::GX::NativeDraw{GekkoAOT::GX::NativeTopology(merged[0]&GX_OPCODE_MASK),
                  std::uint8_t(merged[0]&GX_VAT_MASK),std::uint16_t(count),head.stride,
                  std::uint32_t(base),std::uint32_t(runs->bytes.size()-base)};
              runs->runs.push_back(run);
              i+=members;
            }
            i=std::max(i,j);
          }
        }
        std::size_t next_run=0u;
        for(std::size_t i=0;i<ops.size();++i){
          const auto& operation=ops[i];
          if(const auto* draw=std::get_if<GekkoAOT::GX::NativeDraw>(&operation)){
            if(runs) {
              while(next_run<runs->runs.size() && runs->runs[next_run].first<i) ++next_run;
              if(next_run<runs->runs.size() && runs->runs[next_run].first==i && DrawRunStateAllows()) {
                const auto& run=runs->runs[next_run];
                if(!ProcessNativeDraw(run.merged,runs->bytes.data()+run.merged.offset))return false;
                ++g_draw_run_runs;
                g_draw_run_members+=run.end-run.first;
                g_native_draw_list_executed+=run.end-run.first;
                i=run.end-1u;
                continue;
              }
            }
            if(!ProcessNativeDraw(*draw,source.data()+draw->offset))return false;
            ++g_native_draw_list_executed;
          }else if(!SubmitNativeOperation(std::get<GekkoAOT::GX::NativeOperation>(operation),
                    GekkoAOT::GX::NativeOrigin::Fifo,true))return false;
        }
        if(g_native_ir_profile)g_native_ir_stats.decoder_units+=plan.commands;
        return true;
      }
    }
    if(source.data()==data && bytes){snapshot.assign(source.begin(),source.end());source={snapshot.data(),snapshot.size()};}
    // Recognition and decoding must consume one immutable version. A failed
    // state-only lowering used to decode live guest bytes a second time while
    // the CPU could already be streaming the next version into the same list.
    const bool result=ProcessSpan(source.data(),source.size(),depth+1u);
    if(!result){
      std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_DL_ERROR address=%08x bytes=%u changed_during_decode=%u prefix=",addr,bytes,
        bytes && std::memcmp(source.data(),data,bytes)!=0);
      for(unsigned i=0;i<std::min<unsigned>(bytes,64);++i)std::fprintf(stderr,"%02x",source[i]);
      std::fprintf(stderr,"\n");
    }
    return result;
  }

  if(g_fifo_native_ir && direct_state && op==GX_LOAD_CP_REG && len==6u) {
    GekkoAOT::GX::NativeOperation operation;
    if(GekkoAOT::GX::LowerNativeCp(p[1],Be32(p+2),operation))
      return SubmitNativeOperation(operation,GekkoAOT::GX::NativeOrigin::Fifo);
  }
  if(g_fifo_native_ir && direct_state && op==GX_LOAD_XF_REG && len>=9u) {
    const auto header=Be32(p+1);
    GekkoAOT::GX::NativeOperation operation;
    if(GekkoAOT::GX::LowerNativeXf(header&0xffffu,(header>>16u)+1u,{p+5,len-5u},operation))
      return SubmitNativeOperation(operation,GekkoAOT::GX::NativeOrigin::Fifo);
  }
  if(op==GX_LOAD_CP_REG && p[1]>=0xA0u && p[1]<=0xAFu) {
    MapArrayBase(p[1],Be32(p+2));return true;
  }

  if(op==GX_LOAD_INDX_A || op==GX_LOAD_INDX_B ||
     op==GX_LOAD_INDX_C || op==GX_LOAD_INDX_D) {
    if(!PrepareIndexedXfArrayWindow(p,len)) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR opcode=0x%02x action=reject-xf-array-window\n",
                   unsigned(cmd));
      return false;
    }
  }

  if(op==kLoadBpOpcode) {
    const auto word=Be32(p+1);
    if(g_fifo_native_ir && direct_state) {
      GekkoAOT::GX::NativePixelState pixel;
      if(GekkoAOT::GX::LowerNativeBp(word>>24,word,pixel))
        return SubmitNativeOperation(pixel,GekkoAOT::GX::NativeOrigin::Fifo);
    }
    return ApplyPixelOperation(word,direct_state,{p,len});
  }

  if(op>=0x80u&&op<=0xB8u) {
    if(len<3)return false;
    const auto count=Be16(p+1);
    return ProcessNativeDraw({GekkoAOT::GX::NativeTopology(op),std::uint8_t(cmd&GX_VAT_MASK),count,
      std::uint16_t(count?(len-3)/count:0),0,std::uint32_t(len)},p);
  }

  if(direct_state && op==GX_LOAD_CP_REG && len==6u) {
    aurora::gx::fifo::handle_cp(p[1],Be32(p+2));
  } else if(direct_state && op==GX_LOAD_XF_REG && len>=9u &&
            len==5u+std::size_t((Be32(p+1)>>16u)+1u)*4u) {
    aurora::gx::fifo::handle_xf(static_cast<std::uint16_t>(Be32(p+1)),
                              std::span<const std::uint8_t>(p+5,len-5u));
  } else {
    (void)ProcessAuroraCompatibility(p,static_cast<std::uint32_t>(len));
  }
  if(op==GX_LOAD_CP_REG) InvalidateVertexSize(p[1]);
  return true;
}

bool ProcessSpan(const std::uint8_t* data, std::size_t bytes, unsigned depth) {
  std::size_t off=0;
  while(off<bytes) {
    std::size_t len=0,need=1;
    const auto status=CommandLength(data+off,bytes-off,&len,&need);
    if(status!=DecodeResult::Complete) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR offset=%zu remaining=%zu opcode=0x%02x status=%s need=%zu\n",
                   off,bytes-off,unsigned(data[off]),
                   status==DecodeResult::Incomplete?"incomplete":"invalid",need);
      return false;
    }
    std::size_t consumed=len;
    if(!ProcessCommandOrRun(data+off,bytes-off,len,depth,&consumed)) return false;
    off+=consumed;
  }
  return true;
}

void CompactFifo() {
  if(g_fifo_read==0u) return;
  if(g_fifo_read>=g_fifo.size()) {
    g_fifo_stream_base += g_fifo.size();
    g_fifo.clear();
    g_fifo_read=0u;
    // A single unusually large display list can make std::vector retain that
    // peak forever. Return pathological FIFO capacity once the queue drains.
    constexpr std::size_t kFifoRetainedCapacity=4u*1024u*1024u;
    if(g_fifo.capacity()>kFifoRetainedCapacity) {
      std::vector<std::uint8_t> compact;
      compact.reserve(1u<<20);
      g_fifo.swap(compact);
    }
    return;
  }
  // Avoid vector::erase/memmove on every command. Compact only after a useful
  // amount of data was consumed or when more than half of the buffer is dead.
  if(g_fifo_read<65536u && g_fifo_read*2u<g_fifo.size()) return;
  const std::size_t remain=g_fifo.size()-g_fifo_read;
  g_fifo_stream_base += g_fifo_read;
  std::memmove(g_fifo.data(),g_fifo.data()+g_fifo_read,remain);
  g_fifo.resize(remain);
  g_fifo_read=0u;
}

bool CanRecoverSingleByteFifoDesync(const std::uint8_t* p, std::size_t available,
                                    std::size_t* validated_bytes) {
  if(!p || available<2u || !validated_bytes) return false;

  // 0x18..0x1f are the reserved hole between LOAD_XF (0x10 family) and the
  // indexed-load commands (0x20 family). Do not turn arbitrary unknown GX
  // opcodes into NOPs. Rising Sun can occasionally expose one stray byte from
  // this reserved family while the following byte is already a complete BP/CP
  // control command. Recover only that one-byte loss of framing and only when
  // the complete currently-buffered suffix validates from the next byte.
  if((p[0]&GX_OPCODE_MASK)!=0x18u) return false;
  const std::uint8_t next_op=p[1]&GX_OPCODE_MASK;
  const bool strong_anchor = next_op==kLoadBpOpcode || next_op==GX_LOAD_CP_REG ||
      next_op==GX_LOAD_XF_REG || next_op==GX_LOAD_INDX_A ||
      next_op==GX_LOAD_INDX_B || next_op==GX_LOAD_INDX_C ||
      next_op==GX_LOAD_INDX_D || next_op==GX_CMD_CALL_DL;
  if(!strong_anchor) return false;

  std::size_t off=1u;
  std::size_t commands=0u;
  while(off<available) {
    std::size_t len=0u,need=1u;
    const auto status=CommandLength(p+off,available-off,&len,&need);
    if(status==DecodeResult::Invalid) return false;
    if(status==DecodeResult::Incomplete) {
      // A command split by the host burst boundary is still a valid suffix;
      // DrainFifo() will wait for its remaining bytes normally.
      *validated_bytes=off;
      return commands!=0u;
    }
    off+=len;
    ++commands;
  }
  *validated_bytes=off;
  return commands!=0u;
}

void DrainFifo() {
  while(g_fifo_read<g_fifo.size()) {
    const std::size_t available=g_fifo.size()-g_fifo_read;
    if(available<g_fifo_need) break;
    std::size_t len=0,need=1;
    const auto status=CommandLength(g_fifo.data()+g_fifo_read,available,&len,&need);
    if(status==DecodeResult::Incomplete) {
      g_fifo_need=need;
      break;
    }
    if(status==DecodeResult::Invalid) {
      std::size_t validated=0u;
      if(CanRecoverSingleByteFifoDesync(g_fifo.data()+g_fifo_read,available,&validated)) {
        const std::uint8_t bad=g_fifo[g_fifo_read];
        const std::uint8_t anchor=g_fifo[g_fifo_read+1u];
        static std::uint64_t recoveries=0u;
        ++recoveries;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_FIFO_RESYNC_V201=1 offset=%zu absolute=%llu "
                     "dropped=0x%02x anchor=0x%02x validated=%zu count=%llu action=skip-one-reserved-byte\n",
                     g_fifo_read,
                     static_cast<unsigned long long>(g_fifo_stream_base+g_fifo_read),
                     unsigned(bad),unsigned(anchor),validated,
                     static_cast<unsigned long long>(recoveries));
        ++g_fifo_read;
        g_fifo_need=1u;
        continue;
      }
      DumpFifoFault(g_fifo_read);
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR offset=%zu opcode=0x%02x action=fail-closed\n",
                   g_fifo_read,unsigned(g_fifo[g_fifo_read]));
      g_fifo_fault=true;
      g_quit=true;
      break;
    }
    RememberDecodedCommand(g_fifo.data()+g_fifo_read,len,g_fifo_stream_base+g_fifo_read);
    std::size_t consumed=len;
    if(!ProcessCommandOrRun(g_fifo.data()+g_fifo_read,g_fifo.size()-g_fifo_read,len,0u,&consumed)) {
      DumpFifoFault(g_fifo_read);
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR offset=%zu opcode=0x%02x action=fail-closed-process\n",
                   g_fifo_read,unsigned(g_fifo[g_fifo_read]));
      g_fifo_fault=true;
      g_quit=true;
      break;
    }
    g_fifo_read+=consumed;
    g_fifo_need=1u;
  }
  CompactFifo();
}

bool RuntimeWindowIsEmbedded() {
  const char* parent = std::getenv("GEKKOAOT_EMBED_WINDOW");
  return parent && *parent;
}

#ifdef _WIN32
// The Qt host owns fullscreen and lifetime. The SDL renderer keeps its HWND
// and Vulkan surface, but becomes a child of the persistent game viewport.
HWND g_embed_parent = nullptr;
int g_embed_width = 0, g_embed_height = 0;
void ResizeWindowsEmbed() {
  if (!g_embed_parent || !g_info.window || !IsWindow(g_embed_parent)) return;
  RECT bounds{};
  if (!GetClientRect(g_embed_parent, &bounds)) return;
  const int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
  if (w <= 0 || h <= 0 || (w == g_embed_width && h == g_embed_height)) return;
  g_embed_width = w; g_embed_height = h;
  (void)SDL_SetWindowSize(g_info.window, w, h);
}
bool EmbedWindows() {
  const char* text = std::getenv("GEKKOAOT_EMBED_WINDOW");
  if (!text || !*text) return true;
  char* end = nullptr;
  const auto value = std::strtoull(text, &end, 0);
  const HWND parent = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
  if (end == text || *end || !IsWindow(parent) || !g_info.window) return false;
  const HWND child = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(g_info.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (!child || child == parent) return false;
  const auto style = GetWindowLongPtrW(child, GWL_STYLE);
  SetWindowLongPtrW(child, GWL_STYLE, (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_BORDER)) | WS_CHILD | WS_CLIPSIBLINGS);
  SetLastError(0);
  if (!SetParent(child, parent) && GetLastError() != 0) return false;
  g_embed_parent = parent; g_embed_width = 0; g_embed_height = 0;
  ResizeWindowsEmbed();
  SetWindowPos(child, nullptr, 0, 0, g_embed_width, g_embed_height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_SINGLE_WINDOW=1 backend=win32 parent=%llu\n", value);
  return true;
}
#endif
void ApplyWindowIdentity() {
  if (!g_info.window) return;
  if (const char* path = std::getenv("GEKKOAOT_WINDOW_ICON"); path && *path) {
    if (SDL_Surface* icon = SDL_LoadBMP(path)) {
      (void)SDL_SetWindowIcon(g_info.window, icon); SDL_DestroySurface(icon);
    }
  }
}

void PollFullscreenShortcut() {
  // The Qt frontend has its own Alt+Enter shortcut, but match-GDB/standalone
  // mode gives keyboard focus to Aurora's SDL window. Handle the shortcut in
  // the NativeGX DSO as well. Embedded X11 windows deliberately leave
  // fullscreen ownership to the Qt parent.
  if (!g_info.window || RuntimeWindowIsEmbedded()) {
    g_alt_enter_latched = false;
    return;
  }
  int key_count = 0;
  const bool* keys = SDL_GetKeyboardState(&key_count);
  const SDL_Keymod mods = SDL_GetModState();
  const bool return_down = keys &&
      ((SDL_SCANCODE_RETURN < key_count && keys[SDL_SCANCODE_RETURN]) ||
       (SDL_SCANCODE_KP_ENTER < key_count && keys[SDL_SCANCODE_KP_ENTER]));
  const bool chord = return_down && (mods & SDL_KMOD_ALT) != 0;
  if (chord && !g_alt_enter_latched) {
    const bool was_fullscreen =
        (SDL_GetWindowFlags(g_info.window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (SDL_SetWindowFullscreen(g_info.window, !was_fullscreen)) {
      // SDL3 window-state changes can be asynchronous (notably Wayland/X11).
      // Synchronize this explicit user action so the very next present sees
      // the new framebuffer size, then refresh the live projection policy.
      (void)SDL_SyncWindow(g_info.window);
      ApplyAspectMode(g_aspect_mode.load(std::memory_order_acquire), false);
      std::fprintf(stderr,
                   "GEKKOAOT_FULLSCREEN_V137=1 source=alt-enter state=%s mode=standalone\n",
                   was_fullscreen ? "windowed" : "fullscreen");
    } else {
      std::fprintf(stderr,
                   "GEKKOAOT_FULLSCREEN_V137=0 source=alt-enter error=%s\n",
                   SDL_GetError());
    }
  }
  g_alt_enter_latched = chord;
}

void PollEvents() {
#ifdef _WIN32
  ResizeWindowsEmbed();
#endif
  const AuroraEvent* ev=aurora_update();
  while(ev && ev->type!=AURORA_NONE) {
    if(ev->type==AURORA_EXIT) g_quit=true;
    if (ev->type == AURORA_WINDOW_RESIZED || ev->type == AURORA_UNPAUSED ||
        ev->type == AURORA_DISPLAY_SCALE_CHANGED ||
        (ev->type == AURORA_SDL_EVENT && ev->sdl.type == SDL_EVENT_WINDOW_EXPOSED))
      aurora::gfx::render_worker::enqueue_work([] { g_cached_scanout.Reset(); });
    ++ev;
  }
  PollFullscreenShortcut();
  if (g_info.window) RefreshHostWindowAspect();
}

#ifdef GEKKOAOT_HAVE_X11
bool EmbedX11() {
  const char* parent_text=std::getenv("GEKKOAOT_EMBED_WINDOW");
  if(!parent_text||!*parent_text) return false;
  if(!g_info.window) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=0 reason=no-sdl-window\n");
    return false;
  }
  char* end=nullptr; const unsigned long parent=std::strtoul(parent_text,&end,0);
  if(end==parent_text||*end!='\0'||!parent) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=0 reason=bad-parent-xid\n");
    return false;
  }
  SDL_PropertiesID props=SDL_GetWindowProperties(g_info.window);
  auto* display=static_cast<Display*>(SDL_GetPointerProperty(props,SDL_PROP_WINDOW_X11_DISPLAY_POINTER,nullptr));
  const Window child=static_cast<Window>(SDL_GetNumberProperty(props,SDL_PROP_WINDOW_X11_WINDOW_NUMBER,0));
  if(!display||!child) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=0 reason=sdl-not-x11\n");
    return false;
  }
  if(child==static_cast<Window>(parent)) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=1 platform=x11 mode=sdl-external xid=%lu\n", parent);
    return true;
  }

  // Fallback for an already-built Aurora cache that has not yet picked up the
  // SDL external-window adapter. Keep it deterministic and resize to GameView.
  XWindowAttributes attrs{};
  if(!XGetWindowAttributes(display,static_cast<Window>(parent),&attrs)) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=0 reason=parent-query-failed\n");
    return false;
  }
  XSetWindowBorderWidth(display,child,0);
  XReparentWindow(display,child,static_cast<Window>(parent),0,0);
  XMoveResizeWindow(display,child,0,0,
                    static_cast<unsigned>(std::max(attrs.width,1)),
                    static_cast<unsigned>(std::max(attrs.height,1)));
  XMapWindow(display,child);
  XSync(display,False);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=1 platform=x11 mode=reparent-fallback xid=%lu\n", parent);
  return true;
}

bool PrepareX11Embed() {
  const char* parent = std::getenv("GEKKOAOT_EMBED_WINDOW");
  if (!parent || !*parent) return true;

  const char* window_system = std::getenv("GEKKOAOT_WINDOW_SYSTEM");
  if (window_system && *window_system && !EqualNoCase(window_system, "x11")) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_X11_PREP_V001=0 reason=window-system-%s\n",
                 window_system);
    return false;
  }

  if (::setenv("GEKKOAOT_NATIVE_GX_X11_WINDOW", parent, 1) != 0) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_X11_PREP_V001=0 reason=setenv-xid-failed\n");
    return false;
  }

  // SDL input has initialized EVENTS/GAMEPAD, not SDL_VIDEO. Force X11 before
  // Aurora creates its presentation window so SDL wraps the Qt/XCB GameView
  // instead of opening a separate Wayland top-level.
  if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
    ::setenv("SDL_VIDEODRIVER", "x11", 1);
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "x11", SDL_HINT_OVERRIDE);
  } else {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || !EqualNoCase(driver, "x11")) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_X11_PREP_V001=0 reason=video-already-%s\n",
                   driver ? driver : "unknown");
      return false;
    }
  }

  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_X11_PREP_V001=1 driver=x11 xid=%s\n", parent);
  return true;
}
#endif
}

GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_capture_sink_v1(
    const GekkoAOT::Capture::Sink* sink, std::uint32_t bytes) {
  if (!GekkoAOT::Capture::Compatible(sink, bytes)) {
    std::fprintf(stderr,"GEKKOAOT_CAPTURE_BIND_V1=0 reason=telemetry-abi-mismatch\n");
    return;
  }
  GekkoAOT::Capture::SetSink(*sink);
  if(sink->record) std::fprintf(stderr,"GEKKOAOT_CAPTURE_BIND_V1=1\n");
}

GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_init(HostResolveFn resolve, void* user) {
  if(g_ready) return true;
  // A previous renderer set quit at shutdown. A new device owns a fresh
  // producer lifetime; retaining that latch silently discards every command.
  g_quit=false;
  g_frame_open=false;
  g_alt_enter_latched=false;
  g_resolve=resolve; g_resolve_user=user; g_fifo.reserve(1u<<20);
  g_fifo_read=0; g_fifo_need=1; g_fifo_fault=false; g_fifo_fault_reported=false;
  g_fifo_forensic_trace=EnvBool("GEKKOAOT_NATIVE_GX_FIFO_FORENSIC",false);
  g_native_commands.Reset();
  g_native_ir_stats = {};
  g_native_display_lists.Reset();
  g_native_draw_lists.Reset();g_native_draw_list_executed=0;
  if(const char* entries=std::getenv("GEKKOAOT_NATIVE_GX_LIST_CACHE"); entries && *entries) {
    const auto capacity=std::strtoul(entries,nullptr,10);
    g_native_display_lists.SetCapacity(capacity);g_native_draw_lists.SetCapacity(capacity);
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_LIST_CACHE entries=%lu\n",capacity);
  }
  g_scene_evidence.Reset();
  aurora::gfx::enhanced::reset();
  g_native_ir_profile = EnvBool("GEKKOAOT_NATIVE_GX_IR_PROFILE", false);
  g_phase_profile = EnvBool("GEKKOAOT_GX_PHASE_PROFILE", false);
  g_fifo_native_ir = EnvBool("GEKKOAOT_NATIVE_GX_FIFO_IR", true) && !g_fifo_forensic_trace &&
      !std::getenv("GEKKOAOT_GX_DIAG_FIFO_CAPTURE");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_STATE_IR=1 enabled=%u default=on display_lists=bounded-exact-bytes\n",g_fifo_native_ir?1u:0u);
  g_native_commands.SetDirectSpansEnabled(EnvBool("GEKKOAOT_NATIVE_GX_DIRECT_COMMAND_SPANS",true));
  g_typed_arrays=EnvBool("GEKKOAOT_NATIVE_GX_TYPED_ARRAYS",false) && !g_fifo_forensic_trace &&
      !std::getenv("GEKKOAOT_GX_DIAG_FIFO_CAPTURE");
  g_direct_matrices=EnvBool("GEKKOAOT_NATIVE_GX_DIRECT_MATRICES",true) && !g_fifo_forensic_trace &&
      !std::getenv("GEKKOAOT_GX_DIAG_FIFO_CAPTURE");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_DIRECT_MATRICES_V317=1 enabled=%u renderer=matrix-state\n",g_direct_matrices?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_TYPED_COMMANDS_V313=1 enabled=%u arrays=host-parameters fallback=exact-stream\n",g_typed_arrays?1u:0u);
  g_fifo_streaming=EnvBool("GEKKOAOT_NATIVE_GX_STREAM_FIFO",true);
  g_fifo_stream_stats=EnvBool("GEKKOAOT_NATIVE_GX_STREAM_FIFO_STATS",false);
  g_fifo_submitted_bytes=0; g_fifo_copied_bytes=0; g_fifo_direct_bytes=0;
  g_fifo_write_trace.fill({}); g_fifo_write_trace_head=0; g_fifo_write_trace_count=0;
  g_fifo_last_cmd=0; g_fifo_last_cmd_len=0; g_fifo_last_draw_fmt=0xffu;
  g_fifo_last_draw_count=0; g_fifo_last_draw_stride=0;
  g_vertex_size_cache.fill(0); g_vertex_size_valid=0;
  g_index_scan_layout.fill({}); g_index_scan_valid=0;
  g_texture_image3_word.fill(0); g_texture_image3_valid=0; g_texture_image3_written=0;
  g_texture_write_footprints.fill({});
  g_texture_write_span_hits=g_texture_write_span_misses=0;
  g_guest_memory_write_notifications=g_guest_memory_write_noops=0;
  g_texture_ui_source_hash.fill(0); g_texture_ui_source_hash_valid=0;
  g_tlut_memory.Reset(); g_tlut_revisions.fill(0); g_tlut_bindings.fill(0);
  g_array_available.fill(0);
  g_array_guest_base.fill(0);
  g_cached_scanout.Reset();
  for(auto& slot : g_persistent_array_slots) slot = {};
  for(auto& policy : g_array_upload_policy) policy = {};
  g_adaptive_array_upload=EnvBool("GEKKOAOT_NATIVE_GX_ADAPTIVE_ARRAY_UPLOAD",true);
  g_content_array_cache=EnvBool("GEKKOAOT_NATIVE_GX_CONTENT_ARRAY_CACHE",true);
  g_content_array_hits=0;
  g_persistent_array_cursor=static_cast<std::uint32_t>(aurora::gfx::StorageBufferSize);
  g_persistent_array_frame=UINT32_MAX;
  g_persistent_array_reset_pending=false;
  g_persistent_array_cache=EnvBool("GEKKOAOT_NATIVE_GX_PERSISTENT_ARRAY_CACHE",true);
  g_persistent_array_min_bytes=EnvUnsigned("GEKKOAOT_NATIVE_GX_PERSISTENT_ARRAY_MIN_BYTES",8u*1024u);
  g_persistent_array_uploads=0;
  g_persistent_array_upload_bytes=0;
  g_persistent_array_hits=0;
  g_persistent_array_fallbacks=0;
  g_adaptive_array_fallbacks=0;
  g_compact_array_windows=EnvBool("GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS",true);
  g_wide_solid_spans=EnvBool("GEKKOAOT_WIDESCREEN_SOLID_SPANS",true);
  g_ui_insets=EnvBool("GEKKOAOT_WIDESCREEN_UI_INSETS",true);
  g_copy_provenance_enabled=EnvBool("GEKKOAOT_WIDESCREEN_COPY_PROVENANCE",true);
  g_wide_perspective_fills=EnvBool("GEKKOAOT_WIDESCREEN_PERSPECTIVE_FILLS",true);
  g_wide_perspective_ui=EnvBool("GEKKOAOT_WIDESCREEN_PERSPECTIVE_UI",true);
  g_wide_prerendered_43=EnvBool("GEKKOAOT_WIDESCREEN_PRERENDERED_43",true);
  g_interp_probe_enabled=EnvBool("GEKKOAOT_INTERP_PROBE",false);
  g_copy_provenance.Reset();
  g_display_insets.Reset();
  g_flat_frames.Reset(); g_frame_depth_draws=0; g_draw_perspective_ui=false;
  g_frame_prerendered=false; g_prerendered_idle=0; g_frame_force_43.store(false,std::memory_order_relaxed);
  g_wide_span_trace=EnvBool("GEKKOAOT_WIDESCREEN_SPAN_TRACE",false);
  g_native_index_range_cache=EnvBool("GEKKOAOT_NATIVE_GX_INDEX_RANGE_CACHE",true);
  g_native_index_ranges.Clear();
  std::fprintf(stderr,"GEKKOAOT_NATIVE_INDEX_RANGES_V307=%u payload_limit=512 exact=layout+draw fallback=rescan\n",
               g_native_index_range_cache?1u:0u);
  g_native_array_tiles=EnvBool("GEKKOAOT_NATIVE_GX_ARRAY_TILES",true) &&
                       aurora::gfx::native_snapshot_atlas_enabled();
  g_native_array_snapshots=EnvBool("GEKKOAOT_NATIVE_GX_ARRAY_SNAPSHOTS",true) &&
                           aurora::gfx::native_snapshot_atlas_enabled();
  g_native_attribute_plan=EnvBool("GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN",true);
  g_native_vertex_program=EnvBool("GEKKOAOT_NATIVE_GX_VERTEX_PROGRAM",true);
  g_native_index_bounds=EnvBool("GEKKOAOT_NATIVE_GX_INDEX_BOUNDS",true);
  g_native_vertex_gather=EnvBool("GEKKOAOT_NATIVE_GX_VERTEX_GATHER",false);
  g_native_gather_draws=g_native_gather_bytes=g_native_gather_windows=0;
  std::fprintf(stderr,"GEKKOAOT_NATIVE_VERTEX_GATHER_V318=%u indexed=16 fields=1..4 vertices=1..64 resources=host-packed fallback=original\n",g_native_vertex_gather?1u:0u);
  g_native_vertex_profile=EnvBool("GEKKOAOT_RUNTIME_PROFILE",false);
  g_native_vertex_draws=0;g_native_vertex_vertices=0;
  std::fprintf(stderr,"GEKKOAOT_NATIVE_VERTEX_V315=%u layout=compiled-index16 fields=2..4 equality=exact fallback=original env=GEKKOAOT_NATIVE_GX_VERTEX_PROGRAM\n",g_native_vertex_program?1u:0u);
  const auto* snapshot_recycling=std::getenv("GEKKOAOT_NATIVE_GX_SNAPSHOT_RECYCLING");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SNAPSHOT_RECYCLING_V312=%u scope=cpu-shadow gpu-ranges=immutable env=GEKKOAOT_NATIVE_GX_SNAPSHOT_RECYCLING\n",
               snapshot_recycling && snapshot_recycling[0]=='1' ? 1u : 0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN_V310=%u source=validated-vcd-vat cache=layout-only resources=live env=GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN\n",
               g_native_attribute_plan?1u:0u);
  g_native_array_snapshot_stages=0;
  g_compact_array_uploads=0;
  g_compact_array_bytes=0;
  g_compact_array_prefix_bytes=0;
  g_compact_array_rollovers=0;
  g_compact_array_fallbacks=0;
  g_array_cache_control_events=0;
  g_array_cache_invalidations=0;
  g_array_cache_control_logs=0;
  g_copy_dest_physical=0; g_copy_stride_bytes=0; g_copy_yscale_raw=0x100u; g_copy_src_valid=false; g_copy_dest_valid=false;
  g_frame_ready_xfb.store(0, std::memory_order_relaxed);
  g_texture_viewport_roles.Reset();
  g_screen_fill_draw=false;
  g_pending_efb_pokes={};
  g_efb_poke_batch=EnvBool("GEKKOAOT_NATIVE_GX_EFB_POKE_BATCH",true);
  g_efb_poke_stats=EnvBool("GEKKOAOT_NATIVE_GX_EFB_POKE_STATS",false);
  g_efb_poke_first_flush_logged=false;
  g_efb_poke_writes=0;
  g_efb_poke_draws=0;
  g_alt_enter_latched=false;
  g_interpolation_requested=EnvBool("GEKKOAOT_FRAME_INTERPOLATION",false);
  g_frame_interpolation_enabled=g_interpolation_requested;
  {
    // Frame interpolation re-renders geometry; the image blends stay
    // available as GEKKOAOT_INTERPOLATION_MODE=motion|blend|repeat.
    const char* mode=std::getenv("GEKKOAOT_INTERPOLATION_MODE");
    g_replay_mode=!mode || !*mode || EqualNoCase(mode,"replay");
  }
  g_video=GekkoAOT::GX::GraphicsSettingsFromEnvironment();
  GekkoAOT::GX::Hud::Configure(g_video.hud!=0,g_video.hud_items);
  g_replay_interpolate=g_video.unlock_predict==0;
  g_render_percent.store(g_video.render_percent);
  g_fov_y.store(g_video.fov_y);
  g_render_width.store(g_video.render_width); g_render_height.store(g_video.render_height);
  g_interp_previous={}; g_interp_current={};
  g_interp_previous_valid=false; g_interp_current_valid=false; g_interp_snapshot_count=0;
  g_texture_bind_cache=EnvBool("GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE",true);
  g_texture_version_reuse=EnvBool("GEKKOAOT_NATIVE_GX_TEXTURE_VERSION_REUSE",true);
  g_texture_source_ids=EnvBool("GEKKOAOT_NATIVE_GX_TEXTURE_SOURCE_IDS",true);
  g_copy_range_forget=EnvBool("GEKKOAOT_NATIVE_GX_COPY_RANGE_FORGET",true);
  ForgetTextureSourceVersions();
  g_array_base_trace=EnvBool("GEKKOAOT_NATIVE_GX_ARRAY_BASE_TRACE",false);
  g_z16r_copy_trace=EnvBool("GEKKOAOT_NATIVE_GX_Z16R_TRACE",false);
  g_ztexture_trace=EnvBool("GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE",false);
  g_partial_clear_copy_continuation=
      EnvBool("GEKKOAOT_NATIVE_GX_PARTIAL_CLEAR_CONTINUATION",false);
  g_masked_clear_efb_continuity=
      EnvBool("GEKKOAOT_NATIVE_GX_MASKED_CLEAR_CONTINUITY",true);
  g_efb_boundary_continuity=
      EnvBool("GEKKOAOT_NATIVE_GX_EFB_CONTINUITY",false);
  g_draw_census=EnvBool("GEKKOAOT_RUNTIME_PROFILE",false);
  g_draw_runs=EnvBool("GEKKOAOT_NATIVE_GX_DRAW_RUNS",true);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_DRAW_RUNS_V323=%u list_member_vertices=%u expand_member_vertices=%u "
               "max_run_vertices=%u key=vat mixed=triangles order=aurora-exact\n",
               g_draw_runs?1u:0u,unsigned(kDrawRunListMax),unsigned(kDrawRunExpandMax),
               unsigned(kDrawRunMaxVertices));
  g_display_copy_state_logs=0;
  g_partial_clear_copy_logs=0;
  g_trace_gx_copy=EnvBool("GEKKOAOT_TRACE_GX_COPY",false);
  g_trace_video=EnvBool("GEKKOAOT_TRACE_VIDEO",false);
  g_invalidate_texture_sources=EnvBool("GEKKOAOT_GX_INVALIDATE_TEXTURE_SOURCES",true);
  g_texture_source_invalidation_pending=false;
  g_trace_font=EnvBool("GEKKOAOT_TRACE_FONT",false);
  g_paranoid_texture_hash=EnvBool("GEKKOAOT_GX_PARANOID_TEXTURE_HASH",false);
  g_ztexture_logs=0;
  g_z16r_copy_dest=0; g_z16r_copy_span=0; g_z16r_bound_maps=0;
  g_z16r_bp_logs=0; g_z16r_copy_logs=0; g_z16r_bind_logs=0; g_z16r_draw_logs=0;
  g_array_base_writes=0;
  g_array_base_same_binding=0;
  g_array_base_same_cached=0;
  g_array_base_same_cached_bytes=0;
  g_array_base_trace_draws=0;
  g_array_base_trace_uploads=0;
  g_array_base_trace_upload_bytes=0;
  g_array_base_trace_invalidate=0;
  g_array_base_trace_window_growth=0;
  g_array_base_trace_window_cached=0;
  g_array_base_trace_window_bytes=0;
  g_array_base_trace_eager_growth=0;
  // v114 proved the narrow per-draw indexed-array refresh does not repair
  // MKDD's black 3D scene, while it greatly increases staging traffic. Keep both
  // expensive coherency policies opt-in. The active correctness fix in v115 is
  // Aurora Z-texture (BP F4/F5 + Z8/Z16/Z24X8 sampling + fragment depth).
  g_array_frame_refresh=EnvBool("GEKKOAOT_NATIVE_GX_ARRAY_FRAME_REFRESH",false);
  g_indexed_draw_coherency=EnvBool("GEKKOAOT_NATIVE_GX_INDEXED_DRAW_COHERENCY",false);
  g_post_processing_enabled=EnvBool("GEKKOAOT_POST_PROCESSING",true);
  g_post_preset=std::getenv("GEKKOAOT_POSTFX_PRESET") ? std::getenv("GEKKOAOT_POSTFX_PRESET") : "balanced";
  if(EqualNoCase(g_post_preset.c_str(),"cinematic")) g_post_cinematic=0.20f;
  else g_post_cinematic=0.0f;
  g_post_sharpen=EnvFloat("GEKKOAOT_POSTFX_SHARPEN",0.28f,0.0f,1.0f);
  g_post_bloom=EnvFloat("GEKKOAOT_POSTFX_BLOOM",0.10f,0.0f,1.0f);
  g_post_vignette=EnvFloat("GEKKOAOT_POSTFX_VIGNETTE",0.06f,0.0f,1.0f);
  g_post_grain=EnvFloat("GEKKOAOT_POSTFX_GRAIN",0.0f,0.0f,0.25f);
  g_post_tone=EnvFloat("GEKKOAOT_POSTFX_TONE",0.15f,0.0f,1.0f);
  g_post_local_contrast=EnvFloat("GEKKOAOT_POSTFX_LOCAL_CONTRAST",0.08f,0.0f,1.0f);
  g_post_frame=0;
  AuroraConfig config{};
  config.appName="GekkoAOT " GEKKOAOT_VERSION;
  config.desiredBackend=BackendFromEnv();
#if defined(__linux__)
  // Mesa may otherwise infer Wayland from WAYLAND_DISPLAY even while SDL/Qt
  // supplies an Xlib surface. An XID then reaches the Wayland EGL path as a
  // wl_surface pointer and crashes inside eglCreateWindowSurface.
  if (config.desiredBackend == BACKEND_OPENGL || config.desiredBackend == BACKEND_OPENGLES) {
    const char* driver = std::getenv("SDL_VIDEODRIVER");
    if (driver && EqualNoCase(driver, "x11")) ::setenv("EGL_PLATFORM", "x11", 1);
    else if (driver && EqualNoCase(driver, "wayland")) ::setenv("EGL_PLATFORM", "wayland", 1);
  }
#endif
  config.vsync=EnvBool("GEKKOAOT_NATIVE_GX_VSYNC",false);
  g_vsync_default=g_vsync_applied=config.vsync;
  g_video.vsync=config.vsync ? 1u : 0u;
  config.startFullscreen=EnvBool("GEKKOAOT_NATIVE_GX_FULLSCREEN",false);
  // Aurora reports framebuffer pixels after DPI scaling. A 1280x720 logical
  // default becomes 1920x1080 at Windows 150% scaling, which is unnecessarily
  // large for the standalone window. Keep env overrides, but start compact.
  config.windowWidth=EnvUnsigned("GEKKOAOT_NATIVE_GX_WIDTH",960);
  config.windowHeight=EnvUnsigned("GEKKOAOT_NATIVE_GX_HEIGHT",540);
  config.pauseOnFocusLost=false;
  config.allowJoystickBackgroundEvents=true;
  config.allowCpuAdapter=EnvBool("GEKKOAOT_NATIVE_GX_ALLOW_CPU",false);
  const unsigned msaa=EnvUnsigned("GEKKOAOT_NATIVE_GX_MSAA",1);
  const unsigned anisotropy=EnvUnsigned("GEKKOAOT_NATIVE_GX_ANISOTROPY",16);
  config.msaa=(msaa==4)?4:1;
  config.maxTextureAnisotropy=(anisotropy==1 || anisotropy==2 || anisotropy==4 ||
                               anisotropy==8 || anisotropy==16)?anisotropy:16;
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_QUALITY msaa=%u anisotropy=%u interpolation=%u\n",
               config.msaa, config.maxTextureAnisotropy,
               g_frame_interpolation_enabled ? 1u : 0u);
  std::fprintf(stderr,"GEKKOAOT_POST_PROCESSING_V196=%u preset=%s sharpen=%.3f bloom=%.3f vignette=%.3f grain=%.3f tone=%.3f local=%.3f cinematic=%.3f stage=final-xfb\n",
               g_post_processing_enabled?1u:0u,g_post_preset.c_str(),g_post_sharpen,g_post_bloom,
               g_post_vignette,g_post_grain,g_post_tone,g_post_local_contrast,g_post_cinematic);
  std::fprintf(stderr,"GEKKOAOT_AURORA_HOT_ENV_CACHE_V36=1 trace_copy=%u trace_video=%u trace_font=%u paranoid_hash=%u\n",
               g_trace_gx_copy?1u:0u,g_trace_video?1u:0u,g_trace_font?1u:0u,
               g_paranoid_texture_hash?1u:0u);
  config.mem1Size=0; config.mem2Size=0;
#ifdef GEKKOAOT_HAVE_X11
  if (!PrepareX11Embed()) {
    std::fprintf(stderr,"GEKKOAOT_AURORA_GX_V001=0 reason=x11-prep-failed\n");
    return false;
  }
#endif
  int argc=1; char arg0[]="gekkoaot"; char* argv[]={arg0,nullptr};
  g_info=aurora_initialize(argc,argv,&config);
  ApplyWindowIdentity();
#ifdef _WIN32
  if (!EmbedWindows()) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW=0 reason=win32-embed-failed\n");
    aurora_shutdown(); return false;
  }
#endif
  std::fprintf(stderr,"GEKKOAOT_GRAPHICS_API requested=%u actual=%u match=%u\n",
               unsigned(config.desiredBackend),unsigned(g_info.backend),
               config.desiredBackend==BACKEND_AUTO || config.desiredBackend==g_info.backend);
  if (config.desiredBackend != BACKEND_AUTO && config.desiredBackend != g_info.backend) {
    std::fprintf(stderr,"GEKKOAOT_GRAPHICS_API_ERROR requested API unavailable; refusing silent fallback\n");
    aurora_shutdown(); return false;
  }
  for(auto& slot : g_xfb_slots) slot = {};
  g_xfb_generation=0;
  aurora::gfx::enhanced::configure(g_video);
  aurora::webgpu::g_graphicsConfig.enhancedCapture=g_video.enhanced!=0;
  if(g_video.enhanced&&!aurora::webgpu::enable_normal_buffer()) {
    std::fprintf(stderr,"GEKKOAOT_ENHANCED_INIT_ERROR normal attachment creation failed\n");return false;
  }
  aurora::gfx::enhanced::compile_startup_shaders();
  aurora::gfx::compile_startup_shaders();
  g_ram_xfb_texture.reset();
  g_ram_xfb_present={};
  g_ram_xfb_rgba.clear();
  g_ram_xfb_width=0u; g_ram_xfb_height=0u; g_ram_xfb_generation=0u;
  for (auto& candidate : g_ram_xfb_candidates) candidate = {};
  g_ram_xfb_candidate_epoch=0u;
  const char* aspect=std::getenv("GEKKOAOT_ASPECT_MODE");
  ApplyAspectMode(AspectModeFromText(aspect), true);
  g_ready=true;
#ifdef GEKKOAOT_HAVE_X11
  const char* embed = std::getenv("GEKKOAOT_EMBED_WINDOW");
  if(embed && *embed && !EmbedX11()) {
    std::fprintf(stderr,"GEKKOAOT_AURORA_GX_V001=0 reason=single-window-embed-failed\n");
    aurora_shutdown();
    g_ready=false;
    return false;
  }
#endif
  // Aurora already implements hash-based PNG/DDS replacements and streaming.
  // Register once, before rendering; the source hashes and GPU cache remain
  // entirely Aurora-owned, with no guest texture writes or polling.
  if (const char* list = std::getenv("GEKKOAOT_TEXTURE_PACK_LIST"); list && *list) {
    // The GUI writes a plain UTF-8 list. An existing empty manifest means
    // "disable all packs", including the conventional textures/ folder.
    std::ifstream manifest(std::filesystem::u8path(list));
    if (!manifest) {
      std::fprintf(stderr, "GEKKOAOT_TEXTURE_PACK_LIST_ERROR path=\"%s\"\n", list);
    } else {
      std::string line;
      std::size_t packs = 0;
      while (packs < 64 && std::getline(manifest, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        if (line.size() > 4096) {
          std::fprintf(stderr, "GEKKOAOT_TEXTURE_PACK_LIST_SKIPPED reason=long-path\n");
          continue;
        }
        std::filesystem::path root = std::filesystem::u8path(line);
        if (root.is_relative()) root = std::filesystem::u8path(list).parent_path() / root;
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
          std::fprintf(stderr, "GEKKOAOT_TEXTURE_PACK_LIST_SKIPPED missing-directory=\"%s\"\n",
                       root.string().c_str());
          continue;
        }
        auto group = aurora::texture::load_replacement_directory(
            root, aurora::texture::ReplacementOptions{.priority = static_cast<std::int32_t>(packs)});
        const auto count = group.registrations.size();
        for (const auto& registration : group.registrations)
          g_texture_pack.registrations.push_back(registration);
        std::fprintf(stderr, "GEKKOAOT_TEXTURE_PACK=1 root=\"%s\" registrations=%zu\n",
                     root.string().c_str(), count);
        ++packs;
      }
    }
  } else if (const char* pack = std::getenv("GEKKOAOT_TEXTURE_PACK_DIR"); pack && *pack) {
    const std::filesystem::path root{pack};
    std::error_code ec;
    if (std::filesystem::is_directory(root, ec)) {
      g_texture_pack = aurora::texture::load_replacement_directory(root);
      std::fprintf(stderr, "GEKKOAOT_TEXTURE_PACK=1 root=\"%s\" registrations=%zu\n",
                   pack, g_texture_pack.registrations.size());
    } else {
      std::fprintf(stderr, "GEKKOAOT_TEXTURE_PACK=0 invalid-directory=\"%s\"\n", pack);
    }
  }
  PollEvents();
  if (!g_quit && aurora_begin_frame()) g_frame_open=true;
  std::fprintf(stderr,"GEKKOAOT_AURORA_GX_V001=1 backend=direct-retail-fifo vi=native xfb=vi-selected-aurora-copy-cache-v12+ram-yuvy-v57\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_RAM_XFB_V57=1 mode=vi-geometry-yuvy-fallback fastpath=aurora-zero-copy\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_RAM_XFB_V82=1 detector=per-xfb-history slots=16 arm=3-observations+2-changes fallback=cache-miss-only\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_XFB_PRESENT_BOUNDARY_V110=1 producer=gx-copy submit=gx-copy scanout=present-only ram-upload=direct-queue live-efb-cut=forbidden\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_XFB_PARTIAL_CLEAR_V176_LEGACY=%u default=off policy=keep-recording-open env=GEKKOAOT_NATIVE_GX_PARTIAL_CLEAR_CONTINUATION\n",
               g_partial_clear_copy_continuation?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_XFB_MASKED_CLEAR_CONTINUITY_V1=%u default=on policy=gxcopydisp-boundary+selective-color-depth-load env=GEKKOAOT_NATIVE_GX_MASKED_CLEAR_CONTINUITY\n",
               g_masked_clear_efb_continuity?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_XFB_EFB_CONTINUITY_V177_SAFE=%u default=off policy=diagnostic-all-attachment-load env=GEKKOAOT_NATIVE_GX_EFB_CONTINUITY regression=hpcos-ghse69\n",
               g_efb_boundary_continuity?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_PERF_RECOVERY_V115=1 array-frame-refresh-default=off indexed-draw-coherency-default=off ztexture=aurora-v1 xfb-boundary=v110\n");
  std::fprintf(stderr,
               "GEKKOAOT_AURORA_ARRAY_CACHE_V35=%u region_mib=64 base_mib=%llu min_bytes=%u small=frame-staging coherency=guest-write+dcache-publication fallback=transient-storage\n",
               g_persistent_array_cache?1u:0u,
               static_cast<unsigned long long>(aurora::gfx::StorageBufferSize/(1024ull*1024ull)),
               g_persistent_array_min_bytes);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_ADAPTIVE_ARRAY_UPLOAD_V190=%u recent_frames=4 retry_frames=64 dynamic=mapped-frame-staging stable=persistent env=GEKKOAOT_NATIVE_GX_ADAPTIVE_ARRAY_UPLOAD\n",
               g_adaptive_array_upload?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_CONTENT_ARRAY_CACHE_V191=%u equality=exact-bytes snapshot=immutable same-frame-ranges=distinct env=GEKKOAOT_NATIVE_GX_CONTENT_ARRAY_CACHE\n",
               g_content_array_cache?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS_V293=%u indices=raw-u16 source=used-suffix shader-bias=u32 one-draw=1 preflight=arrays+fog env=GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS\n",
               g_compact_array_windows?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_RESOURCES_V303 atlas=%u tiles=%u small_arrays=%u equality=cacheable-cpu-shadow raw_indices=preserved fallback=original\n",
               aurora::gfx::native_snapshot_atlas_enabled()?1u:0u,
               g_native_array_tiles?1u:0u,g_native_array_snapshots?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_INDEXED_ARRAY_COHERENCY_V114=1 policy=exact-used-windows+no-cross-draw-merge default=off env=GEKKOAOT_NATIVE_GX_INDEXED_DRAW_COHERENCY\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_ZTEXTURE_V115=1 backend=aurora-bp-f4-f5 trace=%u env=GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE\n",g_ztexture_trace?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_INDEX_SCAN_V2=1 layout=cached-fields nbt3=xyz-element\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_XF_ARRAY_WINDOW_V86=1 arrays=pos+normal+texture+light sizing=per-indexed-load bounds=guest-mapping\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_COHERENCY_V87=1 texture-invalidate=bp66 stale-array=fail-closed stale-texture=fail-closed dl-depth=1\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_TEXTURE_ID_V10=1 identity=stable-per-slot version=per-load\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FONT_COHERENCY_V174=1 formats=i4+i8+ia4+ia8+c4+c8+c14x2 policy=content-hash-on-stable-rebind\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_ARRAY_FRAME_REFRESH_V3=%u policy=%s\n",
               g_array_frame_refresh?1u:0u,
               g_array_frame_refresh?"gx-copy+host-frame-boundary":"gx-invalidate-driven");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_INDEXED_DRAW_COHERENCY_V109=%u policy=%s\n",
               g_indexed_draw_coherency?1u:0u,
               g_indexed_draw_coherency?"refresh-used-arrays+forbid-cross-draw-merge":"legacy-cache-reuse");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_DYNAMIC_VERTEX_V141=1 source=dcbst+dcbf+dcbi line=32 target=indexed-arrays policy=invalidate-overlap-no-per-draw-hash\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_TEXTURE_COHERENCY_V159=1 mutation=cache-control+host-dma bp66=tmem-only draw-scans=dirty-driven\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_EFB_COPY_V2=1 mode=retail-bp-state\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_EFB_COPY_FENCE_V78=1 ordering=render-worker-before+after trace=64\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FIFO_CAP_V11=1 bytes=%u policy=fail-closed\n",
               16u*1024u*1024u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FIFO_BURST_V13=1 abi=optional max=4096\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FIFO_FASTPATH_V159=1 forensic=%u env=GEKKOAOT_NATIVE_GX_FIFO_FORENSIC texture-scans=dirty-only\n",g_fifo_forensic_trace?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_PRESENT_V137=1 interpolation-aspect=live-window fullscreen=alt-enter-standalone\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_V156C=1 producer=gc-fit scanout=window-wide "
               "wide-interpolation=%u override=GEKKOAOT_WIDESCREEN_INTERPOLATION\n",
               g_frame_interpolation_enabled ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_V157=1 projection=perspective-only "
               "orthographic=retail-preserved postprocess=safe\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_V158=1 projection=perspective+ui-ortho "
               "ortho-efb-copy=retail-preserved interpolation=%u\n",
               g_frame_interpolation_enabled ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_SOLID_SPANS_V343=%u ortho=untextured-full-width "
               "action=host-width env=GEKKOAOT_WIDESCREEN_SOLID_SPANS\n",
               g_wide_solid_spans ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_UI_INSETS_V348=%u span=inner+narrow confirm=displayed-uncopied "
               "action=ui-viewport+scissor env=GEKKOAOT_WIDESCREEN_UI_INSETS\n",
               g_ui_insets ? 1u : 0u);
  return true;
}
