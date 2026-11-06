#include "edit.h"

#include "evdev.h"
#include "hud.h"
#include "input.h"
#include "log.h"
#include "overlay.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "tool.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <math.h>
#include <stdlib.h>
#include <sys/mman.h>

enum state {
	EDITING,
	DONE,
	CANCELLED,
	FAILED, // already reported
};

// a display buffer, the compositor reads it until release
struct buf {
	struct editor *e;
	struct wl_buffer *wl;
	uint8_t *data;
	bool busy;	   // attached and not released yet
	struct rect stale; // changed since this buffer was last drawn
};

struct editor {
	struct conn *conn;
	struct edit_input in;
	struct rect whole; // the image, in image pixels
	size_t stride;
	struct input *input;
	struct wl_buffer *dim;
	struct catchers *catchers;
	struct wl_region *no_input;
	struct wl_surface *surface; // the layer: dim base, takes all input
	struct wp_viewport *viewport;
	struct zwlr_layer_surface_v1 *layer;
	struct wl_surface *img; // the picture, synchronized subsurface
	struct wl_subsurface *sub;
	struct wp_viewport *img_viewport;
	struct wl_surface *back; // in.backdrop, below the layer's dim
	struct wl_subsurface *back_sub;
	struct wp_viewport *back_viewport;
	struct hud *hud; // toolbar and chip, above the picture
	struct buf bufs[2];
	int front; // the buffer last attached, or drawn first
	uint8_t *map;
	size_t map_len;
	struct wl_callback *frame;
	bool shown;    // a frame callback came back
	bool borrowed; // the picture shows in.backdrop, not bufs
	int32_t lw;    // layer size from configure, 0 until the first
	int32_t lh;
	struct place place;
	struct rect pending; // changed since the last commit
	bool dirty;
	bool over; // the pointer is on the layer, at px, py
	double px;
	double py;
	uint32_t mods;
	struct tool tool;
	enum state state;
};

static void fail(struct editor *e, const char *msg) {
	if (e->state == EDITING) {
		log_error("%s", msg);
		e->state = FAILED;
	}
}

static void render(struct editor *e, int i, struct rect clip) {
	struct buf *b = &e->bufs[i];
	const struct tool *t = &e->tool;
	draw_render(&e->in.image, t->ops.ops, t->ops.n_active,
		t->dragging ? &t->live : NULL, b->data, e->stride, clip);
	b->stale = (struct rect){0};
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static void update_hud(struct editor *e);

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void request_frame(struct editor *e) {
	if (e->frame) {
		return;
	}
	e->frame = wl_surface_frame(e->surface);
	if (!e->frame) {
		fail(e, "out of memory");
		return;
	}
	wl_callback_add_listener(e->frame, &frame_listener, e);
}

static void attach(struct editor *e, int i, struct rect damage) {
	wl_surface_attach(e->img, e->bufs[i].wl, 0, 0);
	wl_surface_damage_buffer(
		e->img, damage.x, damage.y, damage.w, damage.h);
	e->bufs[i].busy = true;
	e->front = i;
}

// draws the back buffer and updates the hud, paced by the frame callback and
// by buffer release, never by timers
static void redraw(struct editor *e) {
	if (e->frame || e->lw == 0 || e->state != EDITING) {
		return;
	}
	int back = !e->front;
	// an image change waits for the back buffer's release, which calls
	// back here
	bool image = e->dirty && !e->bufs[back].busy;
	bool hud = hud_dirty(e->hud);
	if (!image && !hud) {
		return;
	}
	if (image) {
		render(e, back, e->bufs[back].stale);
		if (e->borrowed) {
			wl_surface_set_buffer_transform(
				e->img, WL_OUTPUT_TRANSFORM_NORMAL);
			wp_viewport_set_source(e->img_viewport,
				wl_fixed_from_int(-1), wl_fixed_from_int(-1),
				wl_fixed_from_int(-1), wl_fixed_from_int(-1));
			e->borrowed = false;
		}
		attach(e, back, e->pending);
		wl_surface_commit(e->img);
		e->pending = (struct rect){0};
		e->dirty = false;
	}
	if (hud) {
		hud_apply(e->hud);
	}
	request_frame(e);
	// the subsurfaces are synchronized: their state lands with this commit
	wl_surface_commit(e->surface);
}

static void damage(struct editor *e, struct rect r) {
	r = rect_intersect(r, e->whole);
	if (r.w == 0) {
		return;
	}
	e->pending = rect_union(e->pending, r);
	for (int i = 0; i < 2; i++) {
		e->bufs[i].stale = rect_union(e->bufs[i].stale, r);
	}
	e->dirty = true;
	redraw(e);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	(void)time;
	struct editor *e = data;
	wl_callback_destroy(cb);
	e->frame = NULL;
	e->shown = true;
	redraw(e);
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
	(void)buffer;
	struct buf *b = data;
	b->busy = false;
	redraw(b->e);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static struct place place_image(const struct editor *e) {
	const struct png_image *img = &e->in.image;
	switch (e->in.fit) {
	case EDIT_REGION:
		return rect_place_crop(
			e->in.sel, e->lw, e->lh, e->in.full_w, e->in.full_h);
	case EDIT_WINDOW: {
		const struct output *o = e->in.output;
		// the mode is not rotated, the layer is
		bool turned = o->transform & 1;
		return rect_place_center(img->width, img->height, e->lw, e->lh,
			turned ? o->mode_h : o->mode_w,
			turned ? o->mode_w : o->mode_h);
	}
	case EDIT_OUTPUT:
		break;
	}
	return rect_place_crop((struct rect){0, 0, e->lw, e->lh}, e->lw, e->lh,
		img->width, img->height);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
	uint32_t serial, uint32_t w, uint32_t h) {
	struct editor *e = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	// anchored to all edges, the compositor always picks the size
	if (w == 0 || h == 0 || w > INT32_MAX || h > INT32_MAX) {
		fail(e, "the compositor closed the editor");
		return;
	}
	bool first = e->lw == 0;
	e->lw = (int32_t)w;
	e->lh = (int32_t)h;
	e->place = place_image(e);
	e->tool.scale = (float)e->place.sx;
	struct rect at = e->place.at;
	wp_viewport_set_destination(e->viewport, e->lw, e->lh);
	wl_subsurface_set_position(e->sub, at.x, at.y);
	wp_viewport_set_destination(e->img_viewport, at.w, at.h);
	if (e->back) {
		wp_viewport_set_destination(e->back_viewport, e->lw, e->lh);
	}
	if (first) {
		wl_surface_attach(e->surface, e->dim, 0, 0);
		wl_surface_damage_buffer(e->surface, 0, 0, 1, 1);
	}
	if (first && !e->back) {
		render(e, 0, e->whole);
		attach(e, 0, e->whole);
	}
	if (first && e->back) {
		wl_surface_attach(e->back, e->in.backdrop, 0, 0);
		wl_surface_set_buffer_transform(
			e->back, (int32_t)e->in.backdrop_transform);
		wl_surface_damage_buffer(e->back, 0, 0, INT32_MAX, INT32_MAX);
		struct rect r = e->in.backdrop_crop;
		wl_surface_attach(e->img, e->in.backdrop, 0, 0);
		wl_surface_set_buffer_transform(
			e->img, (int32_t)e->in.backdrop_transform);
		wp_viewport_set_source(e->img_viewport, wl_fixed_from_int(r.x),
			wl_fixed_from_int(r.y), wl_fixed_from_int(r.w),
			wl_fixed_from_int(r.h));
		wl_surface_damage_buffer(e->img, 0, 0, INT32_MAX, INT32_MAX);
		e->borrowed = true;
		e->front = 1;
		e->pending = e->whole;
		e->dirty = true;
	}
	if (e->back) {
		wl_surface_commit(e->back);
	}
	// the toolbar lands in the same commit as the picture: with a frame
	// pending, update_hud only prepares it
	request_frame(e);
	update_hud(e);
	if (hud_dirty(e->hud)) {
		hud_apply(e->hud);
	}
	wl_surface_commit(e->img);
	wl_surface_commit(e->surface);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	(void)layer;
	fail(data, "the compositor closed the editor");
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

// the pointer in image pixels; false when it is off the image
static bool pointer_image(const struct editor *e, struct raster_point *p) {
	float x = (float)(e->px * e->place.sx - e->place.ox);
	float y = (float)(e->py * e->place.sy - e->place.oy);
	*p = (struct raster_point){x, y};
	return x >= 0.0f && x <= e->tool.w && y >= 0.0f && y <= e->tool.h;
}

// output pixels per logical pixel, for the toolbar and chip; a window
// image's own ratio can differ when it is shrunk to fit
static double output_scale(const struct editor *e) {
	const struct output *o = e->in.output;
	int32_t pw = o->transform & 1 ? o->mode_h : o->mode_w;
	return pw > 0 ? (double)pw / e->lw : 1.0;
}

// the toolbar shows the tool state; the chip shows the current tool next to
// the pointer, only over the image and never during a drag
static void update_hud(struct editor *e) {
	if (e->lw == 0 || e->state != EDITING) {
		return;
	}
	struct raster_point p;
	struct hud_view v = {
		.tool = &e->tool,
		.scale = output_scale(e),
		.lw = e->lw,
		.lh = e->lh,
		.image = e->place.at,
		.px = e->px,
		.py = e->py,
		.chip = e->over && !e->tool.dragging && pointer_image(e, &p),
		.hover = e->over && !e->tool.dragging,
	};
	if (!hud_update(e->hud, &v)) {
		// reported
		e->state = FAILED;
		return;
	}
	bool arrow = v.hover && hud_on_bar(e->hud, e->px, e->py);
	input_set_cursor(e->input, arrow ? INPUT_DEFAULT : INPUT_CROSSHAIR);
	redraw(e);
}

static void drag(struct editor *e) {
	struct raster_point p;
	struct rect r;
	pointer_image(e, &p);
	if (!tool_drag(&e->tool, p, e->mods & INPUT_SHIFT, &r)) {
		fail(e, "out of memory");
	}
	damage(e, r);
}

static void on_enter(
	void *data, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
	struct editor *e = data;
	e->over = surface && surface == e->surface;
	e->px = wl_fixed_to_double(x);
	e->py = wl_fixed_to_double(y);
	update_hud(e);
}

static void on_leave(void *data) {
	struct editor *e = data;
	e->over = false;
	update_hud(e);
}

static void on_motion(void *data, wl_fixed_t x, wl_fixed_t y) {
	struct editor *e = data;
	e->px = wl_fixed_to_double(x);
	e->py = wl_fixed_to_double(y);
	// motion without a drag only moves the chip
	if (e->tool.dragging && e->state == EDITING) {
		drag(e);
	}
	update_hud(e);
}

// a key, or a toolbar button standing for one
static void act(struct editor *e, uint32_t key, uint32_t mods) {
	if (key == KEY_ESC) {
		e->state = CANCELLED;
	} else if (key == KEY_ENTER || key == KEY_KPENTER) {
		e->state = DONE;
	} else if (key == KEY_TAB) {
		hud_toggle(e->hud);
	} else {
		damage(e, tool_key(&e->tool, key, mods & INPUT_CTRL,
				  mods & INPUT_SHIFT));
	}
	update_hud(e);
}

static void on_button(void *data, uint32_t button, bool pressed) {
	struct editor *e = data;
	struct tool *t = &e->tool;
	struct raster_point p;
	struct rect r = {0};
	struct bar_action a;
	bool ok = true;
	if (e->state != EDITING || e->lw == 0) {
		return;
	}
	// a press on the toolbar never starts a shape; a shape dragged onto
	// it still ends normally
	if (button == BTN_LEFT && !t->dragging &&
		hud_click(e->hud, e->px, e->py, pressed, &a)) {
		uint32_t mods =
			(a.ctrl ? INPUT_CTRL : 0) | (a.shift ? INPUT_SHIFT : 0);
		tool_resize(t, a.size >= 0 ? a.size - (int)t->size : 0);
		if (a.key) {
			act(e, a.key, mods);
		} else {
			update_hud(e);
		}
		return;
	}
	if (button == BTN_LEFT && pressed && !t->dragging && e->over &&
		pointer_image(e, &p)) {
		ok = tool_press(t, p, &r);
	} else if (button == BTN_LEFT && !pressed) {
		ok = tool_release(t, &r);
	} else if (button == BTN_RIGHT && pressed) {
		r = tool_drop(t);
	}
	if (!ok) {
		fail(e, "out of memory");
	}
	damage(e, r);
	update_hud(e);
}

static void on_key(void *data, uint32_t key, uint32_t mods) {
	struct editor *e = data;
	e->mods = mods;
	if (e->state == EDITING) {
		act(e, key, mods);
	}
}

static void on_wheel(void *data, int32_t steps) {
	struct editor *e = data;
	// up, negative, is larger
	tool_resize(&e->tool, -steps);
	update_hud(e);
}

// Shift squares the shape being drawn without waiting for pointer motion
static void on_mods(void *data, uint32_t mods) {
	struct editor *e = data;
	e->mods = mods;
	if (e->tool.dragging && e->state == EDITING) {
		drag(e);
	}
}

static const struct input_listener input_listener = {
	.enter = on_enter,
	.leave = on_leave,
	.motion = on_motion,
	.button = on_button,
	.key = on_key,
	.mods = on_mods,
	.wheel = on_wheel,
};

// every Wayland object, in reverse creation order; the pixels stay
static void unmap(struct editor *e) {
	catchers_destroy(e->catchers);
	e->catchers = NULL;
	if (e->frame) {
		wl_callback_destroy(e->frame);
		e->frame = NULL;
	}
	for (int i = 1; i >= 0; i--) {
		if (e->bufs[i].wl) {
			wl_buffer_destroy(e->bufs[i].wl);
			e->bufs[i].wl = NULL;
		}
	}
	hud_destroy(e->hud);
	e->hud = NULL;
	if (e->back_viewport) {
		wp_viewport_destroy(e->back_viewport);
		e->back_viewport = NULL;
	}
	if (e->back_sub) {
		wl_subsurface_destroy(e->back_sub);
		e->back_sub = NULL;
	}
	if (e->back) {
		wl_surface_destroy(e->back);
		e->back = NULL;
	}
	if (e->img_viewport) {
		wp_viewport_destroy(e->img_viewport);
		e->img_viewport = NULL;
	}
	if (e->sub) {
		wl_subsurface_destroy(e->sub);
		e->sub = NULL;
	}
	if (e->img) {
		wl_surface_destroy(e->img);
		e->img = NULL;
	}
	if (e->layer) {
		zwlr_layer_surface_v1_destroy(e->layer);
		e->layer = NULL;
	}
	if (e->viewport) {
		wp_viewport_destroy(e->viewport);
		e->viewport = NULL;
	}
	if (e->surface) {
		wl_surface_destroy(e->surface);
		e->surface = NULL;
	}
	if (e->no_input) {
		wl_region_destroy(e->no_input);
		e->no_input = NULL;
	}
	if (e->dim) {
		wl_buffer_destroy(e->dim);
		e->dim = NULL;
	}
	input_destroy(e->input);
	e->input = NULL;
}

// two image-size buffers in one memfd
static bool add_buffers(struct editor *e) {
	size_t size = e->stride * (size_t)e->whole.h;
	void *map;
	struct wl_shm_pool *pool = conn_shm_pool(
		e->conn, 2 * size, "the editor's display buffers", &map);
	if (!pool) {
		return false;
	}
	e->map = map;
	e->map_len = 2 * size;
	uint32_t format = e->in.image.format == PNG_ARGB8888
				  ? WL_SHM_FORMAT_ARGB8888
				  : WL_SHM_FORMAT_XRGB8888;
	for (int i = 0; i < 2; i++) {
		struct buf *b = &e->bufs[i];
		b->e = e;
		b->data = e->map + (size_t)i * size;
		b->stale = e->whole;
		b->wl = wl_shm_pool_create_buffer(pool, (int32_t)(i * size),
			e->whole.w, e->whole.h, (int32_t)e->stride, format);
		if (!b->wl) {
			break;
		}
		wl_buffer_add_listener(b->wl, &buffer_listener, b);
	}
	// the buffers keep the pool's memory alive
	wl_shm_pool_destroy(pool);
	if (!e->bufs[0].wl || !e->bufs[1].wl) {
		log_error("out of memory");
		return false;
	}
	return true;
}

struct editor *edit_create(struct conn *c, const struct edit_input *in) {
	uint32_t iw = in->image.width;
	uint32_t ih = in->image.height;
	// both buffers share one pool, sized by an int32
	if (iw == 0 || ih == 0 ||
		(uint64_t)iw * ih * 4 * 2 > (uint64_t)INT32_MAX) {
		log_error("the %ux%u image is too large to edit", iw, ih);
		return NULL;
	}
	struct editor *e = calloc(1, sizeof(*e));
	if (!e) {
		log_error("out of memory");
		return NULL;
	}
	*e = (struct editor){
		.conn = c,
		.in = *in,
		.whole = {0, 0, (int32_t)iw, (int32_t)ih},
		.stride = (size_t)iw * 4,
		.state = EDITING,
	};
	tool_init(&e->tool, iw, ih);
	// reports its own error
	e->input = input_create(c, &input_listener, e);
	if (!e->input) {
		goto fail;
	}
	e->dim = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
		c->single_pixel, 0, 0, 0, OVERLAY_DIM_ALPHA);
	e->no_input = wl_compositor_create_region(c->compositor);
	e->surface = wl_compositor_create_surface(c->compositor);
	if (!e->dim || !e->no_input || !e->surface) {
		goto oom;
	}
	e->viewport = wp_viewporter_get_viewport(c->viewporter, e->surface);
	e->layer = zwlr_layer_shell_v1_get_layer_surface(c->layer_shell,
		e->surface, in->output->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		"sweetshot");
	e->img = wl_compositor_create_surface(c->compositor);
	if (!e->viewport || !e->layer || !e->img) {
		goto oom;
	}
	e->sub = wl_subcompositor_get_subsurface(
		c->subcompositor, e->img, e->surface);
	e->img_viewport = wp_viewporter_get_viewport(c->viewporter, e->img);
	if (!e->sub || !e->img_viewport) {
		goto oom;
	}
	// pointer input always lands on the layer, in its coordinates
	wl_surface_set_input_region(e->img, e->no_input);
	if (in->backdrop) {
		e->back = wl_compositor_create_surface(c->compositor);
		e->back_sub =
			e->back ? wl_subcompositor_get_subsurface(
					  c->subcompositor, e->back, e->surface)
				: NULL;
		e->back_viewport = e->back ? wp_viewporter_get_viewport(
						     c->viewporter, e->back)
					   : NULL;
		if (!e->back_sub || !e->back_viewport) {
			goto oom;
		}
		// the dim is the layer's own buffer, the backdrop goes under it
		wl_subsurface_place_below(e->back_sub, e->surface);
		wl_surface_set_input_region(e->back, e->no_input);
	}
	// reports its own error
	e->hud = hud_create(c, e->surface, e->no_input);
	if (!e->hud || !add_buffers(e)) {
		goto fail;
	}
	overlay_map(e->layer, e->surface, &layer_listener, e);
	// keys reach the editor whichever output the pointer is on
	e->catchers = catchers_create(c, in->output);
	if (!e->catchers) {
		goto fail;
	}
	return e;

oom:
	log_error("out of memory");
fail:
	edit_destroy(e);
	return NULL;
}

bool edit_wait_shown(struct editor *e) {
	while (!e->shown && e->state == EDITING) {
		if (wl_display_dispatch(e->conn->display) < 0) {
			fail(e, "lost the Wayland connection");
		}
	}
	return e->state != FAILED;
}

enum edit_result edit_run(struct editor *e, struct png_image *view) {
	while (e->state == EDITING) {
		if (wl_display_dispatch(e->conn->display) < 0) {
			fail(e, "lost the Wayland connection");
		}
	}
	if (e->state != DONE) {
		return e->state == CANCELLED ? EDIT_CANCELLED : EDIT_FAILED;
	}

	// a shape still being dragged is dropped, as with the right button,
	// nothing redraws once editing is over, the damage only marks stale
	damage(e, tool_drop(&e->tool));
	unmap(e);
	if (wl_display_roundtrip(e->conn->display) < 0) {
		log_error("lost the Wayland connection");
		return EDIT_FAILED;
	}
	struct buf *b = &e->bufs[e->front];
	render(e, e->front, b->stale);
	*view = (struct png_image){
		.width = e->in.image.width,
		.height = e->in.image.height,
		.stride = e->stride,
		.format = e->in.image.format,
		.data = b->data,
	};
	return EDIT_DONE;
}

void edit_destroy(struct editor *e) {
	if (!e) {
		return;
	}
	unmap(e);
	wl_display_flush(e->conn->display);
	if (e->map) {
		munmap(e->map, e->map_len);
	}
	tool_finish(&e->tool);
	free(e);
}
