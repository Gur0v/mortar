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
    auto live = Layout::split(Split::horizontal);
    check(live.set_split(0, Split::vertical));
    check(live.insert_after(0, Layout::leaf(10)));
    check(live.insert_after(10, Layout::leaf(11)));
    check(live.set_split(10, Split::horizontal));
    check(live.insert_after(10, Layout::leaf(12)));
    check(!live.insert_after(10, Layout::leaf(12)));
    check(!live.insert_after(99, Layout::leaf(14)));
    check(!live.insert_after(0, Layout::leaf(14)));
    check(!live.set_split(99, Split::vertical));
    check(!live.set_split(10, static_cast<Split>(-1)));
    check(live.geometry({0, 0, 100, 100}) ==
          (std::vector<Placement>{{10, {0, 0, 50, 50}}, {12, {50, 0, 50, 50}},
                                  {11, {0, 50, 100, 50}}}));
    using mortar::Direction;
    check(live.neighbor(10, Direction::right) == 12);
    check(live.neighbor(12, Direction::left) == 10);
    auto edge = Layout::split(Split::horizontal, {
        Layout::split(Split::horizontal, {Layout::leaf(21), Layout::leaf(22)}),
        Layout::leaf(23)});
    check(edge.neighbor(23, Direction::left) == 21);
    check(edge.focus(22));
    check(edge.neighbor(23, Direction::left) == 22);
    check(edge.neighbor(23, Direction::right) == 22);
    check(!edge.focus(99));
    check(edge.remove(22));
    check(edge.neighbor(23, Direction::left) == 21);
    check(edge.insert_after(21, Layout::leaf(24)));
    check(edge.focus(24));
    check(edge.set_split(24, Split::vertical));
    check(edge.insert_after(24, Layout::leaf(25)));
    check(edge.focus(25));
    check(edge.neighbor(23, Direction::left) == 25);
    check(edge.remove(25));
    check(edge.neighbor(23, Direction::left) == 24);
    check(live.neighbor(12, Direction::down) == 11);
    check(live.neighbor(10, Direction::up) == 11);
    check(live.neighbor(10, Direction::left) == 12);
    check(live.neighbor(99, Direction::right) == 0);
    check(live.neighbor(10, static_cast<Direction>(-1)) == 0);
    check(live.remove(12));
    check(live.set_split(10, Split::vertical));
    check(live.insert_after(10, Layout::leaf(13)));
    check(live.geometry({0, 0, 100, 100}) ==
          (std::vector<Placement>{{10, {0, 0, 100, 25}}, {13, {0, 25, 100, 25}},
                                  {11, {0, 50, 100, 50}}}));
    auto changed = Layout::split(Split::horizontal, {Layout::leaf(31), Layout::leaf(32)});
    check(changed.resize(31, Direction::right, 10, {0, 0, 100, 20}));
    check(changed.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{31, {0, 0, 60, 20}}, {32, {60, 0, 40, 20}}}));
    check(!changed.resize(31, Direction::right, 40, {0, 0, 100, 20}));
    check(!changed.resize(31, Direction::left, 1, {0, 0, 100, 20}));
    check(changed.move(31, Direction::right));
    check(changed.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{32, {0, 0, 40, 20}}, {31, {40, 0, 60, 20}}}));
    check(!changed.move(31, Direction::right));
    check(!changed.move(99, Direction::left));
    auto nested = Layout::split(Split::vertical, {
        Layout::split(Split::horizontal, {Layout::leaf(41), Layout::leaf(42)}),
        Layout::leaf(43)});
    check(nested.resize(41, Direction::down, 10, {0, 0, 100, 100}));
    check(nested.geometry({0, 0, 100, 100}) ==
          (std::vector<Placement>{{41, {0, 0, 50, 60}}, {42, {50, 0, 50, 60}},
                                  {43, {0, 60, 100, 40}}}));
    check(nested.move(41, Direction::down));
    check(nested.geometry({0, 0, 100, 100}) ==
          (std::vector<Placement>{{42, {0, 0, 100, 60}}, {43, {0, 60, 100, 20}},
                                  {41, {0, 80, 100, 20}}}));
    auto equal = Layout::split(Split::horizontal, {Layout::leaf(61), Layout::leaf(62)});
    check(equal.insert_after(61, Layout::leaf(63)));
    check(equal.geometry({0, 0, 800, 20}) ==
          (std::vector<Placement>{{61, {0, 0, 266, 20}}, {63, {266, 0, 267, 20}},
                                  {62, {533, 0, 267, 20}}}));
    auto insertion = Layout::split(Split::horizontal, {Layout::leaf(51), Layout::leaf(52)});
    check(insertion.resize(51, Direction::right, 10, {0, 0, 100, 20}));
    check(insertion.insert_after(51, Layout::leaf(53)));
    check(insertion.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{51, {0, 0, 30, 20}}, {53, {30, 0, 30, 20}},
                                  {52, {60, 0, 40, 20}}}));
    check(!nested.resize(41, Direction::up, 1, {0, 0, 0, 0}));
    check(!nested.resize(41, static_cast<Direction>(-1), 1, {0, 0, 100, 100}));
    bool rejected = false;
    try { Layout::split(static_cast<Split>(-1)); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
    rejected = false;
    try { Layout::split(Split::horizontal, {one, one}); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
}
