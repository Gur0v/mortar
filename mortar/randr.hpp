#pragma once

#include "outputs.hpp"

#include <xcb/xcb.h>

namespace mortar {

std::vector<OutputGeometry> discover_outputs(xcb_connection_t *connection, xcb_window_t root);

}
