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
	RECT_DIMS = 4,
	RECT_PARTS = 8,
	RECT_CLICK_SLOP = 3,
};

int32_t rect_clamp(int32_t v, int32_t lo, int32_t hi);

// the dim and outline rectangles of a w×h output, sel NULL dims all of it.
// Empty rectangles stay unmapped
void rect_layout(
	int32_t w, int32_t h, const struct rect *sel, struct rect parts[]);

struct rect rect_nudge(struct rect r, int32_t dx, int32_t dy, bool resize,
	int32_t w, int32_t h);

// normalized rectangle between two points, reverse drags included
struct rect rect_span(int32_t ax, int32_t ay, int32_t bx, int32_t by);

struct rect rect_intersect(struct rect a, struct rect b);

// the smallest rectangle holding a and b; an empty one is ignored
struct rect rect_union(struct rect a, struct rect b);

// pixel rectangle of logical r in the upright iw×ih capture of an lw×lh
// output
struct rect rect_physical(
	struct rect r, int32_t lw, int32_t lh, uint32_t iw, uint32_t ih);

struct place {
	struct rect at;
	double sx;
	double sy;
	double ox;
	double oy;
};

struct place rect_place_crop(
	struct rect sel, int32_t lw, int32_t lh, uint32_t iw, uint32_t ih);

struct place rect_place_center(uint32_t iw, uint32_t ih, int32_t lw, int32_t lh,
	int32_t pw, int32_t ph);

#endif
