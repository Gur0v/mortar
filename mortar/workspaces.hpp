#pragma once

#include "layout.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace mortar {

struct Workspace {
    Layout layout = Layout::split(Split::horizontal);
    std::vector<std::uint32_t> windows;
    std::uint32_t focused = 0;
};

struct Workspaces {
    std::array<Workspace, 10> workspaces;
    std::uint32_t current = 0;

    int owner(std::uint32_t window) const;
    bool switch_to(std::uint32_t destination);
    bool move(std::uint32_t window, std::uint32_t destination);
};

}
