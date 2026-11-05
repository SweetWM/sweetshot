#include "icon.h"

#include <math.h>

enum {
	BADGES = 99, // numbers past this draw no digits (draw.c)
	SQUIGGLE = 13,
};

struct grid {
	float x;
	float y;
	float f;
};

static struct raster_point at(const struct grid *g, float x, float y) {
	return (struct raster_point){g->x + x * g->f, g->y + y * g->f};
}

static struct draw_op stroke(const struct grid *g, enum draw_kind kind,
	uint32_t color, float width, float x0, float y0, float x1, float y1) {
	return (struct draw_op){
		.kind = kind,
		.color = color,
		.width = width * g->f,
		.a = at(g, x0, y0),
		.b = at(g, x1, y1),
	};
}

static size_t tool(const struct grid *g, const struct icon *ic,
	struct draw_op *ops, struct raster_point *pts) {
	float f = g->f;
	struct draw_op op = {
		.kind = ic->kind,
		.color = ic->color,
		.fill = ic->fill,
		.width = fminf(ic->size, 6.0f) * f,
		.a = at(g, 8, 24),
		.b = at(g, 24, 8),
	};
	switch (ic->kind) {
	case DRAW_ARROW:
		// the head is 4× the width
		op.width = fminf(ic->size, 3.0f) * f;
		break;
	case DRAW_LINE:
		break;
	case DRAW_RECT:
		op.width = fminf(ic->size, 4.0f) * f;
		op.a = at(g, 8, 8);
		op.b = at(g, 24, 24);
		break;
	case DRAW_ELLIPSE:
		op.width = fminf(ic->size, 4.0f) * f;
		op.a = at(g, 6, 9);
		op.b = at(g, 26, 23);
		break;
	case DRAW_PEN:
		for (int i = 0; i < SQUIGGLE; i++) {
			pts[i] = at(g, 6.0f + (float)i * 20.0f / 12.0f,
				16.0f - 6.0f * sinf((float)i * 0.5236f));
		}
		draw_pen_borrow(&op, pts, SQUIGGLE);
		break;
	case DRAW_HIGHLIGHT:
		// a marker stroke; multiply would vanish on a dark backdrop
		op.kind = DRAW_RECT;
		op.fill = true;
		op.a = at(g, 6, 11);
		op.b = at(g, 26, 21);
		break;
	case DRAW_PIXELATE:
		// a 2×2 mosaic, it has no colour of its own
		for (int i = 0; i < 4; i++) {
			float x = i % 2 ? 16.0f : 8.0f;
			float y = i < 2 ? 8.0f : 16.0f;
			ops[i] = (struct draw_op){
				.kind = DRAW_RECT,
				.color = i == 0 || i == 3 ? 0xbbbbbb : 0x666666,
				.fill = true,
				.a = at(g, x, y),
				.b = at(g, x + 8, y + 8),
			};
		}
		return 4;
	case DRAW_BADGE: {
		// badges are numbered by the badges before them, so number n
		// takes n − 1 invisible ones
		uint32_t before = ic->badge > 0 ? ic->badge - 1 : 0;
		before = before > BADGES ? BADGES : before;
		for (uint32_t i = 0; i < before; i++) {
			ops[i] = (struct draw_op){
				.kind = DRAW_BADGE,
				.width = 1,
				.a = {-1000, -1000},
			};
		}
		op.width = fminf(ic->size, 4.5f) * f;
		op.a = at(g, 16, 16);
		ops[before] = op;
		return before + 1;
	}
	}
	ops[0] = op;
	return 1;
}

size_t icon_ops(const struct icon *ic, float x, float y, float f,
	struct draw_op *ops, struct raster_point *pts) {
	struct grid g = {x, y, f};
	uint32_t c = ic->color;
	switch (ic->type) {
	case ICON_TOOL:
		return tool(&g, ic, ops, pts);
	case ICON_FILL:
		ops[0] = stroke(&g, DRAW_RECT, c, 1, 9, 9, 23, 23);
		ops[0].fill = true;
		return 1;
	case ICON_COLOR:
		// a zero-length line is a disc
		ops[0] = stroke(&g, DRAW_LINE, c, 18, 16, 16, 16, 16);
		return 1;
	case ICON_SIZE:
		ops[0] = stroke(&g, DRAW_LINE, c, ic->size, 16, 16, 16, 16);
		return 1;
	case ICON_UNDO:
		ops[0] = stroke(&g, DRAW_ARROW, c, 2.5f, 23, 14, 7, 14);
		ops[1] = stroke(&g, DRAW_LINE, c, 2.5f, 23, 23, 23, 14);
		return 2;
	case ICON_REDO:
		ops[0] = stroke(&g, DRAW_ARROW, c, 2.5f, 9, 14, 25, 14);
		ops[1] = stroke(&g, DRAW_LINE, c, 2.5f, 9, 23, 9, 14);
		return 2;
	case ICON_DONE:
		pts[0] = at(&g, 8, 17);
		pts[1] = at(&g, 14, 23);
		pts[2] = at(&g, 25, 10);
		ops[0] = stroke(&g, DRAW_PEN, c, 3, 0, 0, 0, 0);
		draw_pen_borrow(&ops[0], pts, 3);
		return 1;
	case ICON_CANCEL:
		ops[0] = stroke(&g, DRAW_LINE, c, 3, 10, 10, 22, 22);
		ops[1] = stroke(&g, DRAW_LINE, c, 3, 22, 10, 10, 22);
		return 2;
	}
	return 0;
}
