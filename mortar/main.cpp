#include <xcb/xcb.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

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

    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    auto cookie = xcb_change_window_attributes_checked(connection.get(), screens.data->root,
                                                        XCB_CW_EVENT_MASK, &mask);
    using Error = std::unique_ptr<xcb_generic_error_t, decltype(&std::free)>;
    Error error{xcb_request_check(connection.get(), cookie), &std::free};
    if (error) {
        std::fprintf(stderr, "mortar: cannot acquire window management (X error %u)\n", error->error_code);
        return EXIT_FAILURE;
    }

    while (auto *event = xcb_wait_for_event(connection.get())) {
        std::free(event);
    }
    std::fputs("mortar: X connection closed\n", stderr);
    return EXIT_FAILURE;
}
