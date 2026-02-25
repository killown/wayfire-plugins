is_system_install = (get_option('prefix') == '/usr')
metadatadir = is_system_install ? wayfire.get_variable(pkgconfig: 'metadatadir') : join_paths(get_option('prefix'), 'share/wayfire/metadata')
install_data('gapsdecor.xml', install_dir: metadatadir)
