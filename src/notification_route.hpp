/* Copyright (c) 2026 Adam DePrince. ISC license; see dv.hpp. */
#pragma once

#include <cerrno>
#include <fcntl.h>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace odin_notification {
using Fields = std::vector<std::pair<std::string, std::string>>;
inline std::string nonce() {
    // A random 128-bit instance identity, not a notification capacity limit.
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot create notification scope");
    size_t have = 0;
    while (have < sizeof bytes) {
        ssize_t n = read(fd, bytes + have, sizeof bytes - have);
        if (n > 0) have += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { close(fd); throw std::runtime_error("cannot read notification identity entropy"); }
    }
    close(fd);
    std::string out;
    for (unsigned char c : bytes) {
        out += "0123456789abcdef"[c >> 4];
        out += "0123456789abcdef"[c & 15];
    }
    return out;
}
inline Fields fields(std::string_view text) {
    Fields out;
    while (!text.empty()) {
        auto end = text.find(':');
        auto part = text.substr(0, end);
        if (!part.empty()) {
            auto equals = part.find('=');
            if (equals == part.npos) throw std::invalid_argument("invalid notification metadata");
            out.emplace_back(part.substr(0, equals), part.substr(equals + 1));
        }
        if (end == text.npos) break;
        text.remove_prefix(end + 1);
    }
    return out;
}
inline std::string value(const Fields &metadata, std::string_view key) {
    for (auto i = metadata.rbegin(); i != metadata.rend(); ++i)
        if (i->first == key) return i->second;
    return {};
}
inline std::string serialize(const Fields &metadata) {
    std::string out;
    for (const auto &[key, val] : metadata) {
        if (!out.empty()) out += ':';
        out += key + '=' + val;
    }
    return out;
}
inline Fields without(const Fields &metadata, std::string_view a, std::string_view b = {}) {
    Fields out;
    for (const auto &field : metadata)
        if (field.first != a && field.first != b) out.push_back(field);
    return out;
}
inline std::string frame(const Fields &metadata, std::string_view payload) {
    return "\033]99;" + serialize(metadata) + ';' + std::string(payload) + "\033\\";
}
inline bool action(std::string_view text, std::string_view wanted) {
    while (!text.empty()) {
        auto end = text.find(',');
        if (text.substr(0, end) == wanted) return true;
        if (end == text.npos) break;
        text.remove_prefix(end + 1);
    }
    return false;
}
struct Forward { bool handled = false; std::string bytes, pending; };
inline Forward forward(std::string_view scope, unsigned window, std::string_view raw, std::string_view pending) {
    Forward out;
    bool converted = false;
    std::string_view title, body;
    Fields metadata;
    if (raw.starts_with("9;")) { title = raw.substr(2); converted = true; }
    else if (raw.starts_with("777;notify;")) {
        auto payload = raw.substr(11);
        auto split = payload.find(';');
        title = payload.substr(0, split);
        if (split != payload.npos) body = payload.substr(split + 1);
        converted = true;
    } else if (raw.starts_with("99;")) {
        raw.remove_prefix(3);
        auto split = raw.find(';');
        metadata = fields(raw.substr(0, split));
        if (split != raw.npos) title = raw.substr(split + 1);
    } else return out;
    out.handled = true;
    std::string id = value(metadata, "i");
    const bool anonymous = id.empty();
    char kind = converted ? 'c' : anonymous ? 'u' : 'n';
    if (anonymous) id = !pending.empty() && !converted ? std::string(pending) : nonce();
    const std::string prefix = "odin." + std::string(scope) + '.' + std::to_string(window) + '.' + kind + '.';
    auto route = value(metadata, "r");
    if (!route.empty()) route += ',';
    route += "odin-seat/" + std::string(scope) + '/' + std::to_string(window);
    auto result = without(metadata, "i", "r");
    result.emplace_back("i", prefix + id);
    result.emplace_back("r", route);
    if (converted) {
        result.emplace_back("a", "focus,report");
        if (!body.empty()) {
            result.emplace_back("d", "0");
            out.bytes = frame(result, title);
            result.pop_back();
            result.emplace_back("p", "body");
        }
    } else if (anonymous && value(metadata, "d") == "0") out.pending = id;
    out.bytes += frame(result, converted && !body.empty() ? body : title);
    return out;
}
struct Address { bool valid = false; unsigned window = 0; char kind = 0; std::string original; };
inline Address address(std::string_view scope, std::string_view id) {
    Address out;
    auto prefix = "odin." + std::string(scope) + '.';
    if (!id.starts_with(prefix)) return out;
    id.remove_prefix(prefix.size());
    auto split = id.find('.');
    if (split == id.npos) return out;
    auto number = id.substr(0, split);
    auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), out.window);
    if (error != std::errc() || end != number.data() + number.size() || !out.window) return out;
    id.remove_prefix(split + 1);
    if (id.size() < 2 || id[1] != '.' || (id[0] != 'n' && id[0] != 'c' && id[0] != 'u')) return out;
    out.kind = id[0];
    out.original = id.substr(2);
    out.valid = true;
    return out;
}
struct Reply { bool handled = false, focus = false, deliver = false; unsigned window = 0; std::string bytes; };
inline Reply reply(std::string_view scope, std::string_view body) {
    Reply out;
    auto split = body.find(';');
    auto metadata = fields(body.substr(0, split));
    auto destination = address(scope, value(metadata, "i"));
    // A routed reply for a different/stale scope is never guessed onto focus.
    if (!destination.valid) { out.handled = value(metadata, "i").starts_with("odin."); return out; }
    out.handled = true;
    out.window = destination.window;
    auto payload = split == body.npos ? std::string_view{} : body.substr(split + 1);
    const auto type = value(metadata, "p");
    const bool activated = type.empty();
    const auto actions = value(metadata, "a");
    bool has_actions = false;
    for (const auto &field : metadata) if (field.first == "a") has_actions = true;
    out.focus = activated && (action(actions, "focus") || (!has_actions && destination.kind == 'c'));
    out.deliver = destination.kind != 'c' && (!activated || !has_actions || action(actions, "report") || destination.original.starts_with("odin."));
    auto result = without(metadata, "i");
    result.emplace_back("i", destination.kind == 'u' ? "0" : destination.original);
    std::string live;
    if (type == "alive") {
        while (!payload.empty()) {
            auto end = payload.find(',');
            auto id = address(scope, payload.substr(0, end));
            if (id.valid && id.window == destination.window) {
                if (!live.empty()) live += ',';
                live += id.kind == 'u' ? "0" : id.original;
            }
            if (end == payload.npos) break;
            payload.remove_prefix(end + 1);
        }
        payload = live;
    }
    out.bytes = frame(result, payload);
    return out;
}
} // namespace odin_notification
