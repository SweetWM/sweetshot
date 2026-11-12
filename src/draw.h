#ifndef SWEETSHOT_DRAW_H
#define SWEETSHOT_DRAW_H

#include "png.h"
#include "raster.h"

enum draw_kind {
	DRAW_ARROW,
	DRAW_LINE,
	DRAW_RECT,
	DRAW_ELLIPSE,
	DRAW_PEN,
	DRAW_HIGHLIGHT,
	DRAW_PIXELATE,
	DRAW_BADGE,
};

enum {
	DRAW_COLORS = 9,
	DRAW_SIZES = 7,
};

// keys 1–9, straight sRGB 0xRRGGBB
extern const uint32_t draw_palette[DRAW_COLORS];
// stroke widths in logical px, stepped by - and =
extern const float draw_sizes[DRAW_SIZES];

// one annotation; coordinates and width are image pixels
struct draw_op {
	enum draw_kind kind;
	uint32_t color; // straight sRGB 0xRRGGBB
	float width;
	bool fill;		  // rect and ellipse
	struct raster_point a;	  // drag start, badge centre
	struct raster_point b;	  // drag end
	struct raster_point *pts; // pen only, owned by the op
	size_t n_pts;
	size_t cap_pts;
	struct raster_box span;
};

// ops[0, n_active) are drawn, ops[n_active, n) is the redo tail
struct draw_ops {
	struct draw_op *ops;
	size_t n;
	size_t n_active;
	size_t cap;
};

// false when out of memory; the op is unchanged then
bool draw_pen_add(struct draw_op *op, struct raster_point p);

void draw_pen_borrow(struct draw_op *op, struct raster_point *pts, size_t n);

void draw_op_finish(struct draw_op *op);

bool draw_ops_push(struct draw_ops *ops, const struct draw_op *op);

void draw_ops_finish(struct draw_ops *ops);

struct rect draw_bounds(const struct draw_op *op);

struct raster_point draw_constrain(enum draw_kind kind, struct raster_point a,
	struct raster_point b, float w, float h);

void draw_render(const struct png_image *src, const struct draw_op *ops,
	size_t n, const struct draw_op *live, uint8_t *dst, size_t dst_stride,
	struct rect clip);

#endif
