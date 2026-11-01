#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "log.h"
#include "wl.h"

#include <stdio.h>
#include <string.h>

static const char usage[] =
	"usage: sweetshot region [--clip] [--file] [--dir PATH] [--cursor]\n"
	"       sweetshot output [NAME] [--clip] [--file] [--dir PATH] "
	"[--cursor]\n"
	"       sweetshot window [--clip] [--file] [--dir PATH] [--cursor]\n";

// region is output capture plus a crop, so it needs what output needs
static const struct wl_interface *const output_needs[] = {
	&wl_shm_interface,
	&wl_output_interface,
	&ext_image_copy_capture_manager_v1_interface,
	&ext_output_image_capture_source_manager_v1_interface,
	NULL,
};

static const struct wl_interface *const window_needs[] = {
	&wl_shm_interface,
	&ext_image_copy_capture_manager_v1_interface,
	&ext_foreign_toplevel_image_capture_source_manager_v1_interface,
	NULL,
};

int main(int argc, char **argv) {
	if (argc < 2) {
		fputs(usage, stderr);
		return 2;
	}

	const char *cmd = argv[1];
	const struct wl_interface *const *needs;
	if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		fputs(usage, stdout);
		return 0;
	} else if (strcmp(cmd, "region") == 0 || strcmp(cmd, "output") == 0) {
		needs = output_needs;
	} else if (strcmp(cmd, "window") == 0) {
		needs = window_needs;
	} else {
		log_error("unknown command \"%s\"", cmd);
		fputs(usage, stderr);
		return 2;
	}

	struct conn conn;
	if (!conn_init(&conn)) {
		return 1;
	}
	if (conn_require(&conn, needs)) {
		log_error("%s: not implemented yet", cmd);
	}
	conn_finish(&conn);
	return 1;
}
