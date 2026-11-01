#include "capture.h"

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum result {
	PENDING,
	READY,
	FAILED,
};

struct state {
	uint32_t width;
	uint32_t height;
	bool xrgb;
	bool argb;
	uint32_t formats[8]; // offered, for the error message
	size_t n_formats;
	bool done;
	bool stopped;
	uint32_t transform;
	enum result result;
	uint32_t reason;
};

static void session_buffer_size(void *data,
	struct ext_image_copy_capture_session_v1 *session, uint32_t width,
	uint32_t height) {
	(void)session;
	struct state *st = data;
	st->width = width;
	st->height = height;
}

static void session_shm_format(void *data,
	struct ext_image_copy_capture_session_v1 *session, uint32_t format) {
	(void)session;
	struct state *st = data;
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

static void session_done(
	void *data, struct ext_image_copy_capture_session_v1 *session) {
	(void)session;
	struct state *st = data;
	st->done = true;
}

static void session_stopped(
	void *data, struct ext_image_copy_capture_session_v1 *session) {
	(void)session;
	struct state *st = data;
	st->stopped = true;
	// a stopped session never answers a pending frame
	if (st->result == PENDING) {
		st->result = FAILED;
		st->reason =
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED;
	}
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
	struct state *st = data;
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

static void frame_ready(
	void *data, struct ext_image_copy_capture_frame_v1 *frame) {
	(void)frame;
	struct state *st = data;
	st->result = READY;
}

static void frame_failed(void *data,
	struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason) {
	(void)frame;
	struct state *st = data;
	st->result = FAILED;
	st->reason = reason;
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
			"compositor stopped the capture of %s (session locked "
			"or output gone)",
			name);
		break;
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
		log_error("capture of %s failed (reason: buffer_constraints): "
			  "the output changed, run again",
			name);
		break;
	default:
		log_error("capture of %s failed (reason: unknown)", name);
		break;
	}
}

static bool dispatch(struct conn *c) {
	if (wl_display_dispatch(c->display) < 0) {
		log_error("lost the Wayland connection");
		return false;
	}
	return true;
}

static bool run(struct conn *c, struct ext_image_capture_source_v1 *source,
	bool cursor, const char *name, struct shot *shot) {
	struct state st = {.transform = WL_OUTPUT_TRANSFORM_NORMAL};
	struct ext_image_copy_capture_frame_v1 *frame = NULL;
	struct wl_buffer *buffer = NULL;
	void *map = MAP_FAILED;
	size_t size = 0;
	bool ok = false;

	uint32_t options =
		cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS
		       : 0;
	struct ext_image_copy_capture_session_v1 *session =
		ext_image_copy_capture_manager_v1_create_session(
			c->capture, source, options);
	if (!session) {
		log_error("out of memory");
		return false;
	}
	ext_image_copy_capture_session_v1_add_listener(
		session, &session_listener, &st);
	while (!st.done && !st.stopped) {
		if (!dispatch(c)) {
			goto out;
		}
	}
	if (st.stopped) {
		report_failure(
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED,
			name);
		goto out;
	}
	if (!st.xrgb && !st.argb) {
		report_formats(&st, name);
		goto out;
	}
	// wl_shm pools are sized by an int32
	size_t stride = (size_t)st.width * 4;
	if (st.width == 0 || st.height == 0 ||
		__builtin_mul_overflow(stride, st.height, &size) ||
		size > INT32_MAX) {
		log_error(
			"compositor sent an invalid capture size %ux%u for %s",
			st.width, st.height, name);
		goto out;
	}
	uint32_t format =
		st.xrgb ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888;

	int fd = memfd_create("sweetshot", MFD_CLOEXEC);
	if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
		map = mmap(
			NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	}
	if (map == MAP_FAILED) {
		log_error("cannot allocate the shared-memory capture buffer");
		if (fd >= 0) {
			close(fd);
		}
		goto out;
	}
	// libwayland dups the fd when it sends create_pool
	struct wl_shm_pool *pool =
		wl_shm_create_pool(c->shm, fd, (int32_t)size);
	close(fd);
	if (pool) {
		buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)st.width,
			(int32_t)st.height, (int32_t)stride, format);
		// the buffer keeps the pool's memory alive
		wl_shm_pool_destroy(pool);
	}
	frame = buffer ? ext_image_copy_capture_session_v1_create_frame(session)
		       : NULL;
	if (!frame) {
		log_error("out of memory");
		goto out;
	}
	ext_image_copy_capture_frame_v1_add_listener(
		frame, &frame_listener, &st);
	ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(
		frame, 0, 0, (int32_t)st.width, (int32_t)st.height);
	ext_image_copy_capture_frame_v1_capture(frame);
	while (st.result == PENDING) {
		if (!dispatch(c)) {
			goto out;
		}
	}
	if (st.result == FAILED) {
		report_failure(st.reason, name);
		goto out;
	}
	if (st.transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		log_error("compositor sent an invalid transform %u for %s",
			st.transform, name);
		goto out;
	}

	*shot = (struct shot){
		.image =
			{
				.width = st.width,
				.height = st.height,
				.stride = stride,
				.format = st.xrgb ? PNG_XRGB8888 : PNG_ARGB8888,
				.data = map,
			},
		.map = map,
		.map_len = size,
	};
	map = MAP_FAILED; // owned by shot
	if (st.transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		shot->heap = malloc(size);
		if (!shot->heap) {
			log_error("out of memory");
			shot_finish(shot);
			goto out;
		}
		shot->image =
			png_upright(&shot->image, st.transform, shot->heap);
	}
	ok = true;

out:
	if (frame) {
		ext_image_copy_capture_frame_v1_destroy(frame);
	}
	if (buffer) {
		wl_buffer_destroy(buffer);
	}
	ext_image_copy_capture_session_v1_destroy(session);
	if (map != MAP_FAILED) {
		munmap(map, size);
	}
	return ok;
}

bool capture_output(struct conn *c, const struct output *o, bool cursor,
	struct shot *shot) {
	struct ext_image_capture_source_v1 *source =
		ext_output_image_capture_source_manager_v1_create_source(
			c->output_source, o->wl);
	if (!source) {
		log_error("out of memory");
		return false;
	}
	bool ok = run(c, source, cursor, o->name, shot);
	ext_image_capture_source_v1_destroy(source);
	return ok;
}

void shot_finish(struct shot *shot) {
	free(shot->heap);
	if (shot->map) {
		munmap(shot->map, shot->map_len);
	}
	*shot = (struct shot){0};
}
