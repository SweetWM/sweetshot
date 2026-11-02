#include "tool.h"

#include <linux/input-event-codes.h>

enum {
	COLOR_DEFAULT = 0, // red
	SIZE_DEFAULT = 2,  // 4 logical px
};

void tool_init(struct tool *t, uint32_t w, uint32_t h) {
	*t = (struct tool){
		.w = (float)w,
		.h = (float)h,
		.scale = 1.0f,
		.color = COLOR_DEFAULT,
		.size = SIZE_DEFAULT,
	};
}

void tool_finish(struct tool *t) {
	draw_op_finish(&t->live);
	draw_ops_finish(&t->ops);
}

void tool_press(struct tool *t, struct raster_point p) {
	draw_op_finish(&t->live);
	t->live = (struct draw_op){
		.kind = DRAW_RECT,
		.color = draw_palette[t->color],
		.width = draw_sizes[t->size] * t->scale,
		.a = p,
		.b = p,
	};
	t->dragging = true;
}

struct rect tool_drag(struct tool *t, struct raster_point p, bool square) {
	if (!t->dragging) {
		return (struct rect){0};
	}
	p.x = p.x < 0.0f ? 0.0f : p.x > t->w ? t->w : p.x;
	p.y = p.y < 0.0f ? 0.0f : p.y > t->h ? t->h : p.y;
	if (square) {
		p = draw_square(t->live.a, p, t->w, t->h);
	}
	struct rect old = draw_bounds(&t->live);
	t->live.b = p;
	return rect_union(old, draw_bounds(&t->live));
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

bool tool_release(struct tool *t, struct rect *damage) {
	*damage = (struct rect){0};
	if (!t->dragging) {
		return true;
	}
	// a click or a flat drag draws nothing
	if (t->live.a.x == t->live.b.x || t->live.a.y == t->live.b.y) {
		*damage = tool_drop(t);
		return true;
	}
	// the pixels on screen stay as they are
	t->dragging = false;
	if (!draw_ops_push(&t->ops, &t->live)) {
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

struct rect tool_key(struct tool *t, uint32_t key, bool ctrl, bool shift) {
	if (!ctrl) {
		return (struct rect){0};
	}
	if (key == KEY_Z) {
		return shift ? redo(t) : undo(t);
	}
	return key == KEY_Y ? redo(t) : (struct rect){0};
}
