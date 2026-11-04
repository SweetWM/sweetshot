#include "clip.h"

#include "ext-data-control-v1-client-protocol.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char mime[] = "image/png";

static void source_send(void *data, struct ext_data_control_source_v1 *source,
	const char *mime_type, int32_t fd) {
	(void)source;
	struct clip *cl = data;
	int flags = fcntl(fd, F_GETFL);
	if (strcmp(mime_type, mime) != 0 || cl->n_transfers == CLIP_TRANSFERS ||
		flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(fd);
		return;
	}
	cl->transfers[cl->n_transfers++] = (struct transfer){.fd = fd};
}

static bool write_some(const struct clip *cl, struct transfer *t) {
	while (t->done < cl->len) {
		ssize_t n = write(t->fd, cl->png + t->done, cl->len - t->done);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0 && errno == EAGAIN) {
			return true;
		}
		if (n <= 0) {
			break; // EPIPE and the rest end this transfer only
		}
		t->done += (size_t)n;
	}
	close(t->fd);
	return false;
}

static void source_cancelled(
	void *data, struct ext_data_control_source_v1 *source) {
	(void)source;
	struct clip *cl = data;
	cl->cancelled = true;
}

static const struct ext_data_control_source_v1_listener source_listener = {
	.send = source_send,
	.cancelled = source_cancelled,
};

static void device_data_offer(void *data,
	struct ext_data_control_device_v1 *device,
	struct ext_data_control_offer_v1 *offer) {
	(void)data, (void)device, (void)offer;
}

static void device_selection(void *data,
	struct ext_data_control_device_v1 *device,
	struct ext_data_control_offer_v1 *offer) {
	(void)data, (void)device;
	if (offer) {
		ext_data_control_offer_v1_destroy(offer);
	}
}

static void device_finished(
	void *data, struct ext_data_control_device_v1 *device) {
	(void)device;
	struct clip *cl = data;
	cl->cancelled = true;
}

static const struct ext_data_control_device_v1_listener device_listener = {
	.data_offer = device_data_offer,
	.selection = device_selection,
	.finished = device_finished,
	.primary_selection = device_selection,
};

bool clip_set(struct clip *cl, struct conn *c, const uint8_t *png, size_t len) {
	*cl = (struct clip){.conn = c, .png = png, .len = len};
	cl->device = ext_data_control_manager_v1_get_data_device(
		c->data_control, c->seat);
	cl->source =
		ext_data_control_manager_v1_create_data_source(c->data_control);
	if (!cl->device || !cl->source) {
		log_error("out of memory");
		goto fail;
	}
	ext_data_control_device_v1_add_listener(
		cl->device, &device_listener, cl);
	ext_data_control_source_v1_add_listener(
		cl->source, &source_listener, cl);
	ext_data_control_source_v1_offer(cl->source, mime);
	ext_data_control_device_v1_set_selection(cl->device, cl->source);
	if (wl_display_roundtrip(c->display) < 0) {
		log_error("lost the Wayland connection");
		goto fail;
	}
	return true;

fail:
	clip_finish(cl);
	return false;
}

static void detach(void) {
	setsid();
	if (chdir("/") < 0) {
		_exit(1);
	}
	int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		_exit(1);
	}
	for (int i = 0; i < 3; i++) {
		dup2(fd, i);
	}
	if (fd > 2) {
		close(fd);
	}
	// a receiver closing early must end one transfer, not the child
	signal(SIGPIPE, SIG_IGN);
}

static void serve(struct clip *cl) {
	struct wl_display *d = cl->conn->display;
	struct pollfd fds[1 + CLIP_TRANSFERS];
	for (;;) {
		if (wl_display_dispatch_pending(d) < 0 || cl->cancelled) {
			return;
		}
		// fails while events are queued, which the top dispatches
		if (wl_display_prepare_read(d) != 0) {
			continue;
		}
		// a full socket waits for POLLOUT, the rest goes out next time
		int flushed = wl_display_flush(d);
		if (flushed < 0 && errno != EAGAIN) {
			wl_display_cancel_read(d);
			return;
		}
		fds[0] = (struct pollfd){
			.fd = wl_display_get_fd(d),
			.events = flushed < 0 ? POLLIN | POLLOUT : POLLIN,
		};
		size_t n = cl->n_transfers;
		for (size_t i = 0; i < n; i++) {
			fds[1 + i] = (struct pollfd){
				.fd = cl->transfers[i].fd,
				.events = POLLOUT,
			};
		}
		if (poll(fds, 1 + n, -1) < 0) {
			wl_display_cancel_read(d);
			if (errno == EINTR) {
				continue;
			}
			return;
		}
		if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(d) < 0) {
				return;
			}
		} else {
			wl_display_cancel_read(d);
		}
		// backwards: the last transfer takes a finished one's place
		for (size_t i = n; i > 0; i--) {
			struct transfer *t = &cl->transfers[i - 1];
			if (fds[i].revents && !write_some(cl, t)) {
				*t = cl->transfers[--cl->n_transfers];
			}
		}
	}
}

bool clip_serve(struct clip *cl) {
	if (cl->cancelled) {
		return true; // replaced already, nothing left to serve
	}
	// unflushed stdout would otherwise be written twice
	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0) {
		log_error("cannot fork the clipboard process: %s",
			strerror(errno));
		return false;
	}
	if (pid > 0) {
		return true;
	}
	detach();
	serve(cl);
	_exit(0);
}

void clip_finish(struct clip *cl) {
	for (size_t i = 0; i < cl->n_transfers; i++) {
		close(cl->transfers[i].fd);
	}
	cl->n_transfers = 0;
	if (cl->source) {
		ext_data_control_source_v1_destroy(cl->source);
	}
	if (cl->device) {
		ext_data_control_device_v1_destroy(cl->device);
	}
	cl->source = NULL;
	cl->device = NULL;
}
