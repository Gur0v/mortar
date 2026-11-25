#include "workspaces.hpp"

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <poll.h>
#include <unistd.h>
#include <memory>
#include <tuple>
#include <vector>

static volatile std::sig_atomic_t stopping = 0;

static void stop(int) { stopping = 1; }

static std::pair<uint32_t, uint32_t> hinted_size(uint32_t width, uint32_t height, const uint32_t *h, size_t count) {
    if (count < 15) return {width, height};
    const auto positive = [](uint32_t value) { return value > 0 && value <= INT32_MAX; };
    const uint32_t flags = h[0];
    uint32_t minw = (flags & 16) && positive(h[5]) ? h[5] : 1;
    uint32_t minh = (flags & 16) && positive(h[6]) ? h[6] : 1;
    uint32_t maxw = (flags & 32) && positive(h[7]) ? std::min(width, h[7]) : width;
    uint32_t maxh = (flags & 32) && positive(h[8]) ? std::min(height, h[8]) : height;
    minw = std::min(minw, maxw);
    minh = std::min(minh, maxh);
    uint32_t basew = count >= 17 && (flags & 256) && h[15] <= INT32_MAX ? h[15] : ((flags & 16) && positive(h[5]) ? h[5] : 0);
    uint32_t baseh = count >= 17 && (flags & 256) && h[16] <= INT32_MAX ? h[16] : ((flags & 16) && positive(h[6]) ? h[6] : 0);
    uint32_t incw = (flags & 64) && positive(h[9]) ? h[9] : 1;
    uint32_t inch = (flags & 64) && positive(h[10]) ? h[10] : 1;
    auto align = [](uint32_t low, uint32_t high, uint32_t base, uint32_t inc) {
        if (base > high) return high;
        uint32_t result = high - (high - base) % inc;
        return result >= low ? result : high;
    };
    maxw = align(minw, maxw, basew, incw);
    maxh = align(minh, maxh, baseh, inch);
    if (!(flags & 128) || !positive(h[11]) || !positive(h[12]) || !positive(h[13]) || !positive(h[14]) ||
        uint64_t{h[11]} * h[14] > uint64_t{h[13]} * h[12]) return {maxw, maxh};
    uint32_t bestw = 0, besth = 0;
    for (uint32_t w = maxw; w >= minw; --w) {
        if (w != maxw && (w < basew || (w - basew) % incw)) continue;
        uint64_t upper = uint64_t{w} * h[12] / h[11];
        uint64_t lower = (uint64_t{w} * h[14] + h[13] - 1) / h[13];
        if (upper < minh || lower > maxh) continue;
        uint32_t candidate = align(minh, static_cast<uint32_t>(std::min<uint64_t>(maxh, upper)), baseh, inch);
        if (candidate < lower || (candidate != maxh && (candidate < baseh || (candidate - baseh) % inch))) continue;
        if (uint64_t{w} * candidate > uint64_t{bestw} * besth) {
            bestw = w;
            besth = candidate;
        }
    }
    return bestw ? std::pair{bestw, besth} : std::pair{maxw, maxh};
}

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
    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                          XCB_EVENT_MASK_PROPERTY_CHANGE;
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
    mortar::Workspaces workspace_state;
    auto &workspaces = workspace_state.workspaces;
    auto &current = workspace_state.current;
    std::vector<xcb_window_t> iconic;
    std::vector<std::pair<xcb_window_t, uint16_t>> pending_unmaps;
    auto atom = [&](const char *name, uint16_t length) {
        std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply{
            xcb_intern_atom_reply(c, xcb_intern_atom(c, false, length, name), nullptr), &std::free};
        return reply ? reply->atom : xcb_atom_t{XCB_ATOM_NONE};
    };
    const auto wm_state = atom("WM_STATE", 8);
    const auto wm_change_state = atom("WM_CHANGE_STATE", 15);
    if (wm_state == XCB_ATOM_NONE || wm_change_state == XCB_ATOM_NONE) {
        std::fputs("mortar: cannot intern window state atoms\n", stderr);
        return EXIT_FAILURE;
    }
    const auto net_supported = atom("_NET_SUPPORTED", 14);
    const auto net_number = atom("_NET_NUMBER_OF_DESKTOPS", 23);
    const auto net_current = atom("_NET_CURRENT_DESKTOP", 20);
    const auto net_desktop = atom("_NET_WM_DESKTOP", 15);
    const auto net_check = atom("_NET_SUPPORTING_WM_CHECK", 24);
    const auto net_name = atom("_NET_WM_NAME", 12);
    const auto utf8 = atom("UTF8_STRING", 11);
    if (!net_supported || !net_number || !net_current || !net_desktop || !net_check || !net_name || !utf8) return EXIT_FAILURE;
    using Property = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;
    auto cardinal = [&](xcb_window_t window, xcb_atom_t property, uint32_t fallback) {
        Property reply{xcb_get_property_reply(c, xcb_get_property(c, false, window, property,
                                                                 XCB_ATOM_CARDINAL, 0, 1), nullptr), &std::free};
        if (reply && reply->type == XCB_ATOM_CARDINAL && reply->format == 32 &&
            xcb_get_property_value_length(reply.get()) == 4) {
            auto value = *static_cast<const uint32_t *>(xcb_get_property_value(reply.get()));
            if (value < workspaces.size()) return value;
        }
        return fallback;
    };
    current = cardinal(root, net_current, 0);
    auto support = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, support, root, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screens.data->root_visual, 0, nullptr);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_check, XCB_ATOM_WINDOW, 32, 1, &support);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, support, net_check, XCB_ATOM_WINDOW, 32, 1, &support);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, support, net_name, utf8, 8, 6, "mortar");
    const xcb_atom_t supported_atoms[] = {net_number, net_current, net_desktop, net_check, net_name};
    const uint32_t desktop_count = workspaces.size();
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_supported, XCB_ATOM_ATOM, 32, 5, supported_atoms);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_number, XCB_ATOM_CARDINAL, 32, 1, &desktop_count);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
    auto set_desktop = [&](xcb_window_t window, uint32_t desktop) {
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, net_desktop, XCB_ATOM_CARDINAL, 32, 1, &desktop);
    };
    auto owner = [&](xcb_window_t window) -> int {
        return workspace_state.owner(window);
    };
    bool starting = true;
    auto unmap = [&](xcb_window_t window) {
        if (!starting) xcb_grab_server(c);
        std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> attributes{
            xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, window), nullptr), &std::free};
        if (attributes && attributes->map_state != XCB_MAP_STATE_UNMAPPED) {
            auto request = xcb_unmap_window(c, window);
            pending_unmaps.emplace_back(window, static_cast<uint16_t>(request.sequence));
        }
        if (!starting) xcb_ungrab_server(c);
    };
    auto set_state = [&](xcb_window_t window, uint32_t state) {
        const uint32_t value[] = {state, XCB_NONE};
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, wm_state, wm_state, 32, 2, value);
    };
    uint32_t width = screens.data->width_in_pixels;
    uint32_t height = screens.data->height_in_pixels;
    const auto *randr = xcb_get_extension_data(c, &xcb_randr_id);
    if (!randr || !randr->present) {
        std::fputs("mortar: RandR extension unavailable\n", stderr);
        return EXIT_FAILURE;
    }
    xcb_randr_select_input(c, root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    const auto protocols = atom("WM_PROTOCOLS", 12);
    const auto take_focus = atom("WM_TAKE_FOCUS", 13);
    const auto focus_time = atom("_MORTAR_FOCUS_TIME", 18);
    if (!protocols || !take_focus || !focus_time) {
        std::fputs("mortar: cannot initialize focus atoms\n", stderr);
        return EXIT_FAILURE;
    }
    auto focus = [&](xcb_window_t window, xcb_timestamp_t time) {
        workspaces[current].focused = window;
        using Property = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;
        Property hints{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_HINTS,
                                                                  XCB_ATOM_WM_HINTS, 0, 2), nullptr), &std::free};
        bool input = true;
        if (hints && hints->type == XCB_ATOM_WM_HINTS && hints->format == 32 &&
            xcb_get_property_value_length(hints.get()) >= 8) {
            auto *values = static_cast<const uint32_t *>(xcb_get_property_value(hints.get()));
            if (values[0] & 1) input = values[1] != 0;
        }
        Property supported{xcb_get_property_reply(c, xcb_get_property(c, false, window, protocols,
                                                                      XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
        if (input) xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, window, time);
        if (supported && supported->type == XCB_ATOM_ATOM && supported->format == 32) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(supported.get()));
            auto count = xcb_get_property_value_length(supported.get()) / 4;
            if (std::find(values, values + count, take_focus) != values + count) {
                xcb_client_message_event_t message{};
                message.response_type = XCB_CLIENT_MESSAGE;
                message.format = 32;
                message.window = window;
                message.type = protocols;
                message.data.data32[0] = take_focus;
                message.data.data32[1] = time;
                xcb_send_event(c, false, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&message));
            }
        }
        xcb_flush(c);
    };
    auto tile = [&] {
        for (const auto &placement : workspaces[current].layout.geometry({0, 0, static_cast<int>(width), static_cast<int>(height)})) {
            using Property = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;
            Property hints{xcb_get_property_reply(c, xcb_get_property(c, false, placement.window,
                XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 18), nullptr), &std::free};
            uint32_t tile_width = std::max(1, placement.bounds.width);
            uint32_t tile_height = std::max(1, placement.bounds.height);
            if (hints && hints->type == XCB_ATOM_WM_SIZE_HINTS && hints->format == 32) {
                std::tie(tile_width, tile_height) = hinted_size(tile_width, tile_height,
                    static_cast<const uint32_t *>(xcb_get_property_value(hints.get())),
                    xcb_get_property_value_length(hints.get()) / 4);
            }
            const uint32_t values[] = {static_cast<uint32_t>(placement.bounds.x),
                                       static_cast<uint32_t>(placement.bounds.y), tile_width, tile_height, 0};
            xcb_configure_window(c, placement.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                                 XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
        }
        xcb_flush(c);
    };
    std::vector<xcb_window_t> pending_focus;
    auto restore_focus = [&] {
        auto window = workspaces[current].focused;
        if (window && workspaces[current].layout.contains(window)) {
            pending_focus.push_back(window);
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr);
        } else xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
        xcb_flush(c);
    };
    auto manage = [&](xcb_window_t window, bool existing) {
        if (window == support) return;
        if (owner(window) >= 0) {
            if (!existing && owner(window) == static_cast<int>(current)) {
                xcb_map_window(c, window);
                xcb_flush(c);
            }
            return;
        }
        auto cookie = xcb_get_window_attributes(c, window);
        std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> attributes{
            xcb_get_window_attributes_reply(c, cookie, nullptr), &std::free};
        if (!attributes) return;
        if (attributes->override_redirect) {
            if (!existing) xcb_map_window(c, window);
            return;
        }
        using Property = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;
        uint32_t desktop = existing || std::find(iconic.begin(), iconic.end(), window) != iconic.end()
                               ? cardinal(window, net_desktop, current) : current;
        if (existing && attributes->map_state != XCB_MAP_STATE_VIEWABLE) {
            Property state{xcb_get_property_reply(c, xcb_get_property(c, false, window, wm_state,
                                                                      wm_state, 0, 2), nullptr), &std::free};
            if (state && state->type == wm_state && state->format == 32 &&
                xcb_get_property_value_length(state.get()) >= 4 &&
                static_cast<const uint32_t *>(xcb_get_property_value(state.get()))[0] == 3) {
                iconic.push_back(window);
            }
            if (!state || state->type != wm_state || state->format != 32 ||
                xcb_get_property_value_length(state.get()) < 4 ||
                static_cast<const uint32_t *>(xcb_get_property_value(state.get()))[0] != 1) return;
        }
        if (!existing && std::find(iconic.begin(), iconic.end(), window) == iconic.end()) {
            Property hints{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_HINTS,
                                                                      XCB_ATOM_WM_HINTS, 0, 3), nullptr), &std::free};
            if (hints && hints->type == XCB_ATOM_WM_HINTS && hints->format == 32 &&
                xcb_get_property_value_length(hints.get()) >= 12) {
                auto *values = static_cast<const uint32_t *>(xcb_get_property_value(hints.get()));
                if ((values[0] & 2) && values[2] == 3) {
                    iconic.push_back(window);
                    set_desktop(window, desktop);
                    set_state(window, 3);
                    xcb_flush(c);
                    return;
                }
            }
        }
        const uint32_t event_mask = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &event_mask);
        std::erase(iconic, window);
        if (!workspaces[desktop].layout.insert(mortar::Layout::leaf(window))) return;
        workspaces[desktop].windows.push_back(window);
        workspaces[desktop].focused = window;
        set_desktop(window, desktop);
        set_state(window, 1);
        tile();
        if (desktop != current) {
            if (attributes->map_state != XCB_MAP_STATE_UNMAPPED) unmap(window);
        } else if (!existing || attributes->map_state == XCB_MAP_STATE_UNMAPPED) {
            xcb_map_window(c, window);
            pending_focus.push_back(window);
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr);
        }
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

    std::unique_ptr<xcb_query_pointer_reply_t, decltype(&std::free)> pointer{
        xcb_query_pointer_reply(c, xcb_query_pointer(c, root), nullptr), &std::free};
    if (pointer && workspaces[current].layout.contains(pointer->child)) {
        pending_focus.push_back(pointer->child);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr);
    }
    xcb_ungrab_server(c);
    starting = false;
    xcb_flush(c);
    struct sigaction action{};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

    while (!stopping) {
        auto *event = xcb_poll_for_event(c);
        if (!event) {
            if (xcb_connection_has_error(c)) break;
            pollfd fd{xcb_get_file_descriptor(c), POLLIN, 0};
            poll(&fd, 1, 100);
            continue;
        }
        const auto type = event->response_type & 0x7f;
        if (type == randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
            auto *change = reinterpret_cast<xcb_randr_screen_change_notify_event_t *>(event);
            width = change->width;
            height = change->height;
            tile();
        }
        switch (type) {
        case XCB_ENTER_NOTIFY: {
            auto *enter = reinterpret_cast<xcb_enter_notify_event_t *>(event);
            if (enter->mode == XCB_NOTIFY_MODE_NORMAL && enter->detail != XCB_NOTIFY_DETAIL_INFERIOR &&
                workspaces[current].layout.contains(enter->event)) {
                if (enter->time != XCB_CURRENT_TIME) focus(enter->event, enter->time);
            }
            break;
        }
        case XCB_PROPERTY_NOTIFY: {
            auto *property = reinterpret_cast<xcb_property_notify_event_t *>(event);
            if (!(event->response_type & 0x80) && property->atom == XCB_ATOM_WM_NORMAL_HINTS &&
                workspaces[current].layout.contains(property->window)) tile();
            if (!(event->response_type & 0x80) && property->window == root && property->atom == focus_time &&
                property->state == XCB_PROPERTY_NEW_VALUE && !pending_focus.empty()) {
                auto window = pending_focus.front();
                pending_focus.erase(pending_focus.begin());
                if (property->time != XCB_CURRENT_TIME &&
                    workspaces[current].layout.contains(window)) focus(window, property->time);
            }
            break;
        }
        case XCB_MAP_REQUEST:
            manage(reinterpret_cast<xcb_map_request_event_t *>(event)->window, false);
            break;
        case XCB_CLIENT_MESSAGE: {
            auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
            if (message->format == 32 && message->type == net_current && message->window == root &&
                message->data.data32[0] < workspaces.size() && message->data.data32[0] != current) {
                for (auto window : workspaces[current].windows) {
                    unmap(window);
                }
                workspace_state.switch_to(message->data.data32[0]);
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
                tile();
                for (auto window : workspaces[current].windows) xcb_map_window(c, window);
                if (workspaces[current].focused && workspaces[current].layout.contains(workspaces[current].focused)) {
                    pending_focus.push_back(workspaces[current].focused);
                    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr);
                } else {
                    xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
                }
                xcb_flush(c);
            } else if (message->format == 32 && message->type == net_desktop &&
                       message->data.data32[0] < workspaces.size()) {
                int from = owner(message->window);
                uint32_t to = message->data.data32[0];
                if (from >= 0 && static_cast<uint32_t>(from) != to) {
                    auto &source = workspaces[from];
                    auto &destination = workspaces[to];
                    bool was_focused = source.focused == message->window;
                    bool destination_empty = destination.windows.empty();
                    if (!workspace_state.move(message->window, to)) break;
                    set_desktop(message->window, to);
                    if (static_cast<uint32_t>(from) == current) {
                        unmap(message->window);
                        tile();
                        if (was_focused) restore_focus();
                    } else if (to == current) {
                        tile();
                        xcb_map_window(c, message->window);
                        if (destination_empty) restore_focus();
                    }
                    xcb_flush(c);
                } else if (from < 0 && std::find(iconic.begin(), iconic.end(), message->window) != iconic.end()) {
                    set_desktop(message->window, to);
                    xcb_flush(c);
                }
            } else if (message->type == wm_change_state && message->format == 32 && message->data.data32[0] == 3 &&
                workspaces[current].layout.contains(message->window)) {
                bool was_focused = workspaces[current].focused == message->window;
                workspaces[current].layout.remove(message->window);
                std::erase(workspaces[current].windows, message->window);
                if (workspaces[current].focused == message->window)
                    workspaces[current].focused = workspaces[current].windows.empty() ? XCB_NONE : workspaces[current].windows.back();
                iconic.push_back(message->window);
                set_state(message->window, 3);
                unmap(message->window);
                tile();
                if (was_focused) restore_focus();
            }
            break;
        }
        case XCB_UNMAP_NOTIFY:
        case XCB_DESTROY_NOTIFY: {
            if (type == XCB_DESTROY_NOTIFY && (event->response_type & 0x80)) break;
            auto window = reinterpret_cast<xcb_unmap_notify_event_t *>(event)->window;
            if (type == XCB_UNMAP_NOTIFY && (event->response_type & 0x80) &&
                reinterpret_cast<xcb_unmap_notify_event_t *>(event)->event != root) break;
            if (type == XCB_UNMAP_NOTIFY && !(event->response_type & 0x80)) {
                auto pending = std::find(pending_unmaps.begin(), pending_unmaps.end(),
                                         std::pair{window, event->sequence});
                if (pending != pending_unmaps.end()) {
                    pending_unmaps.erase(pending);
                    break;
                }
            }
            int workspace = owner(window);
            bool was_tiled = workspace >= 0;
            bool was_focused = was_tiled && workspaces[workspace].focused == window;
            if (was_tiled) {
                auto &ws = workspaces[workspace];
                ws.layout.remove(window);
                std::erase(ws.windows, window);
                if (ws.focused == window) ws.focused = ws.windows.empty() ? XCB_NONE : ws.windows.back();
                xcb_delete_property(c, window, net_desktop);
            }
            auto removed = static_cast<size_t>(was_tiled) + std::erase(iconic, window);
            if (type == XCB_DESTROY_NOTIFY) std::erase_if(pending_unmaps, [&](auto entry) { return entry.first == window; });
            if (removed && type == XCB_UNMAP_NOTIFY) {
                set_state(window, 0);
                xcb_flush(c);
            }
            if (was_tiled && workspace == static_cast<int>(current)) {
                tile();
                if (was_focused) restore_focus();
            }
            break;
        }
        case XCB_CONFIGURE_REQUEST: {
            auto *request = reinterpret_cast<xcb_configure_request_event_t *>(event);
            if (owner(request->window) >= 0) {
                auto cookie = xcb_get_geometry(c, request->window);
                std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                    xcb_get_geometry_reply(c, cookie, nullptr), &std::free};
                if (geometry) {
                    xcb_configure_notify_event_t notify{};
                    notify.response_type = XCB_CONFIGURE_NOTIFY;
                    notify.event = request->window;
                    notify.window = request->window;
                    notify.x = geometry->x;
                    notify.y = geometry->y;
                    notify.width = geometry->width;
                    notify.height = geometry->height;
                    notify.border_width = geometry->border_width;
                    xcb_send_event(c, false, request->window, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                                   reinterpret_cast<const char *>(&notify));
                    xcb_flush(c);
                }
                break;
            }
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
    if (stopping && !xcb_connection_has_error(c)) {
        for (const auto &workspace : workspaces)
            for (auto window : workspace.windows) xcb_map_window(c, window);
        xcb_flush(c);
        return EXIT_SUCCESS;
    }
    std::fputs("mortar: X connection closed\n", stderr);
    return EXIT_FAILURE;
}
