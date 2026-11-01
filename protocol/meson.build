# Generate Wayland client glue with wayland-scanner. ext-foreign-toplevel-list
# is generated from the start because ext-image-capture-source's glue
# references ext_foreign_toplevel_handle_v1_interface.

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

protocols = [
	wl_protocol_dir / 'staging' / 'ext-image-copy-capture' / 'ext-image-copy-capture-v1.xml',
	wl_protocol_dir / 'staging' / 'ext-image-capture-source' / 'ext-image-capture-source-v1.xml',
	wl_protocol_dir / 'staging' / 'ext-foreign-toplevel-list' / 'ext-foreign-toplevel-list-v1.xml',
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
