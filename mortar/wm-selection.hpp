#pragma once

#include <xcb/xcb.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace mortar {

struct WmSelection {
    xcb_atom_t atom = XCB_ATOM_NONE;
    xcb_window_t window = XCB_WINDOW_NONE;
};

inline WmSelection acquire_wm_selection(xcb_connection_t *connection, xcb_window_t root, int screen) {
    using Reply = std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)>;
    auto intern = [&](const std::string &name) {
        Reply reply{xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, static_cast<uint16_t>(name.size()), name.data()), nullptr), &std::free};
        return reply ? reply->atom : xcb_atom_t{XCB_ATOM_NONE};
    };
    const auto selection = intern("WM_S" + std::to_string(screen));
    const auto manager = intern("MANAGER");
    const auto timestamp_atom = intern("_MORTAR_TIMESTAMP");
    if (!selection || !manager || !timestamp_atom) return {};

    const auto window = xcb_generate_id(connection);
    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    auto check = [&](xcb_void_cookie_t cookie) {
        std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> error{xcb_request_check(connection, cookie), &std::free};
        return !error && !xcb_connection_has_error(connection);
    };
    if (!check(xcb_create_window_checked(connection, XCB_COPY_FROM_PARENT, window, root, 0, 0, 1, 1, 0,
                                         XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask))) return {};
    auto fail = [&]() -> WmSelection { xcb_destroy_window(connection, window); return {}; };
    const uint32_t value = 0;
    if (!check(xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE, window, timestamp_atom,
                                           XCB_ATOM_INTEGER, 32, 1, &value))) return fail();
    xcb_timestamp_t timestamp = XCB_CURRENT_TIME;
    while (auto *raw = xcb_wait_for_event(connection)) {
        std::unique_ptr<xcb_generic_event_t, decltype(&std::free)> event{raw, &std::free};
        if ((event->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
            auto *property = reinterpret_cast<xcb_property_notify_event_t *>(event.get());
            if (property->window == window && property->atom == timestamp_atom) {
                timestamp = property->time;
                break;
            }
        }
    }
    if (timestamp == XCB_CURRENT_TIME) return fail();
    if (!check(xcb_grab_server_checked(connection))) return fail();
    std::unique_ptr<xcb_get_selection_owner_reply_t, decltype(&std::free)> owner{
        xcb_get_selection_owner_reply(connection, xcb_get_selection_owner(connection, selection), nullptr), &std::free};
    bool success = owner && owner->owner == XCB_WINDOW_NONE &&
                   check(xcb_set_selection_owner_checked(connection, window, selection, timestamp));
    if (success) {
        owner.reset(xcb_get_selection_owner_reply(connection, xcb_get_selection_owner(connection, selection), nullptr));
        success = owner && owner->owner == window;
    }
    if (success) {
        xcb_client_message_event_t event{};
        event.response_type = XCB_CLIENT_MESSAGE;
        event.format = 32;
        event.window = root;
        event.type = manager;
        event.data.data32[0] = timestamp;
        event.data.data32[1] = selection;
        event.data.data32[2] = window;
        success = check(xcb_send_event_checked(connection, 0, root, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                                               reinterpret_cast<const char *>(&event)));
    }
    xcb_ungrab_server(connection);
    if (xcb_flush(connection) <= 0 || !success) return fail();
    return {selection, window};
}

}
