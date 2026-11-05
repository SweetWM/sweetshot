#include "draw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const uint32_t draw_palette[DRAW_COLORS] = {
	0xf03e3e,
	0xfd7e14,
	0xfcc419,
	0x37b24d,
	0x1c7ed6,
	0x7048e8,
	0xe64980,
	0xffffff,
	0x000000,
};

const float draw_sizes[DRAW_SIZES] = {2, 3, 4, 6, 8, 12, 16};

enum {
	PRIMS_MAX = 40,
	BADGE_MAX = 99,
	BLOCK_MAX = 4096,
	SLACK = 2,
};

// digit polylines on a 4×7 grid in half units; BREAK starts a new polyline
enum { BREAK = -1, END = -2 };

static const int8_t glyphs[10][40] = {
	{2, 0, 6, 0, 8, 2, 8, 12, 6, 14, 2, 14, 0, 12, 0, 2, 2, 0, END},
	{2, 2, 4, 0, 4, 14, BREAK, 2, 14, 6, 14, END},
	{0, 2, 2, 0, 6, 0, 8, 2, 8, 6, 0, 14, 8, 14, END},
	{0, 2, 2, 0, 6, 0, 8, 2, 8, 5, 6, 7, 3, 7, BREAK, 6, 7, 8, 9, 8, 12, 6,
		14, 2, 14, 0, 12, END},
	{6, 14, 6, 0, 0, 10, 8, 10, END},
	{8, 0, 0, 0, 0, 6, 6, 6, 8, 8, 8, 12, 6, 14, 0, 14, END},
	{7, 0, 2, 0, 0, 2, 0, 12, 2, 14, 6, 14, 8, 12, 8, 8, 6, 6, 0, 6, END},
	{0, 0, 8, 0, 2, 14, END},
	{2, 0, 6, 0, 8, 2, 8, 5, 6, 7, 2, 7, 0, 5, 0, 2, 2, 0, BREAK, 2, 7, 0,
		9, 0, 12, 2, 14, 6, 14, 8, 12, 8, 9, 6, 7, END},
	{8, 8, 2, 8, 0, 6, 0, 2, 2, 0, 6, 0, 8, 2, 8, 12, 6, 14, 1, 14, END},
};

static size_t badge_digits(struct raster_point c, float diameter, uint32_t num,
	struct raster_prim *out) {
	uint32_t digits[2] = {num / 10, num % 10};
	int nd = num >= 10 ? 2 : 1;
	float h = diameter * (nd == 2 ? 0.42f : 0.5f);
	float u = h / 7.0f;
	float x = c.x - ((float)nd * 4.0f + (float)(nd - 1) * 1.5f) * u / 2.0f;
	float y = c.y - h / 2.0f;
	size_t n = 0;
	for (int d = 2 - nd; d < 2; d++) {
		const int8_t *g = glyphs[digits[d]];
		bool have = false;
		struct raster_point prev = {0};
		for (int i = 0; g[i] != END; i++) {
			if (g[i] == BREAK) {
				have = false;
				continue;
			}
			struct raster_point p = {x + (float)g[i] * u / 2.0f,
				y + (float)g[i + 1] * u / 2.0f};
			i++;
			if (have && n < PRIMS_MAX) {
				out[n++] = raster_capsule(prev, p, u / 2.0f);
			}
			prev = p;
			have = true;
		}
		x += 5.5f * u;
	}
	return n;
}

// dark digits on light badges
static uint32_t contrast(uint32_t color) {
	uint32_t luma =
		(299 * ((color >> 16) & 0xff) + 587 * ((color >> 8) & 0xff) +
			114 * (color & 0xff)) /
		1000;
	return luma > 170 ? 0x000000 : 0xffffff;
}

// line, arrow and badge circle
static size_t op_prims(const struct draw_op *op, struct raster_prim *out) {
	float w = op->width;
	if (op->kind == DRAW_BADGE) {
		out[0] = raster_capsule(op->a, op->a, 3.0f * w);
		return 1;
	}
	float dx = op->b.x - op->a.x;
	float dy = op->b.y - op->a.y;
	float len = sqrtf(dx * dx + dy * dy);
	if (op->kind == DRAW_LINE || !(len > 0.001f)) {
		out[0] = raster_capsule(op->a, op->b, w / 2.0f);
		return 1;
	}
	// the shaft ends at the head's base
	float head = fminf(4.0f * w, len);
	float half = head * 0.625f;
	float ux = dx / len;
	float uy = dy / len;
	struct raster_point base = {op->b.x - ux * head, op->b.y - uy * head};
	struct raster_point v[3] = {
		op->b,
		{base.x - uy * half, base.y + ux * half},
		{base.x + uy * half, base.y - ux * half},
	};
	out[0] = raster_capsule(op->a, base, w / 2.0f);
	out[1] = raster_triangle(v);
	return 2;
}

// the dragged box, in either direction
static struct raster_box drag_box(const struct draw_op *op) {
	return (struct raster_box){
		fminf(op->a.x, op->b.x),
		fminf(op->a.y, op->b.y),
		fmaxf(op->a.x, op->b.x),
		fmaxf(op->a.y, op->b.y),
	};
}

static struct rect pixelate_area(const struct draw_op *op) {
	struct raster_box f = drag_box(op);
	int32_t x0 = raster_round(f.x0);
	int32_t y0 = raster_round(f.y0);
	return (struct rect){
		x0, y0, raster_round(f.x1) - x0, raster_round(f.y1) - y0};
}

static struct raster_box grow(struct raster_box a, struct raster_box b) {
	return (struct raster_box){fminf(a.x0, b.x0), fminf(a.y0, b.y0),
		fmaxf(a.x1, b.x1), fmaxf(a.y1, b.y1)};
}

struct rect draw_bounds(const struct draw_op *op) {
	struct raster_box f = drag_box(op);
	switch (op->kind) {
	case DRAW_ARROW:
	case DRAW_LINE:
	case DRAW_BADGE: {
		struct raster_prim p[2];
		size_t n = op_prims(op, p);
		f = p[0].box;
		for (size_t i = 1; i < n; i++) {
			f = grow(f, p[i].box);
		}
		break;
	}
	case DRAW_PEN:
		if (op->n_pts == 0) {
			return (struct rect){0};
		}
		f = raster_capsule(
			(struct raster_point){op->span.x0, op->span.y0},
			(struct raster_point){op->span.x1, op->span.y1},
			op->width / 2.0f)
			    .box;
		break;
	case DRAW_ELLIPSE:
		f = (struct raster_box){
			f.x0 - 1.0f, f.y0 - 1.0f, f.x1 + 1.0f, f.y1 + 1.0f};
		break;
	case DRAW_PIXELATE: {
		struct rect r = pixelate_area(op);
		return r.w > 0 && r.h > 0 ? r : (struct rect){0};
	}
	case DRAW_RECT:
	case DRAW_HIGHLIGHT:
		break;
	}
	return raster_pixels(f);
}

// the longest step from a along d (a unit vector) that stays in the image
static float room(struct raster_point a, float dx, float dy, float w, float h) {
	float t = INFINITY;
	if (dx != 0.0f) {
		t = fminf(t, (dx > 0.0f ? w - a.x : -a.x) / dx);
	}
	if (dy != 0.0f) {
		t = fminf(t, (dy > 0.0f ? h - a.y : -a.y) / dy);
	}
	return fmaxf(t, 0.0f);
}

static struct raster_point snap45(
	struct raster_point a, struct raster_point b, float w, float h) {
	const float s = 0.70710678f;
	const float dirs[8][2] = {{1, 0}, {s, s}, {0, 1}, {-s, s}, {-1, 0},
		{-s, -s}, {0, -1}, {s, -s}};
	float dx = b.x - a.x;
	float dy = b.y - a.y;
	float best = 0.0f;
	float ux = 0.0f;
	float uy = 0.0f;
	for (int i = 0; i < 8; i++) {
		float d = dx * dirs[i][0] + dy * dirs[i][1];
		if (d > best) {
			best = d;
			ux = dirs[i][0];
			uy = dirs[i][1];
		}
	}
	float len = fminf(best, room(a, ux, uy, w, h));
	return (struct raster_point){a.x + ux * len, a.y + uy * len};
}

struct raster_point draw_constrain(enum draw_kind kind, struct raster_point a,
	struct raster_point b, float w, float h) {
	if (kind == DRAW_LINE || kind == DRAW_ARROW) {
		return snap45(a, b, w, h);
	}
	if (kind == DRAW_PEN || kind == DRAW_BADGE) {
		return b;
	}
	float dx = b.x - a.x;
	float dy = b.y - a.y;
	float side = fmaxf(fabsf(dx), fabsf(dy));
	side = fminf(side, dx >= 0.0f ? w - a.x : a.x);
	side = fminf(side, dy >= 0.0f ? h - a.y : a.y);
	side = fmaxf(side, 0.0f);
	return (struct raster_point){a.x + (dx >= 0.0f ? side : -side),
		a.y + (dy >= 0.0f ? side : -side)};
}

static void draw_pixelate(const struct raster *t, const struct png_image *src,
	const struct draw_op *op, struct rect clip) {
	struct rect area = pixelate_area(op);
	struct rect img = {0, 0, (int32_t)src->width, (int32_t)src->height};
	struct rect inside = rect_intersect(area, img);
	struct rect r = rect_intersect(inside, clip);
	if (r.w == 0) {
		return;
	}
	int32_t bs = raster_round(4.0f * op->width);
	bs = bs < 1 ? 1 : bs > BLOCK_MAX ? BLOCK_MAX : bs;
	int32_t bx0 = area.x + (r.x - area.x) / bs * bs;
	int32_t by0 = area.y + (r.y - area.y) / bs * bs;
	for (int32_t by = by0; by < r.y + r.h; by += bs) {
		for (int32_t bx = bx0; bx < r.x + r.w; bx += bs) {
			struct rect blk = rect_intersect(
				(struct rect){bx, by, bs, bs}, inside);
			if (blk.w == 0) {
				continue;
			}
			uint64_t sum[4] = {0};
			for (int32_t y = blk.y; y < blk.y + blk.h; y++) {
				const uint8_t *s = src->data +
						   (size_t)y * src->stride +
						   (size_t)blk.x * 4;
				for (int32_t x = 0; x < blk.w; x++, s += 4) {
					sum[0] += s[0];
					sum[1] += s[1];
					sum[2] += s[2];
					sum[3] += s[3];
				}
			}
			uint64_t n = (uint64_t)blk.w * (uint64_t)blk.h;
			uint8_t mean[4];
			for (int i = 0; i < 4; i++) {
				mean[i] = (uint8_t)((sum[i] + n / 2) / n);
			}
			if (t->opaque) {
				mean[3] = 0xff;
			}
			struct rect wr = rect_intersect(blk, clip);
			for (int32_t y = wr.y; y < wr.y + wr.h; y++) {
				uint8_t *p = t->data + (size_t)y * t->stride +
					     (size_t)wr.x * 4;
				for (int32_t x = 0; x < wr.w; x++, p += 4) {
					memcpy(p, mean, 4);
				}
			}
		}
	}
}

static void draw_one(const struct raster *t, const struct png_image *src,
	const struct draw_op *op, struct rect clip, uint32_t *badges) {
	// badge numbers count every badge before this one, drawn or not
	uint32_t num = op->kind == DRAW_BADGE ? ++*badges : 0;
	struct rect b = draw_bounds(op);
	if (b.w == 0) {
		return;
	}
	// the slack keeps a too-small draw_bounds() visible (pixels changed
	// outside it) instead of silently cutting the shape's edge
	struct rect r =
		rect_intersect((struct rect){b.x - SLACK, b.y - SLACK,
				       b.w + 2 * SLACK, b.h + 2 * SLACK},
			clip);
	if (r.w == 0) {
		return;
	}
	struct raster_prim p[PRIMS_MAX];
	enum raster_mode fill = op->fill ? RASTER_FILL : RASTER_OUTLINE;
	switch (op->kind) {
	case DRAW_ARROW:
	case DRAW_LINE:
	case DRAW_BADGE:
		raster_prims(t, r, op->color, p, op_prims(op, p));
		if (num > 0 && num <= BADGE_MAX) {
			size_t n =
				badge_digits(op->a, 6.0f * op->width, num, p);
			raster_prims(t, r, contrast(op->color), p, n);
		}
		break;
	case DRAW_PEN:
		raster_polyline(
			t, r, op->color, op->pts, op->n_pts, op->width / 2.0f);
		break;
	case DRAW_RECT:
		raster_rect(t, r, drag_box(op), op->width, op->color, fill);
		break;
	case DRAW_HIGHLIGHT:
		raster_rect(t, r, drag_box(op), op->width, op->color,
			RASTER_MULTIPLY);
		break;
	case DRAW_ELLIPSE:
		raster_ellipse(t, r, drag_box(op), op->width, op->color, fill);
		break;
	case DRAW_PIXELATE:
		draw_pixelate(t, src, op, r);
		break;
	}
}

void draw_render(const struct png_image *src, const struct draw_op *ops,
	size_t n, const struct draw_op *live, uint8_t *dst, size_t dst_stride,
	struct rect clip) {
	if (src->width > INT32_MAX || src->height > INT32_MAX) {
		return;
	}
	struct rect img = {0, 0, (int32_t)src->width, (int32_t)src->height};
	clip = rect_intersect(clip, img);
	if (clip.w == 0) {
		return;
	}
	for (int32_t y = clip.y; y < clip.y + clip.h; y++) {
		memcpy(dst + (size_t)y * dst_stride + (size_t)clip.x * 4,
			src->data + (size_t)y * src->stride +
				(size_t)clip.x * 4,
			(size_t)clip.w * 4);
	}
	struct raster t = {
		.data = dst,
		.stride = dst_stride,
		.opaque = src->format == PNG_XRGB8888,
	};
	uint32_t badges = 0;
	for (size_t i = 0; i < n; i++) {
		draw_one(&t, src, &ops[i], clip, &badges);
	}
	if (live) {
		draw_one(&t, src, live, clip, &badges);
	}
}

// the first point resets the span, so a copied op can be refilled
static void span_add(struct draw_op *op, struct raster_point p) {
	struct raster_box *s = &op->span;
	if (op->n_pts == 0) {
		*s = (struct raster_box){p.x, p.y, p.x, p.y};
		return;
	}
	s->x0 = fminf(s->x0, p.x);
	s->y0 = fminf(s->y0, p.y);
	s->x1 = fmaxf(s->x1, p.x);
	s->y1 = fmaxf(s->y1, p.y);
}

void draw_pen_borrow(struct draw_op *op, struct raster_point *pts, size_t n) {
	op->pts = pts;
	op->n_pts = 0;
	op->cap_pts = 0;
	for (size_t i = 0; i < n; i++) {
		span_add(op, pts[i]);
		op->n_pts++;
	}
}

bool draw_pen_add(struct draw_op *op, struct raster_point p) {
	if (op->n_pts == op->cap_pts) {
		size_t cap = op->cap_pts ? op->cap_pts * 2 : 64;
		struct raster_point *pts =
			reallocarray(op->pts, cap, sizeof(*pts));
		if (!pts) {
			return false;
		}
		op->pts = pts;
		op->cap_pts = cap;
	}
	span_add(op, p);
	op->pts[op->n_pts++] = p;
	return true;
}

void draw_op_finish(struct draw_op *op) {
	free(op->pts);
	op->pts = NULL;
	op->n_pts = 0;
	op->cap_pts = 0;
}

bool draw_ops_push(struct draw_ops *ops, const struct draw_op *op) {
	for (size_t i = ops->n_active; i < ops->n; i++) {
		draw_op_finish(&ops->ops[i]);
	}
	ops->n = ops->n_active;
	if (ops->n == ops->cap) {
		size_t cap = ops->cap ? ops->cap * 2 : 16;
		struct draw_op *a = reallocarray(ops->ops, cap, sizeof(*a));
		if (!a) {
			return false;
		}
		ops->ops = a;
		ops->cap = cap;
	}
	ops->ops[ops->n++] = *op;
	ops->n_active = ops->n;
	return true;
}

void draw_ops_finish(struct draw_ops *ops) {
	for (size_t i = 0; i < ops->n; i++) {
		draw_op_finish(&ops->ops[i]);
	}
	free(ops->ops);
	*ops = (struct draw_ops){0};
}
