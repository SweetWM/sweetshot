#ifndef SWEETSHOT_SPRITE_H
#define SWEETSHOT_SPRITE_H

#include "draw.h"
#include "wl.h"

// a subsurface showing a picture drawn once from draw ops, at a position,
// shown or hidden. Every change lands with the parent's next commit
struct sprite;

// hidden and empty at first; reports the error and returns NULL on failure
struct sprite *sprite_create(
	struct conn *c, struct wl_surface *parent, struct wl_region *no_input);

bool sprite_draw(struct sprite *s, int32_t w, int32_t h, uint32_t pw,
	uint32_t ph, uint32_t bg, const struct draw_op *ops, size_t n);

// position in the parent, logical
void sprite_move(struct sprite *s, int32_t x, int32_t y, bool shown);

// something changed since the last sprite_apply
bool sprite_dirty(const struct sprite *s);

void sprite_apply(struct sprite *s);

void sprite_destroy(struct sprite *s);

#endif
