#include "actions.hpp"
#include "config.hpp"
#include "workspaces.hpp"
#include "keyboard.hpp"

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <functional>
#include <system_error>
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

    xcb_ungrab_server(connection.get());
    xcb_flush(connection.get());

    struct Actions {
        std::function<void(unsigned)> workspace;
        std::function<void(unsigned)> move;
        std::function<void(mortar::Split)> split;
        std::function<void(mortar::Direction)> focus;
        std::function<void()> close;
        std::function<void(const std::vector<std::string> &)> spawn;
        xcb_timestamp_t time = XCB_CURRENT_TIME;
    } actions;
    mortar::Config config(&actions,
        [](void *host, unsigned digit) { static_cast<Actions *>(host)->workspace(digit); },
        [](void *host, unsigned digit) { static_cast<Actions *>(host)->move(digit); },
        [](void *host, mortar::Split value) { static_cast<Actions *>(host)->split(value); },
        [](void *host, mortar::Direction value) { static_cast<Actions *>(host)->focus(value); },
        [](void *host) { static_cast<Actions *>(host)->close(); },
        [](void *host, const std::vector<std::string> &args) { static_cast<Actions *>(host)->spawn(args); });
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::filesystem::path config_dir;
    if (xdg && *xdg && std::filesystem::path(xdg).is_absolute()) config_dir = xdg;
    else if (home && *home && std::filesystem::path(home).is_absolute()) config_dir = std::filesystem::path(home) / ".config";
    if (!config_dir.empty()) {
        auto path = config_dir / "mortar/config.scm";
        std::error_code error;
        auto status = std::filesystem::status(path, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            std::fprintf(stderr, "mortar: cannot inspect configuration %s: %s\n", path.c_str(), error.message().c_str());
        } else if (std::filesystem::exists(status) && !config.load(path.c_str())) {
            std::fprintf(stderr, "mortar: failed to load configuration %s; using defaults\n", path.c_str());
        }
    } else {
        std::fputs("mortar: cannot locate configuration: HOME is not an absolute path\n", stderr);
    }
    xcb_grab_server(connection.get());
    auto *c = connection.get();
    const auto root = screens.data->root;
    std::unique_ptr<mortar::Keyboard> keyboard;
    try {
        keyboard = std::make_unique<mortar::Keyboard>(c, root);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "mortar: keyboard setup failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
    enum : uint32_t { split_horizontal = UINT32_MAX - 8, split_vertical, focus_left, focus_down,
        focus_up, focus_right, close_focused, terminal, launcher };
    std::vector<mortar::KeyboardBinding> defaults{
        {XKB_KEY_h, 0, split_horizontal, true}, {XKB_KEY_v, 0, split_vertical, true},
        {XKB_KEY_j, 0, focus_left, true}, {XKB_KEY_k, 0, focus_down, true},
        {XKB_KEY_l, 0, focus_up, true}, {XKB_KEY_semicolon, 0, focus_right, true},
        {XKB_KEY_Left, 0, focus_left, true}, {XKB_KEY_Down, 0, focus_down, true},
        {XKB_KEY_Up, 0, focus_up, true}, {XKB_KEY_Right, 0, focus_right, true},
        {XKB_KEY_q, XCB_MOD_MASK_SHIFT, close_focused, true},
        {XKB_KEY_Return, 0, terminal, true}, {XKB_KEY_d, 0, launcher, true}
    };
    auto bindings = defaults;
    for (const auto &binding : config.bindings()) {
        std::erase_if(bindings, [&](const auto &existing) {
            return existing.keysym == binding.keysym &&
                   (existing.modifiers | (existing.dynamic_alt ? keyboard->alt_modifier() : 0)) == binding.modifiers;
        });
        bindings.push_back({binding.keysym, binding.modifiers, binding.callback});
    }
    try {
        keyboard->replace_bindings(bindings);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "mortar: custom key grabs failed: %s; using default keybindings\n", error.what());
        try {
            keyboard->replace_bindings(defaults);
        } catch (const std::exception &fallback_error) {
            std::fprintf(stderr, "mortar: default key grabs failed: %s\n", fallback_error.what());
            return EXIT_FAILURE;
        }
    }
    std::vector<pid_t> children;
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
    const auto delete_window = atom("WM_DELETE_WINDOW", 16);
    const auto focus_time = atom("_MORTAR_FOCUS_TIME", 18);
    if (!protocols || !take_focus || !delete_window || !focus_time) {
        std::fputs("mortar: cannot initialize focus atoms\n", stderr);
        return EXIT_FAILURE;
    }
    std::vector<xcb_window_t> pending_focus;
    uint16_t focus_sequence = 0;
    auto focus = [&](xcb_window_t window, xcb_timestamp_t time) {
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
        bool accepts_focus = input;
        if (input) xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, window, time);
        if (supported && supported->type == XCB_ATOM_ATOM && supported->format == 32) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(supported.get()));
            auto count = xcb_get_property_value_length(supported.get()) / 4;
            if (std::find(values, values + count, take_focus) != values + count) {
                accepts_focus = true;
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
        if (accepts_focus) workspaces[current].focused = window;
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
    auto restore_focus = [&] {
        std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
        auto window = workspaces[current].focused;
        if (window && workspaces[current].layout.contains(window)) {
            pending_focus.push_back(window);
            focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
        } else xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
        xcb_flush(c);
    };
    auto switch_workspace = [&](uint32_t destination) {
        if (destination >= workspaces.size() || destination == current) return;
        for (auto window : workspaces[current].windows) unmap(window);
        workspace_state.switch_to(destination);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
        tile();
        for (auto window : workspaces[current].windows) xcb_map_window(c, window);
        restore_focus();
    };
    auto move_window = [&](xcb_window_t window, uint32_t to) {
        if (to >= workspaces.size()) return;
        int from = owner(window);
        if (from >= 0 && static_cast<uint32_t>(from) != to) {
            auto &source = workspaces[from];
            auto &destination = workspaces[to];
            bool was_focused = source.focused == window;
            bool destination_empty = destination.windows.empty();
            if (!workspace_state.move(window, to)) return;
            set_desktop(window, to);
            if (static_cast<uint32_t>(from) == current) {
                unmap(window);
                tile();
                if (was_focused) restore_focus();
            } else if (to == current) {
                tile();
                xcb_map_window(c, window);
                if (destination_empty) restore_focus();
            }
            xcb_flush(c);
        } else if (from < 0 && std::find(iconic.begin(), iconic.end(), window) != iconic.end()) {
            set_desktop(window, to);
            xcb_flush(c);
        }
    };
    actions.workspace = [&](unsigned digit) { switch_workspace((digit + 9) % 10); };
    actions.move = [&](unsigned digit) { move_window(workspaces[current].focused, (digit + 9) % 10); };
    actions.split = [&](mortar::Split direction) {
        auto &ws = workspaces[current];
        ws.layout.set_split(ws.focused, direction);
        tile();
    };
    actions.focus = [&](mortar::Direction direction) {
        auto &ws = workspaces[current];
        auto target = ws.layout.neighbor(ws.focused, direction);
        if (target) {
            std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
            focus(target, actions.time);
        }
    };
    actions.close = [&] {
        auto window = workspaces[current].focused;
        if (window && mortar::close_window(c, window, protocols, delete_window, actions.time) != mortar::CloseResult::sent)
            std::fputs("mortar: cannot close focused window\n", stderr);
    };
    actions.spawn = [&](const std::vector<std::string> &args) {
        pid_t child;
        int error = mortar::spawn(args, child);
        if (error) std::fprintf(stderr, "mortar: spawn failed: %s\n", std::strerror(error));
        else children.push_back(child);
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
        if (!workspaces[desktop].layout.insert_after(workspaces[desktop].focused, mortar::Layout::leaf(window))) return;
        workspaces[desktop].windows.push_back(window);
        if (desktop != current || !workspaces[desktop].focused) workspaces[desktop].focused = window;
        set_desktop(window, desktop);
        set_state(window, 1);
        tile();
        if (desktop != current) {
            if (attributes->map_state != XCB_MAP_STATE_UNMAPPED) unmap(window);
        } else if (!existing || attributes->map_state == XCB_MAP_STATE_UNMAPPED) {
            xcb_map_window(c, window);
            std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
            pending_focus.push_back(window);
            focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
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
    try {
    auto *children = xcb_query_tree_children(tree.get());
    for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
        manage(children[i], true);
    }

    std::unique_ptr<xcb_query_pointer_reply_t, decltype(&std::free)> pointer{
        xcb_query_pointer_reply(c, xcb_query_pointer(c, root), nullptr), &std::free};
    if (pointer && workspaces[current].layout.contains(pointer->child)) {
        std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
        pending_focus.push_back(pointer->child);
        focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
    } else if (!workspaces[current].windows.empty()) {
        std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
        pending_focus.push_back(workspaces[current].windows.back());
        focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
    }
    } catch (...) {
        xcb_delete_property(c, root, net_check);
        xcb_delete_property(c, root, net_supported);
        xcb_delete_property(c, root, net_number);
        xcb_delete_property(c, root, focus_time);
        const uint32_t no_events = 0;
        xcb_change_window_attributes(c, root, XCB_CW_EVENT_MASK, &no_events);
        for (const auto &workspace : workspaces)
            for (auto window : workspace.windows) xcb_map_window(c, window);
        xcb_destroy_window(c, support);
        xcb_ungrab_server(c);
        xcb_flush(c);
        std::fputs("mortar: startup window management failed\n", stderr);
        return EXIT_FAILURE;
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
        std::erase_if(children, [&](pid_t child) {
            bool reaped = false;
            int error = mortar::reap_child(child, reaped);
            if (error) std::fprintf(stderr, "mortar: cannot reap child: %s\n", std::strerror(error));
            return reaped || error;
        });
        auto *event = xcb_poll_for_event(c);
        if (!event) {
            if (xcb_connection_has_error(c)) break;
            pollfd fd{xcb_get_file_descriptor(c), POLLIN, 0};
            poll(&fd, 1, 100);
            continue;
        }
        try {
            if ((event->response_type & 0x7f) == XCB_KEY_PRESS)
                actions.time = reinterpret_cast<xcb_key_press_event_t *>(event)->time;
            if (auto action = keyboard->process(*event)) {
                unsigned digit = (action->workspace + 1) % 10;
                auto result = action->move ? config.run_move_workspace(digit) : config.run_workspace(digit);
                if (result == mortar::Config::Result::absent) {
                    if (action->move) move_window(workspaces[current].focused, action->workspace);
                    else switch_workspace(action->workspace);
                } else if (result == mortar::Config::Result::failure) {
                    std::fprintf(stderr, "mortar: %sworkspace binding %u failed\n", action->move ? "move-" : "", digit);
                }
            }
            if (auto id = keyboard->callback()) {
                if (*id == split_horizontal || *id == split_vertical) {
                    actions.split(*id == split_horizontal ? mortar::Split::horizontal : mortar::Split::vertical);
                } else if (*id >= focus_left && *id <= focus_right) {
                    const mortar::Direction directions[] = {mortar::Direction::left, mortar::Direction::down,
                        mortar::Direction::up, mortar::Direction::right};
                    actions.focus(directions[*id - focus_left]);
                } else if (*id == close_focused) {
                    actions.close();
                } else if (*id == terminal || *id == launcher) {
                    actions.spawn({*id == terminal ? "x-terminal-emulator" : "dmenu_run"});
                } else if (config.run_binding(*id) != mortar::Config::Result::success) {
                    std::fprintf(stderr, "mortar: key binding %u failed\n", *id);
                }
            }
        } catch (const std::exception &error) {
            std::fprintf(stderr, "mortar: keyboard update failed: %s\n", error.what());
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
                if (enter->time != XCB_CURRENT_TIME &&
                    (pending_focus.empty() || static_cast<int16_t>(enter->sequence - focus_sequence) >= 0)) {
                    std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
                    focus(enter->event, enter->time);
                }
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
                switch_workspace(message->data.data32[0]);
            } else if (message->format == 32 && message->type == net_desktop &&
                       message->data.data32[0] < workspaces.size()) {
                move_window(message->window, message->data.data32[0]);
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
        xcb_grab_server(c);
        Property check{xcb_get_property_reply(c, xcb_get_property(c, false, root, net_check,
                                                                  XCB_ATOM_WINDOW, 0, 1), nullptr), &std::free};
        if (check && check->type == XCB_ATOM_WINDOW && check->format == 32 &&
            xcb_get_property_value_length(check.get()) == 4 && check->bytes_after == 0 &&
            *static_cast<const xcb_window_t *>(xcb_get_property_value(check.get())) == support) {
            xcb_delete_property(c, root, net_check);
            xcb_delete_property(c, root, net_supported);
            xcb_delete_property(c, root, net_number);
            xcb_delete_property(c, root, focus_time);
        }
        const uint32_t no_events = 0;
        xcb_change_window_attributes(c, root, XCB_CW_EVENT_MASK, &no_events);
        for (const auto &workspace : workspaces)
            for (auto window : workspace.windows) xcb_map_window(c, window);
        xcb_ungrab_server(c);
        std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> barrier{
            xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
        return barrier ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    std::fputs("mortar: X connection closed\n", stderr);
    return EXIT_FAILURE;
}
