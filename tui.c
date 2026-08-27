/* freak TUI: raw-mode terminal, ANSI truecolor, no curses.
 *
 * Two ways to run:
 *   - a daemon already holds the socket: every change is sent to it as SET
 *   - no daemon: we take the gamma controls ourselves and serve the socket,
 *     so freakctl over SSH still works while the TUI is open.
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "freak.h"

/* ---------- fields ---------- */

struct field_def {
	const char *name;
	const char *label;
	size_t offset;
};

static const struct field_def FIELDS[] = {
	{ "orange",   "ORNG ", offsetof(struct params, orange)   },
	{ "red",      "RED  ", offsetof(struct params, red)      },
	{ "green",    "GREEN", offsetof(struct params, green)    },
	{ "blue",     "BLUE ", offsetof(struct params, blue)     },
	{ "contrast", "CNTR ", offsetof(struct params, contrast) },
	{ "gamma",    "GAMMA", offsetof(struct params, gamma)    },
};
#define NFIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))

static double *field_ptr(struct params *p, int i)
{
	return (double *)((char *)p + FIELDS[i].offset);
}

/* ---------- colors ---------- */

struct rgb { unsigned char r, g, b; };

static const struct rgb LGRAY  = { 85, 84, 105 };
static const struct rgb ACCENT = { 71, 93, 175 };
static const struct rgb PANEL  = { 27, 26, 36 };
static const struct rgb MUTED  = { 85, 84, 105 };
static const struct rgb TITLE  = { 90, 196, 97 };
static const struct rgb RED    = { 200, 60, 60 };

static struct rgb hsv_to_rgb(double h)
{
	h = fmod(h, 360.0);
	if (h < 0)
		h += 360.0;

	double c = 1.0;
	double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
	double r, g, b;

	if (h < 60)       { r = c; g = x; b = 0; }
	else if (h < 120) { r = x; g = c; b = 0; }
	else if (h < 180) { r = 0; g = c; b = x; }
	else if (h < 240) { r = 0; g = x; b = c; }
	else if (h < 300) { r = x; g = 0; b = c; }
	else              { r = c; g = 0; b = x; }

	struct rgb out = { (unsigned char)(r * 255), (unsigned char)(g * 255),
	                   (unsigned char)(b * 255) };
	return out;
}

/* "gay mode": everything cycles through the rainbow */
static struct rgb gay(double t, double phase)
{
	return hsv_to_rgb(t * 60.0 + phase);
}

static struct rgb pick(int gm, double t, double phase, struct rgb fallback)
{
	return gm ? gay(t, phase) : fallback;
}

/* ---------- output buffer ---------- */

struct buf {
	char *data;
	size_t len, cap;
};

static void bgrow(struct buf *b, size_t need)
{
	if (b->len + need + 1 <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 4096;
	while (cap < b->len + need + 1)
		cap *= 2;
	b->data = realloc(b->data, cap);
	b->cap = cap;
}

static void bputs(struct buf *b, const char *s)
{
	size_t n = strlen(s);
	bgrow(b, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void bprintf(struct buf *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	bputs(b, tmp);
}

/* append at most maxcols characters (utf-8 aware, all our glyphs are 1 wide) */
static void bputs_cols(struct buf *b, const char *s, int maxcols)
{
	int cols = 0;
	while (*s && cols < maxcols) {
		unsigned char c = (unsigned char)*s;
		int len = 1;
		if (c >= 0xF0) len = 4;
		else if (c >= 0xE0) len = 3;
		else if (c >= 0xC0) len = 2;
		bgrow(b, (size_t)len);
		for (int i = 0; i < len && s[i]; i++)
			b->data[b->len++] = s[i];
		b->data[b->len] = '\0';
		s += len;
		cols++;
	}
}

static void brepeat(struct buf *b, const char *s, int n)
{
	for (int i = 0; i < n; i++)
		bputs(b, s);
}

static void fg(struct buf *b, struct rgb c)
{
	bprintf(b, "\x1b[38;2;%u;%u;%um", c.r, c.g, c.b);
}

static void bold(struct buf *b)   { bputs(b, "\x1b[1m"); }
static void creset(struct buf *b) { bputs(b, "\x1b[0m"); }
static void moveto(struct buf *b, int row, int col)
{
	bprintf(b, "\x1b[%d;%dH", row, col);
}
static void clear_eol(struct buf *b) { bputs(b, "\x1b[K"); }

/* ---------- app state ---------- */

enum mode { M_NORMAL, M_PRESETS, M_SAVE, M_CONFIG, M_HELP };

struct app {
	struct params params;
	int selected;
	enum mode mode;

	char **presets;
	int npresets;
	int preset_sel;

	char save_input[64];
	char status[160];

	int daemon_connected;
	int gm;                       /* gay mode */
	double start;

	struct gamma_client *gamma;   /* only when standalone */
	struct ipc_server srv;
	int serving;

	int quit;
	int dirty;
};

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---------- terminal ---------- */

static struct termios orig_tio;
static int raw_active;

static void term_restore(void)
{
	if (!raw_active)
		return;
	raw_active = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
	(void)!write(STDOUT_FILENO, "\x1b[0m\x1b[?25h\x1b[?1049l", 18);
}

static void on_fatal_signal(int sig)
{
	term_restore();
	_exit(128 + sig);
}

static void term_raw(void)
{
	if (tcgetattr(STDIN_FILENO, &orig_tio) < 0) {
		fprintf(stderr, "freak: not a terminal\n");
		exit(1);
	}
	struct termios t = orig_tio;
	cfmakeraw(&t);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
	raw_active = 1;
	atexit(term_restore);
	signal(SIGINT, on_fatal_signal);
	signal(SIGTERM, on_fatal_signal);
	signal(SIGHUP, on_fatal_signal);
	(void)!write(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 14);
}

static void term_size(int *rows, int *cols)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	} else {
		*rows = 24;
		*cols = 80;
	}
}

/* ---------- keys ---------- */

enum key {
	K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT,
	K_ENTER, K_ESC, K_BACKSPACE, K_DELETE, K_IGNORE,
};

static int read_keys(int *keys, int max)
{
	char b[64];
	ssize_t n = read(STDIN_FILENO, b, sizeof b);
	if (n <= 0)
		return 0;

	int count = 0;
	for (ssize_t i = 0; i < n && count < max; ) {
		unsigned char c = (unsigned char)b[i];

		if (c == 0x1b && i + 1 < n && b[i + 1] == '[') {
			if (i + 2 >= n) {
				i = n;
				continue;
			}
			char c2 = b[i + 2];
			if (c2 == 'A') { keys[count++] = K_UP;    i += 3; }
			else if (c2 == 'B') { keys[count++] = K_DOWN;  i += 3; }
			else if (c2 == 'C') { keys[count++] = K_RIGHT; i += 3; }
			else if (c2 == 'D') { keys[count++] = K_LEFT;  i += 3; }
			else if (isdigit((unsigned char)c2)) {
				ssize_t j = i + 2;
				while (j < n && b[j] != '~')
					j++;
				if (j < n && c2 == '3')
					keys[count++] = K_DELETE;
				i = (j < n) ? j + 1 : n;
			} else {
				i += 3;
			}
		} else if (c == 0x1b) {
			keys[count++] = K_ESC;
			i++;
		} else if (c == '\r' || c == '\n') {
			keys[count++] = K_ENTER;
			i++;
		} else if (c == 127 || c == 8) {
			keys[count++] = K_BACKSPACE;
			i++;
		} else if (c == 3) {            /* ctrl-c */
			keys[count++] = 'q';
			i++;
		} else if (c < 32) {
			keys[count++] = K_IGNORE;
			i++;
		} else {
			keys[count++] = c;
			i++;
		}
	}
	return count;
}

/* ---------- config (gay mode) ---------- */

static int file_says_gay(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;                      /* no such config */

	int gm = 0;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (strncmp(s, "mode", 4))
			continue;
		char *eq = strchr(s, '=');
		if (!eq)
			continue;
		char *v = eq + 1;
		while (*v == ' ' || *v == '"' || *v == '\'')
			v++;
		gm = !strncmp(v, "gay", 3);
		break;
	}
	fclose(f);
	return gm;
}

static int load_gay_mode(void)
{
	int gm = file_says_gay("config.toml");
	if (gm >= 0)
		return gm;

	const char *home = getenv("HOME");
	if (!home)
		return 0;
	char path[512];
	snprintf(path, sizeof path, "%s/.config/wlauncher/config.ini", home);
	gm = file_says_gay(path);
	return gm > 0;
}

/* ---------- actions ---------- */

static void refresh_presets(struct app *a)
{
	preset_list_free(a->presets, a->npresets);
	a->npresets = preset_list(&a->presets);
	if (a->preset_sel >= a->npresets)
		a->preset_sel = a->npresets ? a->npresets - 1 : 0;
}

static void push_change(struct app *a)
{
	if (a->daemon_connected) {
		char cmd[128], resp[1024];
		snprintf(cmd, sizeof cmd, "SET %s %.1f",
		         FIELDS[a->selected].name, *field_ptr(&a->params, a->selected));
		if (ipc_send(cmd, resp, sizeof resp) < 0)
			snprintf(a->status, sizeof a->status, "daemon gone");
	} else {
		if (a->gamma)
			gamma_apply(a->gamma, &a->params);
		state_save(&a->params);
	}
}

static void adjust(struct app *a, double delta)
{
	double *v = field_ptr(&a->params, a->selected);
	*v += delta;
	if (*v < 0.0)   *v = 0.0;
	if (*v > 100.0) *v = 100.0;
	push_change(a);
}

static void do_reset(struct app *a)
{
	params_default(&a->params);
	if (a->daemon_connected) {
		char resp[1024];
		ipc_send("RESET", resp, sizeof resp);
	} else {
		if (a->gamma)
			gamma_apply(a->gamma, &a->params);
		state_save(&a->params);
	}
	snprintf(a->status, sizeof a->status, "reset to neutral (80%%)");
}

static void load_preset_named(struct app *a, const char *name)
{
	if (a->daemon_connected) {
		char cmd[160], resp[1024];
		snprintf(cmd, sizeof cmd, "LOAD %s", name);
		if (ipc_send(cmd, resp, sizeof resp) < 0) {
			snprintf(a->status, sizeof a->status, "daemon gone");
			return;
		}
		if (!strcmp(resp, "OK")) {
			preset_load(name, &a->params);
			snprintf(a->status, sizeof a->status, "loaded: %s", name);
		} else {
			snprintf(a->status, sizeof a->status, "%.120s", resp);
		}
		return;
	}

	struct params p;
	if (preset_load(name, &p)) {
		a->params = p;
		if (a->gamma)
			gamma_apply(a->gamma, &a->params);
		state_save(&a->params);
		snprintf(a->status, sizeof a->status, "loaded: %s", name);
	} else {
		snprintf(a->status, sizeof a->status, "not found: %s", name);
	}
}

static void load_preset_index(struct app *a, int idx)
{
	if (idx >= 0 && idx < a->npresets)
		load_preset_named(a, a->presets[idx]);
}

static void do_save(struct app *a, const char *name)
{
	if (!*name) {
		snprintf(a->status, sizeof a->status, "save cancelled: empty name");
		return;
	}
	if (a->daemon_connected) {
		char cmd[160], resp[1024];
		snprintf(cmd, sizeof cmd, "SAVE %s", name);
		if (ipc_send(cmd, resp, sizeof resp) < 0)
			snprintf(a->status, sizeof a->status, "daemon gone");
		else if (!strcmp(resp, "OK"))
			snprintf(a->status, sizeof a->status, "saved: %s", name);
		else
			snprintf(a->status, sizeof a->status, "%.120s", resp);
	} else {
		int err = preset_save(name, &a->params);
		if (err)
			snprintf(a->status, sizeof a->status, "save failed: %s", strerror(err));
		else
			snprintf(a->status, sizeof a->status, "saved: %s", name);
	}
	refresh_presets(a);
}

static void delete_preset(struct app *a, const char *name)
{
	char copy[128];
	snprintf(copy, sizeof copy, "%s", name);
	preset_delete(copy);
	refresh_presets(a);
	snprintf(a->status, sizeof a->status, "deleted: %s", copy);
}

/* ---------- rendering ---------- */

static void draw_slider(struct buf *b, const struct app *a, int idx, int width,
                        double t, int selected)
{
	const struct params *p = &a->params;
	double val = *(const double *)((const char *)p + FIELDS[idx].offset);

	int bar_width = width - 16;
	if (bar_width < 3)
		bar_width = 3;
	int marker = (int)(val / 100.0 * (bar_width - 1) + 0.5);

	double phase = idx * 72.0;
	struct rgb bar_color   = pick(a->gm, t, phase, ACCENT);
	struct rgb track_color = pick(a->gm, t, phase + 210.0, MUTED);
	struct rgb pct_color   = pick(a->gm, t, phase + 90.0, selected ? ACCENT : LGRAY);

	fg(b, bar_color);
	bputs(b, selected ? "▸" : " ");

	if (selected) {
		bold(b);
		fg(b, pick(a->gm, t, phase, ACCENT));
	} else {
		fg(b, LGRAY);
	}
	bprintf(b, "%s ", FIELDS[idx].label);
	creset(b);

	fg(b, track_color);
	bputs(b, "⟨");

	for (int i = 0; i < bar_width; i++) {
		if (i == marker) {
			fg(b, bar_color);
			bputs(b, "●");
		} else if (i < marker) {
			fg(b, bar_color);
			bputs(b, "─");
		} else {
			fg(b, track_color);
			bputs(b, "·");
		}
	}

	fg(b, track_color);
	bputs(b, "⟩");

	fg(b, pct_color);
	if (selected)
		bold(b);
	bprintf(b, " %3d%%", (int)(val + 0.5));
	creset(b);
}

static void draw_sliders_block(struct buf *b, const struct app *a, int rows,
                               int cols, double t)
{
	int height = NFIELDS * 2 + 4;
	if (height > rows)
		height = rows;
	int inner_w = cols - 2;
	struct rgb border = pick(a->gm, t, 30.0, PANEL);

	for (int row = 1; row <= height; row++) {
		moveto(b, row, 1);
		if (row == 1) {
			fg(b, border);
			bputs(b, "┌");
			fg(b, pick(a->gm, t, 24.0, TITLE));
			bputs(b, " freak 2 ");
			fg(b, border);
			brepeat(b, "─", inner_w - 9);
			bputs(b, "┐");
			creset(b);
		} else if (row == height) {
			fg(b, border);
			bputs(b, "└");
			brepeat(b, "─", inner_w);
			bputs(b, "┘");
			creset(b);
		} else {
			fg(b, border);
			bputs(b, "│");
			creset(b);

			int slider = -1;
			if (row >= 3 && (row - 3) % 2 == 0 && (row - 3) / 2 < NFIELDS)
				slider = (row - 3) / 2;

			if (slider >= 0 && cols - 4 >= 20) {
				bputs(b, " ");
				draw_slider(b, a, slider, cols - 4, t,
				            a->selected == slider && a->mode == M_NORMAL);
				bputs(b, "   ");   /* pad past the border */
			} else {
				brepeat(b, " ", inner_w);
			}
			moveto(b, row, cols);
			fg(b, border);
			bputs(b, "│");
			creset(b);
		}
		clear_eol(b);
	}

	if (height < rows) {
		moveto(b, height + 1, 1);
		fg(b, PANEL);
		bputs(b, "  (");
		fg(b, ACCENT);
		bputs(b, "p");
		fg(b, PANEL);
		bputs(b, ")resets  ");
		fg(b, pick(a->gm, t, 180.0, LGRAY));
		bputs_cols(b, a->status, cols - 13);
		creset(b);
		clear_eol(b);
	}

	int tail = height + 1 + (height < rows ? 1 : 0);
	if (tail <= rows) {
		moveto(b, tail, 1);
		bputs(b, "\x1b[J");
	}
}

/* box with a title, filled with blanks — panels are drawn on top of it */
static void draw_panel_frame(struct buf *b, int y, int x, int h, int w,
                             const char *title, struct rgb color)
{
	for (int row = 0; row < h; row++) {
		moveto(b, y + row, x);
		fg(b, color);
		if (row == 0) {
			bputs(b, "┌");
			bprintf(b, "%s", title);
			brepeat(b, "─", w - 2 - (int)strlen(title));
			bputs(b, "┐");
		} else if (row == h - 1) {
			bputs(b, "└");
			brepeat(b, "─", w - 2);
			bputs(b, "┘");
		} else {
			bputs(b, "│");
			creset(b);
			brepeat(b, " ", w - 2);
			fg(b, color);
			bputs(b, "│");
		}
		creset(b);
	}
}

static void draw_presets_panel(struct buf *b, const struct app *a, int y, int x,
                               int h, int w, double t)
{
	int inner_y = y + 1, inner_x = x + 1;
	int inner_h = h - 2, inner_w = w - 2;

	if (a->npresets == 0) {
		moveto(b, inner_y, inner_x);
		fg(b, LGRAY);
		bputs(b, " no presets — press (s)ave");
		creset(b);
		return;
	}

	int visible = inner_h - 1;
	int row = 0, col = 0;

	for (int i = 0; i < a->npresets && row < visible; i++) {
		int selected = (i == a->preset_sel);
		struct rgb c   = a->gm ? gay(t, i * 30.0) : LGRAY;
		struct rgb dim = a->gm ? gay(t, i * 30.0 + 180.0) : PANEL;

		char idx[8];
		if (i < 9)
			snprintf(idx, sizeof idx, "[%d]", i + 1);
		else
			snprintf(idx, sizeof idx, "   ");

		int item_w = 2 + (int)strlen(idx) + 1 + (int)strlen(a->presets[i]) + 3;
		if (col > 0 && col + item_w > inner_w) {
			row++;
			col = 0;
			if (row >= visible)
				break;
		}

		moveto(b, inner_y + row, inner_x + col);
		fg(b, c);
		bputs(b, selected ? "▸ " : "  ");
		fg(b, dim);
		bprintf(b, "%s ", idx);
		if (selected)
			bold(b);
		fg(b, c);
		bputs_cols(b, a->presets[i], inner_w - col - 6);
		creset(b);
		col += item_w;
	}

	moveto(b, inner_y + inner_h - 1, inner_x);
	fg(b, pick(a->gm, t, 0.0, ACCENT));
	bputs(b, " Enter");
	fg(b, LGRAY);
	bputs(b, " load  ");
	fg(b, pick(a->gm, t, 120.0, RED));
	bputs(b, "Del");
	fg(b, LGRAY);
	bputs(b, " delete  Esc close");
	creset(b);
}

static void draw_save_panel(struct buf *b, const struct app *a, int y, int x,
                            double t)
{
	struct rgb c = pick(a->gm, t, 60.0, ACCENT);

	moveto(b, y + 2, x + 1);
	fg(b, c);
	bputs(b, " save as: >> ");
	fg(b, pick(a->gm, t, 0.0, LGRAY));
	bold(b);
	bprintf(b, "%s_", a->save_input);
	creset(b);

	moveto(b, y + 4, x + 1);
	fg(b, LGRAY);
	bputs(b, " Enter to save  Esc cancel");
	creset(b);
}

static void draw_keys_panel(struct buf *b, const struct app *a, int y, int x,
                            int h, int w, double t)
{
	static const char *lines[] = {
		" ← →     adjust value",
		" ↑ ↓     select parameter",
		" r       reset all to 80% (neutral)",
		" s       save preset",
		" p       preset panel",
		" 1-9     load preset by index",
		" c       config info",
		" h       toggle this help",
		" q       quit",
	};
	int n = (int)(sizeof lines / sizeof lines[0]);

	for (int i = 0; i < n && i + 2 < h; i++) {
		moveto(b, y + 1 + i, x + 1);
		fg(b, a->gm ? gay(t, i * 25.0) : LGRAY);
		bputs_cols(b, lines[i], w - 2);
		creset(b);
	}
}

static void draw_config_panel(struct buf *b, const struct app *a, int y, int x,
                              int h, int w)
{
	const char *home = getenv("HOME");
	char lines[5][256];

	snprintf(lines[0], sizeof lines[0], " socket    %s", SOCKET_PATH);
	snprintf(lines[1], sizeof lines[1], " presets   %s/.config/controlorfreak/presets",
	         home ? home : "?");
	snprintf(lines[2], sizeof lines[2], " state     %s/.config/controlorfreak/state.freak",
	         home ? home : "?");
	snprintf(lines[3], sizeof lines[3], " mode      %s",
	         a->daemon_connected ? "daemon connected" :
	         (a->gamma ? "standalone (holding gamma)" : "no gamma control"));
	snprintf(lines[4], sizeof lines[4], " colors    %s", a->gm ? "gay" : "default");

	for (int i = 0; i < 5 && i + 2 < h; i++) {
		moveto(b, y + 1 + i, x + 1);
		fg(b, LGRAY);
		bputs_cols(b, lines[i], w - 2);
		creset(b);
	}
}

static void render(struct app *a, struct buf *b, int rows, int cols)
{
	double t = now_seconds() - a->start;

	b->len = 0;
	if (b->data)
		b->data[0] = '\0';
	bputs(b, "\x1b[H");

	draw_sliders_block(b, a, rows, cols, t);

	if (a->mode == M_NORMAL)
		return;

	int ph = rows * 70 / 100;
	int pw = cols * 80 / 100;
	if (ph < 6) ph = rows < 6 ? rows : 6;
	if (pw < 24) pw = cols < 24 ? cols : 24;
	int py = 1 + (rows - ph) / 2;
	int px = 1 + (cols - pw) / 2;
	if (ph < 3 || pw < 6)
		return;

	const char *title = " PRESETS ";
	if (a->mode == M_HELP)   title = " KEYS ";
	if (a->mode == M_CONFIG) title = " CONFIG ";

	struct rgb color = pick(a->gm, t, 120.0, ACCENT);
	draw_panel_frame(b, py, px, ph, pw, title, color);

	switch (a->mode) {
	case M_PRESETS: draw_presets_panel(b, a, py, px, ph, pw, t); break;
	case M_SAVE:    draw_save_panel(b, a, py, px, t);            break;
	case M_HELP:    draw_keys_panel(b, a, py, px, ph, pw, t);    break;
	case M_CONFIG:  draw_config_panel(b, a, py, px, ph, pw);     break;
	default: break;
	}
}

/* ---------- key handling ---------- */

static void handle_key(struct app *a, int key)
{
	a->dirty = 1;

	switch (a->mode) {
	case M_NORMAL:
		switch (key) {
		case 'q': a->quit = 1; break;
		case 'r': do_reset(a); break;
		case 'h': a->mode = M_HELP; break;
		case 'c': a->mode = M_CONFIG; break;
		case 'p':
			refresh_presets(a);
			a->preset_sel = 0;
			a->mode = M_PRESETS;
			break;
		case 's':
			a->save_input[0] = '\0';
			a->mode = M_SAVE;
			break;
		case K_UP:    if (a->selected > 0) a->selected--; break;
		case K_DOWN:  if (a->selected < NFIELDS - 1) a->selected++; break;
		case K_LEFT:  adjust(a, -1.0); break;
		case K_RIGHT: adjust(a, 1.0); break;
		default:
			if (key >= '1' && key <= '9')
				load_preset_index(a, key - '1');
			break;
		}
		break;

	case M_PRESETS:
		switch (key) {
		case K_ESC:
		case 'p': a->mode = M_NORMAL; break;
		case 'q': a->quit = 1; break;
		case K_UP:   if (a->preset_sel > 0) a->preset_sel--; break;
		case K_DOWN: if (a->preset_sel + 1 < a->npresets) a->preset_sel++; break;
		case K_ENTER:
			load_preset_index(a, a->preset_sel);
			a->mode = M_NORMAL;
			break;
		case K_DELETE:
		case K_BACKSPACE:
			if (a->preset_sel < a->npresets)
				delete_preset(a, a->presets[a->preset_sel]);
			break;
		case 's':
			a->save_input[0] = '\0';
			a->mode = M_SAVE;
			break;
		default:
			if (key >= '1' && key <= '9') {
				load_preset_index(a, key - '1');
				a->mode = M_NORMAL;
			}
			break;
		}
		break;

	case M_SAVE:
		switch (key) {
		case K_ESC: a->mode = M_NORMAL; break;
		case K_ENTER:
			do_save(a, a->save_input);
			a->save_input[0] = '\0';
			a->mode = M_NORMAL;
			break;
		case K_BACKSPACE: {
			size_t n = strlen(a->save_input);
			if (n)
				a->save_input[n - 1] = '\0';
			break;
		}
		default:
			if (key < 256 && (isalnum(key) || key == '_' || key == '-')) {
				size_t n = strlen(a->save_input);
				if (n + 1 < sizeof a->save_input) {
					a->save_input[n] = (char)key;
					a->save_input[n + 1] = '\0';
				}
			}
			break;
		}
		break;

	case M_CONFIG:
	case M_HELP:
		if (key == K_ESC || key == 'q' || key == 'h' || key == 'c')
			a->mode = M_NORMAL;
		break;
	}
}

/* ---------- main loop ---------- */

void tui_run(void)
{
	signal(SIGPIPE, SIG_IGN);

	struct app a;
	memset(&a, 0, sizeof a);
	a.srv.listen_fd = -1;
	a.start = now_seconds();
	a.gm = load_gay_mode();
	a.dirty = 1;

	char resp[1024];
	a.daemon_connected = (ipc_send("GET", resp, sizeof resp) == 0);
	if (a.daemon_connected && params_from_json(resp, &a.params)) {
		snprintf(a.status, sizeof a.status, "daemon connected");
	} else if (a.daemon_connected) {
		state_load(&a.params);
		snprintf(a.status, sizeof a.status, "daemon connected");
	} else {
		state_load(&a.params);
		a.gamma = gamma_connect();
		if (a.gamma && gamma_acquire(a.gamma)) {
			gamma_apply(a.gamma, &a.params);
		} else {
			gamma_destroy(a.gamma);
			a.gamma = NULL;
		}
		a.serving = (ipc_server_start(&a.srv) == 0);
		snprintf(a.status, sizeof a.status, "%s  %s",
		         a.gamma ? "standalone" : "no gamma control",
		         a.serving ? SOCKET_PATH : "(socket unavailable)");
	}

	refresh_presets(&a);
	term_raw();
	sway_set_floating();

	struct buf b = { 0 };
	int rows, cols, last_rows = 0, last_cols = 0;

	while (!a.quit) {
		term_size(&rows, &cols);
		if (rows != last_rows || cols != last_cols) {
			a.dirty = 1;
			last_rows = rows;
			last_cols = cols;
		}
		if (a.dirty || a.gm) {
			render(&a, &b, rows, cols);
			(void)!write(STDOUT_FILENO, b.data, b.len);
			a.dirty = 0;
		}

		struct pollfd fds[2 + 1 + MAX_CLIENTS];
		int n = 0;
		fds[n].fd = STDIN_FILENO;
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;

		int gamma_idx = -1;
		if (a.gamma) {
			gamma_idx = n;
			fds[n].fd = gamma_fd(a.gamma);
			fds[n].events = POLLIN;
			fds[n].revents = 0;
			n++;
			gamma_flush(a.gamma);
		}
		int srv_idx = n;
		if (a.serving)
			n += ipc_server_fill(&a.srv, fds + n,
			                     (int)(sizeof fds / sizeof *fds) - n);

		if (poll(fds, (nfds_t)n, 33) < 0)
			continue;

		if (fds[0].revents & POLLIN) {
			int keys[32];
			int count = read_keys(keys, 32);
			for (int i = 0; i < count && !a.quit; i++)
				if (keys[i] != K_IGNORE)
					handle_key(&a, keys[i]);
		}

		if (gamma_idx >= 0 && (fds[gamma_idx].revents & POLLIN)) {
			if (gamma_dispatch(a.gamma) < 0) {
				gamma_destroy(a.gamma);
				a.gamma = NULL;
				snprintf(a.status, sizeof a.status, "wayland connection lost");
				a.dirty = 1;
			}
		}

		if (a.serving) {
			int changed = 0;
			ipc_server_process(&a.srv, fds + srv_idx, n - srv_idx, &a.params, &changed);
			if (changed) {
				if (a.gamma)
					gamma_apply(a.gamma, &a.params);
				snprintf(a.status, sizeof a.status, "updated via ipc");
				a.dirty = 1;
			}
		}
	}

	if (a.serving)
		ipc_server_stop(&a.srv);
	gamma_destroy(a.gamma);
	preset_list_free(a.presets, a.npresets);
	free(b.data);
	term_restore();
}
