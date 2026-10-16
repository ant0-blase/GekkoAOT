"""Take CPU EFB peek snapshots from the frame resolved by GXCopyDisp (v349).

GXPeekZ/GXPeekARGB read an asynchronous snapshot taken when a render pass
ends. finish() used the last pass of the recording, but GekkoAOT ends each
recording right after GXCopyDisp, whose resolve already started a fresh EFB
pass with the guest's clear. Titles that clear on the display copy therefore
always peeked an empty EFB (Killer7's aim test saw nothing under the laser
sight, so its shots never registered). The display copy now marks the pass it
resolves; finish() keeps the old behaviour only for recordings without one.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
MARKER = 'GEKKOAOT_PEEK_SNAPSHOT_V349'


def patch(relative, edits):
    path = root / relative
    text = path.read_text()
    if MARKER in text:
        return
    for before, after in edits:
        if text.count(before) != 1:
            raise RuntimeError(f'{relative}: peek snapshot anchor changed: {before[:100]}')
        text = text.replace(before, after)
    path.write_text(text)


patch('lib/gfx/recording.hpp', [
    ('void gekkoaot_preserve_efb_on_next_begin(bool preserveColor, bool preserveDepth) noexcept;',
     'void gekkoaot_preserve_efb_on_next_begin(bool preserveColor, bool preserveDepth) noexcept;\n'
     '// ' + MARKER + ': snapshot CPU EFB peeks from the pass a display copy resolves.\n'
     'void gekkoaot_capture_peek_snapshot() noexcept;'),
])

patch('lib/gfx/recording.cpp', [
    ('uint32_t g_gekkoaotPreserveEfbOnNextBegin = 0u;',
     'uint32_t g_gekkoaotPreserveEfbOnNextBegin = 0u;\n'
     '// ' + MARKER + '\n'
     'bool g_gekkoaotPeekSnapshotMarked = false;'),
    ('''void gekkoaot_preserve_efb_on_next_begin(bool preserveColor, bool preserveDepth) noexcept {''',
     '''void gekkoaot_capture_peek_snapshot() noexcept {
  if (!g_recorder.active() || g_recorder.inOffscreen || g_recorder.currentRenderPass == UINT32_MAX) {
    return;
  }
  current_render_passes()[g_recorder.currentRenderPass].captureDepthSnapshot = true;
  g_gekkoaotPeekSnapshotMarked = true;
}

void gekkoaot_preserve_efb_on_next_begin(bool preserveColor, bool preserveDepth) noexcept {'''),
    ('''    auto& pass = frame.renderPasses[g_recorder.currentRenderPass];
    pass.captureDepthSnapshot = true;
    enqueue_pass(frame, g_recorder.currentRenderPass);''',
     '''    auto& pass = frame.renderPasses[g_recorder.currentRenderPass];
    if (!gekkoaotPeekMarked) {
      pass.captureDepthSnapshot = true;
    }
    enqueue_pass(frame, g_recorder.currentRenderPass);'''),
    ('''  AURORA_ASSERT(!g_recorder.inOffscreen, "finish called while offscreen rendering is active");
  if (g_recorder.currentRenderPass != UINT32_MAX) {''',
     '''  AURORA_ASSERT(!g_recorder.inOffscreen, "finish called while offscreen rendering is active");
  const bool gekkoaotPeekMarked = std::exchange(g_gekkoaotPeekSnapshotMarked, false);
  if (g_recorder.currentRenderPass != UINT32_MAX) {'''),
])
