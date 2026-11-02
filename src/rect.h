#ifndef SWEETSHOT_RECT_H
#define SWEETSHOT_RECT_H

#include <stdbool.h>
#include <stdint.h>

struct rect {
	int32_t x;
	int32_t y;
	int32_t w;
	int32_t h;
};

enum {
	RECT_DIMS = 4,	// dim above, below, left and right of the selection
	RECT_PARTS = 8, // then outline top, bottom, left and right
};

// the dim and outline rectangles of a w×h output, sel NULL dims all of it.
// Empty rectangles stay unmapped
void rect_layout(
	int32_t w, int32_t h, const struct rect *sel, struct rect parts[]);

struct rect rect_nudge(struct rect r, int32_t dx, int32_t dy, bool resize,
	int32_t w, int32_t h);

// normalized rectangle between two points, reverse drags included
struct rect rect_span(int32_t ax, int32_t ay, int32_t bx, int32_t by);

struct rect rect_intersect(struct rect a, struct rect b);

// pixel rectangle of logical r in the upright iw×ih capture of an lw×lh
// output
struct rect rect_physical(
	struct rect r, int32_t lw, int32_t lh, uint32_t iw, uint32_t ih);

#endif
