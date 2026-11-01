# Generate Wayland client glue with wayland-scanner. Some XMLs are here only
# because another one's glue references their interfaces:
# ext-foreign-toplevel-list (ext-image-capture-source), tablet-v2
# (cursor-shape) and xdg-shell (wlr-layer-shell).

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

protocols = [
	wl_protocol_dir / 'staging' / 'ext-image-copy-capture' / 'ext-image-copy-capture-v1.xml',
	wl_protocol_dir / 'staging' / 'ext-image-capture-source' / 'ext-image-capture-source-v1.xml',
	wl_protocol_dir / 'staging' / 'ext-foreign-toplevel-list' / 'ext-foreign-toplevel-list-v1.xml',
	wl_protocol_dir / 'staging' / 'ext-data-control' / 'ext-data-control-v1.xml',
	wl_protocol_dir / 'stable' / 'viewporter' / 'viewporter.xml',
	wl_protocol_dir / 'staging' / 'single-pixel-buffer' / 'single-pixel-buffer-v1.xml',
	wl_protocol_dir / 'staging' / 'cursor-shape' / 'cursor-shape-v1.xml',
	wl_protocol_dir / 'stable' / 'tablet' / 'tablet-v2.xml',
	wl_protocol_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml',
	meson.current_source_dir() / 'wlr-layer-shell-unstable-v1.xml',
]

protocol_src = []
protocol_headers = []

foreach xml : protocols
	base = '@0@'.format(xml).split('/')[-1].split('.xml')[0]

	protocol_headers += custom_target(
		base + '-client-protocol.h',
		input: xml,
		output: base + '-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
	)

	protocol_src += custom_target(
		base + '-protocol.c',
		input: xml,
		output: base + '-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
endforeach

sweetshot_protocols = static_library(
	'sweetshot-protocols',
	protocol_src,
	protocol_headers,
	dependencies: wayland_client,
)

sweetshot_protocols_dep = declare_dependency(
	link_with: sweetshot_protocols,
	# listing the headers as sources makes their build dir an include path
	sources: protocol_headers,
)
