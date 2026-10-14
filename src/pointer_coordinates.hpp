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
} }
