#include "clip.h"

#include "ext-data-control-v1-client-protocol.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char mime[] = "image/png";

static void source_send(void *data, struct ext_data_control_source_v1 *source,
	const char *mime_type, int32_t fd) {
	(void)source;
	struct clip *cl = data;
	if (strcmp(mime_type, mime) != 0) {
		close(fd);
		return;
	}
	// receivers may hand over a non-blocking pipe
	int flags = fcntl(fd, F_GETFL);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	}
	const uint8_t *p = cl->png;
	size_t rest = cl->len;
	while (rest > 0) {
		ssize_t n = write(fd, p, rest);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break; // EPIPE and the rest end this transfer only
		}
		p += n;
		rest -= (size_t)n;
	}
	close(fd);
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

// a long-lived child sees every later copy, so offers are destroyed on
// arrival instead of piling up
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
	while (!cl->cancelled) {
		if (wl_display_dispatch(cl->conn->display) < 0) {
			break;
		}
	}
	_exit(0);
}

void clip_finish(struct clip *cl) {
	if (cl->source) {
		ext_data_control_source_v1_destroy(cl->source);
	}
	if (cl->device) {
		ext_data_control_device_v1_destroy(cl->device);
	}
	cl->source = NULL;
	cl->device = NULL;
}
