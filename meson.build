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

executable(
	'sweetshot',
	'src/main.c',
	install: true,
)
