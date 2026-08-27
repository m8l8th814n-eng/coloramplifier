/* Presets and persisted state: ~/.config/controlorfreak/ */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "freak.h"

#define CONFIG_SUBDIR ".config/controlorfreak"

static const char *home(void)
{
	const char *h = getenv("HOME");
	return (h && *h) ? h : "/tmp";
}

static void preset_dir(char *buf, size_t n)
{
	snprintf(buf, n, "%s/%s/presets", home(), CONFIG_SUBDIR);
}

static void preset_path(const char *name, char *buf, size_t n)
{
	snprintf(buf, n, "%s/%s/presets/%s.freak", home(), CONFIG_SUBDIR, name);
}

static void state_path(char *buf, size_t n)
{
	snprintf(buf, n, "%s/%s/state.freak", home(), CONFIG_SUBDIR);
}

/* mkdir -p, but only ever for our own two-deep config path */
static int make_dirs(const char *path)
{
	char tmp[512];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/* The preset format is TOML, but only ever "key = number" lines. */
static int read_params(const char *path, struct params *out)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;

	params_default(out);

	char line[256];
	while (fgets(line, sizeof line, f)) {
		char key[64];
		double val;
		if (sscanf(line, " %63[a-zA-Z_] = %lf", key, &val) == 2) {
			double *field = params_field(out, key);
			if (field)
				*field = val;
		}
	}
	fclose(f);
	return 1;
}

static int write_params(const char *path, const struct params *p)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return errno;

	fprintf(f, "red = %.1f\n",      p->red);
	fprintf(f, "orange = %.1f\n",   p->orange);
	fprintf(f, "green = %.1f\n",    p->green);
	fprintf(f, "blue = %.1f\n",     p->blue);
	fprintf(f, "contrast = %.1f\n", p->contrast);
	fprintf(f, "gamma = %.1f\n",    p->gamma);

	int err = ferror(f) ? EIO : 0;
	if (fclose(f) != 0 && !err)
		err = errno;
	return err;
}

int preset_load(const char *name, struct params *out)
{
	char path[512];
	preset_path(name, path, sizeof path);
	return read_params(path, out);
}

int preset_save(const char *name, const struct params *p)
{
	char dir[512], path[512];
	preset_dir(dir, sizeof dir);
	if (make_dirs(dir) < 0)
		return errno;
	preset_path(name, path, sizeof path);
	return write_params(path, p);
}

int preset_delete(const char *name)
{
	char path[512];
	preset_path(name, path, sizeof path);
	return unlink(path);
}

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int preset_list(char ***names)
{
	char dir[512];
	preset_dir(dir, sizeof dir);

	DIR *d = opendir(dir);
	*names = NULL;
	if (!d)
		return 0;

	int cap = 16, n = 0;
	char **list = calloc(cap, sizeof *list);
	if (!list) {
		closedir(d);
		return 0;
	}

	struct dirent *e;
	while ((e = readdir(d))) {
		size_t len = strlen(e->d_name);
		if (len <= 6 || strcmp(e->d_name + len - 6, ".freak"))
			continue;
		if (n == cap) {
			char **grown = realloc(list, (size_t)cap * 2 * sizeof *list);
			if (!grown)
				break;
			list = grown;
			cap *= 2;
		}
		list[n] = strndup(e->d_name, len - 6);
		if (!list[n])
			break;
		n++;
	}
	closedir(d);

	qsort(list, (size_t)n, sizeof *list, cmp_name);
	*names = list;
	return n;
}

void preset_list_free(char **names, int n)
{
	for (int i = 0; i < n; i++)
		free(names[i]);
	free(names);
}

void state_load(struct params *p)
{
	char path[512];
	state_path(path, sizeof path);
	if (!read_params(path, p))
		params_default(p);
}

void state_save(const struct params *p)
{
	char dir[512], path[512];
	snprintf(dir, sizeof dir, "%s/%s", home(), CONFIG_SUBDIR);
	if (make_dirs(dir) < 0)
		return;
	state_path(path, sizeof path);
	write_params(path, p);
}
