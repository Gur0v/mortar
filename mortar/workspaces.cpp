#include "workspaces.hpp"

#include <algorithm>

namespace mortar {

std::uint32_t Workspace::tiled_focus() const {
    if (layout.contains(focused)) return focused;
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if (layout.contains(*it)) return *it;
    return 0;
}

std::uint32_t Workspace::focus_mode_target() const {
    if (!focused) return 0;
    const bool to_floating = layout.contains(focused);
    for (auto it = focus_history.rbegin(); it != focus_history.rend(); ++it)
        if (to_floating ? std::find(floating.begin(), floating.end(), *it) != floating.end() : layout.contains(*it)) return *it;
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if (to_floating ? std::find(floating.begin(), floating.end(), *it) != floating.end() : layout.contains(*it)) return *it;
    return 0;
}

bool Workspace::add_floating(std::uint32_t window) {
    if (!window || layout.contains(window) ||
        std::find(floating.begin(), floating.end(), window) != floating.end()) return false;
    floating.push_back(window);
    windows.push_back(window);
    if (!focused) focused = window;
    return true;
}

bool Workspace::remove_floating(std::uint32_t window) {
    if (!std::erase(floating, window)) return false;
    std::erase(windows, window);
    std::erase(focus_history, window);
    if (focused == window) focused = windows.empty() ? 0 : windows.back();
    return true;
}

bool Workspace::toggle_floating(std::uint32_t window) {
    if (layout.contains(window)) {
        floating.push_back(window);
        layout.remove(window);
        return true;
    }
    auto it = std::find(floating.begin(), floating.end(), window);
    if (it == floating.end() || !layout.insert_after(tiled_focus(), Layout::leaf(window))) return false;
    floating.erase(it);
    return true;
}

int Workspaces::owner(std::uint32_t window) const {
    for (std::size_t i = 0; i < workspaces.size(); ++i)
        if (workspaces[i].layout.contains(window) ||
            std::find(workspaces[i].floating.begin(), workspaces[i].floating.end(), window) !=
                workspaces[i].floating.end()) return static_cast<int>(i);
    return -1;
}

std::uint32_t Workspaces::adjacent(bool forward, const std::vector<bool> &visible) const {
    for (std::size_t step = 1; step < workspaces.size(); ++step) {
        auto index = (current + (forward ? step : workspaces.size() - step)) % workspaces.size();
        if ((index < visible.size() && visible[index]) || !workspaces[index].windows.empty()) return static_cast<std::uint32_t>(index);
    }
    return current;
}

bool Workspaces::ensure(std::uint32_t index) {
    if (index >= 16384) return false;
    if (index >= workspaces.size()) workspaces.resize(static_cast<std::size_t>(index) + 1);
    return true;
}

bool Workspaces::switch_to(std::uint32_t destination) {
    if (destination >= workspaces.size() || destination == current) return false;
    previous = current;
    current = destination;
    return true;
}

bool Workspaces::move(std::uint32_t window, std::uint32_t destination) {
    if (destination >= workspaces.size()) return false;
    int from = owner(window);
    if (from < 0 || static_cast<std::uint32_t>(from) == destination) return false;
    auto &source = workspaces[from];
    auto &target = workspaces[destination];
    bool floating = std::find(source.floating.begin(), source.floating.end(), window) != source.floating.end();
    if (floating) {
        target.floating.push_back(window);
        std::erase(source.floating, window);
    } else {
        if (!target.layout.insert_after(target.tiled_focus(), Layout::leaf(window))) return false;
        source.layout.remove(window);
    }
    std::erase(source.windows, window);
    std::erase(source.focus_history, window);
    if (source.focused == window) source.focused = source.windows.empty() ? 0 : source.windows.back();
    target.windows.push_back(window);
    if (destination != current || !target.focused) target.focused = window;
    return true;
}

}
