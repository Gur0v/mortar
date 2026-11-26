#include "../mortar/gaps.hpp"

#include <cstdlib>
#include <limits>
#include <stdexcept>

using mortar::Rect;

void check(bool value) {
    if (!value) std::abort();
}

int main() {
    using mortar::inner_gap;
    using mortar::outer_gap;
    check(outer_gap({-10, 20, 100, 80}, 5) == (Rect{-5, 25, 90, 70}));
    check(outer_gap({0, 0, 3, 2}, 10) == (Rect{3, 2, 0, 0}));
    check(inner_gap({0, 0, 50, 100}, {0, 0, 100, 100}, 6) == (Rect{6, 6, 41, 88}));
    check(inner_gap({50, 0, 50, 100}, {0, 0, 100, 100}, 6) == (Rect{53, 6, 41, 88}));
    check(inner_gap({0, 0, 50, 100}, {0, 0, 100, 100}, 5) == (Rect{5, 5, 43, 90}));
    check(inner_gap({0, 0, 100, 100}, {0, 0, 100, 100}, 0) == (Rect{0, 0, 100, 100}));
    check(inner_gap({0, 0, 1, 1}, {0, 0, 1, 1}, 10) == (Rect{1, 1, 0, 0}));
    bool rejected = false;
    try { outer_gap({0, 0, 5, 5}, -1); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
    rejected = false;
    try { inner_gap({0, 0, 6, 5}, {0, 0, 5, 5}, 1); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
    rejected = false;
    try { outer_gap({std::numeric_limits<int>::max(), 0, 1, 1}, 1); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
}
