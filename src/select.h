#ifndef SWEETSHOT_SELECT_H
#define SWEETSHOT_SELECT_H

#include "rect.h"
#include "wl.h"

struct shot;
struct capture;

struct choice {
	const struct output *output;
	struct shot *shot; // its frozen pixels, in the chosen variant
	struct rect rect;  // output-local logical
	int32_t width;	   // logical size of the output
	int32_t height;
};

enum select_result {
	SELECT_CONFIRMED, // every capture done
	SELECT_CANCELLED,
	SELECT_FAILED,
};

struct selector;


struct selector *select_create(struct conn *c, bool cursor, struct capture *cap,
	struct shot *plain, struct shot *pointer);

enum select_result select_run(struct selector *s, struct choice *choice);

void select_destroy(struct selector *s);

#endif
