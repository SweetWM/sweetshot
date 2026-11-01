#ifndef SWEETSHOT_CLIP_H
#define SWEETSHOT_CLIP_H

#include "wl.h"

#include <stddef.h>
#include <stdint.h>

struct clip {
	struct conn *conn;
	const uint8_t *png;
	size_t len;
	struct ext_data_control_device_v1 *device;
	struct ext_data_control_source_v1 *source;
	bool cancelled; // another client took the selection, or no device
};

bool clip_set(struct clip *cl, struct conn *c, const uint8_t *png, size_t len);

bool clip_serve(struct clip *cl);

void clip_finish(struct clip *cl);

#endif
