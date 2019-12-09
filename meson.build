project(
	'xdg-desktop-portal-wlr',
	'c',
	version: '0.0.0',
	license: 'MIT',
	meson_version: '>=0.43.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
	],
)

add_project_arguments('-Wno-unused-parameter', language: 'c')

xdpw_inc = include_directories('include')

wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols', version: '>=1.14')
systemd = dependency('libsystemd')

# subdir('protocol')

executable(
	'xdg-desktop-portal-wlr',
	files([
		'main.c',
		'request.c',
		'session.c',
		'screenshot.c',
	]),
	dependencies: [
		wayland_client,
		systemd,
	],
	include_directories: [xdpw_inc],
	install: true,
)

install_data(
	'wlr.portal',
	install_dir: join_paths(get_option('datadir'), 'xdg-desktop-portal', 'portals'),
)
