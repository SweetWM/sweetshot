#include "rect.h"

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
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
		r.w = clamp(r.w + dx, 1, w - r.x);
		r.h = clamp(r.h + dy, 1, h - r.y);
	} else {
		r.x = clamp(r.x + dx, 0, w - r.w);
		r.y = clamp(r.y + dy, 0, h - r.h);
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
	int32_t x0 = clamp(scale_round(r.x, iw, lw), 0, (int32_t)iw - 1);
	int32_t y0 = clamp(scale_round(r.y, ih, lh), 0, (int32_t)ih - 1);
	int32_t x1 = clamp(scale_round(r.x + r.w, iw, lw), x0 + 1, (int32_t)iw);
	int32_t y1 = clamp(scale_round(r.y + r.h, ih, lh), y0 + 1, (int32_t)ih);
	return (struct rect){x0, y0, x1 - x0, y1 - y0};
}
