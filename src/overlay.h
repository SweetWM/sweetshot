#ifndef SWEETSHOT_OVERLAY_H
#define SWEETSHOT_OVERLAY_H

#include "wl.h"

struct zwlr_layer_surface_v1;
struct zwlr_layer_surface_v1_listener;

// a full-output overlay with exclusive keyboard, over panels too; the
// commit without a buffer asks for the first configure
void overlay_map(struct zwlr_layer_surface_v1 *layer,
	struct wl_surface *surface,
	const struct zwlr_layer_surface_v1_listener *l, void *data);

struct catchers;

// clear, click-through, keyboard-exclusive layers on every output but
// skip. Reports the error and returns NULL on failure
struct catchers *catchers_create(struct conn *c, const struct output *skip);

void catchers_destroy(struct catchers *k);

#endif
