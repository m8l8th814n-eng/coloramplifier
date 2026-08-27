/* zwlr_gamma_control_v1 client: one gamma control per output.
 *
 * The compositor drops the gamma table as soon as the client that set it
 * disconnects, so whoever holds these controls has to stay alive. That is
 * the whole reason freak has a daemon.
 */
#define _GNU_SOURCE

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "freak.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

#define MAX_OUTPUTS 16

struct output_ctrl {
	struct zwlr_gamma_control_v1 *ctrl;
	uint32_t size;
	int failed;
};

struct gamma_client {
	struct wl_display *dpy;
	struct wl_registry *registry;
	struct zwlr_gamma_control_manager_v1 *manager;

	struct wl_output *outputs[MAX_OUTPUTS];
	int n_outputs;

	struct output_ctrl ctrls[MAX_OUTPUTS];
	int n_ctrls;

	/* filled in by the events of the control we are currently setting up */
	uint32_t pending_size;
	int pending_have_size;
	int pending_failed;
};

/* ---------- ramp maths (identical to the Rust version) ---------- */

static double clamp01(double v)
{
	if (v < 0.0) return 0.0;
	if (v > 1.0) return 1.0;
	return v;
}

static void fill_ramp(uint16_t *ramp, size_t n, const struct params *p)
{
	double warmth = pct_to_val(p->orange) - 1.0;
	double gain[3] = {
		pct_to_val(p->red)   + warmth * 0.70,
		pct_to_val(p->green) + warmth * 0.45,
		pct_to_val(p->blue),
	};
	double contrast = pct_to_val(p->contrast);
	double gamma = pct_to_val(p->gamma);
	if (gamma < 0.01)
		gamma = 0.01;

	for (size_t i = 0; i < n; i++) {
		double x = (n == 1) ? 1.0 : (double)i / (double)(n - 1);
		for (int ch = 0; ch < 3; ch++) {
			double y = (x - 0.5) * contrast + 0.5;
			y = clamp01(y);
			y = pow(y, 1.0 / gamma);
			y *= gain[ch];
			ramp[(size_t)ch * n + i] = (uint16_t)(clamp01(y) * 65535.0 + 0.5);
		}
	}
}

/* wlroots read()s the whole table out of this fd, so it must be exactly
   ramp_size * 3 * sizeof(uint16_t) bytes and positioned at zero. */
static int build_ramp_fd(uint32_t size, const struct params *p)
{
	size_t n = size;
	size_t bytes = n * 3 * sizeof(uint16_t);

	int fd = memfd_create("freak-gamma", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)bytes) < 0) {
		close(fd);
		return -1;
	}

	void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return -1;
	}
	fill_ramp(map, n, p);
	munmap(map, bytes);
	return fd;
}

/* ---------- wayland plumbing ---------- */

static void gc_gamma_size(void *data, struct zwlr_gamma_control_v1 *ctrl, uint32_t size)
{
	struct gamma_client *g = data;
	(void)ctrl;
	g->pending_size = size;
	g->pending_have_size = 1;
}

static void gc_failed(void *data, struct zwlr_gamma_control_v1 *ctrl)
{
	struct gamma_client *g = data;
	g->pending_failed = 1;
	for (int i = 0; i < g->n_ctrls; i++)
		if (g->ctrls[i].ctrl == ctrl)
			g->ctrls[i].failed = 1;
}

static const struct zwlr_gamma_control_v1_listener gamma_listener = {
	.gamma_size = gc_gamma_size,
	.failed = gc_failed,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
	struct gamma_client *g = data;
	(void)version;

	if (!strcmp(iface, wl_output_interface.name)) {
		if (g->n_outputs < MAX_OUTPUTS)
			g->outputs[g->n_outputs++] =
				wl_registry_bind(reg, name, &wl_output_interface, 1);
	} else if (!strcmp(iface, zwlr_gamma_control_manager_v1_interface.name)) {
		g->manager = wl_registry_bind(reg, name,
			&zwlr_gamma_control_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

struct gamma_client *gamma_connect(void)
{
	struct gamma_client *g = calloc(1, sizeof *g);
	if (!g)
		return NULL;

	g->dpy = wl_display_connect(NULL);
	if (!g->dpy) {
		free(g);
		return NULL;
	}

	g->registry = wl_display_get_registry(g->dpy);
	wl_registry_add_listener(g->registry, &registry_listener, g);
	wl_display_roundtrip(g->dpy);

	if (!g->manager) {
		gamma_destroy(g);
		return NULL;
	}
	return g;
}

int gamma_acquire(struct gamma_client *g)
{
	for (int i = 0; i < g->n_outputs; i++) {
		g->pending_have_size = 0;
		g->pending_failed = 0;

		struct zwlr_gamma_control_v1 *ctrl =
			zwlr_gamma_control_manager_v1_get_gamma_control(g->manager, g->outputs[i]);
		zwlr_gamma_control_v1_add_listener(ctrl, &gamma_listener, g);
		wl_display_roundtrip(g->dpy);

		if (g->pending_failed || !g->pending_have_size || g->pending_size == 0) {
			zwlr_gamma_control_v1_destroy(ctrl);
			continue;
		}
		g->ctrls[g->n_ctrls].ctrl = ctrl;
		g->ctrls[g->n_ctrls].size = g->pending_size;
		g->ctrls[g->n_ctrls].failed = 0;
		g->n_ctrls++;
	}
	return g->n_ctrls > 0;
}

void gamma_apply(struct gamma_client *g, const struct params *p)
{
	for (int i = 0; i < g->n_ctrls; i++) {
		if (g->ctrls[i].failed)
			continue;
		int fd = build_ramp_fd(g->ctrls[i].size, p);
		if (fd < 0)
			continue;
		zwlr_gamma_control_v1_set_gamma(g->ctrls[i].ctrl, fd);
		close(fd);   /* libwayland dup()s it while marshalling */
	}
	wl_display_roundtrip(g->dpy);
}

int gamma_fd(struct gamma_client *g)
{
	return wl_display_get_fd(g->dpy);
}

int gamma_dispatch(struct gamma_client *g)
{
	return wl_display_dispatch(g->dpy) < 0 ? -1 : 0;
}

void gamma_flush(struct gamma_client *g)
{
	wl_display_flush(g->dpy);
}

void gamma_destroy(struct gamma_client *g)
{
	if (!g)
		return;
	for (int i = 0; i < g->n_ctrls; i++)
		zwlr_gamma_control_v1_destroy(g->ctrls[i].ctrl);
	for (int i = 0; i < g->n_outputs; i++)
		wl_output_destroy(g->outputs[i]);
	if (g->manager)
		zwlr_gamma_control_manager_v1_destroy(g->manager);
	if (g->registry)
		wl_registry_destroy(g->registry);
	if (g->dpy)
		wl_display_disconnect(g->dpy);
	free(g);
}
