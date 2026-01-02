
is_system_install = (get_option('prefix') == '/usr')

if is_system_install
    install_dir_lib = join_paths(get_option('libdir'), 'wayfire')
else
    install_dir_lib = join_paths(get_option('prefix'), 'lib/wayfire')
endif

shared_module(
    'inertial-view-vk',
    'inertial-view-vk.cpp',
    dependencies: [wayfire],
    install: true,
    install_dir: install_dir_lib,
    name_prefix: 'lib'
)
