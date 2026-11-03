#ifndef SWEETSHOT_BAR_H
#define SWEETSHOT_BAR_H

#include "rect.h"

#include <stddef.h>

enum bar_kind {
	BAR_TOOL, // index: tool_keys
	BAR_FILL,
	BAR_COLOR, // index: draw_palette
	BAR_SIZE,  // index: draw_sizes
	BAR_UNDO,
	BAR_REDO,
	BAR_CANCEL,
	BAR_DONE,
};

enum {
	// 8 tools, fill, 9 colours, 7 sizes, undo, redo, cancel, done
	BAR_BUTTONS = 29,
	BAR_BUTTON = 32, // logical px, one icon
};

struct bar_button {
	enum bar_kind kind;
	size_t index;
	struct rect at; // bar-local, logical
};

// the toolbar's buttons and size, in logical px
struct bar {
	struct bar_button buttons[BAR_BUTTONS];
	int32_t w;
	int32_t h;
};

// what a click means: the key it stands for, or a size to set
struct bar_action {
	uint32_t key; // evdev code, 0 for none
	bool ctrl;
	bool shift;
	int size; // draw_sizes index to set, -1 for none
};

// one row on an output lw logical px wide, two (broken after fill) if one
// does not fit
void bar_layout(struct bar *b, int32_t lw);

struct rect bar_place(
	const struct bar *b, struct rect image, int32_t lw, int32_t lh);

int bar_hit(const struct bar *b, double x, double y);

struct bar_action bar_action(const struct bar *b, int button);

#endif
