/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Silkie extension protocols (OSC 777 and the SGR/graphics additions),
 * implemented from the published specifications. Nothing here is linked
 * from Silkie or from kitty.
 */
#pragma once

#include "dv.hpp"

const char	*dv_silkie_probe(void);

/* Host bootstrap: fold a cap reply into the identify blob. */
void		 dv_silkie_note(struct dv_host *, const unsigned char *, size_t);

/* Program OSC 777. Returns 1 if the selector is one we speak. */
int		 dv_silkie_program(struct dv_window *, const char *, size_t);

/* Host OSC 777. Returns 1 if it was ours (a cap reply or a routed event). */
int		 dv_silkie_from_host(struct dv_peer *, const char *, size_t);

/* first is the first display peer in the server list (NULL if none). */
void		 dv_silkie_recompute(struct dv_session *, struct dv_peer *first);
void		 dv_silkie_push(struct dv_session *);
void		 dv_silkie_attach(struct dv_peer *);
void		 dv_silkie_requery_display(struct dv_peer *);
void		 dv_silkie_cancel_window(struct dv_window *);
void		 dv_silkie_reset_pane(struct dv_window *);
void		 dv_silkie_focus(struct dv_session *, struct dv_window *prev);
void		 dv_silkie_output(struct dv_window *);
void		 dv_silkie_ink_peer_gone(struct dv_peer *);
int		 dv_silkie_gfx_ok(const struct dv_session *, long f, int G,
		    int medium);

/* The terminal's probe answered for this capability-blob key. */
int		 dv_silkie_peer_has(const struct dv_peer *, const char *key);

/* Strong definitions live in the server. Weak no-ops let unit tests link. */
void		 dv_host_passthrough(struct dv_session *, const char *, size_t);
void		 dv_mouse_inject(struct dv_peer *, unsigned vx, unsigned vy,
		    unsigned code, int press);
void		 dv_silkie_peers_changed(struct dv_session *);
