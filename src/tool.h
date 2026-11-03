#ifndef SWEETSHOT_TOOL_H
#define SWEETSHOT_TOOL_H

#include "draw.h"

struct tool {
	struct draw_ops ops;
	struct draw_op live;
	bool dragging;
	float w; // image size, drags stay inside it
	float h;
	float scale; // image pixels per logical pixel; sizes are logical
	enum draw_kind kind; // the tool the next press uses
	size_t color;	     // draw_palette index
	size_t size;	     // draw_sizes index
	bool fill;	     // rect and ellipse
};

void tool_init(struct tool *t, uint32_t w, uint32_t h);

void tool_finish(struct tool *t);

// every change below reports the image rectangle it affects in *damage; a
// false return means out of memory

// starts a shape at p, a point on the image
bool tool_press(struct tool *t, struct raster_point p, struct rect *damage);

// moves the end of the shape to p (clamped), constrained when asked
bool tool_drag(struct tool *t, struct raster_point p, bool constrain,
	struct rect *damage);

// the shape becomes an op
bool tool_release(struct tool *t, struct rect *damage);

// throws the shape away
struct rect tool_drop(struct tool *t);

// Ctrl+Z undo, Ctrl+Shift+Z and Ctrl+Y redo; tool, colour, fill and size
// keys change the settings, not the image
struct rect tool_key(struct tool *t, uint32_t key, bool ctrl, bool shift);

// size steps, positive is larger
void tool_resize(struct tool *t, int steps);

// the number the next badge would get
uint32_t tool_next_badge(const struct tool *t);

#endif
