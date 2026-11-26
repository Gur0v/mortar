#pragma once

#include "layout.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace mortar {

struct WorkspaceSnapshot {
    LayoutSnapshot layout;
    std::vector<std::uint32_t> windows;
    std::vector<std::uint32_t> floating;
    std::uint32_t focused;
    bool operator==(const WorkspaceSnapshot &) const = default;
};

struct WorkspacesSnapshot {
    std::array<WorkspaceSnapshot, 10> workspaces;
    std::uint32_t current;
    std::optional<std::uint32_t> previous;
    bool operator==(const WorkspacesSnapshot &) const = default;
};

struct Workspace {
    Layout layout = Layout::split(Split::horizontal);
    std::vector<std::uint32_t> windows;
    std::vector<std::uint32_t> floating;
    std::uint32_t focused = 0;

    bool add_floating(std::uint32_t window);
    bool remove_floating(std::uint32_t window);
    bool toggle_floating(std::uint32_t window);
    std::uint32_t tiled_focus() const;
};

struct Workspaces {
    std::array<Workspace, 10> workspaces;
    std::uint32_t current = 0;
    std::optional<std::uint32_t> previous;

    WorkspacesSnapshot snapshot() const;
    static Workspaces restore(const WorkspacesSnapshot &snapshot);
    std::uint32_t adjacent(bool forward, const std::array<bool, 10> &visible) const;
    int owner(std::uint32_t window) const;
    bool switch_to(std::uint32_t destination);
    bool move(std::uint32_t window, std::uint32_t destination);
};

}
