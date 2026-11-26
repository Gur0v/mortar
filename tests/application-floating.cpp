#include <xcb/xcb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

int main() {
    using Connection = std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)>;
    Connection connection{xcb_connect(nullptr, nullptr), &xcb_disconnect};
    if (!connection || xcb_connection_has_error(connection.get())) return EXIT_FAILURE;
    auto *c = connection.get();
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    if (!screen) return EXIT_FAILURE;
    auto atom = [&](const char *name) {
        std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply{
            xcb_intern_atom_reply(c, xcb_intern_atom(c, false, std::strlen(name), name), nullptr), &std::free};
        return reply ? reply->atom : xcb_atom_t{XCB_NONE};
    };
    const auto desktop = atom("_NET_WM_DESKTOP");
    const auto type = atom("_NET_WM_WINDOW_TYPE");
    const auto state = atom("_NET_WM_STATE");
    const auto modal = atom("_NET_WM_STATE_MODAL");
    if (!desktop || !type || !state || !modal) return EXIT_FAILURE;
    auto check = [&](const char *label, const char *window_type, bool is_modal, const uint32_t *hints,
                     uint32_t hint_count, xcb_atom_t hint_type, const char *window_class, bool floating) {
        const auto window = xcb_generate_id(c);
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 20, 30, 120, 80, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        if (window_type) {
            const auto value = atom(window_type);
            if (!value) return false;
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, type, XCB_ATOM_ATOM, 32, 1, &value);
        }
        if (is_modal) xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, state, XCB_ATOM_ATOM, 32, 1, &modal);
        if (hints) xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NORMAL_HINTS,
                                       hint_type, 32, hint_count, hints);
        if (window_class) {
            char value[128];
            const int length = std::snprintf(value, sizeof(value), "instance%c%s", 0, window_class);
            if (length < 0 || static_cast<size_t>(length) >= sizeof(value)) return false;
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING,
                                8, static_cast<uint32_t>(length + 1), value);
        }
        xcb_map_window(c, window);
        xcb_flush(c);
        bool ok = false;
        for (int i = 0; i < 100; ++i) {
            std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> property{
                xcb_get_property_reply(c, xcb_get_property(c, false, window, desktop,
                                                          XCB_ATOM_CARDINAL, 0, 1), nullptr), &std::free};
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
            if (property && property->type == XCB_ATOM_CARDINAL && property->format == 32 &&
                xcb_get_property_value_length(property.get()) == 4 && geometry &&
                (floating ? geometry->x > 100 && geometry->y > 100 : geometry->x < 100 && geometry->y < 100)) {
                ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!ok) std::fprintf(stderr, "%s: expected %s geometry\n", label, floating ? "floating" : "tiled");
        xcb_destroy_window(c, window);
        xcb_flush(c);
        return ok;
    };
    uint32_t fixed[18]{};
    fixed[0] = 16 | 32;
    fixed[5] = fixed[7] = 120;
    fixed[6] = fixed[8] = 80;
    uint32_t partial[18]{};
    partial[0] = 16 | 32;
    partial[5] = partial[7] = 120;
    partial[6] = 80;
    partial[8] = 100;
    bool ok = true;
    auto run = [&](const char *label, const char *window_type, bool is_modal, const uint32_t *hints,
                   uint32_t count, xcb_atom_t hint_type, const char *window_class, bool floating) {
        ok = check(label, window_type, is_modal, hints, count, hint_type, window_class, floating) && ok;
    };
    run("normal", nullptr, false, nullptr, 0, XCB_ATOM_WM_SIZE_HINTS, nullptr, false);
    run("fixed size", nullptr, false, fixed, 18, XCB_ATOM_WM_SIZE_HINTS, nullptr, true);
    run("partial fixed size", nullptr, false, partial, 18, XCB_ATOM_WM_SIZE_HINTS, nullptr, false);
    run("truncated hints", nullptr, false, fixed, 8, XCB_ATOM_WM_SIZE_HINTS, nullptr, false);
    run("wrong hint type", nullptr, false, fixed, 18, XCB_ATOM_CARDINAL, nullptr, false);
    for (const char *name : {"_NET_WM_WINDOW_TYPE_UTILITY", "_NET_WM_WINDOW_TYPE_TOOLBAR",
                             "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_SPLASH",
                             "_NET_WM_WINDOW_TYPE_DIALOG"})
        run(name, name, false, nullptr, 0, XCB_ATOM_WM_SIZE_HINTS, nullptr, true);
    run("modal", nullptr, true, nullptr, 0, XCB_ATOM_WM_SIZE_HINTS, nullptr, true);
    run("tiled rule overrides fixed", nullptr, false, fixed, 18, XCB_ATOM_WM_SIZE_HINTS, "Override", false);
    run("tiled rule overrides dialog", "_NET_WM_WINDOW_TYPE_DIALOG", false, nullptr, 0,
        XCB_ATOM_WM_SIZE_HINTS, "Override", false);
    run("floating rule overrides normal", nullptr, false, nullptr, 0, XCB_ATOM_WM_SIZE_HINTS, "Float", true);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
