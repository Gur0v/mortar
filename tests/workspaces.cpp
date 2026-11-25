#include "../mortar/workspaces.hpp"

#include <cstdlib>
#include <vector>

void check(bool value) {
    if (!value) std::abort();
}

int main() {
    mortar::Workspaces state;
    auto &first = state.workspaces[0];
    auto &second = state.workspaces[1];
    check(first.layout.insert(mortar::Layout::leaf(1)));
    check(first.layout.insert(mortar::Layout::leaf(2)));
    first.windows = {1, 2};
    first.focused = 2;
    check(state.owner(1) == 0 && state.owner(3) == -1);
    check(!state.switch_to(0) && !state.switch_to(10));
    check(state.current == 0);
    check(!state.move(3, 1) && !state.move(1, 10) && !state.move(1, 0));
    check(state.move(2, 1));
    check(state.owner(2) == 1 && !first.layout.contains(2));
    check(first.windows == std::vector<std::uint32_t>{1} && first.focused == 1);
    check(second.windows == std::vector<std::uint32_t>{2} && second.focused == 2);
    check(state.switch_to(1));
    check(state.current == 1 && first.focused == 1 && second.focused == 2);
    check(state.move(1, 1));
    check(state.owner(1) == 1 && first.windows.empty() && first.focused == 0);
    check(second.windows == (std::vector<std::uint32_t>{2, 1}) && second.focused == 2);
    check(state.move(2, 0));
    check(second.focused == 1 && first.focused == 2);
    check(state.switch_to(0));
    check(first.focused == 2 && second.focused == 1);

    mortar::Workspaces navigation;
    std::array<bool, 10> visible{};
    visible[0] = true;
    check(!navigation.previous && navigation.adjacent(true, visible) == 0);
    check(!navigation.switch_to(0) && !navigation.previous);
    visible[4] = true;
    navigation.workspaces[7].windows.push_back(99);
    check(navigation.adjacent(true, visible) == 4 && navigation.adjacent(false, visible) == 7);
    check(navigation.switch_to(4) && navigation.previous == 0);
    check(navigation.adjacent(true, visible) == 7 && navigation.adjacent(false, visible) == 0);
    check(navigation.switch_to(7) && navigation.previous == 4);
    check(navigation.adjacent(true, visible) == 0 && navigation.adjacent(false, visible) == 4);
    check(navigation.switch_to(*navigation.previous) && navigation.previous == 7);
    check(!navigation.switch_to(10) && navigation.previous == 7);

    mortar::Workspaces nested;
    auto &source = nested.workspaces[0];
    auto &target = nested.workspaces[1];
    source.layout = mortar::Layout::split(mortar::Split::horizontal, {
        mortar::Layout::leaf(10),
        mortar::Layout::split(mortar::Split::vertical, {
            mortar::Layout::leaf(11), mortar::Layout::leaf(12)})});
    source.windows = {10, 11, 12};
    source.focused = 11;
    check(target.layout.insert(mortar::Layout::leaf(20)));
    target.windows = {20};
    target.focused = 20;
    check(nested.move(11, 1));
    check(nested.owner(11) == 1 && source.focused == 12 && target.focused == 11);
    check(source.windows == (std::vector<std::uint32_t>{10, 12}));
    check(target.windows == (std::vector<std::uint32_t>{20, 11}));
    check(source.layout.geometry({0, 0, 120, 80}) ==
          (std::vector<mortar::Placement>{{10, {0, 0, 60, 80}}, {12, {60, 0, 60, 80}}}));
    check(target.layout.geometry({0, 0, 120, 80}) ==
          (std::vector<mortar::Placement>{{20, {0, 0, 60, 80}}, {11, {60, 0, 60, 80}}}));
    check(nested.move(12, 1));
    check(nested.owner(12) == 1 && source.focused == 10 && target.focused == 12);
    check(source.windows == std::vector<std::uint32_t>{10});
    check(source.layout.geometry({0, 0, 120, 80}) ==
          (std::vector<mortar::Placement>{{10, {0, 0, 120, 80}}}));
    check(target.layout.geometry({0, 0, 120, 80}) ==
          (std::vector<mortar::Placement>{{20, {0, 0, 40, 80}}, {11, {40, 0, 40, 80}},
                                          {12, {80, 0, 40, 80}}}));
    check(nested.move(11, 0));
    check(source.focused == 10 && target.focused == 12);
    check(source.layout.geometry({0, 0, 120, 80}) ==
          (std::vector<mortar::Placement>{{10, {0, 0, 60, 80}}, {11, {60, 0, 60, 80}}}));

    mortar::Workspaces floating;
    auto &a = floating.workspaces[0];
    auto &b = floating.workspaces[1];
    check(!a.add_floating(0) && a.add_floating(30) && !a.add_floating(30));
    check(a.focused == 30 && floating.owner(30) == 0 && !a.layout.contains(30));
    check(!a.remove_floating(31) && !a.toggle_floating(31));
    check(a.toggle_floating(30) && a.layout.contains(30) && a.floating.empty());
    check(a.focused == 30 && a.windows == std::vector<std::uint32_t>{30});
    check(a.toggle_floating(30) && !a.layout.contains(30));
    check(a.add_floating(31));
    check(floating.move(30, 1));
    check(floating.owner(30) == 1 && b.floating == std::vector<std::uint32_t>{30});
    check(a.focused == 31 && b.focused == 30 && !b.layout.contains(30));
    check(a.toggle_floating(31));
    check(floating.move(31, 1));
    check(b.layout.contains(31) && b.focused == 31 && a.focused == 0);
    check(floating.switch_to(1));
    check(b.toggle_floating(31) && b.focused == 31);
    check(b.toggle_floating(31) && b.layout.contains(31));
    check(b.remove_floating(30) && floating.owner(30) == -1 && b.focused == 31);
    check(!b.remove_floating(30));
    check(b.toggle_floating(31) && b.remove_floating(31));
    check(b.windows.empty() && b.focused == 0);
}
