"""A small clear under a copy that drew no surface keeps the Enhanced receivers.

GoldenEye's bloom saves the top-left quarter, clears it, draws its glow there and
restores the color before presenting. The partial clear draw wiped normal, capture
and contribution of that quarter, so the restored corner lost every effect.
"""
from pathlib import Path
import sys
root=Path(sys.argv[1])
marker='GEKKOAOT_WORK_AREA_CLEAR_V394'
def edit(name,pairs):
    p=root/name
    s=p.read_text()
    if marker in s:return
    for old,new,count in pairs:
        if s.count(old)!=count:raise RuntimeError(f'{name}: anchor changed {old[:80]} ({s.count(old)}/{count})')
        s=s.replace(old,new)
    p.write_text('// '+marker+'\n'+s)
edit('lib/gfx/clear.hpp',[
 ('ClearPipelineConfigVersion = 5;','ClearPipelineConfigVersion = 6;',1),
 ('  uint8_t _pad = 0;\n','  bool keepReceivers = false;\n',1),
])
normal='((config.clearDepth && !webgpu::g_graphicsConfig.enhancedCapture) || config.clearColor) && layout.colorAttachments[i].semantic == ColorAttachmentSemantic::Normal'
edit('lib/gfx/clear.cpp',[
 (normal,'!config.keepReceivers && '+normal,2),
 ('  if(config.clearColor)for(uint32_t i=0;i<layout.colorAttachmentCount;++i)','  if(config.clearColor && !config.keepReceivers)for(uint32_t i=0;i<layout.colorAttachmentCount;++i)',1),
 ('colorTargets[i].writeMask=config.clearColor?wgpu::ColorWriteMask::All:wgpu::ColorWriteMask::None;','colorTargets[i].writeMask=config.clearColor && !config.keepReceivers?wgpu::ColorWriteMask::All:wgpu::ColorWriteMask::None;',1),
])
edit('lib/gfx/recording.cpp',[
 ('  const bool fullColorClear = clearColor && clearAlpha && fullTarget;\n',
  '  const bool fullColorClear = clearColor && clearAlpha && fullTarget;\n'
  '  const bool workAreaClear=!fullTarget && clearColor && enhanced::keeps_receivers_under_clear(prevPass,clearRect.width,clearRect.height);\n',1),
 ('            .clearDepth = drawClearDepth,\n','            .clearDepth = drawClearDepth,\n            .keepReceivers = workAreaClear,\n',1),
])
