#ifndef SWEETSHOT_OVERLAY_H
#define SWEETSHOT_OVERLAY_H

#include "rect.h"
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

struct handles;

struct handles *handles_create(
	struct conn *c, struct wl_surface *parent, struct wl_region *no_input);

bool handles_update(struct handles *h, const struct rect *sel, int32_t lw,
	int32_t lh, double scale);

void handles_destroy(struct handles *h);

#endif
