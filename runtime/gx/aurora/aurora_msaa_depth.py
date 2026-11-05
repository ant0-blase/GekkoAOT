"""Keep guest depth copies/peeks available with host MSAA (sample zero)."""
from pathlib import Path
import sys
root=Path(sys.argv[1])
def patch(path,marker,edits):
 p=root/path;s=p.read_text()
 if marker in s:return
 for a,b in edits:
  if s.count(a)!=1:raise RuntimeError(f'{path}: MSAA depth anchor changed: {a[:70]}')
  s=s.replace(a,b)
 p.write_text('// '+marker+'\n'+s)
patch('lib/gfx/tex_copy_conv.hpp','GEKKOAOT_DEPTH_MS_REQUEST',[
 ('  SampleFilter sampleFilter = SampleFilter::Nearest;','  SampleFilter sampleFilter = SampleFilter::Nearest;\n  bool multisampled = false;')])
patch('lib/gfx/encoding.cpp','GEKKOAOT_DEPTH_MS_ENCODING',[
 ('''    if (isDepth && passInfo.msaaSamples > 1) {
      Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
    }
''',''),
 ('        .sampleFilter = needsScaling ? tex_copy_conv::SampleFilter::Linear : tex_copy_conv::SampleFilter::Nearest,',
  '        .sampleFilter = needsScaling ? tex_copy_conv::SampleFilter::Linear : tex_copy_conv::SampleFilter::Nearest,\n        .multisampled = isDepth && passInfo.msaaSamples > 1,')])
patch('lib/gfx/tex_copy_conv.cpp','GEKKOAOT_DEPTH_MS_CONVERSION',[
 ('static wgpu::BindGroupLayout g_depthBindGroupLayout;', 'static wgpu::BindGroupLayout g_depthBindGroupLayout;\nstatic wgpu::BindGroupLayout g_depthBindGroupLayoutMS;\nstatic absl::flat_hash_map<GXTexFmt, wgpu::RenderPipeline> g_depthPipelinesMS;'),
 ('  g_depthBindGroupLayout = g_device.CreateBindGroupLayout(&depthBindGroupLayoutDescriptor);',
 '''  g_depthBindGroupLayout = g_device.CreateBindGroupLayout(&depthBindGroupLayoutDescriptor);
  auto msEntries = depthBindGroupLayoutEntries;
  msEntries[0].texture.multisampled = true;
  auto msDescriptor = depthBindGroupLayoutDescriptor;
  msDescriptor.entries = msEntries.data();
  g_depthBindGroupLayoutMS = g_device.CreateBindGroupLayout(&msDescriptor);
  auto depthMS = DepthShaderPreamble;
  depthMS.replace(depthMS.find("texture_depth_2d"), std::string("texture_depth_2d").size(), "texture_depth_multisampled_2d");'''),
 ('      g_pipelines[conv.fmt] = create_pipeline(conv, DepthShaderPreamble, g_depthBindGroupLayout);',
  '      g_pipelines[conv.fmt] = create_pipeline(conv, DepthShaderPreamble, g_depthBindGroupLayout);\n      g_depthPipelinesMS[conv.fmt] = create_pipeline(conv, depthMS, g_depthBindGroupLayoutMS);'),
 ('  g_depthBindGroupLayout = {};', '  g_depthBindGroupLayout = {};\n  g_depthBindGroupLayoutMS = {};\n  g_depthPipelinesMS.clear();'),
 ('        .layout = g_depthBindGroupLayout,','        .layout = req.multisampled ? g_depthBindGroupLayoutMS : g_depthBindGroupLayout,'),
 ('''  const auto it = g_pipelines.find(req.fmt);
  if (it == g_pipelines.end()) {''','''  const auto& pipelines = req.multisampled ? g_depthPipelinesMS : g_pipelines;
  const auto it = pipelines.find(req.fmt);
  if (it == pipelines.end()) {''')])
patch('lib/gfx/depth_peek.cpp','GEKKOAOT_DEPTH_MS_PEEK',[
 ('wgpu::ComputePipeline g_pipeline;', 'wgpu::ComputePipeline g_pipeline;\nwgpu::ComputePipeline g_pipelineMS;\nwgpu::BindGroupLayout g_bindGroupLayoutMS;'),
 ('wgpu::ComputePipeline create_pipeline(const wgpu::BindGroupLayout& bindGroupLayout, const char* label) {\n  const auto shaderSource = build_shader_source();',
 '''wgpu::ComputePipeline create_pipeline(const wgpu::BindGroupLayout& bindGroupLayout, const char* label, bool ms = false) {
  auto shaderSource = build_shader_source();
  if(ms) shaderSource.replace(shaderSource.find("texture_depth_2d"), std::string("texture_depth_2d").size(), "texture_depth_multisampled_2d");'''),
 ('wgpu::BindGroupLayout create_bind_group_layout(const char* label) {\n  constexpr std::array entries{',
  'wgpu::BindGroupLayout create_bind_group_layout(const char* label, bool ms = false) {\n  const std::array entries{'),
 ('                  .sampleType = wgpu::TextureSampleType::Depth,\n                  .viewDimension = wgpu::TextureViewDimension::e2D,',
  '                  .sampleType = wgpu::TextureSampleType::Depth,\n                  .viewDimension = wgpu::TextureViewDimension::e2D,\n                  .multisampled = ms,'),
 ('  g_pipeline = create_pipeline(g_bindGroupLayout, "Depth Peek Pipeline");',
  '  g_pipeline = create_pipeline(g_bindGroupLayout, "Depth Peek Pipeline");\n  g_bindGroupLayoutMS = create_bind_group_layout("Depth Peek MS Layout", true);\n  g_pipelineMS = create_pipeline(g_bindGroupLayoutMS, "Depth Peek MS", true);'),
 ('  g_pipeline = {};', '  g_pipeline = {};\n  g_pipelineMS = {};\n  g_bindGroupLayoutMS = {};'),
 ('''  if (msaaSamples > 1) {
    Log.fatal("Depth Peek from multisampled EFB targets is not supported");
  }
''',''),
 ('      .layout = g_bindGroupLayout,','      .layout = msaaSamples > 1 ? g_bindGroupLayoutMS : g_bindGroupLayout,'),
 ('  pass.SetPipeline(g_pipeline);','  pass.SetPipeline(msaaSamples > 1 ? g_pipelineMS : g_pipeline);')])
