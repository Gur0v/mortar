#pragma once

#include "layout.hpp"

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
    std::vector<WorkspaceSnapshot> workspaces;
    std::uint32_t current;
    std::optional<std::uint32_t> previous;
    bool operator==(const WorkspacesSnapshot &) const = default;
};

struct Workspace {
    Layout layout = Layout::split(Split::horizontal);
    std::vector<std::uint32_t> windows;
    std::vector<std::uint32_t> floating;
    std::uint32_t focused = 0;
    std::vector<std::uint32_t> focus_history = {};

    bool add_floating(std::uint32_t window);
    bool remove_floating(std::uint32_t window);
    bool toggle_floating(std::uint32_t window);
    std::uint32_t tiled_focus() const;
    std::uint32_t focus_mode_target() const;
};

struct Workspaces {
    std::vector<Workspace> workspaces = std::vector<Workspace>(10);
    std::uint32_t current = 0;
    std::optional<std::uint32_t> previous;

    WorkspacesSnapshot snapshot() const;
    static Workspaces restore(const WorkspacesSnapshot &snapshot);
    std::uint32_t adjacent(bool forward, const std::vector<bool> &visible) const;
    bool ensure(std::uint32_t index);
    int owner(std::uint32_t window) const;
    bool switch_to(std::uint32_t destination);
    bool move(std::uint32_t window, std::uint32_t destination);
};

}
