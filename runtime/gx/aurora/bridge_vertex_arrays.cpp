// SPDX-License-Identifier: GPL-3.0-or-later
// Texture verdicts, vertex array mapping, index scan layouts and XF windows.
// Included by bridge.cpp, whose translation unit holds the bridge state; this
// file is not compiled on its own.

// Texture uniformity verdicts of the current frame (IsUniformTexture scans up
// to 16 KiB per call; Star Fox Adventures draws thousands of orthographic mask
// triangles per frame over the same few textures). Dropped at each display
// copy and whenever texture memory may have changed (EFB copies, TMEM/texture
// cache invalidation).
// Texture-content verdicts used by screen-fill classification. Both
// predicates are pure functions of the first 16 KiB of the texture, so a
// verdict stays valid while an XXH3 fingerprint of those bytes is unchanged;
// the fingerprint is re-checked once per display copy (the previous per-frame
// validity) instead of rescanning the texture on every draw. The old 16-entry
// round-robin rescanned whenever a frame sampled more textures, and the
// opacity test was not cached at all.
struct TextureVerdict {
  const void* data=nullptr; std::size_t bytes=0; unsigned format=0;
  std::uint64_t frame=~0ull, fingerprint=0; signed char uniform=-1, opaque=-1;
};
std::array<TextureVerdict,256> g_texture_verdicts{};
TextureVerdict& TextureVerdictFor(const void* data,std::size_t bytes,unsigned format) {
  const auto index=std::size_t((std::uint64_t(reinterpret_cast<std::uintptr_t>(data))*0x9E3779B97F4A7C15ull)>>56);
  auto& verdict=g_texture_verdicts[index];
  const bool same=verdict.data==data && verdict.bytes==bytes && verdict.format==format;
  if(same && verdict.frame==g_display_copy_count) return verdict;
  const std::uint64_t fingerprint=data && bytes ? XXH3_64bits(data,bytes) : 0u;
  if(!same || verdict.fingerprint!=fingerprint) {
    verdict.data=data; verdict.bytes=bytes; verdict.format=format;
    verdict.fingerprint=fingerprint; verdict.uniform=verdict.opaque=-1;
  }
  verdict.frame=g_display_copy_count;
  return verdict;
}
void ForgetUniformTextureVerdicts() { g_texture_verdicts={}; }

bool ClassifyScreenFill(const std::uint8_t* data,std::size_t length,
                        GXPrimitive primitive,GXVtxFmt format,unsigned count) {
  using namespace aurora::gx;
  const auto& state=g_gxState;
  // A fade or flash drawn as a 3D quad in front of the camera covers the
  // retail view exactly; hor+ would shrink it to 4:3 and leave the widened
  // scene visible beside it (Call of Duty: Finest Hour level fades).
  const bool perspective=state.projType==GX_PERSPECTIVE;
  if (perspective ? !g_wide_perspective_fills : state.projType!=GX_ORTHOGRAPHIC) return false;
  // Meshes never qualify: only a single quad (or two triangles) can be a fade.
  if (perspective && !((count==4u && (primitive==GX_QUADS || primitive==GX_TRIANGLESTRIP ||
                                       primitive==GX_TRIANGLEFAN)) ||
                       (count==6u && primitive==GX_TRIANGLES))) return false;
  const bool fill=!perspective && state.numTexGens==1u && state.numTevStages<=2u &&
      state.blendMode==GX_BM_NONE &&
      ((count==4u && (primitive==GX_QUADS || primitive==GX_TRIANGLESTRIP)) ||
       (count==6u && primitive==GX_TRIANGLES));
  // Full-width spans (letterbox bars, fades, flashes) also bypass the UI
  // x-scale. Otherwise they stay centred at 4:3 width and leave the widened 3D
  // scene visible beside a cinematic bar or a fade.
  const bool span=g_wide_solid_spans && count<=kMaxSpanVertices &&
      ((primitive==GX_QUADS && count>=4u && count%4u==0u) ||
       (primitive==GX_TRIANGLES && count>=3u && count%3u==0u) ||
       // Fans too (Call of Duty: Finest Hour's mission fade-in).
       ((primitive==GX_TRIANGLESTRIP || primitive==GX_TRIANGLEFAN) && count>=4u));
  // A stage samples its texture only when a combiner input reads TEXC/TEXA
  // (Aurora's shader_info rule); CoS binds texmap 0 to its solid cinematic
  // bars without reading it.
  const auto samples=[](const TevStage& stage) {
    if (stage.texMapId==GX_TEXMAP_NULL) return false;
    const auto& c=stage.colorPass; const auto& a=stage.alphaPass;
    for (const auto arg : {c.a,c.b,c.c,c.d}) if (arg==GX_CC_TEXC || arg==GX_CC_TEXA) return true;
    for (const auto arg : {a.a,a.b,a.c,a.d}) if (arg==GX_CA_TEXA) return true;
    return false;
  };
  bool sampled=state.numIndStages!=0u || state.zTexOp!=GX_ZT_DISABLE, uniform=!sampled;
  // Perspective quads test their geometry first (cheap) and textures after.
  const auto texture_uniformity=[&] {
  for (unsigned stage=0;span && uniform && stage<state.numTevStages && stage<MaxTevStages;++stage) {
    const auto map=state.tevStages[stage].texMapId;
    if (!samples(state.tevStages[stage])) continue;
    sampled=true;
    const auto& slot=state.loadedTextures[unsigned(map)&7u];
    const std::size_t bytes=slot.data ? std::min<std::size_t>(
        texture::texture_source_size(slot.raw_format(),slot.width(),slot.height(),1u),16384u) : 0u;
    auto& verdict=TextureVerdictFor(slot.data,bytes,unsigned(slot.raw_format()));
    if (verdict.uniform<0)
      verdict.uniform=GekkoAOT::GX::IsUniformTexture(static_cast<const std::uint8_t*>(slot.data),
                                                     bytes,slot.raw_format()) ? 1 : 0;
    uniform=verdict.uniform!=0;
  }
  };
  if (!perspective) texture_uniformity();
  // A whole-screen quad sampling only EFB copies composites screen-
  // derived data: bloom/blur chains downsample the widened EFB into small
  // copies and add the result back over the frame (Shadow the Hedgehog). The
  // small copy fails the screen-copy size test, but it still covers the whole
  // retail width, so it must keep it rather than shrink to 4:3.
  bool copy_only=false, screen_inputs=false;
  const aurora::gfx::ClipRect* copy_source=nullptr;
  if (span && sampled && state.numIndStages==0u && !perspective) {
    bool any=false;
    copy_only=true;
    screen_inputs=g_copy_provenance_enabled;
    for (unsigned stage=0;stage<state.numTevStages && stage<MaxTevStages;++stage) {
      if (!samples(state.tevStages[stage])) continue;
      any=true;
      const auto& slot=state.loadedTextures[unsigned(state.tevStages[stage].texMapId)&7u];
      const auto copy=slot.data ? state.copyTextures.find(slot.data) : state.copyTextures.end();
      if (copy==state.copyTextures.end()) { copy_only=false; break; }
      screen_inputs=screen_inputs && g_copy_provenance.IsScreenDerived(slot.data);
      copy_source=copy_source && copy_source!=&copy->second.guestSource ? nullptr : &copy->second.guestSource;
    }
    copy_only=copy_only && any;
    screen_inputs=screen_inputs && copy_only;
  }
  // A single modulate stage (RASC x TEXC) with a black raster colour
  // draws black whatever the texture holds (Metroid Prime's letterbox bars);
  // with alpha that ignores the texture, or an opaque texture, it is solid.
  bool vertex_black=false;
  if (!perspective && span && sampled && !uniform && state.numTevStages==1u && state.numIndStages==0u &&
      state.zTexOp==GX_ZT_DISABLE) {
    const auto& st=state.tevStages[0];
    const auto& c=st.colorPass; const auto& a=st.alphaPass;
    const bool modulate=c.a==GX_CC_ZERO && c.d==GX_CC_ZERO &&
        ((c.b==GX_CC_RASC && c.c==GX_CC_TEXC) || (c.b==GX_CC_TEXC && c.c==GX_CC_RASC));
    const bool alpha_reads_texture=a.a==GX_CA_TEXA || a.b==GX_CA_TEXA || a.c==GX_CA_TEXA || a.d==GX_CA_TEXA;
    const auto& slot=state.loadedTextures[unsigned(st.texMapId)&7u];
    const bool alpha_solid=!alpha_reads_texture || [&] {
      const std::size_t bytes=slot.data ? std::min<std::size_t>(
          texture::texture_source_size(slot.raw_format(),slot.width(),slot.height(),1u),16384u) : 0u;
      auto& verdict=TextureVerdictFor(slot.data,bytes,unsigned(slot.raw_format()));
      if (verdict.opaque<0)
        verdict.opaque=GekkoAOT::GX::IsOpaqueTexture(static_cast<const std::uint8_t*>(slot.data),
                                                     bytes,slot.raw_format()) ? 1 : 0;
      return verdict.opaque!=0;
    }();
    if (modulate && alpha_solid && (st.channelId==GX_COLOR0A0 || st.channelId==GX_COLOR0)) {
      if (state.colorChannelConfig[0].matSrc==GX_SRC_REG) {
        const auto& m=state.colorChannelState[0].matColor;
        uniform=m[0]==0.f && m[1]==0.f && m[2]==0.f;
      } else {
        vertex_black=state.vtxDesc[GX_VA_CLR0]==GX_DIRECT;
      }
    }
  }
  // Diagnostic: GEKKOAOT_WIDESCREEN_DRAWLOG_AT=<s> logs every small draw for
  // ~0.1 s that many seconds after the first draw.
  static const double drawlog_at=[] { const char* v=std::getenv("GEKKOAOT_WIDESCREEN_DRAWLOG_AT"); return v ? std::atof(v) : 0.0; }();
  static const auto drawlog_origin=std::chrono::steady_clock::now();
  bool drawlog=false;
  if (drawlog_at>0.0) {
    const double t=std::chrono::duration<double>(std::chrono::steady_clock::now()-drawlog_origin).count();
    static const double drawlog_for=[] { const char* v=std::getenv("GEKKOAOT_WIDESCREEN_DRAWLOG_FOR"); return v ? std::atof(v) : 4.0; }();
    drawlog=t>=drawlog_at && t<drawlog_at+drawlog_for;
  }
  const bool trace=(g_wide_span_trace || drawlog) && count>=3u && count<=kMaxSpanVertices &&
      primitive!=GX_POINTS && primitive!=GX_LINES && primitive!=GX_LINESTRIP;
  if (!fill && !span && !trace) return false;
  const auto display=aurora::vi::configured_fb_size();
  const auto& viewport=state.logicalViewport;
  // Horizontal stretching only needs a full-width viewport; PoA renders its
  // cinematics into a 640x448 viewport of a 640x480 display.
  const bool wide_viewport=DisplayViewWidth(viewport.width)>=display.x*.95f;
  const bool full_viewport=wide_viewport && viewport.height>=display.y*.95f;
  if (!wide_viewport && !trace && !copy_only) return false;
  const unsigned stride=VertexSize(format);
  if (!stride || length!=3u+count*stride) return false;
  unsigned position_offset=0, matrix_offset=0;
  bool indexed_matrix=false;
  for (int attr=GX_VA_PNMTXIDX;attr<GX_VA_POS;++attr) {
    const auto desc=state.vtxDesc[attr];
    if (desc==GX_NONE) continue;
    if (desc!=GX_DIRECT) return false;
    if (attr==GX_VA_PNMTXIDX) { indexed_matrix=true;matrix_offset=position_offset; }
    const auto& f=state.vtxFmts[format].attrs[attr];
    position_offset+=unsigned(comp_type_size(static_cast<GXAttr>(attr),f.type))*
                     comp_cnt_count(static_cast<GXAttr>(attr),f.cnt);
  }
  const auto desc=state.vtxDesc[GX_VA_POS];
  const auto& posfmt=state.vtxFmts[format].attrs[GX_VA_POS];
  const unsigned components=comp_cnt_count(GX_VA_POS,posfmt.cnt);
  const unsigned size=comp_type_size(GX_VA_POS,posfmt.type);
  if (components<2u || components>3u || !size) return false;
  // CLR0 offset, laid out exactly like calc_vtx_size().
  unsigned color_offset=0;
  const auto color_type=state.vtxFmts[format].attrs[GX_VA_CLR0].type;
  if (vertex_black) {
    for (int attr=GX_VA_PNMTXIDX;attr<GX_VA_CLR0;++attr) {
      const auto d=state.vtxDesc[attr];
      const auto& f=state.vtxFmts[format].attrs[attr];
      if (d==GX_DIRECT)
        color_offset+=unsigned(comp_type_size(static_cast<GXAttr>(attr),f.type))*
                      comp_cnt_count(static_cast<GXAttr>(attr),f.cnt);
      else if (d==GX_INDEX8 || d==GX_INDEX16)
        color_offset+=(attr==GX_VA_NRM && f.cnt==GX_NRM_NBT3 ? 3u : 1u)*(d==GX_INDEX16 ? 2u : 1u);
    }
    vertex_black=color_offset+comp_type_size(GX_VA_CLR0,color_type)<=stride;
  }
  int vertex_alpha=-1;
  std::array<std::array<float,2>,kMaxSpanVertices> corners{};
  std::array<float,kMaxSpanVertices> view_z{};
  for (unsigned i=0;i<count;++i) {
    const auto* vertex=data+3u+i*stride;
    if (vertex_black) {
      const auto* p=vertex+color_offset;
      unsigned rgb=1u, alpha=255u;
      switch (color_type) {
      case GX_RGB565: rgb=(unsigned(p[0])<<8)|p[1]; break;
      case GX_RGB8: case GX_RGBX8: rgb=unsigned(p[0])|p[1]|p[2]; break;
      case GX_RGBA4: rgb=unsigned(p[0])|(p[1]&0xf0u); alpha=(p[1]&0x0fu)*17u; break;
      case GX_RGBA6: { const unsigned v=(unsigned(p[0])<<16)|(unsigned(p[1])<<8)|p[2]; rgb=v>>6; alpha=v&63u; break; }
      case GX_RGBA8: rgb=unsigned(p[0])|p[1]|p[2]; alpha=p[3]; break;
      default: break;
      }
      if (rgb!=0u || (vertex_alpha>=0 && unsigned(vertex_alpha)!=alpha)) vertex_black=false;
      vertex_alpha=int(alpha);
    }
    const auto* pos=vertex+position_offset;
    bool little=false;
    if (desc==GX_INDEX8 || desc==GX_INDEX16) {
      const unsigned index=desc==GX_INDEX8 ? *pos : Be16(pos);
      const auto& array=state.arrays[GX_VA_POS];
      const std::uint64_t offset=std::uint64_t(index)*array.stride;
      if (!array.data || offset+components*size>array.size) return false;
      pos=static_cast<const std::uint8_t*>(array.data)+offset;little=array.le;
    } else if (desc!=GX_DIRECT || position_offset+components*size>stride) return false;
    float xyz[4]={0,0,0,1};
    for (unsigned axis=0;axis<components;++axis) {
      const auto* q=pos+axis*size;
      const unsigned bits=size==1u ? *q : size==2u ?
          (little ? unsigned(q[0])|(unsigned(q[1])<<8u) : Be16(q)) :
          (little ? unsigned(q[0])|(unsigned(q[1])<<8u)|(unsigned(q[2])<<16u)|(unsigned(q[3])<<24u) : Be32(q));
      // VAT frac is 5 bits: 8/16-bit integers scaled by 2^-frac stay normal
      // floats, so the multiply is exactly ldexp without a libm call per vertex.
      const float scale=1.f/float(1u<<(unsigned(posfmt.frac)&31u));
      switch (posfmt.type) {
      case GX_F32: std::memcpy(&xyz[axis],&bits,4u);break;
      case GX_U8: case GX_U16: xyz[axis]=float(bits)*scale;break;
      case GX_S8: xyz[axis]=float(static_cast<std::int8_t>(bits))*scale;break;
      case GX_S16: xyz[axis]=float(static_cast<std::int16_t>(bits))*scale;break;
      default: return false;
      }
    }
    const unsigned matrix=indexed_matrix ? (vertex[matrix_offset]&63u) : state.currentPnMtx;
    if (matrix+2u>=state.xfPosRows.size()) return false;
    float transformed[3]={0,0,0};
    for (unsigned axis=0;axis<(perspective ? 3u : 2u);++axis)
      for (unsigned j=0;j<4;++j) transformed[axis]+=state.xfPosRows[matrix+axis][j]*xyz[j];
    view_z[i]=transformed[2];
    if (perspective) {
      const auto row=[&](const auto& m) {
        return m[0]*transformed[0]+m[1]*transformed[1]+m[2]*transformed[2]+m[3];
      };
      const float w=row(state.proj.m3);
      if (!(w>1e-6f)) return false;
      corners[i]={row(state.proj.m0)/w,row(state.proj.m1)/w};
    } else {
      corners[i]={state.proj.m0[0]*transformed[0]+state.proj.m0[3],
                  state.proj.m1[1]*transformed[1]+state.proj.m1[3]};
    }
    // HUD sprites and glyphs fail the span test at their first vertex.
    if (!fill && !trace && !copy_only && std::abs(corners[i][0])<.975f) return false;
  }
  bool flat_depth_fill=false;
  const bool full_depth_span=state.zTexOp!=GX_ZT_DISABLE && state.depthUpdate && count<=6u &&
      GekkoAOT::GX::IsFullScreenSpan({corners.data(),count});
  if(full_depth_span && state.numTevStages>0u){
    // Z texture uses the final TEV stage. A uniform 4x4 GX depth-clear
    // texture is not a pre-rendered backdrop (J3D/Mario Kart Double Dash).
    const auto map=state.tevStages[std::min<unsigned>(state.numTevStages,MaxTevStages)-1u].texMapId;
    if(map!=GX_TEXMAP_NULL){
      const auto& depth=state.loadedTextures[unsigned(map)&7u];
      const auto bytes=depth.data?texture::texture_source_size(depth.raw_format(),depth.width(),depth.height(),1u):0u;
      flat_depth_fill=bytes>0 && bytes<=16384u &&
        GekkoAOT::GX::IsUniformTexture(static_cast<const std::uint8_t*>(depth.data),bytes,depth.raw_format());
    }
  }
  if(g_wide_prerendered_43 && GekkoAOT::GX::IsPrerenderedDepthBackdrop(
      state.zTexOp!=GX_ZT_DISABLE,state.depthUpdate,full_depth_span,flat_depth_fill,screen_inputs))
    g_frame_prerendered=true;
  // A constant-Z overwrite covers the whole EFB, including the newly exposed
  // side bands. It must follow the same pixel mapping as an ordinary clear.
  if(flat_depth_fill && state.blendMode==GX_BM_NONE &&
     (!state.depthCompare || state.depthFunc==GX_ALWAYS))return true;
  if (trace) {
    float lo_x=corners[0][0],hi_x=lo_x,lo_y=corners[0][1],hi_y=lo_y;
    for (unsigned i=1;i<count;++i) {
      lo_x=std::min(lo_x,corners[i][0]); hi_x=std::max(hi_x,corners[i][0]);
      lo_y=std::min(lo_y,corners[i][1]); hi_y=std::max(hi_y,corners[i][1]);
    }
    unsigned maps=0;
    for (unsigned stage=0;stage<state.numTevStages && stage<MaxTevStages;++stage)
      maps=maps*16u+(state.tevStages[stage].texMapId==GX_TEXMAP_NULL ? 15u : unsigned(state.tevStages[stage].texMapId)&7u);
    const std::uint64_t signature=(std::uint64_t(primitive)<<56)^(std::uint64_t(count)<<48)^
        (std::uint64_t(state.numTexGens)<<44)^(std::uint64_t(maps)<<24)^(std::uint64_t(state.blendMode)<<20)^
        (std::uint64_t(viewport.height)<<8)^std::uint64_t(int(lo_y*8.f)&15)^(std::uint64_t(int(hi_y*8.f)&15)<<4);
    for (unsigned stage=0;stage<state.numTevStages && stage<MaxTevStages;++stage) {
      if (!samples(state.tevStages[stage])) continue;
      const auto& slot=state.loadedTextures[unsigned(state.tevStages[stage].texMapId)&7u];
      const auto copy=state.copyTextures.find(slot.data);
      if (!slot.data || copy==state.copyTextures.end()) continue;
      const auto& src=copy->second.guestSource;
      static std::vector<std::uint64_t> sprites;
      const std::uint64_t key=(std::uint64_t(src.width)<<48)^(std::uint64_t(src.height)<<36)^
          (std::uint64_t(int(lo_x*16.f)&255)<<16)^(std::uint64_t(int(hi_x*16.f)&255)<<8)^std::uint64_t(viewport.width);
      if (sprites.size()<64u && std::find(sprites.begin(),sprites.end(),key)==sprites.end()) {
        sprites.push_back(key);
        std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_COPY_SPRITE_TRACE src=%d,%d,%dx%d tex=%ux%u viewport=%.0fx%.0f@%.0f,%.0f "
                     "x=%.3f..%.3f y=%.3f..%.3f count=%u prim=0x%02x\n",src.x,src.y,src.width,src.height,
                     slot.width(),slot.height(),viewport.width,viewport.height,viewport.left,viewport.top,
                     lo_x,hi_x,lo_y,hi_y,count,unsigned(primitive));
      }
    }
    static std::vector<std::uint64_t> seen;
    if (hi_x-lo_x>=1.5f && seen.size()<128u &&
        std::find(seen.begin(),seen.end(),signature)==seen.end()) {
      seen.push_back(signature);
      const auto& st0=state.tevStages[0];
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_SPAN_TRACE_V343 prim=0x%02x count=%u texgens=%u tev=%u maps=%x blend=%u "
                   "viewport=%.0fx%.0f@%.0f,%.0f full=%u x=%.3f..%.3f y=%.3f..%.3f fill=%u span=%u sampled=%u uniform=%u "
                   "c0=%u,%u,%u,%u a0=%u,%u,%u,%u chan=%u kc=%u matsrc=%u mat=%.2f,%.2f,%.2f,%.2f clr0=%u tex=%ux%u:%u\n",
                   unsigned(primitive),count,unsigned(state.numTexGens),unsigned(state.numTevStages),maps,
                   unsigned(state.blendMode),viewport.width,viewport.height,viewport.left,viewport.top,
                   full_viewport?1u:0u,lo_x,hi_x,lo_y,hi_y,fill?1u:0u,span?1u:0u,sampled?1u:0u,uniform?1u:0u,
                   unsigned(st0.colorPass.a),unsigned(st0.colorPass.b),unsigned(st0.colorPass.c),unsigned(st0.colorPass.d),
                   unsigned(st0.alphaPass.a),unsigned(st0.alphaPass.b),unsigned(st0.alphaPass.c),unsigned(st0.alphaPass.d),
                   unsigned(st0.channelId),unsigned(st0.kcSel),unsigned(state.colorChannelConfig[0].matSrc),
                   state.colorChannelState[0].matColor[0],state.colorChannelState[0].matColor[1],
                   state.colorChannelState[0].matColor[2],state.colorChannelState[0].matColor[3],
                   unsigned(state.vtxDesc[GX_VA_CLR0]),
                   st0.texMapId==GX_TEXMAP_NULL?0u:state.loadedTextures[unsigned(st0.texMapId)&7u].width(),
                   st0.texMapId==GX_TEXMAP_NULL?0u:state.loadedTextures[unsigned(st0.texMapId)&7u].height(),
                   st0.texMapId==GX_TEXMAP_NULL?0u:unsigned(state.loadedTextures[unsigned(st0.texMapId)&7u].raw_format()));
      if (st0.texMapId!=GX_TEXMAP_NULL && state.loadedTextures[unsigned(st0.texMapId)&7u].data) {
        const auto* tb=static_cast<const std::uint8_t*>(state.loadedTextures[unsigned(st0.texMapId)&7u].data);
        std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_SPAN_TEX");
        for (unsigned i=0;i<48;++i) std::fprintf(stderr,"%s%02x",i%8?"":" ",tb[i]);
        std::fprintf(stderr,"\n");
      }
    }
  }
  if (vertex_black) uniform=true;
  if (drawlog) {
    float lo_x=corners[0][0],hi_x=lo_x,lo_y=corners[0][1],hi_y=lo_y;
    for (unsigned i=1;i<count;++i) {
      lo_x=std::min(lo_x,corners[i][0]); hi_x=std::max(hi_x,corners[i][0]);
      lo_y=std::min(lo_y,corners[i][1]); hi_y=std::max(hi_y,corners[i][1]);
    }
    float vz_lo=view_z[0],vz_hi=view_z[0];
    for (unsigned i=1;i<count;++i) { vz_lo=std::min(vz_lo,view_z[i]); vz_hi=std::max(vz_hi,view_z[i]); }
    const auto& st0=state.tevStages[0];
    // Wide draws, and draws entirely outside the 4:3 view that widescreen
    // can reveal in the side bands.
    if (hi_x-lo_x>=1.f || (hi_x<-1.f && hi_x>-1.4f) || (lo_x>1.f && lo_x<1.4f))
    std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_DRAWLOG copy=%llu ztex=%u fill=%u ztest=%u zwrite=%u vz=%.2f..%.2f pm=%.3f,%.3f proj=%u prim=0x%02x count=%u x=%.3f..%.3f y=%.3f..%.3f view=%.0fx%.0f@%.0f,%.0f scissor=%d,%d,%dx%d tev=%u texgens=%u blend=%u span=%u sampled=%u uniform=%u copy_only=%u c0=%u,%u,%u,%u a0=%u,%u,%u,%u tex=%ux%u:%u\n",
                 static_cast<unsigned long long>(g_display_copy_count),unsigned(state.zTexOp),fill?1u:0u,
                 state.depthCompare?1u:0u,state.depthUpdate?1u:0u,
                 vz_lo,vz_hi,state.proj.m0[0],state.proj.m1[1],unsigned(state.projType),unsigned(primitive),count,
                 lo_x,hi_x,lo_y,hi_y,viewport.width,viewport.height,viewport.left,viewport.top,
                 state.logicalScissor.x,state.logicalScissor.y,state.logicalScissor.width,state.logicalScissor.height,
                 unsigned(state.numTevStages),unsigned(state.numTexGens),unsigned(state.blendMode),span?1u:0u,
                 sampled?1u:0u,uniform?1u:0u,copy_only?1u:0u,
                 unsigned(st0.colorPass.a),unsigned(st0.colorPass.b),unsigned(st0.colorPass.c),unsigned(st0.colorPass.d),
                 unsigned(st0.alphaPass.a),unsigned(st0.alphaPass.b),unsigned(st0.alphaPass.c),unsigned(st0.alphaPass.d),
                 st0.texMapId==GX_TEXMAP_NULL?0u:state.loadedTextures[unsigned(st0.texMapId)&7u].width(),
                 st0.texMapId==GX_TEXMAP_NULL?0u:state.loadedTextures[unsigned(st0.texMapId)&7u].height(),
                 st0.texMapId==GX_TEXMAP_NULL?0u:unsigned(state.loadedTextures[unsigned(st0.texMapId)&7u].raw_format()));
  }
  if (perspective) {
    if (!span || !wide_viewport || !GekkoAOT::GX::IsFullScreenSpan({corners.data(),count})) return false;
    texture_uniformity();
    const bool screen=uniform;
    static unsigned perspective_logs=0;
    if (screen && perspective_logs++<8u)
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_PERSPECTIVE_FILL_V356 prim=0x%02x count=%u blend=%u sampled=%u action=retail-width\n",
                   unsigned(primitive),count,unsigned(state.blendMode),sampled?1u:0u);
    return screen;
  }
  if (copy_only && g_copy_provenance_enabled) {
    // Logical EFB rectangle covered by this pass (GX viewport y points down).
    float lo_x=corners[0][0],hi_x=lo_x,lo_y=corners[0][1],hi_y=lo_y;
    for (unsigned i=1;i<count;++i) {
      lo_x=std::min(lo_x,corners[i][0]); hi_x=std::max(hi_x,corners[i][0]);
      lo_y=std::min(lo_y,corners[i][1]); hi_y=std::max(hi_y,corners[i][1]);
    }
    const float x0=viewport.left+(std::max(lo_x,-1.f)+1.f)*.5f*viewport.width;
    const float x1=viewport.left+(std::min(hi_x,1.f)+1.f)*.5f*viewport.width;
    const float y0=viewport.top+(1.f-std::min(hi_y,1.f))*.5f*viewport.height;
    const float y1=viewport.top+(1.f-std::max(lo_y,-1.f))*.5f*viewport.height;
    // v354 rule A: a pass sampling only screen-derived copies is EFB pixel
    // work (downsample/blur/upsample/composite) and keeps the retail width.
    // Rule B: a copy drawn back onto exactly its own source rectangle
    // restores saved EFB contents (Lemony saves the quarter its bloom
    // pyramid overwrites) and must land on the same pixels.
    const bool restore=copy_source &&
        std::abs(x0-float(copy_source->x))<=2.f && std::abs(y0-float(copy_source->y))<=2.f &&
        std::abs(x1-float(copy_source->x+copy_source->width))<=2.f &&
        std::abs(y1-float(copy_source->y+copy_source->height))<=2.f;
    if (screen_inputs || restore) {
      if (screen_inputs) g_copy_provenance.NoteScreenDraw(x0,y0,x1,y1);
      static unsigned provenance_logs=0;
      if (provenance_logs++<16u)
        std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_COPY_PROVENANCE_V354 rule=%s rect=%.0f,%.0f-%.0f,%.0f viewport=%.0fx%.0f action=retail-width\n",
                     screen_inputs ? "screen-derived" : "restore",x0,y0,x1,y1,viewport.width,viewport.height);
      return true;
    }
  }
  if (span && wide_viewport && copy_only &&
      GekkoAOT::GX::IsFullScreenSpan({corners.data(),count})) {
    static unsigned composite_logs=0;
    if (composite_logs++<8u)
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_COPY_COMPOSITE_V353 prim=0x%02x count=%u blend=%u action=retail-width\n",
                   unsigned(primitive),count,unsigned(state.blendMode));
    return true;
  }
  if (span && wide_viewport && uniform &&
      GekkoAOT::GX::IsEdgeAnchoredSpan({corners.data(),count})) {
    if (g_wide_span_logs<8u) {
      ++g_wide_span_logs;
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_SPAN_V343 prim=0x%02x count=%u sampled=%u blend=%u y=%.3f..%.3f\n",
                   unsigned(primitive),count,sampled?1u:0u,unsigned(state.blendMode),
                   corners[0][1],corners[count-1u][1]);
    }
    return true;
  }
  if (!fill || !full_viewport) return false;
  bool screen=false;
  if (count==6u) {
    std::array<std::array<float,2>,6> triangles{};
    std::copy_n(corners.begin(),6u,triangles.begin());
    screen=GekkoAOT::GX::IsScreenFillTriangles(triangles);
  } else {
    std::array<std::array<float,2>,4> quad{};
    std::copy_n(corners.begin(),4u,quad.begin());
    screen=GekkoAOT::GX::IsScreenFillQuad(quad,primitive==GX_TRIANGLESTRIP);
  }
  if (screen) {
    // Diagnostic: which textures the opaque full-screen rule stretches.
    const auto& slot=state.loadedTextures[0];
    const std::uint64_t key=(std::uint64_t(slot.width())<<32)|(std::uint64_t(slot.height())<<8)|slot.raw_format();
    static std::vector<std::uint64_t> fills;
    if (fills.size()<8u && std::find(fills.begin(),fills.end(),key)==fills.end()) {
      fills.push_back(key);
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_FILL_V253 tex=%ux%u fmt=%u data=%u tev=%u\n",
                   slot.width(),slot.height(),slot.raw_format(),slot.data?1u:0u,unsigned(state.numTevStages));
    }
  }
  return screen;
}

void MapArrayBase(std::uint8_t reg, std::uint32_t value) {
  using namespace aurora::gx;
  const std::uint32_t attr=std::uint32_t(reg-0xA0u)+GX_VA_POS;
  if(attr>=GX_VA_MAX_ATTR) return;
  g_array_guest_base[attr]=value;
  auto& array=g_gxState.arrays[attr];
  const void* ptr=nullptr; std::uint32_t available=0;
  const bool mapped=Resolve(value,1,&ptr,&available);
  if(g_array_base_trace) {
    ++g_array_base_writes;
    const bool same=mapped && g_gxState.cpRegValid.test(reg) &&
                    g_gxState.cpRegCache[reg]==value && array.data==ptr &&
                    g_array_available[attr]==available;
    if(same) {
      ++g_array_base_same_binding;
      if(array.cachedRange.size!=0u) {
        ++g_array_base_same_cached;
        g_array_base_same_cached_bytes+=array.cachedRange.size;
      }
    }
    if((g_array_base_writes&0x3fffu)==0u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_ARRAY_BASE_TRACE_V1 writes=%llu same_binding=%llu same_cached=%llu discarded_cached_bytes=%llu\n",
                   static_cast<unsigned long long>(g_array_base_writes),
                   static_cast<unsigned long long>(g_array_base_same_binding),
                   static_cast<unsigned long long>(g_array_base_same_cached),
                   static_cast<unsigned long long>(g_array_base_same_cached_bytes));
  }

  // The CP register write is authoritative even when the pointed address is
  // not currently mappable. Never retain the previous host pointer: doing so
  // makes a failed base update render stale vertices/matrices from an older
  // frame, which looks exactly like random GPU/VRAM corruption. Actual use of
  // an unmapped array will fail closed in the per-command window checks.
  if(!mapped) {
    array.data=nullptr; array.size=0; array.cachedRange={};
    g_array_available[attr]=0u;
    g_gxState.cpRegValid.set(reg); g_gxState.cpRegCache[reg]=value;
    g_gxState.dirty |= DirtyImmediates;
    static unsigned invalid_logs=0u;
    if(invalid_logs++<64u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_ARRAY_BASE_V86=0 reg=0x%02x attr=%u guest=%08x action=invalidate-stale-mapping\n",
                   unsigned(reg),attr,value);
    return;
  }

  // GX only exposes an array base and stride; it does not expose a byte size.
  // Do NOT hand Aurora the remainder of MEM1 here. Aurora uploads array.size to
  // its per-frame storage staging buffer, so using `available` (often tens of
  // MiB) overflows Aurora's fixed 8 MiB storage buffer as soon as indexed 3D
  // geometry is drawn. Keep the host mapping limit separately and grow the
  // visible Aurora window from the indices actually referenced by each draw.
  array.data=ptr; array.size=0; array.le=false; array.cachedRange={};
  g_array_available[attr]=available;
  g_gxState.cpRegValid.set(reg); g_gxState.cpRegCache[reg]=value;
  g_gxState.dirty |= DirtyImmediates;
}

// The sole semantic sink for admitted SDK, FIFO and display-list state. Resource mapping
// and dirty-state policy stay in Aurora's existing recording thread. A failed
// submission is fatal to its caller, never retried through another decoder.
bool ApplyPixelOperation(std::uint32_t word, bool direct_state, std::span<const std::uint8_t> wire);
bool SubmitNativeOperation(const GekkoAOT::GX::NativeOperation& operation,
                           GekkoAOT::GX::NativeOrigin origin, bool validated_list = false) {
  using namespace GekkoAOT::GX;
  PhaseScope phase_scope(kPhOperation);
  // A cache plan has already validated EVERY owned operation before any side
  // effect, including its suffix. Reclassifying each BP domain on every hit
  // repeats that work; ordinary submissions still validate at this boundary.
  if (!g_ready || g_fifo_fault || g_quit || (!validated_list && !ValidNativeOperation(operation))) return false;
  const auto begin = g_native_ir_profile ? std::chrono::steady_clock::now() :
                                         std::chrono::steady_clock::time_point{};
  // State-only plans cannot contain CPU EFB pokes. Their caller flushes once
  // before the ordered batch, matching ProcessSpan's raw-list boundary.
  if(!validated_list)FlushEfbPokes();
  if (const auto* array = std::get_if<NativeArrayWrite>(&operation)) {
    const auto fields = static_cast<unsigned>(array->fields);
    if (fields & 1u) MapArrayBase(std::uint8_t(0xa0u + array->index), array->base);
    if (fields & 2u) aurora::gx::fifo::handle_cp(std::uint8_t(0xb0u + array->index), array->stride);
    if (g_native_ir_profile) ++g_native_ir_stats.arrays;
  } else if (const auto* upload = std::get_if<NativeMatrixUpload>(&operation)) {
    const auto& matrix = *upload;
    auto& state = aurora::gx::g_gxState;
    bool changed = false;
    for (unsigned i = 0; i < matrix.words; ++i) {
      float* value;
      if (matrix.address < 0x100u) {
        const auto word = matrix.address + i;
        value = &state.xfPosRows[word / 4u][word % 4u];
      } else if (matrix.address < 0x500u) {
        const auto word = matrix.address - 0x400u + i;
        value = &state.xfNrmRows[word / 3u][word % 3u];
      } else {
        value = reinterpret_cast<float*>(&state.ptTexMtxs[(matrix.address - 0x500u) / 12u]) + i;
      }
      if (std::bit_cast<std::uint32_t>(*value) != matrix.values[i]) {
        *value = std::bit_cast<float>(matrix.values[i]);
        changed = true;
      }
    }
    if (changed) state.dirty |= aurora::gx::DirtyUniform;
    if (g_native_ir_profile) {
      ++g_native_ir_stats.matrices;
      if (!changed) ++g_native_ir_stats.unchanged_matrices;
    }
  } else if (const auto* vertex=std::get_if<NativeVertexState>(&operation)) {
    const auto reg=NativeVertexRegister(*vertex);
    aurora::gx::fifo::handle_cp(reg,vertex->value);
    InvalidateVertexSize(reg);
    if(g_native_ir_profile)++g_native_ir_stats.vertex_states;
  } else if (const auto* projection=std::get_if<NativeProjection>(&operation)) {
    aurora::gx::fifo::handle_xf_words(0x1020u,projection->values);
    if(g_native_ir_profile)++g_native_ir_stats.projections;
  } else if (const auto* viewport=std::get_if<NativeViewport>(&operation)) {
    aurora::gx::fifo::handle_xf_words(0x101au,viewport->values);
    if(g_native_ir_profile)++g_native_ir_stats.viewports;
  } else if (const auto* transform=std::get_if<NativeTransformState>(&operation)) {
    aurora::gx::fifo::handle_xf_words(transform->address,{transform->values.data(),transform->words});
    if(g_native_ir_profile)++g_native_ir_stats.transform_states;
  } else {
    const auto& pixel=std::get<NativePixelState>(operation);
    if(!ApplyPixelOperation((std::uint32_t(pixel.reg)<<24)|pixel.value,true,{}))return false;
    if(g_native_ir_profile)++g_native_ir_stats.pixel_states;
  }
  if (g_native_ir_profile) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
    if (origin == NativeOrigin::Sdk) {
      ++g_native_ir_stats.sdk; g_native_ir_stats.sdk_ns += ns;
      g_native_ir_stats.sdk_command_units += std::holds_alternative<NativeArrayWrite>(operation) ? 2u : 1u;
    }
    else { ++g_native_ir_stats.fifo; g_native_ir_stats.fifo_ns += ns; }
    g_native_ir_stats.in_compatibility = false;
  }
  return true;
}

auto ProcessAuroraCompatibility(const std::uint8_t* bytes, std::uint32_t size) {
  const auto begin = g_native_ir_profile ? std::chrono::steady_clock::now() :
                                         std::chrono::steady_clock::time_point{};
  if (g_native_ir_profile) {
    ++g_native_ir_stats.compatibility;
    if (!g_native_ir_stats.in_compatibility) ++g_native_ir_stats.compatibility_transitions;
    g_native_ir_stats.in_compatibility = true;
  }
  const auto result = aurora::gx::fifo::process(bytes, size);
  if (g_native_ir_profile)
    g_native_ir_stats.compatibility_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
  return result;
}

bool BuildIndexScanLayout(GXVtxFmt fmt, IndexScanLayout* out) {
  using namespace aurora::gx;
  if(!out) return false;
  IndexScanLayout layout{};
  const auto& vf=g_gxState.vtxFmts[fmt];
  std::uint32_t offset=0;

  for(int i=GX_VA_PNMTXIDX;i<=GX_VA_TEX7;++i) {
    const auto desc=g_gxState.vtxDesc[i];
    if(desc==GX_NONE) continue;
    const auto attr=static_cast<GXAttr>(i);
    const auto& af=vf.attrs[i];

    if(desc==GX_DIRECT) {
      offset+=std::uint32_t(comp_type_size(attr,af.type))*comp_cnt_count(attr,af.cnt);
      continue;
    }
    if(desc!=GX_INDEX8 && desc!=GX_INDEX16) return false;

    const std::uint32_t width=desc==GX_INDEX8 ? 1u : 2u;
    const std::uint32_t count=(i==GX_VA_NRM && af.cnt==GX_NRM_NBT3) ? 3u : 1u;
    if(i>=GX_VA_POS) {
      if(layout.field_count>=layout.fields.size() || offset>std::numeric_limits<std::uint16_t>::max())
        return false;
      // NBT3 carries three independent normal indices. Each index addresses one
      // XYZ vector, not a full N/B/T tuple, so the source element is 3
      // components even though a DIRECT NBT vertex contains 9 components.
      const std::uint32_t components=(i==GX_VA_NRM && af.cnt==GX_NRM_NBT3)
                                         ? 3u : comp_cnt_count(attr,af.cnt);
      const std::uint32_t element_size=std::uint32_t(comp_type_size(attr,af.type))*components;
      if(element_size>std::numeric_limits<std::uint16_t>::max()) return false;
      if(!layout.bindings.Add(static_cast<unsigned>(i))) return false;
      layout.fields[layout.field_count++]={
          static_cast<std::uint8_t>(i),static_cast<std::uint8_t>(width),
          static_cast<std::uint8_t>(count),0,
          static_cast<std::uint16_t>(offset),static_cast<std::uint16_t>(element_size)};
    }
    offset+=width*count;
  }

  const std::uint32_t expected=VertexSize(fmt);
  if(offset==0u || offset!=expected || offset>std::numeric_limits<std::uint16_t>::max()) {
    static unsigned logs=0;
    if(logs++<16u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_INDEX_SCAN_V2=0 fmt=%u scan_stride=%u fifo_stride=%u action=reject-layout\n",
                   unsigned(fmt)&7u,offset,expected);
    return false;
  }
  layout.stride=static_cast<std::uint16_t>(offset);
  for(unsigned f=0;f<layout.field_count;++f)
    if(layout.fields[f].index_width==1u) layout.has_index8=1;
  if(layout.field_count>=2u && layout.field_count<=4u) {
    std::array<std::uint16_t,4> offsets{};
    bool eligible=true;
    for(unsigned f=0;f<layout.field_count;++f) {
      const auto& field=layout.fields[f];
      offsets[f]=field.offset;
      eligible=eligible && field.index_width==2u && field.index_count==1u;
    }
    if(eligible) layout.native.Compile(layout.stride,{offsets.data(),layout.field_count});
  }
  if(layout.field_count) {
    std::array<GekkoAOT::GX::NativeIndexBounds::Field,12> fields;
    for(unsigned f=0;f<layout.field_count;++f)
      fields[f]={layout.fields[f].offset,layout.fields[f].index_width,layout.fields[f].index_count};
    if(!layout.index_bounds.Compile(layout.stride,{fields.data(),layout.field_count}))return false;
  }
  if(layout.field_count && layout.field_count<=GekkoAOT::GX::NativeVertexGather::MaxFields) {
    std::array<GekkoAOT::GX::NativeVertexGather::Field,4> fields;
    bool eligible=true;
    for(unsigned f=0;f<layout.field_count;++f) {
      const auto& field=layout.fields[f];
      fields[f]={field.offset,field.element_size};
      eligible=eligible && field.index_width==2u && field.index_count==1u;
    }
    if(eligible) layout.gather.Compile(layout.stride,{fields.data(),layout.field_count});
  }
  for(unsigned minimum=0;minimum<2;++minimum){
    auto& key=layout.range_keys[minimum];
    key.stride=layout.stride;key.count=layout.field_count;key.minimumRequired=minimum;
    for(unsigned f=0;f<layout.field_count;++f){const auto& field=layout.fields[f];
      key.fields[f]={field.offset,field.index_width,field.index_count,field.attr,0};}
  }
  layout.valid=1;
  *out=layout;
  return true;
}

const IndexScanLayout* GetIndexScanLayout(GXVtxFmt fmt) {
  const unsigned index=static_cast<unsigned>(fmt)&7u;
  const auto bit=static_cast<std::uint8_t>(1u<<index);
  if((g_index_scan_valid&bit)!=0u) return &g_index_scan_layout[index];
  IndexScanLayout layout{};
  if(!BuildIndexScanLayout(fmt,&layout)) return nullptr;
  g_index_scan_layout[index]=layout;
  g_index_scan_valid=static_cast<std::uint8_t>(g_index_scan_valid|bit);
  return &g_index_scan_layout[index];
}

bool PrepareIndexedArrayWindows(const std::uint8_t* p, std::size_t len,
                                CompactArrayPlan* compact = nullptr,
                                const GekkoAOT::GX::NativeAttributePlan** bindings = nullptr) {
  using namespace aurora::gx;
  if(!p || len<3u) return false;

  const auto fmt=static_cast<GXVtxFmt>(p[0]&GX_VAT_MASK);
  const std::uint32_t vtx_count=Be16(p+1);
  const auto* layout=GetIndexScanLayout(fmt);
  if(!layout) return false;
  if(bindings) *bindings=&layout->bindings;
  const std::size_t vertex_bytes=std::size_t(vtx_count)*layout->stride;
  if(vertex_bytes!=len-3u) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_INDEX_SCAN_V2=0 reason=draw-size-mismatch stride=%u count=%u bytes=%zu expected=%zu\n",
                 unsigned(layout->stride),vtx_count,vertex_bytes,len-3u);
    return false;
  }
  if(layout->field_count==0u) return true;

  // An indexed draw is a guest-memory visibility boundary. The guest CPU
  // can rewrite an array in MEM1 without issuing GXInvalidateVtxCache and there
  // is no host write notification for Aurora's cachedRange. If we carry that
  // cachedRange into another draw, Aurora can also merge the draw because GX
  // state itself did not change, making the second draw consume the first
  // draw's array snapshot. That produces exactly the transient exploded/off-
  // screen geometry seen when engines recycle scratch vertex buffers.
  //
  // Mark the draw dirty unconditionally when indexed arrays participate so
  // Aurora cannot merge it across this memory-visibility boundary. The actual
  // cached ranges are cleared after the exact per-draw windows are established
  // below, avoiding the old multi-MiB whole-MEM1 upload behaviour.
  if(g_indexed_draw_coherency || g_native_array_snapshots) g_gxState.dirty |= DirtyImmediates;

  // INDEX8 can address at most 256 elements. For the common small SDK arrays
  // it is cheaper to expose that complete bounded window once than to rescan
  // every vertex of every draw just to rediscover the current maximum index.
  // Keep the eager window bounded so large/INDEX16 arrays still use exact
  // per-draw sizing and never recreate the old multi-MiB staging problem.
  constexpr std::uint64_t kEagerIndexWindowBytes=64u*1024u;
  std::array<bool,12> skip_scan{};
  for(std::uint32_t f=0;layout->has_index8 && f<layout->field_count;++f) {
    const auto& field=layout->fields[f];
    if(field.index_width!=1u || (g_native_array_snapshots && compact)) continue;
    auto& array=g_gxState.arrays[field.attr];
    if(!array.data) return false;
    const std::uint64_t full=std::uint64_t(255u)*array.stride+field.element_size;
    const std::uint32_t available=g_array_available[field.attr];
    if(full==0u || full>available || full>kEagerIndexWindowBytes) continue;
    const auto full32=static_cast<std::uint32_t>(full);
    if(array.size<full32) {
      if(g_array_base_trace) ++g_array_base_trace_eager_growth;
      array.size=full32;
      array.cachedRange={};
      g_gxState.dirty |= DirtyImmediates;
    }
    skip_scan[f]=true;
  }

  const auto* const vertices=p+3;

  // Scan only the indexed fields. v1 walked every VCD attribute for every
  // vertex, which made ProcessOne a measurable hot spot. The cached layout
  // turns the inner loop into fixed-offset byte/halfword reads.
  //
  // One pass over the vertices for all scanned fields. Retail draws are
  // short (a few vertices) and a separate loop per field mispredicted at every
  // draw-dependent exit; the common one-INDEX16-per-field layout also drops
  // the per-vertex width/count branches. Results are identical (the minimum
  // is only consumed for INDEX16 compact windows).
  struct ScanState {
    std::uint16_t offset;
    std::uint8_t width;
    std::uint8_t count;
    std::uint8_t attr;
    std::uint32_t maximum;
    std::uint32_t minimum;
  };
  std::array<ScanState,std::tuple_size_v<decltype(layout->fields)>> scan;
  std::uint32_t scan_count=0;
  bool single_index16=true;
  for(std::uint32_t f=0;f<layout->field_count;++f) {
    if(skip_scan[f]) continue;
    const auto& field=layout->fields[f];
    scan[scan_count++]={field.offset,field.index_width,field.index_count,field.attr,0u,65535u};
    single_index16=single_index16 && field.index_width==2u && field.index_count==1u;
  }
  const std::size_t vertex_stride=layout->stride;
  // Fixed field counts unroll the per-vertex work; the vertex loop exit is the
  // only data-dependent branch left on the common one-INDEX16-per-field path.
  const auto scan_index16=[&]<std::uint32_t N>() {
    std::array<std::uint32_t,N> offset{}, maximum{}, minimum{};
    for(std::uint32_t k=0;k<N;++k) {
      offset[k]=scan[k].offset;
      minimum[k]=65535u;
    }
    const auto* base=vertices;
    for(std::uint32_t v=0;v<vtx_count;++v,base+=vertex_stride) {
      for(std::uint32_t k=0;k<N;++k) {
        const auto index=std::uint32_t(Be16(base+offset[k]));
        maximum[k]=std::max(maximum[k],index);
        minimum[k]=std::min(minimum[k],index);
      }
    }
    for(std::uint32_t k=0;k<N;++k) {
      scan[k].maximum=maximum[k];
      scan[k].minimum=minimum[k];
    }
  };
  // CPU-native decode cache: reuse exact bounds for repeated immutable draws.
  // Include skip decisions because eager INDEX8 admission depends on arrays.
  // Only the semantic scan plan belongs in the key. IndexScanLayout also
  // owns execution/gather programs and exceeds the cache's 128-byte limit,
  // so the previous key could never be admitted.
  IndexRangeCacheLayout cache_layout;
  const IndexRangeCacheLayout* cache_key=&layout->range_keys[compact!=nullptr];
  const GekkoAOT::GX::NativeIndexRangeCache::Bounds* cached=nullptr;
  if(g_native_index_range_cache && scan_count) {
    // INDEX16 visibility never changes the scan layout. Compile its exact
    // cache key with VCD/VAT instead of clearing/rebuilding it on every draw.
    // INDEX8 eager windows can omit fields and retain their dynamic key.
    if(layout->has_index8){
      std::memset(&cache_layout,0,sizeof cache_layout);
      cache_layout.stride=layout->stride;cache_layout.count=scan_count;cache_layout.minimumRequired=compact!=nullptr;
      for(unsigned k=0;k<scan_count;++k)cache_layout.fields[k]={scan[k].offset,scan[k].width,scan[k].count,scan[k].attr,0};
      cache_key=&cache_layout;
    }
    cached=g_native_index_ranges.Find(cache_key,sizeof *cache_key,vertices,vertex_bytes);
  }
  if(cached) {
    for(std::uint32_t k=0;k<scan_count;++k) {
      scan[k].minimum=cached->minimum[k];scan[k].maximum=cached->maximum[k];
    }
  } else if(g_native_vertex_program && layout->native.fields==scan_count &&
            layout->native.fields>=2u) {
    std::array<GekkoAOT::GX::NativeVertexProgram::Bounds,4> bounds;
    // Layout and draw-size were proved above. NBT3/mixed formats are not
    // admitted and preserve the existing scan/visibility path below.
    if(!layout->native.Execute({vertices,vertex_bytes},vtx_count,bounds)) return false;
    for(unsigned k=0;k<scan_count;++k) {
      scan[k].minimum=bounds[k].minimum;scan[k].maximum=bounds[k].maximum;
    }
    if(g_native_vertex_profile) {
      ++g_native_vertex_draws;g_native_vertex_vertices+=vtx_count;
    }
  } else if(g_native_index_bounds && scan_count && layout->has_index8) {
    // Mixed INDEX8/INDEX16 and NBT3 previously dispatched width/count inside
    // every vertex/field pair. The immutable VCD/VAT plan now selects each
    // native reduction once per field; array visibility checks below remain
    // per draw, and no command contents or guest mappings are cached.
    std::uint16_t skipped=0;
    for(unsigned f=0;f<layout->field_count;++f)if(skip_scan[f])skipped|=1u<<f;
    std::array<GekkoAOT::GX::NativeIndexBounds::Bounds,12> bounds;
    if(!layout->index_bounds.Execute({vertices,vertex_bytes},vtx_count,skipped,bounds))return false;
    for(unsigned f=0,k=0;f<layout->field_count;++f)if(!skip_scan[f]) {
      scan[k].minimum=bounds[f].minimum;scan[k].maximum=bounds[f].maximum;++k;
    }
  } else if(scan_count!=0u && single_index16) {
    switch(scan_count) {
    case 1u: scan_index16.template operator()<1u>(); break;
    case 2u: scan_index16.template operator()<2u>(); break;
    case 3u: scan_index16.template operator()<3u>(); break;
    case 4u: scan_index16.template operator()<4u>(); break;
    default: {
      const auto* base=vertices;
      for(std::uint32_t v=0;v<vtx_count;++v,base+=vertex_stride) {
        for(std::uint32_t k=0;k<scan_count;++k) {
          const auto index=std::uint32_t(Be16(base+scan[k].offset));
          scan[k].maximum=std::max(scan[k].maximum,index);
          scan[k].minimum=std::min(scan[k].minimum,index);
        }
      }
      break;
    }
    }
  } else if(scan_count!=0u) {
    const auto* base=vertices;
    for(std::uint32_t v=0;v<vtx_count;++v,base+=vertex_stride) {
      for(std::uint32_t k=0;k<scan_count;++k) {
        auto& state=scan[k];
        const auto* q=base+state.offset;
        if(state.width==1u) {
          for(std::uint32_t n=0;n<state.count;++n) {
            const auto index=std::uint32_t(q[n]);
            state.maximum=std::max(state.maximum,index);
            if(compact) state.minimum=std::min(state.minimum,index);
          }
        } else {
          for(std::uint32_t n=0;n<state.count;++n) {
            const auto index=std::uint32_t(Be16(q+n*2u));
            state.maximum=std::max(state.maximum,index);
            if(compact) state.minimum=std::min(state.minimum,index);
          }
        }
      }
    }
  }
  if(!cached && g_native_index_range_cache && scan_count) {
    GekkoAOT::GX::NativeIndexRangeCache::Bounds bounds;
    for(std::uint32_t k=0;k<scan_count;++k) {
      bounds.minimum[k]=scan[k].minimum;bounds.maximum[k]=scan[k].maximum;
    }
    g_native_index_ranges.Insert(cache_key,sizeof *cache_key,vertices,vertex_bytes,bounds);
  }
  static unsigned logs=0;
  std::uint32_t scanned=0;
  for(std::uint32_t f=0;f<layout->field_count;++f) {
    if(skip_scan[f]) continue;
    const auto& field=layout->fields[f];
    const auto& bounds=scan[scanned++];
    const int i=field.attr;
    auto& array=g_gxState.arrays[i];
    if(!array.data) return false;

    const std::uint64_t required=
        std::uint64_t(bounds.maximum)*array.stride+field.element_size;
    const std::uint32_t available=g_array_available[i];
    if(required>available || required>std::numeric_limits<std::uint32_t>::max()) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_ARRAY_WINDOW_V2=0 attr=%d max_index=%u stride=%u required=%llu available=%u action=reject\n",
                   i,bounds.maximum,unsigned(array.stride),
                   static_cast<unsigned long long>(required),available);
      return false;
    }

    const auto required32=static_cast<std::uint32_t>(required);
    if(required32>array.size) {
      if(g_array_base_trace) {
        ++g_array_base_trace_window_growth;
        if(array.cachedRange.size!=0u) {
          ++g_array_base_trace_window_cached;
          g_array_base_trace_window_bytes+=required32;
        }
      }
      array.size=required32;
      array.cachedRange={};
      g_gxState.dirty |= DirtyImmediates;
      if(logs++<32u) {
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ARRAY_WINDOW_V2=1 attr=%d max_index=%u stride=%u bytes=%u available=%u\n",
                     i,bounds.maximum,unsigned(array.stride),required32,available);
      }
    }
    if(compact && (field.index_width==2u || g_native_array_snapshots)) {
      (*compact)[i]=GekkoAOT::GX::MakeCompactArrayWindow(
          bounds.minimum,bounds.maximum,array.stride,field.element_size,
          available,array.size,g_native_array_snapshots);
      if(g_native_array_tiles)
        (*compact)[i]=GekkoAOT::GX::TileCompactArrayWindow((*compact)[i],available,array.size);
    }
  }

  if(g_indexed_draw_coherency || g_native_array_snapshots) {
    bool any=false;
    for(std::uint32_t f=0;f<layout->field_count;++f) {
      const auto attr=layout->fields[f].attr;
      // BuildIndexScanLayout proves one field per attribute (NBT3 keeps its
      // three indices in that single field). No per-draw duplicate map.
      auto& array=g_gxState.arrays[attr];
      if(!g_indexed_draw_coherency && (!g_native_array_snapshots || array.size>8u*1024u)) continue;
      // Clear even when the visible window did not grow: guest RAM may have
      // changed in place since the previous draw. push_gx_draw() will upload
      // the current bytes into a fresh storage range for this draw.
      if(array.cachedRange.size!=0u) {
        array.cachedRange={};
        any=true;
      }
    }
    // DirtyImmediates was set above to block draw merging. Keep it asserted if
    // a snapshot was explicitly invalidated as documentation of the contract.
    if(any) g_gxState.dirty |= DirtyImmediates;
  }
  return true;
}

bool PrepareIndexedXfArrayWindow(const std::uint8_t* p, std::size_t len) {
  using namespace aurora::gx;
  if(!p || len!=5u) return false;

  const std::uint8_t op=p[0]&GX_OPCODE_MASK;
  if(op!=GX_LOAD_INDX_A && op!=GX_LOAD_INDX_B &&
     op!=GX_LOAD_INDX_C && op!=GX_LOAD_INDX_D)
    return false;

  const std::uint32_t array_type=
      GX_POS_MTX_ARRAY+(std::uint32_t(op)-GX_LOAD_INDX_A)/0x08u;
  if(array_type>=GX_VA_MAX_ATTR) return false;

  const std::uint32_t source_index=Be16(p+1);
  const std::uint16_t addr_len=Be16(p+3);
  const std::uint32_t words=(std::uint32_t(addr_len)>>12u)+1u;
  const std::uint32_t source_bytes=words*4u;
  auto& array=g_gxState.arrays[array_type];
  const std::uint32_t available=g_array_available[array_type];

  if(!array.data || available==0u) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_XF_ARRAY_WINDOW_V86=0 op=0x%02x attr=%u index=%u stride=%u words=%u reason=unmapped-array action=reject\n",
                 unsigned(op),array_type,source_index,unsigned(array.stride),words);
    return false;
  }

  const std::uint64_t source_offset=std::uint64_t(source_index)*array.stride;
  const std::uint64_t required=source_offset+source_bytes;
  if(required>available || required>std::numeric_limits<std::uint32_t>::max()) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_XF_ARRAY_WINDOW_V86=0 op=0x%02x attr=%u index=%u stride=%u words=%u required=%llu available=%u reason=outside-mapped-guest-memory action=reject\n",
                 unsigned(op),array_type,source_index,unsigned(array.stride),words,
                 static_cast<unsigned long long>(required),available);
    return false;
  }

  // Aurora validates LOAD_INDX against AttrArray::size before reading the
  // matrix/light source. Gekko exposes only base+stride, so grow exactly to
  // the byte touched by this indexed XF command instead of exposing the rest
  // of MEM1/FakeVMEM (which would also poison storage staging sizes).
  const auto required32=static_cast<std::uint32_t>(required);
  if(array.size<required32) {
    array.size=required32;
    array.cachedRange={};
    g_gxState.dirty |= DirtyImmediates;
    static unsigned logs=0u;
    if(logs++<64u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_XF_ARRAY_WINDOW_V86=1 op=0x%02x attr=%u index=%u stride=%u words=%u bytes=%u available=%u\n",
                   unsigned(op),array_type,source_index,unsigned(array.stride),words,
                   required32,available);
  }
  return true;
}

std::uint32_t NormalizeGuestCacheAddress(std::uint32_t address) noexcept {
  // Preserve the SDK VM aperture; normalize cached/uncached MEM1 aliases to the
  // same physical range used by the rest of the standalone runtime.
  if(address>=0x7e000000u && address<0x80000000u) return address;
  return (address & 0x80000000u)!=0u ? address & 0x3fffffffu : address;
}

bool GuestRangeOverlaps(std::uint32_t address, std::uint32_t size,
                        std::uint32_t base, std::uint64_t bytes) noexcept {
  if(size==0u || bytes==0u) return false;
  const std::uint64_t range_begin=NormalizeGuestCacheAddress(address);
  const std::uint64_t range_end=range_begin+size;
  const std::uint64_t object_begin=NormalizeGuestCacheAddress(base);
  const std::uint64_t object_end=object_begin+bytes;
  return range_begin<object_end && object_begin<range_end;
}

constexpr std::uint32_t PersistentArrayBytesV161H = 64u * 1024u * 1024u;

std::uint32_t AlignPersistentArraySize(std::uint32_t value) noexcept {
  constexpr std::uint32_t kAlign = 256u;
  return (value + kAlign - 1u) & ~(kAlign - 1u);
}

void BeginPersistentArrayFrameV161H(std::uint32_t frame) {
  if (g_persistent_array_frame == frame) return;
  g_persistent_array_frame = frame;
  if (!g_persistent_array_reset_pending) return;

  g_persistent_array_cursor = static_cast<std::uint32_t>(aurora::gfx::StorageBufferSize);
  for (auto& slot : g_persistent_array_slots) slot = {};
  for (auto& array : aurora::gx::g_gxState.arrays) array.cachedRange = {};
  aurora::gx::g_gxState.dirty |= aurora::gx::DirtyImmediates;
  g_persistent_array_reset_pending = false;
  std::fprintf(stderr,
               "GEKKOAOT_AURORA_ARRAY_CACHE_V178=1 action=reset frame=%u base=%u bytes=%u\n",
               frame, static_cast<unsigned>(aurora::gfx::StorageBufferSize),
               PersistentArrayBytesV161H);
}

bool UploadPersistentArrayV161H(std::uint32_t attr, std::uint32_t frame) {
  using namespace aurora::gx;
  if (!g_persistent_array_cache || attr >= GX_VA_MAX_ATTR) return false;
  auto& array = g_gxState.arrays[attr];
  // The upper storage region survives frame rotation and guest-memory
  // writes already invalidate cachedRange through NotifyGuestMemoryWriteRange.
  // Promote medium/static indexed arrays too: otherwise the same 8-64 KiB
  // window is memcpy'd through push_storage every frame. Tiny scratch arrays
  // stay on mapped per-frame staging where Queue.WriteBuffer would cost more.
  if (array.size < g_persistent_array_min_bytes) {
    ++g_persistent_array_fallbacks;
    return false;
  }
  if (array.cachedRange.size != 0u) {
    ++g_persistent_array_hits;
    return true;
  }
  if (array.data == nullptr || array.size == 0u) return false;

  const std::uint32_t padded = AlignPersistentArraySize(array.size);
  const std::uint32_t base = static_cast<std::uint32_t>(aurora::gfx::StorageBufferSize);
  const std::uint32_t limit = base + PersistentArrayBytesV161H;
  auto& slot = g_persistent_array_slots[attr];
  // Guest invalidation remains authoritative. Once invalidated, exact byte
  // equality can prove that the previous immutable GPU snapshot is still the
  // right one. No hash or address/SDK assumption can admit different data.
  if (g_content_array_cache && slot.snapshot && slot.offset >= base &&
      slot.capacity >= padded && slot.snapshot_size >= array.size &&
      std::memcmp(slot.snapshot.get(), array.data, array.size) == 0) {
    array.cachedRange = {slot.offset, array.size};
    slot.last_frame = frame;
    ++g_content_array_hits;
    return true;
  }
  if (g_adaptive_array_upload && !g_array_upload_policy[attr].UsePersistent(frame)) {
    ++g_persistent_array_fallbacks;
    ++g_adaptive_array_fallbacks;
    return false;
  }
  const bool reusable = slot.offset >= base && slot.capacity >= padded &&
                        slot.last_frame != frame;
  std::uint32_t offset = reusable ? slot.offset : g_persistent_array_cursor;

  if (!reusable) {
    if (padded > PersistentArrayBytesV161H || offset > limit || padded > limit - offset) {
      ++g_persistent_array_fallbacks;
      g_persistent_array_reset_pending = true;
      return false;
    }
    g_persistent_array_cursor = offset + padded;
  }

  const std::size_t write_size =
      (static_cast<std::size_t>(array.size) + 3u) & ~std::size_t{3u};
  // All payload bytes are overwritten; initialize only the 0-3 copy-alignment
  // padding bytes. The slot and queued upload share this immutable snapshot.
  std::shared_ptr<std::uint8_t[]> snapshot(new std::uint8_t[write_size]);
  std::memcpy(snapshot.get(), array.data, array.size);
  std::memset(snapshot.get() + array.size, 0, write_size - array.size);
  auto storage = aurora::gfx::detail::resources().storageBuffer;

  aurora::gfx::render_worker::enqueue_work(
      [storage = std::move(storage), offset, snapshot, write_size]() mutable {
        aurora::webgpu::g_queue.WriteBuffer(storage, offset, snapshot.get(), write_size);
      });

  slot.offset = offset;
  slot.capacity = padded;
  slot.identity = array.data;
  slot.last_frame = frame;
  slot.snapshot = std::move(snapshot);
  slot.snapshot_size = array.size;
  g_array_upload_policy[attr].Uploaded(frame);
  array.cachedRange = {offset, array.size};
  ++g_persistent_array_uploads;
  g_persistent_array_upload_bytes += array.size;

  if (g_persistent_array_uploads <= 32u) {
    std::fprintf(stderr,
                 "GEKKOAOT_AURORA_ARRAY_CACHE_V178=1 action=upload attr=%u frame=%u offset=%u bytes=%u cap=%u reused=%u\n",
                 attr, frame, offset, array.size, padded, reusable ? 1u : 0u);
  }
  return true;
}

void PrimePersistentIndexedArraysV161H(std::uint32_t compact_mask,
                                     const GekkoAOT::GX::NativeAttributePlan& bindings) {
  using namespace aurora::gx;
  if (!g_persistent_array_cache) return;
  const auto frame = aurora::gfx::current_frame();
  // Recycle before ANY draw in the new frame uses an old persistent range.
  // A delayed reset after a cached draw would let an upload overwrite it.
  BeginPersistentArrayFrameV161H(frame);
  bindings.ForEach(g_native_attribute_plan,[&](unsigned i) {
    if((compact_mask & (1u << i))!=0u) return;
    const auto desc = g_gxState.vtxDesc[i];
    if (desc != GX_INDEX8 && desc != GX_INDEX16) return;
    auto& array = g_gxState.arrays[i];
    if (array.cachedRange.size != 0u) {
      auto& slot = g_persistent_array_slots[i];
      // A surviving snapshot can be used in a later frame without an upload.
      // Record that use: a subsequent mutation in this SAME frame must receive
      // another GPU range, or it would change geometry of the earlier draw.
      if (array.cachedRange.offset >= aurora::gfx::StorageBufferSize &&
          array.cachedRange.offset == slot.offset)
        slot.last_frame = frame;
      return;
    }
    if (array.size == 0u || array.data == nullptr) return;
    (void)UploadPersistentArrayV161H(static_cast<std::uint32_t>(i), frame);
  });
}

std::uint32_t SelectCompactArrayMask(const CompactArrayPlan& plan,
                                   const GekkoAOT::GX::NativeAttributePlan& bindings) {
  using namespace aurora::gx;
  std::uint32_t mask=0;
  bindings.ForEach(g_native_attribute_plan,[&](unsigned attr) {
    if(plan[attr] && g_gxState.arrays[attr].cachedRange.size==0u)
      mask|=1u<<attr;
  });
  return mask;
}
