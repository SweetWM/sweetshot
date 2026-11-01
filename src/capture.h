#ifndef SWEETSHOT_CAPTURE_H
#define SWEETSHOT_CAPTURE_H

#include "png.h"
#include "wl.h"

struct shot {
	struct png_image image; // upright, points into map or heap
	void *map;
	size_t map_len;
	uint8_t *heap; // transformed copy, NULL for a normal transform
};

// captures the whole output, reports the error and returns false on failure
bool capture_output(
	struct conn *c, const struct output *o, bool cursor, struct shot *shot);
void shot_finish(struct shot *shot);

#endif
