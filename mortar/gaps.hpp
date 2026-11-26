#pragma once

#include "layout.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mortar {

inline Rect inset_gaps(Rect bounds, int left, int top, int right, int bottom) {
    if (bounds.width < 0 || bounds.height < 0 || left < 0 || top < 0 || right < 0 || bottom < 0 ||
        bounds.x > std::numeric_limits<int>::max() - bounds.width ||
        bounds.y > std::numeric_limits<int>::max() - bounds.height)
        throw std::invalid_argument("invalid gap geometry");
    left = std::min(left, bounds.width);
    right = std::min(right, bounds.width - left);
    top = std::min(top, bounds.height);
    bottom = std::min(bottom, bounds.height - top);
    return {bounds.x + left, bounds.y + top, bounds.width - left - right, bounds.height - top - bottom};
}

inline Rect outer_gap(Rect bounds, int gap) {
    return inset_gaps(bounds, gap, gap, gap, gap);
}

inline Rect inner_gap(Rect bounds, Rect workspace, int gap) {
    if (gap < 0 || workspace.width < 0 || workspace.height < 0 ||
        workspace.x > std::numeric_limits<int>::max() - workspace.width ||
        workspace.y > std::numeric_limits<int>::max() - workspace.height ||
        bounds.width < 0 || bounds.height < 0 || bounds.x < workspace.x || bounds.y < workspace.y ||
        bounds.x > std::numeric_limits<int>::max() - bounds.width ||
        bounds.y > std::numeric_limits<int>::max() - bounds.height ||
        bounds.x + bounds.width > workspace.x + workspace.width ||
        bounds.y + bounds.height > workspace.y + workspace.height)
        throw std::invalid_argument("invalid gap geometry");
    return inset_gaps(bounds, bounds.x == workspace.x ? gap : gap / 2,
                      bounds.y == workspace.y ? gap : gap / 2,
                      bounds.x + bounds.width == workspace.x + workspace.width ? gap : gap / 2,
                      bounds.y + bounds.height == workspace.y + workspace.height ? gap : gap / 2);
}

}
