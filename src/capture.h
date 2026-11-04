#ifndef SWEETSHOT_CAPTURE_H
#define SWEETSHOT_CAPTURE_H

#include "png.h"
#include "wl.h"

struct shot {
	struct png_image image;
	void *map;
	size_t map_len;
	uint8_t *heap;
	struct wl_buffer *buffer;
	uint32_t transform;
};

struct capture_req {
	const struct output *output;
	bool cursor;
};

enum capture_status {
	CAPTURE_PENDING,
	CAPTURE_DONE,
	CAPTURE_FAILED, // reported already
};

struct capture;

struct capture *capture_start(struct conn *c, const struct capture_req *reqs,
	size_t n, struct shot *shots);
enum capture_status capture_status(const struct capture *cap);
// leaves the shots taken so far to the caller
void capture_destroy(struct capture *cap);

bool capture_output(
	struct conn *c, const struct output *o, bool cursor, struct shot *shot);
bool capture_window(
	struct conn *c, const char *identifier, bool cursor, struct shot *shot);

bool shot_upright(struct shot *shot);

void shot_finish(struct shot *shot);

#endif
