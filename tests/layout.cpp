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
    check(tree.focus(2));
    check(tree.select_parent(2));
    check(tree.selected_path() == std::vector<std::size_t>{1});
    check(tree.select_parent(2));
    check(tree.selected_path() == std::vector<std::size_t>{});
    check(!tree.select_parent(2));
    check(tree.select_child());
    check(tree.selected_path() == std::vector<std::size_t>{1});
    check(tree.select_child());
    check(!tree.selected_path());
    check(!tree.select_child());
    {
        auto selected = tree;
        check(selected.select_parent(2));
        check(selected.neighbor(2, mortar::Direction::left) == 1);
        check(selected.sibling(2, false) == 4);
        check(selected.resize(2, mortar::Direction::right, 10, {0, 0, 300, 100}));
        check(selected.move(2, mortar::Direction::right));
        check(selected.selected_path() == std::vector<std::size_t>{2});
        check(selected.window_path(2) == (std::vector<std::size_t>{2, 0}));
        check(selected.remove(1));
        check(!selected.selected_path());
        check(selected.select_parent(2));
        check(selected.focus(3));
        check(!selected.selected_path());
        check(selected.select_parent(2));
        check(selected.set_split(2, Split::horizontal));
        check(selected.insert_after(2, Layout::leaf(8)));
        check(!selected.selected_path());
        check(selected.window_path(8) == (std::vector<std::size_t>{1, 0, 2}));
    }
    {
        auto selected = Layout::split(Split::horizontal, {
            Layout::split(Split::vertical, {Layout::leaf(1), Layout::leaf(2)}), Layout::leaf(3)});
        check(selected.focus(1));
        check(selected.select_parent(1));
        check(selected.set_split(1, Split::horizontal));
        check(selected.selected_path() == (std::vector<std::size_t>{0, 0}));
        check(selected.insert_after(1, Layout::leaf(4)));
        check(selected.geometry({0, 0, 100, 100}) ==
              (std::vector<Placement>{{1, {0, 0, 50, 33}}, {4, {0, 33, 50, 33}},
                                      {2, {0, 66, 50, 34}}, {3, {50, 0, 50, 100}}}));
    }
    {
        auto selected = Layout::split(Split::horizontal, {Layout::leaf(1), Layout::leaf(2)});
        check(selected.focus(1));
        check(selected.select_parent(1));
        check(selected.set_split(1, Split::vertical));
        check(selected.selected_path() == std::vector<std::size_t>{0});
        check(selected.insert_after(1, Layout::leaf(3)));
        check(selected.geometry({0, 0, 100, 100}) ==
              (std::vector<Placement>{{1, {0, 0, 33, 100}}, {3, {33, 0, 33, 100}},
                                      {2, {66, 0, 34, 100}}}));
    }
    {
        auto selected = Layout::split(Split::horizontal, {Layout::leaf(1), Layout::leaf(2), Layout::leaf(3)});
        check(selected.focus(1));
        check(selected.select_parent(1));
        check(selected.insert_after(1, Layout::leaf(4)));
        check(selected.window_path(4) == std::vector<std::size_t>{1});
        check(selected.window_path(2) == std::vector<std::size_t>{2});
        check(selected.neighbor(3, mortar::Direction::right) == 1);
    }
    {
        auto nested = Layout::split(Split::horizontal, {
            Layout::split(Split::vertical, {Layout::split(Split::horizontal, {Layout::leaf(10), Layout::leaf(11)}), Layout::leaf(12)}),
            Layout::leaf(13)});
        check(nested.focus(10));
        check(nested.select_parent(10));
        check(nested.move(10, mortar::Direction::right));
        check(nested.selected_path() == std::vector<std::size_t>{2});
        check(nested.window_path(10) == (std::vector<std::size_t>{2, 0}));
        check(nested.window_path(12) == (std::vector<std::size_t>{0, 0}));
    }
    {
        auto nested = Layout::split(Split::horizontal, {
            Layout::split(Split::vertical, {Layout::split(Split::horizontal, {Layout::leaf(20), Layout::leaf(21)})}),
            Layout::leaf(22)});
        check(nested.focus(20));
        check(nested.select_parent(20));
        check(nested.move(20, mortar::Direction::right));
        check(nested.selected_path() == std::vector<std::size_t>{1});
        check(nested.window_path(20) == (std::vector<std::size_t>{1, 0}));
        check(nested.window_path(22) == std::vector<std::size_t>{0});
    }
    check(tree.window_path(2) == (std::vector<std::size_t>{1, 0}));
    check(!tree.window_path(99));
    check(Layout::leaf(1).window_path(1) == std::vector<std::size_t>{});
    check(tree.parent_path({1, 0}) == std::vector<std::size_t>{1});
    check(tree.parent_path({1}) == std::vector<std::size_t>{});
    check(!tree.parent_path({}));
    check(!tree.parent_path({3}));
    check(!tree.parent_path({0, 0}));
    check(!Layout::leaf(1).parent_path({0}));
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
        auto before = tree.snapshot();
        rejected = false;
        try { tree.resize(1, mortar::Direction::right, 1, invalid); }
        catch (const std::invalid_argument &) { rejected = true; }
        check(rejected);
        check(tree.snapshot() == before);
    }
    auto toggle = Layout::split(Split::horizontal);
    check(toggle.toggle_split(0));
    check(toggle.insert_after(0, Layout::leaf(71)));
    check(toggle.insert_after(71, Layout::leaf(72)));
    check(toggle.geometry({0, 0, 100, 100}) ==
          (std::vector<Placement>{{71, {0, 0, 100, 50}}, {72, {0, 50, 100, 50}}}));
    check(!toggle.toggle_split(99));
    check(!toggle.toggle_split(0));
    check(toggle.toggle_split(71));
    check(toggle.geometry({0, 0, 100, 100}) ==
          (std::vector<Placement>{{71, {0, 0, 50, 100}}, {72, {50, 0, 50, 100}}}));
    check(toggle.focus(71));
    check(toggle.select_parent(71));
    check(toggle.toggle_split(71));
    check(toggle.geometry({0, 0, 100, 100}) ==
          (std::vector<Placement>{{71, {0, 0, 100, 50}}, {72, {0, 50, 100, 50}}}));
    check(toggle.selected_path() == std::vector<std::size_t>{});
    auto single = Layout::split(Split::horizontal, {Layout::leaf(75)});
    check(single.toggle_split(75));
    check(single.insert_after(75, Layout::leaf(76)));
    check(single.geometry({0, 0, 100, 100}) ==
          (std::vector<Placement>{{75, {0, 0, 100, 50}}, {76, {0, 50, 100, 50}}}));
    check(!Layout::leaf(75).toggle_split(75));
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
    check(edge.sibling(23, true) == 22);
    check(edge.sibling(23, false) == 0);
    check(edge.sibling(21, false) == 22);
    check(edge.sibling(22, false) == 0);
    check(edge.sibling(21, true) == 0);
    check(edge.sibling(99, false) == 0);
    check(Layout::leaf(21).sibling(21, false) == 0);
    check(edge.neighbor(23, Direction::right) == 22);
    check(edge.neighbor(23, Direction::right, false) == 0);
    check(edge.neighbor(23, Direction::left, false) == 22);
    check(edge.neighbor(22, Direction::right, false) == 23);
    check(edge.neighbor(21, Direction::left, false) == 0);
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
    auto clamped = Layout::split(Split::horizontal, {Layout::leaf(71), Layout::leaf(72)});
    check(clamped.resize(71, Direction::right, 1000, {0, 0, 100, 20}, true));
    check(clamped.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{71, {0, 0, 99, 20}}, {72, {99, 0, 1, 20}}}));
    check(!clamped.resize(71, Direction::right, 1, {0, 0, 100, 20}, true));
    check(clamped.resize(71, Direction::right, -2, {0, 0, 100, 20}, true));
    check(clamped.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{71, {0, 0, 97, 20}}, {72, {97, 0, 3, 20}}}));
    check(clamped.resize(72, Direction::left, 1000, {0, 0, 100, 20}, true));
    check(clamped.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{71, {0, 0, 1, 20}}, {72, {1, 0, 99, 20}}}));
    check(!clamped.resize(72, Direction::left, 1, {0, 0, 100, 20}, true));
    check(clamped.resize(72, Direction::left, -2, {0, 0, 100, 20}, true));
    check(clamped.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{71, {0, 0, 3, 20}}, {72, {3, 0, 97, 20}}}));
    auto deep = Layout::split(Split::horizontal, {Layout::leaf(76),
        Layout::split(Split::horizontal, {Layout::leaf(77), Layout::leaf(78)})});
    check(deep.resize(76, Direction::right, 1000, {0, 0, 100, 20}, true));
    check(deep.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{76, {0, 0, 98, 20}}, {77, {98, 0, 1, 20}},
                                  {78, {99, 0, 1, 20}}}));
    auto zero_snapshot = Layout::split(Split::horizontal, {Layout::leaf(88), Layout::leaf(89), Layout::leaf(90)}).snapshot();
    zero_snapshot.weights = {0.1, 3, 3};
    auto zero = Layout::restore(zero_snapshot);
    check(zero.resize(89, Direction::right, 1, {0, 0, 10, 10}, true));
    check(zero.geometry({0, 0, 10, 10}) ==
          (std::vector<Placement>{{88, {0, 0, 0, 10}}, {89, {0, 0, 6, 10}}, {90, {6, 0, 4, 10}}}));
    auto uneven_snapshot = Layout::split(Split::horizontal, {Layout::leaf(84),
        Layout::split(Split::horizontal, {Layout::leaf(85), Layout::leaf(86), Layout::leaf(87)})}).snapshot();
    uneven_snapshot.weights = {3, 7};
    uneven_snapshot.children[1].weights = {3, 1, 2};
    auto uneven = Layout::restore(uneven_snapshot);
    check(uneven.resize(84, Direction::right, 5, {0, 0, 10, 10}, true));
    check(uneven.geometry({0, 0, 10, 10}) ==
          (std::vector<Placement>{{84, {0, 0, 5, 10}}, {85, {5, 0, 2, 10}},
                                  {86, {7, 0, 1, 10}}, {87, {8, 0, 2, 10}}}));
    auto huge_snapshot = Layout::split(Split::horizontal, {Layout::leaf(79), Layout::leaf(80)}).snapshot();
    huge_snapshot.weights = {std::numeric_limits<double>::max() / 4, std::numeric_limits<double>::max() / 4};
    auto huge = Layout::restore(huge_snapshot);
    check(huge.resize(79, Direction::right, 1000, {0, 0, 100, 20}, true));
    check(huge.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{79, {0, 0, 99, 20}}, {80, {99, 0, 1, 20}}}));
    auto thirds = Layout::split(Split::vertical, {Layout::leaf(73), Layout::leaf(74), Layout::leaf(75)});
    check(thirds.resize(74, Direction::down, 1000, {0, 0, 20, 100}, true));
    check(thirds.geometry({0, 0, 20, 100}) ==
          (std::vector<Placement>{{73, {0, 0, 20, 33}}, {74, {0, 33, 20, 66}},
                                  {75, {0, 99, 20, 1}}}));
    check(thirds.resize(74, Direction::down, -1000, {0, 0, 20, 100}, true));
    check(thirds.geometry({0, 0, 20, 100}) ==
          (std::vector<Placement>{{73, {0, 0, 20, 33}}, {74, {0, 33, 20, 1}},
                                  {75, {0, 34, 20, 66}}}));
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
    auto constrained = Layout::split(Split::horizontal, {
        Layout::split(Split::horizontal, {Layout::leaf(81), Layout::leaf(82)}),
        Layout::leaf(83)});
    auto before_resize = constrained.snapshot();
    check(!constrained.resize(81, Direction::right, 30, {0, 0, 100, 20}));
    check(constrained.snapshot() == before_resize);
    check(constrained.resize(82, Direction::right, 10, {0, 0, 100, 20}));
    check(constrained.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{81, {0, 0, 30, 20}}, {82, {30, 0, 30, 20}},
                                  {83, {60, 0, 40, 20}}}));
    check(nested.move(41, Direction::down));
    check(nested.geometry({0, 0, 100, 100}) ==
          (std::vector<Placement>{{42, {0, 0, 100, 60}}, {43, {0, 60, 100, 20}},
                                  {41, {0, 80, 100, 20}}}));
    for (auto direction : {Direction::left, Direction::right}) {
        auto outward = Layout::split(Split::horizontal, {
            Layout::split(Split::vertical, {Layout::leaf(51), Layout::leaf(52)})});
        check(outward.move(51, direction));
        auto placements = outward.geometry({0, 0, 100, 100});
        if (direction == Direction::left)
            check(placements == (std::vector<Placement>{{51, {0, 0, 50, 100}}, {52, {50, 0, 50, 100}}}));
        else
            check(placements == (std::vector<Placement>{{52, {0, 0, 50, 100}}, {51, {50, 0, 50, 100}}}));
    }
    auto equal = Layout::split(Split::horizontal, {Layout::leaf(61), Layout::leaf(62)});
    check(equal.insert_after(61, Layout::leaf(63)));
    check(equal.geometry({0, 0, 800, 20}) ==
          (std::vector<Placement>{{61, {0, 0, 266, 20}}, {63, {266, 0, 267, 20}},
                                  {62, {533, 0, 267, 20}}}));
    auto saved = nested.snapshot();
    auto restored = Layout::restore(saved);
    check(restored.snapshot() == saved);
    check(restored.geometry({0, 0, 100, 100}) == nested.geometry({0, 0, 100, 100}));
    check(restored.neighbor(43, Direction::up) == nested.neighbor(43, Direction::up));
    check(restored.insert_after(43, Layout::leaf(44)));
    check(nested.insert_after(43, Layout::leaf(44)));
    check(restored.snapshot() == nested.snapshot());
    auto invalid_snapshot = saved;
    auto reject_snapshot = [](const mortar::LayoutSnapshot &snapshot) {
        try { Layout::restore(snapshot); } catch (const std::invalid_argument &) { return true; }
        return false;
    };
    invalid_snapshot.focus_order[0] = invalid_snapshot.focus_order[1];
    check(reject_snapshot(invalid_snapshot));
    invalid_snapshot = saved;
    invalid_snapshot.weights[0] = std::numeric_limits<double>::infinity();
    check(reject_snapshot(invalid_snapshot));
    invalid_snapshot = saved;
    invalid_snapshot.weights[0] = 0;
    check(reject_snapshot(invalid_snapshot));
    invalid_snapshot = saved;
    invalid_snapshot.children.push_back(invalid_snapshot.children.back());
    invalid_snapshot.focus_order.push_back(invalid_snapshot.focus_order.size());
    invalid_snapshot.weights.push_back(1);
    check(reject_snapshot(invalid_snapshot));
    invalid_snapshot = saved;
    invalid_snapshot.direction = static_cast<Split>(-1);
    check(reject_snapshot(invalid_snapshot));
    auto insertion = Layout::split(Split::horizontal, {Layout::leaf(51), Layout::leaf(52)});
    check(insertion.resize(51, Direction::right, 10, {0, 0, 100, 20}));
    check(insertion.insert_after(51, Layout::leaf(53)));
    check(insertion.geometry({0, 0, 100, 20}) ==
          (std::vector<Placement>{{51, {0, 0, 40, 20}}, {53, {40, 0, 33, 20}},
                                  {52, {73, 0, 27, 20}}}));
    check(!nested.resize(41, Direction::up, 1, {0, 0, 0, 0}));
    check(!nested.resize(41, static_cast<Direction>(-1), 1, {0, 0, 100, 100}));
    bool rejected = false;
    try { Layout::split(static_cast<Split>(-1)); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
    rejected = false;
    try { Layout::split(Split::horizontal, {one, one}); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
}
