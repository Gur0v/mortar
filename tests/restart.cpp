#include "../mortar/restart.hpp"

#include <cassert>
#include <limits>
#include <stdexcept>

using namespace mortar;

template <typename F> void rejects(F f) {
    try {
        f();
        assert(false);
    } catch (const std::invalid_argument &) {
    }
}

int main() {
    auto layout = Layout::split(Split::vertical, {Layout::leaf(42), Layout::split(Split::horizontal, {Layout::leaf(7), Layout::leaf(8)})});
    layout.focus(8);
    auto snapshot = layout.snapshot();
    auto bytes = encode_layout_snapshot(snapshot);
    assert(decode_layout_snapshot(bytes) == snapshot);
    assert(Layout::restore(decode_layout_snapshot(bytes)).snapshot() == snapshot);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        rejects([&] { decode_layout_snapshot(std::span(bytes).first(i)); });
    auto changed = bytes;
    changed.push_back(0);
    rejects([&] { decode_layout_snapshot(changed); });
    for (std::size_t i : {std::size_t(0), std::size_t(4), std::size_t(8), std::size_t(9), std::size_t(10)}) {
        changed = bytes;
        changed[i] = 255;
        rejects([&] { decode_layout_snapshot(changed); });
    }
    changed = bytes;
    for (std::size_t i = 15; i < 19; ++i) changed[i] = 255;
    rejects([&] { decode_layout_snapshot(changed); });
    auto bad = snapshot;
    bad.focus_order[0] = bad.focus_order[1];
    rejects([&] { encode_layout_snapshot(bad); });
    bad = snapshot;
    bad.weights[0] = std::numeric_limits<double>::infinity();
    rejects([&] { encode_layout_snapshot(bad); });
    bad = snapshot;
    bad.children[1].children[0].window = 42;
    rejects([&] { encode_layout_snapshot(bad); });
    bad = Layout::leaf(1).snapshot();
    for (std::size_t i = 0; i <= restart_max_depth; ++i)
        bad = LayoutSnapshot{false, 0, Split::horizontal, {bad}, {0}, {1}, false};
    rejects([&] { encode_layout_snapshot(bad); });
    rejects([&] { decode_layout_snapshot(std::vector<std::uint8_t>(restart_max_bytes + 1)); });

    Workspaces workspaces;
    workspaces.workspaces[0].layout = Layout::split(Split::vertical, {Layout::leaf(42)});
    workspaces.workspaces[0].windows = {42, 7};
    workspaces.workspaces[0].floating = {7};
    workspaces.workspaces[0].focused = 7;
    workspaces.current = 1;
    workspaces.previous = 0;
    RestartSnapshot state{workspaces.snapshot(), {{7, 2, Rect{-10, 20, 300, 200}, 0}, {9, 3, std::nullopt, std::nullopt}}};
    state.outputs = {{"left", {0, 0, 800, 600}, 0}, {"right", {800, 0, 800, 600}, 1}};
    state.owners[0] = "left";
    state.owners[1] = "right";
    state.owners[2] = "left";
    state.focused_output = "right";
    state.floating_geometry = {{7, {-10, 20, 300, 200}, 2}, {9, {10, 20, 50, 60}, 1}};
    state.fullscreen_geometry = {{42, {0, 0, 400, 300}, 2}};
    auto encoded = encode_restart_snapshot(state);
    assert(decode_restart_snapshot(encoded) == state);
    assert(Workspaces::restore(state.workspaces).snapshot() == state.workspaces);
    assert(Scratchpad::restore(state.scratchpad).snapshot() == state.scratchpad);
    for (std::size_t i = 0; i < encoded.size(); ++i)
        rejects([&] { decode_restart_snapshot(std::span(encoded).first(i)); });
    auto corrupt = encoded;
    corrupt[4] = 1;
    rejects([&] { decode_restart_snapshot(corrupt); });
    corrupt = encoded;
    corrupt[8] = 10;
    rejects([&] { decode_restart_snapshot(corrupt); });
    auto trailing = encoded;
    trailing.push_back(0);
    rejects([&] { decode_restart_snapshot(trailing); });
    auto invalid_state = state;
    invalid_state.workspaces.current = 10;
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.workspaces.workspaces[0].windows.push_back(42);
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.workspaces.workspaces[0].focused = 9;
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.scratchpad[0].visible_workspace = 1;
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.scratchpad[1].window = 7;
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.scratchpad[0].geometry->width = 0;
    rejects([&] { encode_restart_snapshot(invalid_state); });
    rejects([&] { Scratchpad::restore(invalid_state.scratchpad); });
    invalid_state = state;
    invalid_state.owners[0] = "right";
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.focused_output = "left";
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.outputs[1].name = "left";
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.floating_geometry[0].geometry.width = 0;
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.fullscreen_geometry.push_back({7, {0, 0, 1, 1}, 0});
    rejects([&] { encode_restart_snapshot(invalid_state); });
    invalid_state = state;
    invalid_state.fullscreen_geometry[0].window = 9;
    rejects([&] { encode_restart_snapshot(invalid_state); });
}
