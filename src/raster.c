#include "raster.h"

#include <math.h>
#include <string.h>

enum {
	// every float coordinate is clamped to ±this before it becomes an int
	COORD_MAX = 1 << 24,
	// columns of coverage kept on the stack; wider clips go in chunks
	COV_MAX = 4096,
};

static int32_t ifloor(float v) {
	// NaN fails both comparisons and ends up at -COORD_MAX
	if (!(v > (float)-COORD_MAX)) {
		return -COORD_MAX;
	}
	if (v > (float)COORD_MAX) {
		return COORD_MAX;
	}
	int32_t i = (int32_t)v;
	return (float)i > v ? i - 1 : i;
}

static int32_t iceil(float v) {
	return -ifloor(-v);
}

static int32_t imin(int32_t a, int32_t b) {
	return a < b ? a : b;
}

static int32_t imax(int32_t a, int32_t b) {
	return a > b ? a : b;
}

int32_t raster_round(float v) {
	return ifloor(v + 0.5f);
}

struct rect raster_pixels(struct raster_box b) {
	int32_t l = ifloor(b.x0);
	int32_t t = ifloor(b.y0);
	int32_t r = iceil(b.x1);
	int32_t d = iceil(b.y1);
	if (r <= l || d <= t) {
		return (struct rect){0};
	}
	return (struct rect){l, t, r - l, d - t};
}

static uint32_t div255(uint32_t v) {
	v += 128;
	return (v + (v >> 8)) >> 8;
}

static uint32_t to_u8(float c) {
	if (!(c > 0.0f)) {
		return 0;
	}
	return c >= 1.0f ? 255 : (uint32_t)(c * 255.0f + 0.5f);
}

static uint8_t *row_at(const struct raster *t, int32_t y) {
	return t->data + (size_t)y * t->stride;
}

// src-over of an opaque colour with coverage c
static void blend(
	const struct raster *t, uint8_t *p, uint32_t color, uint32_t c) {
	if (c == 0) {
		return;
	}
	uint32_t k = 255 - c;
	p[0] = (uint8_t)div255((color & 0xff) * c + p[0] * k);
	p[1] = (uint8_t)div255(((color >> 8) & 0xff) * c + p[1] * k);
	p[2] = (uint8_t)div255(((color >> 16) & 0xff) * c + p[2] * k);
	p[3] = t->opaque ? 0xff : (uint8_t)div255(255 * c + p[3] * k);
}

// multiply keeps premultiplication valid and leaves alpha alone
static void multiply(uint8_t *p, uint32_t color, uint32_t c) {
	for (int i = 0; i < 3; i++) {
		uint32_t f = div255(c * (255 - ((color >> (8 * i)) & 0xff)));
		p[i] = (uint8_t)(p[i] - div255(p[i] * f));
	}
}

// static so the pen loop, which builds one per segment and row, inlines it
static struct raster_prim capsule(
	struct raster_point a, struct raster_point b, float r) {
	float m = r + 0.5f;
	return (struct raster_prim){
		.ax = a.x,
		.ay = a.y,
		.bx = b.x,
		.by = b.y,
		.r = r,
		.box = {fminf(a.x, b.x) - m, fminf(a.y, b.y) - m,
			fmaxf(a.x, b.x) + m, fmaxf(a.y, b.y) + m},
	};
}

struct raster_prim raster_capsule(
	struct raster_point a, struct raster_point b, float r) {
	return capsule(a, b, r);
}

struct raster_prim raster_triangle(const struct raster_point v[3]) {
	// the 0.5 px ramp on edges reaches further out at the vertices;
	// 1.5 px covers the arrow head's angles
	struct raster_prim p = {
		.tri = true,
		.box = {fminf(fminf(v[0].x, v[1].x), v[2].x) - 1.5f,
			fminf(fminf(v[0].y, v[1].y), v[2].y) - 1.5f,
			fmaxf(fmaxf(v[0].x, v[1].x), v[2].x) + 1.5f,
			fmaxf(fmaxf(v[0].y, v[1].y), v[2].y) + 1.5f},
	};
	for (int i = 0; i < 3; i++) {
		struct raster_point s = v[i];
		struct raster_point e = v[(i + 1) % 3];
		struct raster_point o = v[(i + 2) % 3];
		float nx = e.y - s.y;
		float ny = s.x - e.x;
		float len = sqrtf(nx * nx + ny * ny);
		if (len > 0.0f) {
			nx /= len;
			ny /= len;
		}
		if (nx * (o.x - s.x) + ny * (o.y - s.y) > 0.0f) {
			nx = -nx;
			ny = -ny;
		}
		p.edge[i][0] = nx;
		p.edge[i][1] = ny;
		p.edge[i][2] = nx * s.x + ny * s.y;
	}
	return p;
}

// columns [*a, *b) of row centre py that can have coverage
static void prim_span(
	const struct raster_prim *p, float py, int32_t *a, int32_t *b) {
	if (p->tri) {
		*a = ifloor(p->box.x0);
		*b = iceil(p->box.x1);
		return;
	}
	// a pixel within m of the segment is within m, horizontally, of a
	// segment point in the band py ± m
	float m = p->r + 0.5f;
	float dy = p->by - p->ay;
	float lo = fminf(p->ax, p->bx);
	float hi = fmaxf(p->ax, p->bx);
	if (fabsf(dy) > 1e-6f) {
		float t0 = (py - m - p->ay) / dy;
		float t1 = (py + m - p->ay) / dy;
		if (t0 > t1) {
			float s = t0;
			t0 = t1;
			t1 = s;
		}
		t0 = fmaxf(t0, 0.0f);
		t1 = fminf(t1, 1.0f);
		if (!(t0 <= t1)) {
			*a = *b = 0;
			return;
		}
		float xa = p->ax + t0 * (p->bx - p->ax);
		float xb = p->ax + t1 * (p->bx - p->ax);
		lo = fminf(xa, xb);
		hi = fmaxf(xa, xb);
	}
	*a = ifloor(lo - m);
	*b = iceil(hi + m);
}

static uint32_t prim_cov(const struct raster_prim *p, float px, float py) {
	if (p->tri) {
		float d = -INFINITY;
		for (int i = 0; i < 3; i++) {
			d = fmaxf(d, p->edge[i][0] * px + p->edge[i][1] * py -
					     p->edge[i][2]);
		}
		return to_u8(0.5f - d);
	}
	float dx = p->bx - p->ax;
	float dy = p->by - p->ay;
	float l2 = dx * dx + dy * dy;
	float t =
		l2 > 0.0f ? ((px - p->ax) * dx + (py - p->ay) * dy) / l2 : 0.0f;
	t = fminf(fmaxf(t, 0.0f), 1.0f);
	float ex = px - p->ax - t * dx;
	float ey = py - p->ay - t * dy;
	float d2 = ex * ex + ey * ey;
	float in = p->r - 0.5f;
	float out = p->r + 0.5f;
	// the square root only runs on the 1 px ramp
	if (in > 0.0f && d2 <= in * in) {
		return 255;
	}
	if (!(d2 < out * out)) {
		return 0;
	}
	return to_u8(out - sqrtf(d2));
}

// prims, or the capsules along a polyline
struct shape {
	const struct raster_prim *prims;
	size_t n;
	const struct raster_point *pts;
	size_t n_pts;
	float r;
};

static size_t shape_len(const struct shape *s) {
	if (!s->pts) {
		return s->n;
	}
	return s->n_pts > 1 ? s->n_pts - 1 : s->n_pts;
}

static struct raster_prim shape_prim(const struct shape *s, size_t i) {
	if (!s->pts) {
		return s->prims[i];
	}
	size_t j = i + 1 < s->n_pts ? i + 1 : i;
	return capsule(s->pts[i], s->pts[j], s->r);
}

static void fill_shape(const struct raster *t, struct rect clip, uint32_t color,
	const struct shape *s) {
	uint8_t cov[COV_MAX];
	size_t n = shape_len(s);
	int32_t end = clip.x + clip.w;
	for (int32_t cx = clip.x; cx < end; cx += COV_MAX) {
		int32_t cx1 = imin(cx + COV_MAX, end);
		for (int32_t y = clip.y; y < clip.y + clip.h; y++) {
			float py = (float)y + 0.5f;
			int32_t lo = cx1;
			int32_t hi = cx1;
			for (size_t i = 0; i < n; i++) {
				struct raster_prim p = shape_prim(s, i);
				if (!(py >= p.box.y0 && py <= p.box.y1)) {
					continue;
				}
				int32_t a;
				int32_t b;
				prim_span(&p, py, &a, &b);
				a = imax(a, cx);
				b = imin(b, cx1);
				if (a >= b) {
					continue;
				}
				// columns join the run zeroed
				if (lo >= hi) {
					memset(cov + (a - cx), 0,
						(size_t)(b - a));
					lo = a;
					hi = b;
				}
				if (a < lo) {
					memset(cov + (a - cx), 0,
						(size_t)(lo - a));
					lo = a;
				}
				if (b > hi) {
					memset(cov + (hi - cx), 0,
						(size_t)(b - hi));
					hi = b;
				}
				for (int32_t x = a; x < b; x++) {
					uint8_t *c = &cov[x - cx];
					if (*c == 255) {
						continue;
					}
					uint32_t v = prim_cov(
						&p, (float)x + 0.5f, py);
					if (v > *c) {
						*c = (uint8_t)v;
					}
				}
			}
			uint8_t *row = row_at(t, y);
			for (int32_t x = lo; x < hi; x++) {
				blend(t, row + (size_t)x * 4, color,
					cov[x - cx]);
			}
		}
	}
}

void raster_prims(const struct raster *t, struct rect clip, uint32_t color,
	const struct raster_prim *prims, size_t n) {
	struct shape s = {.prims = prims, .n = n};
	fill_shape(t, clip, color, &s);
}

void raster_polyline(const struct raster *t, struct rect clip, uint32_t color,
	const struct raster_point *pts, size_t n, float r) {
	struct shape s = {.pts = pts, .n_pts = n, .r = r};
	fill_shape(t, clip, color, &s);
}

// length of [a0, a1] inside the pixel [p, p + 1]
static float overlap(float a0, float a1, int32_t p) {
	float lo = fmaxf(a0, (float)p);
	float hi = fminf(a1, (float)p + 1.0f);
	return hi > lo ? hi - lo : 0.0f;
}

// exact area coverage: outer fill minus inner fill
void raster_rect(const struct raster *t, struct rect clip, struct raster_box b,
	float w, uint32_t color, enum raster_mode mode) {
	struct rect r = rect_intersect(raster_pixels(b), clip);
	struct raster_box in = {b.x0 + w, b.y0 + w, b.x1 - w, b.y1 - w};
	bool ring = mode == RASTER_OUTLINE && in.x0 < in.x1 && in.y0 < in.y1;
	int32_t skip0 = ring ? iceil(in.x0) : 0;
	int32_t skip1 = ring ? ifloor(in.x1) : 0;
	for (int32_t y = r.y; y < r.y + r.h; y++) {
		float oy = overlap(b.y0, b.y1, y);
		float iy = ring ? overlap(in.y0, in.y1, y) : 0.0f;
		uint8_t *row = row_at(t, y);
		for (int32_t x = r.x; x < r.x + r.w; x++) {
			// fully inside the inner rect: nothing to draw
			if (iy >= 1.0f && x >= skip0 && x < skip1) {
				x = skip1 - 1;
				continue;
			}
			float c = overlap(b.x0, b.x1, x) * oy;
			if (ring) {
				c -= overlap(in.x0, in.x1, x) * iy;
			}
			uint8_t *p = row + (size_t)x * 4;
			if (mode == RASTER_MULTIPLY) {
				multiply(p, color, to_u8(c));
			} else {
				blend(t, p, color, to_u8(c));
			}
		}
	}
}

struct ellipse {
	float cx;
	float cy;
	float rx;
	float ry;
};

// fill coverage from the signed distance estimate f / |∇f|
static float ellipse_cov(const struct ellipse *e, float px, float py) {
	if (!(e->rx > 0.0f && e->ry > 0.0f)) {
		return 0.0f;
	}
	float dx = px - e->cx;
	float dy = py - e->cy;
	float ix = 1.0f / (e->rx * e->rx);
	float iy = 1.0f / (e->ry * e->ry);
	float f = dx * dx * ix + dy * dy * iy - 1.0f;
	float gx = 2.0f * dx * ix;
	float gy = 2.0f * dy * iy;
	float g2 = gx * gx + gy * gy;
	// |distance| >= 0.5 needs no square root
	if (f * f >= 0.25f * g2) {
		return f < 0.0f ? 1.0f : 0.0f;
	}
	return 0.5f - f / sqrtf(g2);
}

// half the width of an ellipse with radii rx, ry at dy from its centre,
// or a negative value off it
static float ellipse_half(float rx, float ry, float dy) {
	if (!(rx > 0.0f && ry > 0.0f && fabsf(dy) < ry)) {
		return -1.0f;
	}
	return rx * sqrtf(1.0f - dy * dy / (ry * ry));
}

void raster_ellipse(const struct raster *t, struct rect clip,
	struct raster_box b, float w, uint32_t color, enum raster_mode mode) {
	if (!(b.x1 - b.x0 >= 1.0f && b.y1 - b.y0 >= 1.0f)) {
		return;
	}
	struct ellipse o = {(b.x0 + b.x1) / 2.0f, (b.y0 + b.y1) / 2.0f,
		(b.x1 - b.x0) / 2.0f, (b.y1 - b.y0) / 2.0f};
	struct ellipse in = {o.cx, o.cy, o.rx - w, o.ry - w};
	bool ring = mode == RASTER_OUTLINE && in.rx > 0.0f && in.ry > 0.0f;
	for (int32_t y = clip.y; y < clip.y + clip.h; y++) {
		float py = (float)y + 0.5f;
		float dy = py - o.cy;
		float half = ellipse_half(o.rx + 1.0f, o.ry + 1.0f, dy);
		if (half < 0.0f) {
			continue;
		}
		int32_t a = imax(ifloor(o.cx - half), clip.x);
		int32_t e = imin(iceil(o.cx + half), clip.x + clip.w);
		// columns well inside the inner ellipse stay untouched
		int32_t s0 = e;
		int32_t s1 = e;
		float ih = ring ? ellipse_half(in.rx - 1.0f, in.ry - 1.0f, dy)
				: -1.0f;
		if (ih >= 0.0f) {
			s0 = imax(iceil(o.cx - ih), a);
			s1 = imin(ifloor(o.cx + ih), e);
		}
		if (s1 <= s0) {
			s0 = s1 = e;
		}
		uint8_t *row = row_at(t, y);
		for (int32_t x = a; x < e; x++) {
			if (x == s0) {
				x = s1 - 1;
				continue;
			}
			float px = (float)x + 0.5f;
			float c = ellipse_cov(&o, px, py);
			if (ring) {
				c -= ellipse_cov(&in, px, py);
			}
			blend(t, row + (size_t)x * 4, color, to_u8(c));
		}
	}
}
