#pragma once

#include <xcb/xcb.h>

#include <string>
#include <sys/types.h>
#include <vector>

namespace mortar {

enum class CloseResult { sent, unsupported, error };

CloseResult close_window(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t protocols,
    xcb_atom_t delete_window, xcb_timestamp_t timestamp);

int spawn(const std::vector<std::string>& arguments, pid_t& child);
int reap_child(pid_t child, bool& reaped);

}
