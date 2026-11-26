#include "../mortar/wm-selection.hpp"

#include <cstdlib>
#include <cstdio>

#undef assert
#define assert(condition) ((condition) ? static_cast<void>(0) : (std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition), std::abort()))
#include <memory>
#include <chrono>
#include <thread>

int main(int argc, char **) {
    int screen_number = 0;
    auto *observer = xcb_connect(nullptr, &screen_number);
    assert(!xcb_connection_has_error(observer));
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(observer));
    for (int i = 0; i < screen_number; ++i) xcb_screen_next(&screens);
    assert(screens.rem);
    const auto root = screens.data->root;
    if (argc > 1) {
        const auto name = std::string("WM_S") + std::to_string(screen_number);
        auto atom = xcb_intern_atom_reply(observer, xcb_intern_atom(observer, 0, static_cast<uint16_t>(name.size()), name.data()), nullptr);
        assert(atom);
        xcb_window_t previous = XCB_WINDOW_NONE;
        for (int i = 0; i < 200 && !previous; ++i) {
            auto owner = xcb_get_selection_owner_reply(observer, xcb_get_selection_owner(observer, atom->atom), nullptr);
            assert(owner);
            previous = owner->owner;
            std::free(owner);
            if (!previous) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(previous);
        auto window = xcb_generate_id(observer);
        xcb_create_window(observer, XCB_COPY_FROM_PARENT, window, root, 0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
        xcb_set_selection_owner(observer, window, atom->atom, XCB_CURRENT_TIME);
        auto owner = xcb_get_selection_owner_reply(observer, xcb_get_selection_owner(observer, atom->atom), nullptr);
        assert(owner && owner->owner == window);
        std::free(owner);
        std::free(atom);
        xcb_disconnect(observer);
        return 0;
    }
    const uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(observer, root, XCB_CW_EVENT_MASK, &mask);
    auto sync = xcb_get_input_focus_reply(observer, xcb_get_input_focus(observer), nullptr);
    assert(sync);
    std::free(sync);
    auto *connection = xcb_connect(nullptr, nullptr);
    assert(!xcb_connection_has_error(connection));
    auto selected = mortar::acquire_wm_selection(connection, root, screen_number);
    assert(selected.atom && selected.window);
    auto owner = xcb_get_selection_owner_reply(observer, xcb_get_selection_owner(observer, selected.atom), nullptr);
    assert(owner && owner->owner == selected.window);
    std::free(owner);
    std::unique_ptr<xcb_generic_event_t, decltype(&std::free)> event{xcb_wait_for_event(observer), &std::free};
    assert(event && (event->response_type & 0x7f) == XCB_CLIENT_MESSAGE);
    auto *message = reinterpret_cast<xcb_client_message_event_t *>(event.get());
    assert(message->format == 32 && message->window == root && message->data.data32[0] != XCB_CURRENT_TIME);
    assert(message->data.data32[1] == selected.atom && message->data.data32[2] == selected.window);
    auto duplicate = mortar::acquire_wm_selection(observer, root, screen_number);
    assert(!duplicate.atom && !duplicate.window);
    xcb_disconnect(connection);
    for (int i = 0; i < 200; ++i) {
        owner = xcb_get_selection_owner_reply(observer, xcb_get_selection_owner(observer, selected.atom), nullptr);
        assert(owner);
        const auto current = owner->owner;
        std::free(owner);
        if (current == XCB_WINDOW_NONE) break;
        assert(i < 199);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    xcb_disconnect(observer);
}
