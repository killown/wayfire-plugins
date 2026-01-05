is_system_install = (get_option('prefix') == '/usr')

if is_system_install
    install_dir_lib = join_paths(get_option('libdir'), 'wayfire')
else
    install_dir_lib = join_paths(get_option('prefix'), 'lib/wayfire')
endif

wlroots = dependency('wlroots-0.19')
vulkan  = dependency('vulkan')
threads = dependency('threads')

shared_module(
    'inertial-view-vk',
    [
        'inertial-view-vk.cpp', 
        'vulkan-context.cpp', 
        'inertial-node.cpp'
    ],
    dependencies: [
        wayfire, 
        wlroots, 
        vulkan, 
        threads
    ],
    install: true,
    install_dir: install_dir_lib,
    name_prefix: 'lib',
    cpp_args: ['-DWLR_USE_UNSTABLE', '-DWAYFIRE_PLUGIN']
)
