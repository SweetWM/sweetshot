#include "capture.h"
#include "cli.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "log.h"
#include "png.h"
#include "save.h"
#include "wl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static struct output *find_output(struct conn *c, const char *name) {
	struct output *o;
	if (!name) {
		return wl_container_of(c->outputs.next, o, link);
	}
	wl_list_for_each(o, &c->outputs, link) {
		if (strcmp(o->name, name) == 0) {
			return o;
		}
	}
	char list[256] = "";
	wl_list_for_each(o, &c->outputs, link) {
		if (list[0]) {
			strncat(list, ", ", sizeof(list) - strlen(list) - 1);
		}
		strncat(list, o->name, sizeof(list) - strlen(list) - 1);
	}
	log_error("no output named \"%s\" (available: %s)", name, list);
	return NULL;
}

// captures and encodes, the caller frees *png. Returns an exit status
static int shoot(const struct args *a, uint8_t **png, size_t *len) {
	struct conn conn;
	if (!conn_init(&conn)) {
		return 1;
	}
	int status = 1;
	bool window = a->cmd == CMD_WINDOW;
	if (!conn_require(&conn, window ? window_needs : output_needs)) {
		goto out;
	}
	if (a->cmd != CMD_OUTPUT) {
		log_error("%s: not implemented yet",
			window ? "window" : "region");
		goto out;
	}
	struct output *o = find_output(&conn, a->name);
	if (!o) {
		status = 2;
		goto out;
	}
	struct shot shot;
	if (!capture_output(&conn, o, a->cursor, &shot)) {
		goto out;
	}
	bool encoded = png_encode(&shot.image, png, len);
	shot_finish(&shot);
	status = encoded ? 0 : 1;

out:
	conn_finish(&conn);
	return status;
}

int main(int argc, char **argv) {
	if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
				strcmp(argv[1], "--help") == 0)) {
		fputs(cli_usage, stdout);
		return 0;
	}
	struct args a;
	if (!cli_parse(argc, argv, &a)) {
		return 2;
	}
	if (a.clip && !a.file) {
		log_error("the clipboard is not implemented yet");
		return 1;
	}

	// resolved before connecting, a bad environment fails before capture
	char *owned = NULL;
	const char *dir = a.dir;
	if (a.file && !dir) {
		dir = owned = save_default_dir();
		if (!dir) {
			return 1;
		}
	}

	uint8_t *png = NULL;
	size_t len = 0;
	int status = shoot(&a, &png, &len);
	if (status == 0 && a.file) {
		char *path = save_png(dir, png, len);
		if (path) {
			puts(path);
			free(path);
		} else {
			status = 1;
		}
	}
	if (status == 0 && a.clip) {
		log_error("the clipboard is not implemented yet");
		status = 1;
	}
	free(png);
	free(owned);
	return status;
}
