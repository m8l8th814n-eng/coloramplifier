#include <string.h>

#include "freak.h"

void params_default(struct params *p)
{
	p->red = p->orange = p->green = p->blue = 80.0;
	p->contrast = p->gamma = 80.0;
}

double pct_to_val(double pct)
{
	return (pct / 100.0) * 5.0 - 3.0;
}

double *params_field(struct params *p, const char *name)
{
	if (!strcmp(name, "red"))      return &p->red;
	if (!strcmp(name, "orange"))   return &p->orange;
	if (!strcmp(name, "green"))    return &p->green;
	if (!strcmp(name, "blue"))     return &p->blue;
	if (!strcmp(name, "contrast")) return &p->contrast;
	if (!strcmp(name, "gamma"))    return &p->gamma;
	return NULL;
}
