#ifndef SWEETSHOT_SWEETS_H
#define SWEETSHOT_SWEETS_H

#include <stdbool.h>
#include <stddef.h>

// the active window of a Sweets IPC windows reply. Strings are "" when
// the reply has null, Sweets bounds them to 512 bytes
struct focus {
	bool found;
	char identifier[513]; // ext_foreign_toplevel_handle_v1.identifier
	char output[513];
};

// fills f from "sweets msg --json windows". Reports the error and returns
// false when Sweets cannot be asked
bool sweets_focus(struct focus *f);

// the parsing half of sweets_focus, for a reply of len bytes
bool sweets_parse_focus(const char *json, size_t len, struct focus *f);

#endif
