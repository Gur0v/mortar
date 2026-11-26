#include "restart.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace mortar {
namespace {

[[noreturn]] void invalid() { throw std::invalid_argument("invalid restart snapshot"); }

struct Codec {
    std::vector<std::uint8_t> output;
    std::span<const std::uint8_t> input;
    std::size_t position = 0;
    std::size_t nodes = 0;
    std::unordered_set<std::uint32_t> windows;

    void put(std::uint64_t value, unsigned count) {
        if (output.size() > restart_max_bytes - count) invalid();
        for (unsigned i = 0; i < count; ++i) output.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    std::uint64_t get(unsigned count) {
        if (count > input.size() - position) invalid();
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(input[position++]) << (8 * i);
        return value;
    }

    void validate(const LayoutSnapshot &node, std::size_t depth) {
        if (depth > restart_max_depth || ++nodes > restart_max_nodes ||
            (node.direction != Split::horizontal && node.direction != Split::vertical)) invalid();
        if (node.leaf) {
            if (!node.children.empty() || !node.focus_order.empty() || !node.weights.empty() || node.resized ||
                node.direction != Split::horizontal || !windows.insert(node.window).second) invalid();
            return;
        }
        const auto count = node.children.size();
        if (node.window != 0 || count > restart_max_nodes - nodes ||
            node.focus_order.size() != count || node.weights.size() != count) invalid();
        std::vector<bool> seen(count);
        for (auto index : node.focus_order) {
            if (index >= count || seen[index]) invalid();
            seen[index] = true;
        }
        double total = 0;
        for (double weight : node.weights) {
            if (!(weight > 0 && weight <= std::numeric_limits<double>::max())) invalid();
            total += weight;
        }
        if (total > std::numeric_limits<double>::max()) invalid();
    }

    void encode(const LayoutSnapshot &node, std::size_t depth) {
        validate(node, depth);
        put(node.leaf, 1);
        put(node.direction == Split::vertical, 1);
        put(node.resized, 1);
        put(node.window, 4);
        put(node.children.size(), 4);
        for (auto index : node.focus_order) put(index, 4);
        for (double weight : node.weights) put(std::bit_cast<std::uint64_t>(weight), 8);
        for (const auto &child : node.children) encode(child, depth + 1);
    }

    LayoutSnapshot decode(std::size_t depth) {
        if (depth > restart_max_depth || nodes >= restart_max_nodes) invalid();
        ++nodes;
        auto leaf = get(1);
        auto direction = get(1);
        auto resized = get(1);
        if (leaf > 1 || direction > 1 || resized > 1) invalid();
        auto window = static_cast<std::uint32_t>(get(4));
        auto count = static_cast<std::size_t>(get(4));
        if (count > restart_max_nodes - nodes || count > (input.size() - position) / 12) invalid();
        LayoutSnapshot node{leaf != 0, window, direction ? Split::vertical : Split::horizontal, {}, {}, {}, resized != 0};
        node.focus_order.reserve(count);
        node.weights.reserve(count);
        for (std::size_t i = 0; i < count; ++i) node.focus_order.push_back(static_cast<std::size_t>(get(4)));
        for (std::size_t i = 0; i < count; ++i) node.weights.push_back(std::bit_cast<double>(get(8)));
        node.children.reserve(count);
        for (std::size_t i = 0; i < count; ++i) node.children.push_back(decode(depth + 1));
        return node;
    }
};

static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559);

}

WorkspacesSnapshot Workspaces::snapshot() const {
    WorkspacesSnapshot result{};
    result.current = current;
    result.previous = previous;
    for (const auto &workspace : workspaces)
        result.workspaces.push_back({workspace.layout.snapshot(), workspace.windows, workspace.floating, workspace.focused});
    return result;
}

Workspaces Workspaces::restore(const WorkspacesSnapshot &snapshot) {
    RestartSnapshot validation{snapshot, {}};
    validation.owners.resize(snapshot.workspaces.size());
    validate_restart_snapshot(validation);
    Workspaces result;
    result.workspaces.resize(snapshot.workspaces.size());
    result.current = snapshot.current;
    result.previous = snapshot.previous;
    for (std::size_t i = 0; i < result.workspaces.size(); ++i) {
        const auto &source = snapshot.workspaces[i];
        result.workspaces[i] = {Layout::restore(source.layout), source.windows, source.floating, source.focused};
    }
    return result;
}

std::vector<ScratchpadEntry> Scratchpad::snapshot() const { return entries_; }

Scratchpad Scratchpad::restore(const std::vector<ScratchpadEntry> &snapshot) {
    if (snapshot.size() > restart_max_nodes) invalid();
    std::unordered_set<std::uint32_t> windows;
    for (const auto &entry : snapshot)
        if (!entry.window || !windows.insert(entry.window).second || entry.workspace >= restart_max_nodes ||
            (entry.visible_workspace && *entry.visible_workspace >= restart_max_nodes) ||
            (entry.geometry && (entry.geometry->width <= 0 || entry.geometry->height <= 0))) invalid();
    Scratchpad result;
    result.entries_ = snapshot;
    return result;
}

void validate_restart_snapshot(const RestartSnapshot &snapshot) {
    const auto &state = snapshot.workspaces;
    if (state.workspaces.empty() || state.workspaces.size() > restart_max_nodes ||
        snapshot.owners.size() != state.workspaces.size() || state.current >= state.workspaces.size() || (state.previous && *state.previous >= state.workspaces.size()) ||
        snapshot.scratchpad.size() > restart_max_nodes) invalid();
    if (!snapshot.workspace_names.empty() && snapshot.workspace_names.size() != state.workspaces.size()) invalid();
    std::unordered_set<std::string> workspace_names;
    for (const auto &name : snapshot.workspace_names)
        if (name.empty() || name.size() > 4096 || name.find('\0') != std::string::npos || !workspace_names.insert(name).second) invalid();
    Codec codec;
    std::unordered_set<std::uint32_t> all;
    for (const auto &workspace : state.workspaces) {
        auto before = codec.windows;
        codec.encode(workspace.layout, 0);
        std::unordered_set<std::uint32_t> members;
        if (workspace.windows.size() > restart_max_nodes || workspace.floating.size() > restart_max_nodes) invalid();
        for (auto window : workspace.windows)
            if (!window || !members.insert(window).second || !all.insert(window).second || all.size() > restart_max_nodes) invalid();
        std::unordered_set<std::uint32_t> floating;
        for (auto window : workspace.floating)
            if (!members.contains(window) || !floating.insert(window).second || codec.windows.contains(window)) invalid();
        for (auto window : codec.windows)
            if (!before.contains(window) && (!window || !members.contains(window))) invalid();
        if (members.size() != codec.windows.size() - before.size() + floating.size() ||
            (workspace.focused && !members.contains(workspace.focused)) ||
            (!workspace.focused && !members.empty())) invalid();
    }
    std::unordered_set<std::uint32_t> scratch;
    for (const auto &entry : snapshot.scratchpad) {
        if (!entry.window || !scratch.insert(entry.window).second || entry.workspace >= state.workspaces.size() ||
            (entry.visible_workspace && *entry.visible_workspace >= state.workspaces.size()) ||
            (entry.geometry && (entry.geometry->width <= 0 || entry.geometry->height <= 0))) invalid();
        if (entry.visible_workspace) {
            const auto &floating = state.workspaces[*entry.visible_workspace].floating;
            if (std::find(floating.begin(), floating.end(), entry.window) == floating.end()) invalid();
        } else if (all.contains(entry.window)) invalid();
    }
    std::unordered_set<std::string> names;
    std::unordered_set<std::uint32_t> visible;
    if (snapshot.outputs.size() > state.workspaces.size()) invalid();
    for (const auto &output : snapshot.outputs) {
        const auto &r = output.bounds;
        if (output.name.empty() || output.name.size() > 255 || !names.insert(output.name).second ||
            r.width <= 0 || r.height <= 0 || r.x > std::numeric_limits<int>::max() - r.width ||
            r.y > std::numeric_limits<int>::max() - r.height || output.workspace >= state.workspaces.size() ||
            !visible.insert(output.workspace).second || snapshot.owners[output.workspace] != output.name) invalid();
    }
    for (const auto &owner : snapshot.owners)
        if (owner && !names.contains(*owner)) invalid();
    if (snapshot.outputs.empty() != !snapshot.focused_output ||
        (snapshot.focused_output && !names.contains(*snapshot.focused_output))) invalid();
    if (snapshot.focused_output) {
        auto focused = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(), [&](const auto &output) {
            return output.name == *snapshot.focused_output;
        });
        if (focused->workspace != state.current) invalid();
    }
    auto validate_geometry = [&](const auto &entries, bool fullscreen) {
        std::unordered_set<std::uint32_t> seen;
        std::unordered_set<std::uint32_t> fullscreen_workspaces;
        if (entries.size() > restart_max_nodes) invalid();
        for (const auto &entry : entries) {
            if (!entry.window || !seen.insert(entry.window).second ||
                (!all.contains(entry.window) && (!scratch.contains(entry.window) || fullscreen)) ||
                entry.geometry.width <= 0 || entry.geometry.height <= 0 || entry.border > 65535) invalid();
            if (fullscreen) {
                for (std::size_t i = 0; i < state.workspaces.size(); ++i)
                    if (std::find(state.workspaces[i].windows.begin(), state.workspaces[i].windows.end(), entry.window) !=
                        state.workspaces[i].windows.end() && !fullscreen_workspaces.insert(i).second) invalid();
            }
        }
    };
    validate_geometry(snapshot.floating_geometry, false);
    validate_geometry(snapshot.fullscreen_geometry, true);
}

std::vector<std::uint8_t> encode_restart_snapshot(const RestartSnapshot &snapshot) {
    validate_restart_snapshot(snapshot);
    Codec codec;
    codec.put(0x3154524d, 4);
    codec.put(5, 4);
    codec.put(snapshot.workspaces.workspaces.size(), 4);
    codec.put(snapshot.workspaces.current, 4);
    codec.put(snapshot.workspaces.previous ? *snapshot.workspaces.previous + 1 : 0, 4);
    for (const auto &workspace : snapshot.workspaces.workspaces) {
        codec.encode(workspace.layout, 0);
        codec.put(workspace.windows.size(), 4);
        for (auto window : workspace.windows) codec.put(window, 4);
        codec.put(workspace.floating.size(), 4);
        for (auto window : workspace.floating) codec.put(window, 4);
        codec.put(workspace.focused, 4);
    }
    codec.put(snapshot.scratchpad.size(), 4);
    for (const auto &entry : snapshot.scratchpad) {
        codec.put(entry.window, 4);
        codec.put(entry.workspace, 4);
        codec.put(entry.visible_workspace ? *entry.visible_workspace + 1 : 0, 4);
        codec.put(entry.geometry.has_value(), 1);
        if (entry.geometry) {
            codec.put(std::bit_cast<std::uint32_t>(entry.geometry->x), 4);
            codec.put(std::bit_cast<std::uint32_t>(entry.geometry->y), 4);
            codec.put(std::bit_cast<std::uint32_t>(entry.geometry->width), 4);
            codec.put(std::bit_cast<std::uint32_t>(entry.geometry->height), 4);
        }
    }
    auto put_string = [&](const std::optional<std::string> &value) {
        codec.put(value ? value->size() : 0, 4);
        if (value) for (unsigned char c : *value) codec.put(c, 1);
    };
    auto put_rect = [&](const Rect &rect) {
        for (int value : {rect.x, rect.y, rect.width, rect.height})
            codec.put(std::bit_cast<std::uint32_t>(value), 4);
    };
    codec.put(snapshot.outputs.size(), 4);
    for (const auto &output : snapshot.outputs) {
        put_string(output.name);
        put_rect(output.bounds);
        codec.put(output.workspace, 4);
    }
    for (const auto &owner : snapshot.owners) put_string(owner);
    put_string(snapshot.focused_output);
    auto put_geometry = [&](const auto &entries) {
        codec.put(entries.size(), 4);
        for (const auto &entry : entries) {
            codec.put(entry.window, 4);
            put_rect(entry.geometry);
            codec.put(entry.border, 4);
        }
    };
    put_geometry(snapshot.floating_geometry);
    put_geometry(snapshot.fullscreen_geometry);
    codec.put(snapshot.workspace_names.size(), 4);
    for (const auto &name : snapshot.workspace_names) put_string(name);
    return codec.output;
}

RestartSnapshot decode_restart_snapshot(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > restart_max_bytes) invalid();
    Codec codec;
    codec.input = bytes;
    if (codec.get(4) != 0x3154524d) invalid();
    auto version = codec.get(4);
    if (version != 3 && version != 4 && version != 5) invalid();
    auto workspace_count = version == 3 ? 10 : codec.get(4);
    if (!workspace_count || workspace_count > restart_max_nodes || workspace_count > (bytes.size() - codec.position) / 23) invalid();
    RestartSnapshot result{};
    result.workspaces.workspaces.resize(workspace_count);
    result.owners.resize(workspace_count);
    result.workspaces.current = static_cast<std::uint32_t>(codec.get(4));
    auto previous = codec.get(4);
    if (previous) result.workspaces.previous = static_cast<std::uint32_t>(previous - 1);
    auto read_windows = [&] {
        auto count = codec.get(4);
        if (count > restart_max_nodes || count > (bytes.size() - codec.position) / 4) invalid();
        std::vector<std::uint32_t> windows;
        windows.reserve(count);
        for (std::size_t i = 0; i < count; ++i) windows.push_back(static_cast<std::uint32_t>(codec.get(4)));
        return windows;
    };
    for (auto &workspace : result.workspaces.workspaces) {
        workspace.layout = codec.decode(0);
        workspace.windows = read_windows();
        workspace.floating = read_windows();
        workspace.focused = static_cast<std::uint32_t>(codec.get(4));
    }
    auto count = codec.get(4);
    if (count > restart_max_nodes || count > (bytes.size() - codec.position) / 13) invalid();
    result.scratchpad.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ScratchpadEntry entry{};
        entry.window = static_cast<std::uint32_t>(codec.get(4));
        entry.workspace = static_cast<std::uint32_t>(codec.get(4));
        auto visible = codec.get(4);
        if (visible) entry.visible_workspace = static_cast<std::uint32_t>(visible - 1);
        auto geometry = codec.get(1);
        if (geometry > 1) invalid();
        if (geometry) {
            auto read_int = [&] { return std::bit_cast<int>(static_cast<std::uint32_t>(codec.get(4))); };
            entry.geometry = Rect{read_int(), read_int(), read_int(), read_int()};
        }
        result.scratchpad.push_back(entry);
    }
    auto read_string = [&]() -> std::optional<std::string> {
        auto size = codec.get(4);
        if (size > 255 || size > bytes.size() - codec.position) invalid();
        if (!size) return std::nullopt;
        std::string value;
        value.reserve(size);
        for (std::size_t i = 0; i < size; ++i) value.push_back(static_cast<char>(codec.get(1)));
        return value;
    };
    auto read_rect = [&] {
        auto read_int = [&] { return std::bit_cast<int>(static_cast<std::uint32_t>(codec.get(4))); };
        return Rect{read_int(), read_int(), read_int(), read_int()};
    };
    auto output_count = codec.get(4);
    if (output_count > result.owners.size()) invalid();
    for (std::size_t i = 0; i < output_count; ++i) {
        auto name = read_string();
        if (!name) invalid();
        auto bounds = read_rect();
        auto workspace = static_cast<std::uint32_t>(codec.get(4));
        result.outputs.push_back({*name, bounds, workspace});
    }
    for (auto &owner : result.owners) owner = read_string();
    result.focused_output = read_string();
    auto read_geometry = [&](auto &entries) {
        auto size = codec.get(4);
        if (size > restart_max_nodes || size > (bytes.size() - codec.position) / 24) invalid();
        entries.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            auto window = static_cast<std::uint32_t>(codec.get(4));
            auto geometry = read_rect();
            auto border = static_cast<std::uint32_t>(codec.get(4));
            entries.push_back({window, geometry, border});
        }
    };
    read_geometry(result.floating_geometry);
    read_geometry(result.fullscreen_geometry);
    if (version >= 5) {
        auto name_count = codec.get(4);
        if (name_count && name_count != workspace_count) invalid();
        for (std::size_t i = 0; i < name_count; ++i) {
            auto size = codec.get(4);
            if (!size || size > 4096 || size > bytes.size() - codec.position) invalid();
            std::string name;
            name.reserve(size);
            for (std::size_t j = 0; j < size; ++j) name.push_back(static_cast<char>(codec.get(1)));
            result.workspace_names.push_back(std::move(name));
        }
    }
    if (codec.position != bytes.size()) invalid();
    validate_restart_snapshot(result);
    return result;
}

std::vector<std::uint8_t> encode_layout_snapshot(const LayoutSnapshot &snapshot) {
    Codec codec;
    codec.put(0x3154524d, 4);
    codec.put(1, 4);
    codec.encode(snapshot, 0);
    return codec.output;
}

LayoutSnapshot decode_layout_snapshot(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > restart_max_bytes) invalid();
    Codec codec;
    codec.input = bytes;
    if (codec.get(4) != 0x3154524d || codec.get(4) != 1) invalid();
    auto result = codec.decode(0);
    if (codec.position != bytes.size()) invalid();
    codec.nodes = 0;
    codec.validate(result, 0);
    auto validate_children = [&](auto &&self, const LayoutSnapshot &node, std::size_t depth) -> void {
        for (const auto &child : node.children) {
            codec.validate(child, depth + 1);
            self(self, child, depth + 1);
        }
    };
    validate_children(validate_children, result, 0);
    return result;
}

}
