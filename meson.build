project(
	'xdg-desktop-portal-wlr',
	'c',
	version: '0.7.0',
	license: 'MIT',
	meson_version: '>=0.58.0',
	default_options: ['c_std=c11', 'warning_level=2', 'werror=true'],
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
pipewire = dependency('libpipewire-0.3', version: '>= 0.3.62')
wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols', version: '>=1.24')
iniparser = dependency('inih')
gbm = dependency('gbm')
drm = dependency('libdrm')

epoll = dependency('', required: false)
if not cc.has_function('timerfd_create', prefix: '#include <sys/timerfd.h>')
	epoll = dependency('epoll-shim')
endif

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

xdpw_files = files(
	'src/core/main.c',
	'src/core/logger.c',
	'src/core/config.c',
	'src/core/request.c',
	'src/core/session.c',
	'src/core/timer.c',
	'src/core/timespec_util.c',
	'src/screenshot/screenshot.c',
	'src/screencast/screencast.c',
	'src/screencast/screencast_common.c',
	'src/screencast/wlr_screencast.c',
	'src/screencast/pipewire_screencast.c',
	'src/screencast/fps_limit.c',
)

executable(
	'xdg-desktop-portal-wlr',
	[xdpw_files, wl_proto_files],
	dependencies: [
		wayland_client,
		sdbus,
		pipewire,
		rt,
		iniparser,
		gbm,
		drm,
		epoll,
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
	user_unit_dir = systemd.get_variable(pkgconfig: 'systemduserunitdir',
		pkgconfig_define: ['prefix', get_option('prefix')])
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

scdoc = dependency('scdoc', required: get_option('man-pages'), version: '>= 1.9.7', native: true)
if scdoc.found()
	man_pages = ['xdg-desktop-portal-wlr.5.scd']
	foreach src : man_pages
		topic = src.split('.')[0]
		section = src.split('.')[1]
		output = topic + '.' + section

		custom_target(
			output,
			input: files(src),
			output: output,
			command: [
				'sh', '-c', '@0@ < @INPUT@ > @1@'.format(scdoc.get_variable(pkgconfig: 'scdoc'), output)
			],
			install: true,
			install_dir: join_paths(get_option('mandir'), 'man' + section),
		)
	endforeach
endif
