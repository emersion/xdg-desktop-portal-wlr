project(
	'xdg-desktop-portal-wlr',
	'c',
	version: '0.0.0',
	license: 'MIT',
	meson_version: '>=0.46.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
	],
)

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
	'-Wno-type-limits',
	'-Wno-missing-braces',
	'-Wno-missing-field-initializers',
	'-Wno-unused-parameter',
]), language: 'c')

inc = include_directories('include')

rt = cc.find_library('rt')
pipewire = dependency('libpipewire-0.3', version: '>= 0.2.9')
wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols', version: '>=1.14')
drm = dependency('libdrm').partial_dependency(includes: true)

logind = dependency('libsystemd', required: false)
if logind.found()
	add_project_arguments('-DHAVE_SYSTEMD=1', language: 'c')
else
	logind = dependency('libelogind')
	add_project_arguments('-DHAVE_ELOGIND=1', language: 'c')
endif

subdir('protocols')

executable(
	'xdg-desktop-portal-wlr',
	files([
		'src/core/main.c',
		'src/core/logger.c',
		'src/core/request.c',
		'src/core/session.c',
		'src/screenshot/screenshot.c',
		'src/screencast/screencast.c',
		'src/screencast/screencast_common.c',
		'src/screencast/wlr_screencast.c',
		'src/screencast/pipewire_screencast.c',
	]),
	dependencies: [
		wayland_client,
		wlr_protos,
		logind,
		pipewire,
		rt,
		drm
	],
	include_directories: [inc],
	install: true,
)

install_data(
	'wlr.portal',
	install_dir: join_paths(get_option('datadir'), 'xdg-desktop-portal', 'portals'),
)
