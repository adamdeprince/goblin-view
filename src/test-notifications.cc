/* Copyright (c) 2026 Adam DePrince. ISC license; see dv.hpp. */
#include "notification_route.hpp"
#include <cassert>
#include <iostream>
using namespace odin_notification;
static std::string body(std::string bytes) { return bytes.substr(5, bytes.size() - 7); }
int main() {
    const std::string a="sessionA", b="sessionB";
    auto inner=forward(a,3,"9;foo is done",{});
    assert(inner.handled && inner.bytes.find("r=odin-seat/sessionA/3")!=std::string::npos);
    auto outer=forward(b,8,inner.bytes.substr(2,inner.bytes.size()-4),{});
    auto data=body(outer.bytes);
    auto meta=fields(data.substr(0,data.find(';')));
    assert(value(meta,"r")=="odin-seat/sessionA/3,odin-seat/sessionB/8");
    meta.emplace_back("r",value(meta,"r")+",silkie/host/12");
    meta.emplace_back("a","focus,report");
    auto first=reply(b,serialize(meta)+";0");
    assert(first.handled && first.window==8 && first.focus && first.deliver);
    auto second=reply(a,body(first.bytes));
    assert(second.handled && second.window==3 && second.focus && !second.deliver);
    assert(reply("different",serialize(meta)+";0").window==0);
    // Rich callers keep their own IDs, button and the complete route.
    auto native=forward(a,3,"99;i=foo:a=report;done",{});
    auto native_data=body(native.bytes);
    auto native_meta=fields(native_data.substr(0,native_data.find(';')));
    native_meta.emplace_back("a","report");
    auto clicked=reply(a,serialize(native_meta)+";2");
    assert(clicked.window==3 && !clicked.focus && clicked.deliver);
    assert(value(fields(body(clicked.bytes).substr(0,body(clicked.bytes).find(';'))),"i")=="foo");
    assert(clicked.bytes.find(";2\033\\")!=std::string::npos);
    auto muted=without(native_meta,"a");muted.emplace_back("a","");
    clicked=reply(a,serialize(muted)+";0");
    assert(!clicked.focus && !clicked.deliver);
    auto many=forward(a,3,"777;notify;Codex;Need an answer",{});
    assert(many.bytes.find(":p=body;")!=std::string::npos && many.bytes.find("Need an answer")!=std::string::npos);
    auto anon=forward(a,3,"99;d=0;part1",{});
    auto anon2=forward(a,3,"99;d=1;part2",anon.pending);
    auto id1=value(fields(body(anon.bytes).substr(0,body(anon.bytes).find(';'))),"i");
    auto id2=value(fields(body(anon2.bytes).substr(0,body(anon2.bytes).find(';'))),"i");
    assert(id1==id2 && anon2.pending.empty());
    // Self-routing addresses never evict old notifications or truncate IDs.
    std::vector<std::string> replies;
    std::string longid(16384,'x');
    for(unsigned i=1;i<=2048;i++) {
        auto request=forward(a,i,"99;i="+longid+":a=report;done",{});
        auto d=body(request.bytes);auto m=fields(d.substr(0,d.find(';')));
        replies.push_back(serialize(m)+";0");
    }
    for(unsigned i=0;i<replies.size();i++) {
        auto response=reply(a,replies[i]);
        assert(response.window==i+1 && response.deliver);
        assert(value(fields(body(response.bytes).substr(0,body(response.bytes).find(';'))),"i")==longid);
    }
    // Alive replies expose only the requesting window's IDs.
    auto other=forward(a,4,"99;i=foo;done",{});
    auto other_data=body(other.bytes);auto other_meta=fields(other_data.substr(0,other_data.find(';')));
    auto alive=without(native_meta,"p");alive.emplace_back("p","alive");
    auto response=reply(a,serialize(alive)+';'+value(native_meta,"i")+','+value(other_meta,"i"));
    assert(response.bytes.ends_with(";foo\033\\"));
    std::cout<<"test-notifications: ok\n";
}
