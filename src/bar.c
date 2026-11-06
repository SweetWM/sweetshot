#include "bar.h"

#include "evdev.h"
#include "tool.h"

enum {
	GAP = 4,       // between buttons of a group, and between rows
	GROUP = 10,    // between groups
	PAD_X = 10,    // clear of the pill's round ends
	PAD_Y = 6,     // a one-row bar is 44 px tall
	MARGIN = 12,   // from the image and the output edges
	ROW_BREAK = 2, // the group a second row starts with
};

// kind and group of every button, in bar order
static void buttons(struct bar *b, int *group) {
	size_t n = 0;
	for (size_t i = 0; i < TOOL_KINDS; i++) {
		group[n] = 0;
		b->buttons[n++] = (struct bar_button){BAR_TOOL, i, {0}};
	}
	group[n] = 1;
	b->buttons[n++] = (struct bar_button){BAR_FILL, 0, {0}};
	for (size_t i = 0; i < DRAW_COLORS; i++) {
		group[n] = 2;
		b->buttons[n++] = (struct bar_button){BAR_COLOR, i, {0}};
	}
	for (size_t i = 0; i < DRAW_SIZES; i++) {
		group[n] = 3;
		b->buttons[n++] = (struct bar_button){BAR_SIZE, i, {0}};
	}
	const enum bar_kind last[] = {BAR_UNDO, BAR_REDO, BAR_CANCEL, BAR_DONE};
	for (size_t i = 0; i < 4; i++) {
		group[n] = 4 + (int)i / 2;
		b->buttons[n++] = (struct bar_button){last[i], 0, {0}};
	}
}

// lays the buttons out left to right, a new row at ROW_BREAK when asked;
// returns the number of rows and each row's width
static int flow(struct bar *b, const int *group, bool two, int32_t *width) {
	int row = 0;
	int32_t x = PAD_X;
	for (size_t i = 0; i < BAR_BUTTONS; i++) {
		if (i > 0 && two && group[i] == ROW_BREAK &&
			group[i - 1] != ROW_BREAK) {
			width[row++] = x - GAP + PAD_X;
			x = PAD_X;
		} else if (i > 0) {
			x += group[i] == group[i - 1] ? 0 : GROUP - GAP;
		}
		int32_t y = PAD_Y + row * (BAR_BUTTON + GAP);
		b->buttons[i].at = (struct rect){x, y, BAR_BUTTON, BAR_BUTTON};
		x += BAR_BUTTON + GAP;
	}
	width[row] = x - GAP + PAD_X;
	return row + 1;
}

void bar_layout(struct bar *b, int32_t lw) {
	int group[BAR_BUTTONS];
	int32_t width[2];
	buttons(b, group);
	int rows = flow(b, group, false, width);
	if (width[0] + 2 * MARGIN > lw) {
		rows = flow(b, group, true, width);
	}
	b->w = rows == 2 && width[1] > width[0] ? width[1] : width[0];
	b->h = rows * BAR_BUTTON + (rows - 1) * GAP + 2 * PAD_Y;
	// centre the shorter row
	for (size_t i = 0; i < BAR_BUTTONS; i++) {
		int row = (b->buttons[i].at.y - PAD_Y) / (BAR_BUTTON + GAP);
		b->buttons[i].at.x += (b->w - width[row]) / 2;
	}
}

struct rect bar_place(
	const struct bar *b, struct rect image, int32_t lw, int32_t lh) {
	int32_t x = image.x + (image.w - b->w) / 2;
	int32_t hi = lw - b->w - MARGIN;
	x = hi < MARGIN	 ? (lw - b->w) / 2
	    : x < MARGIN ? MARGIN
	    : x > hi	 ? hi
			 : x;
	int32_t under = image.y + image.h + MARGIN;
	int32_t above = image.y - MARGIN - b->h;
	int32_t y;
	if (under + b->h + MARGIN <= lh) {
		y = under;
	} else if (above >= MARGIN) {
		y = above;
	} else {
		// over the image's bottom edge, kept on the output
		y = image.y + image.h - MARGIN - b->h;
		y = y > lh - MARGIN - b->h ? lh - MARGIN - b->h : y;
		y = y < MARGIN ? MARGIN : y;
	}
	return (struct rect){x, y, b->w, b->h};
}

int bar_hit(const struct bar *b, double x, double y) {
	for (int i = 0; i < BAR_BUTTONS; i++) {
		struct rect r = b->buttons[i].at;
		if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
			return i;
		}
	}
	return -1;
}

struct bar_action bar_action(const struct bar *b, int button) {
	struct bar_action a = {.size = -1};
	if (button < 0 || button >= BAR_BUTTONS) {
		return a;
	}
	const struct bar_button *k = &b->buttons[button];
	switch (k->kind) {
	case BAR_TOOL:
		a.key = tool_keys[k->index].key;
		break;
	case BAR_FILL:
		a.key = KEY_F;
		break;
	case BAR_COLOR:
		a.key = KEY_1 + (uint32_t)k->index;
		break;
	case BAR_SIZE:
		a.size = (int)k->index;
		break;
	case BAR_UNDO:
		a = (struct bar_action){KEY_Z, true, false, -1};
		break;
	case BAR_REDO:
		a = (struct bar_action){KEY_Z, true, true, -1};
		break;
	case BAR_CANCEL:
		a.key = KEY_ESC;
		break;
	case BAR_DONE:
		a.key = KEY_ENTER;
		break;
	}
	return a;
}

enum {
	PILL = 0x202020,
	SELECTED = 0x4a4a4a,
	HOVER = 0x333333,
	ICON = 0xd0d0d0,
	DIM = 0x5a5a5a,	    // undo/redo with nothing to do
	OUTLINE = 0x808080, // keeps the black swatch visible on the pill
	RING = 0xffffff,    // the current colour
};

// a zero-length line is a disc
static struct draw_op disc(float cx, float cy, float d, uint32_t color) {
	return (struct draw_op){
		.kind = DRAW_LINE,
		.color = color,
		.width = d,
		.a = {cx, cy},
		.b = {cx, cy},
	};
}

static struct draw_op ring(
	float x, float y, float inset, float f, float width, uint32_t color) {
	return (struct draw_op){
		.kind = DRAW_ELLIPSE,
		.color = color,
		.width = width * f,
		.a = {x + inset * f, y + inset * f},
		.b = {x + (BAR_BUTTON - inset) * f,
			y + (BAR_BUTTON - inset) * f},
	};
}

static struct icon button_icon(
	const struct bar_button *k, const struct bar_state *st) {
	struct icon ic = {.color = ICON, .size = 3, .badge = 1};
	switch (k->kind) {
	case BAR_TOOL:
		ic.type = ICON_TOOL;
		ic.kind = tool_keys[k->index].kind;
		break;
	case BAR_FILL:
		ic.type = ICON_FILL;
		break;
	case BAR_COLOR:
		ic.type = ICON_COLOR;
		ic.color = draw_palette[k->index];
		break;
	case BAR_SIZE:
		// the real stroke width in the current colour
		ic.type = ICON_SIZE;
		ic.color = draw_palette[st->color];
		ic.size = draw_sizes[k->index];
		break;
	case BAR_UNDO:
		ic.type = ICON_UNDO;
		ic.color = st->can_undo ? ICON : DIM;
		break;
	case BAR_REDO:
		ic.type = ICON_REDO;
		ic.color = st->can_redo ? ICON : DIM;
		break;
	case BAR_CANCEL:
		ic.type = ICON_CANCEL;
		break;
	case BAR_DONE:
		ic.type = ICON_DONE;
		break;
	}
	return ic;
}

size_t bar_ops(const struct bar *b, const struct bar_state *st, float f,
	struct draw_op *ops, struct raster_point (*pts)[ICON_PTS]) {
	float h = (float)b->h * f;
	float r = h / 2.0f;
	size_t n = 0;
	// the pill: one capsule as wide as the bar
	ops[n++] = (struct draw_op){
		.kind = DRAW_LINE,
		.color = PILL,
		.width = h,
		.a = {r, r},
		.b = {(float)b->w * f - r, r},
	};
	for (int i = 0; i < BAR_BUTTONS; i++) {
		const struct bar_button *k = &b->buttons[i];
		float x = (float)k->at.x * f;
		float y = (float)k->at.y * f;
		float c = (float)BAR_BUTTON / 2.0f * f;
		bool sel = (k->kind == BAR_TOOL && k->index == st->tool) ||
			   (k->kind == BAR_FILL && st->fill) ||
			   (k->kind == BAR_SIZE && k->index == st->size);
		if (sel || i == st->hover) {
			ops[n++] = disc(x + c, y + c, (float)BAR_BUTTON * f,
				sel ? SELECTED : HOVER);
		}
		struct icon ic = button_icon(k, st);
		// badge icons show 1, which takes a single op
		n += icon_ops(&ic, x, y, f, ops + n, pts[i]);
		if (k->kind == BAR_COLOR) {
			ops[n++] = ring(x, y, 7, f, 1, OUTLINE);
			if (k->index == st->color) {
				ops[n++] = ring(x, y, 3, f, 2, RING);
			}
		}
	}
	return n;
}
