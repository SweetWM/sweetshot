#include "capture.h"
#include "cli.h"
#include "clip.h"
#include "edit.h"
#include "ext-data-control-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "log.h"
#include "png.h"
#include "save.h"
#include "select.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "sweets.h"
#include "viewporter-client-protocol.h"
#include "wl.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct wl_interface *const output_needs[] = {
	&wl_shm_interface,
	&wl_output_interface,
	&ext_image_copy_capture_manager_v1_interface,
	&ext_output_image_capture_source_manager_v1_interface,
	NULL,
};

static const struct wl_interface *const region_needs[] = {
	&wl_shm_interface,
	&wl_output_interface,
	&ext_image_copy_capture_manager_v1_interface,
	&ext_output_image_capture_source_manager_v1_interface,
	&wl_seat_interface,
	&wl_compositor_interface,
	&wl_subcompositor_interface,
	&wp_viewporter_interface,
	&wp_single_pixel_buffer_manager_v1_interface,
	&zwlr_layer_shell_v1_interface,
	NULL,
};

// the editor overlay, plus the output it is shown on
static const struct wl_interface *const edit_needs[] = {
	&wl_shm_interface,
	&wl_output_interface,
	&wl_seat_interface,
	&wl_compositor_interface,
	&wl_subcompositor_interface,
	&wp_viewporter_interface,
	&wp_single_pixel_buffer_manager_v1_interface,
	&zwlr_layer_shell_v1_interface,
	NULL,
};

static const struct wl_interface *const clip_needs[] = {
	&wl_seat_interface,
	&ext_data_control_manager_v1_interface,
	NULL,
};

static const struct wl_interface *const window_needs[] = {
	&wl_shm_interface,
	&ext_image_copy_capture_manager_v1_interface,
	&ext_foreign_toplevel_image_capture_source_manager_v1_interface,
	&ext_foreign_toplevel_list_v1_interface,
	NULL,
};

static struct output *first_output(struct conn *c) {
	struct output *o;
	return wl_container_of(c->outputs.next, o, link);
}

static struct output *output_named(struct conn *c, const char *name) {
	struct output *o;
	wl_list_for_each(o, &c->outputs, link) {
		// one plugged in while capturing has no name yet
		if (name && o->name && strcmp(o->name, name) == 0) {
			return o;
		}
	}
	return NULL;
}

static struct output *find_output(struct conn *c, const char *name) {
	if (!name) {
		return first_output(c);
	}
	struct output *o = output_named(c, name);
	if (o) {
		return o;
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

static int encode(struct shot *shot, const struct png_image *img, bool edit,
	struct editor *e, uint8_t **png, size_t *len) {
	struct png_image view = *img;
	int status = 1;
	if (edit) {
		enum edit_result r = e ? edit_run(e, &view) : EDIT_FAILED;
		if (r != EDIT_DONE) {
			status = r == EDIT_CANCELLED ? 3 : 1;
			goto out;
		}
	}
	status = png_encode(&view, png, len) ? 0 : 1;
out:
	edit_destroy(e);
	shot_finish(shot);
	return status;
}

// without a NAME, the output of the focused window, else the first one
static int shoot_output(
	struct conn *c, const struct args *a, uint8_t **png, size_t *len) {
	const char *name = a->name;
	struct focus f;
	if (!name && !sweets_focus(&f)) {
		f = (struct focus){0};
		log_error("using the first output");
	}
	if (!name && f.found && f.output[0]) {
		name = f.output;
	}
	struct output *o = find_output(c, name);
	if (!o) {
		return 2;
	}
	struct shot shot;
	if (!capture_output(c, o, a->cursor, &shot)) {
		return 1;
	}
	struct edit_input in = {
		.output = o,
		.image = shot.image,
		.fit = EDIT_OUTPUT,
	};
	return encode(&shot, &shot.image, a->edit,
		a->edit ? edit_create(c, &in) : NULL, png, len);
}

// the window focused when sweetshot started, a later focus change does not
// retarget it, and a missing one is an error, never another window
static int shoot_window(
	struct conn *c, const struct args *a, uint8_t **png, size_t *len) {
	struct focus f;
	if (!sweets_focus(&f)) {
		return 1;
	}
	if (!f.found) {
		log_error("no window has focus");
		return 1;
	}
	if (!f.identifier[0]) {
		log_error("the focused window has no capture identifier");
		return 1;
	}
	struct shot shot;
	if (!capture_window(c, f.identifier, a->cursor, &shot)) {
		return 1;
	}
	// edited on the window's output
	struct output *o = output_named(c, f.output);
	struct edit_input in = {
		.output = o ? o : first_output(c),
		.image = shot.image,
		.fit = EDIT_WINDOW,
	};
	return encode(&shot, &shot.image, a->edit,
		a->edit ? edit_create(c, &in) : NULL, png, len);
}

// region crops one frozen output: every output is captured at launch, with
// and without the pointer and the selector shows them as they land
static int shoot_region(
	struct conn *c, const struct args *a, uint8_t **png, size_t *len) {
	size_t n = (size_t)wl_list_length(&c->outputs);
	struct capture_req *reqs = calloc(2 * n, sizeof(*reqs));
	struct shot *shots = calloc(2 * n, sizeof(*shots));
	if (!reqs || !shots) {
		log_error("out of memory");
		free(reqs);
		free(shots);
		return 1;
	}
	// readbacks land in request order, the variant shown first goes first
	size_t i = 0;
	struct output *o;
	wl_list_for_each(o, &c->outputs, link) {
		reqs[i] = (struct capture_req){o, a->cursor};
		reqs[n + i] = (struct capture_req){o, !a->cursor};
		i++;
	}
	struct capture *cap = capture_start(c, reqs, 2 * n, shots);
	free(reqs);
	struct shot *plain = a->cursor ? shots + n : shots;
	struct shot *pointer = a->cursor ? shots : shots + n;
	struct selector *s =
		cap ? select_create(c, a->cursor, cap, plain, pointer) : NULL;
	struct choice ch;
	enum select_result r = s ? select_run(s, &ch) : SELECT_FAILED;
	// only the chosen pixels are kept for encoding or editing
	struct shot shot = {0};
	struct png_image img = {0};
	struct rect px = {0};
	struct editor *e = NULL;
	// the selector showed every shot through its buffer transform, so only
	// the chosen one is copied upright, and not before the choice
	if (r == SELECT_CONFIRMED && !shot_upright(ch.shot)) {
		r = SELECT_FAILED;
	}
	if (r == SELECT_CONFIRMED) {
		shot = *ch.shot;
		*ch.shot = (struct shot){0};
		img = shot.image;
		px = rect_physical(
			ch.rect, ch.width, ch.height, img.width, img.height);
		img.data += (size_t)px.y * img.stride + (size_t)px.x * 4;
		img.width = (uint32_t)px.w;
		img.height = (uint32_t)px.h;
	}
	if (r == SELECT_CONFIRMED && a->edit) {
		struct edit_input in = {
			.output = ch.output,
			.image = img,
			.fit = EDIT_REGION,
			.sel = ch.rect,
			.full_w = shot.image.width,
			.full_h = shot.image.height,
			.backdrop = shot.buffer,
			.backdrop_transform = shot.transform,
			.backdrop_crop = px,
		};
		e = edit_create(c, &in);
		if (e && !edit_wait_shown(e)) {
			edit_destroy(e);
			e = NULL;
		}
	}
	select_destroy(s);
	capture_destroy(cap);
	for (i = 0; i < 2 * n; i++) {
		shot_finish(&shots[i]);
	}
	free(shots);
	if (r != SELECT_CONFIRMED) {
		return r == SELECT_CANCELLED ? 3 : 1;
	}
	return encode(&shot, &img, a->edit, e, png, len);
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

	// resolved before connecting, a bad environment fails before capture
	char *owned = NULL;
	const char *dir = a.dir;
	if (a.file && !dir) {
		dir = owned = save_default_dir();
		if (!dir) {
			return 1;
		}
	}
	struct conn conn;
	if (!conn_init(&conn)) {
		free(owned);
		return 1;
	}

	int status = 1;
	uint8_t *png = NULL;
	size_t len = 0;
	const struct wl_interface *const *needs[] = {
		[CMD_REGION] = region_needs,
		[CMD_OUTPUT] = output_needs,
		[CMD_WINDOW] = window_needs,
	};
	if (!conn_require(&conn, needs[a.cmd]) ||
		(a.edit && !conn_require(&conn, edit_needs))) {
		goto out;
	}
	// with the clipboard as the only destination, nothing is worth
	// capturing without it
	if (a.clip && !a.file && !conn_require(&conn, clip_needs)) {
		goto out;
	}
	int (*shoot[])(
		struct conn *, const struct args *, uint8_t **, size_t *) = {
		[CMD_REGION] = shoot_region,
		[CMD_OUTPUT] = shoot_output,
		[CMD_WINDOW] = shoot_window,
	};
	status = shoot[a.cmd](&conn, &a, &png, &len);
	if (status != 0) {
		goto out;
	}

	// a failed destination is reported but does not cancel the other:
	// file first, then the clipboard, then the path on stdout
	char *path = NULL;
	if (a.file) {
		path = save_png(dir, png, len);
		status = path ? 0 : 1;
	}
	struct clip cl = {0};
	bool clipped = false;
	if (a.clip) {
		clipped = conn_require(&conn, clip_needs) &&
			  clip_set(&cl, &conn, png, len);
		status = clipped ? status : 1;
	}
	if (path) {
		puts(path);
		free(path);
	}
	if (clipped) {
		if (clip_serve(&cl)) {
			// the child shares the connection, tearing it down here
			// would drop the selection
			free(png);
			free(owned);
			return status;
		}
		status = 1;
		clip_finish(&cl);
	}

out:
	free(png);
	conn_finish(&conn);
	free(owned);
	return status;
}
