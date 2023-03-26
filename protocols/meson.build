wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')
hl_protocol_dir = hyprland_protos.get_variable('pkgdatadir')

wayland_scanner_dep = dependency('wayland-scanner', required: false, native: true)
if wayland_scanner_dep.found()
	wayland_scanner = find_program(
		wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner'),
		native: true,
	)
else
	wayland_scanner = find_program('wayland-scanner', native: true)
endif

client_protocols = [
	wl_protocol_dir / 'unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml',
	wl_protocol_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
	hl_protocol_dir / 'protocols/hyprland-toplevel-export-v1.xml',
	'wlr-screencopy-unstable-v1.xml',
	'wlr-foreign-toplevel-management-unstable-v1.xml',
	'hyprland-global-shortcuts-v1.xml',
]

wl_proto_files = []

foreach xml: client_protocols
	code = custom_target(
		xml.underscorify() + '_c',
		input: xml,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)

	client_header = custom_target(
		xml.underscorify() + '_client_h',
		input: xml,
		output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
	)

	wl_proto_files += [code, client_header]
endforeach
