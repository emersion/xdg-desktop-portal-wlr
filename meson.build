project(
	'xdg-desktop-portal-wlr',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.47.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
	],
)

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
	'-Wno-missing-braces',
	'-Wno-missing-field-initializers',
	'-Wno-unused-parameter',
	'-D_POSIX_C_SOURCE=200809L',
]), language: 'c')

inc = include_directories('include')

rt = cc.find_library('rt')
pipewire = dependency('libpipewire-0.3', version: '>= 0.3.2')
wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols', version: '>=1.14')

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
	],
	include_directories: [inc],
	install: true,
	install_dir: get_option('libexecdir'),
)

conf_data = configuration_data()
conf_data.set('libexecdir',
	join_paths(get_option('prefix'), get_option('libexecdir')))
conf_data.set('systemd_service', '')

systemd = dependency('systemd', required: get_option('systemd'))

if systemd.found()
	systemd_service_file = 'xdg-desktop-portal-wlr.service'
	user_unit_dir = systemd.get_pkgconfig_variable('systemduserunitdir',
		define_variable: ['prefix', get_option('prefix')])
	conf_data.set('systemd_service', 'SystemdService=' + systemd_service_file)

	configure_file(
		configuration: conf_data,
		input: 'contrib/systemd/' + systemd_service_file + '.in',
		output: '@BASENAME@',
		install_dir: user_unit_dir,
	)
endif

configure_file(
	configuration: conf_data,
	input: 'org.freedesktop.impl.portal.desktop.wlr.service.in',
	output: '@BASENAME@',
	install_dir: join_paths(get_option('datadir'), 'dbus-1', 'services'),
)

install_data(
	'wlr.portal',
	install_dir: join_paths(get_option('datadir'), 'xdg-desktop-portal', 'portals'),
)
