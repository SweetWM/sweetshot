#include "overlay.h"

#include "log.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdlib.h>

void overlay_map(struct zwlr_layer_surface_v1 *layer,
	struct wl_surface *surface,
	const struct zwlr_layer_surface_v1_listener *l, void *data) {
	zwlr_layer_surface_v1_add_listener(layer, l, data);
	zwlr_layer_surface_v1_set_anchor(
		layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	// cover panels too instead of being placed beside them
	zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	wl_surface_commit(surface);
}

struct catcher {
	struct catchers *all;
	struct wl_surface *surface;
	struct wp_viewport *viewport;
	struct zwlr_layer_surface_v1 *layer;
};

struct catchers {
	struct wl_buffer *clear;
	struct wl_region *no_input;
	struct catcher *list;
	size_t n;
};

static void configure(void *data, struct zwlr_layer_surface_v1 *layer,
	uint32_t serial, uint32_t w, uint32_t h) {
	struct catcher *k = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if (w == 0 || h == 0 || w > INT32_MAX || h > INT32_MAX) {
		return;
	}
	wp_viewport_set_destination(k->viewport, (int32_t)w, (int32_t)h);
	wl_surface_attach(k->surface, k->all->clear, 0, 0);
	wl_surface_damage_buffer(k->surface, 0, 0, 1, 1);
	wl_surface_commit(k->surface);
}

// its output went away; the rest carries on
static void closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	(void)data, (void)layer;
}

static const struct zwlr_layer_surface_v1_listener catcher_listener = {
	.configure = configure,
	.closed = closed,
};

struct catchers *catchers_create(struct conn *c, const struct output *skip) {
	struct catchers *all = calloc(1, sizeof(*all));
	if (!all) {
		goto fail;
	}
	all->list = calloc(wl_list_length(&c->outputs), sizeof(*all->list));
	all->clear = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
		c->single_pixel, 0, 0, 0, 0);
	all->no_input = wl_compositor_create_region(c->compositor);
	if (!all->list || !all->clear || !all->no_input) {
		goto fail;
	}
	struct output *o;
	wl_list_for_each(o, &c->outputs, link) {
		if (o == skip) {
			continue;
		}
		struct catcher *k = &all->list[all->n++];
		k->all = all;
		k->surface = wl_compositor_create_surface(c->compositor);
		if (!k->surface) {
			goto fail;
		}
		k->viewport =
			wp_viewporter_get_viewport(c->viewporter, k->surface);
		k->layer = zwlr_layer_shell_v1_get_layer_surface(c->layer_shell,
			k->surface, o->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
			"sweetshot");
		if (!k->viewport || !k->layer) {
			goto fail;
		}
		wl_surface_set_input_region(k->surface, all->no_input);
		overlay_map(k->layer, k->surface, &catcher_listener, k);
	}
	return all;

fail:
	log_error("out of memory");
	catchers_destroy(all);
	return NULL;
}

void catchers_destroy(struct catchers *all) {
	if (!all) {
		return;
	}
	for (size_t i = all->n; i > 0; i--) {
		struct catcher *k = &all->list[i - 1];
		if (k->layer) {
			zwlr_layer_surface_v1_destroy(k->layer);
		}
		if (k->viewport) {
			wp_viewport_destroy(k->viewport);
		}
		if (k->surface) {
			wl_surface_destroy(k->surface);
		}
	}
	free(all->list);
	if (all->no_input) {
		wl_region_destroy(all->no_input);
	}
	if (all->clear) {
		wl_buffer_destroy(all->clear);
	}
	free(all);
}
