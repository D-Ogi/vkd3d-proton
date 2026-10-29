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

# The disk cache decisions (ddi_cache_policy.h) and libvkd3d's file calls on a non-ANSI path; no GPU.
amdgpu_wddm_vkd3d_cache_test = executable('amdgpu_wddm_vkd3d_cache_policy_test', 'cache_policy_test.c',
  include_directories : vkd3d_private_includes,
  dependencies        : [ vkd3d_common_dep ],
  install             : false)
test('ddi-cache-policy', amdgpu_wddm_vkd3d_cache_test, timeout : 30)

# The admission rules of the pipeline compile limiter and the V14 private data (BD-226); no GPU, no device.
amdgpu_wddm_vkd3d_admission_test = executable('amdgpu_wddm_vkd3d_compile_admission_test', 'compile_admission_test.c',
  include_directories : vkd3d_private_includes,
  install             : false)
test('ddi-compile-admission', amdgpu_wddm_vkd3d_admission_test, timeout : 30)

# Shader translation has no Vulkan/device dependency and runs in both driver flavours.
amdgpu_wddm_vkd3d_shader_test = executable('amdgpu_wddm_vkd3d_shader_test', 'engine_test_shader.c',
  include_directories : vkd3d_private_includes,
  dependencies        : [ vkd3d_shader_dep, dxil_spirv_dep ],
  install             : false)
test('ddi-shader-compile', amdgpu_wddm_vkd3d_shader_test, timeout : 30)
