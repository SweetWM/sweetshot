#ifndef SWEETSHOT_CLIP_H
#define SWEETSHOT_CLIP_H

#include "wl.h"

#include <stddef.h>
#include <stdint.h>

enum { CLIP_TRANSFERS = 16 };

struct transfer {
	int fd;
	size_t done; // bytes of the png written so far
};

struct clip {
	struct conn *conn;
	const uint8_t *png;
	size_t len;
	struct ext_data_control_device_v1 *device;
	struct ext_data_control_source_v1 *source;
	struct transfer transfers[CLIP_TRANSFERS];
	size_t n_transfers;
	bool cancelled; // another client took the selection
	bool finished;	// the compositor ended the device, nothing is served
};

bool clip_set(struct clip *cl, struct conn *c, const uint8_t *png, size_t len);

bool clip_serve(struct clip *cl);

void clip_finish(struct clip *cl);

#endif
