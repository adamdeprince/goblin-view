/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Text for vector scenes (graphics protocol f=200) drawn here: the layout
 * object of a text record shaped into outlines, the same object measured,
 * and the fonts both are set in. See vtext.cc.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
#include <string>
#include <utility>
#include <vector>

/*
 * A font a program uploaded, by the handle the scene's records name it with
 * ("dvf12"). `request` is the upload request's JSON: its data and index are
 * read from it. On success `*info` is the face's description as JSON members
 * (no braces), for the reply.
 */
int	 dv_vtext_upload(const std::string &handle, const std::string &request,
	    std::string *info, std::string *err);
void	 dv_vtext_release(const std::string &handle);
/* 1 when glyph outlines can be drawn (HarfBuzz 4.0 or later), else text is
 * laid out and measured only. */
int	 dv_vtext_outlines(void);

/*
 * The vector service answered here: {"op":"fonts"}, {"op":"inspect",...} and
 * {"op":"measure",...}. `session` is the window's uploaded fonts, as the
 * program named them and as their handles. On success `*result` is the JSON
 * to reply with.
 */
int	 dv_vtext_service(const std::string &request,
	    const std::vector<std::pair<std::string, std::string>> &session,
	    std::string *result, std::string *err);

extern "C" {
#endif

/* The outlines of a text record, for the rasterizer (src/vector/vector.c):
 * runs of path commands, each with its paints. Ops are the rasterizer's:
 * 0 move, 1 line, 2 quadratic, 3 cubic, 4 close. */
typedef struct {
	int	op;
	double	p[6];
} OdinTextCmd;

typedef struct {
	OdinTextCmd	*cmds;
	size_t		 ncmds;
	char		*fill;
	char		*stroke;
	double		 width;
} OdinTextRun;

/* Lays out the base64 JSON of a text record. 1 on success, with the runs to
 * free; 0 with a static message in *why when it is not a layout. */
int	 odin_vector_text(const char *b64, size_t len, OdinTextRun **runs,
	    size_t *nruns, const char **why);
void	 odin_vector_text_free(OdinTextRun *runs, size_t nruns);

#ifdef __cplusplus
}
#endif
