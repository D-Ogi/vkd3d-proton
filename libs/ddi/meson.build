# bc250vkd3d.dll: vkd3d-proton as the engine behind the BC-250 system D3D12 user-mode driver
# (bc250_vkd3d_engine.h). Built only with -Denable_ddi_engine=true, on Windows.

bc250vkd3d_lib = shared_library('bc250vkd3d', 'ddi_engine.c',
  name_prefix         : '',
  dependencies        : [ vkd3d_dep ],
  include_directories : vkd3d_private_includes,
  install             : true,
  vs_module_defs      : 'bc250vkd3d.def')

# The test uses the SDK's d3d12.h and dxgi1_6.h, not vkd3d-proton's own D3D12 headers, as the shell will.
bc250vkd3d_test = executable('bc250vkd3d_engine_test', 'engine_test.c',
  include_directories : vkd3d_external_includes,
  dependencies        : [ vkd3d_compiler.find_library('dxgi'), vkd3d_compiler.find_library('psapi'),
                          vkd3d_compiler.find_library('bcrypt') ],
  install             : false)
