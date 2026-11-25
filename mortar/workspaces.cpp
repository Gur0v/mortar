#include "workspaces.hpp"

#include <algorithm>

namespace mortar {

int Workspaces::owner(std::uint32_t window) const {
    for (std::size_t i = 0; i < workspaces.size(); ++i)
        if (workspaces[i].layout.contains(window)) return static_cast<int>(i);
    return -1;
}

bool Workspaces::switch_to(std::uint32_t destination) {
    if (destination >= workspaces.size() || destination == current) return false;
    current = destination;
    return true;
}

bool Workspaces::move(std::uint32_t window, std::uint32_t destination) {
    if (destination >= workspaces.size()) return false;
    int from = owner(window);
    if (from < 0 || static_cast<std::uint32_t>(from) == destination) return false;
    auto &source = workspaces[from];
    auto &target = workspaces[destination];
    if (!target.layout.insert(Layout::leaf(window))) return false;
    source.layout.remove(window);
    std::erase(source.windows, window);
    if (source.focused == window) source.focused = source.windows.empty() ? 0 : source.windows.back();
    target.windows.push_back(window);
    if (destination != current || !target.focused) target.focused = window;
    return true;
}

}
