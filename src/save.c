#include "save.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char *join(const char *dir, const char *name) {
	size_t n = strlen(dir);
	const char *sep = n && dir[n - 1] == '/' ? "" : "/";
	char *p = NULL;
	if (asprintf(&p, "%s%s%s", dir, sep, name) < 0) {
		log_error("out of memory");
		return NULL;
	}
	return p;
}

static bool absolute(const char *p) {
	return p && p[0] == '/';
}

static char *pictures_from_user_dirs(const char *home) {
	const char *config = getenv("XDG_CONFIG_HOME");
	char *path = absolute(config) ? join(config, "user-dirs.dirs")
				      : join(home, ".config/user-dirs.dirs");
	if (!path) {
		return NULL;
	}
	FILE *f = fopen(path, "re");
	free(path);
	if (!f) {
		return NULL; // missing or unreadable: the default
	}

	static const char key[] = "XDG_PICTURES_DIR=\"";
	char *found = NULL;
	char *line = NULL;
	size_t cap = 0;
	while (!found && getline(&line, &cap, f) > 0) {
		char *s = line;
		while (*s == ' ' || *s == '\t') {
			s++;
		}
		char *end = s + strlen(s);
		while (end > s && strchr(" \t\r\n", end[-1])) {
			end--;
		}
		*end = '\0';
		size_t len = (size_t)(end - s);
		if (len < sizeof(key) ||
			strncmp(s, key, sizeof(key) - 1) != 0 ||
			end[-1] != '"') {
			continue;
		}
		end[-1] = '\0';
		const char *v = s + sizeof(key) - 1;
		if (strcmp(v, "$HOME") == 0) {
			found = strdup(home);
		} else if (strncmp(v, "$HOME/", 6) == 0) {
			found = join(home, v + 6);
		} else if (absolute(v)) {
			found = strdup(v);
		}
	}
	free(line);
	fclose(f);
	return found;
}

char *save_default_dir(void) {
	const char *home = getenv("HOME");
	if (!absolute(home)) {
		log_error("HOME is not set to an absolute path; pass --dir");
		return NULL;
	}
	const char *env = getenv("XDG_PICTURES_DIR");
	if (absolute(env)) {
		return join(env, "Screenshots");
	}
	char *pictures = pictures_from_user_dirs(home);
	char *dir = pictures ? join(pictures, "Screenshots")
			     : join(home, "Pictures/Screenshots");
	free(pictures);
	return dir;
}

// mkdir -p; existing components are fine, the final open checks the type
static int make_dirs(const char *dir) {
	char *p = strdup(dir);
	if (!p) {
		errno = ENOMEM;
		return -1;
	}
	for (char *s = p + 1;; s++) {
		if (*s == '/' || *s == '\0') {
			char c = *s;
			*s = '\0';
			if (mkdir(p, 0777) < 0 && errno != EEXIST) {
				free(p);
				return -1;
			}
			*s = c;
			if (c == '\0') {
				break;
			}
		}
	}
	free(p);
	return 0;
}

static int write_all(int fd, const uint8_t *data, size_t len) {
	while (len > 0) {
		ssize_t n = write(fd, data, len);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

char *save_png(const char *dir, const uint8_t *data, size_t len) {
	if (make_dirs(dir) < 0) {
		log_error("cannot create %s: %s", dir, strerror(errno));
		return NULL;
	}
	int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0) {
		log_error("cannot open %s: %s", dir, strerror(errno));
		return NULL;
	}

	char *path = NULL;
	int fd = openat(dirfd, ".", O_TMPFILE | O_WRONLY | O_CLOEXEC, 0600);
	if (fd < 0 || write_all(fd, data, len) < 0 || fsync(fd) < 0) {
		goto fail;
	}

	time_t now = time(NULL);
	struct tm tm;
	if (!localtime_r(&now, &tm)) {
		goto fail;
	}
	char proc[32];
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	char name[64];
	for (int tries = 0; tries < 8; tries++) {
		uint16_t r;
		if (getrandom(&r, sizeof(r), 0) != sizeof(r)) {
			goto fail;
		}
		snprintf(name, sizeof(name),
			"sweetshot-%04d%02d%02d-%02d%02d%02d-%04x.png",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, r);
		path = join(dir, name);
		if (!path) {
			goto out; // reported
		}
		// AT_EMPTY_PATH would need CAP_DAC_READ_SEARCH
		if (linkat(AT_FDCWD, proc, dirfd, name, AT_SYMLINK_FOLLOW) ==
			0) {
			close(fd);
			close(dirfd);
			return path;
		}
		if (errno != EEXIST) {
			goto fail;
		}
		free(path);
		path = NULL;
	}

fail:
	log_error("cannot save screenshot in %s: %s", dir, strerror(errno));
out:
	free(path);
	if (fd >= 0) {
		close(fd);
	}
	close(dirfd);
	return NULL;
}
