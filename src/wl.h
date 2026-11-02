#ifndef SWEETSHOT_WL_H
#define SWEETSHOT_WL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct output {
	struct wl_list link; // conn.outputs
	struct wl_output *wl;
	char *name; // wl_output v4 name, e.g. "DP-4"; NULL until sent
	// current mode in physical pixels, not rotated; 0 until sent
	int32_t mode_w;
	int32_t mode_h;
	int32_t transform; // wl_output.transform
};

// one Wayland connection with every global sweetshot binds. Fields stay
// NULL when the compositor does not offer the global
struct conn {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct wl_list outputs; // struct output.link, in advertised order
	struct wl_seat *seat;	// the first one, sweetshot never picks seats
	uint32_t seat_caps;	// wl_seat.capabilities of seat
	struct ext_image_copy_capture_manager_v1 *capture;
	struct ext_output_image_capture_source_manager_v1 *output_source;
	struct ext_foreign_toplevel_image_capture_source_manager_v1
		*toplevel_source;
	struct ext_data_control_manager_v1 *data_control;
	uint32_t toplevel_list_name; // ext_foreign_toplevel_list_v1, unbound
	struct wl_compositor *compositor; // v4
	struct wl_subcompositor *subcompositor;
	struct wp_viewporter *viewporter;
	struct wp_single_pixel_buffer_manager_v1 *single_pixel;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_cursor_shape_manager_v1 *cursor_shape; // optional
	bool oom; // allocation failed inside a listener
};

bool conn_init(struct conn *c);
void conn_finish(struct conn *c);

bool conn_require(
	const struct conn *c, const struct wl_interface *const *needs);

#endif
