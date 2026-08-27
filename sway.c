/* Ask sway/i3 to float the TUI window, if we are running under one. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "freak.h"

static void ipc_command(const char *sock_path, const char *cmd)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return;
	}

	uint32_t len = (uint32_t)strlen(cmd);
	uint32_t type = 0;                 /* RUN_COMMAND */
	char msg[256];
	size_t at = 0;

	memcpy(msg + at, "i3-ipc", 6);     at += 6;
	memcpy(msg + at, &len, 4);         at += 4;
	memcpy(msg + at, &type, 4);        at += 4;
	if (len < sizeof msg - at) {
		memcpy(msg + at, cmd, len);
		at += len;
		if (write(fd, msg, at) > 0) {
			char header[14];
			(void)!read(fd, header, sizeof header);
		}
	}
	close(fd);
}

void sway_set_floating(void)
{
	const char *sock = getenv("SWAYSOCK");
	if (!sock || !*sock)
		return;
	ipc_command(sock, "floating enable");
	ipc_command(sock, "sticky enable");
	ipc_command(sock, "focus");
}
