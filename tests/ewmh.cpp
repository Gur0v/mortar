#include <xcb/xcb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

int main() {
    using Connection = std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)>;
    Connection connection{xcb_connect(nullptr, nullptr), &xcb_disconnect};
    if (!connection || xcb_connection_has_error(connection.get())) return EXIT_FAILURE;
    auto *c = connection.get();
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    if (!screen) return EXIT_FAILURE;
    auto atom = [&](const char *name) {
        std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply{
            xcb_intern_atom_reply(c, xcb_intern_atom(c, false, static_cast<uint16_t>(std::strlen(name)), name), nullptr), &std::free};
        return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    };
    auto clients = atom("_NET_CLIENT_LIST");
    auto stacking = atom("_NET_CLIENT_LIST_STACKING");
    auto active = atom("_NET_ACTIVE_WINDOW");
    auto names = atom("_NET_DESKTOP_NAMES");
    auto utf8 = atom("UTF8_STRING");
    if (!clients || !stacking || !active || !names || !utf8) return EXIT_FAILURE;
    auto property = [&](xcb_atom_t name) {
        std::vector<xcb_window_t> result;
        std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
            xcb_get_property_reply(c, xcb_get_property(c, false, screen->root, name, XCB_ATOM_WINDOW, 0, 1024), nullptr), &std::free};
        if (reply && reply->type == XCB_ATOM_WINDOW && reply->format == 32 && reply->bytes_after == 0) {
            auto *values = static_cast<xcb_window_t *>(xcb_get_property_value(reply.get()));
            auto count = xcb_get_property_value_length(reply.get()) / static_cast<int>(sizeof(xcb_window_t));
            if (count) result.assign(values, values + count);
        }
        return result;
    };
    auto wait = [&](auto predicate, const char *message) {
        for (int i = 0; i < 100; ++i) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::fprintf(stderr, "%s\n", message);
        return false;
    };
    if (!wait([&] {
        std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
            xcb_get_property_reply(c, xcb_get_property(c, false, screen->root, names, utf8, 0, 1024), nullptr), &std::free};
        const char expected[] = "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0" "8\0" "9\0" "10";
        return reply && reply->type == utf8 && reply->format == 8 && !reply->bytes_after &&
            xcb_get_property_value_length(reply.get()) == sizeof(expected) &&
            std::memcmp(xcb_get_property_value(reply.get()), expected, sizeof(expected)) == 0;
    }, "desktop names incorrect")) return EXIT_FAILURE;
    auto first = xcb_generate_id(c);
    auto second = xcb_generate_id(c);
    for (auto window : {first, second}) {
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 100, 100, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        xcb_map_window(c, window);
        xcb_flush(c);
        if (!wait([&] { auto list = property(clients); return !list.empty() && list.back() == window; },
                  "managed window missing from client list")) return EXIT_FAILURE;
    }
    if (!wait([&] { return property(clients) == std::vector<xcb_window_t>{first, second} &&
                           property(active) == std::vector<xcb_window_t>{second}; },
              "client list or active window incorrect")) return EXIT_FAILURE;
    auto actual_stack = [&] {
        std::vector<xcb_window_t> result;
        std::unique_ptr<xcb_query_tree_reply_t, decltype(&std::free)> reply{
            xcb_query_tree_reply(c, xcb_query_tree(c, screen->root), nullptr), &std::free};
        if (!reply) return result;
        auto *children = xcb_query_tree_children(reply.get());
        for (int i = 0; i < xcb_query_tree_children_length(reply.get()); ++i)
            if (children[i] == first || children[i] == second) result.push_back(children[i]);
        return result;
    };
    if (!wait([&] { return property(stacking) == actual_stack() && actual_stack().size() == 2; },
              "stacking list does not match X server")) return EXIT_FAILURE;
    auto unmanaged = xcb_generate_id(c);
    const uint32_t override_redirect = 1;
    xcb_create_window(c, XCB_COPY_FROM_PARENT, unmanaged, screen->root, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
    const uint32_t below = XCB_STACK_MODE_BELOW;
    xcb_configure_window(c, second, XCB_CONFIG_WINDOW_STACK_MODE, &below);
    xcb_flush(c);
    if (!wait([&] { return property(stacking) == actual_stack(); }, "stacking change not published")) return EXIT_FAILURE;
    xcb_destroy_window(c, unmanaged);
    xcb_client_message_event_t request{};
    request.response_type = XCB_CLIENT_MESSAGE;
    request.format = 32;
    request.window = first;
    request.type = active;
    request.data.data32[0] = 2;
    xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&request));
    xcb_flush(c);
    if (!wait([&] { return property(active) == std::vector<xcb_window_t>{first}; },
              "activation request did not focus window")) return EXIT_FAILURE;
    auto urgent = atom("_MORTAR_URGENT_WORKSPACES");
    auto urgent_property = [&] {
        std::vector<uint32_t> result;
        std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
            xcb_get_property_reply(c, xcb_get_property(c, false, screen->root, urgent, XCB_ATOM_CARDINAL, 0, 10), nullptr), &std::free};
        if (reply && reply->type == XCB_ATOM_CARDINAL && reply->format == 32 && !reply->bytes_after) {
            auto *values = static_cast<uint32_t *>(xcb_get_property_value(reply.get()));
            auto count = xcb_get_property_value_length(reply.get()) / 4;
            if (count) result.assign(values, values + count);
        }
        return result;
    };
    auto state = atom("_NET_WM_STATE");
    auto attention = atom("_NET_WM_STATE_DEMANDS_ATTENTION");
    auto other = atom("_MORTAR_TEST_STATE");
    auto states = [&] {
        std::vector<xcb_atom_t> result;
        std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply{
            xcb_get_property_reply(c, xcb_get_property(c, false, second, state, XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
        if (reply && reply->type == XCB_ATOM_ATOM && reply->format == 32 && !reply->bytes_after) {
            auto *values = static_cast<xcb_atom_t *>(xcb_get_property_value(reply.get()));
            auto count = xcb_get_property_value_length(reply.get()) / 4;
            if (count) result.assign(values, values + count);
        }
        return result;
    };
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, second, state, XCB_ATOM_ATOM, 32, 1, &other);
    xcb_client_message_event_t state_request{};
    state_request.response_type = XCB_CLIENT_MESSAGE;
    state_request.format = 32;
    state_request.window = second;
    state_request.type = state;
    state_request.data.data32[1] = attention;
    auto change_attention = [&](uint32_t action, bool enabled) {
        state_request.data.data32[0] = action;
        xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char *>(&state_request));
        xcb_flush(c);
        return wait([&] { return states() == (enabled ? std::vector<xcb_atom_t>{other, attention} : std::vector<xcb_atom_t>{other}) &&
            urgent_property() == (enabled ? std::vector<uint32_t>{0} : std::vector<uint32_t>{}); }, "attention state incorrect");
    };
    if (!change_attention(1, true) || !change_attention(0, false) ||
        !change_attention(2, true) || !change_attention(2, false)) return EXIT_FAILURE;
    const uint32_t hints[] = {1u << 8, 0, 0, 0, 0, 0, 0, 0, 0};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, second, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, 9, hints);
    xcb_flush(c);
    if (!wait([&] { return urgent_property() == std::vector<uint32_t>{0} &&
                           states() == std::vector<xcb_atom_t>{other, attention}; }, "urgency hint not published")) return EXIT_FAILURE;
    request.window = second;
    xcb_send_event(c, false, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&request));
    xcb_flush(c);
    if (!wait([&] { return property(active) == std::vector<xcb_window_t>{second} && urgent_property().empty() &&
                           states() == std::vector<xcb_atom_t>{other}; },
              "focus did not clear urgency")) return EXIT_FAILURE;
    xcb_destroy_window(c, first);
    xcb_flush(c);
    if (!wait([&] { return property(clients) == std::vector<xcb_window_t>{second} &&
                           property(stacking) == std::vector<xcb_window_t>{second} &&
                           property(active) == std::vector<xcb_window_t>{second}; },
              "window removal left stale EWMH state")) return EXIT_FAILURE;
    xcb_destroy_window(c, second);
    xcb_flush(c);
    if (!wait([&] { return property(clients).empty() && property(stacking).empty() &&
                           property(active) == std::vector<xcb_window_t>{XCB_WINDOW_NONE}; },
              "empty desktop EWMH state incorrect")) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
