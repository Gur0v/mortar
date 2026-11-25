#include "layout.hpp"

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

int main() {
    int screen_number = 0;
    using Connection = std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)>;
    Connection connection{xcb_connect(nullptr, &screen_number), &xcb_disconnect};
    if (!connection || xcb_connection_has_error(connection.get())) {
        std::fputs("mortar: cannot connect to X server\n", stderr);
        return EXIT_FAILURE;
    }

    auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection.get()));
    while (screen_number-- > 0 && screens.rem > 0) {
        xcb_screen_next(&screens);
    }
    if (screens.rem == 0) {
        std::fputs("mortar: X screen not found\n", stderr);
        return EXIT_FAILURE;
    }

    xcb_grab_server(connection.get());
    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                          XCB_EVENT_MASK_PROPERTY_CHANGE;
    auto cookie = xcb_change_window_attributes_checked(connection.get(), screens.data->root,
                                                        XCB_CW_EVENT_MASK, &mask);
    using Error = std::unique_ptr<xcb_generic_error_t, decltype(&std::free)>;
    Error error{xcb_request_check(connection.get(), cookie), &std::free};
    if (error) {
        std::fprintf(stderr, "mortar: cannot acquire window management (X error %u)\n", error->error_code);
        return EXIT_FAILURE;
    }

    auto *c = connection.get();
    const auto root = screens.data->root;
    auto layout = mortar::Layout::split(mortar::Split::horizontal);
    std::vector<xcb_window_t> iconic;
    std::vector<xcb_window_t> pending_unmaps;
    auto atom = [&](const char *name, uint16_t length) {
        std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply{
            xcb_intern_atom_reply(c, xcb_intern_atom(c, false, length, name), nullptr), &std::free};
        return reply ? reply->atom : xcb_atom_t{XCB_ATOM_NONE};
    };
    const auto wm_state = atom("WM_STATE", 8);
    const auto wm_change_state = atom("WM_CHANGE_STATE", 15);
    if (wm_state == XCB_ATOM_NONE || wm_change_state == XCB_ATOM_NONE) {
        std::fputs("mortar: cannot intern window state atoms\n", stderr);
        return EXIT_FAILURE;
    }
    auto set_state = [&](xcb_window_t window, uint32_t state) {
        const uint32_t value[] = {state, XCB_NONE};
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, wm_state, wm_state, 32, 2, value);
    };
    uint32_t width = screens.data->width_in_pixels;
    uint32_t height = screens.data->height_in_pixels;
    const auto *randr = xcb_get_extension_data(c, &xcb_randr_id);
    if (!randr || !randr->present) {
        std::fputs("mortar: RandR extension unavailable\n", stderr);
        return EXIT_FAILURE;
    }
    xcb_randr_select_input(c, root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    const auto protocols = atom("WM_PROTOCOLS", 12);
    const auto take_focus = atom("WM_TAKE_FOCUS", 13);
    const auto focus_time = atom("_MORTAR_FOCUS_TIME", 18);
    if (!protocols || !take_focus || !focus_time) {
        std::fputs("mortar: cannot initialize focus atoms\n", stderr);
        return EXIT_FAILURE;
    }
    auto focus = [&](xcb_window_t window, xcb_timestamp_t time) {
        using Property = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;
        Property hints{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_HINTS,
                                                                  XCB_ATOM_WM_HINTS, 0, 2), nullptr), &std::free};
        bool input = true;
        if (hints && hints->type == XCB_ATOM_WM_HINTS && hints->format == 32 &&
            xcb_get_property_value_length(hints.get()) >= 8) {
            auto *values = static_cast<const uint32_t *>(xcb_get_property_value(hints.get()));
            if (values[0] & 1) input = values[1] != 0;
        }
        Property supported{xcb_get_property_reply(c, xcb_get_property(c, false, window, protocols,
                                                                      XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
        if (input) xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, window, time);
        if (supported && supported->type == XCB_ATOM_ATOM && supported->format == 32) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(supported.get()));
            auto count = xcb_get_property_value_length(supported.get()) / 4;
            if (std::find(values, values + count, take_focus) != values + count) {
                xcb_client_message_event_t message{};
                message.response_type = XCB_CLIENT_MESSAGE;
                message.format = 32;
                message.window = window;
                message.type = protocols;
                message.data.data32[0] = take_focus;
                message.data.data32[1] = time;
                xcb_send_event(c, false, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&message));
            }
        }
        xcb_flush(c);
    };
    auto tile = [&] {
        for (const auto &placement : layout.geometry({0, 0, static_cast<int>(width), static_cast<int>(height)})) {
            const uint32_t values[] = {static_cast<uint32_t>(placement.bounds.x),
                                       static_cast<uint32_t>(placement.bounds.y),
                                       static_cast<uint32_t>(std::max(1, placement.bounds.width)),
                                       static_cast<uint32_t>(std::max(1, placement.bounds.height)), 0};
            xcb_configure_window(c, placement.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                                 XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
        }
        xcb_flush(c);
    };
    std::vector<xcb_window_t> pending_focus;
    auto manage = [&](xcb_window_t window, bool existing) {
        if (layout.contains(window)) {
            if (!existing) {
                xcb_map_window(c, window);
                xcb_flush(c);
            }
            return;
        }
        auto cookie = xcb_get_window_attributes(c, window);
        std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> attributes{
            xcb_get_window_attributes_reply(c, cookie, nullptr), &std::free};
        if (!attributes) return;
        if (attributes->override_redirect) {
            if (!existing) xcb_map_window(c, window);
            return;
        }
        if (existing && attributes->map_state != XCB_MAP_STATE_VIEWABLE) return;
        const uint32_t event_mask = XCB_EVENT_MASK_ENTER_WINDOW;
        xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &event_mask);
        std::erase(iconic, window);
        if (!layout.insert(mortar::Layout::leaf(window))) return;
        set_state(window, 1);
        tile();
        if (!existing) {
            xcb_map_window(c, window);
            pending_focus.push_back(window);
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr);
        }
        xcb_flush(c);
    };
    auto tree_cookie = xcb_query_tree(c, root);
    std::unique_ptr<xcb_query_tree_reply_t, decltype(&std::free)> tree{
        xcb_query_tree_reply(c, tree_cookie, nullptr), &std::free};
    if (!tree) {
        std::fputs("mortar: cannot query root windows\n", stderr);
        return EXIT_FAILURE;
    }
    auto *children = xcb_query_tree_children(tree.get());
    for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
        manage(children[i], true);
    }

    xcb_ungrab_server(c);
    xcb_flush(c);

    while (auto *event = xcb_wait_for_event(c)) {
        const auto type = event->response_type & 0x7f;
        if (type == randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
            auto *change = reinterpret_cast<xcb_randr_screen_change_notify_event_t *>(event);
            width = change->width;
            height = change->height;
            tile();
        }
        switch (type) {
        case XCB_ENTER_NOTIFY: {
            auto *enter = reinterpret_cast<xcb_enter_notify_event_t *>(event);
            if (enter->mode == XCB_NOTIFY_MODE_NORMAL && enter->detail != XCB_NOTIFY_DETAIL_INFERIOR &&
                layout.contains(enter->event)) {
                if (enter->time != XCB_CURRENT_TIME) focus(enter->event, enter->time);
            }
            break;
        }
        case XCB_PROPERTY_NOTIFY: {
            auto *property = reinterpret_cast<xcb_property_notify_event_t *>(event);
            if (!(event->response_type & 0x80) && property->window == root && property->atom == focus_time &&
                property->state == XCB_PROPERTY_NEW_VALUE && !pending_focus.empty()) {
                auto window = pending_focus.front();
                pending_focus.erase(pending_focus.begin());
                if (property->time != XCB_CURRENT_TIME &&
                    layout.contains(window)) focus(window, property->time);
            }
            break;
        }
        case XCB_MAP_REQUEST:
            manage(reinterpret_cast<xcb_map_request_event_t *>(event)->window, false);
            break;
        case XCB_CLIENT_MESSAGE: {
            auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
            if (message->type == wm_change_state && message->format == 32 && message->data.data32[0] == 3 &&
                layout.contains(message->window)) {
                layout.remove(message->window);
                iconic.push_back(message->window);
                pending_unmaps.push_back(message->window);
                set_state(message->window, 3);
                xcb_unmap_window(c, message->window);
                tile();
                xcb_flush(c);
            }
            break;
        }
        case XCB_UNMAP_NOTIFY:
        case XCB_DESTROY_NOTIFY: {
            auto window = reinterpret_cast<xcb_unmap_notify_event_t *>(event)->window;
            if (type == XCB_UNMAP_NOTIFY && !(event->response_type & 0x80)) {
                auto pending = std::find(pending_unmaps.begin(), pending_unmaps.end(), window);
                if (pending != pending_unmaps.end()) {
                    pending_unmaps.erase(pending);
                    break;
                }
            }
            bool was_tiled = layout.remove(window);
            auto removed = static_cast<size_t>(was_tiled) + std::erase(iconic, window);
            if (type == XCB_DESTROY_NOTIFY) std::erase(pending_unmaps, window);
            if (removed && type == XCB_UNMAP_NOTIFY) {
                set_state(window, 0);
                xcb_flush(c);
            }
            if (was_tiled) tile();
            break;
        }
        case XCB_CONFIGURE_REQUEST: {
            auto *request = reinterpret_cast<xcb_configure_request_event_t *>(event);
            if (layout.contains(request->window)) {
                auto cookie = xcb_get_geometry(c, request->window);
                std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                    xcb_get_geometry_reply(c, cookie, nullptr), &std::free};
                if (geometry) {
                    xcb_configure_notify_event_t notify{};
                    notify.response_type = XCB_CONFIGURE_NOTIFY;
                    notify.event = request->window;
                    notify.window = request->window;
                    notify.x = geometry->x;
                    notify.y = geometry->y;
                    notify.width = geometry->width;
                    notify.height = geometry->height;
                    notify.border_width = geometry->border_width;
                    xcb_send_event(c, false, request->window, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                                   reinterpret_cast<const char *>(&notify));
                    xcb_flush(c);
                }
                break;
            }
            uint32_t values[7];
            unsigned int n = 0;
            if (request->value_mask & XCB_CONFIG_WINDOW_X) values[n++] = request->x;
            if (request->value_mask & XCB_CONFIG_WINDOW_Y) values[n++] = request->y;
            if (request->value_mask & XCB_CONFIG_WINDOW_WIDTH) values[n++] = request->width;
            if (request->value_mask & XCB_CONFIG_WINDOW_HEIGHT) values[n++] = request->height;
            if (request->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[n++] = request->border_width;
            if (request->value_mask & XCB_CONFIG_WINDOW_SIBLING) values[n++] = request->sibling;
            if (request->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) values[n++] = request->stack_mode;
            xcb_configure_window(c, request->window, request->value_mask, values);
            xcb_flush(c);
            break;
        }
        }
        std::free(event);
    }
    std::fputs("mortar: X connection closed\n", stderr);
    return EXIT_FAILURE;
}
