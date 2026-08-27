/* freak — TUI by default, --daemon to run headless. */
#include <stdio.h>
#include <string.h>

#include "freak.h"

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--daemon")) {
			daemon_run();
			return 0;
		}
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			printf("usage: freak [--daemon]\n"
			       "  (no args)  interactive TUI\n"
			       "  --daemon   hold gamma and serve %s\n", SOCKET_PATH);
			return 0;
		}
	}
	tui_run();
	return 0;
}
