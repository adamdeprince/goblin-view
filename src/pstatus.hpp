/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * The Program Status Protocol (OSC 7501, revision 0.4), implemented from
 * its published specification. Each window keeps the records its program
 * reports, shows the most urgent of them in its frame, and, because the
 * protocol carries any number of records per terminal in a hierarchy of
 * ids, exports them to every attached terminal that answers the feature
 * query, under an id of the window's own (w<id>/...).
 */
#pragma once

#include "dv.hpp"

#include <string>
#include <vector>

/* States, in increasing urgency: the frame shows the most urgent one. */
enum {
	DV_PS_IDLE = 0,
	DV_PS_DONE,
	DV_PS_WORKING,
	DV_PS_ERROR,
	DV_PS_BLOCKED,
};

/* kind= of a blocked record. */
enum {
	DV_PS_KIND_NONE = 0,
	DV_PS_KIND_PERMISSION,
	DV_PS_KIND_QUESTION,
	DV_PS_KIND_AUTH,
};

struct dv_pstatus_rec {
	std::string		 id;		/* "" is the root record */
	int			 state = DV_PS_IDLE;
	int			 kind = DV_PS_KIND_NONE;
	int			 progress = -1;	/* 0-100, -1 absent: indeterminate */
	std::string		 app;		/* as reported, "" absent */
	std::string		 title, msg;	/* decoded UTF-8 */
	std::string		 title_b64, msg_b64;	/* as sent, for export */
	unsigned long long	 seq = 0;	/* when it was last reported */
	bool			 osc9 = false;	/* made from OSC 9;4, not a report */
};

struct dv_pstatus {
	std::vector<struct dv_pstatus_rec> recs;
	bool			 real = false;	/* a 7501 report since the last RIS */
	bool			 focused = false;	/* as last seen by dv_pstatus_focus */
};

/* What a window's frame shows: its most urgent record. */
struct dv_pstatus_view {
	int			 state = -1;	/* -1: nothing to show */
	int			 kind = DV_PS_KIND_NONE;
	int			 progress = -1;
	unsigned		 count = 0;	/* records in that state */
	std::string		 text;		/* app/title and msg, for the bottom border */
};

/* OSC 7501 from the program; body is what follows "7501;". */
void		 dv_pstatus_osc(struct dv_window *, const char *body, size_t n);
/* OSC 9;4 (ConEmu progress); args is what follows "4", ";" included. */
void		 dv_pstatus_progress(struct dv_window *, const char *args,
		    size_t n);
void		 dv_pstatus_prompt(struct dv_window *);	/* OSC 133;A */
void		 dv_pstatus_reset(struct dv_window *);	/* RIS */
void		 dv_pstatus_key(struct dv_window *);	/* the user typed in it */
void		 dv_pstatus_focus(struct dv_session *);	/* before each frame */
void		 dv_pstatus_close(struct dv_window *);	/* the window is going */
void		 dv_pstatus_attach(struct dv_peer *);	/* a terminal was attached */
void		 dv_pstatus_session_gone(struct dv_session *);
int		 dv_pstatus_view_of(const struct dv_window *,
		    struct dv_pstatus_view *);

/* For tests and the state handover. */
size_t		 dv_pstatus_count(const struct dv_window *);
const struct dv_pstatus_rec *dv_pstatus_find(const struct dv_window *,
		    const char *id);
/* The state handover across an upgrade: one line per record, in order. */
std::vector<std::string> dv_pstatus_save(const struct dv_window *);
void		 dv_pstatus_restore(struct dv_window *, const char *line);

/* The report that recreates a record, without OSC and ST. */
std::string	 dv_pstatus_report(const struct dv_pstatus_rec *, const char *id);
