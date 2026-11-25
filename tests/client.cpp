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
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(connection.get())).data;
    if (!screen) return EXIT_FAILURE;
    const auto window = xcb_generate_id(connection.get());
    xcb_create_window(connection.get(), XCB_COPY_FROM_PARENT, window, screen->root,
                      10, 10, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, nullptr);
    xcb_map_window(connection.get(), window);
    xcb_flush(connection.get());
    for (int i = 0; i < 100; ++i) {
        using Reply = std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)>;
        Reply geometry{xcb_get_geometry_reply(connection.get(), xcb_get_geometry(connection.get(), window), nullptr), &std::free};
        if (!geometry) return EXIT_FAILURE;
        if (geometry->width == 800 && geometry->height == 600) return EXIT_SUCCESS;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::fputs("window was not tiled to screen\n", stderr);
    return EXIT_FAILURE;
}
