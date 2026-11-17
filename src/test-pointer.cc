#include "dv.hpp"
#include "pointer_coordinates.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <unistd.h>
namespace gp=goblin::pointer;
static void send(dv_window *w,const std::string &s){for(char c:s)dv_vt_write(w,&c,1);}
int main(){
    auto *w=dv_new<dv_window>(); auto *session=dv_new<dv_session>();
    dv_screen_init(&w->screen,60,20); w->session=session;w->pty_fd=999999;
    session->desktop.xpixel=10;session->desktop.ypixel=20;session->pointer_caps=7;
    send(w,gp::mode(true));assert(w->screen.pointer_mode);
    gp::Message query;query.op=gp::Op::Query;query.id=12;send(w,gp::encode(query));
    gp::Message cap;assert(gp::parse(std::string_view(reinterpret_cast<char*>(w->pty_out.data),w->pty_out.len),cap));
    assert(cap.caps==7 && cap.w==600 && cap.h==400);
    w->pty_out.len=0;
    gp::Message m;m.op=gp::Op::Event;m.phase=gp::Down;m.kind=gp::Pen;m.tool=2;m.valid=gp::Pressure|gp::Tilt;m.p=32768;m.tx=-1200;
    dv_pointer_send(w,m);assert(w->npointer==1);w->pty_out.len=0;
    send(w,gp::mode(false));assert(!w->npointer && !w->screen.pointer_mode);
    assert(gp::parse(std::string_view(reinterpret_cast<char*>(w->pty_out.data),w->pty_out.len),cap));
    assert(cap.phase==gp::Cancel && !cap.p && cap.tool==2 && cap.tx==-1200);
    assert(gp::window_coordinate(123*256+17,5,9,2,10,10)==103*256+17);
    assert(gp::window_coordinate(50*256,0,10,0,10,10)==-50*256);
    assert(gp::window_coordinate(50*256,2,3,0,10,20)==80*256);
    assert(gp::cell_at(-1,10)==-1 && gp::cell_at(-2560,10)==-1 && gp::cell_at(-2561,10)==-2);
    assert(gp::sgr_pixel(1, -1, 10, 10, 5) == 5);
    assert(gp::sgr_pixel(1, 0, 10, 10, 5) == 1);
    assert(gp::sgr_pixel(5, 0, 10, 10, 20) == 41);
    assert(gp::sgr_pixel(5, 9, 10, 10, 20) == 50);
    assert(gp::sgr_pixel(1, 123, 256, 10, 5) == 5);
    send(w,gp::mode(true));send(w,"\033c");assert(!w->screen.pointer_mode);
    dv_screen_free(&w->screen);dv_buf_free(&w->pty_out);dv_buf_free(&w->cap);free(w);free(session);
    std::cout<<"Per-window pointer negotiation, reset/cancellation and coordinate transforms passed.\n";
}
