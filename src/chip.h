#ifndef SWEETSHOT_CHIP_H
#define SWEETSHOT_CHIP_H

#include "draw.h"
#include "wl.h"

// what the chip shows: the current tool in its colour and size
struct chip_look {
	enum draw_kind kind;
	uint32_t color;
	float size; // logical px
	bool fill;
	uint32_t badge; // the next badge's number
};

struct chip;

// a small subsurface of parent that follows the pointer; hidden at first.
// Reports the error and returns NULL on failure
struct chip *chip_create(
	struct conn *c, struct wl_surface *parent, struct wl_region *no_input);

bool chip_set(struct chip *k, const struct chip_look *look, double scale);

// next to the pointer at px, py on a lw×lh parent, flipped near its edges
void chip_move(struct chip *k, double px, double py, int32_t lw, int32_t lh,
	bool shown);

// something changed since the last chip_apply
bool chip_dirty(const struct chip *k);

// sends the changes; they land with the parent's next commit
void chip_apply(struct chip *k);

void chip_destroy(struct chip *k);

#endif
