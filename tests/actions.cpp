#include "../mortar/actions.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <sys/wait.h>

namespace {

void require(bool condition)
{
    if (!condition) throw std::runtime_error("actions test failed");
}

xcb_atom_t atom(xcb_connection_t* connection, const char* name, std::uint16_t length)
{
    auto cookie = xcb_intern_atom(connection, false, length, name);
    std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply(
        xcb_intern_atom_reply(connection, cookie, nullptr), &std::free);
    require(reply != nullptr);
    return reply->atom;
}

}

int main()
{
    pid_t child;
    require(mortar::spawn({}, child) == EINVAL);
    require(mortar::spawn({"/definitely/not/a/program"}, child) == ENOENT);
    require(mortar::spawn({"true", "; touch /tmp/mortar-no-shell"}, child) == 0);
    int status;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    require(mortar::spawn({"true"}, child) == 0);
    bool reaped = false;
    while (!reaped) require(mortar::reap_child(child, reaped) == 0);
    require(mortar::reap_child(child, reaped) == ECHILD);
    require(mortar::reap_child(-1, reaped) == EINVAL);

    auto* connection = xcb_connect(nullptr, nullptr);
    require(connection && !xcb_connection_has_error(connection));
    auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    auto window = xcb_generate_id(connection);
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 10, 10, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
    auto protocols = atom(connection, "WM_PROTOCOLS", 12);
    auto delete_window = atom(connection, "WM_DELETE_WINDOW", 16);
    require(mortar::close_window(connection, window, protocols, delete_window, XCB_CURRENT_TIME) ==
        mortar::CloseResult::unsupported);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, protocols, XCB_ATOM_ATOM, 32, 1, &delete_window);
    require(mortar::close_window(connection, window, protocols, delete_window, 42) == mortar::CloseResult::sent);
    std::unique_ptr<xcb_generic_event_t, decltype(&std::free)> event(xcb_wait_for_event(connection), &std::free);
    require(event && (event->response_type & 0x7f) == XCB_CLIENT_MESSAGE);
    auto* message = reinterpret_cast<xcb_client_message_event_t*>(event.get());
    require(message->window == window && message->type == protocols && message->format == 32 &&
        message->data.data32[0] == delete_window && message->data.data32[1] == 42);
    xcb_destroy_window(connection, window);
    require(mortar::close_window(connection, window, protocols, delete_window, 0) == mortar::CloseResult::error);
    xcb_disconnect(connection);
}
