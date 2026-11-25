#include "../mortar/scratchpad.hpp"

#include <cstdlib>

using namespace mortar;

void check(bool value) {
    if (!value) std::abort();
}

int main() {
    Scratchpad pad;
    check(!pad.toggle(0));
    check(!pad.add(0, 0));
    check(pad.add(1, 2));
    check(pad.add(2, 3, Rect{1, 2, 30, 40}));
    check(!pad.add(1, 4));
    check(!pad.remove(99));
    check(!pad.update_geometry(99, {0, 0, 1, 1}));
    auto result = pad.toggle(0);
    check(result && result->action == ScratchpadAction::show && result->entry.window == 1);
    check(result->entry.workspace == 2 && !result->entry.geometry);
    result = pad.toggle(0, 99);
    check(result && result->action == ScratchpadAction::focus && result->entry.window == 1);
    result = pad.toggle(0, 1);
    check(result && result->action == ScratchpadAction::hide && !result->entry.visible_workspace);
    result = pad.toggle(0);
    check(result && result->entry.window == 2 && result->entry.geometry == Rect{1, 2, 30, 40});
    check(pad.update_geometry(2, {5, 6, 7, 8}));
    result = pad.toggle(1);
    check(result && result->action == ScratchpadAction::move && result->entry.visible_workspace == 1);
    result = pad.toggle(0, 0, 1);
    check(result && result->action == ScratchpadAction::show && result->entry.window == 1);
    result = pad.toggle(0, 0, 1);
    check(result && result->action == ScratchpadAction::hide);
    check(!pad.toggle(0, 0, 99));
    check(pad.get(2)->geometry == Rect{5, 6, 7, 8});
    check(pad.remove(2));
    check(!pad.get(2));
    check(pad.remove(1));
    check(!pad.toggle(0));
}
