sources = [
    'decoration.cpp',
    'deco-button.cpp',
    'deco-layout.cpp',
    'deco-subsurface.cpp',
    'deco-theme.cpp'
]

shared_module('gapsdecor',
    sources,
    dependencies: [
        wayfire,
        wlroots,
        cairo,
        pango,
        pangocairo,
        pixman,
        vulkan,
        shaderc,
        threads
    ],
    install: true,
    install_dir: join_paths(get_option('libdir'), 'wayfire')
)
