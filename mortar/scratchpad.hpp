#pragma once

#include "layout.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mortar {

struct ScratchpadEntry {
    std::uint32_t window;
    std::uint32_t workspace;
    std::optional<Rect> geometry;
    std::optional<std::uint32_t> visible_workspace;
    bool operator==(const ScratchpadEntry &) const = default;
};

enum class ScratchpadAction { show, hide, focus, move };

struct ScratchpadResult {
    ScratchpadAction action;
    ScratchpadEntry entry;
};

class Scratchpad {
public:
    bool add(std::uint32_t window, std::uint32_t workspace, std::optional<Rect> geometry = std::nullopt);
    bool remove(std::uint32_t window);
    bool update_geometry(std::uint32_t window, Rect geometry);
    std::optional<ScratchpadEntry> get(std::uint32_t window) const;
    std::optional<ScratchpadResult> toggle(std::uint32_t workspace, std::uint32_t focused = 0,
                                           std::optional<std::uint32_t> target = std::nullopt);

private:
    std::vector<ScratchpadEntry> entries_;
};

}
