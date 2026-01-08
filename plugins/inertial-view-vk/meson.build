project(
    'inertial-view-vk',
    ['cpp', 'c'],
    version: '0.1.0',
    default_options: [
        'cpp_std=c++17',
        'warning_level=2',
        'prefix=' + join_paths(get_variable('home', '/home/user'), '.local')
    ]
)

wayfire_share_dir = join_paths(get_option('datadir'), 'wayfire/inertial-vk')
full_shader_path = join_paths(get_option('prefix'), wayfire_share_dir, 'shaders/')

add_project_arguments([
    '-DWLR_USE_UNSTABLE', 
    '-DWAYFIRE_PLUGIN',
    '-DSHADER_PATH="' + full_shader_path + '"'
], language: ['cpp', 'c'])

add_project_link_arguments(['-rdynamic'], language: 'cpp')

wayfire = dependency('wayfire')
wlroots = dependency('wlroots-0.19')
vulkan  = dependency('vulkan')
shaderc = dependency('shaderc')
threads = dependency('threads')

subdir('src')
subdir('metadata')

install_subdir('shaders', install_dir: wayfire_share_dir)
