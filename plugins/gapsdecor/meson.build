project('gapsdecor', ['cpp', 'c'], version: '0.1', default_options: ['cpp_std=c++17'])
add_project_arguments(['-DWLR_USE_UNSTABLE', '-DWAYFIRE_PLUGIN'], language: ['cpp', 'c'])

wayfire = dependency('wayfire')

wlroots = dependency('wlroots', required: false)
if not wlroots.found()
    wlroots = dependency('wlroots-0.19', required: false)
endif

vulkan     = dependency('vulkan')
shaderc    = dependency('shaderc')
threads    = dependency('threads')
cairo      = dependency('cairo')
pango      = dependency('pango')
pangocairo = dependency('pangocairo')
pixman     = dependency('pixman-1')

conf_data = configuration_data()
conf_data.set_quoted('PACKAGE_VERSION', meson.project_version())
configure_file(output: 'config.h', configuration: conf_data)
add_project_arguments('-I' + meson.current_build_dir(), language: 'cpp')


subdir('src')
subdir('metadata')
