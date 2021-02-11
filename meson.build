project(
	'xdg-desktop-portal-wlr',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.50.0',
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

prefix = get_option('prefix')
sysconfdir = get_option('sysconfdir')
add_project_arguments('-DSYSCONFDIR="@0@"'.format(join_paths(prefix, sysconfdir)), language : 'c')

inc = include_directories('include')

rt = cc.find_library('rt')
pipewire = dependency('libpipewire-0.3', version: '>= 0.3.2')
wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols', version: '>=1.14')
iniparser = cc.find_library('iniparser', dirs: [join_paths(get_option('prefix'),get_option('libdir'))])

if get_option('sd-bus-provider') == 'auto'
	assert(get_option('auto_features').auto(), 'sd-bus-provider must not be set to auto since auto_features != auto')
	sdbus = dependency('libsystemd',
		required: false,
		not_found_message: 'libsystemd not found, trying libelogind',
	)
	if not sdbus.found()
		sdbus = dependency('libelogind',
			required: false,
			not_found_message: 'libelogind not found, trying basu',
		)
	endif
	if not sdbus.found()
		sdbus = dependency('basu',
			required: false,
		)
	endif
	if not sdbus.found()
		error('Neither libsystemd, nor libelogind, nor basu was found')
	endif
else
	sdbus = dependency(get_option('sd-bus-provider'))
endif
add_project_arguments('-DHAVE_' + sdbus.name().to_upper() + '=1', language: 'c')

subdir('protocols')

executable(
	'xdg-desktop-portal-wlr',
	files([
		'src/core/main.c',
		'src/core/logger.c',
		'src/core/config.c',
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
		sdbus,
		pipewire,
		rt,
		iniparser,
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
