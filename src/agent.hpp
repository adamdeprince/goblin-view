/* Copyright (c) 2026 Adam DePrince. SPDX-License-Identifier: ISC */
#pragma once
#include "dv.hpp"
#include <poll.h>
#include <vector>
#include <string>

const char *dv_agent_path(dv_session *);
void dv_agent_attach(dv_peer *);
void dv_agent_detach(dv_peer *);
bool dv_agent_reply(dv_peer *, const char *, size_t);
void dv_agent_poll(std::vector<pollfd> &);
void dv_agent_tick();
/* Milliseconds until the bridges next need a tick; -1 when nothing is due. */
int dv_agent_wait_ms();
void dv_agent_free(dv_session *);
void dv_agent_window_closed(dv_window *);
int dv_agent_listener(dv_session *);
int dv_agent_peer(dv_session *);
void dv_agent_restore(dv_session *, int, const char *, int);
void dv_agent_quiesce(dv_session *);
const char *dv_agent_scope(dv_session *);
void dv_agent_restore_scope(dv_session *, const char *);
std::string dv_agent_peers(dv_session *);
void dv_agent_restore_peers(dv_session *, const char *);
