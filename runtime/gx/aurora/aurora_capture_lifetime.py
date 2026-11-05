"""Expire derived camera metadata when its backing guest depth expires.

Color-only retention across display frames must not retain camera IDs after
GX cleared Z. Same-frame depth slices (world -> weapon) are unaffected.
"""
from pathlib import Path
import sys
p=Path(sys.argv[1])/'lib/gfx/recording.cpp'
s=p.read_text()
marker='GEKKOAOT_CAPTURE_LIFETIME_V1'
if marker not in s:
    anchor='GekkoAOT::GX::SceneCaptureLayers g_enhancedLastCapture;'
    assert s.count(anchor)==1
    s=s.replace(anchor,anchor+'\nwgpu::Texture g_enhancedLastCaptureTexture;')
    anchor='  if ((preserveEfb & 1u) != 0u) pass.sceneCapture=g_enhancedLastCapture;'
    assert s.count(anchor)==1
    s=s.replace(anchor,'''  // A new display frame can retain color while GX clears guest Z. Keeping
  // the old derived depth in that case projects an old scene as a ghost and
  // fills the camera-ID table with every aim/viewport encountered over time.
  // Retention inside the SAME display frame is handled by continuation passes.
  const bool sameCaptureTarget=g_enhancedLastCaptureTexture.Get()==pass.copySourceCaptureTexture.Get();
  const bool retainCapture=(preserveEfb&3u)==3u && sameCaptureTarget;
  if(retainCapture)pass.sceneCapture=g_enhancedLastCapture;
  else {
    for(unsigned i=0;i<pass.colorAttachmentCount;++i){auto& attachment=pass.colorAttachments[i];
      if(attachment.semantic==ColorAttachmentSemantic::Normal ||
         attachment.semantic==ColorAttachmentSemantic::SceneCapture ||
         attachment.semantic==ColorAttachmentSemantic::SceneContribution){
        attachment.clear=true;attachment.loadOp=wgpu::LoadOp::Clear;attachment.clearValue={};
      }
    }
    enhanced::note_capture_cleared();
    gx::g_gxState.dirty|=gx::DirtyUniform;
  }
  static unsigned captureLogs=0;
  if(captureLogs++<8 || (packet.frameId%300u)==0u)
    std::fprintf(stderr,"GEKKOAOT_CAPTURE_LIFETIME frame=%llu retained=%u color=%u depth=%u same_target=%u views=%u\\n",
      (unsigned long long)packet.frameId,unsigned(retainCapture),unsigned((preserveEfb&1u)!=0),
      unsigned((preserveEfb&2u)!=0),unsigned(sameCaptureTarget),pass.sceneCapture.count);''')
    anchor='    g_enhancedLastCapture=pass.sceneCapture;'
    assert s.count(anchor)==1
    s=s.replace(anchor,anchor+'\n    g_enhancedLastCaptureTexture=pass.copySourceCaptureTexture;')
    p.write_text('// '+marker+'\n'+s)

# A frame may retain Z at display-copy time, then clear it BEFORE its first
# receiver draw. That starts a new scene too; a later world -> weapon clear
# must retain the world's derived depth.
s=p.read_text()
if 'GEKKOAOT_CAPTURE_LIFETIME_V2' not in s:
    anchor='wgpu::Texture g_enhancedLastCaptureTexture;'
    assert s.count(anchor)==1
    s=s.replace(anchor,anchor+'\nbool g_enhancedFrameReceivers=false;')
    anchor='  g_recorder.drawCallCount = 0;'
    assert s.count(anchor)==1
    s=s.replace(anchor,anchor+'\n  g_enhancedFrameReceivers=false;')
    anchor='  auto& pass=recorder_current_pass();\n  if(reason<pass.enhancedReasons.size())'
    assert s.count(anchor)==1
    s=s.replace(anchor,'  auto& pass=recorder_current_pass();\n  if(reason==0u)g_enhancedFrameReceivers=true;\n  if(reason<pass.enhancedReasons.size())')
    old='  if(clearColor && fullTarget){newPass.sceneCapture={};enhanced::note_capture_cleared();}'
    new='''  const bool retireCapture=fullTarget && (clearColor || (clearDepth && !g_enhancedFrameReceivers));
  if(retireCapture){newPass.sceneCapture={};enhanced::note_capture_cleared();gx::g_gxState.dirty|=gx::DirtyUniform;}'''
    assert s.count(old)==1
    s=s.replace(old,new)
    old='((clearDepth && !webgpu::g_graphicsConfig.enhancedCapture) || clearColor)'
    assert s.count(old)==1
    s=s.replace(old,'((clearDepth && !webgpu::g_graphicsConfig.enhancedCapture) || clearColor || retireCapture)')
    old='semantic==ColorAttachmentSemantic::SceneContribution) && clearColor)'
    assert s.count(old)==1
    s=s.replace(old,'semantic==ColorAttachmentSemantic::SceneContribution) && (clearColor || retireCapture))')
    p.write_text('// GEKKOAOT_CAPTURE_LIFETIME_V2\n'+s)

# GXCopyDisp is a producer boundary, not necessarily a new world image.
# Frontline can publish the world, clear Z, then publish its view model. Wait
# for the next receiver lens before retiring retained world metadata.
s=p.read_text()
if 'GEKKOAOT_CAPTURE_LIFETIME_V3' not in s:
    s=s.replace('bool g_enhancedFrameReceivers=false;','bool g_enhancedFrameReceivers=false;\nbool g_enhancedRetirePending=false;')
    s=s.replace('const bool retainCapture=(preserveEfb&3u)==3u && sameCaptureTarget;','const bool retainCapture=(preserveEfb&1u)!=0u && sameCaptureTarget;\n  g_enhancedRetirePending=retainCapture && (preserveEfb&2u)==0u;')
    old='''  return recorder_current_pass().sceneCapture.Observe(capture);'''
    new='''  auto& pass=recorder_current_pass();
  if(g_enhancedRetirePending && capture.validity==GekkoAOT::GX::SceneValidity::Valid){
    float worldNear=0;
    for(unsigned i=0;i<pass.sceneCapture.count;++i){const auto& old=pass.sceneCapture.views[i];
      if(old.viewport==capture.viewport)worldNear=std::max(worldNear,GekkoAOT::GX::NearDistance(old));
    }
    // A near-only view model continues the retained world. A new world lens,
    // including a changed aim FOV, starts a fresh derived capture instead.
    const bool newWorld=worldNear<=0 || GekkoAOT::GX::NearDistance(capture)>=worldNear*.25f;
    g_enhancedRetirePending=false;
    if(newWorld){
      pass.sceneCapture={};
      for(unsigned i=0;i<pass.colorAttachmentCount;++i){auto& a=pass.colorAttachments[i];
        if(a.semantic==ColorAttachmentSemantic::Normal || a.semantic==ColorAttachmentSemantic::SceneCapture ||
           a.semantic==ColorAttachmentSemantic::SceneContribution){a.clear=true;a.loadOp=wgpu::LoadOp::Clear;a.clearValue={};}
      }
      enhanced::note_capture_cleared();
    }
  }
  return pass.sceneCapture.Observe(capture);'''
    assert s.count(old)==1;s=s.replace(old,new)
    old='const bool retireCapture=fullTarget && (clearColor || (clearDepth && !g_enhancedFrameReceivers));'
    new='''const bool retireCapture=fullTarget && clearColor;
  if(fullTarget && clearDepth && !g_enhancedFrameReceivers)g_enhancedRetirePending=true;
  if(retireCapture)g_enhancedRetirePending=false;'''
    assert s.count(old)==1;s=s.replace(old,new)
    p.write_text('// GEKKOAOT_CAPTURE_LIFETIME_V3\n'+s)
