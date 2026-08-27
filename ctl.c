/* freakctl — talk to the daemon (or to a running TUI) over the unix socket. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freak.h"

static void usage(void)
{
	fprintf(stderr,
		"freakctl <command> [args]\n"
		"  get\n"
		"  set <field> <0-100>   field: red orange green blue contrast gamma\n"
		"  reset\n"
		"  load <preset>\n"
		"  save <preset>\n"
		"  presets\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 1;
	}

	char cmd[256];
	if (!strcmp(argv[1], "get"))
		snprintf(cmd, sizeof cmd, "GET");
	else if (!strcmp(argv[1], "reset"))
		snprintf(cmd, sizeof cmd, "RESET");
	else if (!strcmp(argv[1], "presets"))
		snprintf(cmd, sizeof cmd, "PRESETS");
	else if (!strcmp(argv[1], "set") && argc >= 4)
		snprintf(cmd, sizeof cmd, "SET %s %s", argv[2], argv[3]);
	else if (!strcmp(argv[1], "load") && argc >= 3)
		snprintf(cmd, sizeof cmd, "LOAD %s", argv[2]);
	else if (!strcmp(argv[1], "save") && argc >= 3)
		snprintf(cmd, sizeof cmd, "SAVE %s", argv[2]);
	else {
		usage();
		return 1;
	}

	char resp[4096];
	if (ipc_send(cmd, resp, sizeof resp) < 0) {
		fprintf(stderr, "error: could not connect to %s\n", SOCKET_PATH);
		return 1;
	}
	printf("%s\n", resp);
	return 0;
}
