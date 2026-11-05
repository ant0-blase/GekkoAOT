"""Memoized current render pass lookup (v372).

RenderPass is ~7.5 KB, so std::deque<RenderPass> holds one pass per node and
every `current_render_passes()[g_recorder.currentRenderPass]` is an
out-of-line iterator walk. The recorder evaluates it several times per draw
and per BP state write (Star Fox Adventures ThornTail: 7.8% of the GX thread).

Within one recorded frame the pass list only grows at its back, and deque
growth never moves an existing pass. suspend_efb's pop_back is the only
removal and it forgets the memo, so the list, frame id and index identify
the same pass object.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
MARKER = 'GEKKOAOT_CURRENT_PASS_MEMO_V372B'
LOOKUP = 'current_render_passes()[g_recorder.currentRenderPass]'

path = root / 'lib/gfx/recording.cpp'
text = path.read_text()
OLD_BLOCK_START = '\n// GEKKOAOT_CURRENT_PASS_MEMO_V372: '
OLD_BLOCK_END = '  memo = {&passes, frame.frameId, index, size, &passes.back(), &pass};\n  return pass;\n}\n'
if MARKER not in text and OLD_BLOCK_START in text:
    # First v372 revision (size/back key) applied in place: drop its block and
    # restore the original lookups so the current revision applies cleanly.
    start = text.index(OLD_BLOCK_START)
    end = text.index(OLD_BLOCK_END, start) + len(OLD_BLOCK_END)
    text = text[:start] + text[end:]
    text = text.replace('recorder_current_pass()', LOOKUP)
if MARKER not in text:
    anchor = 'RenderPassList& current_render_passes() { return g_recorder.passes(); }\n'
    if text.count(anchor) != 1:
        raise RuntimeError('recording.cpp: current pass memo anchor changed')
    head, tail = text.split(anchor)
    if LOOKUP in head:
        raise RuntimeError('recording.cpp: current pass used before current_render_passes()')
    uses = tail.count(LOOKUP)
    if uses < 10:
        raise RuntimeError(f'recording.cpp: expected the current pass lookups, found {uses}')
    tail = tail.replace(LOOKUP, 'recorder_current_pass()')
    pop = '    current_render_passes().pop_back();\n'
    if tail.count(pop) != 1:
        raise RuntimeError('recording.cpp: current pass memo pop_back anchor changed')
    tail = tail.replace(pop, pop + '    g_currentPassMemo = {};\n')
    memo = anchor + '''
// ''' + MARKER + ''': one pass per deque node makes indexing an out-of-line
// iterator walk. Growth at the back never moves a pass and the only removal
// (suspend_efb) forgets the memo, so list + frame + index identify the pass.
struct CurrentPassMemo {
  const RenderPassList* passes = nullptr;
  uint64_t frameId = 0;
  uint32_t index = UINT32_MAX;
  RenderPass* pass = nullptr;
};
CurrentPassMemo g_currentPassMemo;

RenderPass& recorder_current_pass() {
  auto& frame = g_recorder.frame();
  auto& passes = frame.renderPasses;
  const uint32_t index = g_recorder.currentRenderPass;
  auto& memo = g_currentPassMemo;
  if (memo.passes == &passes && memo.frameId == frame.frameId && memo.index == index) {
    return *memo.pass;
  }
  auto& pass = passes[index];
  memo = {&passes, frame.frameId, index, &pass};
  return pass;
}
'''
    path.write_text(head + memo + tail)
