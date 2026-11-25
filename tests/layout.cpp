#include "../mortar/layout.hpp"

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

using mortar::Layout;
using mortar::Placement;
using mortar::Rect;
using mortar::Split;

void check(bool value) {
    if (!value) std::abort();
}

int main() {
    auto one = Layout::leaf(1);
    check(one.geometry({-3, 4, 5, 7}) == std::vector<Placement>{{1, {-3, 4, 5, 7}}});
    check(!one.insert(Layout::leaf(2)));
    auto tree = Layout::split(Split::horizontal, {one,
        Layout::split(Split::vertical, {Layout::leaf(2), Layout::leaf(3)}), Layout::leaf(4)});
    check(tree.geometry({-3, 4, 8, 7}) ==
          (std::vector<Placement>{{1, {-3, 4, 2, 7}}, {2, {-1, 4, 3, 3}},
                                  {3, {-1, 7, 3, 4}}, {4, {2, 4, 3, 7}}}));
    check(tree.contains(2));
    check(!tree.contains(5));
    check(!tree.insert(Layout::leaf(2)));
    check(tree.insert(Layout::leaf(5)));
    check(tree.contains(5));
    check(tree.remove(2));
    check(!tree.contains(2));
    check(!tree.remove(2));
    check(tree.remove(3));
    check(tree.geometry({0, 0, 2, 1}) ==
          (std::vector<Placement>{{1, {0, 0, 0, 1}}, {4, {0, 0, 1, 1}}, {5, {1, 0, 1, 1}}}));
    check(Layout::split(Split::horizontal).geometry({0, 0, 1, 1}).empty());
    for (Rect invalid : {Rect{0, 0, -1, 1}, Rect{0, 0, 1, -1},
                         Rect{std::numeric_limits<int>::max(), 0, 1, 0},
                         Rect{0, std::numeric_limits<int>::max(), 0, 1}}) {
        bool rejected = false;
        try { tree.geometry(invalid); } catch (const std::invalid_argument &) { rejected = true; }
        check(rejected);
    }
    bool rejected = false;
    try { Layout::split(static_cast<Split>(-1)); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
    rejected = false;
    try { Layout::split(Split::horizontal, {one, one}); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
}
