#include "../mortar/outputs.hpp"

#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace mortar;

void check(bool value) {
    if (!value) std::abort();
}

int main() {
    Outputs state;
    check(!state.focused_output());
    check(!state.adjacent_workspace(true));
    check(!state.switch_workspace(1));
    check(state.update({{"a", {0, 0, 100, 100}}, {"b", {100, 0, 100, 100}}}));
    check(state.outputs().size() == 2);
    check(state.focused_output() == "a");
    check(state.outputs()[0].workspace == 0 && state.outputs()[1].workspace == 1);
    check(state.adjacent_workspace(true) == 1 && state.adjacent_workspace(false) == 1);
    check(state.switch_workspace(1));
    check(state.focused_output() == "b");
    check(state.adjacent_workspace(true) == 0 && state.adjacent_workspace(false) == 0);
    check(state.switch_workspace(4));
    check(state.focused_workspace() == 4 && state.owners()[4] == "b");
    check(state.focus_output("a"));
    check(state.switch_workspace(4));
    check(state.focused_output() == "b");
    check(state.update({{"a", {-100, 0, 100, 100}}}));
    check(state.focused_output() == "a" && state.focused_workspace() == 4);
    check(state.owners()[1] == "a" && state.owners()[4] == "a");
    check(state.update({}));
    check(!state.focused_output() && state.focused_workspace() == 4);
    check(!state.owners()[4]);
    check(state.update({{"c", {0, 0, 50, 50}}}));
    check(state.focused_workspace() == 4 && state.outputs()[0].workspace == 4);
    check(state.update({{"c", {0, 0, 50, 50}}, {"clone", {0, 0, 50, 50}}}));
    check(state.outputs().size() == 1);
    check(!state.adjacent_workspace(true) && !state.adjacent_workspace(false));
    check(!state.update({{"c", {0, 0, 50, 50}}, {"c", {50, 0, 50, 50}}}));
    check(!state.update({{"bad", {0, 0, 0, 1}}}));
    check(!state.update({{"bad", {std::numeric_limits<int>::max(), 0, 1, 1}}}));
    check(state.outputs().size() == 1 && state.focused_workspace() == 4);
    check(!state.focus_output("missing") && !state.switch_workspace(16384));
    check(state.update({{"c", {0, 0, 50, 50}}, {"d", {50, 0, 50, 50}}}));
    check(state.outputs()[0].workspace != state.outputs()[1].workspace);

    Outputs cycle;
    check(cycle.update({{"a", {0, 0, 50, 50}}, {"b", {50, 0, 50, 50}}, {"c", {100, 0, 50, 50}}}));
    check(cycle.adjacent_workspace(true) == 1 && cycle.adjacent_workspace(false) == 2);
    check(cycle.focus_output("b"));
    check(cycle.adjacent_workspace(true) == 2 && cycle.adjacent_workspace(false) == 0);

    Outputs replacement;
    check(replacement.update({{"a", {0, 0, 50, 50}}, {"b", {50, 0, 50, 50}}}));
    check(replacement.focus_output("b"));
    check(replacement.switch_workspace(7));
    check(replacement.update({{"a", {0, 0, 50, 50}}, {"c", {50, 0, 50, 50}}}));
    check(replacement.focused_workspace() == 7);
    check(replacement.focused_output() == "a");
    check(replacement.owners()[7] == "a");
    check(replacement.outputs()[0].workspace == 7);

    Outputs single;
    check(single.update({{"b", {0, 0, 50, 50}}}));
    check(single.switch_workspace(7));
    check(single.update({{"c", {0, 0, 50, 50}}}));
    check(single.focused_output() == "c" && single.focused_workspace() == 7);

    Outputs ownership;
    check(ownership.update({{"a", {0, 0, 50, 50}}}));
    check(ownership.switch_workspace(1));
    check(ownership.switch_workspace(0));
    check(ownership.update({{"a", {0, 0, 50, 50}}, {"b", {50, 0, 50, 50}}}));
    check(ownership.owners()[1] == "a");
    check(ownership.outputs()[1].workspace == 2);
    check(ownership.update({{"clone", {0, 0, 50, 50}}, {"a", {0, 0, 50, 50}},
                            {"b", {50, 0, 50, 50}}}));
    check(ownership.outputs()[0].name == "a");
    check(ownership.owners()[1] == "a");

    Outputs topology;
    check(topology.update({{"left", {-100, -50, 100, 100}}, {"right", {0, -50, 100, 100}}}));
    check(topology.focus_output("right") && topology.switch_workspace(5));
    check(topology.update({{"right", {25, 10, 120, 80}}, {"left", {-100, -50, 100, 100}}}));
    check(topology.focused_output() == "right" && topology.focused_workspace() == 5);
    check(topology.owners()[5] == "right");
    check(topology.outputs()[0].name == "right" && topology.outputs()[0].bounds == (Rect{25, 10, 120, 80}));
    const auto before_outputs = topology.outputs();
    const auto before_owners = topology.owners();
    check(!topology.update({{"left", {-100, -50, 100, 100}}, {"", {0, 0, 1, 1}}}));
    check(!topology.update({{"bad", {0, std::numeric_limits<int>::max(), 1, 1}}}));
    check(topology.outputs() == before_outputs && topology.owners() == before_owners);
    check(topology.focused_output() == "right" && topology.focused_workspace() == 5);
    check(topology.update({{"left", {-100, -50, 100, 100}}}));
    check(topology.focused_output() == "left" && topology.focused_workspace() == 5);
    check(topology.owners()[1] == "left" && topology.owners()[5] == "left");

    Outputs extended;
    check(extended.update({{"a", {0, 0, 50, 50}}}));
    check(extended.switch_workspace(12));
    check(extended.focused_workspace() == 12 && extended.owners()[12] == "a");
    check(!extended.switch_workspace(16384));

    auto restored = Outputs::restore(topology.outputs(), topology.owners(), topology.focused_output(),
                                     topology.focused_workspace());
    check(restored.outputs() == topology.outputs() && restored.owners() == topology.owners());
    check(restored.focused_output() == topology.focused_output() &&
          restored.focused_workspace() == topology.focused_workspace());
    check(restored.update({{"replacement", {0, 0, 100, 100}}}));
    check(restored.focused_workspace() == 5 && restored.owners()[5] == "replacement");
    auto no_outputs = Outputs::restore({}, std::vector<std::optional<std::string>>(10), std::nullopt, 4);
    check(no_outputs.update({{"a", {0, 0, 50, 50}}}));
    check(no_outputs.focused_workspace() == 4);
    bool rejected = false;
    try {
        Outputs::restore(topology.outputs(), topology.owners(), topology.focused_output(), 0);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected);

    Outputs full;
    check(full.update({{"a", {0, 0, 50, 50}}}));
    for (std::uint32_t i = 1; i < 10; ++i) check(full.switch_workspace(i));
    check(full.update({{"a", {0, 0, 50, 50}}, {"b", {50, 0, 50, 50}}}));
    check(full.focused_output() == "a" && full.focused_workspace() == 9);
    check(full.outputs()[0].workspace == 9 && full.outputs()[1].workspace == 0);
    check(full.owners()[0] == "b" && full.owners()[9] == "a");
    for (std::uint32_t i = 1; i < 9; ++i) check(full.owners()[i] == "a");
}
