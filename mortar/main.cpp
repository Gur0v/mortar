#include <xcb/xcb.h>

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
    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
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
    std::vector<xcb_window_t> windows;
    auto tile = [&] {
        if (windows.empty()) return;
        const uint32_t width = screens.data->width_in_pixels;
        const uint32_t height = screens.data->height_in_pixels;
        for (size_t i = 0; i < windows.size(); ++i) {
            const uint32_t left = width * i / windows.size();
            const uint32_t right = width * (i + 1) / windows.size();
            const uint32_t values[] = {left, 0, std::max(1u, right - left), height, 0};
            xcb_configure_window(c, windows[i], XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                                 XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
        }
        xcb_flush(c);
    };
    auto manage = [&](xcb_window_t window, bool existing) {
        if (std::find(windows.begin(), windows.end(), window) != windows.end()) {
            if (!existing) {
                xcb_map_window(c, window);
                xcb_flush(c);
            }
            return;
        }
        auto cookie = xcb_get_window_attributes(c, window);
        std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> attributes{
            xcb_get_window_attributes_reply(c, cookie, nullptr), &std::free};
        if (!attributes || attributes->override_redirect ||
            (existing && attributes->map_state != XCB_MAP_STATE_VIEWABLE)) return;
        windows.push_back(window);
        tile();
        if (!existing) xcb_map_window(c, window);
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
        switch (event->response_type & 0x7f) {
        case XCB_MAP_REQUEST:
            manage(reinterpret_cast<xcb_map_request_event_t *>(event)->window, false);
            break;
        case XCB_UNMAP_NOTIFY:
        case XCB_DESTROY_NOTIFY: {
            auto window = reinterpret_cast<xcb_unmap_notify_event_t *>(event)->window;
            auto old_size = windows.size();
            std::erase(windows, window);
            if (windows.size() != old_size) tile();
            break;
        }
        case XCB_CONFIGURE_REQUEST: {
            auto *request = reinterpret_cast<xcb_configure_request_event_t *>(event);
            if (std::find(windows.begin(), windows.end(), request->window) != windows.end()) break;
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
