#include "overlay.h"

#include "log.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "sprite.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <math.h>
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

enum {
	HANDLE = 11,
	HANDLES = 4,
};

struct handles {
	struct sprite *dots[HANDLES];
	double scale; // the dots are drawn for, 0 before
};

struct handles *handles_create(
	struct conn *c, struct wl_surface *parent, struct wl_region *no_input) {
	struct handles *h = calloc(1, sizeof(*h));
	if (!h) {
		log_error("out of memory");
		return NULL;
	}
	for (int i = 0; i < HANDLES; i++) {
		// reports its own error
		h->dots[i] = sprite_create(c, parent, no_input);
		if (!h->dots[i]) {
			handles_destroy(h);
			return NULL;
		}
	}
	return h;
}

// drawn at the output's own scale, so they stay sharp at 1.5
static bool draw(struct handles *h, double scale) {
	long n = lround(HANDLE * scale);
	uint32_t px = n < 1 ? 1 : (uint32_t)n;
	float k = (float)px / HANDLE;
	struct raster_point c = {(float)px / 2.0f, (float)px / 2.0f};
	// a dark ring keeps the dot visible over white
	const struct draw_op ops[] = {
		{.kind = DRAW_LINE,
			.color = 0,
			.width = 10 * k,
			.a = c,
			.b = c},
		{.kind = DRAW_LINE,
			.color = 0xffffff,
			.width = 8 * k,
			.a = c,
			.b = c},
	};
	for (int i = 0; i < HANDLES; i++) {
		if (!sprite_draw(h->dots[i], HANDLE, HANDLE, px, px, 0, ops,
			    sizeof(ops) / sizeof(ops[0]))) {
			return false;
		}
	}
	h->scale = scale;
	return true;
}

bool handles_update(struct handles *h, const struct rect *sel, int32_t lw,
	int32_t lh, double scale) {
	if (sel && scale != h->scale && !draw(h, scale)) {
		return false;
	}
	for (int i = 0; i < HANDLES; i++) {
		int32_t x = 0;
		int32_t y = 0;
		if (sel) {
			x = i & 1 ? sel->x + sel->w - 1 : sel->x;
			y = i & 2 ? sel->y + sel->h - 1 : sel->y;
		}
		// pulled inward at the output's edges, so they stay on screen
		sprite_move(h->dots[i],
			rect_clamp(x - HANDLE / 2, 0, lw - HANDLE),
			rect_clamp(y - HANDLE / 2, 0, lh - HANDLE),
			sel != NULL);
		if (sprite_dirty(h->dots[i])) {
			sprite_apply(h->dots[i]);
		}
	}
	return true;
}

void handles_destroy(struct handles *h) {
	if (!h) {
		return;
	}
	for (int i = HANDLES; i > 0; i--) {
		sprite_destroy(h->dots[i - 1]);
	}
	free(h);
}
