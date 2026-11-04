#include "capture.h"

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct state {
	uint32_t width;
	uint32_t height;
	bool xrgb;
	bool argb;
	uint32_t formats[8]; // offered, for the error message
	size_t n_formats;
	uint32_t transform;
};

// one capture of a batch: its shot is taken as soon as its frame is ready
struct job {
	struct capture *cap;
	struct ext_image_capture_source_v1 *source;
	bool cursor;
	bool alpha; // prefer ARGB8888 when both formats are offered
	const char *name;
	struct shot *shot;
	struct state st;
	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_frame_v1 *frame;
	struct wl_buffer *buffer;
	void *map;
	size_t size;
	bool argb;
};

struct capture {
	struct conn *conn;
	struct job *jobs;
	size_t n;
	size_t left; // shots not taken yet
	bool failed;
};

static void report_failure(uint32_t reason, const char *name);
static bool request_frame(struct conn *c, struct job *j);
static bool take_shot(struct job *j);

// the first failure is the one worth reporting, the rest follow from it
static void fail(struct job *j, uint32_t reason) {
	if (!j->cap->failed) {
		report_failure(reason, j->name);
	}
	j->cap->failed = true;
}

static void session_buffer_size(void *data,
	struct ext_image_copy_capture_session_v1 *session, uint32_t width,
	uint32_t height) {
	(void)session;
	struct state *st = &((struct job *)data)->st;
	st->width = width;
	st->height = height;
}

static void session_shm_format(void *data,
	struct ext_image_copy_capture_session_v1 *session, uint32_t format) {
	(void)session;
	struct state *st = &((struct job *)data)->st;
	st->xrgb |= format == WL_SHM_FORMAT_XRGB8888;
	st->argb |= format == WL_SHM_FORMAT_ARGB8888;
	if (st->n_formats < sizeof(st->formats) / sizeof(st->formats[0])) {
		st->formats[st->n_formats++] = format;
	}
}

// SHM only: DMA-BUF needs a GPU allocator for no gain on a one-shot capture
static void session_dmabuf_device(void *data,
	struct ext_image_copy_capture_session_v1 *session,
	struct wl_array *device) {
	(void)data, (void)session, (void)device;
}

static void session_dmabuf_format(void *data,
	struct ext_image_copy_capture_session_v1 *session, uint32_t format,
	struct wl_array *modifiers) {
	(void)data, (void)session, (void)format, (void)modifiers;
}

// a later done (changed constraints) makes the pending frame fail, so only
// the first one asks for a frame
static void session_done(
	void *data, struct ext_image_copy_capture_session_v1 *session) {
	(void)session;
	struct job *j = data;
	if (!j->frame && !j->cap->failed && !request_frame(j->cap->conn, j)) {
		j->cap->failed = true;
	}
}

// a taken shot's session is gone, so this is always a lost capture
static void session_stopped(
	void *data, struct ext_image_copy_capture_session_v1 *session) {
	(void)session;
	fail(data, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
}

static const struct ext_image_copy_capture_session_v1_listener
	session_listener = {
		.buffer_size = session_buffer_size,
		.shm_format = session_shm_format,
		.dmabuf_device = session_dmabuf_device,
		.dmabuf_format = session_dmabuf_format,
		.done = session_done,
		.stopped = session_stopped,
};

static void frame_transform(void *data,
	struct ext_image_copy_capture_frame_v1 *frame, uint32_t transform) {
	(void)frame;
	struct state *st = &((struct job *)data)->st;
	st->transform = transform;
}

// one full-damage frame per session: damage and timing do not matter
static void frame_damage(void *data,
	struct ext_image_copy_capture_frame_v1 *frame, int32_t x, int32_t y,
	int32_t width, int32_t height) {
	(void)data, (void)frame, (void)x, (void)y, (void)width, (void)height;
}

static void frame_presentation_time(void *data,
	struct ext_image_copy_capture_frame_v1 *frame, uint32_t sec_hi,
	uint32_t sec_lo, uint32_t nsec) {
	(void)data, (void)frame, (void)sec_hi, (void)sec_lo, (void)nsec;
}

// the session's work is done: frame and session go now, which also keeps a
// later stop (a lock) from failing a shot already taken
static void frame_ready(
	void *data, struct ext_image_copy_capture_frame_v1 *frame) {
	struct job *j = data;
	ext_image_copy_capture_frame_v1_destroy(frame);
	j->frame = NULL;
	ext_image_copy_capture_session_v1_destroy(j->session);
	j->session = NULL;
	if (j->cap->failed) {
		return;
	}
	if (!take_shot(j)) {
		j->cap->failed = true;
		return;
	}
	j->cap->left--;
}

static void frame_failed(void *data,
	struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason) {
	(void)frame;
	fail(data, reason);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

static void report_formats(const struct state *st, const char *name) {
	// wl_shm formats are DRM fourcc codes, except ARGB8888 (0) and
	// XRGB8888 (1), which are accepted and so never listed here
	char list[8 * 6] = "";
	for (size_t i = 0; i < st->n_formats; i++) {
		uint32_t f = st->formats[i];
		char cc[6] = {(char)f, (char)(f >> 8), (char)(f >> 16),
			(char)(f >> 24), i + 1 < st->n_formats ? ',' : '\0'};
		strncat(list, cc, sizeof(list) - strlen(list) - 1);
	}
	log_error("compositor offers no XRGB8888 or ARGB8888 shm format for "
		  "%s (offered: %s)",
		name, st->n_formats ? list : "none");
}

static void report_failure(uint32_t reason, const char *name) {
	switch (reason) {
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
		log_error(
			"compositor stopped the capture of %s (session locked, "
			"or it is gone)",
			name);
		break;
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
		log_error("capture of %s failed (reason: buffer_constraints): "
			  "it changed size, run again",
			name);
		break;
	default:
		log_error("capture of %s failed (reason: unknown)", name);
		break;
	}
}

// sizes the buffer from the session's constraints and asks for the frame
static bool request_frame(struct conn *c, struct job *j) {
	const struct state *st = &j->st;
	if (!st->xrgb && !st->argb) {
		report_formats(st, j->name);
		return false;
	}
	// wl_shm pools are sized by an int32
	size_t stride = (size_t)st->width * 4;
	if (st->width == 0 || st->height == 0 ||
		__builtin_mul_overflow(stride, st->height, &j->size) ||
		j->size > INT32_MAX) {
		log_error(
			"compositor sent an invalid capture size %ux%u for %s",
			st->width, st->height, j->name);
		return false;
	}
	j->argb = st->argb && (j->alpha || !st->xrgb);
	uint32_t format =
		j->argb ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;

	int fd = memfd_create("sweetshot", MFD_CLOEXEC);
	if (fd >= 0 && ftruncate(fd, (off_t)j->size) == 0) {
		j->map = mmap(NULL, j->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			fd, 0);
	}
	if (j->map == MAP_FAILED) {
		log_error("cannot allocate the shared-memory capture buffer");
		if (fd >= 0) {
			close(fd);
		}
		return false;
	}
	// libwayland dups the fd when it sends create_pool
	struct wl_shm_pool *pool =
		wl_shm_create_pool(c->shm, fd, (int32_t)j->size);
	close(fd);
	if (pool) {
		j->buffer =
			wl_shm_pool_create_buffer(pool, 0, (int32_t)st->width,
				(int32_t)st->height, (int32_t)stride, format);
		// the buffer keeps the pool's memory alive
		wl_shm_pool_destroy(pool);
	}
	j->frame = j->buffer ? ext_image_copy_capture_session_v1_create_frame(
				       j->session)
			     : NULL;
	if (!j->frame) {
		log_error("out of memory");
		return false;
	}
	ext_image_copy_capture_frame_v1_add_listener(
		j->frame, &frame_listener, j);
	ext_image_copy_capture_frame_v1_attach_buffer(j->frame, j->buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(
		j->frame, 0, 0, (int32_t)st->width, (int32_t)st->height);
	ext_image_copy_capture_frame_v1_capture(j->frame);
	return true;
}

static bool take_shot(struct job *j) {
	const struct state *st = &j->st;
	if (st->transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		log_error("compositor sent an invalid transform %u for %s",
			st->transform, j->name);
		return false;
	}
	struct shot *shot = j->shot;
	*shot = (struct shot){
		.image =
			{
				.width = st->width,
				.height = st->height,
				.stride = (size_t)st->width * 4,
				.format = j->argb ? PNG_ARGB8888 : PNG_XRGB8888,
				.data = j->map,
			},
		.map = j->map,
		.map_len = j->size,
		.buffer = j->buffer,
		.transform = st->transform,
	};
	j->map = MAP_FAILED;
	j->buffer = NULL;
	return true;
}

// a batch of n jobs for the caller to fill, every shot cleared first so a
// failure at any point leaves them all safe to finish
static struct capture *capture_new(
	struct conn *c, size_t n, struct shot *shots) {
	for (size_t i = 0; i < n; i++) {
		shots[i] = (struct shot){0};
	}
	struct capture *cap = calloc(1, sizeof(*cap));
	struct job *jobs = calloc(n, sizeof(*jobs));
	if (!cap || !jobs) {
		log_error("out of memory");
		free(cap);
		free(jobs);
		return NULL;
	}
	*cap = (struct capture){.conn = c, .jobs = jobs, .n = n, .left = n};
	for (size_t i = 0; i < n; i++) {
		jobs[i] = (struct job){
			.cap = cap,
			.shot = &shots[i],
			.st = {.transform = WL_OUTPUT_TRANSFORM_NORMAL},
			.map = MAP_FAILED,
		};
	}
	return cap;
}

static bool capture_begin(struct capture *cap) {
	for (size_t i = 0; i < cap->n; i++) {
		struct job *j = &cap->jobs[i];
		if (!j->source) {
			log_error("out of memory");
			return false;
		}
		uint32_t options =
			j->cursor
				? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS
				: 0;
		j->session = ext_image_copy_capture_manager_v1_create_session(
			cap->conn->capture, j->source, options);
		if (!j->session) {
			log_error("out of memory");
			return false;
		}
		ext_image_copy_capture_session_v1_add_listener(
			j->session, &session_listener, j);
	}
	return true;
}

struct capture *capture_start(struct conn *c, const struct capture_req *reqs,
	size_t n, struct shot *shots) {
	struct capture *cap = capture_new(c, n, shots);
	if (!cap) {
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		struct job *j = &cap->jobs[i];
		j->source =
			ext_output_image_capture_source_manager_v1_create_source(
				c->output_source, reqs[i].output->wl);
		j->cursor = reqs[i].cursor;
		j->name = reqs[i].output->name;
	}
	if (!capture_begin(cap)) {
		capture_destroy(cap);
		return NULL;
	}
	return cap;
}

enum capture_status capture_status(const struct capture *cap) {
	return cap->failed ? CAPTURE_FAILED
	       : cap->left ? CAPTURE_PENDING
			   : CAPTURE_DONE;
}

void capture_destroy(struct capture *cap) {
	if (!cap) {
		return;
	}
	for (size_t i = cap->n; i > 0; i--) {
		struct job *j = &cap->jobs[i - 1];
		if (j->frame) {
			ext_image_copy_capture_frame_v1_destroy(j->frame);
		}
		if (j->buffer) {
			wl_buffer_destroy(j->buffer);
		}
		if (j->session) {
			ext_image_copy_capture_session_v1_destroy(j->session);
		}
		if (j->map != MAP_FAILED) {
			munmap(j->map, j->size);
		}
		if (j->source) {
			ext_image_capture_source_v1_destroy(j->source);
		}
	}
	free(cap->jobs);
	free(cap);
}

static bool capture_wait(struct capture *cap) {
	bool ok = cap != NULL;
	while (ok && capture_status(cap) == CAPTURE_PENDING) {
		if (wl_display_dispatch(cap->conn->display) < 0) {
			log_error("lost the Wayland connection");
			ok = false;
		}
	}
	ok = ok && capture_status(cap) == CAPTURE_DONE;
	for (size_t i = 0; ok && i < cap->n; i++) {
		ok = shot_upright(cap->jobs[i].shot);
	}
	if (cap && !ok) {
		for (size_t i = 0; i < cap->n; i++) {
			shot_finish(cap->jobs[i].shot);
		}
	}
	capture_destroy(cap);
	return ok;
}

bool capture_output(struct conn *c, const struct output *o, bool cursor,
	struct shot *shot) {
	struct capture_req req = {o, cursor};
	return capture_wait(capture_start(c, &req, 1, shot));
}

struct toplevels {
	const char *identifier;
	struct ext_foreign_toplevel_handle_v1 *match;
	bool closed;
};

static void handle_closed(
	void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
	struct toplevels *t = data;
	if (handle == t->match) {
		t->closed = true;
	}
}

static void handle_done(
	void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
	(void)data, (void)handle;
}

static void handle_title(void *data,
	struct ext_foreign_toplevel_handle_v1 *handle, const char *title) {
	(void)data, (void)handle, (void)title;
}

static void handle_app_id(void *data,
	struct ext_foreign_toplevel_handle_v1 *handle, const char *app_id) {
	(void)data, (void)handle, (void)app_id;
}

// every handle sends its identifier before its first done, so the others
// can go right away instead of being collected
static void handle_identifier(void *data,
	struct ext_foreign_toplevel_handle_v1 *handle, const char *identifier) {
	struct toplevels *t = data;
	if (!t->match && strcmp(identifier, t->identifier) == 0) {
		t->match = handle;
	} else if (handle != t->match) {
		ext_foreign_toplevel_handle_v1_destroy(handle);
	}
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
	.closed = handle_closed,
	.done = handle_done,
	.title = handle_title,
	.app_id = handle_app_id,
	.identifier = handle_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
	struct ext_foreign_toplevel_handle_v1 *handle) {
	(void)list;
	ext_foreign_toplevel_handle_v1_add_listener(
		handle, &handle_listener, data);
}

static void list_finished(
	void *data, struct ext_foreign_toplevel_list_v1 *list) {
	(void)data, (void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

bool capture_window(struct conn *c, const char *identifier, bool cursor,
	struct shot *shot) {
	static const char name[] = "the focused window";
	struct toplevels t = {.identifier = identifier};
	bool ok = false;
	*shot = (struct shot){0};
	// bound only here: a bound list streams every toplevel's handle
	struct ext_foreign_toplevel_list_v1 *list =
		wl_registry_bind(c->registry, c->toplevel_list_name,
			&ext_foreign_toplevel_list_v1_interface, 1);
	if (!list) {
		log_error("out of memory");
		return false;
	}
	ext_foreign_toplevel_list_v1_add_listener(list, &list_listener, &t);
	// the compositor sends every handle and its identifier before the
	// roundtrip's reply
	if (wl_display_roundtrip(c->display) < 0) {
		log_error("lost the Wayland connection");
		goto out;
	}
	if (!t.match || t.closed) {
		log_error("%s is gone (no toplevel with identifier %s)", name,
			identifier);
		goto out;
	}
	struct capture *cap = capture_new(c, 1, shot);
	if (cap) {
		struct job *j = &cap->jobs[0];
		j->source =
			ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
				c->toplevel_source, t.match);
		j->cursor = cursor;
		j->alpha = true;
		j->name = name;
		if (!capture_begin(cap)) {
			capture_destroy(cap);
			cap = NULL;
		}
	}
	// destroys the source before its handle below
	ok = capture_wait(cap);

out:
	if (t.match) {
		ext_foreign_toplevel_handle_v1_destroy(t.match);
	}
	ext_foreign_toplevel_list_v1_destroy(list);
	return ok;
}

bool shot_upright(struct shot *shot) {
	// a normal transform is upright already, the no-copy path
	if (shot->transform == WL_OUTPUT_TRANSFORM_NORMAL || shot->heap) {
		return true;
	}
	shot->heap = malloc(shot->map_len);
	if (!shot->heap) {
		log_error("out of memory");
		return false;
	}
	shot->image = png_upright(&shot->image, shot->transform, shot->heap);
	return true;
}

void shot_finish(struct shot *shot) {
	if (shot->buffer) {
		wl_buffer_destroy(shot->buffer);
	}
	free(shot->heap);
	if (shot->map) {
		munmap(shot->map, shot->map_len);
	}
	*shot = (struct shot){0};
}
