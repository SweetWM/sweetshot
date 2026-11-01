#ifndef SWEETSHOT_SELECT_H
#define SWEETSHOT_SELECT_H

#include "rect.h"
#include "wl.h"

struct choice {
	const struct output *output;
	struct rect rect; // output-local logical
	int32_t width;	  // logical size of the output
	int32_t height;
	bool cursor;
};

enum select_result {
	SELECT_CONFIRMED,
	SELECT_CANCELLED,
	SELECT_FAILED,
};

struct selector;

// maps an overlay on every output, reports the error and returns NULL on
// failure
struct selector *select_create(struct conn *c, bool cursor);

enum select_result select_run(struct selector *s, struct choice *choice);

void select_destroy(struct selector *s);

#endif
