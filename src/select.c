#include "select.h"

#include "capture.h"
#include "input.h"
#include "log.h"
#include "overlay.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>

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
	struct shot *plain; // this output's frozen pixels, without the pointer
	struct shot *pointer;	    // and with it
	struct wl_buffer *shown;    // attached to surface, NULL until mapped
	struct wl_surface *surface; // NULL once destroyed
	struct wp_viewport *viewport;
	struct zwlr_layer_surface_v1 *layer;
	struct part parts[RECT_PARTS];
	int n_parts;
	int32_t w; // logical size from configure, 0 until the first
	int32_t h;
	bool dirty;
	struct wl_callback *frame;
};

enum state {
	SELECTING,
	CONFIRMED,
	CANCELLED,
	CLOSED,
	FAILED, // reported already
};

struct selector {
	struct conn *conn;
	struct capture *cap;
	struct overlay *overlays;
	size_t n;
	struct wl_buffer *dim;
	struct wl_buffer *white;
	struct wl_region *no_input;
	struct input *input;
	struct overlay *hover; // under the pointer, at px, py
	int32_t px;
	int32_t py;
	struct overlay *sel_o; // the selection's output, NULL for none
	struct rect sel;
	struct overlay *drag_o; // a left-button drag started here
	int32_t drag_x;
	int32_t drag_y;
	bool cursor;
	enum state state;
	// taken at confirm, late input cannot change it
	struct choice choice;
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

// without the callback nothing would pace the next redraw
static void request_frame(struct overlay *o) {
	o->frame = wl_surface_frame(o->surface);
	if (!o->frame) {
		log_error("out of memory");
		o->sel->state = FAILED;
		return;
	}
	wl_callback_add_listener(o->frame, &frame_listener, o);
}

static struct wl_buffer *wanted(const struct overlay *o) {
	const struct shot *a = o->sel->cursor ? o->pointer : o->plain;
	const struct shot *b = o->sel->cursor ? o->plain : o->pointer;
	return a->buffer ? a->buffer : b->buffer;
}

static void apply(struct overlay *o) {
	struct selector *s = o->sel;
	struct wl_buffer *b = wanted(o);
	if (!b) {
		return;
	}
	if (b != o->shown) {
		const struct shot *shot =
			b == o->plain->buffer ? o->plain : o->pointer;
		// the capture's own buffer, the viewport maps it 1:1 onto
		// the output's physical pixels
		wl_surface_attach(o->surface, b, 0, 0);
		wl_surface_set_buffer_transform(
			o->surface, (int32_t)shot->transform);
		wl_surface_damage_buffer(
			o->surface, 0, 0, INT32_MAX, INT32_MAX);
		o->shown = b;
	}
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
	o->w = (int32_t)w;
	o->h = (int32_t)h;
	wp_viewport_set_destination(o->viewport, o->w, o->h);
	o->dirty = true;
	// without a frozen buffer the ack waits for the commit that maps it
	if (!wanted(o)) {
		return;
	}
	if (o->frame || o->sel->state != SELECTING) {
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

static void on_enter(
	void *data, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
	struct selector *s = data;
	s->hover = NULL;
	for (size_t i = 0; i < s->n; i++) {
		if (surface && s->overlays[i].surface == surface) {
			s->hover = &s->overlays[i];
		}
	}
	s->px = fixed_floor(x);
	s->py = fixed_floor(y);
}

static void on_leave(void *data) {
	struct selector *s = data;
	s->hover = NULL;
}

static void on_motion(void *data, wl_fixed_t x, wl_fixed_t y) {
	struct selector *s = data;
	s->px = fixed_floor(x);
	s->py = fixed_floor(y);
	struct overlay *o = s->drag_o;
	if (!o || s->hover != o || s->state != SELECTING) {
		return;
	}
	set_sel(s, o,
		rect_span(s->drag_x, s->drag_y, rect_clamp(s->px, 0, o->w),
			rect_clamp(s->py, 0, o->h)));
}

static void on_button(void *data, uint32_t button, bool pressed) {
	struct selector *s = data;
	if (button != BTN_LEFT || s->state != SELECTING) {
		return;
	}
	struct overlay *o = s->hover;
	if (pressed) {
		if (!o || o->w == 0) {
			return;
		}
		s->drag_o = o;
		s->drag_x = rect_clamp(s->px, 0, o->w);
		s->drag_y = rect_clamp(s->py, 0, o->h);
		set_sel(s, o, (struct rect){s->drag_x, s->drag_y, 0, 0});
	} else if (s->drag_o) {
		o = s->drag_o;
		s->drag_o = NULL;
		// a click selects the whole output
		if (abs(s->px - s->drag_x) < RECT_CLICK_SLOP &&
			abs(s->py - s->drag_y) < RECT_CLICK_SLOP) {
			set_sel(s, o, whole(o));
		}
	}
}

static void on_key(void *data, uint32_t key, uint32_t mods) {
	struct selector *s = data;
	if (s->state != SELECTING) {
		return;
	}
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
				.shot = s->cursor ? o->pointer : o->plain,
				.rect = s->sel,
				.width = o->w,
				.height = o->h,
			};
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
		for (size_t i = 0; i < s->n; i++) {
			touch(&s->overlays[i]);
		}
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
		bool resize = mods & INPUT_SHIFT;
		set_sel(s, o, rect_nudge(s->sel, dx, dy, resize, o->w, o->h));
	}
}

static const struct input_listener input_listener = {
	.enter = on_enter,
	.leave = on_leave,
	.motion = on_motion,
	.button = on_button,
	.key = on_key,
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
static bool add_overlay(struct selector *s, const struct output *out,
	struct shot *plain, struct shot *pointer) {
	struct conn *c = s->conn;
	struct overlay *o = &s->overlays[s->n];
	*o = (struct overlay){
		.sel = s,
		.index = s->n,
		.out = out,
		.plain = plain,
		.pointer = pointer,
	};
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

	overlay_map(o->layer, o->surface, &layer_listener, o);
	return true;
}

struct selector *select_create(struct conn *c, bool cursor, struct capture *cap,
	struct shot *plain, struct shot *pointer) {
	struct selector *s = calloc(1, sizeof(*s));
	if (!s) {
		log_error("out of memory");
		return NULL;
	}
	*s = (struct selector){
		.conn = c,
		.cap = cap,
		.cursor = cursor,
		.state = SELECTING,
	};
	// reports its own error
	s->input = input_create(c, &input_listener, s);
	if (!s->input) {
		select_destroy(s);
		return NULL;
	}
	s->overlays = calloc(wl_list_length(&c->outputs), sizeof(*s->overlays));
	struct wp_single_pixel_buffer_manager_v1 *sp = c->single_pixel;
	s->dim = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
		sp, 0, 0, 0, OVERLAY_DIM_ALPHA);
	s->white = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
		sp, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
	s->no_input = wl_compositor_create_region(c->compositor);
	if (!s->overlays || !s->dim || !s->white || !s->no_input) {
		goto fail;
	}
	struct output *out;
	wl_list_for_each(out, &c->outputs, link) {
		size_t i = s->n;
		if (!add_overlay(s, out, &plain[i], &pointer[i])) {
			goto fail;
		}
	}
	return s;

fail:
	log_error("out of memory");
	select_destroy(s);
	return NULL;
}

// each output maps as soon as its own capture lands: they arrive one by one
static bool dispatch(struct selector *s) {
	if (wl_display_dispatch(s->conn->display) < 0) {
		log_error("lost the Wayland connection");
		return false;
	}
	if (s->state == CLOSED) {
		log_error("the compositor closed the selector");
		return false;
	}
	if (capture_status(s->cap) == CAPTURE_FAILED) {
		return false;
	}
	for (size_t i = 0; i < s->n; i++) {
		if (s->overlays[i].shown != wanted(&s->overlays[i])) {
			touch(&s->overlays[i]);
		}
	}
	return true;
}

enum select_result select_run(struct selector *s, struct choice *choice) {
	while (s->state == SELECTING) {
		if (!dispatch(s)) {
			return SELECT_FAILED;
		}
	}
	// a redraw at the end of a dispatch can fail without failing it
	if (s->state != CONFIRMED) {
		return s->state == CANCELLED ? SELECT_CANCELLED : SELECT_FAILED;
	}
	// the chosen variant may still be on its way, the others no longer
	// matter, so one of them failing now cannot fail the command
	capture_keep(s->cap, s->choice.shot);
	while (capture_status(s->cap) == CAPTURE_PENDING) {
		if (!dispatch(s)) {
			return SELECT_FAILED;
		}
	}
	*choice = s->choice;
	return SELECT_CONFIRMED;
}

void select_destroy(struct selector *s) {
	if (!s) {
		return;
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
	input_destroy(s->input);
	// hand keyboard focus back now, not after encoding
	wl_display_flush(s->conn->display);
	free(s);
}
