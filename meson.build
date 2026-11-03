project(
	'sweetshot',
	'c',
	version: '0.1.0',
	meson_version: '>= 0.60.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
	],
)

# memfd_create, O_TMPFILE, linkat and the wider POSIX surface
add_project_arguments('-D_GNU_SOURCE', language: 'c')

fs = import('fs')

wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols')
zlib = dependency('zlib')
# sqrtf and friends for the rasterizer
libm = meson.get_compiler('c').find_library('m', required: false)

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

subdir('protocol')

sweetshot_inc = include_directories('src')

executable(
	'sweetshot',
	'src/main.c',
	'src/overlay.c',
	'src/capture.c',
	'src/chip.c',
	'src/cli.c',
	'src/clip.c',
	'src/draw.c',
	'src/edit.c',
	'src/icon.c',
	'src/input.c',
	'src/log.c',
	'src/png.c',
	'src/raster.c',
	'src/rect.c',
	'src/save.c',
	'src/select.c',
	'src/sweets.c',
	'src/tool.c',
	'src/wl.c',
	dependencies: [wayland_client, zlib, libm, sweetshot_protocols_dep],
	install: true,
)

# tests
if fs.exists('tests/meson.build')
	subdir('tests')
endif
