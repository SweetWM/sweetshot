#include "hud.h"

#include "bar.h"
#include "icon.h"
#include "log.h"
#include "sprite.h"

#include <math.h>
#include <stdlib.h>

enum {
	CHIP = ICON_SIDE, // logical px
	GAP = 16,	  // chip from the pointer
};

static const uint32_t chip_bg = 0x99000000;

struct hud {
	struct sprite *bar_sprite;
	struct sprite *chip; // created last, so it stacks above the bar
	struct bar bar;
	int32_t bar_lw; // the output width the bar is laid out for
	struct bar_state state;
	double bar_scale; // of the bar's picture, 0 before the first
	struct icon look;
	double chip_scale;
	struct rect at; // the bar on the layer, logical
	bool hidden;
	bool pressing; // a left press started on the bar
	int pressed;   // its button, or -1 for a gap
};

static bool on_bar(const struct hud *h, double x, double y) {
	return !h->hidden && x >= h->at.x && x < h->at.x + h->at.w &&
	       y >= h->at.y && y < h->at.y + h->at.h;
}

static int button_at(const struct hud *h, double x, double y) {
	return on_bar(h, x, y) ? bar_hit(&h->bar, x - h->at.x, y - h->at.y)
			       : -1;
}

static uint32_t pixels(int32_t logical, double scale) {
	long px = lround(logical * scale);
	return px < 1 ? 1 : (uint32_t)px;
}

static bool same_state(const struct bar_state *a, const struct bar_state *b) {
	return a->tool == b->tool && a->color == b->color &&
	       a->size == b->size && a->fill == b->fill &&
	       a->can_undo == b->can_undo && a->can_redo == b->can_redo &&
	       a->hover == b->hover;
}

static bool same_look(const struct icon *a, const struct icon *b) {
	return a->kind == b->kind && a->color == b->color &&
	       a->size == b->size && a->fill == b->fill && a->badge == b->badge;
}

static bool draw_bar(struct hud *h, const struct bar_state *st, double scale) {
	struct draw_op ops[BAR_OPS];
	struct raster_point pts[BAR_BUTTONS][ICON_PTS];
	uint32_t pw = pixels(h->bar.w, scale);
	uint32_t ph = pixels(h->bar.h, scale);
	size_t n = bar_ops(&h->bar, st, (float)pw / (float)h->bar.w, ops, pts);
	if (!sprite_draw(
		    h->bar_sprite, h->bar.w, h->bar.h, pw, ph, 0, ops, n)) {
		return false;
	}
	h->state = *st;
	h->bar_scale = scale;
	return true;
}

static bool draw_chip(struct hud *h, const struct icon *look, double scale) {
	struct draw_op ops[ICON_OPS];
	struct raster_point pts[ICON_PTS];
	uint32_t px = pixels(CHIP, scale);
	size_t n = icon_ops(look, 0, 0, (float)px / CHIP, ops, pts);
	if (!sprite_draw(h->chip, CHIP, CHIP, px, px, chip_bg, ops, n)) {
		return false;
	}
	h->look = *look;
	h->chip_scale = scale;
	return true;
}

bool hud_update(struct hud *h, const struct hud_view *v) {
	const struct tool *t = v->tool;
	if (v->lw != h->bar_lw) {
		bar_layout(&h->bar, v->lw);
		h->bar_lw = v->lw;
		h->bar_scale = 0;
	}
	struct bar_state st = {
		.color = t->color,
		.size = t->size,
		.fill = t->fill,
		.can_undo = t->ops.n_active > 0,
		.can_redo = t->ops.n_active < t->ops.n,
	};
	h->at = bar_place(&h->bar, v->image, v->lw, v->lh);
	st.hover = v->hover ? button_at(h, v->px, v->py) : -1;
	for (size_t i = 0; i < TOOL_KINDS; i++) {
		st.tool = tool_keys[i].kind == t->kind ? i : st.tool;
	}
	if ((v->scale != h->bar_scale || !same_state(&st, &h->state)) &&
		!draw_bar(h, &st, v->scale)) {
		return false;
	}
	sprite_move(h->bar_sprite, h->at.x, h->at.y, !h->hidden);

	struct icon look = {
		.type = ICON_TOOL,
		.kind = t->kind,
		.color = draw_palette[t->color],
		.size = draw_sizes[t->size],
		.fill = t->fill,
		.badge = tool_next_badge(t),
	};
	if ((v->scale != h->chip_scale || !same_look(&look, &h->look)) &&
		!draw_chip(h, &look, v->scale)) {
		return false;
	}
	// down-right of the pointer, flipped near the output's edges
	int32_t x = (int32_t)floor(v->px) + GAP;
	int32_t y = (int32_t)floor(v->py) + GAP;
	x = x + CHIP > v->lw ? x - 2 * GAP - CHIP : x;
	y = y + CHIP > v->lh ? y - 2 * GAP - CHIP : y;
	sprite_move(h->chip, x, y, v->chip && !on_bar(h, v->px, v->py));
	return true;
}

bool hud_on_bar(const struct hud *h, double x, double y) {
	return on_bar(h, x, y);
}

bool hud_click(
	struct hud *h, double x, double y, bool pressed, struct bar_action *a) {
	*a = (struct bar_action){.size = -1};
	if (pressed) {
		if (!on_bar(h, x, y)) {
			return false;
		}
		h->pressing = true;
		h->pressed = button_at(h, x, y);
		return true;
	}
	if (!h->pressing) {
		return false;
	}
	h->pressing = false;
	if (h->pressed >= 0 && button_at(h, x, y) == h->pressed) {
		*a = bar_action(&h->bar, h->pressed);
	}
	return true;
}

void hud_toggle(struct hud *h) {
	h->hidden = !h->hidden;
}

bool hud_dirty(const struct hud *h) {
	return sprite_dirty(h->bar_sprite) || sprite_dirty(h->chip);
}

void hud_apply(struct hud *h) {
	if (sprite_dirty(h->bar_sprite)) {
		sprite_apply(h->bar_sprite);
	}
	if (sprite_dirty(h->chip)) {
		sprite_apply(h->chip);
	}
}

struct hud *hud_create(
	struct conn *c, struct wl_surface *parent, struct wl_region *no_input) {
	struct hud *h = calloc(1, sizeof(*h));
	if (!h) {
		log_error("out of memory");
		return NULL;
	}
	// each reports its own error
	h->bar_sprite = sprite_create(c, parent, no_input);
	h->chip = h->bar_sprite ? sprite_create(c, parent, no_input) : NULL;
	if (!h->chip) {
		hud_destroy(h);
		return NULL;
	}
	return h;
}

void hud_destroy(struct hud *h) {
	if (!h) {
		return;
	}
	sprite_destroy(h->chip);
	sprite_destroy(h->bar_sprite);
	free(h);
}
