"""Keep native EFB color peeks functional at WebGPU Compatibility feature level."""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'lib/gfx/color_peek.cpp'
text = path.read_text()
marker = 'GEKKOAOT_COLOR_PEEK_RASTER'
if marker in text:
    sys.exit(0)


def replace(before, after):
    global text
    if text.count(before) != 1:
        raise RuntimeError('Color peek raster anchor changed: ' + before[:100])
    text = text.replace(before, after)


replace('#include "color_peek.hpp"', '#include "color_peek.hpp"\n#include "gekkoaot_color_peek_raster.hpp"\n// ' + marker)
replace('struct Slot {\n  wgpu::Buffer storageBuffer;',
        'struct Slot {\n  GekkoAOT::GX::ColorPeek::Snapshot raster;\n  wgpu::Buffer storageBuffer;')
replace('wgpu::ComputePipeline g_pipeline;', 'wgpu::ComputePipeline g_pipeline;\nwgpu::RenderPipeline g_rasterPipeline;')
replace('  const uint64_t byteSize = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * sizeof(uint32_t);\n'
        '  if (slot.storageBuffer && slot.width == width && slot.height == height && slot.byteSize == byteSize)',
        '''  const auto rasterExtent = GekkoAOT::GX::ColorSnapshotExtent(width, height);
  if (!rasterExtent) return false;
  const uint64_t byteSize = webgpu::g_hasCoreFeatures ? uint64_t(width) * height * sizeof(uint32_t)
                                                    : rasterExtent->byte_size;
  if ((slot.storageBuffer || slot.raster.texture) && slot.width == width && slot.height == height && slot.byteSize == byteSize)''')
replace('  slot.storageBuffer = {};', '  slot.raster = {};\n  slot.storageBuffer = {};')
replace('  slot.storageBuffer = g_device.CreateBuffer(&storageDescriptor);',
        '''  if (webgpu::g_hasCoreFeatures) slot.storageBuffer = g_device.CreateBuffer(&storageDescriptor);
  else if (!slot.raster.Create(g_device, width, height)) return false;''')
replace('  return slot.storageBuffer && slot.readbackBuffer && slot.paramsBuffer;',
        '  return (slot.storageBuffer || slot.raster.texture) && slot.readbackBuffer && slot.paramsBuffer;')
replace('GetConstMappedRange(0, valueCount * sizeof(uint32_t))', 'GetConstMappedRange(0, slot.byteSize)')
replace('      g_latest.data.assign(mapped, mapped + valueCount);',
        '''      if (slot.raster.texture) {
        g_latest.data.resize(valueCount);
        for (uint32_t y = 0; y < slot.height; ++y)
          std::copy_n(mapped + size_t(y) * (slot.raster.layout.row_pitch / 4u), slot.width,
                      g_latest.data.data() + size_t(y) * slot.width);
      } else g_latest.data.assign(mapped, mapped + valueCount);''')
replace('''  if (!webgpu::g_hasCoreFeatures) {
    return;
  }
  g_bindGroupLayout = create_bind_group_layout("Color Peek Bind Group Layout");
  g_pipeline = create_pipeline(g_bindGroupLayout, "Color Peek Pipeline");''',
        '''  if (webgpu::g_hasCoreFeatures) {
    g_bindGroupLayout = create_bind_group_layout("Color Peek Bind Group Layout");
    g_pipeline = create_pipeline(g_bindGroupLayout, "Color Peek Pipeline");
  } else {
    g_bindGroupLayout = GekkoAOT::GX::ColorPeek::CreateLayout(g_device);
    g_rasterPipeline = GekkoAOT::GX::ColorPeek::CreatePipeline(g_device, g_bindGroupLayout);
  }''')
replace('  g_pipeline = {};', '  g_pipeline = {};\n  g_rasterPipeline = {};')
# This declaration is in encode_frame_snapshot, not Slot after the edit above.
replace('  uint64_t byteSize = 0;\n  {', '  uint64_t byteSize = 0;\n  GekkoAOT::GX::ColorPeek::Snapshot raster;\n  {')
replace('    byteSize = slot->byteSize;', '    byteSize = slot->byteSize;\n    raster = slot->raster;')
replace('  g_queue.WriteBuffer(paramsBuffer, 0, &params, sizeof(params));',
        '''  g_queue.WriteBuffer(paramsBuffer, 0, &params, sizeof(params));
  if (raster.texture) {
    raster.Encode(g_device, cmd, g_rasterPipeline, g_bindGroupLayout, colorView, paramsBuffer, sizeof(params),
                  readbackBuffer, dstSize.x, dstSize.y);
    return;
  }''')
path.write_text(text)
