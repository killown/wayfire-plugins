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

# Detect home directory for default prefix if not set
home_dir = run_command('sh', '-c', 'echo $HOME', check: true).stdout().strip()

add_project_arguments(
    ['-DWLR_USE_UNSTABLE', '-DWAYFIRE_PLUGIN'],
    language: ['cpp', 'c']
)

add_project_link_arguments(
    ['-rdynamic'],
    language: 'cpp'
)

wayfire = dependency('wayfire')

subdir('src')
subdir('metadata')
