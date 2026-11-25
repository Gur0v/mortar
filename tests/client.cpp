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
    if (argc == 2 && std::strcmp(argv[1], "ipc-window") == 0) {
        auto window = xcb_generate_id(c);
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 100, 100, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        xcb_map_window(c, window);
        xcb_flush(c);
        std::printf("%u\n", window);
        std::fflush(stdout);
        std::getchar();
        xcb_destroy_window(c, window);
        xcb_flush(c);
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::strcmp(argv[1], "borders") == 0) {
        bool rgb_visual = false;
        for (auto depths = xcb_screen_allowed_depths_iterator(screen); depths.rem; xcb_depth_next(&depths)) {
            if (depths.data->depth != 24) continue;
            for (auto visuals = xcb_depth_visuals_iterator(depths.data); visuals.rem; xcb_visualtype_next(&visuals)) {
                auto *visual = visuals.data;
                if (visual->visual_id == screen->root_visual && visual->_class == XCB_VISUAL_CLASS_TRUE_COLOR &&
                    visual->red_mask == 0xff0000 && visual->green_mask == 0xff00 && visual->blue_mask == 0xff)
                    rgb_visual = true;
            }
        }
        if (!rgb_visual || xcb_get_setup(c)->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST) {
            std::fprintf(stderr, "borders: expected 24-bit little-endian RGB visual\n");
            return EXIT_FAILURE;
        }
        auto make_window = [&](int x, int y) {
            auto window = xcb_generate_id(c);
            xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, x, y, 100, 100, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
            return window;
        };
        auto first = make_window(0, 0);
        auto second = make_window(200, 100);
        auto dialog = [&] {
            std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> type{
                xcb_intern_atom_reply(c, xcb_intern_atom(c, false, 19, "_NET_WM_WINDOW_TYPE"), nullptr), &std::free};
            std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> value{
                xcb_intern_atom_reply(c, xcb_intern_atom(c, false, 26, "_NET_WM_WINDOW_TYPE_DIALOG"), nullptr), &std::free};
            if (!type || !value) return false;
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, second, type->atom, XCB_ATOM_ATOM, 32, 1, &value->atom);
            return true;
        };
        if (!dialog()) return EXIT_FAILURE;
        xcb_map_window(c, first);
        xcb_flush(c);
        auto wait = [&](auto predicate) {
            for (int i = 0; i < 100; ++i) {
                if (predicate()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return false;
        };
        auto border = [&](xcb_window_t window, uint16_t width) {
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
            return geometry && geometry->border_width == width;
        };
        if (!wait([&] { return border(first, 3); })) return EXIT_FAILURE;
        xcb_map_window(c, second);
        xcb_flush(c);
        if (!wait([&] { return border(second, 3); })) return EXIT_FAILURE;
        auto pixel = [&](int16_t x, int16_t y, uint32_t expected) {
            std::unique_ptr<xcb_get_image_reply_t, decltype(&std::free)> image{
                xcb_get_image_reply(c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, screen->root,
                                                    x, y, 1, 1, ~uint32_t{0}), nullptr), &std::free};
            if (!image || xcb_get_image_data_length(image.get()) < 4) return false;
            const auto *data = xcb_get_image_data(image.get());
            const uint32_t value = uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16);
            return value == expected;
        };
        if (!wait([&] { return pixel(0, 100, 0x654321) && pixel(200, 100, 0x123456); })) {
            std::fprintf(stderr, "borders: focused or unfocused color mismatch\n");
            return EXIT_FAILURE;
        }
        const uint32_t requested = 9;
        xcb_configure_window(c, second, XCB_CONFIG_WINDOW_BORDER_WIDTH, &requested);
        xcb_flush(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!border(second, 3)) return EXIT_FAILURE;
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::strcmp(argv[1], "bar-reservation") == 0) {
        auto window = xcb_generate_id(c);
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 100, 100, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        xcb_map_window(c, window);
        xcb_flush(c);
        for (int i = 0; i < 100; ++i) {
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
            if (geometry && geometry->y == 24 && geometry->height == 576 && geometry->width == 800) return EXIT_SUCCESS;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return EXIT_FAILURE;
    }
    if (argc == 2 && std::strcmp(argv[1], "bar-workspaces") == 0) {
        auto atom = [&](const char *name) {
            std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply{
                xcb_intern_atom_reply(c, xcb_intern_atom(c, false, static_cast<uint16_t>(std::strlen(name)), name), nullptr), &std::free};
            return reply ? reply->atom : xcb_atom_t{XCB_ATOM_NONE};
        };
        auto names = atom("_NET_DESKTOP_NAMES");
        auto utf8 = atom("UTF8_STRING");
        auto current = atom("_NET_CURRENT_DESKTOP");
        auto supported = atom("_NET_SUPPORTED");
        if (!names || !utf8 || !current || !supported) {
            std::fprintf(stderr, "bar-workspaces: failed to intern EWMH atoms\n");
            return EXIT_FAILURE;
        }
        std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> property{
            xcb_get_property_reply(c, xcb_get_property(c, false, screen->root, names, utf8, 0, 256), nullptr), &std::free};
        constexpr char expected[] = "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0" "8\0" "9\0" "10\0";
        if (!property || property->type != utf8 || property->format != 8 ||
            xcb_get_property_value_length(property.get()) != sizeof(expected) - 1 ||
            std::memcmp(xcb_get_property_value(property.get()), expected, sizeof(expected) - 1) != 0) {
            std::fprintf(stderr, "bar-workspaces: invalid _NET_DESKTOP_NAMES (type=%u format=%u length=%d)\n",
                         property ? property->type : 0, property ? property->format : 0,
                         property ? xcb_get_property_value_length(property.get()) : -1);
            return EXIT_FAILURE;
        }
        property.reset(xcb_get_property_reply(c, xcb_get_property(c, false, screen->root, supported, XCB_ATOM_ATOM, 0, 256), nullptr));
        if (!property || property->type != XCB_ATOM_ATOM || property->format != 32) {
            std::fprintf(stderr, "bar-workspaces: invalid _NET_SUPPORTED\n");
            return EXIT_FAILURE;
        }
        auto *atoms = static_cast<xcb_atom_t *>(xcb_get_property_value(property.get()));
        bool advertised = false;
        for (int i = 0; i < xcb_get_property_value_length(property.get()) / 4; ++i) advertised |= atoms[i] == names;
        if (!advertised) {
            std::fprintf(stderr, "bar-workspaces: _NET_DESKTOP_NAMES not advertised\n");
            return EXIT_FAILURE;
        }
        int x = 8;
        for (int workspace = 1; workspace < 10; ++workspace) {
            bool changed = false;
            for (; x < screen->width_in_pixels && !changed; ++x) {
            xcb_test_fake_input(c, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, screen->root, x, 12, 0);
            xcb_test_fake_input(c, XCB_BUTTON_PRESS, 1, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
            xcb_test_fake_input(c, XCB_BUTTON_RELEASE, 1, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
            xcb_flush(c);
            for (int i = 0; i < 5; ++i) {
                property.reset(xcb_get_property_reply(c, xcb_get_property(c, false, screen->root, current, XCB_ATOM_CARDINAL, 0, 1), nullptr));
                if (property && property->type == XCB_ATOM_CARDINAL && property->format == 32 &&
                    xcb_get_property_value_length(property.get()) == 4 &&
                    *static_cast<uint32_t *>(xcb_get_property_value(property.get())) == static_cast<uint32_t>(workspace)) {
                    changed = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            }
            if (!changed) {
                std::fprintf(stderr, "bar-workspaces: desktop %d not reached after clicks through x=%d\n", workspace, x - 1);
                return EXIT_FAILURE;
            }
        }
        return EXIT_SUCCESS;
    }
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
        if (std::strcmp(mode, "reload-keys") == 0) {
            auto alt = key(0xffe9);
            auto shift = key(0xffe1);
            if (!alt || !shift || !key('c') || !key('z') || !key('x')) return EXIT_FAILURE;
            std::puts("ready");
            std::fflush(stdout);
            for (int input; (input = std::getchar()) != EOF;) {
                if (input == '\n') continue;
                auto code = key(static_cast<xcb_keysym_t>(input));
                if (!code) return EXIT_FAILURE;
                auto press = [&](xcb_keycode_t k, bool down) {
                    xcb_test_fake_input(c, down ? XCB_KEY_PRESS : XCB_KEY_RELEASE, k, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
                };
                press(alt, true);
                if (input == 'c') press(shift, true);
                press(code, true);
                press(code, false);
                if (input == 'c') press(shift, false);
                press(alt, false);
                xcb_flush(c);
                std::puts("sent");
                std::fflush(stdout);
            }
            return EXIT_SUCCESS;
        }
        if (std::strcmp(mode, "reload-conflict") == 0) {
            auto code = key('z');
            if (!code) return EXIT_FAILURE;
            auto *error = xcb_request_check(c, xcb_grab_key_checked(c, false, screen->root, XCB_MOD_MASK_1,
                                                                   code, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            if (error) { std::free(error); return EXIT_FAILURE; }
            std::puts("ready");
            std::fflush(stdout);
            return std::getchar() == EOF ? EXIT_FAILURE : EXIT_SUCCESS;
        }
        if (std::strcmp(mode, "startup-failure") == 0) {
            auto one = key('1');
            if (!one) return EXIT_FAILURE;
            auto *error = xcb_request_check(c, xcb_grab_key_checked(c, false, screen->root, XCB_MOD_MASK_1,
                                                                    one, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            if (error) { std::free(error); return EXIT_FAILURE; }
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
            for (const char *name : {"_NET_SUPPORTED", "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP", "_NET_DESKTOP_NAMES", "_NET_SUPPORTING_WM_CHECK"}) {
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
        if (std::strcmp(mode, "scratchpad") == 0) {
            auto minus = key('-');
            if (!minus) return EXIT_FAILURE;
            auto shortcut = [&](bool shifted) {
                press(alt, true);
                if (shifted) press(shift, true);
                tap(minus);
                if (shifted) press(shift, false);
                press(alt, false);
                xcb_flush(c);
            };
            auto wait = [&](auto predicate, const char *label) {
                for (int i = 0; i < 250; ++i) {
                    if (predicate()) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::fprintf(stderr, "scratchpad: %s failed\n", label);
                return false;
            };
            auto mapped = [&](xcb_window_t target, uint8_t state) {
                std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> reply{
                    xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, target), nullptr), &std::free};
                return reply && reply->map_state == state;
            };
            auto focus = [&](xcb_window_t target) {
                std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> reply{
                    xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
                return reply && reply->focus == target;
            };
            auto geometry = [&](xcb_window_t target, int x, int y, int w, int h) {
                std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> reply{
                    xcb_get_geometry_reply(c, xcb_get_geometry(c, target), nullptr), &std::free};
                return reply && reply->x == x && reply->y == y && reply->width == w && reply->height == h;
            };
            auto other = xcb_generate_id(c);
            xcb_create_window(c, XCB_COPY_FROM_PARENT, other, screen->root, 0, 0, 100, 100, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
            xcb_map_window(c, other); xcb_flush(c);
            if (!wait([&] { return focus(other); }, "second window focus")) return EXIT_FAILURE;
            shortcut(true);
            if (!wait([&] { return mapped(other, XCB_MAP_STATE_UNMAPPED) && focus(window) &&
                                  geometry(window, 0, 0, 800, 600); }, "send tiled window")) return EXIT_FAILURE;
            press(alt, true); tap(key('f')); press(alt, false); xcb_flush(c);
            auto state = intern_key("_NET_WM_STATE");
            auto full = intern_key("_NET_WM_STATE_FULLSCREEN");
            auto fullscreen = [&] {
                std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
                    xcb_get_property_reply(c, xcb_get_property(c, false, window, state, XCB_ATOM_ATOM, 0, 64), nullptr), &std::free};
                if (!reply || reply->type != XCB_ATOM_ATOM || reply->format != 32) return false;
                auto *atoms = static_cast<const xcb_atom_t *>(xcb_get_property_value(reply.get()));
                return std::find(atoms, atoms + xcb_get_property_value_length(reply.get()) / 4, full) !=
                    atoms + xcb_get_property_value_length(reply.get()) / 4;
            };
            if (!wait(fullscreen, "enter fullscreen")) return EXIT_FAILURE;
            shortcut(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!mapped(other, XCB_MAP_STATE_UNMAPPED) || !focus(window)) return EXIT_FAILURE;
            press(alt, true); tap(key('f')); press(alt, false); xcb_flush(c);
            if (!wait([&] { return !fullscreen(); }, "leave fullscreen")) return EXIT_FAILURE;
            shortcut(false);
            if (!wait([&] { return mapped(other, XCB_MAP_STATE_VIEWABLE) && focus(other) &&
                                  geometry(other, 200, 150, 400, 300); }, "show floating window")) return EXIT_FAILURE;
            const uint32_t rect[] = {70, 80, 240, 180};
            xcb_configure_window(c, other, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, rect);
            xcb_flush(c);
            if (!wait([&] { return geometry(other, 70, 80, 240, 180); }, "resize floating window")) return EXIT_FAILURE;
            shortcut(false);
            if (!wait([&] { return mapped(other, XCB_MAP_STATE_UNMAPPED) && focus(window); }, "hide and restore focus")) return EXIT_FAILURE;
            press(alt, true); tap(two); press(alt, false); xcb_flush(c);
            if (!wait_property(screen->root, current, 1)) return EXIT_FAILURE;
            shortcut(false);
            if (!wait([&] { return mapped(other, XCB_MAP_STATE_VIEWABLE) && focus(other) &&
                                  geometry(other, 70, 80, 240, 180); }, "show on another workspace")) return EXIT_FAILURE;
            xcb_client_message_event_t message{};
            message.response_type = XCB_CLIENT_MESSAGE;
            message.format = 32;
            message.window = other;
            message.type = desktop;
            message.data.data32[0] = 0;
            xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                           reinterpret_cast<const char *>(&message));
            xcb_flush(c);
            if (!wait_property(other, desktop, 0) || !wait([&] { return mapped(other, XCB_MAP_STATE_UNMAPPED); }, "move shown scratchpad")) return EXIT_FAILURE;
            shortcut(false);
            press(alt, true); tap(key('1')); press(alt, false); xcb_flush(c);
            if (!wait_property(screen->root, current, 0) ||
                !wait([&] { return mapped(other, XCB_MAP_STATE_VIEWABLE); }, "moved window remains on workspace")) return EXIT_FAILURE;
            if (!wait([&] { return focus(other); }, "focus moved window")) return EXIT_FAILURE;
            shortcut(true);
            if (!wait([&] { return mapped(other, XCB_MAP_STATE_UNMAPPED); }, "send moved window again")) return EXIT_FAILURE;
            shortcut(false);
            if (!wait([&] { return mapped(other, XCB_MAP_STATE_VIEWABLE) && focus(other); }, "show moved window again")) return EXIT_FAILURE;
            message.type = intern_key("WM_CHANGE_STATE");
            message.data.data32[0] = 3;
            xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                           reinterpret_cast<const char *>(&message));
            xcb_flush(c);
            if (!wait([&] { return mapped(other, XCB_MAP_STATE_UNMAPPED); }, "iconify scratchpad")) return EXIT_FAILURE;
            shortcut(false);
            xcb_map_window(c, other); xcb_flush(c);
            if (!wait([&] { return mapped(other, XCB_MAP_STATE_VIEWABLE); }, "restore iconified window")) return EXIT_FAILURE;
            xcb_destroy_window(c, other); xcb_flush(c);
            return EXIT_SUCCESS;
        }
        if (std::strcmp(mode, "fullscreen") == 0) {
            auto state = intern_key("_NET_WM_STATE");
            auto full = intern_key("_NET_WM_STATE_FULLSCREEN");
            auto f = key('f');
            if (!state || !full || !f) return EXIT_FAILURE;
            auto wait = [&](auto predicate, const char *label) {
                for (int i = 0; i < 250; ++i) {
                    if (predicate()) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::fprintf(stderr, "fullscreen: %s failed\n", label);
                return false;
            };
            auto geometry = [&](xcb_window_t target, int x, int y, int w, int h) {
                std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> reply{
                    xcb_get_geometry_reply(c, xcb_get_geometry(c, target), nullptr), &std::free};
                return reply && reply->x == x && reply->y == y && reply->width == w && reply->height == h;
            };
            auto has_full = [&](xcb_window_t target) {
                std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
                    xcb_get_property_reply(c, xcb_get_property(c, false, target, state, XCB_ATOM_ATOM, 0, 64), nullptr), &std::free};
                if (!reply || reply->type != XCB_ATOM_ATOM || reply->format != 32) return false;
                auto *atoms = static_cast<const xcb_atom_t *>(xcb_get_property_value(reply.get()));
                for (int i = 0; i < xcb_get_property_value_length(reply.get()) / 4; ++i) if (atoms[i] == full) return true;
                return false;
            };
            auto send = [&](xcb_window_t target, uint32_t operation) {
                xcb_client_message_event_t event{};
                event.response_type = XCB_CLIENT_MESSAGE; event.format = 32; event.window = target;
                event.type = state; event.data.data32[0] = operation; event.data.data32[1] = full;
                xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                               reinterpret_cast<const char *>(&event));
                xcb_flush(c);
            };
            send(window, 1);
            if (!wait([&] { return has_full(window) && geometry(window, 0, 0, 800, 600); }, "EWMH add")) return EXIT_FAILURE;
            auto other = xcb_generate_id(c);
            xcb_create_window(c, XCB_COPY_FROM_PARENT, other, screen->root, 20, 30, 120, 100, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
            xcb_map_window(c, other); xcb_flush(c);
            if (!wait_property(other, desktop, 0)) return EXIT_FAILURE;
            if (!wait([&] {
                std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> focus{
                    xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
                std::unique_ptr<xcb_query_tree_reply_t, decltype(&std::free)> tree{
                    xcb_query_tree_reply(c, xcb_query_tree(c, screen->root), nullptr), &std::free};
                if (!focus || focus->focus != window || !tree) return false;
                auto *children = xcb_query_tree_children(tree.get());
                int a = -1, b = -1;
                for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
                    if (children[i] == window) a = i;
                    if (children[i] == other) b = i;
                }
                return a > b && b >= 0;
            }, "focus and stacking")) return EXIT_FAILURE;
            auto dialog = xcb_generate_id(c);
            xcb_create_window(c, XCB_COPY_FROM_PARENT, dialog, screen->root, 40, 50, 200, 150, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, dialog, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &window);
            xcb_map_window(c, dialog); xcb_flush(c);
            if (!wait([&] {
                std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> focus{
                    xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
                std::unique_ptr<xcb_query_tree_reply_t, decltype(&std::free)> tree{
                    xcb_query_tree_reply(c, xcb_query_tree(c, screen->root), nullptr), &std::free};
                if (!focus || focus->focus != dialog || !tree) return false;
                auto *children = xcb_query_tree_children(tree.get());
                int a = -1, b = -1;
                for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
                    if (children[i] == dialog) a = i;
                    if (children[i] == window) b = i;
                }
                return a > b && b >= 0;
            }, "transient focus and stacking")) return EXIT_FAILURE;
            xcb_destroy_window(c, dialog); xcb_flush(c);
            if (!wait([&] {
                std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> focus{
                    xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
                return focus && focus->focus == window;
            }, "fullscreen focus after dialog")) return EXIT_FAILURE;
            press(alt, true); tap(two); press(alt, false); xcb_flush(c);
            if (!wait_property(screen->root, current, 1)) return EXIT_FAILURE;
            press(alt, true); tap(one); press(alt, false); xcb_flush(c);
            if (!wait_property(screen->root, current, 0) ||
                !wait([&] { return has_full(window) && geometry(window, 0, 0, 800, 600); }, "workspace restore")) return EXIT_FAILURE;
            press(alt, true); tap(f); press(alt, false); xcb_flush(c);
            if (!wait([&] { return !has_full(window) && geometry(window, 0, 0, 400, 600); }, "Alt+f remove")) return EXIT_FAILURE;
            send(window, 2);
            if (!wait([&] { return has_full(window) && geometry(window, 0, 0, 800, 600); }, "EWMH toggle")) return EXIT_FAILURE;
            send(window, 0);
            if (!wait([&] { return !has_full(window) && geometry(window, 0, 0, 400, 600); }, "EWMH remove")) return EXIT_FAILURE;
            send(window, 1);
            if (!wait([&] { return has_full(window); }, "fullscreen before withdrawal")) return EXIT_FAILURE;
            xcb_unmap_window(c, window); xcb_flush(c);
            if (!wait([&] { return !has_full(window); }, "withdrawal clears fullscreen")) return EXIT_FAILURE;
            return EXIT_SUCCESS;
        }
        if (std::strcmp(mode, "transient") == 0 || std::strcmp(mode, "dialog") == 0 || std::strcmp(mode, "floating-toggle") == 0) {
            auto dialog = xcb_generate_id(c);
            xcb_create_window(c, XCB_COPY_FROM_PARENT, dialog, screen->root, 40, 50, 200, 150, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
            if (std::strcmp(mode, "transient") == 0) {
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, dialog, XCB_ATOM_WM_TRANSIENT_FOR,
                                    XCB_ATOM_WINDOW, 32, 1, &window);
            } else {
                auto type = intern_key("_NET_WM_WINDOW_TYPE");
                auto dialog_type = intern_key("_NET_WM_WINDOW_TYPE_DIALOG");
                if (!type || !dialog_type) return EXIT_FAILURE;
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, dialog, type, XCB_ATOM_ATOM, 32, 1, &dialog_type);
            }
            auto geometry_is = [&](xcb_window_t target, int x, int y, int width, int height) {
                for (int i = 0; i < 250; ++i) {
                    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> reply{
                        xcb_get_geometry_reply(c, xcb_get_geometry(c, target), nullptr), &std::free};
                    if (reply && reply->x == x && reply->y == y && reply->width == width && reply->height == height) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::fprintf(stderr, "%s: window %#x has wrong geometry\n", mode, target);
                return false;
            };
            auto focus_is = [&](xcb_window_t target) {
                for (int i = 0; i < 250; ++i) {
                    std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> reply{
                        xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
                    if (reply && reply->focus == target) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::fprintf(stderr, "%s: focus did not reach %#x\n", mode, target);
                return false;
            };
            auto map_is = [&](xcb_window_t target, uint8_t state) {
                for (int i = 0; i < 250; ++i) {
                    std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> reply{
                        xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, target), nullptr), &std::free};
                    if (reply && reply->map_state == state) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::fprintf(stderr, "%s: window %#x did not reach map state %u\n", mode, target, state);
                return false;
            };
            if (std::strcmp(mode, "floating-toggle") == 0) {
                auto space = key(' ');
                auto left = key(0xff51);
                if (!space || !left) return EXIT_FAILURE;
                press(alt, true); press(shift, true); tap(space); press(shift, false); press(alt, false); xcb_flush(c);
                if (!geometry_is(window, 200, 150, 400, 300)) return EXIT_FAILURE;
                auto tiled = xcb_generate_id(c);
                xcb_create_window(c, XCB_COPY_FROM_PARENT, tiled, screen->root, 0, 0, 100, 100, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
                xcb_map_window(c, tiled); xcb_flush(c);
                if (!wait_property(tiled, desktop, 0) || !focus_is(tiled)) return EXIT_FAILURE;
                auto above = [&](xcb_window_t upper, xcb_window_t lower) {
                    for (int i = 0; i < 250; ++i) {
                        std::unique_ptr<xcb_query_tree_reply_t, decltype(&std::free)> tree{
                            xcb_query_tree_reply(c, xcb_query_tree(c, screen->root), nullptr), &std::free};
                        if (tree) {
                            auto children = xcb_query_tree_children(tree.get());
                            int upper_index = -1, lower_index = -1;
                            for (int j = 0; j < xcb_query_tree_children_length(tree.get()); ++j) {
                                if (children[j] == upper) upper_index = j;
                                if (children[j] == lower) lower_index = j;
                            }
                            if (lower_index >= 0 && upper_index > lower_index) return true;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    std::fprintf(stderr, "%s: incorrect stacking\n", mode);
                    return false;
                };
                if (!above(window, tiled)) return EXIT_FAILURE;
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, dialog, XCB_ATOM_WM_TRANSIENT_FOR,
                                    XCB_ATOM_WINDOW, 32, 1, &tiled);
                xcb_map_window(c, dialog); xcb_flush(c);
                if (!wait_property(dialog, desktop, 0) || !focus_is(dialog) || !above(dialog, window)) return EXIT_FAILURE;
                xcb_warp_pointer(c, XCB_NONE, screen->root, 0, 0, 0, 0, 500, 400); xcb_flush(c);
                if (!focus_is(window) || !above(window, dialog) || !above(window, tiled)) return EXIT_FAILURE;
                press(alt, true); tap(left); press(alt, false); xcb_flush(c);
                return focus_is(tiled) ? EXIT_SUCCESS : EXIT_FAILURE;
            }
            xcb_map_window(c, dialog);
            xcb_flush(c);
            if (!wait_property(dialog, desktop, 0) || !map_is(dialog, XCB_MAP_STATE_VIEWABLE) ||
                !focus_is(dialog) || !geometry_is(window, 0, 0, 800, 600) ||
                !geometry_is(dialog, 40, 50, 200, 150)) return EXIT_FAILURE;
            const uint32_t geometry[] = {70, 80, 240, 180};
            xcb_configure_window(c, dialog, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geometry);
            xcb_flush(c);
            if (!geometry_is(dialog, 70, 80, 240, 180) || !geometry_is(window, 0, 0, 800, 600)) return EXIT_FAILURE;
            press(alt, true); tap(two); press(alt, false); xcb_flush(c);
            if (!wait_property(screen->root, current, 1) || !map_is(window, XCB_MAP_STATE_UNMAPPED) ||
                !map_is(dialog, XCB_MAP_STATE_UNMAPPED)) return EXIT_FAILURE;
            press(alt, true); tap(one); press(alt, false); xcb_flush(c);
            if (!wait_property(screen->root, current, 0) || !map_is(window, XCB_MAP_STATE_VIEWABLE) ||
                !map_is(dialog, XCB_MAP_STATE_VIEWABLE) || !focus_is(dialog) ||
                !geometry_is(window, 0, 0, 800, 600) || !geometry_is(dialog, 70, 80, 240, 180)) return EXIT_FAILURE;
            xcb_destroy_window(c, dialog);
            xcb_flush(c);
            return focus_is(window) ? EXIT_SUCCESS : EXIT_FAILURE;
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
            auto shortcut = [&](xcb_keysym_t symbol, bool shifted = false, bool controlled = false) {
                auto code = key(symbol);
                if (!code) { std::fprintf(stderr, "missing keysym %u\n", symbol); return false; }
                press(alt, true);
                if (shifted) press(shift, true);
                if (controlled) press(key(0xffe3), true);
                tap(code);
                if (controlled) press(key(0xffe3), false);
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
                std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> actual{
                    xcb_get_geometry_reply(c, xcb_get_geometry(c, target), nullptr), &std::free};
                if (actual) std::fprintf(stderr, "window %#x geometry %d,%d %ux%u expected %d,%d %dx%d\n",
                    target, actual->x, actual->y, actual->width, actual->height, x, y, width, height);
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
            if (!shortcut(scheme ? 'p' : 0xff51, !scheme) ||
                !tiled(third, 0, 300, 400, 300) || !tiled(second, 400, 300, 400, 300)) return EXIT_FAILURE;
            if (!shortcut(scheme ? 'g' : 0xff51) || !focused_on(third)) return EXIT_FAILURE;
            if (!shortcut(scheme ? 'u' : 0xff53, false, !scheme) ||
                !tiled(third, 0, 300, 409, 300) || !tiled(second, 409, 300, 391, 300)) return EXIT_FAILURE;
            if (!shortcut(scheme ? 'y' : 0xff53, !scheme) ||
                !tiled(second, 0, 300, 390, 300) || !tiled(third, 390, 300, 410, 300)) return EXIT_FAILURE;
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
            if (!received || !tiled(third, 390, 300, 410, 300)) return EXIT_FAILURE;
            auto launch_seen = [&](const char *expected) {
                const char *path = std::getenv("MORTAR_TEST_LAUNCH");
                if (!path) return false;
                for (int i = 0; i < 250; ++i) {
                    if (auto *file = std::fopen(path, "r")) {
                        char line[256];
                        bool found = false;
                        while (std::fgets(line, sizeof(line), file)) {
                            if (std::strcmp(line, expected) == 0) found = true;
                        }
                        std::fclose(file);
                        if (found) return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                std::fprintf(stderr, "launch record missing: %s", expected);
                return false;
            };
            if (scheme) return shortcut('o') && launch_seen("[a b][; touch forbidden]\n") ? EXIT_SUCCESS : EXIT_FAILURE;
            return shortcut(0xff0d) && launch_seen("x-terminal-emulator 1\n") &&
                   shortcut('d') && launch_seen("dmenu_run 1\n") ? EXIT_SUCCESS : EXIT_FAILURE;
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
