"""Feed environment lighting evidence from every GX FIFO draw.

The enhanced renderer weights each receiver's model-view rotation by its
vertex count; only draw_prim knows that count. Applied after the capture
coverage overlay (which inserts begin_draw); every anchor is preflighted.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = 'GEKKOAOT_ENVIRONMENT_LIGHTING_V1'
updates = {}
def edit(name, changes):
    path = root / name
    source = path.read_text()
    if marker in source:
        return
    for old, new in changes:
        if source.count(old) != 1:
            raise RuntimeError(f'{name}: environment lighting anchor changed ({source.count(old)})')
        source = source.replace(old, new)
    updates[path] = '// ' + marker + '\n' + source

edit('lib/gx/command_processor.cpp', [(
    '  gfx::enhanced::begin_draw();\n  gfx::enhanced::observe_draw();\n',
    '  gfx::enhanced::begin_draw();\n  gfx::enhanced::observe_draw();\n'
    '  gfx::enhanced::observe_environment(vtxCount);\n')])
for path, source in updates.items():
    path.write_text(source)
