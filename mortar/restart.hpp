#pragma once

#include "scratchpad.hpp"
#include "workspaces.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mortar {

inline constexpr std::size_t restart_max_bytes = 1024 * 1024;
inline constexpr std::size_t restart_max_nodes = 16384;
inline constexpr std::size_t restart_max_depth = 128;

struct RestartSnapshot {
    WorkspacesSnapshot workspaces;
    std::vector<ScratchpadEntry> scratchpad;
    bool operator==(const RestartSnapshot &) const = default;
};

void validate_restart_snapshot(const RestartSnapshot &snapshot);
std::vector<std::uint8_t> encode_restart_snapshot(const RestartSnapshot &snapshot);
RestartSnapshot decode_restart_snapshot(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> encode_layout_snapshot(const LayoutSnapshot &snapshot);
LayoutSnapshot decode_layout_snapshot(std::span<const std::uint8_t> bytes);

}
