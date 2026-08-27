/* freak — Wayland gamma control. Shared declarations. */
#ifndef FREAK_H
#define FREAK_H

#include <stddef.h>
#include <stdint.h>

#define SOCKET_PATH "/tmp/controller.sucker"
#define MAX_CLIENTS 8

/* ---------- params ---------- */

struct params {
	double red, orange, green, blue, contrast, gamma;
};

void   params_default(struct params *p);
double pct_to_val(double pct);           /* 0..100 -> -3.0..+2.0, neutral at 80 */
double *params_field(struct params *p, const char *name);  /* NULL if unknown */

/* ---------- preset.c ---------- */

int  preset_load(const char *name, struct params *out);   /* 1 ok, 0 missing */
int  preset_save(const char *name, const struct params *p); /* 0 ok, else errno */
int  preset_delete(const char *name);
int  preset_list(char ***names);          /* count, sorted; free with preset_list_free */
void preset_list_free(char **names, int n);
void state_load(struct params *p);
void state_save(const struct params *p);

/* ---------- gamma.c ---------- */

struct gamma_client;

struct gamma_client *gamma_connect(void);
int  gamma_acquire(struct gamma_client *g);   /* 1 if at least one output */
void gamma_apply(struct gamma_client *g, const struct params *p);
int  gamma_fd(struct gamma_client *g);
int  gamma_dispatch(struct gamma_client *g);  /* 0 ok, -1 connection lost */
void gamma_flush(struct gamma_client *g);
void gamma_destroy(struct gamma_client *g);

/* ---------- ipc.c ---------- */

void params_to_json(const struct params *p, char *buf, size_t n);
int  params_from_json(const char *s, struct params *p);

/* Execute one protocol line against *p. Writes the reply into resp.
   Sets *changed when the gamma ramps need reapplying. */
void ipc_handle(const char *line, struct params *p, int *changed, char *resp, size_t n);

/* Client side: one request, one reply line. 0 ok, -1 no daemon. */
int  ipc_send(const char *cmd, char *resp, size_t n);

/* Server side: listener plus a handful of line-buffered clients. */
struct ipc_client {
	int fd;
	size_t len;
	char buf[512];
};

struct ipc_server {
	int listen_fd;
	struct ipc_client clients[MAX_CLIENTS];
};

struct pollfd;

int  ipc_server_start(struct ipc_server *s);   /* 0 ok, -1 socket in use */
int  ipc_server_fill(struct ipc_server *s, struct pollfd *fds, int max);
void ipc_server_process(struct ipc_server *s, struct pollfd *fds, int n,
                        struct params *p, int *changed);
void ipc_server_stop(struct ipc_server *s);

/* ---------- sway.c ---------- */

void sway_set_floating(void);

/* ---------- entry points ---------- */

void daemon_run(void);
void tui_run(void);

#endif
