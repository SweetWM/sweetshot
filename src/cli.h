#ifndef SWEETSHOT_CLI_H
#define SWEETSHOT_CLI_H

#include <stdbool.h>

enum command {
	CMD_REGION,
	CMD_OUTPUT,
	CMD_WINDOW,
};

struct args {
	enum command cmd;
	const char *name; // output NAME, or NULL
	const char *dir;  // absolute --dir, or NULL
	bool clip;
	bool file;
	bool cursor;
	bool edit;
};

extern const char cli_usage[];

bool cli_parse(int argc, char **argv, struct args *a);

#endif
