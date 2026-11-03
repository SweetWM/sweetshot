#ifndef SWEETSHOT_INPUT_H
#define SWEETSHOT_INPUT_H

#include "wl.h"

// modifier state passed with key presses and mods()
enum {
	INPUT_SHIFT = 1,
	INPUT_CTRL = 2,
};

// positions are surface-local logical coordinates, as the compositor sent
// them; every callback but mods and wheel must be set
struct input_listener {
	void (*enter)(void *data, struct wl_surface *surface, wl_fixed_t x,
		wl_fixed_t y);
	void (*leave)(void *data);
	void (*motion)(void *data, wl_fixed_t x, wl_fixed_t y);
	void (*button)(void *data, uint32_t button, bool pressed);
	// presses of keys other than Shift and Ctrl; raw evdev codes
	void (*key)(void *data, uint32_t key, uint32_t mods);
	// Shift or Ctrl went down or up, or focus changed them; may be NULL
	void (*mods)(void *data, uint32_t mods);
	// vertical wheel clicks, negative is up; may be NULL
	void (*wheel)(void *data, int32_t steps);
};

enum input_cursor {
	INPUT_CROSSHAIR, // set on every pointer enter
	INPUT_DEFAULT,
};

struct input;

// binds the seat's pointer and keyboard, reports the error and returns NULL
// on failure
struct input *input_create(
	struct conn *c, const struct input_listener *l, void *data);

void input_destroy(struct input *in);

// the pointer's shape until it changes or the next enter; sends nothing
// when it is already that shape
void input_set_cursor(struct input *in, enum input_cursor c);

#endif
