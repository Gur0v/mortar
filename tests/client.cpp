#include <xcb/xcb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

int main() {
    using Connection = std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)>;
    Connection connection{xcb_connect(nullptr, nullptr), &xcb_disconnect};
    if (!connection || xcb_connection_has_error(connection.get())) return EXIT_FAILURE;
    auto *c = connection.get();
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    if (!screen) return EXIT_FAILURE;
    auto create = [&] {
        auto window = xcb_generate_id(c);
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root,
                          10, 10, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual, 0, nullptr);
        xcb_map_window(c, window);
        xcb_flush(c);
        return window;
    };
    auto first = create();
    auto unmapped = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, unmapped, screen->root,
                      20, 20, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, nullptr);
    auto override_window = xcb_generate_id(c);
    const uint32_t override_redirect = 1;
    xcb_create_window(c, XCB_COPY_FROM_PARENT, override_window, screen->root,
                      30, 30, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
    xcb_map_window(c, override_window);
    xcb_flush(c);
    using Attributes = std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)>;
    Attributes mapped{xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, first), nullptr), &std::free};
    if (!mapped || mapped->map_state != XCB_MAP_STATE_VIEWABLE) return EXIT_FAILURE;
    std::puts("existing window ready");
    std::fflush(stdout);
    if (std::getchar() == EOF) return EXIT_FAILURE;
    auto tiled = [&](xcb_window_t window, int x, int width) {
        for (int i = 0; i < 250; ++i) {
            using Reply = std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)>;
            Reply geometry{xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
            if (!geometry) return false;
            if (geometry->x == x && geometry->y == 0 && geometry->width == width &&
                geometry->height == 600 && geometry->border_width == 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::fprintf(stderr, "window %#x not tiled at %d,0 %dx600\n", window, x, width);
        return false;
    };
    if (!tiled(first, 0, 800)) return EXIT_FAILURE;
    using Geometry = std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)>;
    Geometry override_geometry{xcb_get_geometry_reply(c, xcb_get_geometry(c, override_window), nullptr), &std::free};
    if (!override_geometry || override_geometry->x != 30 || override_geometry->y != 30 ||
        override_geometry->width != 100 || override_geometry->height != 100) return EXIT_FAILURE;
    xcb_map_window(c, unmapped);
    xcb_flush(c);
    if (!tiled(first, 0, 400) || !tiled(unmapped, 400, 400)) return EXIT_FAILURE;
    xcb_unmap_window(c, unmapped);
    xcb_flush(c);
    if (!tiled(first, 0, 800)) return EXIT_FAILURE;
    std::puts("window manager ready");
    std::fflush(stdout);
    auto second = create();
    auto third = create();
    if (!tiled(first, 0, 266) || !tiled(second, 266, 267) || !tiled(third, 533, 267)) return EXIT_FAILURE;
    const uint32_t event_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(c, second, XCB_CW_EVENT_MASK, &event_mask);
    const uint32_t requested_width = 90;
    xcb_configure_window(c, second, XCB_CONFIG_WINDOW_WIDTH, &requested_width);
    xcb_flush(c);
    bool notified = false;
    for (int i = 0; i < 250 && !notified; ++i) {
        while (auto *event = xcb_poll_for_event(c)) {
            if ((event->response_type & 0x80) && (event->response_type & 0x7f) == XCB_CONFIGURE_NOTIFY) {
                auto *reply = reinterpret_cast<xcb_configure_notify_event_t *>(event);
                notified = reply->window == second && reply->x == 266 && reply->width == 267 && reply->height == 600;
            }
            std::free(event);
        }
        if (!notified) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!notified) {
        std::fputs("missing synthetic configure notification\n", stderr);
        return EXIT_FAILURE;
    }
    xcb_warp_pointer(c, XCB_NONE, first, 0, 0, 0, 0, 10, 10);
    xcb_flush(c);
    using Pointer = std::unique_ptr<xcb_query_pointer_reply_t, decltype(&std::free)>;
    Pointer pointer{xcb_query_pointer_reply(c, xcb_query_pointer(c, screen->root), nullptr), &std::free};
    if (!pointer || pointer->child != first) return EXIT_FAILURE;
    xcb_warp_pointer(c, XCB_NONE, second, 0, 0, 0, 0, 10, 10);
    xcb_flush(c);
    bool focused = false;
    for (int i = 0; i < 250; ++i) {
        using Focus = std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)>;
        Focus reply{xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
        if (!reply) return EXIT_FAILURE;
        if (reply->focus == second) {
            focused = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!focused) {
        std::fputs("pointer entry did not focus window\n", stderr);
        return EXIT_FAILURE;
    }
    auto intern = [&](const char *name, uint16_t length) {
        std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply{
            xcb_intern_atom_reply(c, xcb_intern_atom(c, false, length, name), nullptr), &std::free};
        return reply ? reply->atom : xcb_atom_t{XCB_ATOM_NONE};
    };
    auto wm_state = intern("WM_STATE", 8);
    auto wm_change_state = intern("WM_CHANGE_STATE", 15);
    auto state_is = [&](xcb_window_t window, uint32_t expected) {
        for (int i = 0; i < 250; ++i) {
            std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
                xcb_get_property_reply(c, xcb_get_property(c, false, window, wm_state, wm_state, 0, 2), nullptr), &std::free};
            if (reply && reply->type == wm_state && reply->format == 32 &&
                xcb_get_property_value_length(reply.get()) == 8 &&
                static_cast<uint32_t *>(xcb_get_property_value(reply.get()))[0] == expected) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::fprintf(stderr, "window %#x did not reach state %u\n", window, expected);
        return false;
    };
    if (!state_is(first, 1) || !state_is(second, 1)) return EXIT_FAILURE;
    xcb_client_message_event_t message{};
    message.response_type = XCB_CLIENT_MESSAGE;
    message.format = 32;
    message.window = second;
    message.type = wm_change_state;
    message.data.data32[0] = 3;
    xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&message));
    xcb_flush(c);
    if (!state_is(second, 3) || !tiled(first, 0, 400) || !tiled(third, 400, 400)) return EXIT_FAILURE;
    xcb_map_window(c, second);
    xcb_flush(c);
    if (!state_is(second, 1) || !tiled(first, 0, 266) || !tiled(third, 266, 267) ||
        !tiled(second, 533, 267)) return EXIT_FAILURE;
    xcb_unmap_window(c, second);
    xcb_flush(c);
    if (!state_is(second, 0) || !tiled(first, 0, 400) || !tiled(third, 400, 400)) return EXIT_FAILURE;
    xcb_destroy_window(c, third);
    xcb_flush(c);
    if (!tiled(first, 0, 800)) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
