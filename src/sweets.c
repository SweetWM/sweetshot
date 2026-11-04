#include "sweets.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct scan {
	const char *p;
	const char *end;
	bool bad;
};

static void ws(struct scan *s) {
	while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' ||
					*s->p == '\r' || *s->p == '\n')) {
		s->p++;
	}
}

static bool eat(struct scan *s, char c) {
	ws(s);
	if (s->p < s->end && *s->p == c) {
		s->p++;
		return true;
	}
	return false;
}

static bool word(struct scan *s, const char *w) {
	ws(s);
	size_t n = strlen(w);
	if ((size_t)(s->end - s->p) >= n && memcmp(s->p, w, n) == 0) {
		s->p += n;
		return true;
	}
	return false;
}

static bool string(struct scan *s, char *out, size_t cap) {
	size_t n = 0;
	if (!eat(s, '"')) {
		goto bad;
	}
	while (s->p < s->end && *s->p != '"') {
		char c = *s->p++;
		if (c == '\\') {
			if (s->p == s->end) {
				goto bad;
			}
			c = *s->p++;
			if (c == 'u') {
				if (out || s->end - s->p < 4) {
					goto bad;
				}
				s->p += 4;
				continue;
			}
			if (c == 'n') {
				c = '\n';
			} else if (c == 't') {
				c = '\t';
			} else if (c == 'r') {
				c = '\r';
			}
		}
		if (out) {
			if (n + 1 >= cap) {
				goto bad;
			}
			out[n++] = c;
		}
	}
	if (s->p == s->end) {
		goto bad;
	}
	s->p++;
	if (out) {
		out[n] = '\0';
	}
	return true;

bad:
	s->bad = true;
	return false;
}

static void skip(struct scan *s, int depth) {
	ws(s);
	if (s->bad || s->p == s->end || depth > 32) {
		s->bad = true;
		return;
	}
	char open = *s->p;
	if (open == '"') {
		string(s, NULL, 0);
		return;
	}
	if (open != '{' && open != '[') {
		// number, true, false or null
		const char *start = s->p;
		while (s->p < s->end && *s->p &&
			strchr("+-.eEtruflasn0123456789", *s->p)) {
			s->p++;
		}
		s->bad = s->p == start;
		return;
	}
	char close = open == '{' ? '}' : ']';
	s->p++;
	if (eat(s, close)) {
		return;
	}
	do {
		if (open == '{' && !(string(s, NULL, 0) && eat(s, ':'))) {
			s->bad = true;
			return;
		}
		skip(s, depth + 1);
	} while (!s->bad && eat(s, ','));
	s->bad |= !eat(s, close);
}

// the next key of an object whose '{' was taken, false at its end
static bool next_key(struct scan *s, bool *first, char *key, size_t cap) {
	if (s->bad) {
		return false;
	}
	if (*first) {
		*first = false;
		if (eat(s, '}')) {
			return false;
		}
	} else if (!eat(s, ',')) {
		s->bad |= !eat(s, '}');
		return false;
	}
	if (!string(s, key, cap) || !eat(s, ':')) {
		s->bad = true;
		return false;
	}
	return true;
}

static bool object(struct scan *s) {
	s->bad |= !eat(s, '{');
	return !s->bad;
}

static void nullable(struct scan *s, char *out, size_t cap) {
	out[0] = '\0';
	if (!word(s, "null")) {
		string(s, out, cap);
	}
}

static void windows(struct scan *s, struct focus *f) {
	if (!eat(s, '[')) {
		s->bad = true;
		return;
	}
	if (eat(s, ']')) {
		return;
	}
	do {
		struct focus w = {0};
		bool active = false;
		bool first = true;
		char key[64];
		if (!object(s)) {
			return;
		}
		while (next_key(s, &first, key, sizeof(key))) {
			if (strcmp(key, "active") == 0) {
				active = word(s, "true");
				s->bad |= !active && !word(s, "false");
			} else if (strcmp(key, "toplevel_identifier") == 0) {
				nullable(s, w.identifier, sizeof(w.identifier));
			} else if (strcmp(key, "output") == 0) {
				nullable(s, w.output, sizeof(w.output));
			} else {
				skip(s, 0);
			}
		}
		if (active && !s->bad) {
			*f = w;
			f->found = true;
		}
	} while (!s->bad && eat(s, ','));
	s->bad |= !eat(s, ']');
}

bool sweets_parse_focus(const char *json, size_t len, struct focus *f) {
	*f = (struct focus){0};
	struct scan s = {.p = json, .end = json + len};
	char status[16] = "";
	bool data = false;
	bool first = true;
	char key[64];
	if (object(&s)) {
		while (next_key(&s, &first, key, sizeof(key))) {
			if (strcmp(key, "outcome") != 0) {
				skip(&s, 0);
				continue;
			}
			bool first2 = true;
			if (!object(&s)) {
				break;
			}
			while (next_key(&s, &first2, key, sizeof(key))) {
				if (strcmp(key, "status") == 0) {
					string(&s, status, sizeof(status));
				} else if (strcmp(key, "value") != 0) {
					skip(&s, 0);
				} else if (object(&s)) {
					bool first3 = true;
					while (next_key(&s, &first3, key,
						sizeof(key))) {
						if (strcmp(key, "data") == 0) {
							windows(&s, f);
							data = true;
						} else {
							skip(&s, 0);
						}
					}
				}
			}
		}
	}
	if (s.bad || strcmp(status, "success") != 0 || !data) {
		log_error("cannot read the Sweets IPC windows reply");
		return false;
	}
	return true;
}

// a Sweets reply frame is at most 256 KiB, anything longer fails to parse
enum { REPLY_MAX = 512 * 1024 };

bool sweets_focus(struct focus *f) {
	bool ok = false;
	char *buf = malloc(REPLY_MAX);
	int fds[2] = {-1, -1};
	if (!buf || pipe2(fds, O_CLOEXEC) < 0) {
		log_error("cannot ask sweets for the focused window: %s",
			buf ? strerror(errno) : "out of memory");
		free(buf);
		return false;
	}
	extern char **environ;
	pid_t pid;
	char *argv[] = {"sweets", "msg", "--json", "windows", NULL};
	// an inherited SIG_IGN would reap the child before waitpid sees it
	signal(SIGCHLD, SIG_DFL);
	posix_spawn_file_actions_t fa;
	int err = posix_spawn_file_actions_init(&fa);
	if (err == 0) {
		// without the dup2 the reply would land on our stdout
		err = posix_spawn_file_actions_adddup2(
			&fa, fds[1], STDOUT_FILENO);
		if (err == 0) {
			err = posix_spawnp(
				&pid, "sweets", &fa, NULL, argv, environ);
		}
		posix_spawn_file_actions_destroy(&fa);
	}
	close(fds[1]);
	if (err) {
		log_error("cannot run sweets to find the focused window: %s",
			strerror(err));
		goto out;
	}

	size_t len = 0;
	while (len < REPLY_MAX) {
		ssize_t n = read(fds[0], buf + len, REPLY_MAX - len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		len += (size_t)n;
	}
	// closed before waiting: a reply that overflows buf ends in EPIPE,
	// not in a writer blocked forever
	close(fds[0]);
	fds[0] = -1;
	int status;
	pid_t waited;
	while ((waited = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
	}
	// sweets msg reports its own errors on stderr
	if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		log_error("sweets msg windows failed, cannot find the focused "
			  "window");
		goto out;
	}
	ok = sweets_parse_focus(buf, len, f);

out:
	if (fds[0] >= 0) {
		close(fds[0]);
	}
	free(buf);
	return ok;
}
