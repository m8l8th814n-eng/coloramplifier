#define _GNU_SOURCE

/* Line protocol over a unix socket: GET / SET / RESET / LOAD / SAVE / PRESETS */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "freak.h"

static double clamp_pct(double v)
{
	if (v < 0.0) return 0.0;
	if (v > 100.0) return 100.0;
	return v;
}

void params_to_json(const struct params *p, char *buf, size_t n)
{
	snprintf(buf, n,
		"{\"red\":%.1f,\"orange\":%.1f,\"green\":%.1f,"
		"\"blue\":%.1f,\"contrast\":%.1f,\"gamma\":%.1f}",
		p->red, p->orange, p->green, p->blue, p->contrast, p->gamma);
}

static int json_number(const char *s, const char *key, double *out)
{
	char pat[32];
	snprintf(pat, sizeof pat, "\"%s\":", key);
	const char *at = strstr(s, pat);
	if (!at)
		return 0;
	return sscanf(at + strlen(pat), " %lf", out) == 1;
}

int params_from_json(const char *s, struct params *p)
{
	params_default(p);
	if (!json_number(s, "red", &p->red) ||
	    !json_number(s, "green", &p->green) ||
	    !json_number(s, "blue", &p->blue) ||
	    !json_number(s, "contrast", &p->contrast) ||
	    !json_number(s, "gamma", &p->gamma))
		return 0;
	json_number(s, "orange", &p->orange);   /* older presets have no orange */
	return 1;
}

static void presets_json(char *resp, size_t n)
{
	char **names;
	int count = preset_list(&names);
	size_t at = 0;

	at += (size_t)snprintf(resp + at, n - at, "[");
	for (int i = 0; i < count && at < n; i++)
		at += (size_t)snprintf(resp + at, n - at, "%s\"%s\"", i ? "," : "", names[i]);
	if (at < n)
		snprintf(resp + at, n - at, "]");
	preset_list_free(names, count);
}

void ipc_handle(const char *line, struct params *p, int *changed, char *resp, size_t n)
{
	char verb[32], arg[128];
	double value;

	if (sscanf(line, "%31s", verb) != 1) {
		snprintf(resp, n, "ERROR: unknown command");
		return;
	}

	if (!strcmp(verb, "GET")) {
		params_to_json(p, resp, n);
	} else if (!strcmp(verb, "RESET")) {
		params_default(p);
		state_save(p);
		*changed = 1;
		snprintf(resp, n, "OK");
	} else if (!strcmp(verb, "SET")) {
		if (sscanf(line, "%*s %127s %lf", arg, &value) != 2) {
			snprintf(resp, n, "ERROR: usage: SET <field> <0-100>");
			return;
		}
		double *field = params_field(p, arg);
		if (!field) {
			snprintf(resp, n, "ERROR: unknown field '%s'", arg);
			return;
		}
		*field = clamp_pct(value);
		state_save(p);
		*changed = 1;
		snprintf(resp, n, "OK");
	} else if (!strcmp(verb, "LOAD")) {
		struct params loaded;
		if (sscanf(line, "%*s %127s", arg) != 1) {
			snprintf(resp, n, "ERROR: usage: LOAD <preset>");
		} else if (preset_load(arg, &loaded)) {
			*p = loaded;
			state_save(p);
			*changed = 1;
			snprintf(resp, n, "OK");
		} else {
			snprintf(resp, n, "ERROR: preset '%s' not found", arg);
		}
	} else if (!strcmp(verb, "SAVE")) {
		if (sscanf(line, "%*s %127s", arg) != 1) {
			snprintf(resp, n, "ERROR: usage: SAVE <preset>");
			return;
		}
		int err = preset_save(arg, p);
		if (err)
			snprintf(resp, n, "ERROR: %s", strerror(err));
		else
			snprintf(resp, n, "OK");
	} else if (!strcmp(verb, "PRESETS")) {
		presets_json(resp, n);
	} else {
		snprintf(resp, n, "ERROR: unknown command");
	}
}

/* ---------- client ---------- */

static int connect_socket(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCKET_PATH);

	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int ipc_send(const char *cmd, char *resp, size_t n)
{
	int fd = connect_socket();
	if (fd < 0)
		return -1;

	char out[256];
	int len = snprintf(out, sizeof out, "%s\n", cmd);
	if (write(fd, out, (size_t)len) != len) {
		close(fd);
		return -1;
	}

	size_t at = 0;
	while (at + 1 < n) {
		ssize_t got = read(fd, resp + at, 1);
		if (got <= 0)
			break;
		if (resp[at] == '\n')
			break;
		at++;
	}
	resp[at] = '\0';
	close(fd);
	return 0;
}

/* ---------- server ---------- */

int ipc_server_start(struct ipc_server *s)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		s->clients[i].fd = -1;
		s->clients[i].len = 0;
	}

	s->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s->listen_fd < 0)
		return -1;

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCKET_PATH);
	unlink(SOCKET_PATH);

	if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
	    listen(s->listen_fd, 8) < 0) {
		close(s->listen_fd);
		s->listen_fd = -1;
		return -1;
	}
	return 0;
}

int ipc_server_fill(struct ipc_server *s, struct pollfd *fds, int max)
{
	int n = 0;
	if (s->listen_fd < 0 || max < 1)
		return 0;

	fds[n].fd = s->listen_fd;
	fds[n].events = POLLIN;
	fds[n].revents = 0;
	n++;

	for (int i = 0; i < MAX_CLIENTS && n < max; i++) {
		if (s->clients[i].fd < 0)
			continue;
		fds[n].fd = s->clients[i].fd;
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;
	}
	return n;
}

static void client_close(struct ipc_client *c)
{
	close(c->fd);
	c->fd = -1;
	c->len = 0;
}

static void client_read(struct ipc_client *c, struct params *p, int *changed)
{
	ssize_t got = read(c->fd, c->buf + c->len, sizeof c->buf - c->len - 1);
	if (got <= 0) {
		client_close(c);
		return;
	}
	c->len += (size_t)got;
	c->buf[c->len] = '\0';

	char *start = c->buf;
	char *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = '\0';

		char resp[1024];
		resp[0] = '\0';
		ipc_handle(start, p, changed, resp, sizeof resp);

		char out[1100];
		int len = snprintf(out, sizeof out, "%s\n", resp);
		if (write(c->fd, out, (size_t)len) != len) {
			client_close(c);
			return;
		}
		start = nl + 1;
	}

	/* keep whatever is left of a partial line */
	c->len = strlen(start);
	if (c->len >= sizeof c->buf - 1) {   /* overlong garbage, drop it */
		c->len = 0;
		return;
	}
	memmove(c->buf, start, c->len + 1);
}

void ipc_server_process(struct ipc_server *s, struct pollfd *fds, int n,
                        struct params *p, int *changed)
{
	int k = 1;
	for (int i = 0; i < MAX_CLIENTS && k < n; i++) {
		if (s->clients[i].fd < 0)
			continue;
		struct pollfd *pf = &fds[k++];
		if (pf->revents & (POLLIN | POLLHUP | POLLERR))
			client_read(&s->clients[i], p, changed);
	}

	if (n > 0 && (fds[0].revents & POLLIN)) {
		int fd = accept4(s->listen_fd, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0)
			return;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (s->clients[i].fd < 0) {
				s->clients[i].fd = fd;
				s->clients[i].len = 0;
				return;
			}
		}
		close(fd);   /* all slots busy */
	}
}

void ipc_server_stop(struct ipc_server *s)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0)
			client_close(&s->clients[i]);
	if (s->listen_fd >= 0) {
		close(s->listen_fd);
		s->listen_fd = -1;
		unlink(SOCKET_PATH);
	}
}
