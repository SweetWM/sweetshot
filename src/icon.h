#ifndef SWEETSHOT_ICON_H
#define SWEETSHOT_ICON_H

#include "draw.h"

enum icon_type {
	ICON_TOOL, // kind in color, size, fill; a badge shows its number
	ICON_FILL,
	ICON_COLOR, // a disc of color
	ICON_SIZE,  // a dot of size logical px in color
	ICON_UNDO,
	ICON_REDO,
	ICON_DONE,
	ICON_CANCEL,
};

struct icon {
	enum icon_type type;
	enum draw_kind kind;
	uint32_t color;
	float size; // logical px
	bool fill;
	uint32_t badge; // the number a badge icon shows, 1 and up
};

enum {
	ICON_SIDE = 32, // the grid icons are designed on
	// a badge numbered n takes n − 1 invisible badges before it, up to 99
	ICON_OPS = 100,
	ICON_PTS = 13, // polyline points one icon may use
};

size_t icon_ops(const struct icon *ic, float x, float y, float f,
	struct draw_op *ops, struct raster_point *pts);

#endif
