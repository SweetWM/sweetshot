#include "sprite.h"

#include "log.h"
#include "viewporter-client-protocol.h"

#include <stdlib.h>
#include <sys/mman.h>

struct sprite {
	struct conn *conn;
	struct wl_surface *surface;
	struct wl_subsurface *sub;
	struct wp_viewport *viewport;
	struct wl_buffer *buffer; // the picture on screen, or to show
	struct wl_buffer *next;	  // a new picture for the next apply
	int32_t w;		  // logical size of next, then of buffer
	int32_t h;
	int32_t x;
	int32_t y;
	bool shown;
	bool mapped;
	bool dirty;
};

// each picture is a buffer of its own that is never written again, so the
// compositor never reads one being drawn
static struct wl_buffer *render(struct sprite *s, uint32_t pw, uint32_t ph,
	uint32_t bg, const struct draw_op *ops, size_t n) {
	if (pw == 0 || ph == 0 || pw > 16384 || ph > 16384) {
		log_error("invalid overlay picture size %ux%u", pw, ph);
		return NULL;
	}
	size_t stride = (size_t)pw * 4;
	size_t size = stride * ph;
	uint32_t *fill = malloc(size);
	if (!fill) {
		log_error("out of memory");
		return NULL;
	}
	void *map;
	struct wl_shm_pool *pool =
		conn_shm_pool(s->conn, size, "an overlay picture", &map);
	if (!pool) {
		free(fill);
		return NULL;
	}
	// little-endian ARGB8888 is the wl_shm byte order
	for (size_t i = 0; i < size / 4; i++) {
		fill[i] = bg;
	}
	struct png_image src = {pw, ph, stride, PNG_ARGB8888, (uint8_t *)fill};
	draw_render(&src, ops, n, NULL, map, stride,
		(struct rect){0, 0, (int32_t)pw, (int32_t)ph});
	free(fill);
	struct wl_buffer *buffer =
		wl_shm_pool_create_buffer(pool, 0, (int32_t)pw, (int32_t)ph,
			(int32_t)stride, WL_SHM_FORMAT_ARGB8888);
	// the compositor keeps the pool's memory, and nothing writes this
	// picture again
	wl_shm_pool_destroy(pool);
	munmap(map, size);
	if (!buffer) {
		log_error("out of memory");
	}
	return buffer;
}

bool sprite_draw(struct sprite *s, int32_t w, int32_t h, uint32_t pw,
	uint32_t ph, uint32_t bg, const struct draw_op *ops, size_t n) {
	struct wl_buffer *b = render(s, pw, ph, bg, ops, n);
	if (!b) {
		return false;
	}
	if (s->next) {
		wl_buffer_destroy(s->next);
	}
	s->next = b;
	s->w = w;
	s->h = h;
	s->dirty = true;
	return true;
}

void sprite_move(struct sprite *s, int32_t x, int32_t y, bool shown) {
	// moving a hidden sprite changes nothing on screen
	if (shown != s->shown || (shown && (x != s->x || y != s->y))) {
		s->dirty = true;
	}
	s->x = x;
	s->y = y;
	s->shown = shown;
}

bool sprite_dirty(const struct sprite *s) {
	return s->dirty;
}

void sprite_apply(struct sprite *s) {
	struct wl_buffer *old = NULL;
	if (s->next) {
		old = s->buffer;
		s->buffer = s->next;
		s->next = NULL;
		wp_viewport_set_destination(s->viewport, s->w, s->h);
	}
	wl_subsurface_set_position(s->sub, s->x, s->y);
	bool show = s->shown && s->buffer;
	if (show && (!s->mapped || old)) {
		wl_surface_attach(s->surface, s->buffer, 0, 0);
		wl_surface_damage_buffer(
			s->surface, 0, 0, INT32_MAX, INT32_MAX);
	} else if (!show && s->mapped) {
		wl_surface_attach(s->surface, NULL, 0, 0);
	}
	s->mapped = show;
	wl_surface_commit(s->surface);
	// after the commit that replaced it
	if (old) {
		wl_buffer_destroy(old);
	}
	s->dirty = false;
}

struct sprite *sprite_create(
	struct conn *c, struct wl_surface *parent, struct wl_region *no_input) {
	struct sprite *s = calloc(1, sizeof(*s));
	if (!s) {
		log_error("out of memory");
		return NULL;
	}
	*s = (struct sprite){.conn = c};
	s->surface = wl_compositor_create_surface(c->compositor);
	if (!s->surface) {
		goto fail;
	}
	s->sub = wl_subcompositor_get_subsurface(
		c->subcompositor, s->surface, parent);
	s->viewport = wp_viewporter_get_viewport(c->viewporter, s->surface);
	if (!s->sub || !s->viewport) {
		goto fail;
	}
	// pointer input lands on the parent, in its coordinates
	wl_surface_set_input_region(s->surface, no_input);
	return s;

fail:
	log_error("out of memory");
	sprite_destroy(s);
	return NULL;
}

void sprite_destroy(struct sprite *s) {
	if (!s) {
		return;
	}
	if (s->next) {
		wl_buffer_destroy(s->next);
	}
	if (s->buffer) {
		wl_buffer_destroy(s->buffer);
	}
	if (s->viewport) {
		wp_viewport_destroy(s->viewport);
	}
	if (s->sub) {
		wl_subsurface_destroy(s->sub);
	}
	if (s->surface) {
		wl_surface_destroy(s->surface);
	}
	free(s);
}
