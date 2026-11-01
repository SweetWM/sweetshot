#include "cli.h"

#include "log.h"

#include <stdio.h>
#include <string.h>

const char cli_usage[] =
	"usage: sweetshot region [--clip] [--file] [--dir PATH] [--cursor]\n"
	"       sweetshot output [NAME] [--clip] [--file] [--dir PATH] "
	"[--cursor]\n"
	"       sweetshot window [--clip] [--file] [--dir PATH] [--cursor]\n";

bool cli_parse(int argc, char **argv, struct args *a) {
	*a = (struct args){0};
	const char *cmd = argc > 1 ? argv[1] : "";
	if (strcmp(cmd, "region") == 0) {
		a->cmd = CMD_REGION;
	} else if (strcmp(cmd, "output") == 0) {
		a->cmd = CMD_OUTPUT;
	} else if (strcmp(cmd, "window") == 0) {
		a->cmd = CMD_WINDOW;
	} else {
		if (argc > 1) {
			log_error("unknown command \"%s\"", cmd);
		}
		fputs(cli_usage, stderr);
		return false;
	}

	for (int i = 2; i < argc; i++) {
		const char *arg = argv[i];
		if (strcmp(arg, "--clip") == 0) {
			a->clip = true;
		} else if (strcmp(arg, "--file") == 0) {
			a->file = true;
		} else if (strcmp(arg, "--cursor") == 0) {
			a->cursor = true;
		} else if (strcmp(arg, "--dir") == 0) {
			if (++i == argc) {
				log_error("--dir needs a value");
				return false;
			}
			// files go only where the user said; no cwd guessing
			if (argv[i][0] != '/') {
				log_error("--dir must be an absolute path, not "
					  "\"%s\"",
					argv[i]);
				return false;
			}
			a->dir = argv[i];
		} else if (arg[0] == '-') {
			log_error("unknown option \"%s\"", arg);
			return false;
		} else if (a->cmd == CMD_OUTPUT && !a->name) {
			a->name = arg;
		} else {
			log_error("unexpected argument \"%s\"", arg);
			return false;
		}
	}
	if (!a->clip && !a->file) {
		a->clip = true;
		a->file = true;
	}
	return true;
}
