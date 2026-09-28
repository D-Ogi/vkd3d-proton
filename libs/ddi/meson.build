# amdgpu_wddm_vkd3d.dll: vkd3d-proton as the engine behind the BC-250 system D3D12 user-mode driver
# (bc250_vkd3d_engine.h). Built only with -Denable_ddi_engine=true, on Windows.

# The engine is loaded into every D3D12 process: its C runtime is linked statically, so that it needs no
# VC++ runtime on the machine and shares no CRT state with the application.
if vkd3d_compiler.get_argument_syntax() == 'msvc'
  if get_option('b_vscrt') not in [ 'mt', 'static_from_buildtype' ]
    error('enable_ddi_engine needs the static C runtime: -Db_vscrt=mt')
  endif
endif

amdgpu_wddm_vkd3d_lib = shared_library('amdgpu_wddm_vkd3d', 'ddi_engine.c',
  name_prefix         : '',
  dependencies        : [ vkd3d_dep ],
  include_directories : vkd3d_private_includes,
  install             : true,
  vs_module_defs      : 'amdgpu_wddm_vkd3d.def')

# The test uses the SDK's d3d12.h and dxgi1_6.h, not vkd3d-proton's own D3D12 headers, as the shell will.
amdgpu_wddm_vkd3d_test = executable('amdgpu_wddm_vkd3d_engine_test', 'engine_test.c',
  include_directories : vkd3d_external_includes,
  dependencies        : [ vkd3d_compiler.find_library('dxgi'), vkd3d_compiler.find_library('psapi'),
                          vkd3d_compiler.find_library('bcrypt') ],
  install             : false)
