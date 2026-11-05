"""Opt-in nonblocking contribution samples and real per-pass GPU timestamps."""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_ENHANCED_MEASUREMENT_V1';updates={}
def edit(name,changes):
    path=root/name;source=path.read_text()
    if marker in source:return
    for old,new in changes:
        if source.count(old)!=1:raise RuntimeError(f'{name}: measurement anchor changed: {old[:80]}')
        source=source.replace(old,new)
    updates[path]='// '+marker+'\n'+source
edit('lib/gfx/frame.cpp',[
    ('#include "frame.hpp"','#include "frame.hpp"\n#include "gekkoaot_enhanced_measurement.hpp"'),
    ('void after_submit() noexcept {','void after_submit() noexcept {\n  enhanced::measurement::after_submit();')])
edit('lib/webgpu/gpu.cpp',[
    ('#ifdef TRACY_ENABLE\n      if (feature == wgpu::FeatureName::TimestampQuery)',
     '#if 1 // Timestamp queries are also requested for opt-in enhancement measurements.\n'
     '      if (feature == wgpu::FeatureName::TimestampQuery &&\n'
     '          (std::getenv("GEKKOAOT_ENHANCED_MEASURE") != nullptr\n'
     '#ifdef TRACY_ENABLE\n            || true\n#endif\n          ))')])
for path,source in updates.items():path.write_text(source)
