"""Low-latency VSync: Mailbox before FIFO (v384).

With VSync on, Aurora presented with FifoRelaxed. A game paced at the display
rate keeps that queue full, so every frame waited one to two extra refreshes
(16-33 ms at 60 Hz) between submission and scanout. Mailbox also presents
only at vertical blank (no tearing) but replaces the queued image, so the
newest frame is shown at the next refresh. Presents stay paced: guest frames
arrive at the game's own rate, and frame interpolation already limits itself
to one present per display refresh under VSync.

GEKKOAOT_LOW_LATENCY_VSYNC=0 restores FifoRelaxed.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
MARKER = 'GEKKOAOT_LOW_LATENCY_VSYNC_V384'
path = root / 'lib/webgpu/gpu.cpp'
text = path.read_text()
if MARKER not in text:
    old = '''  if (vsync_enabled()) {
    if (supports(wgpu::PresentMode::FifoRelaxed)) {
      return wgpu::PresentMode::FifoRelaxed;
    }
  } else {'''
    new = '''  if (vsync_enabled()) {
    // ''' + MARKER + ''': Mailbox shows the newest frame at the next vblank
    // without tearing; a full FIFO queue adds one to two refreshes of latency.
    static const bool lowLatency = [] {
      const char* value = std::getenv("GEKKOAOT_LOW_LATENCY_VSYNC");
      return !value || value[0] != '0';
    }();
    if (lowLatency && g_backendType != wgpu::BackendType::Metal && supports(wgpu::PresentMode::Mailbox)) {
      return wgpu::PresentMode::Mailbox;
    }
    if (supports(wgpu::PresentMode::FifoRelaxed)) {
      return wgpu::PresentMode::FifoRelaxed;
    }
  } else {'''
    if text.count(old) != 1:
        raise RuntimeError('gpu.cpp: low-latency vsync anchor changed')
    text = text.replace(old, new)
    if '#include <cstdlib>' not in text:
        text = '#include <cstdlib>\n' + text
    path.write_text(text)
