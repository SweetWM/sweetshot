#ifndef SWEETSHOT_LOG_H
#define SWEETSHOT_LOG_H

// one line on stderr, prefixed "sweetshot: ", newline added
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
