/* Copyright (c) 2026 Adam DePrince. SPDX-License-Identifier: ISC */
#include "agent.hpp"
#include "ssh_agent.hpp"
#include <memory>
#include <sstream>

using namespace terminal_agent;
namespace {
struct Pending { unsigned window; Fields original; uint64_t deadline; };
struct Bridge {
    Proxy proxy;
    std::string scope = nonce();
    std::vector<int> peers;
    std::map<std::string, Pending> pending;
    std::map<unsigned, std::map<std::string, std::string>> channels;
    Bridge() = default;
    Bridge(int fd, const char *path, int peer) : proxy(fd, path) { if (peer >= 0) peers.push_back(peer); }
};
std::map<dv_session *, std::unique_ptr<Bridge>> bridges;

Bridge &bridge(dv_session *s) {
    auto &value = bridges[s];
    if (!value) value = std::make_unique<Bridge>();
    return *value;
}
dv_peer *provider(dv_session *s, Bridge &b) {
    if (b.peers.empty()) return nullptr;
    dv_peer *p;
    TAILQ_FOREACH(p, &dv_srv->peers, entry)
        if (p->fd == b.peers.back() && p->session == s && p->display) return p;
    return nullptr;
}
void send(dv_peer *p, const std::string &bytes) {
    if (p) { dv_conn_send_agent(&p->conn, bytes.data(), bytes.size()); dv_conn_flush(&p->conn); }
}
void answer(dv_session *s, const Pending &pending, Fields f) {
    auto *w = dv_window_find(s, pending.window);
    if (!w || w->pty_fd < 0) return;
    f["id"] = get(pending.original, "id");
    if (f.count("c")) f["c"] = get(pending.original, "c");
    auto bytes = frame(f);
    dv_window_pty_send(w, bytes.data(), bytes.size());
}
void cancel(dv_session *s, Bridge &b) {
    auto *p = provider(s, b);
    b.proxy.cancel();
    for (const auto &bytes : b.proxy.take_output()) send(p, bytes);
    for (const auto &[window, channels] : b.channels)
        for (const auto &[channel, generation] : channels)
            send(p, frame({{"v","1"},{"op","close"},{"id",nonce()},
                {"c",b.scope + '.' + std::to_string(window) + '.' + channel},{"g",generation}}));
    b.channels.clear();
    for (const auto &[id, pending] : b.pending) answer(s, pending, error(pending.original, "changed"));
    b.pending.clear();
}
int from_app(dv_window *w, const char *data, size_t size) {
    Fields f;
    if (!parse(std::string(data, size), f)) return 1;
    auto op = get(f, "op");
    if (op != "query" && op != "request" && op != "close") return 1;
    auto &b = bridge(w->session);
    auto *p = provider(w->session, b);
    if (!p) {
        if (op != "close") answer(w->session, {w->id, f, 0}, error(f, "unavailable"));
        return 1;
    }
    auto forwarded = f;
    forwarded["id"] = nonce();
    if (op != "query") {
        if (!token(get(f, "c"))) return 1;
        forwarded["c"] = b.scope + '.' + std::to_string(w->id) + '.' + get(f, "c");
        if (op == "request") b.channels[w->id][get(f,"c")] = get(f,"g");
        else {
            auto channels = b.channels.find(w->id);
            if (channels != b.channels.end()) {
                channels->second.erase(get(f,"c"));
                if (channels->second.empty()) b.channels.erase(channels);
            }
        }
    }
    if (op != "close") b.pending.emplace(forwarded["id"], Pending{w->id, f, now_ms() + (op == "query" ? 5000 : 120000)});
    send(p, frame(forwarded));
    return 1;
}
}

const char *dv_agent_path(dv_session *s) {
    dv_agent_app_hook = from_app;
    return bridge(s).proxy.path().c_str();
}
void dv_agent_attach(dv_peer *p) {
    auto &b = bridge(p->session);
    cancel(p->session, b);
    std::erase(b.peers, p->fd);
    b.peers.push_back(p->fd);
}
void dv_agent_detach(dv_peer *p) {
    if (!p->session || !bridges.count(p->session)) return;
    auto &b = bridge(p->session);
    if (!b.peers.empty() && b.peers.back() == p->fd) cancel(p->session, b);
    std::erase(b.peers, p->fd);
}
bool dv_agent_reply(dv_peer *p, const char *data, size_t size) {
    std::string raw(data, size);
    auto normalized = raw.starts_with("\033]") ? raw.substr(2) : raw;
    if (!normalized.starts_with("777;ssh-agent;")) return false;
    Fields f;
    if (!parse(raw, f) || !p->session || !bridges.count(p->session)) return true;
    auto &b = bridge(p->session);
    if (provider(p->session, b) != p) return true;
    if (b.proxy.receive(raw, now_ms())) return true;
    auto i = b.pending.find(get(f, "id"));
    if (i != b.pending.end() && (get(f,"op") == "cap" || get(f,"op") == "reply")) {
        answer(p->session, i->second, f);
        b.pending.erase(i);
    }
    return true;
}
void dv_agent_poll(std::vector<pollfd> &out) {
    for (const auto &[s, b] : bridges) {
        auto fds = b->proxy.pollfds();
        out.insert(out.end(), fds.begin(), fds.end());
    }
}
void dv_agent_tick() {
    auto now = now_ms();
    for (auto &[s, b] : bridges) {
        b->proxy.tick(now);
        auto *p = provider(s, *b);
        for (const auto &bytes : b->proxy.take_output()) {
            if (p) send(p, bytes);
            else { Fields f; if (parse(bytes, f) && get(f,"op") != "close") b->proxy.receive(frame(error(f,"unavailable")), now); }
        }
        for (auto i = b->pending.begin(); i != b->pending.end();) {
            if (now < i->second.deadline) { ++i; continue; }
            answer(s, i->second, error(i->second.original, "timeout"));
            i = b->pending.erase(i);
        }
    }
}
int dv_agent_wait_ms() {
    auto now = now_ms();
    int delay = -1;
    auto sooner = [&delay](int ms) { if (ms >= 0 && (delay < 0 || ms < delay)) delay = ms; };
    for (const auto &[s, b] : bridges) {
        sooner(b->proxy.wait_ms(now));
        for (const auto &[id, p] : b->pending)
            sooner(int(std::min<uint64_t>(INT32_MAX, p.deadline > now ? p.deadline - now : 0)));
    }
    return delay;
}
void dv_agent_free(dv_session *s) { bridges.erase(s); }
void dv_agent_window_closed(dv_window *w) {
    if (!w->session || !bridges.count(w->session)) return;
    auto &b = bridge(w->session);
    auto i = b.channels.find(w->id);
    if (i != b.channels.end()) {
        for (const auto &[channel,generation] : i->second)
            send(provider(w->session,b), frame({{"v","1"},{"op","close"},{"id",nonce()},
                {"c",b.scope + '.' + std::to_string(w->id) + '.' + channel},{"g",generation}}));
        b.channels.erase(i);
    }
    for (auto p=b.pending.begin(); p!=b.pending.end();)
        if (p->second.window==w->id) p=b.pending.erase(p); else ++p;
}
int dv_agent_listener(dv_session *s) { auto i=bridges.find(s); return i==bridges.end() ? -1 : i->second->proxy.listener(); }
int dv_agent_peer(dv_session *s) { auto i=bridges.find(s); return i==bridges.end() || i->second->peers.empty() ? -1 : i->second->peers.back(); }
void dv_agent_restore(dv_session *s, int fd, const char *path, int peer) {
    bridges[s] = std::make_unique<Bridge>(fd, path, peer);
    dv_agent_app_hook = from_app;
}
void dv_agent_quiesce(dv_session *s) { auto i=bridges.find(s); if (i!=bridges.end()) cancel(s,*i->second); }
const char *dv_agent_scope(dv_session *s) { return bridge(s).scope.c_str(); }
void dv_agent_restore_scope(dv_session *s, const char *scope) { bridge(s).scope = scope; }
std::string dv_agent_peers(dv_session *s) {
    std::string out;
    for (int fd : bridge(s).peers) { if (!out.empty()) out += ' '; out += std::to_string(fd); }
    return out;
}
void dv_agent_restore_peers(dv_session *s, const char *peers) {
    auto &b = bridge(s); b.peers.clear();
    std::istringstream input(peers);
    for (int fd; input >> fd;) if (fd >= 0) b.peers.push_back(fd);
}
