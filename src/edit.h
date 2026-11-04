#ifndef SWEETSHOT_EDIT_H
#define SWEETSHOT_EDIT_H

#include "png.h"
#include "rect.h"
#include "wl.h"

// how the image relates to the output it is shown on
enum edit_fit {
	EDIT_OUTPUT, // the whole output
	EDIT_REGION, // sel, cropped from a full_w×full_h capture of the output
	EDIT_WINDOW, // a window: centred, shrunk to fit
};

struct edit_input {
	const struct output *output;
	struct png_image image; // must outlive the editor
	enum edit_fit fit;
	struct rect sel; // EDIT_REGION, logical
	uint32_t full_w; // EDIT_REGION
	uint32_t full_h;
	struct wl_buffer *backdrop;
	uint32_t backdrop_transform;
	struct rect backdrop_crop;
};

enum edit_result {
	EDIT_DONE,
	EDIT_CANCELLED,
	EDIT_FAILED,
};

struct editor;

// maps the editor overlay; reports the error and returns NULL on failure
struct editor *edit_create(struct conn *c, const struct edit_input *in);

// dispatches until the editor's first frame is on screen, so whatever it
// covers can go without a flash; false when it failed (reported)
bool edit_wait_shown(struct editor *e);

enum edit_result edit_run(struct editor *e, struct png_image *view);

void edit_destroy(struct editor *e);

#endif
