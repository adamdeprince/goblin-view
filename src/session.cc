/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"

#include <string.h>

struct dv_session *
dv_session_find(const char *name)
{
	struct dv_session	*s;

	if (dv_srv == NULL)
		return (NULL);
	TAILQ_FOREACH(s, &dv_srv->sessions, entry) {
		if (strcmp(s->name, name) == 0)
			return (s);
	}
	return (NULL);
}

struct dv_session *
dv_session_default(void)
{
	if (dv_srv == NULL)
		return (NULL);
	return (TAILQ_FIRST(&dv_srv->sessions));
}

struct dv_session *
dv_session_create(const char *name, struct dv_size sz)
{
	struct dv_session	*s;

	s = dv_new<dv_session>();
	dv_strlcpy(s->name, name != NULL && name[0] != '\0' ? name : "0",
	    sizeof s->name);
	dv_desktop_init(&s->desktop, sz);
	TAILQ_INSERT_TAIL(&dv_srv->sessions, s, entry);
	return (s);
}

void
dv_session_destroy(struct dv_session *s)
{
	struct dv_window	*w;

	while ((w = TAILQ_FIRST(&s->desktop.windows)) != NULL)
		dv_window_close(w);
	TAILQ_REMOVE(&dv_srv->sessions, s, entry);
	dv_buf_free(&s->paste);
	dv_buf_free(&s->mark);
	free(s);
}
