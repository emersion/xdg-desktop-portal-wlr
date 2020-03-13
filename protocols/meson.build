wayland_scanner_dep = dependency('wayland-scanner', required: false, native: true)
if wayland_scanner_dep.found()
	wayland_scanner = find_program(
		wayland_scanner_dep.get_pkgconfig_variable('wayland_scanner'),
		native: true,
	)
else
	wayland_scanner = find_program('wayland-scanner', native: true)
endif

client_protocols = [
	'wlr-screencopy-unstable-v1.xml',
	'xdg-output-unstable-v1.xml',
]

wl_protos_src = []
wl_protos_headers = []

foreach xml: client_protocols
	wl_protos_src += custom_target(
		xml.underscorify() + '_server_c',
		input: xml,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
	wl_protos_headers += custom_target(
		xml.underscorify() + '_server_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)

	wl_protos_headers += custom_target(
		xml.underscorify() + '_client_h',
		input: xml,
		output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

lib_wl_protos = static_library(
	'wl_protos',
	wl_protos_src + wl_protos_headers,
	dependencies: wayland_client.partial_dependency(compile_args: true),
)

wlr_protos = declare_dependency(
	link_with: lib_wl_protos,
	sources: wl_protos_headers,
)
