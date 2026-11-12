#include "rect.h"

int32_t rect_clamp(int32_t v, int32_t lo, int32_t hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

void rect_layout(
	int32_t w, int32_t h, const struct rect *sel, struct rect parts[]) {
	for (int i = 0; i < RECT_PARTS; i++) {
		parts[i] = (struct rect){0};
	}
	if (!sel) {
		parts[0] = (struct rect){0, 0, w, h};
		return;
	}
	struct rect s = *sel;
	int32_t right = s.x + s.w;
	int32_t bottom = s.y + s.h;
	parts[0] = (struct rect){0, 0, w, s.y};
	parts[1] = (struct rect){0, bottom, w, h - bottom};
	parts[2] = (struct rect){0, s.y, s.x, s.h};
	parts[3] = (struct rect){right, s.y, w - right, s.h};
	// a drag that has not moved yet has no outline
	if (s.w > 0 && s.h > 0) {
		parts[4] = (struct rect){s.x, s.y, s.w, 1};
		parts[5] = (struct rect){s.x, bottom - 1, s.w, 1};
		parts[6] = (struct rect){s.x, s.y, 1, s.h};
		parts[7] = (struct rect){right - 1, s.y, 1, s.h};
	}
}

struct rect rect_nudge(struct rect r, int32_t dx, int32_t dy, bool resize,
	int32_t w, int32_t h) {
	if (resize) {
		r.w = rect_clamp(r.w + dx, 1, w - r.x);
		r.h = rect_clamp(r.h + dy, 1, h - r.y);
	} else {
		r.x = rect_clamp(r.x + dx, 0, w - r.w);
		r.y = rect_clamp(r.y + dy, 0, h - r.h);
	}
	return r;
}

struct rect rect_span(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
	return (struct rect){
		.x = ax < bx ? ax : bx,
		.y = ay < by ? ay : by,
		.w = ax < bx ? bx - ax : ax - bx,
		.h = ay < by ? by - ay : ay - by,
	};
}

// a thin side keeps a third of itself for the inside
static uint32_t grab_axis(
	int32_t v, int32_t lo, int32_t len, uint32_t low, uint32_t high) {
	int32_t in = len / 3 < RECT_GRAB_IN ? len / 3 : RECT_GRAB_IN;
	if (v < lo + in) {
		return low;
	}
	return v >= lo + len - in ? high : 0;
}

uint32_t rect_grab(struct rect sel, int32_t x, int32_t y) {
	if (sel.w <= 0 || sel.h <= 0 || x < sel.x - RECT_GRAB_OUT ||
		x >= sel.x + sel.w + RECT_GRAB_OUT ||
		y < sel.y - RECT_GRAB_OUT ||
		y >= sel.y + sel.h + RECT_GRAB_OUT) {
		return 0;
	}
	return grab_axis(x, sel.x, sel.w, RECT_LEFT, RECT_RIGHT) |
	       grab_axis(y, sel.y, sel.h, RECT_TOP, RECT_BOTTOM);
}

struct rect rect_resize(struct rect from, uint32_t edges, int32_t dx,
	int32_t dy, int32_t w, int32_t h) {
	int32_t x0 = from.x;
	int32_t y0 = from.y;
	int32_t x1 = from.x + from.w;
	int32_t y1 = from.y + from.h;
	if (edges & RECT_LEFT) {
		x0 = rect_clamp(x0 + dx, 0, w);
	}
	if (edges & RECT_RIGHT) {
		x1 = rect_clamp(x1 + dx, 0, w);
	}
	if (edges & RECT_TOP) {
		y0 = rect_clamp(y0 + dy, 0, h);
	}
	if (edges & RECT_BOTTOM) {
		y1 = rect_clamp(y1 + dy, 0, h);
	}
	return rect_span(x0, y0, x1, y1);
}

struct rect rect_intersect(struct rect a, struct rect b) {
	int64_t x0 = a.x > b.x ? a.x : b.x;
	int64_t y0 = a.y > b.y ? a.y : b.y;
	int64_t ax1 = (int64_t)a.x + a.w;
	int64_t ay1 = (int64_t)a.y + a.h;
	int64_t bx1 = (int64_t)b.x + b.w;
	int64_t by1 = (int64_t)b.y + b.h;
	int64_t x1 = ax1 < bx1 ? ax1 : bx1;
	int64_t y1 = ay1 < by1 ? ay1 : by1;
	if (x1 <= x0 || y1 <= y0) {
		return (struct rect){0};
	}
	return (struct rect){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0),
		(int32_t)(y1 - y0)};
}

static int32_t scale_round(int32_t v, uint32_t pixels, int32_t logical) {
	int64_t q =
		(2 * (int64_t)v * pixels + logical) / (2 * (int64_t)logical);
	return (int32_t)(q < 0 ? 0 : q > pixels ? pixels : q);
}

struct rect rect_physical(
	struct rect r, int32_t lw, int32_t lh, uint32_t iw, uint32_t ih) {
	int32_t x0 = rect_clamp(scale_round(r.x, iw, lw), 0, (int32_t)iw - 1);
	int32_t y0 = rect_clamp(scale_round(r.y, ih, lh), 0, (int32_t)ih - 1);
	int32_t x1 =
		rect_clamp(scale_round(r.x + r.w, iw, lw), x0 + 1, (int32_t)iw);
	int32_t y1 =
		rect_clamp(scale_round(r.y + r.h, ih, lh), y0 + 1, (int32_t)ih);
	return (struct rect){x0, y0, x1 - x0, y1 - y0};
}

struct rect rect_union(struct rect a, struct rect b) {
	if (a.w <= 0 || a.h <= 0) {
		return b;
	}
	if (b.w <= 0 || b.h <= 0) {
		return a;
	}
	int64_t x0 = a.x < b.x ? a.x : b.x;
	int64_t y0 = a.y < b.y ? a.y : b.y;
	int64_t ax1 = (int64_t)a.x + a.w;
	int64_t ay1 = (int64_t)a.y + a.h;
	int64_t bx1 = (int64_t)b.x + b.w;
	int64_t by1 = (int64_t)b.y + b.h;
	int64_t x1 = ax1 > bx1 ? ax1 : bx1;
	int64_t y1 = ay1 > by1 ? ay1 : by1;
	return (struct rect){(int32_t)x0, (int32_t)y0,
		(int32_t)(x1 - x0 > INT32_MAX ? INT32_MAX : x1 - x0),
		(int32_t)(y1 - y0 > INT32_MAX ? INT32_MAX : y1 - y0)};
}

struct place rect_place_crop(
	struct rect sel, int32_t lw, int32_t lh, uint32_t iw, uint32_t ih) {
	// Sweets sizes a surface as round(x·s) to round((x + w)·s), the same
	// edges rect_physical picks, so the crop lands on its own pixels
	struct rect px = rect_physical(sel, lw, lh, iw, ih);
	return (struct place){
		.at = sel,
		.sx = (double)iw / lw,
		.sy = (double)ih / lh,
		.ox = px.x,
		.oy = px.y,
	};
}

struct place rect_place_center(uint32_t iw, uint32_t ih, int32_t lw, int32_t lh,
	int32_t pw, int32_t ph) {
	// no mode yet: treat the output as scale 1
	double kx = pw > 0 ? (double)pw / lw : 1.0;
	double ky = ph > 0 ? (double)ph / lh : 1.0;
	double w = iw / kx;
	double h = ih / ky;
	double fx = lw / w;
	double fy = lh / h;
	double f = fx < fy ? fx : fy;
	if (f < 1.0) {
		w *= f;
		h *= f;
	}
	int32_t aw = (int32_t)(w + 0.5);
	int32_t ah = (int32_t)(h + 0.5);
	aw = rect_clamp(aw, 1, lw);
	ah = rect_clamp(ah, 1, lh);
	struct rect at = {(lw - aw) / 2, (lh - ah) / 2, aw, ah};
	double sx = (double)iw / aw;
	double sy = (double)ih / ah;
	return (struct place){at, sx, sy, at.x * sx, at.y * sy};
}
