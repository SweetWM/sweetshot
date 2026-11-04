#ifndef SWEETSHOT_OVERLAY_H
#define SWEETSHOT_OVERLAY_H

#include "wl.h"

struct zwlr_layer_surface_v1;
struct zwlr_layer_surface_v1_listener;

enum { OVERLAY_DIM_ALPHA = 0x73333333 };

void overlay_map(struct zwlr_layer_surface_v1 *layer,
	struct wl_surface *surface,
	const struct zwlr_layer_surface_v1_listener *l, void *data);

struct catchers;

struct catchers *catchers_create(struct conn *c, const struct output *skip);

void catchers_destroy(struct catchers *k);

#endif
