#include <stdio.h>
#include <string.h>

static const char usage[] =
	"usage: sweetshot region [--clip] [--file] [--dir PATH] [--cursor]\n"
	"       sweetshot output [NAME] [--clip] [--file] [--dir PATH] "
	"[--cursor]\n"
	"       sweetshot window [--clip] [--file] [--dir PATH] [--cursor]\n";

int main(int argc, char **argv) {
	if (argc < 2) {
		fputs(usage, stderr);
		return 2;
	}

	const char *cmd = argv[1];
	if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		fputs(usage, stdout);
		return 0;
	}
	if (strcmp(cmd, "region") == 0 || strcmp(cmd, "output") == 0 ||
		strcmp(cmd, "window") == 0) {
		fprintf(stderr, "sweetshot: %s: not implemented yet\n", cmd);
		return 1;
	}

	fprintf(stderr, "sweetshot: unknown command \"%s\"\n", cmd);
	fputs(usage, stderr);
	return 2;
}
