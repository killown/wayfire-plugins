
if is_system_install
    metadatadir = wayfire.get_variable(pkgconfig: 'metadatadir')
else
    # Metadata goes to ~/.local/share/wayfire/metadata
    metadatadir = join_paths(get_option('prefix'), 'share/wayfire/metadata')
endif

install_data(
    'edge-flux.xml',
    install_dir: metadatadir
)
