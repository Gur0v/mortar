#include <xcb/xcb.h>
#include <xcb/xtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <cstring>

int main(int argc, char **argv) {
    if (std::strstr(argv[0], "x-terminal-emulator") || std::strstr(argv[0], "dmenu_run") || std::strstr(argv[0], "mortar-test-spawn")) {
        auto *path = std::getenv("MORTAR_TEST_LAUNCH");
        if (!path) return EXIT_FAILURE;
        auto *file = std::fopen(path, "a");
        if (!file) return EXIT_FAILURE;
        if (std::strstr(argv[0], "mortar-test-spawn")) {
            for (int i = 1; i < argc; ++i) std::fprintf(file, "[%s]", argv[i]);
            std::fputc('\n', file);
        } else std::fprintf(file, "%s %d\n", std::strstr(argv[0], "dmenu_run") ? "dmenu_run" : "x-terminal-emulator", argc);
        return std::fclose(file) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    using Connection = std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)>;
    Connection connection{xcb_connect(nullptr, nullptr), &xcb_disconnect};
    if (!connection || xcb_connection_has_error(connection.get())) return EXIT_FAILURE;
    auto *c = connection.get();
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    if (!screen) return EXIT_FAILURE;
    if (argc == 2) {
        auto mode = argv[1];
        auto setup = xcb_get_setup(c);
        auto count = static_cast<uint8_t>(setup->max_keycode - setup->min_keycode + 1);
        using Mapping = std::unique_ptr<xcb_get_keyboard_mapping_reply_t, decltype(&std::free)>;
        Mapping mapping{xcb_get_keyboard_mapping_reply(c, xcb_get_keyboard_mapping(c, setup->min_keycode, count), nullptr), &std::free};
        if (!mapping) return EXIT_FAILURE;
        auto symbols = xcb_get_keyboard_mapping_keysyms(mapping.get());
        auto key = [&](xcb_keysym_t symbol) -> xcb_keycode_t {
            for (int i = 0; i < count; ++i)
                for (int j = 0; j < mapping->keysyms_per_keycode; ++j)
                    if (symbols[i * mapping->keysyms_per_keycode + j] == symbol)
                        return static_cast<xcb_keycode_t>(setup->min_keycode + i);
            return 0;
        };
        if (std::strcmp(mode, "startup-failure") == 0) {
            auto window = xcb_generate_id(c);
            xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 100, 100, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
            xcb_map_window(c, window);
            xcb_flush(c);
            std::puts("startup failure ready");
            std::fflush(stdout);
            if (std::getchar() == EOF) return EXIT_FAILURE;
            using Attributes = std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)>;
            Attributes attributes{xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, window), nullptr), &std::free};
            if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE) return EXIT_FAILURE;
            for (const char *name : {"_NET_SUPPORTED", "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP", "_NET_SUPPORTING_WM_CHECK"}) {
                std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> atom{
                    xcb_intern_atom_reply(c, xcb_intern_atom(c, false, static_cast<uint16_t>(std::strlen(name)), name), nullptr), &std::free};
                if (!atom) return EXIT_FAILURE;
                std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> property{
                    xcb_get_property_reply(c, xcb_get_property(c, false, screen->root, atom->atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 1), nullptr), &std::free};
                if (!property || property->type != XCB_ATOM_NONE) {
                    std::fprintf(stderr, "startup failure left root property %s\n", name);
                    return EXIT_FAILURE;
                }
            }
            return EXIT_SUCCESS;
        }
        if (std::strcmp(mode, "grab-conflict") == 0) {
            auto one = key('1');
            if (!one) return EXIT_FAILURE;
            auto error = xcb_request_check(c, xcb_grab_key_checked(c, false, screen->root, XCB_MOD_MASK_1,
                                                                  one, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            if (error) { std::free(error); return EXIT_FAILURE; }
            auto window = xcb_generate_id(c);
            xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 100, 100, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
            xcb_map_window(c, window);
            using Attributes = std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)>;
            auto viewable = [&] {
                Attributes reply{xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, window), nullptr), &std::free};
                return reply && reply->map_state == XCB_MAP_STATE_VIEWABLE;
            };
            if (!viewable()) return EXIT_FAILURE;
            std::puts("grab conflict ready");
            std::fflush(stdout);
            return std::getchar() != EOF && viewable() ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        auto extension = xcb_get_extension_data(c, &xcb_test_id);
        if (!extension || !extension->present) return EXIT_FAILURE;
        auto alt = key(0xffe9);
        auto shift = key(0xffe1);
        auto one = key('1');
        auto two = key('2');
        auto caps = key(0xffe5);
        auto num = key(0xff7f);
        if (!alt || !shift || !one || !two || !caps || !num) return EXIT_FAILURE;
        auto press = [&](xcb_keycode_t code, bool down) {
            xcb_test_fake_input(c, down ? XCB_KEY_PRESS : XCB_KEY_RELEASE, code, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
        };
        auto tap = [&](xcb_keycode_t code) { press(code, true); press(code, false); };
        auto intern_key = [&](const char *name) {
            std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply{
                xcb_intern_atom_reply(c, xcb_intern_atom(c, false, static_cast<uint16_t>(std::strlen(name)), name), nullptr), &std::free};
            return reply ? reply->atom : xcb_atom_t{XCB_ATOM_NONE};
        };
        auto current = intern_key("_NET_CURRENT_DESKTOP");
        auto desktop = intern_key("_NET_WM_DESKTOP");
        if (!current || !desktop) return EXIT_FAILURE;
        xcb_client_message_event_t request{};
        request.response_type = XCB_CLIENT_MESSAGE;
        request.format = 32;
        request.window = screen->root;
        request.type = current;
        xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char *>(&request));
        auto wait_property = [&](xcb_window_t window, xcb_atom_t atom, uint32_t expected) {
            for (int i = 0; i < 250; ++i) {
                std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
                    xcb_get_property_reply(c, xcb_get_property(c, false, window, atom, XCB_ATOM_CARDINAL, 0, 1), nullptr), &std::free};
                if (reply && reply->type == XCB_ATOM_CARDINAL && reply->format == 32 &&
                    xcb_get_property_value_length(reply.get()) == 4 &&
                    *static_cast<uint32_t *>(xcb_get_property_value(reply.get())) == expected) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            std::fprintf(stderr, "%s: property %#x on %#x did not reach %u\n", mode, atom, window, expected);
            return false;
        };
        if (!wait_property(screen->root, current, 0)) return EXIT_FAILURE;
        auto window = xcb_generate_id(c);
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 100, 100, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        xcb_map_window(c, window);
        xcb_flush(c);
        if (!wait_property(window, desktop, 0)) return EXIT_FAILURE;
        bool focused = false;
        for (int i = 0; i < 250; ++i) {
            std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> reply{
                xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
            if (reply && reply->focus == window) { focused = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!focused) {
            std::fprintf(stderr, "%s: test window did not receive focus\n", mode);
            return EXIT_FAILURE;
        }
        if (std::strcmp(mode, "remap") == 0) {
            auto width = mapping->keysyms_per_keycode;
            auto first = symbols + (one - setup->min_keycode) * width;
            auto second = symbols + (two - setup->min_keycode) * width;
            auto error = xcb_request_check(c, xcb_change_keyboard_mapping_checked(c, 1, one, width, second));
            if (error) { std::free(error); return EXIT_FAILURE; }
            error = xcb_request_check(c, xcb_change_keyboard_mapping_checked(c, 1, two, width, first));
            if (error) { std::free(error); return EXIT_FAILURE; }
            std::swap(one, two);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (std::strcmp(mode, "group") == 0) {
            press(alt, true); tap(shift); press(alt, false); xcb_flush(c);
            std::unique_ptr<xcb_query_pointer_reply_t, decltype(&std::free)> pointer{
                xcb_query_pointer_reply(c, xcb_query_pointer(c, screen->root), nullptr), &std::free};
            if (!pointer || ((pointer->mask >> 13) & 3) != 1) return EXIT_FAILURE;
        }
        if (std::strcmp(mode, "config") == 0) {
            press(alt, true); tap(one); press(alt, false); xcb_flush(c);
            press(alt, true); tap(two); press(alt, false); xcb_flush(c);
            return wait_property(screen->root, current, 2) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        if (std::strcmp(mode, "milestone") == 0 || std::strcmp(mode, "scheme-actions") == 0) {
            bool scheme = std::strcmp(mode, "scheme-actions") == 0;
            auto shortcut = [&](xcb_keysym_t symbol, bool shifted = false) {
                auto code = key(symbol);
                if (!code) { std::fprintf(stderr, "missing keysym %u\n", symbol); return false; }
                press(alt, true);
                if (shifted) press(shift, true);
                tap(code);
                if (shifted) press(shift, false);
                press(alt, false);
                xcb_flush(c);
                return true;
            };
            auto focused_on = [&](xcb_window_t target) {
                for (int i = 0; i < 250; ++i) {
                    std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> reply{
                        xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
                    if (reply && reply->focus == target) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::fprintf(stderr, "focus did not reach %#x\n", target);
                return false;
            };
            auto tiled = [&](xcb_window_t target, int x, int y, int width, int height) {
                for (int i = 0; i < 250; ++i) {
                    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> reply{
                        xcb_get_geometry_reply(c, xcb_get_geometry(c, target), nullptr), &std::free};
                    if (reply && reply->x == x && reply->y == y && reply->width == width && reply->height == height) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::fprintf(stderr, "window %#x has wrong geometry\n", target);
                return false;
            };
            auto create = [&] {
                auto id = xcb_generate_id(c);
                xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 100, 100, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
                xcb_map_window(c, id);
                xcb_flush(c);
                return id;
            };
            if (!shortcut(scheme ? 'b' : 'v')) return EXIT_FAILURE;
            auto second = create();
            if (!tiled(window, 0, 0, 800, 300) || !tiled(second, 0, 300, 800, 300) || !focused_on(second)) return EXIT_FAILURE;
            if (!shortcut(scheme ? 'c' : 0xff52) || !focused_on(window) || !shortcut(scheme ? 'e' : 0xff54) || !focused_on(second)) return EXIT_FAILURE;
            if (!shortcut(scheme ? 'f' : 'h')) return EXIT_FAILURE;
            auto third = create();
            if (!tiled(second, 0, 300, 400, 300) || !tiled(third, 400, 300, 400, 300) || !focused_on(third)) return EXIT_FAILURE;
            if (!shortcut(scheme ? 'g' : 0xff51) || !focused_on(second) || !shortcut(scheme ? 'i' : 0xff53) || !focused_on(third)) return EXIT_FAILURE;
            auto protocols = intern_key("WM_PROTOCOLS");
            auto delete_window = intern_key("WM_DELETE_WINDOW");
            if (!protocols || !delete_window) return EXIT_FAILURE;
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, third, protocols, XCB_ATOM_ATOM, 32, 1, &delete_window);
            if (!shortcut(scheme ? 'n' : 'q', !scheme)) return EXIT_FAILURE;
            bool received = false;
            for (int i = 0; i < 250 && !received; ++i) {
                while (auto *event = xcb_poll_for_event(c)) {
                    if ((event->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
                        auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
                        received = message->window == third && message->type == protocols && message->format == 32 &&
                                   message->data.data32[0] == delete_window && message->data.data32[1] != XCB_CURRENT_TIME;
                    }
                    std::free(event);
                }
                if (!received) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!received || !tiled(third, 400, 300, 400, 300)) return EXIT_FAILURE;
            return (scheme ? shortcut('o') : shortcut(0xff0d) && shortcut('d')) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        if (std::strcmp(mode, "arbitrary") == 0) {
            auto f6 = key(0xffc3);
            if (!f6) return EXIT_FAILURE;
            press(alt, true); tap(f6); press(alt, false); xcb_flush(c);
            return wait_property(screen->root, current, 2) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        bool move = std::strncmp(mode, "move", 4) == 0;
        bool caps_lock = std::strstr(mode, "caps") || std::strstr(mode, "both");
        bool num_lock = std::strstr(mode, "num") || std::strstr(mode, "both");
        if (caps_lock) tap(caps);
        if (num_lock) tap(num);
        press(alt, true);
        if (move) press(shift, true);
        tap(two);
        if (move) press(shift, false);
        press(alt, false);
        if (caps_lock) tap(caps);
        if (num_lock) tap(num);
        xcb_flush(c);
        if (move) {
            if (!wait_property(window, desktop, 1) || !wait_property(screen->root, current, 0)) return EXIT_FAILURE;
            press(alt, true); tap(two); press(alt, false); xcb_flush(c);
        }
        if (!wait_property(screen->root, current, 1)) return EXIT_FAILURE;
        press(alt, true); tap(one); press(alt, false); xcb_flush(c);
        if (!wait_property(screen->root, current, 0)) return EXIT_FAILURE;
        return EXIT_SUCCESS;
    }
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
    xcb_warp_pointer(c, XCB_NONE, first, 0, 0, 0, 0, 10, 10);
    xcb_flush(c);
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
            if (i == 249) std::fprintf(stderr, "actual geometry: %d,%d %ux%u border %u\n", geometry->x, geometry->y, geometry->width, geometry->height, geometry->border_width);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::fprintf(stderr, "window %#x not tiled at %d,0 %dx600\n", window, x, width);
        return false;
    };
    if (!tiled(first, 0, 800)) return EXIT_FAILURE;
    bool startup_focused = false;
    for (int i = 0; i < 250; ++i) {
        std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> reply{
            xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
        if (!reply) return EXIT_FAILURE;
        if (reply->focus == first) { startup_focused = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!startup_focused) return EXIT_FAILURE;
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
    if (!tiled(second, 400, 400)) return EXIT_FAILURE;
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
    auto initially_iconic = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, initially_iconic, screen->root,
                      0, 0, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, nullptr);
    const uint32_t iconic_hints[] = {2, 0, 3};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, initially_iconic, XCB_ATOM_WM_HINTS,
                        XCB_ATOM_WM_HINTS, 32, 3, iconic_hints);
    xcb_map_window(c, initially_iconic);
    xcb_flush(c);
    if (!state_is(initially_iconic, 3) || !tiled(first, 0, 266)) return EXIT_FAILURE;
    xcb_map_window(c, initially_iconic);
    xcb_flush(c);
    if (!state_is(initially_iconic, 1)) return EXIT_FAILURE;
    xcb_destroy_window(c, initially_iconic);
    xcb_flush(c);
    if (!tiled(first, 0, 266) || !tiled(second, 266, 267)) return EXIT_FAILURE;
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
    if (!state_is(second, 1) || !tiled(first, 0, 266) || !tiled(second, 266, 267) ||
        !tiled(third, 533, 267)) return EXIT_FAILURE;
    xcb_unmap_window(c, second);
    xcb_flush(c);
    if (!state_is(second, 0) || !tiled(first, 0, 400) || !tiled(third, 400, 400)) return EXIT_FAILURE;
    xcb_destroy_window(c, third);
    xcb_flush(c);
    if (!tiled(first, 0, 800)) return EXIT_FAILURE;
    auto fourth = create();
    if (!tiled(first, 0, 400) || !tiled(fourth, 400, 400)) return EXIT_FAILURE;
    xcb_destroy_window(c, first);
    xcb_flush(c);
    if (!tiled(fourth, 0, 800)) return EXIT_FAILURE;
    auto protocols = intern("WM_PROTOCOLS", 12);
    auto take_focus = intern("WM_TAKE_FOCUS", 13);
    for (int model = 0; model < 4; ++model) {
        auto window = xcb_generate_id(c);
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root,
                          0, 0, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual, 0, nullptr);
        const uint32_t hints[] = {1, static_cast<uint32_t>(model & 1)};
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_HINTS,
                            XCB_ATOM_WM_HINTS, 32, 2, hints);
        if (model & 2) xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, protocols,
                                           XCB_ATOM_ATOM, 32, 1, &take_focus);
        xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, fourth, XCB_CURRENT_TIME);
        xcb_map_window(c, window);
        xcb_flush(c);
        if (!tiled(window, 400, 400)) return EXIT_FAILURE;
        bool message_received = false;
        bool correct_focus = false;
        for (int i = 0; i < 250; ++i) {
            while (auto *event = xcb_poll_for_event(c)) {
                if ((event->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
                    auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
                    if (message->window == window && message->type == protocols &&
                        message->format == 32 && message->data.data32[0] == take_focus &&
                        message->data.data32[1] != XCB_CURRENT_TIME) message_received = true;
                }
                std::free(event);
            }
            using Focus = std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)>;
            Focus reply{xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
            if (!reply) return EXIT_FAILURE;
            correct_focus = reply->focus == ((model & 1) ? window : fourth);
            if (correct_focus && (!(model & 2) || message_received) && i >= 10) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!correct_focus || message_received != static_cast<bool>(model & 2)) {
            std::fprintf(stderr, "focus model %d failed: focus=%d message=%d\n", model, correct_focus, message_received);
            return EXIT_FAILURE;
        }
        xcb_destroy_window(c, window);
        xcb_flush(c);
        if (!tiled(fourth, 0, 800)) return EXIT_FAILURE;
    }
    auto current_desktop = intern("_NET_CURRENT_DESKTOP", 20);
    auto window_desktop = intern("_NET_WM_DESKTOP", 15);
    auto wm_check = intern("_NET_SUPPORTING_WM_CHECK", 24);
    auto get_check = [&](xcb_window_t window) {
        std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
            xcb_get_property_reply(c, xcb_get_property(c, false, window, wm_check, XCB_ATOM_WINDOW, 0, 1), nullptr), &std::free};
        return reply && reply->type == XCB_ATOM_WINDOW && reply->format == 32 &&
                       xcb_get_property_value_length(reply.get()) == 4
                   ? *static_cast<const uint32_t *>(xcb_get_property_value(reply.get())) : uint32_t{XCB_WINDOW_NONE};
    };
    auto support = get_check(screen->root);
    if (!support || get_check(support) != support) return EXIT_FAILURE;
    auto send_desktop = [&](xcb_atom_t type, xcb_window_t window, uint32_t desktop) {
        xcb_client_message_event_t request{};
        request.response_type = XCB_CLIENT_MESSAGE;
        request.format = 32;
        request.window = window;
        request.type = type;
        request.data.data32[0] = desktop;
        xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char *>(&request));
        xcb_flush(c);
    };
    auto property_is = [&](xcb_window_t window, xcb_atom_t property, uint32_t value) {
        for (int i = 0; i < 250; ++i) {
            std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
                xcb_get_property_reply(c, xcb_get_property(c, false, window, property, XCB_ATOM_CARDINAL, 0, 1), nullptr), &std::free};
            if (reply && reply->type == XCB_ATOM_CARDINAL && reply->format == 32 &&
                xcb_get_property_value_length(reply.get()) == 4 &&
                *static_cast<uint32_t *>(xcb_get_property_value(reply.get())) == value) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    auto focused_on = [&](xcb_window_t window) {
        for (int i = 0; i < 250; ++i) {
            std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> reply{
                xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
            if (reply && reply->focus == window) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    send_desktop(current_desktop, screen->root, 1);
    if (!property_is(screen->root, current_desktop, 1)) return EXIT_FAILURE;
    auto other = create();
    if (!property_is(other, window_desktop, 1) || !tiled(other, 0, 800) || !focused_on(other)) return EXIT_FAILURE;
    send_desktop(current_desktop, screen->root, 0);
    if (!property_is(screen->root, current_desktop, 0) || !tiled(fourth, 0, 800) || !focused_on(fourth)) return EXIT_FAILURE;
    send_desktop(window_desktop, fourth, 1);
    if (!property_is(fourth, window_desktop, 1)) return EXIT_FAILURE;
    send_desktop(current_desktop, screen->root, 1);
    if (!property_is(screen->root, current_desktop, 1) || !tiled(other, 0, 400) ||
        !tiled(fourth, 400, 400) || !focused_on(fourth)) return EXIT_FAILURE;
    send_desktop(current_desktop, screen->root, 2);
    if (!property_is(screen->root, current_desktop, 2)) return EXIT_FAILURE;
    send_desktop(window_desktop, fourth, 2);
    if (!property_is(fourth, window_desktop, 2) || !focused_on(fourth)) return EXIT_FAILURE;
    send_desktop(current_desktop, screen->root, 1);
    if (!property_is(screen->root, current_desktop, 1) || !focused_on(other)) return EXIT_FAILURE;
    send_desktop(current_desktop, screen->root, 99);
    xcb_destroy_window(c, fourth);
    xcb_flush(c);
    if (!tiled(other, 0, 800) || !focused_on(other) || !property_is(screen->root, current_desktop, 1)) return EXIT_FAILURE;
    auto hidden = create();
    auto iconic = create();
    if (!property_is(hidden, window_desktop, 1) || !property_is(iconic, window_desktop, 1)) return EXIT_FAILURE;
    message.window = iconic;
    xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&message));
    xcb_flush(c);
    if (!state_is(iconic, 3)) return EXIT_FAILURE;
    send_desktop(window_desktop, hidden, 2);
    if (!property_is(hidden, window_desktop, 2)) return EXIT_FAILURE;
    auto map_is = [&](xcb_window_t window, uint8_t expected) {
        for (int i = 0; i < 250; ++i) {
            Attributes reply{xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, window), nullptr), &std::free};
            if (reply && reply->map_state == expected) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::fprintf(stderr, "window %#x did not reach map state %u\n", window, expected);
        return false;
    };
    auto check = [&](bool hidden_visible) {
        return property_is(screen->root, current_desktop, 1) &&
               property_is(other, window_desktop, 1) && property_is(hidden, window_desktop, 2) &&
               property_is(iconic, window_desktop, 1) && state_is(other, 1) &&
               state_is(hidden, 1) && state_is(iconic, 3) &&
               map_is(other, XCB_MAP_STATE_VIEWABLE) &&
               map_is(hidden, hidden_visible ? XCB_MAP_STATE_VIEWABLE : XCB_MAP_STATE_UNMAPPED) &&
               map_is(iconic, XCB_MAP_STATE_UNMAPPED);
    };
    if (!check(false)) return EXIT_FAILURE;
    std::puts("lifecycle ready");
    std::fflush(stdout);
    if (std::getchar() == EOF || !check(true)) return EXIT_FAILURE;
    std::puts("shutdown checked");
    std::fflush(stdout);
    if (std::getchar() == EOF || !check(false)) return EXIT_FAILURE;
    send_desktop(current_desktop, screen->root, 2);
    if (!property_is(screen->root, current_desktop, 2) || !map_is(hidden, XCB_MAP_STATE_VIEWABLE) ||
        !map_is(other, XCB_MAP_STATE_UNMAPPED) || !map_is(iconic, XCB_MAP_STATE_UNMAPPED)) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
