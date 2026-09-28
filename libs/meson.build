subdir('vkd3d-common')
subdir('vkd3d-shader')
subdir('vkd3d')
subdir('d3d12core')
subdir('d3d12')
if get_option('enable_ddi_engine')
  if vkd3d_platform != 'windows'
    error('enable_ddi_engine needs a Windows build')
  endif
  subdir('ddi')
endif
