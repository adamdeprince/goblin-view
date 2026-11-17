#pragma once
#include "goblin_pointer.hpp"
#include <algorithm>
#include <cstdint>
namespace goblin { namespace pointer {
inline int window_coordinate(int value, int viewport, int origin, int scroll,
                             unsigned host_cell, unsigned app_cell) {
    const int64_t local = int64_t(value) + int64_t(viewport-origin+scroll)*host_cell*unit;
    return int(std::clamp<int64_t>(local*app_cell/std::max(1u,host_cell),-1073741823,1073741823));
}
inline int cell_at(int value, unsigned size) {
    const int64_t divisor=int64_t(std::max(1u,size))*unit;
    return int(value >= 0 ? value/divisor : -((-int64_t(value)+divisor-1)/divisor));
}
/* One axis of an SGR pixel report (mode 1016), 1-based, matching the
 * translation of a Goblin pointer event. `cell` is the 1-based window
 * cell. `sub` is the 0-based pixel inside the host cell, or -1 when the
 * host only named the cell: the report is then that cell's centre, in
 * the numbering used before pixel positions were available. */
inline unsigned sgr_pixel(int cell, int sub, unsigned host_cell, unsigned app_cell,
                          unsigned limit_cells) {
    if (app_cell == 0)
        app_cell = 1;
    if (cell < 1)
        cell = 1;
    const unsigned maxp = limit_cells * app_cell;
    if (sub < 0 || host_cell == 0) {
        unsigned v = (unsigned)(cell - 1) * app_cell + app_cell / 2;
        if (maxp && v > maxp)
            v = maxp;
        return v;
    }
    if ((unsigned)sub >= host_cell)
        sub = (int)host_cell - 1;
    unsigned off = host_cell == app_cell ? (unsigned)sub
        : (unsigned)((uint64_t)sub * app_cell / host_cell);
    if (off >= app_cell)
        off = app_cell - 1;
    uint64_t v = (uint64_t)(cell - 1) * app_cell + off + 1;
    if (v < 1)
        v = 1;
    if (maxp && v > maxp)
        v = maxp;
    return (unsigned)v;
}
} }
