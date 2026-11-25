#include "scratchpad.hpp"

#include <algorithm>

namespace mortar {

bool Scratchpad::add(std::uint32_t window, std::uint32_t workspace, std::optional<Rect> geometry) {
    if (window == 0 || get(window)) return false;
    entries_.push_back({window, workspace, geometry, std::nullopt});
    return true;
}

bool Scratchpad::remove(std::uint32_t window) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [window](const auto &entry) { return entry.window == window; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

bool Scratchpad::update_geometry(std::uint32_t window, Rect geometry) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [window](const auto &entry) { return entry.window == window; });
    if (it == entries_.end()) return false;
    it->geometry = geometry;
    return true;
}

std::optional<ScratchpadEntry> Scratchpad::get(std::uint32_t window) const {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [window](const auto &entry) { return entry.window == window; });
    if (it == entries_.end()) return std::nullopt;
    return *it;
}

std::optional<ScratchpadResult> Scratchpad::toggle(std::uint32_t workspace, std::uint32_t focused,
                                                   std::optional<std::uint32_t> target) {
    auto it = entries_.end();
    if (target) {
        it = std::find_if(entries_.begin(), entries_.end(),
                          [target](const auto &entry) { return entry.window == *target; });
        if (it == entries_.end()) return std::nullopt;
    } else {
        it = std::find_if(entries_.begin(), entries_.end(), [focused, workspace](const auto &entry) {
            return entry.window == focused && entry.visible_workspace == workspace;
        });
        if (it == entries_.end()) {
            it = std::find_if(entries_.begin(), entries_.end(), [workspace](const auto &entry) {
                return entry.visible_workspace == workspace;
            });
            if (it != entries_.end()) return ScratchpadResult{ScratchpadAction::focus, *it};
            it = std::find_if(entries_.begin(), entries_.end(), [](const auto &entry) {
                return entry.visible_workspace.has_value();
            });
            if (it == entries_.end()) {
                it = std::find_if(entries_.begin(), entries_.end(), [](const auto &entry) {
                    return !entry.visible_workspace;
                });
            }
        }
    }
    if (it == entries_.end()) return std::nullopt;
    if (it->visible_workspace == workspace) {
        it->visible_workspace.reset();
        auto entry = *it;
        std::rotate(it, it + 1, entries_.end());
        return ScratchpadResult{ScratchpadAction::hide, entry};
    }
    auto action = it->visible_workspace ? ScratchpadAction::move : ScratchpadAction::show;
    it->visible_workspace = workspace;
    return ScratchpadResult{action, *it};
}

}
