#include "select.h"

#include "cursor-shape-v1-client-protocol.h"
#include "log.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <unistd.h>

// a press and release closer than this, in logical px, selects the whole
// output
enum { CLICK_SLOP = 3 };

struct part {
	struct wl_surface *surface;
	struct wl_subsurface *sub;
	struct wp_viewport *viewport;
	bool mapped;
};

struct overlay {
	struct selector *sel;
	size_t index;
	const struct output *out;
	struct wl_surface *surface; // NULL once destroyed
	struct wp_viewport *viewport;
	struct zwlr_layer_surface_v1 *layer;
	struct part parts[RECT_PARTS];
	int n_parts;
	int32_t w; // logical size from configure, 0 until the first
	int32_t h;
	bool dirty;
	struct wl_callback *frame;
	uint32_t frames;
};

enum state {
	SELECTING,
	CONFIRMED,
	CANCELLED,
	CLOSED,
};

struct selector {
	struct conn *conn;
	struct overlay *overlays;
	size_t n;
	struct wl_buffer *clear;
	struct wl_buffer *dim;
	struct wl_buffer *white;
	struct wl_region *no_input;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wp_cursor_shape_device_v1 *shape;
	struct overlay *hover; // under the pointer, at px, py
	int32_t px;
	int32_t py;
	struct overlay *sel_o; // the selection's output, NULL for none
	struct rect sel;
	struct overlay *drag_o; // a left-button drag started here
	int32_t drag_x;
	int32_t drag_y;
	bool shift[2];
	bool cursor;
	enum state state;
	// taken at confirm, late input cannot change them
	struct overlay *target;
	struct choice choice;
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static bool request_frame(struct overlay *o) {
	o->frame = wl_surface_frame(o->surface);
	if (!o->frame) {
		return false;
	}
	wl_callback_add_listener(o->frame, &frame_listener, o);
	return true;
}

static void apply(struct overlay *o) {
	struct selector *s = o->sel;
	struct rect r[RECT_PARTS];
	rect_layout(o->w, o->h, s->sel_o == o ? &s->sel : NULL, r);
	for (int i = 0; i < RECT_PARTS; i++) {
		struct part *p = &o->parts[i];
		if (r[i].w > 0 && r[i].h > 0) {
			wl_subsurface_set_position(p->sub, r[i].x, r[i].y);
			wp_viewport_set_destination(
				p->viewport, r[i].w, r[i].h);
			if (!p->mapped) {
				wl_surface_attach(p->surface,
					i < RECT_DIMS ? s->dim : s->white, 0,
					0);
				wl_surface_damage_buffer(
					p->surface, 0, 0, 1, 1);
				p->mapped = true;
			}
			wl_surface_commit(p->surface);
		} else if (p->mapped) {
			wl_surface_attach(p->surface, NULL, 0, 0);
			wl_surface_commit(p->surface);
			p->mapped = false;
		}
	}
	o->dirty = false;
	request_frame(o);
	// subsurfaces are synchronized: everything above lands in this commit
	wl_surface_commit(o->surface);
}

// redraws are paced by the output's frame callback, never by timers
static void touch(struct overlay *o) {
	if (o->sel->state != SELECTING || !o->surface) {
		return;
	}
	o->dirty = true;
	if (!o->frame && o->w > 0) {
		apply(o);
	}
}

static void set_sel(struct selector *s, struct overlay *o, struct rect r) {
	struct overlay *old = s->sel_o;
	s->sel_o = o;
	s->sel = r;
	if (old) {
		touch(old);
	}
	if (o && o != old) {
		touch(o);
	}
}

static struct rect whole(const struct overlay *o) {
	return (struct rect){0, 0, o->w, o->h};
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	(void)time;
	struct overlay *o = data;
	wl_callback_destroy(cb);
	o->frame = NULL;
	o->frames++;
	if (o->dirty && o->sel->state == SELECTING) {
		apply(o);
	}
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
	uint32_t serial, uint32_t w, uint32_t h) {
	struct overlay *o = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	// anchored to all edges, the compositor always picks the size
	if (w == 0 || h == 0 || w > INT32_MAX || h > INT32_MAX) {
		o->sel->state = CLOSED;
		return;
	}
	bool first = o->w == 0;
	o->w = (int32_t)w;
	o->h = (int32_t)h;
	wp_viewport_set_destination(o->viewport, o->w, o->h);
	if (first) {
		wl_surface_attach(o->surface, o->sel->clear, 0, 0);
		wl_surface_damage_buffer(o->surface, 0, 0, 1, 1);
	}
	if (o->sel->state != SELECTING) {
		wl_surface_commit(o->surface);
		return;
	}
	o->dirty = true;
	if (o->frame) {
		wl_surface_commit(o->surface);
	} else {
		apply(o);
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	(void)layer;
	struct overlay *o = data;
	o->sel->state = CLOSED;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static int32_t fixed_floor(wl_fixed_t v) {
	return v >= 0 ? v / 256 : -((-v + 255) / 256);
}

static int32_t clamp(int32_t v, int32_t hi) {
	return v < 0 ? 0 : v > hi ? hi : v;
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
	uint32_t serial, struct wl_surface *surface, wl_fixed_t x,
	wl_fixed_t y) {
	(void)pointer;
	struct selector *s = data;
	s->hover = NULL;
	for (size_t i = 0; i < s->n; i++) {
		if (surface && s->overlays[i].surface == surface) {
			s->hover = &s->overlays[i];
		}
	}
	s->px = fixed_floor(x);
	s->py = fixed_floor(y);
	if (s->shape) {
		wp_cursor_shape_device_v1_set_shape(s->shape, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
	}
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
	uint32_t serial, struct wl_surface *surface) {
	(void)pointer, (void)serial, (void)surface;
	struct selector *s = data;
	s->hover = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
	uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	(void)pointer, (void)time;
	struct selector *s = data;
	s->px = fixed_floor(x);
	s->py = fixed_floor(y);
	// the compositor's implicit grab keeps motion on the surface the drag
	// started on, in its coordinates; clamping keeps the drag on that
	// output
	struct overlay *o = s->drag_o;
	if (!o || s->hover != o || s->state != SELECTING) {
		return;
	}
	set_sel(s, o,
		rect_span(s->drag_x, s->drag_y, clamp(s->px, o->w),
			clamp(s->py, o->h)));
}

static void pointer_button(void *data, struct wl_pointer *pointer,
	uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	(void)pointer, (void)serial, (void)time;
	struct selector *s = data;
	if (button != BTN_LEFT || s->state != SELECTING) {
		return;
	}
	struct overlay *o = s->hover;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (!o || o->w == 0) {
			return;
		}
		s->drag_o = o;
		s->drag_x = clamp(s->px, o->w);
		s->drag_y = clamp(s->py, o->h);
		set_sel(s, o, (struct rect){s->drag_x, s->drag_y, 0, 0});
	} else if (s->drag_o) {
		o = s->drag_o;
		s->drag_o = NULL;
		if (abs(s->px - s->drag_x) < CLICK_SLOP &&
			abs(s->py - s->drag_y) < CLICK_SLOP) {
			set_sel(s, o, whole(o));
		}
	}
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
	uint32_t axis, wl_fixed_t value) {
	(void)data, (void)pointer, (void)time, (void)axis, (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
	(void)data, (void)pointer;
}

static void pointer_axis_source(
	void *data, struct wl_pointer *pointer, uint32_t source) {
	(void)data, (void)pointer, (void)source;
}

static void pointer_axis_stop(
	void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
	(void)data, (void)pointer, (void)time, (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
	uint32_t axis, int32_t discrete) {
	(void)data, (void)pointer, (void)axis, (void)discrete;
}

static void pointer_axis_value120(
	void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value) {
	(void)data, (void)pointer, (void)axis, (void)value;
}

static void pointer_axis_relative_direction(void *data,
	struct wl_pointer *pointer, uint32_t axis, uint32_t direction) {
	(void)data, (void)pointer, (void)axis, (void)direction;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
	.axis_relative_direction = pointer_axis_relative_direction,
};

static void on_key(struct selector *s, uint32_t key) {
	struct overlay *o = s->sel_o;
	int32_t dx = 0;
	int32_t dy = 0;
	switch (key) {
	case KEY_ESC:
		s->state = CANCELLED;
		return;
	case KEY_ENTER:
	case KEY_KPENTER:
		if (o && s->sel.w > 0 && s->sel.h > 0) {
			s->choice = (struct choice){
				.output = o->out,
				.rect = s->sel,
				.width = o->w,
				.height = o->h,
				.cursor = s->cursor,
			};
			s->target = o;
			s->state = CONFIRMED;
		}
		return;
	case KEY_TAB: {
		size_t next = o		 ? (o->index + 1) % s->n
			      : s->hover ? s->hover->index
					 : 0;
		if (s->overlays[next].w > 0) {
			set_sel(s, &s->overlays[next],
				whole(&s->overlays[next]));
		}
		return;
	}
	case KEY_P:
		s->cursor = !s->cursor;
		return;
	case KEY_LEFT:
		dx = -1;
		break;
	case KEY_RIGHT:
		dx = 1;
		break;
	case KEY_UP:
		dy = -1;
		break;
	case KEY_DOWN:
		dy = 1;
		break;
	default:
		return;
	}
	if (o) {
		bool resize = s->shift[0] || s->shift[1];
		set_sel(s, o, rect_nudge(s->sel, dx, dy, resize, o->w, o->h));
	}
}

// raw evdev codes need no keymap
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
	uint32_t format, int32_t fd, uint32_t size) {
	(void)data, (void)keyboard, (void)format, (void)size;
	close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
	uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	(void)keyboard, (void)serial, (void)surface;
	struct selector *s = data;
	// shift may already be down when the overlay takes focus
	s->shift[0] = s->shift[1] = false;
	uint32_t *k;
	wl_array_for_each(k, keys) {
		s->shift[0] |= *k == KEY_LEFTSHIFT;
		s->shift[1] |= *k == KEY_RIGHTSHIFT;
	}
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
	uint32_t serial, struct wl_surface *surface) {
	(void)keyboard, (void)serial, (void)surface;
	struct selector *s = data;
	s->shift[0] = s->shift[1] = false;
}

// Shift is tracked from its own key events: modifier masks are keymap
// indices, which need xkbcommon to interpret
static void keyboard_key(void *data, struct wl_keyboard *keyboard,
	uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	(void)keyboard, (void)serial, (void)time;
	struct selector *s = data;
	bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	if (key == KEY_LEFTSHIFT) {
		s->shift[0] = pressed;
	} else if (key == KEY_RIGHTSHIFT) {
		s->shift[1] = pressed;
	} else if (pressed && s->state == SELECTING) {
		on_key(s, key);
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
	uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked,
	uint32_t group) {
	(void)data, (void)keyboard, (void)serial, (void)depressed;
	(void)latched, (void)locked, (void)group;
}

static void keyboard_repeat_info(
	void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
	(void)data, (void)keyboard, (void)rate, (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void destroy_parts(struct overlay *o) {
	for (int i = o->n_parts - 1; i >= 0; i--) {
		struct part *p = &o->parts[i];
		if (p->viewport) {
			wp_viewport_destroy(p->viewport);
		}
		if (p->sub) {
			wl_subsurface_destroy(p->sub);
		}
		if (p->surface) {
			wl_surface_destroy(p->surface);
		}
		*p = (struct part){0};
	}
	o->n_parts = 0;
}

static void destroy_overlay(struct overlay *o) {
	if (o->frame) {
		wl_callback_destroy(o->frame);
	}
	o->frame = NULL;
	destroy_parts(o);
	if (o->layer) {
		zwlr_layer_surface_v1_destroy(o->layer);
	}
	if (o->viewport) {
		wp_viewport_destroy(o->viewport);
	}
	if (o->surface) {
		wl_surface_destroy(o->surface);
	}
	o->layer = NULL;
	o->viewport = NULL;
	o->surface = NULL;
}

// fills the next overlay; on failure the selector's destroy cleans up
// whatever was made
static bool add_overlay(struct selector *s, const struct output *out) {
	struct conn *c = s->conn;
	struct overlay *o = &s->overlays[s->n];
	*o = (struct overlay){.sel = s, .index = s->n, .out = out};
	s->n++;
	o->surface = wl_compositor_create_surface(c->compositor);
	if (!o->surface) {
		return false;
	}
	o->viewport = wp_viewporter_get_viewport(c->viewporter, o->surface);
	o->layer = zwlr_layer_shell_v1_get_layer_surface(c->layer_shell,
		o->surface, out->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		"sweetshot");
	if (!o->viewport || !o->layer) {
		return false;
	}
	for (int i = 0; i < RECT_PARTS; i++) {
		struct part *p = &o->parts[o->n_parts++];
		p->surface = wl_compositor_create_surface(c->compositor);
		if (!p->surface) {
			return false;
		}
		p->sub = wl_subcompositor_get_subsurface(
			c->subcompositor, p->surface, o->surface);
		p->viewport =
			wp_viewporter_get_viewport(c->viewporter, p->surface);
		if (!p->sub || !p->viewport) {
			return false;
		}
		// pointer input always lands on the parent, in its coordinates
		wl_surface_set_input_region(p->surface, s->no_input);
	}

	zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
	zwlr_layer_surface_v1_set_anchor(
		o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
				  ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
				  ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	// cover panels too instead of being placed beside them
	zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	// no buffer yet: this commit asks for the first configure
	wl_surface_commit(o->surface);
	return true;
}

struct selector *select_create(struct conn *c, bool cursor) {
	uint32_t input =
		WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
	if (!(c->seat_caps & input)) {
		log_error("the seat has neither a pointer nor a keyboard");
		return NULL;
	}
	struct selector *s = calloc(1, sizeof(*s));
	if (!s) {
		log_error("out of memory");
		return NULL;
	}
	*s = (struct selector){.conn = c, .cursor = cursor, .state = SELECTING};
	s->overlays = calloc(wl_list_length(&c->outputs), sizeof(*s->overlays));
	struct wp_single_pixel_buffer_manager_v1 *sp = c->single_pixel;
	s->clear = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
		sp, 0, 0, 0, 0);
	// premultiplied black at ~45%
	s->dim = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
		sp, 0, 0, 0, 0x73333333);
	s->white = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
		sp, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
	s->no_input = wl_compositor_create_region(c->compositor);
	if (!s->overlays || !s->clear || !s->dim || !s->white || !s->no_input) {
		goto fail;
	}
	struct output *out;
	wl_list_for_each(out, &c->outputs, link) {
		if (!add_overlay(s, out)) {
			goto fail;
		}
	}
	if (c->seat_caps & WL_SEAT_CAPABILITY_POINTER) {
		s->pointer = wl_seat_get_pointer(c->seat);
		if (!s->pointer) {
			goto fail;
		}
		wl_pointer_add_listener(s->pointer, &pointer_listener, s);
		if (c->cursor_shape) {
			s->shape = wp_cursor_shape_manager_v1_get_pointer(
				c->cursor_shape, s->pointer);
		}
	}
	if (c->seat_caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		s->keyboard = wl_seat_get_keyboard(c->seat);
		if (!s->keyboard) {
			goto fail;
		}
		wl_keyboard_add_listener(s->keyboard, &keyboard_listener, s);
	}
	return s;

fail:
	log_error("out of memory");
	select_destroy(s);
	return NULL;
}

static bool dispatch(struct selector *s) {
	if (wl_display_dispatch(s->conn->display) < 0) {
		log_error("lost the Wayland connection");
		return false;
	}
	if (s->state == CLOSED) {
		log_error("the compositor closed the selector");
		return false;
	}
	return true;
}

enum select_result select_run(struct selector *s, struct choice *choice) {
	while (s->state == SELECTING) {
		if (!dispatch(s)) {
			return SELECT_FAILED;
		}
	}
	if (s->state == CANCELLED) {
		return SELECT_CANCELLED;
	}

	// the other outputs: gone, keyboard and pointer focus go with them
	struct overlay *t = s->target;
	for (size_t i = 0; i < s->n; i++) {
		if (&s->overlays[i] != t) {
			destroy_overlay(&s->overlays[i]);
		}
	}
	destroy_parts(t);
	while (t->frame) {
		if (!dispatch(s)) {
			return SELECT_FAILED;
		}
	}
	for (int i = 0; i < 2; i++) {
		uint32_t before = t->frames;
		if (!request_frame(t)) {
			log_error("out of memory");
			return SELECT_FAILED;
		}
		wl_surface_commit(t->surface);
		while (t->frames == before) {
			if (!dispatch(s)) {
				return SELECT_FAILED;
			}
		}
	}
	*choice = s->choice;
	return SELECT_CONFIRMED;
}

void select_destroy(struct selector *s) {
	if (!s) {
		return;
	}
	// release arrived in wl_keyboard and wl_pointer v3
	if (s->keyboard && wl_keyboard_get_version(s->keyboard) >= 3) {
		wl_keyboard_release(s->keyboard);
	} else if (s->keyboard) {
		wl_keyboard_destroy(s->keyboard);
	}
	if (s->shape) {
		wp_cursor_shape_device_v1_destroy(s->shape);
	}
	if (s->pointer && wl_pointer_get_version(s->pointer) >= 3) {
		wl_pointer_release(s->pointer);
	} else if (s->pointer) {
		wl_pointer_destroy(s->pointer);
	}
	for (size_t i = s->n; i > 0; i--) {
		destroy_overlay(&s->overlays[i - 1]);
	}
	free(s->overlays);
	if (s->no_input) {
		wl_region_destroy(s->no_input);
	}
	if (s->white) {
		wl_buffer_destroy(s->white);
	}
	if (s->dim) {
		wl_buffer_destroy(s->dim);
	}
	if (s->clear) {
		wl_buffer_destroy(s->clear);
	}
	// hand keyboard focus back now, not after encoding
	wl_display_flush(s->conn->display);
	free(s);
}
