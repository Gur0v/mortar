#include "actions.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace mortar {

CloseResult close_window(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t protocols,
    xcb_atom_t delete_window, xcb_timestamp_t timestamp)
{
    if (!connection || window == XCB_WINDOW_NONE || protocols == XCB_ATOM_NONE ||
        delete_window == XCB_ATOM_NONE) return CloseResult::error;
    auto cookie = xcb_get_property(connection, false, window, protocols, XCB_ATOM_ATOM, 0, 4096);
    xcb_generic_error_t* raw_error = nullptr;
    std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply(
        xcb_get_property_reply(connection, cookie, &raw_error), &std::free);
    std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> error(raw_error, &std::free);
    if (!reply || error) return CloseResult::error;
    if (reply->type != XCB_ATOM_ATOM || reply->format != 32 || reply->bytes_after != 0)
        return CloseResult::unsupported;
    auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(reply.get()));
    auto count = xcb_get_property_value_length(reply.get()) / sizeof(xcb_atom_t);
    if (std::find(atoms, atoms + count, delete_window) == atoms + count) return CloseResult::unsupported;
    xcb_client_message_event_t event{};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = protocols;
    event.data.data32[0] = delete_window;
    event.data.data32[1] = timestamp;
    auto request = xcb_send_event_checked(connection, false, window, XCB_EVENT_MASK_NO_EVENT,
        reinterpret_cast<const char*>(&event));
    std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> send_error(xcb_request_check(connection, request), &std::free);
    if (send_error || xcb_connection_has_error(connection)) return CloseResult::error;
    return CloseResult::sent;
}

int spawn(const std::vector<std::string>& arguments, pid_t& child)
{
    child = -1;
    if (arguments.empty() || arguments.front().empty()) return EINVAL;
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos) return EINVAL;
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    return posix_spawnp(&child, argv.front(), nullptr, nullptr, argv.data(), environ);
}

int reap_child(pid_t child, bool& reaped)
{
    reaped = false;
    if (child <= 0) return EINVAL;
    pid_t result;
    do {
        result = waitpid(child, nullptr, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return errno;
    reaped = result == child;
    return 0;
}

}
