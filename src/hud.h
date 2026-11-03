#ifndef SWEETSHOT_HUD_H
#define SWEETSHOT_HUD_H

#include "bar.h"
#include "tool.h"
#include "wl.h"

// the editor's controls over the image: the toolbar and the chip next to
// the pointer. Neither is ever part of the image
struct hud;

// what the hud reflects, in the layer's logical coordinates
struct hud_view {
	const struct tool *tool;
	double scale; // output pixels per logical pixel
	int32_t lw;   // layer size
	int32_t lh;
	struct rect image; // where the image is shown
	double px;	   // the pointer
	double py;
	bool chip;  // the pointer is over the image and not dragging
	bool hover; // the pointer is on the layer and not dragging
};

// both hidden until the first hud_update; reports the error and returns
// NULL on failure
struct hud *hud_create(
	struct conn *c, struct wl_surface *parent, struct wl_region *no_input);

// new pictures only when what they show changed. False on failure
// (reported)
bool hud_update(struct hud *h, const struct hud_view *v);

// Tab: hide or show the toolbar
void hud_toggle(struct hud *h);

// x, y is on the shown toolbar
bool hud_on_bar(const struct hud *h, double x, double y);

bool hud_click(
	struct hud *h, double x, double y, bool pressed, struct bar_action *a);

bool hud_dirty(const struct hud *h);

// sends the changes; they land with the parent's next commit
void hud_apply(struct hud *h);

void hud_destroy(struct hud *h);

#endif
