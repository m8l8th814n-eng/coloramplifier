/* freak --daemon: holds the gamma controls, serves the socket. One poll loop. */
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "freak.h"

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

void daemon_run(void)
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	struct params params;
	state_load(&params);

	struct gamma_client *gc = gamma_connect();
	if (!gc) {
		fprintf(stderr, "freak: no wayland gamma control "
		                "(WAYLAND_DISPLAY set? compositor supports "
		                "zwlr_gamma_control_manager_v1?)\n");
		exit(1);
	}
	if (!gamma_acquire(gc)) {
		fprintf(stderr, "freak: failed to acquire gamma controls "
		                "(another gamma tool running?)\n");
		exit(1);
	}
	gamma_apply(gc, &params);

	struct ipc_server srv;
	if (ipc_server_start(&srv) < 0) {
		fprintf(stderr, "freak: cannot bind %s\n", SOCKET_PATH);
		exit(1);
	}

	while (running) {
		struct pollfd fds[1 + 1 + MAX_CLIENTS];
		fds[0].fd = gamma_fd(gc);
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		int n = 1 + ipc_server_fill(&srv, fds + 1,
		                            (int)(sizeof fds / sizeof *fds) - 1);

		gamma_flush(gc);
		if (poll(fds, (nfds_t)n, -1) < 0)
			break;

		if (fds[0].revents & POLLIN) {
			if (gamma_dispatch(gc) < 0) {
				fprintf(stderr, "freak: wayland connection lost\n");
				break;
			}
		}

		int changed = 0;
		ipc_server_process(&srv, fds + 1, n - 1, &params, &changed);
		if (changed)
			gamma_apply(gc, &params);
	}

	ipc_server_stop(&srv);
	gamma_destroy(gc);
}
