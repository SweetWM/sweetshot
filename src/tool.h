#ifndef SWEETSHOT_TOOL_H
#define SWEETSHOT_TOOL_H

#include "draw.h"

struct tool {
	struct draw_ops ops;
	struct draw_op live;
	bool dragging;
	float w; // image size, drags stay inside it
	float h;
	float scale;  // image pixels per logical pixel; sizes are logical
	size_t color; // draw_palette index
	size_t size;  // draw_sizes index
};

void tool_init(struct tool *t, uint32_t w, uint32_t h);

void tool_finish(struct tool *t);

// starts a shape at p, a point on the image
void tool_press(struct tool *t, struct raster_point p);

// moves the end of the shape to p (clamped), squared when asked
struct rect tool_drag(struct tool *t, struct raster_point p, bool square);

// the shape becomes an op. False when out of memory
bool tool_release(struct tool *t, struct rect *damage);

// throws the shape away
struct rect tool_drop(struct tool *t);

// Ctrl+Z undo, Ctrl+Shift+Z and Ctrl+Y redo, other keys do nothing
struct rect tool_key(struct tool *t, uint32_t key, bool ctrl, bool shift);

#endif
