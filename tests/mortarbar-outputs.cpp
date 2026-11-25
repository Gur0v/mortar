#include "../mortar/randr.hpp"

namespace mortar {
std::vector<OutputGeometry> discover_outputs(xcb_connection_t*, xcb_window_t) {
    return {{"left", {0, 0, 400, 600}}, {"right", {400, 0, 400, 600}},
            {"lower", {0, 300, 800, 200}}};
}
}
