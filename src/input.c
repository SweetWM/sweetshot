#include "input.h"

#include "cursor-shape-v1-client-protocol.h"
#include "log.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <unistd.h>

struct input {
	const struct input_listener *l;
	void *data;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wp_cursor_shape_device_v1 *shape;
	uint32_t held; // mod_bit() of each modifier key that is down
};

// shift and Ctrl are tracked from their own key events: modifier masks are
// keymap indices, which need xkbcommon to interpret
static uint32_t mod_bit(uint32_t key) {
	switch (key) {
	case KEY_LEFTSHIFT:
		return 1;
	case KEY_RIGHTSHIFT:
		return 2;
	case KEY_LEFTCTRL:
		return 4;
	case KEY_RIGHTCTRL:
		return 8;
	default:
		return 0;
	}
}

static uint32_t mods(const struct input *in) {
	return (in->held & 3 ? INPUT_SHIFT : 0) |
	       (in->held & 12 ? INPUT_CTRL : 0);
}

// tells the owner when the Shift/Ctrl state changes
static void set_held(struct input *in, uint32_t held) {
	uint32_t before = mods(in);
	in->held = held;
	if (in->l->mods && mods(in) != before) {
		in->l->mods(in->data, mods(in));
	}
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
	uint32_t serial, struct wl_surface *surface, wl_fixed_t x,
	wl_fixed_t y) {
	(void)pointer;
	struct input *in = data;
	in->l->enter(in->data, surface, x, y);
	if (in->shape) {
		wp_cursor_shape_device_v1_set_shape(in->shape, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
	}
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
	uint32_t serial, struct wl_surface *surface) {
	(void)pointer, (void)serial, (void)surface;
	struct input *in = data;
	in->l->leave(in->data);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
	uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	(void)pointer, (void)time;
	struct input *in = data;
	in->l->motion(in->data, x, y);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
	uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	(void)pointer, (void)serial, (void)time;
	struct input *in = data;
	in->l->button(
		in->data, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
	uint32_t axis, wl_fixed_t value) {
	(void)data, (void)pointer, (void)time, (void)axis, (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
	(void)data, (void)pointer;
}

static void pointer_axis_source(
	void *data, struct wl_pointer *pointer, uint32_t source) {
	(void)data, (void)pointer, (void)source;
}

static void pointer_axis_stop(
	void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
	(void)data, (void)pointer, (void)time, (void)axis;
}

// the seat is bound at v5, so wheels send axis_discrete, not value120
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
	uint32_t axis, int32_t discrete) {
	(void)pointer;
	struct input *in = data;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && in->l->wheel) {
		in->l->wheel(in->data, discrete);
	}
}

static void pointer_axis_value120(
	void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value) {
	(void)data, (void)pointer, (void)axis, (void)value;
}

static void pointer_axis_relative_direction(void *data,
	struct wl_pointer *pointer, uint32_t axis, uint32_t direction) {
	(void)data, (void)pointer, (void)axis, (void)direction;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
	.axis_relative_direction = pointer_axis_relative_direction,
};

// raw evdev codes need no keymap
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
	uint32_t format, int32_t fd, uint32_t size) {
	(void)data, (void)keyboard, (void)format, (void)size;
	close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
	uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	(void)keyboard, (void)serial, (void)surface;
	struct input *in = data;
	// a modifier may already be down when the overlay takes focus
	uint32_t held = 0;
	uint32_t *k;
	wl_array_for_each(k, keys) {
		held |= mod_bit(*k);
	}
	set_held(in, held);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
	uint32_t serial, struct wl_surface *surface) {
	(void)keyboard, (void)serial, (void)surface;
	struct input *in = data;
	set_held(in, 0);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
	uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	(void)keyboard, (void)serial, (void)time;
	struct input *in = data;
	// wl_keyboard v10 adds a "repeated" state; it is neither of these
	bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	if (!pressed && state != WL_KEYBOARD_KEY_STATE_RELEASED) {
		return;
	}
	uint32_t bit = mod_bit(key);
	if (bit) {
		set_held(in, pressed ? in->held | bit : in->held & ~bit);
	} else if (pressed) {
		in->l->key(in->data, key, mods(in));
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
	uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked,
	uint32_t group) {
	(void)data, (void)keyboard, (void)serial, (void)depressed;
	(void)latched, (void)locked, (void)group;
}

static void keyboard_repeat_info(
	void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
	(void)data, (void)keyboard, (void)rate, (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

struct input *input_create(
	struct conn *c, const struct input_listener *l, void *data) {
	uint32_t caps = c->seat_caps & (WL_SEAT_CAPABILITY_POINTER |
					       WL_SEAT_CAPABILITY_KEYBOARD);
	if (!caps) {
		log_error("the seat has neither a pointer nor a keyboard");
		return NULL;
	}
	struct input *in = calloc(1, sizeof(*in));
	if (!in) {
		goto fail;
	}
	*in = (struct input){.l = l, .data = data};
	if (caps & WL_SEAT_CAPABILITY_POINTER) {
		in->pointer = wl_seat_get_pointer(c->seat);
		if (!in->pointer) {
			goto fail;
		}
		wl_pointer_add_listener(in->pointer, &pointer_listener, in);
		if (c->cursor_shape) {
			in->shape = wp_cursor_shape_manager_v1_get_pointer(
				c->cursor_shape, in->pointer);
		}
	}
	if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		in->keyboard = wl_seat_get_keyboard(c->seat);
		if (!in->keyboard) {
			goto fail;
		}
		wl_keyboard_add_listener(in->keyboard, &keyboard_listener, in);
	}
	return in;

fail:
	log_error("out of memory");
	input_destroy(in);
	return NULL;
}

void input_destroy(struct input *in) {
	if (!in) {
		return;
	}
	// release arrived in wl_keyboard and wl_pointer v3
	if (in->keyboard && wl_keyboard_get_version(in->keyboard) >= 3) {
		wl_keyboard_release(in->keyboard);
	} else if (in->keyboard) {
		wl_keyboard_destroy(in->keyboard);
	}
	if (in->shape) {
		wp_cursor_shape_device_v1_destroy(in->shape);
	}
	if (in->pointer && wl_pointer_get_version(in->pointer) >= 3) {
		wl_pointer_release(in->pointer);
	} else if (in->pointer) {
		wl_pointer_destroy(in->pointer);
	}
	free(in);
}
