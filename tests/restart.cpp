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
}
