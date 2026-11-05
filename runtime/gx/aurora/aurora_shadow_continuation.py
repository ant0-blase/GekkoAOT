"""Keep one cascade atlas while an EFB scene continues across copy boundaries."""
from pathlib import Path
import sys

p = Path(sys.argv[1]) / 'lib/gfx/frame_packet.hpp'
s = p.read_text()
if 'bool shadowLoadPrevious' not in s:
    anchor = '  uint32_t shadowResolution=0, shadowCascades=0;'
    if s.count(anchor) != 1:
        raise RuntimeError('shadow continuation: RenderPass anchor changed')
    p.write_text(s.replace(anchor, anchor + '\n  bool shadowLoadPrevious=false;'))
