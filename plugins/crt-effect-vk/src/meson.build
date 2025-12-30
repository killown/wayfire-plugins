
# We check if prefix is /usr. If not, we assume a local install.
is_system_install = (get_option('prefix') == '/usr')

if is_system_install
    install_dir_lib = join_paths(get_option('libdir'), 'wayfire')
else
    # Force ~/.local/lib/wayfire for local development
    install_dir_lib = join_paths(get_option('prefix'), 'lib/wayfire')
endif

shared_module(
    'crt-effect-vk',
    'crt-effect-vk.cpp',
    dependencies: [wayfire],
    install: true,
    install_dir: install_dir_lib,
    name_prefix: 'lib'
)
