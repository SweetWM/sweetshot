#ifndef SWEETSHOT_WL_H
#define SWEETSHOT_WL_H

#include <stdbool.h>
#include <wayland-client.h>

struct output {
	struct wl_list link; // conn.outputs
	struct wl_output *wl;
	char *name; // wl_output v4 name, e.g. "DP-4"; NULL until sent
};

// one Wayland connection with every global sweetshot binds. Fields stay
// NULL when the compositor does not offer the global
struct conn {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct wl_list outputs; // struct output.link, in advertised order
	struct wl_seat *seat;	// the first one, sweetshot never picks seats
	struct ext_image_copy_capture_manager_v1 *capture;
	struct ext_output_image_capture_source_manager_v1 *output_source;
	struct ext_foreign_toplevel_image_capture_source_manager_v1
		*toplevel_source;
	struct ext_data_control_manager_v1 *data_control;
	bool oom; // allocation failed inside a listener
};

bool conn_init(struct conn *c);
void conn_finish(struct conn *c);

bool conn_require(
	const struct conn *c, const struct wl_interface *const *needs);

#endif
