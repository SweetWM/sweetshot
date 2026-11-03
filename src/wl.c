#include "wl.h"

#include "cursor-shape-v1-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "log.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void output_geometry(void *data, struct wl_output *wl, int32_t x,
	int32_t y, int32_t phys_w, int32_t phys_h, int32_t subpixel,
	const char *make, const char *model, int32_t transform) {
	(void)wl, (void)x, (void)y, (void)phys_w, (void)phys_h;
	(void)subpixel, (void)make, (void)model;
	struct output *o = data;
	o->transform = transform;
}

static void output_mode(void *data, struct wl_output *wl, uint32_t flags,
	int32_t width, int32_t height, int32_t refresh) {
	(void)wl, (void)refresh;
	struct output *o = data;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		o->mode_w = width;
		o->mode_h = height;
	}
}

static void output_done(void *data, struct wl_output *wl) {
	(void)data, (void)wl;
}

static void output_scale(void *data, struct wl_output *wl, int32_t factor) {
	(void)data, (void)wl, (void)factor;
}

static void output_name(void *data, struct wl_output *wl, const char *name) {
	(void)wl;
	struct output *o = data;
	char *dup = strdup(name);
	if (!dup) {
		// the conn is not reachable from here; an unnamed output is
		// caught by conn_init's name check
		return;
	}
	free(o->name);
	o->name = dup;
}

static void output_description(
	void *data, struct wl_output *wl, const char *description) {
	(void)data, (void)wl, (void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void *bind_global(struct conn *c, uint32_t name,
	const struct wl_interface *iface, uint32_t version) {
	void *p = wl_registry_bind(c->registry, name, iface, version);
	if (!p) {
		c->oom = true;
	}
	return p;
}

static void add_output(struct conn *c, uint32_t name) {
	struct output *o = calloc(1, sizeof(*o));
	if (!o) {
		c->oom = true;
		return;
	}
	o->wl = bind_global(c, name, &wl_output_interface, 4);
	if (!o->wl) {
		free(o);
		return;
	}
	wl_output_add_listener(o->wl, &output_listener, o);
	wl_list_insert(c->outputs.prev, &o->link);
}

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	(void)seat;
	struct conn *c = data;
	c->seat_caps = caps;
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
	(void)data, (void)seat, (void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static bool is(const char *iface, const struct wl_interface *want) {
	return strcmp(iface, want->name) == 0;
}

static void registry_global(void *data, struct wl_registry *registry,
	uint32_t name, const char *iface, uint32_t version) {
	(void)registry;
	struct conn *c = data;
	const struct wl_interface *capture =
		&ext_image_copy_capture_manager_v1_interface;
	const struct wl_interface *output_source =
		&ext_output_image_capture_source_manager_v1_interface;
	const struct wl_interface *toplevel_source =
		&ext_foreign_toplevel_image_capture_source_manager_v1_interface;
	const struct wl_interface *data_control =
		&ext_data_control_manager_v1_interface;
	const struct wl_interface *single_pixel =
		&wp_single_pixel_buffer_manager_v1_interface;
	const struct wl_interface *cursor_shape =
		&wp_cursor_shape_manager_v1_interface;

	if (is(iface, &wl_shm_interface) && !c->shm) {
		c->shm = bind_global(c, name, &wl_shm_interface, 1);
	} else if (is(iface, &wl_output_interface) && version >= 4) {
		// v4 is the first version with the name event
		add_output(c, name);
	} else if (is(iface, capture) && !c->capture) {
		c->capture = bind_global(c, name, capture, 1);
	} else if (is(iface, output_source) && !c->output_source) {
		c->output_source = bind_global(c, name, output_source, 1);
	} else if (is(iface, toplevel_source) && !c->toplevel_source) {
		c->toplevel_source = bind_global(c, name, toplevel_source, 1);
	} else if (is(iface, &wl_seat_interface) && !c->seat) {
		// v5: release; later versions add nothing the selector uses
		c->seat = bind_global(
			c, name, &wl_seat_interface, version < 5 ? version : 5);
		if (c->seat) {
			wl_seat_add_listener(c->seat, &seat_listener, c);
		}
	} else if (is(iface, data_control) && !c->data_control) {
		c->data_control = bind_global(c, name, data_control, 1);
	} else if (is(iface, &wl_compositor_interface) && version >= 4 &&
		   !c->compositor) {
		// v4: wl_surface.damage_buffer
		c->compositor =
			bind_global(c, name, &wl_compositor_interface, 4);
	} else if (is(iface, &wl_subcompositor_interface) &&
		   !c->subcompositor) {
		c->subcompositor =
			bind_global(c, name, &wl_subcompositor_interface, 1);
	} else if (is(iface, &wp_viewporter_interface) && !c->viewporter) {
		c->viewporter =
			bind_global(c, name, &wp_viewporter_interface, 1);
	} else if (is(iface, single_pixel) && !c->single_pixel) {
		c->single_pixel = bind_global(c, name, single_pixel, 1);
	} else if (is(iface, &zwlr_layer_shell_v1_interface) &&
		   !c->layer_shell) {
		c->layer_shell =
			bind_global(c, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (is(iface, &ext_foreign_toplevel_list_v1_interface)) {
		// binding streams every toplevel, so window capture binds it
		// itself when it needs it
		c->toplevel_list_name = name;
	} else if (is(iface, cursor_shape) && !c->cursor_shape) {
		c->cursor_shape = bind_global(c, name, cursor_shape, 1);
	}
}

// sweetshot lives for one capture. An output unplugged meanwhile ends its
// capture session with `stopped`, which the capture path reports
static void registry_global_remove(
	void *data, struct wl_registry *registry, uint32_t name) {
	(void)data, (void)registry, (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

bool conn_init(struct conn *c) {
	*c = (struct conn){0};
	wl_list_init(&c->outputs);

	c->display = wl_display_connect(NULL);
	if (!c->display) {
		const char *name = getenv("WAYLAND_DISPLAY");
		if (name) {
			log_error("cannot connect to Wayland display \"%s\"",
				name);
		} else {
			log_error(
				"cannot connect to Wayland: WAYLAND_DISPLAY is "
				"not set");
		}
		return false;
	}
	c->registry = wl_display_get_registry(c->display);
	if (!c->registry) {
		log_error("out of memory");
		goto fail;
	}
	wl_registry_add_listener(c->registry, &registry_listener, c);

	// the first roundtrip delivers the globals, the second the initial
	// events (names) of the outputs bound during the first
	for (int i = 0; i < 2; i++) {
		if (wl_display_roundtrip(c->display) < 0) {
			log_error("lost the Wayland connection");
			goto fail;
		}
	}
	bool unnamed = false;
	struct output *o;
	wl_list_for_each(o, &c->outputs, link) {
		unnamed |= !o->name;
	}
	if (c->oom || unnamed) {
		// v4 outputs always send a name, so a missing one means
		// strdup failed
		log_error("out of memory");
		goto fail;
	}
	return true;

fail:
	conn_finish(c);
	return false;
}

void conn_finish(struct conn *c) {
	if (c->cursor_shape) {
		wp_cursor_shape_manager_v1_destroy(c->cursor_shape);
	}
	if (c->layer_shell) {
		zwlr_layer_shell_v1_destroy(c->layer_shell);
	}
	if (c->single_pixel) {
		wp_single_pixel_buffer_manager_v1_destroy(c->single_pixel);
	}
	if (c->viewporter) {
		wp_viewporter_destroy(c->viewporter);
	}
	if (c->subcompositor) {
		wl_subcompositor_destroy(c->subcompositor);
	}
	if (c->compositor) {
		wl_compositor_destroy(c->compositor);
	}
	if (c->data_control) {
		ext_data_control_manager_v1_destroy(c->data_control);
	}
	if (c->seat && wl_seat_get_version(c->seat) >= 5) {
		wl_seat_release(c->seat);
	} else if (c->seat) {
		wl_seat_destroy(c->seat);
	}
	if (c->toplevel_source) {
		ext_foreign_toplevel_image_capture_source_manager_v1_destroy(
			c->toplevel_source);
	}
	if (c->output_source) {
		ext_output_image_capture_source_manager_v1_destroy(
			c->output_source);
	}
	if (c->capture) {
		ext_image_copy_capture_manager_v1_destroy(c->capture);
	}
	struct output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &c->outputs, link) {
		wl_output_release(o->wl);
		free(o->name);
		free(o);
	}
	if (c->shm) {
		wl_shm_destroy(c->shm);
	}
	if (c->registry) {
		wl_registry_destroy(c->registry);
	}
	if (c->display) {
		wl_display_disconnect(c->display);
	}
	*c = (struct conn){0};
}

static bool has(const struct conn *c, const struct wl_interface *iface) {
	if (iface == &wl_shm_interface) {
		return c->shm;
	}
	if (iface == &wl_output_interface) {
		return !wl_list_empty(&c->outputs);
	}
	if (iface == &ext_image_copy_capture_manager_v1_interface) {
		return c->capture;
	}
	if (iface == &ext_output_image_capture_source_manager_v1_interface) {
		return c->output_source;
	}
	if (iface ==
		&ext_foreign_toplevel_image_capture_source_manager_v1_interface) {
		return c->toplevel_source;
	}
	if (iface == &wl_seat_interface) {
		return c->seat;
	}
	if (iface == &ext_data_control_manager_v1_interface) {
		return c->data_control;
	}
	if (iface == &ext_foreign_toplevel_list_v1_interface) {
		return c->toplevel_list_name;
	}
	if (iface == &wl_compositor_interface) {
		return c->compositor;
	}
	if (iface == &wl_subcompositor_interface) {
		return c->subcompositor;
	}
	if (iface == &wp_viewporter_interface) {
		return c->viewporter;
	}
	if (iface == &wp_single_pixel_buffer_manager_v1_interface) {
		return c->single_pixel;
	}
	if (iface == &zwlr_layer_shell_v1_interface) {
		return c->layer_shell;
	}
	return false;
}

// Sweets permission group that hides iface unless granted, or NULL
static const char *permission(const struct wl_interface *iface) {
	if (iface == &ext_image_copy_capture_manager_v1_interface ||
		iface ==
			&ext_output_image_capture_source_manager_v1_interface ||
		iface ==
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface) {
		return "screencopy";
	}
	if (iface == &ext_data_control_manager_v1_interface) {
		return "data_control";
	}
	return NULL;
}

bool conn_require(
	const struct conn *c, const struct wl_interface *const *needs) {
	for (; *needs; needs++) {
		if (has(c, *needs)) {
			continue;
		}
		const char *perm = permission(*needs);
		if (perm) {
			log_error("compositor does not offer %s (Sweets: allow "
				  "\"%s\")",
				(*needs)->name, perm);
		} else {
			log_error(
				"compositor does not offer %s", (*needs)->name);
		}
		return false;
	}
	return true;
}
