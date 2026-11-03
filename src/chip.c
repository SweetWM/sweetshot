#include "chip.h"

#include "log.h"
#include "viewporter-client-protocol.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
	SIDE = 32, // logical px
	GAP = 16,  // from the pointer
	// numbers past this draw no digits (draw.c)
	BADGES = 99,
	SQUIGGLE = 13,
};

struct chip {
	struct conn *conn;
	struct wl_surface *surface;
	struct wl_subsurface *sub;
	struct wp_viewport *viewport;
	struct wl_buffer *buffer; // the glyph on screen, or to show
	struct wl_buffer *next;	  // a new glyph for the next apply
	struct chip_look look;
	double scale; // of the current glyph, 0 before the first
	int32_t x;
	int32_t y;
	bool shown;
	bool mapped;
	bool dirty;
};

static struct raster_point at(float x, float y, float f) {
	return (struct raster_point){x * f, y * f};
}

static size_t glyph(const struct chip_look *l, float f, struct draw_op *ops,
	struct raster_point *squiggle) {
	struct draw_op op = {
		.kind = l->kind,
		.color = l->color,
		.fill = l->fill,
		.width = fminf(l->size, 6.0f) * f,
		.a = at(8, 24, f),
		.b = at(24, 8, f),
	};
	switch (l->kind) {
	case DRAW_ARROW:
		// the head is 4× the width
		op.width = fminf(l->size, 3.0f) * f;
		break;
	case DRAW_LINE:
		break;
	case DRAW_RECT:
		op.width = fminf(l->size, 4.0f) * f;
		op.a = at(8, 8, f);
		op.b = at(24, 24, f);
		break;
	case DRAW_ELLIPSE:
		op.width = fminf(l->size, 4.0f) * f;
		op.a = at(6, 9, f);
		op.b = at(26, 23, f);
		break;
	case DRAW_PEN:
		for (int i = 0; i < SQUIGGLE; i++) {
			squiggle[i] = at(6.0f + (float)i * 20.0f / 12.0f,
				16.0f - 6.0f * sinf((float)i * 0.5236f), f);
		}
		op.pts = squiggle;
		op.n_pts = SQUIGGLE;
		break;
	case DRAW_HIGHLIGHT:
		// a marker stroke; multiply would vanish on the dark backdrop
		op.kind = DRAW_RECT;
		op.fill = true;
		op.a = at(6, 11, f);
		op.b = at(26, 21, f);
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
				.a = at(x, y, f),
				.b = at(x + 8, y + 8, f),
			};
		}
		return 4;
	case DRAW_BADGE: {
		// badges are numbered by the badges before them, so the
		// next number takes that many invisible ones
		uint32_t before = l->badge > 0 ? l->badge - 1 : 0;
		before = before > BADGES ? BADGES : before;
		for (uint32_t i = 0; i < before; i++) {
			ops[i] = (struct draw_op){
				.kind = DRAW_BADGE,
				.width = 1,
				.a = {-1000, -1000},
			};
		}
		op.width = fminf(l->size, 4.5f) * f;
		op.a = at(16, 16, f);
		ops[before] = op;
		return before + 1;
	}
	}
	ops[0] = op;
	return 1;
}

static struct wl_buffer *render(
	struct chip *k, const struct chip_look *look, double scale) {
	struct wl_buffer *buffer = NULL;
	uint8_t *bg = NULL;
	void *map = MAP_FAILED;
	long px = lround(SIDE * scale);
	uint32_t n = px < 1 ? 1 : px > 1024 ? 1024 : (uint32_t)px;
	size_t stride = (size_t)n * 4;
	size_t size = stride * n;
	int fd = memfd_create("sweetshot-chip", MFD_CLOEXEC);
	if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
		map = mmap(
			NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	}
	bg = malloc(size);
	if (map == MAP_FAILED || !bg) {
		log_error("cannot allocate the tool chip");
		goto out;
	}
	// premultiplied black at 60%, so any colour reads on any background
	for (size_t i = 0; i < size; i += 4) {
		memcpy(bg + i, (const uint8_t[]){0, 0, 0, 0x99}, 4);
	}
	struct png_image src = {n, n, stride, PNG_ARGB8888, bg};
	struct draw_op ops[BADGES + 1];
	struct raster_point squiggle[SQUIGGLE];
	size_t count = glyph(look, (float)n / SIDE, ops, squiggle);
	draw_render(&src, ops, count, NULL, map, stride,
		(struct rect){0, 0, (int32_t)n, (int32_t)n});
	struct wl_shm_pool *pool =
		wl_shm_create_pool(k->conn->shm, fd, (int32_t)size);
	if (pool) {
		buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)n,
			(int32_t)n, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
		wl_shm_pool_destroy(pool);
	}
	if (!buffer) {
		log_error("out of memory");
	}

out:
	// the glyph never changes once drawn; the compositor keeps the pool
	free(bg);
	if (map != MAP_FAILED) {
		munmap(map, size);
	}
	if (fd >= 0) {
		close(fd);
	}
	return buffer;
}

static bool same(const struct chip_look *a, const struct chip_look *b) {
	return a->kind == b->kind && a->color == b->color &&
	       a->size == b->size && a->fill == b->fill && a->badge == b->badge;
}

bool chip_set(struct chip *k, const struct chip_look *look, double scale) {
	if (scale == k->scale && same(look, &k->look)) {
		return true;
	}
	struct wl_buffer *b = render(k, look, scale);
	if (!b) {
		return false;
	}
	if (k->next) {
		wl_buffer_destroy(k->next);
	}
	k->next = b;
	k->look = *look;
	k->scale = scale;
	k->dirty = true;
	return true;
}

void chip_move(struct chip *k, double px, double py, int32_t lw, int32_t lh,
	bool shown) {
	int32_t x = (int32_t)floor(px) + GAP;
	int32_t y = (int32_t)floor(py) + GAP;
	if (x + SIDE > lw) {
		x -= 2 * GAP + SIDE;
	}
	if (y + SIDE > lh) {
		y -= 2 * GAP + SIDE;
	}
	// moving a hidden chip changes nothing on screen
	if (shown != k->shown || (shown && (x != k->x || y != k->y))) {
		k->dirty = true;
	}
	k->x = x;
	k->y = y;
	k->shown = shown;
}

bool chip_dirty(const struct chip *k) {
	return k->dirty;
}

void chip_apply(struct chip *k) {
	struct wl_buffer *old = NULL;
	if (k->next) {
		old = k->buffer;
		k->buffer = k->next;
		k->next = NULL;
	}
	wl_subsurface_set_position(k->sub, k->x, k->y);
	bool show = k->shown && k->buffer;
	if (show && (!k->mapped || old)) {
		wl_surface_attach(k->surface, k->buffer, 0, 0);
		wl_surface_damage_buffer(
			k->surface, 0, 0, INT32_MAX, INT32_MAX);
	} else if (!show && k->mapped) {
		wl_surface_attach(k->surface, NULL, 0, 0);
	}
	k->mapped = show;
	wl_surface_commit(k->surface);
	// after the commit that replaced it
	if (old) {
		wl_buffer_destroy(old);
	}
	k->dirty = false;
}

struct chip *chip_create(
	struct conn *c, struct wl_surface *parent, struct wl_region *no_input) {
	struct chip *k = calloc(1, sizeof(*k));
	if (!k) {
		log_error("out of memory");
		return NULL;
	}
	*k = (struct chip){.conn = c};
	k->surface = wl_compositor_create_surface(c->compositor);
	if (!k->surface) {
		goto fail;
	}
	k->sub = wl_subcompositor_get_subsurface(
		c->subcompositor, k->surface, parent);
	k->viewport = wp_viewporter_get_viewport(c->viewporter, k->surface);
	if (!k->sub || !k->viewport) {
		goto fail;
	}
	// pointer input lands on the parent, in its coordinates
	wl_surface_set_input_region(k->surface, no_input);
	wp_viewport_set_destination(k->viewport, SIDE, SIDE);
	return k;

fail:
	log_error("out of memory");
	chip_destroy(k);
	return NULL;
}

void chip_destroy(struct chip *k) {
	if (!k) {
		return;
	}
	if (k->next) {
		wl_buffer_destroy(k->next);
	}
	if (k->buffer) {
		wl_buffer_destroy(k->buffer);
	}
	if (k->viewport) {
		wp_viewport_destroy(k->viewport);
	}
	if (k->sub) {
		wl_subsurface_destroy(k->sub);
	}
	if (k->surface) {
		wl_surface_destroy(k->surface);
	}
	free(k);
}
