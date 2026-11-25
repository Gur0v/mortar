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
}
