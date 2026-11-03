#include "tool.h"

#include <linux/input-event-codes.h>
#include <math.h>

enum {
	COLOR_DEFAULT = 0, // red
	SIZE_DEFAULT = 2,  // 4 logical px
	// a badge press that moves this far, in logical px, is not a click
	// (as the selector)
	CLICK_SLOP = 3,
};

// evdev codes are physical keys, the same on any layout
const struct tool_key tool_keys[TOOL_KINDS] = {
	{KEY_A, DRAW_ARROW},
	{KEY_L, DRAW_LINE},
	{KEY_R, DRAW_RECT},
	{KEY_E, DRAW_ELLIPSE},
	{KEY_D, DRAW_PEN},
	{KEY_H, DRAW_HIGHLIGHT},
	{KEY_X, DRAW_PIXELATE},
	{KEY_N, DRAW_BADGE},
};

void tool_init(struct tool *t, uint32_t w, uint32_t h) {
	*t = (struct tool){
		.w = (float)w,
		.h = (float)h,
		.scale = 1.0f,
		.kind = DRAW_ARROW,
		.color = COLOR_DEFAULT,
		.size = SIZE_DEFAULT,
	};
}

void tool_finish(struct tool *t) {
	draw_op_finish(&t->live);
	draw_ops_finish(&t->ops);
}

// dragged as a box; whole-pixel corners keep their edges crisp
static bool boxy(enum draw_kind k) {
	return k == DRAW_RECT || k == DRAW_ELLIPSE || k == DRAW_HIGHLIGHT ||
	       k == DRAW_PIXELATE;
}

static struct raster_point place(
	const struct tool *t, enum draw_kind k, struct raster_point p) {
	p.x = fminf(fmaxf(p.x, 0.0f), t->w);
	p.y = fminf(fmaxf(p.y, 0.0f), t->h);
	return boxy(k) ? (struct raster_point){roundf(p.x), roundf(p.y)} : p;
}

bool tool_press(struct tool *t, struct raster_point p, struct rect *damage) {
	*damage = (struct rect){0};
	draw_op_finish(&t->live);
	p = place(t, t->kind, p);
	t->live = (struct draw_op){
		.kind = t->kind,
		.color = draw_palette[t->color],
		.width = draw_sizes[t->size] * t->scale,
		.fill = t->fill,
		.a = p,
		.b = p,
	};
	if (t->kind == DRAW_PEN && !draw_pen_add(&t->live, p)) {
		return false;
	}
	t->dragging = true;
	// a badge or a pen dot shows at once
	*damage = draw_bounds(&t->live);
	return true;
}

bool tool_drag(struct tool *t, struct raster_point p, bool constrain,
	struct rect *damage) {
	*damage = (struct rect){0};
	if (!t->dragging) {
		return true;
	}
	struct draw_op *op = &t->live;
	p = place(t, op->kind, p);
	if (op->kind == DRAW_PEN) {
		struct raster_point last = op->pts[op->n_pts - 1];
		float dx = p.x - last.x;
		float dy = p.y - last.y;
		if (dx * dx + dy * dy < 1.0f) {
			return true;
		}
		if (!draw_pen_add(op, p)) {
			return false;
		}
		// only the new segment changes
		struct draw_op seg = {
			.kind = DRAW_LINE,
			.width = op->width,
			.a = last,
			.b = p,
		};
		*damage = draw_bounds(&seg);
		return true;
	}
	if (op->kind == DRAW_BADGE) {
		// badges are placed by a click; a drag cancels it
		float slop = (float)CLICK_SLOP * t->scale;
		if (fabsf(p.x - op->a.x) >= slop ||
			fabsf(p.y - op->a.y) >= slop) {
			*damage = tool_drop(t);
		}
		return true;
	}
	if (constrain) {
		p = draw_constrain(op->kind, op->a, p, t->w, t->h);
	}
	struct rect old = draw_bounds(op);
	op->b = p;
	*damage = rect_union(old, draw_bounds(op));
	return true;
}

struct rect tool_drop(struct tool *t) {
	if (!t->dragging) {
		return (struct rect){0};
	}
	t->dragging = false;
	struct rect r = draw_bounds(&t->live);
	draw_op_finish(&t->live);
	return r;
}

// a click with a line tool or a flat box draws nothing; a pen click is a dot
static bool empty(const struct draw_op *op) {
	bool flat_x = op->a.x == op->b.x;
	bool flat_y = op->a.y == op->b.y;
	switch (op->kind) {
	case DRAW_PEN:
	case DRAW_BADGE:
		return false;
	case DRAW_ARROW:
	case DRAW_LINE:
		return flat_x && flat_y;
	case DRAW_RECT:
	case DRAW_ELLIPSE:
	case DRAW_HIGHLIGHT:
	case DRAW_PIXELATE:
		break;
	}
	return flat_x || flat_y;
}

bool tool_release(struct tool *t, struct rect *damage) {
	*damage = (struct rect){0};
	if (!t->dragging) {
		return true;
	}
	if (empty(&t->live)) {
		*damage = tool_drop(t);
		return true;
	}
	// the pixels on screen stay as they are
	t->dragging = false;
	if (!draw_ops_push(&t->ops, &t->live)) {
		*damage = draw_bounds(&t->live);
		draw_op_finish(&t->live);
		return false;
	}
	// the list owns it now
	t->live = (struct draw_op){0};
	return true;
}

// both wait for the drag to end
static struct rect undo(struct tool *t) {
	if (t->dragging || t->ops.n_active == 0) {
		return (struct rect){0};
	}
	t->ops.n_active--;
	return draw_bounds(&t->ops.ops[t->ops.n_active]);
}

static struct rect redo(struct tool *t) {
	if (t->dragging || t->ops.n_active == t->ops.n) {
		return (struct rect){0};
	}
	t->ops.n_active++;
	return draw_bounds(&t->ops.ops[t->ops.n_active - 1]);
}

void tool_resize(struct tool *t, int steps) {
	if (t->dragging) {
		return;
	}
	long size = (long)t->size + steps;
	size = size < 0 ? 0 : size >= DRAW_SIZES ? DRAW_SIZES - 1 : size;
	t->size = (size_t)size;
}

struct rect tool_key(struct tool *t, uint32_t key, bool ctrl, bool shift) {
	if (ctrl) {
		if (key == KEY_Z) {
			return shift ? redo(t) : undo(t);
		}
		return key == KEY_Y ? redo(t) : (struct rect){0};
	}
	// the shape being drawn keeps the settings it started with
	if (t->dragging) {
		return (struct rect){0};
	}
	for (size_t i = 0; i < TOOL_KINDS; i++) {
		if (tool_keys[i].key == key) {
			t->kind = tool_keys[i].kind;
		}
	}
	if (key >= KEY_1 && key <= KEY_9) {
		t->color = key - KEY_1;
	} else if (key == KEY_F) {
		t->fill = !t->fill;
	} else if (key == KEY_MINUS) {
		tool_resize(t, -1);
	} else if (key == KEY_EQUAL) {
		tool_resize(t, 1);
	}
	return (struct rect){0};
}

uint32_t tool_next_badge(const struct tool *t) {
	uint32_t n = 1;
	for (size_t i = 0; i < t->ops.n_active; i++) {
		n += t->ops.ops[i].kind == DRAW_BADGE;
	}
	return n;
}
