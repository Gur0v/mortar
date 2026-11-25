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
}
