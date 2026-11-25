#include <xcb/xcb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

int main() {
    using Connection = std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)>;
    using Reply = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;
    Connection connection{xcb_connect(nullptr, nullptr), &xcb_disconnect};
    if (!connection || xcb_connection_has_error(connection.get())) return EXIT_FAILURE;
    auto *c = connection.get();
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    if (!screen) return EXIT_FAILURE;
    const char name[] = "_NET_WM_DESKTOP";
    std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> atom{
        xcb_intern_atom_reply(c, xcb_intern_atom(c, false, sizeof(name) - 1, name), nullptr), &std::free};
    if (!atom) return EXIT_FAILURE;
    auto check = [&](const char *label, const char *value, uint32_t length, xcb_atom_t type,
                     uint32_t desktop, bool floating) {
        auto window = xcb_generate_id(c);
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 20, 30, 120, 80, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, type, 8, length, value);
        xcb_map_window(c, window);
        xcb_flush(c);
        bool ok = false;
        for (int i = 0; i < 100; ++i) {
            Reply property{xcb_get_property_reply(c, xcb_get_property(c, false, window, atom->atom,
                XCB_ATOM_CARDINAL, 0, 1), nullptr), &std::free};
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
            if (property && property->type == XCB_ATOM_CARDINAL && property->format == 32 &&
                xcb_get_property_value_length(property.get()) == 4 && geometry &&
                *static_cast<const uint32_t *>(xcb_get_property_value(property.get())) == desktop &&
                (desktop != 0 || (floating ? geometry->width < 400 : geometry->width > 400))) {
                ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!ok) std::fprintf(stderr, "%s: wrong desktop or geometry\n", label);
        xcb_destroy_window(c, window);
        xcb_flush(c);
        return ok;
    };
    const char floating[] = "instance\0Float";
    const char workspace[] = "instance\0Remote";
    const char overridden[] = "instance\0Override";
    const char instance_only[] = "Float\0Other";
    const char missing_nul[] = "instance\0Float";
    const char trailing[] = "instance\0Float\0extra";
    return check("floating", floating, sizeof(floating), XCB_ATOM_STRING, 0, true) &&
           check("workspace", workspace, sizeof(workspace), XCB_ATOM_STRING, 1, false) &&
           check("override", overridden, sizeof(overridden), XCB_ATOM_STRING, 0, false) &&
           check("instance", instance_only, sizeof(instance_only), XCB_ATOM_STRING, 0, false) &&
           check("unterminated", missing_nul, sizeof(missing_nul) - 1, XCB_ATOM_STRING, 0, false) &&
           check("trailing", trailing, sizeof(trailing), XCB_ATOM_STRING, 0, false) &&
           check("wrong type", floating, sizeof(floating), XCB_ATOM_ATOM, 0, false)
        ? EXIT_SUCCESS : EXIT_FAILURE;
}
