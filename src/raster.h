#ifndef SWEETSHOT_RASTER_H
#define SWEETSHOT_RASTER_H

#include "rect.h"

#include <stddef.h>

struct raster_point {
	float x;
	float y;
};

// [x0, x1] × [y0, y1] in pixels
struct raster_box {
	float x0;
	float y0;
	float x1;
	float y1;
};

// premultiplied B,G,R,A pixels (wl_shm order). Opaque (XRGB) targets get
// alpha 0xff wherever something is blended
struct raster {
	uint8_t *data;
	size_t stride;
	bool opaque;
};

// a capsule (segment a–b with round caps, radius r) or a triangle; its
// coverage is zero outside box
struct raster_prim {
	bool tri;
	float ax;
	float ay;
	float bx;
	float by;
	float r;
	float edge[3][3]; // triangle
	struct raster_box box;
};

enum raster_mode {
	RASTER_OUTLINE, // a band of the given width inside the box
	RASTER_FILL,
	RASTER_MULTIPLY, // fill, multiplied with what is there
};

struct raster_prim raster_capsule(
	struct raster_point a, struct raster_point b, float r);

struct raster_prim raster_triangle(const struct raster_point v[3]);

// the pixels b touches, empty for an empty or non-finite box
struct rect raster_pixels(struct raster_box b);

// nearest integer, clamped far beyond any image, NaN-safe
int32_t raster_round(float v);

// the union of prims, blended once with an opaque colour (0xRRGGBB)
void raster_prims(const struct raster *t, struct rect clip, uint32_t color,
	const struct raster_prim *prims, size_t n);

// the union of radius-r capsules along pts (a dot for one point)
void raster_polyline(const struct raster *t, struct rect clip, uint32_t color,
	const struct raster_point *pts, size_t n, float r);

// box-filtered rectangle, the outline is w wide, inside b
void raster_rect(const struct raster *t, struct rect clip, struct raster_box b,
	float w, uint32_t color, enum raster_mode mode);

void raster_ellipse(const struct raster *t, struct rect clip,
	struct raster_box b, float w, uint32_t color, enum raster_mode mode);

#endif
