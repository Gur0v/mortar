#include "actions.hpp"
#include "config.hpp"
#include "ipc.hpp"
#include "randr.hpp"
#include "scratchpad.hpp"
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
#include <stdexcept>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>
#include <chrono>
#include <thread>
#include <memory>
#include <tuple>
#include <vector>

extern char **environ;

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
        std::function<void(mortar::Direction)> move_direction;
        std::function<void(mortar::Direction, int)> resize;
        std::function<void()> close;
        std::function<void()> scratch_send, scratch_toggle, toggle_fullscreen, toggle_floating;
        std::function<void(const std::vector<std::string> &)> spawn;
        xcb_timestamp_t time = XCB_CURRENT_TIME;
    } actions;
    mortar::Config config(&actions,
        [](void *host, unsigned digit) { static_cast<Actions *>(host)->workspace(digit); },
        [](void *host, unsigned digit) { static_cast<Actions *>(host)->move(digit); },
        [](void *host, mortar::Split value) { static_cast<Actions *>(host)->split(value); },
        [](void *host, mortar::Direction value) { static_cast<Actions *>(host)->focus(value); },
        [](void *host) { static_cast<Actions *>(host)->close(); },
        [](void *host, const std::vector<std::string> &args) { static_cast<Actions *>(host)->spawn(args); },
        [](void *host, mortar::Direction value) { static_cast<Actions *>(host)->move_direction(value); },
        [](void *host, mortar::Direction value, int pixels) { static_cast<Actions *>(host)->resize(value, pixels); },
        [](void *host, mortar::Config::WindowAction action) {
            auto &actions = *static_cast<Actions *>(host);
            switch (action) {
            case mortar::Config::WindowAction::floating: actions.toggle_floating(); break;
            case mortar::Config::WindowAction::scratch_move: actions.scratch_send(); break;
            case mortar::Config::WindowAction::scratch_show: actions.scratch_toggle(); break;
            case mortar::Config::WindowAction::fullscreen: actions.toggle_fullscreen(); break;
            }
        });
    if (!config.load("/dev/null")) {
        std::fputs("mortar: cannot initialize default configuration\n", stderr);
        return EXIT_FAILURE;
    }
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::filesystem::path config_dir;
    if (xdg && *xdg && std::filesystem::path(xdg).is_absolute()) config_dir = xdg;
    else if (home && *home && std::filesystem::path(home).is_absolute()) config_dir = std::filesystem::path(home) / ".config";
    const auto path = config_dir.empty() ? std::filesystem::path{} : config_dir / "mortar/config.scm";
    if (!path.empty()) {
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
    enum : uint32_t { split_horizontal = UINT32_MAX - 21, split_vertical, focus_left, focus_down,
        focus_up, focus_right, move_left, move_down, move_up, move_right,
        resize_left, resize_down, resize_up, resize_right, close_focused, terminal, launcher, toggle_floating, toggle_fullscreen, scratch_send, scratch_toggle, reload_config };
    std::vector<mortar::KeyboardBinding> defaults{
        {XKB_KEY_h, 0, split_horizontal, true}, {XKB_KEY_v, 0, split_vertical, true},
        {XKB_KEY_j, 0, focus_left, true}, {XKB_KEY_k, 0, focus_down, true},
        {XKB_KEY_l, 0, focus_up, true}, {XKB_KEY_semicolon, 0, focus_right, true},
        {XKB_KEY_Left, 0, focus_left, true}, {XKB_KEY_Down, 0, focus_down, true},
        {XKB_KEY_Up, 0, focus_up, true}, {XKB_KEY_Right, 0, focus_right, true},
        {XKB_KEY_j, XCB_MOD_MASK_SHIFT, move_left, true}, {XKB_KEY_k, XCB_MOD_MASK_SHIFT, move_down, true},
        {XKB_KEY_l, XCB_MOD_MASK_SHIFT, move_up, true}, {XKB_KEY_semicolon, XCB_MOD_MASK_SHIFT, move_right, true},
        {XKB_KEY_Left, XCB_MOD_MASK_SHIFT, move_left, true}, {XKB_KEY_Down, XCB_MOD_MASK_SHIFT, move_down, true},
        {XKB_KEY_Up, XCB_MOD_MASK_SHIFT, move_up, true}, {XKB_KEY_Right, XCB_MOD_MASK_SHIFT, move_right, true},
        {XKB_KEY_Left, XCB_MOD_MASK_CONTROL, resize_left, true}, {XKB_KEY_Down, XCB_MOD_MASK_CONTROL, resize_down, true},
        {XKB_KEY_Up, XCB_MOD_MASK_CONTROL, resize_up, true}, {XKB_KEY_Right, XCB_MOD_MASK_CONTROL, resize_right, true},
        {XKB_KEY_q, XCB_MOD_MASK_SHIFT, close_focused, true},
        {XKB_KEY_space, XCB_MOD_MASK_SHIFT, toggle_floating, true}, {XKB_KEY_f, 0, toggle_fullscreen, true},
        {XKB_KEY_minus, XCB_MOD_MASK_SHIFT, scratch_send, true}, {XKB_KEY_minus, 0, scratch_toggle, true},
        {XKB_KEY_Return, 0, terminal, true}, {XKB_KEY_d, 0, launcher, true},
        {XKB_KEY_c, XCB_MOD_MASK_SHIFT, reload_config, true}
    };
    auto prepare_bindings = [&](const auto &custom) {
        auto bindings = defaults;
        for (const auto &binding : custom) {
            std::erase_if(bindings, [&](const auto &existing) {
                return existing.keysym == binding.keysym &&
                       (existing.modifiers | (existing.dynamic_alt ? keyboard->alt_modifier() : 0)) == binding.modifiers;
            });
            bindings.push_back({binding.keysym, binding.modifiers, binding.callback});
        }
        return bindings;
    };
    try {
        keyboard->replace_bindings(prepare_bindings(config.bindings()));
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
    mortar::Scratchpad scratchpad;
    std::vector<xcb_window_t> scratch_windows;
    auto &workspaces = workspace_state.workspaces;
    auto &current = workspace_state.current;
    std::vector<xcb_window_t> iconic;
    std::vector<std::pair<xcb_window_t, std::array<uint32_t, 5>>> floating_geometry;
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
    const auto net_clients = atom("_NET_CLIENT_LIST", 16);
    const auto net_stacking = atom("_NET_CLIENT_LIST_STACKING", 25);
    const auto net_active = atom("_NET_ACTIVE_WINDOW", 18);
    const auto net_names = atom("_NET_DESKTOP_NAMES", 18);
    const auto net_number = atom("_NET_NUMBER_OF_DESKTOPS", 23);
    const auto net_current = atom("_NET_CURRENT_DESKTOP", 20);
    const auto net_desktop = atom("_NET_WM_DESKTOP", 15);
    const auto net_check = atom("_NET_SUPPORTING_WM_CHECK", 24);
    const auto net_name = atom("_NET_WM_NAME", 12);
    const auto utf8 = atom("UTF8_STRING", 11);
    const auto window_type = atom("_NET_WM_WINDOW_TYPE", 19);
    const auto dialog_type = atom("_NET_WM_WINDOW_TYPE_DIALOG", 26);
    const auto dock_type = atom("_NET_WM_WINDOW_TYPE_DOCK", 24);
    const auto strut = atom("_NET_WM_STRUT", 13);
    const auto strut_partial = atom("_NET_WM_STRUT_PARTIAL", 21);
    const auto net_state = atom("_NET_WM_STATE", 13);
    const auto fullscreen_atom = atom("_NET_WM_STATE_FULLSCREEN", 24);
    if (!net_clients || !net_stacking || !net_active || !net_names || !net_supported || !net_number || !net_current || !net_desktop || !net_check || !net_name || !utf8 || !window_type || !dialog_type || !dock_type || !strut || !strut_partial || !net_state || !fullscreen_atom) return EXIT_FAILURE;
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
    std::vector<std::pair<xcb_window_t, std::array<uint32_t, 5>>> fullscreen;
    auto is_fullscreen = [&](xcb_window_t window) {
        return std::find_if(fullscreen.begin(), fullscreen.end(), [&](const auto &entry) { return entry.first == window; });
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
    mortar::Outputs outputs;
    try {
        if (!outputs.update(mortar::discover_outputs(c, root)) || outputs.outputs().empty())
            throw std::runtime_error("invalid output configuration");
        outputs.switch_workspace(current);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "mortar: output discovery failed: %s; using root geometry\n", error.what());
        if (!outputs.update({{"root", {0, 0, screens.data->width_in_pixels, screens.data->height_in_pixels}}}))
            return EXIT_FAILURE;
        outputs.switch_workspace(current);
    }
    auto visible = [&](uint32_t workspace) {
        return std::any_of(outputs.outputs().begin(), outputs.outputs().end(),
            [&](const auto &output) { return output.workspace == workspace; });
    };
    auto bounds = [&]() {
        auto it = std::find_if(outputs.outputs().begin(), outputs.outputs().end(),
            [&](const auto &output) { return output.workspace == current; });
        return it != outputs.outputs().end() ? it->bounds : mortar::Rect{0, 0, screens.data->width_in_pixels, screens.data->height_in_pixels};
    };
    const auto *randr = xcb_get_extension_data(c, &xcb_randr_id);
    if (randr && randr->present)
        xcb_randr_select_input(c, root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
            XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
            XCB_RANDR_NOTIFY_MASK_RESOURCE_CHANGE);
    const auto protocols = atom("WM_PROTOCOLS", 12);
    const auto take_focus = atom("WM_TAKE_FOCUS", 13);
    const auto delete_window = atom("WM_DELETE_WINDOW", 16);
    const auto focus_time = atom("_MORTAR_FOCUS_TIME", 18);
    if (!protocols || !take_focus || !delete_window || !focus_time) {
        std::fputs("mortar: cannot initialize focus atoms\n", stderr);
        return EXIT_FAILURE;
    }
    struct BorderColors {
        xcb_connection_t *connection;
        std::vector<std::tuple<xcb_window_t, xcb_colormap_t, uint32_t, uint32_t>> colors;
        void remove(xcb_window_t window) {
            auto found = std::find_if(colors.begin(), colors.end(), [&](const auto &entry) {
                return std::get<0>(entry) == window;
            });
            if (found == colors.end()) return;
            auto [ignored, colormap, rgb, pixel] = *found;
            (void)ignored;
            (void)rgb;
            const uint32_t zero = 0;
            xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &zero);
            xcb_free_colors(connection, colormap, 0, 1, &pixel);
            colors.erase(found);
        }
        ~BorderColors() {
            while (!colors.empty()) remove(std::get<0>(colors.back()));
        }
        void apply(xcb_window_t window, uint32_t rgb) {
            std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> attributes{
                xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, window), nullptr), &std::free};
            if (!attributes || attributes->colormap == XCB_NONE) return;
            auto colormap = attributes->colormap;
            auto found = std::find_if(colors.begin(), colors.end(), [&](const auto &entry) {
                return std::get<0>(entry) == window;
            });
            if (found != colors.end() && std::get<1>(*found) == colormap && std::get<2>(*found) == rgb) return;
            {
                xcb_generic_error_t *raw_error = nullptr;
                std::unique_ptr<xcb_alloc_color_reply_t, decltype(&std::free)> reply{
                    xcb_alloc_color_reply(connection, xcb_alloc_color(connection, colormap,
                        ((rgb >> 16) & 255) * 257, ((rgb >> 8) & 255) * 257, (rgb & 255) * 257), &raw_error), &std::free};
                std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> error{raw_error, &std::free};
                if (!reply) {
                    std::fprintf(stderr, "mortar: cannot allocate border color (X error %u)\n", error ? error->error_code : 0);
                    return;
                }
                const auto pixel = reply->pixel;
                std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> change_error{
                    xcb_request_check(connection, xcb_change_window_attributes_checked(connection, window,
                        XCB_CW_BORDER_PIXEL, &pixel)), &std::free};
                if (change_error) {
                    std::fprintf(stderr, "mortar: cannot set border color (X error %u)\n", change_error->error_code);
                    xcb_free_colors(connection, colormap, 0, 1, &pixel);
                    return;
                }
                if (found != colors.end()) {
                    const auto old_colormap = std::get<1>(*found);
                    const auto old_pixel = std::get<3>(*found);
                    xcb_free_colors(connection, old_colormap, 0, 1, &old_pixel);
                    *found = {window, colormap, rgb, pixel};
                } else colors.emplace_back(window, colormap, rgb, pixel);
            }
        }
    } border_colors{c, {}};
    std::function<void(std::string_view, xcb_window_t)> window_event = [](std::string_view, xcb_window_t) {};
    std::vector<xcb_window_t> pending_focus;
    uint16_t focus_sequence = 0;
    auto focus = [&](xcb_window_t window, xcb_timestamp_t time) {
        auto active = std::find_if(fullscreen.begin(), fullscreen.end(), [&](const auto &entry) {
            return owner(entry.first) == static_cast<int>(current);
        });
        if (active != fullscreen.end() && window != active->first) {
            Property transient{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_TRANSIENT_FOR,
                XCB_ATOM_WINDOW, 0, 1), nullptr), &std::free};
            bool dialog = transient && transient->type == XCB_ATOM_WINDOW && transient->format == 32 &&
                xcb_get_property_value_length(transient.get()) == 4 &&
                *static_cast<const xcb_window_t *>(xcb_get_property_value(transient.get())) == active->first;
            if (!dialog) window = active->first;
        }
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
        if (accepts_focus) {
            for (const auto &output : outputs.outputs())
                for (auto managed : workspaces[output.workspace].windows)
                    border_colors.apply(managed, output.workspace == current && managed == window
                        ? config.border().focused : config.border().unfocused);
            workspaces[current].focused = window;
            workspaces[current].layout.focus(window);
            if (!workspaces[current].layout.contains(window) || is_fullscreen(window) != fullscreen.end()) {
                const uint32_t above = XCB_STACK_MODE_ABOVE;
                xcb_configure_window(c, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);
            }
        }
        xcb_flush(c);
    };
    std::vector<xcb_window_t> docks;
    auto usable = [&](mortar::Rect area) {
        int64_t left = area.x, top = area.y, right = int64_t(area.x) + area.width, bottom = int64_t(area.y) + area.height;
        const int64_t root_width = screens.data->width_in_pixels, root_height = screens.data->height_in_pixels;
        for (auto window : docks) {
            Property reply{xcb_get_property_reply(c, xcb_get_property(c, false, window, strut_partial,
                XCB_ATOM_CARDINAL, 0, 12), nullptr), &std::free};
            bool partial = reply && reply->type == XCB_ATOM_CARDINAL && reply->format == 32 &&
                !reply->bytes_after && xcb_get_property_value_length(reply.get()) == 48;
            if (!partial) reply.reset(xcb_get_property_reply(c, xcb_get_property(c, false, window, strut,
                XCB_ATOM_CARDINAL, 0, 4), nullptr));
            if (!reply || reply->type != XCB_ATOM_CARDINAL || reply->format != 32 || reply->bytes_after ||
                xcb_get_property_value_length(reply.get()) != (partial ? 48 : 16)) continue;
            const auto *v = static_cast<const uint32_t *>(xcb_get_property_value(reply.get()));
            auto overlap = [](int64_t a, int64_t b, uint32_t start, uint32_t end) {
                return start <= end && int64_t(start) < b && int64_t(end) >= a;
            };
            if (v[0] <= root_width && (!partial || overlap(area.y, int64_t(area.y) + area.height, v[4], v[5]))) left = std::max(left, int64_t(v[0]));
            if (v[1] <= root_width && (!partial || overlap(area.y, int64_t(area.y) + area.height, v[6], v[7]))) right = std::min(right, root_width - v[1]);
            if (v[2] <= root_height && (!partial || overlap(area.x, int64_t(area.x) + area.width, v[8], v[9]))) top = std::max(top, int64_t(v[2]));
            if (v[3] <= root_height && (!partial || overlap(area.x, int64_t(area.x) + area.width, v[10], v[11]))) bottom = std::min(bottom, root_height - v[3]);
        }
        left = std::clamp(left, int64_t(area.x), int64_t(area.x) + area.width - 1);
        top = std::clamp(top, int64_t(area.y), int64_t(area.y) + area.height - 1);
        right = std::clamp(right, left + 1, int64_t(area.x) + area.width);
        bottom = std::clamp(bottom, top + 1, int64_t(area.y) + area.height);
        return mortar::Rect{static_cast<int>(left), static_cast<int>(top), static_cast<int>(right-left), static_cast<int>(bottom-top)};
    };
    auto tile = [&] {
        for (const auto &output : outputs.outputs()) {
        const auto workspace = output.workspace;
        for (auto window : workspaces[workspace].windows) {
            const uint32_t color = workspace == current && window == workspaces[workspace].focused
                ? config.border().focused : config.border().unfocused;
            border_colors.apply(window, color);
        }
        for (auto window : workspaces[workspace].floating) {
            if (is_fullscreen(window) != fullscreen.end()) continue;
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
            if (!geometry) continue;
            const uint32_t outer_width = geometry->width + 2u * geometry->border_width;
            const uint32_t outer_height = geometry->height + 2u * geometry->border_width;
            const uint32_t border = std::min<uint32_t>(config.border().width,
                (std::min(outer_width, outer_height) - 1) / 2);
            const uint32_t values[] = {outer_width - 2 * border, outer_height - 2 * border, border};
            xcb_configure_window(c, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
        }
        for (const auto &placement : workspaces[workspace].layout.geometry(usable(output.bounds))) {
            if (is_fullscreen(placement.window) != fullscreen.end()) continue;
            using Property = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;
            Property hints{xcb_get_property_reply(c, xcb_get_property(c, false, placement.window,
                XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 18), nullptr), &std::free};
            const uint32_t border = std::min<uint32_t>(config.border().width,
                (std::min(placement.bounds.width, placement.bounds.height) - 1) / 2);
            uint32_t tile_width = std::max(1, placement.bounds.width - static_cast<int>(2 * border));
            uint32_t tile_height = std::max(1, placement.bounds.height - static_cast<int>(2 * border));
            if (hints && hints->type == XCB_ATOM_WM_SIZE_HINTS && hints->format == 32) {
                std::tie(tile_width, tile_height) = hinted_size(tile_width, tile_height,
                    static_cast<const uint32_t *>(xcb_get_property_value(hints.get())),
                    xcb_get_property_value_length(hints.get()) / 4);
            }
            const uint32_t values[] = {static_cast<uint32_t>(placement.bounds.x),
                                       static_cast<uint32_t>(placement.bounds.y), tile_width, tile_height, border};
            xcb_configure_window(c, placement.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                                 XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
        }
        const uint32_t above = XCB_STACK_MODE_ABOVE;
        for (auto window : workspaces[workspace].floating)
            if (window != workspaces[workspace].focused)
                xcb_configure_window(c, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        if (std::find(workspaces[workspace].floating.begin(), workspaces[workspace].floating.end(),
                      workspaces[workspace].focused) != workspaces[workspace].floating.end())
            xcb_configure_window(c, workspaces[workspace].focused, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        for (const auto &[window, geometry] : fullscreen) {
            if (owner(window) != static_cast<int>(workspace)) continue;
            const uint32_t values[] = {static_cast<uint32_t>(output.bounds.x), static_cast<uint32_t>(output.bounds.y), static_cast<uint32_t>(output.bounds.width), static_cast<uint32_t>(output.bounds.height), 0, XCB_STACK_MODE_ABOVE};
            xcb_configure_window(c, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH |
                XCB_CONFIG_WINDOW_STACK_MODE, values);
        }
        auto focused = workspaces[workspace].focused;
        if (focused && is_fullscreen(focused) == fullscreen.end() &&
            !workspaces[workspace].layout.contains(focused)) {
            Property transient{xcb_get_property_reply(c, xcb_get_property(c, false, focused, XCB_ATOM_WM_TRANSIENT_FOR,
                XCB_ATOM_WINDOW, 0, 1), nullptr), &std::free};
            if (transient && transient->type == XCB_ATOM_WINDOW && transient->format == 32 &&
                xcb_get_property_value_length(transient.get()) == 4 &&
                is_fullscreen(*static_cast<const xcb_window_t *>(xcb_get_property_value(transient.get()))) != fullscreen.end()) {
                const uint32_t above = XCB_STACK_MODE_ABOVE;
                xcb_configure_window(c, focused, XCB_CONFIG_WINDOW_STACK_MODE, &above);
            }
        }
        }
        xcb_flush(c);
    };
    std::function<void(xcb_window_t, bool)> set_fullscreen;
    set_fullscreen = [&](xcb_window_t window, bool enabled) {
        if (owner(window) < 0) return;
        auto saved = is_fullscreen(window);
        if ((saved != fullscreen.end()) == enabled) return;
        Property state{xcb_get_property_reply(c, xcb_get_property(c, false, window, net_state,
            XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
        if (!state || state->bytes_after || (state->type != XCB_ATOM_NONE &&
            (state->type != XCB_ATOM_ATOM || state->format != 32))) return;
        std::vector<xcb_atom_t> atoms;
        if (state->type == XCB_ATOM_ATOM) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(state.get()));
            atoms.assign(values, values + xcb_get_property_value_length(state.get()) / 4);
        }
        std::erase(atoms, fullscreen_atom);
        if (enabled) {
            auto previous = std::find_if(fullscreen.begin(), fullscreen.end(), [&](const auto &entry) {
                return owner(entry.first) == owner(window);
            });
            if (previous != fullscreen.end()) {
                auto prior = previous->first;
                set_fullscreen(prior, false);
                if (is_fullscreen(prior) != fullscreen.end()) return;
            }
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
            if (!geometry) return;
            fullscreen.push_back({window, {static_cast<uint32_t>(geometry->x), static_cast<uint32_t>(geometry->y),
                geometry->width, geometry->height, geometry->border_width}});
            atoms.push_back(fullscreen_atom);
        } else {
            auto geometry = saved->second;
            fullscreen.erase(saved);
            if (!workspaces[owner(window)].layout.contains(window))
                xcb_configure_window(c, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH, geometry.data());
        }
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, net_state, XCB_ATOM_ATOM, 32, atoms.size(), atoms.data());
        tile();
        if (enabled && owner(window) == static_cast<int>(current)) {
            std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
            pending_focus.push_back(window);
            focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time,
                XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
        }
    };
    auto restore_focus = [&] {
        std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
        auto window = workspaces[current].focused;
        if (window && owner(window) == static_cast<int>(current)) {
            pending_focus.push_back(window);
            focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
        } else xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
        xcb_flush(c);
    };
    std::function<void(uint32_t)> workspace_event = [](uint32_t) {};
    std::function<void(uint32_t)> settle_workspace = [](uint32_t) {};
    auto switch_workspace = [&](uint32_t destination) {
        if (destination >= workspaces.size() || destination == current) return;
        auto old_outputs = outputs.outputs();
        if (outputs.owners()[destination]) outputs.focus_output(*outputs.owners()[destination]);
        if (!outputs.switch_workspace(destination)) return;
        for (const auto &output : old_outputs)
            if (!visible(output.workspace))
                for (auto window : workspaces[output.workspace].windows) unmap(window);
        settle_workspace(destination);
        workspace_state.switch_to(outputs.focused_workspace());
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
        tile();
        for (const auto &output : outputs.outputs())
            if (std::none_of(old_outputs.begin(), old_outputs.end(), [&](const auto &old) { return old.workspace == output.workspace; }))
                for (auto window : workspaces[output.workspace].windows) xcb_map_window(c, window);
        restore_focus();
        workspace_event(current);
    };
    auto workspace_bounds = [&](uint32_t workspace) -> std::optional<mortar::Rect> {
        if (!outputs.owners()[workspace]) return std::nullopt;
        auto output = std::find_if(outputs.outputs().begin(), outputs.outputs().end(), [&](const auto &item) {
            return item.name == *outputs.owners()[workspace];
        });
        return output == outputs.outputs().end() ? std::nullopt : std::optional{output->bounds};
    };
    std::vector<std::pair<xcb_window_t, mortar::Rect>> pending_floating_origins;
    auto migrate_window = [&](xcb_window_t window, mortar::Rect from, mortar::Rect to, bool floating) {
        if (from == to) return;
        auto translate = [&](uint32_t &x, uint32_t &y) {
            x = static_cast<uint32_t>(std::clamp<int64_t>(int64_t(static_cast<int32_t>(x)) - from.x + to.x,
                to.x, int64_t(to.x) + to.width - 1));
            y = static_cast<uint32_t>(std::clamp<int64_t>(int64_t(static_cast<int32_t>(y)) - from.y + to.y,
                to.y, int64_t(to.y) + to.height - 1));
        };
        auto remembered = std::find_if(floating_geometry.begin(), floating_geometry.end(),
            [&](const auto &entry) { return entry.first == window; });
        if (remembered != floating_geometry.end()) translate(remembered->second[0], remembered->second[1]);
        if (!floating) return;
        auto saved = is_fullscreen(window);
        if (saved != fullscreen.end()) {
            translate(saved->second[0], saved->second[1]);
        } else {
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
            if (!geometry) return;
            uint32_t x = static_cast<uint32_t>(geometry->x), y = static_cast<uint32_t>(geometry->y);
            translate(x, y);
            const uint32_t position[] = {x, y};
            xcb_configure_window(c, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, position);
        }
    };
    settle_workspace = [&](uint32_t workspace) {
        auto target = workspace_bounds(workspace);
        if (!target) return;
        std::erase_if(pending_floating_origins, [&](const auto &entry) {
            if (owner(entry.first) != static_cast<int>(workspace)) return false;
            migrate_window(entry.first, entry.second, *target, !workspaces[workspace].layout.contains(entry.first));
            return true;
        });
    };
    auto move_window = [&](xcb_window_t window, uint32_t to) {
        if (to >= workspaces.size()) return;
        int from = owner(window);
        if (from >= 0 && static_cast<uint32_t>(from) != to) {
            auto &source = workspaces[from];
            auto &destination = workspaces[to];
            bool was_focused = source.focused == window;
            bool destination_empty = destination.windows.empty();
            if (is_fullscreen(window) != fullscreen.end()) {
                auto existing = std::find_if(fullscreen.begin(), fullscreen.end(), [&](const auto &entry) {
                    return entry.first != window && owner(entry.first) == static_cast<int>(to);
                });
                if (existing != fullscreen.end()) {
                    auto prior = existing->first;
                    set_fullscreen(prior, false);
                    if (is_fullscreen(prior) != fullscreen.end()) return;
                }
            }
            if (scratchpad.get(window)) {
                scratchpad.remove(window);
                std::erase(scratch_windows, window);
            }
            auto from_bounds = workspace_bounds(from);
            auto to_bounds = workspace_bounds(to);
            bool floating = !source.layout.contains(window);
            if (!workspace_state.move(window, to)) return;
            auto pending = std::find_if(pending_floating_origins.begin(), pending_floating_origins.end(),
                [&](const auto &entry) { return entry.first == window; });
            if (pending != pending_floating_origins.end()) from_bounds = pending->second;
            if (from_bounds && to_bounds) {
                migrate_window(window, *from_bounds, *to_bounds, floating);
                std::erase_if(pending_floating_origins, [&](const auto &entry) { return entry.first == window; });
            } else if (from_bounds && pending == pending_floating_origins.end()) {
                pending_floating_origins.emplace_back(window, *from_bounds);
            }
            set_desktop(window, to);
            window_event("move", window);
            if (visible(from) && !visible(to)) unmap(window);
            if (!visible(from) && visible(to)) xcb_map_window(c, window);
            tile();
            if (static_cast<uint32_t>(from) == current && was_focused) restore_focus();
            else if (to == current && (destination_empty || is_fullscreen(window) != fullscreen.end())) {
                if (is_fullscreen(window) != fullscreen.end()) destination.focused = window;
                restore_focus();
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
        ws.layout.set_split(ws.tiled_focus(), direction);
        tile();
    };
    actions.focus = [&](mortar::Direction direction) {
        auto &ws = workspaces[current];
        auto target = ws.layout.contains(ws.focused) ? ws.layout.neighbor(ws.focused, direction) : ws.tiled_focus();
        if (target) {
            std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
            focus(target, actions.time);
        } else {
            auto area = bounds();
            const mortar::Output *best = nullptr;
            for (const auto &output : outputs.outputs()) {
                auto other = output.bounds;
                bool horizontal = direction == mortar::Direction::left || direction == mortar::Direction::right;
                bool overlap = horizontal ? int64_t(other.y) + other.height > area.y && int64_t(area.y) + area.height > other.y
                                          : int64_t(other.x) + other.width > area.x && int64_t(area.x) + area.width > other.x;
                int origin = horizontal ? area.x : area.y;
                int candidate = horizontal ? other.x : other.y;
                bool forward = direction == mortar::Direction::right || direction == mortar::Direction::down;
                if (!overlap || (forward ? candidate <= origin : candidate >= origin)) continue;
                int previous = best ? (horizontal ? best->bounds.x : best->bounds.y) : candidate;
                if (!best || (forward ? candidate < previous : candidate > previous)) best = &output;
            }
            if (best) switch_workspace(best->workspace);
        }
    };
    actions.move_direction = [&](mortar::Direction direction) {
        auto &ws = workspaces[current];
        if (is_fullscreen(ws.focused) == fullscreen.end() && ws.layout.move(ws.focused, direction)) {
            window_event("move", ws.focused);
            tile();
        }
    };
    actions.resize = [&](mortar::Direction direction, int pixels) {
        auto &ws = workspaces[current];
        if (is_fullscreen(ws.focused) == fullscreen.end() &&
            ws.layout.resize(ws.focused, direction, pixels, bounds())) tile();
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
    std::vector<xcb_window_t> clients;
    auto manage = [&](xcb_window_t window, bool existing) {
        if (std::find(docks.begin(), docks.end(), window) != docks.end()) { if (!existing) xcb_map_window(c, window); return; }
        if (window == support || (scratchpad.get(window) && owner(window) < 0)) return;
        if (owner(window) >= 0) {
            if (!existing && visible(owner(window))) {
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
                if (std::find(clients.begin(), clients.end(), window) == clients.end()) clients.push_back(window);
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
                    if (std::find(clients.begin(), clients.end(), window) == clients.end()) clients.push_back(window);
                    set_desktop(window, desktop);
                    set_state(window, 3);
                    xcb_flush(c);
                    return;
                }
            }
        }
        Property transient{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_TRANSIENT_FOR,
            XCB_ATOM_WINDOW, 0, 1), nullptr), &std::free};
        Property types{xcb_get_property_reply(c, xcb_get_property(c, false, window, window_type,
            XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
        if (types && types->type == XCB_ATOM_ATOM && types->format == 32 && !types->bytes_after) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(types.get()));
            auto count = xcb_get_property_value_length(types.get()) / 4;
            if (std::find(values, values + count, dock_type) != values + count) {
                const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
                xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &mask);
                docks.push_back(window);
                if (std::find(clients.begin(), clients.end(), window) == clients.end()) clients.push_back(window);
                if (!existing) xcb_map_window(c, window);
                tile();
                return;
            }
        }
        bool floating = transient && transient->type == XCB_ATOM_WINDOW && transient->format == 32 &&
                        xcb_get_property_value_length(transient.get()) == 4 &&
                        *static_cast<const xcb_window_t *>(xcb_get_property_value(transient.get())) != XCB_NONE;
        if (types && types->type == XCB_ATOM_ATOM && types->format == 32) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(types.get()));
            auto count = xcb_get_property_value_length(types.get()) / 4;
            floating = floating || std::find(values, values + count, dialog_type) != values + count;
        }
        Property window_class{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_CLASS,
            XCB_ATOM_STRING, 0, 1024), nullptr), &std::free};
        if (window_class && window_class->type == XCB_ATOM_STRING && window_class->format == 8 &&
            !window_class->bytes_after) {
            auto *value = static_cast<const char *>(xcb_get_property_value(window_class.get()));
            auto length = static_cast<size_t>(xcb_get_property_value_length(window_class.get()));
            auto *instance_end = static_cast<const char *>(std::memchr(value, 0, length));
            if (instance_end && instance_end + 1 < value + length) {
                auto *class_start = instance_end + 1;
                auto *class_end = static_cast<const char *>(std::memchr(class_start, 0, value + length - class_start));
                if (class_end && class_end + 1 == value + length) {
                    std::string_view name(class_start, class_end - class_start);
                    for (const auto &rule : config.window_rules()) {
                        if (name != rule.window_class) continue;
                        if (rule.workspace) desktop = (*rule.workspace + 9) % 10;
                        if (rule.floating) floating = *rule.floating;
                    }
                }
            }
        }
        const uint32_t event_mask = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE |
                                    XCB_EVENT_MASK_COLOR_MAP_CHANGE;
        xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &event_mask);
        std::erase(iconic, window);
        if (floating) {
            if (!workspaces[desktop].add_floating(window)) return;
        } else {
            if (!workspaces[desktop].layout.insert_after(workspaces[desktop].tiled_focus(), mortar::Layout::leaf(window))) return;
            workspaces[desktop].windows.push_back(window);
        }
        if (std::find(clients.begin(), clients.end(), window) == clients.end()) clients.push_back(window);
        if (desktop != current || !workspaces[desktop].focused) workspaces[desktop].focused = window;
        set_desktop(window, desktop);
        set_state(window, 1);
        window_event("new", window);
        Property initial_state{xcb_get_property_reply(c, xcb_get_property(c, false, window, net_state,
            XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
        if (initial_state && !initial_state->bytes_after && initial_state->type == XCB_ATOM_ATOM && initial_state->format == 32) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(initial_state.get()));
            if (std::find(values, values + xcb_get_property_value_length(initial_state.get()) / 4, fullscreen_atom) !=
                values + xcb_get_property_value_length(initial_state.get()) / 4) set_fullscreen(window, true);
        }
        tile();
        if (!visible(desktop)) {
            if (attributes->map_state != XCB_MAP_STATE_UNMAPPED) unmap(window);
        } else if (!existing || attributes->map_state == XCB_MAP_STATE_UNMAPPED) {
            xcb_map_window(c, window);
            if (desktop == current) {
            std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
            pending_focus.push_back(window);
            focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
            }
        }
        xcb_flush(c);
    };
    std::vector<xcb_window_t> published_clients, published_stack;
    xcb_window_t published_active = XCB_NONE;
    bool published = false;
    auto publish = [&] {
        std::unique_ptr<xcb_query_tree_reply_t, decltype(&std::free)> reply{
            xcb_query_tree_reply(c, xcb_query_tree(c, root), nullptr), &std::free};
        if (!reply) return;
        std::vector<xcb_window_t> stack;
        auto *children = xcb_query_tree_children(reply.get());
        for (int i = 0; i < xcb_query_tree_children_length(reply.get()); ++i)
            if (std::find(clients.begin(), clients.end(), children[i]) != clients.end()) stack.push_back(children[i]);
        auto active = workspaces[current].focused;
        if (!published || clients != published_clients) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_clients, XCB_ATOM_WINDOW, 32, clients.size(), clients.data());
            published_clients = clients;
        }
        if (!published || stack != published_stack) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_stacking, XCB_ATOM_WINDOW, 32, stack.size(), stack.data());
            published_stack = std::move(stack);
        }
        if (!published || active != published_active) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_active, XCB_ATOM_WINDOW, 32, 1, &active);
            if (published && active) window_event("focus", active);
            published_active = active;
        }
        published = true;
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
    const xcb_atom_t supported_atoms[] = {net_clients, net_stacking, net_active, net_names, net_number, net_current, net_desktop, net_check, net_name, net_state, fullscreen_atom, window_type, dock_type, strut, strut_partial};
    const uint32_t desktop_count = workspaces.size();
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_supported, XCB_ATOM_ATOM, 32, std::size(supported_atoms), supported_atoms);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_number, XCB_ATOM_CARDINAL, 32, 1, &desktop_count);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
    const char names[] = "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0" "8\0" "9\0" "10";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_names, utf8, 8, sizeof(names), names);
    try {
    auto *children = xcb_query_tree_children(tree.get());
    for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
        manage(children[i], true);
    }

    std::unique_ptr<xcb_query_pointer_reply_t, decltype(&std::free)> pointer{
        xcb_query_pointer_reply(c, xcb_query_pointer(c, root), nullptr), &std::free};
    if (pointer && owner(pointer->child) == static_cast<int>(current)) {
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
        xcb_delete_property(c, root, net_clients);
        xcb_delete_property(c, root, net_stacking);
        xcb_delete_property(c, root, net_active);
        xcb_delete_property(c, root, net_names);
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
    publish();
    xcb_ungrab_server(c);
    starting = false;
    xcb_flush(c);
    struct sigaction action{};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

    std::unique_ptr<mortar::Ipc> ipc;
    try {
        const char *runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !std::filesystem::path(runtime).is_absolute())
            throw std::runtime_error("XDG_RUNTIME_DIR must be an absolute private directory");
        ipc = std::make_unique<mortar::Ipc>(runtime);
        std::fprintf(stderr, "mortar: IPC socket %s\n", ipc->path().c_str());
    } catch (const std::exception &error) {
        std::fprintf(stderr, "mortar: IPC unavailable: %s\n", error.what());
    }
    std::vector<int> subscribers, window_subscribers;
    if (ipc) ipc->on_close = [&](int client) {
        std::erase(subscribers, client);
        std::erase(window_subscribers, client);
    };
    window_event = [&](std::string_view change, xcb_window_t window) {
        if (!ipc) return;
        auto targets = window_subscribers;
        for (int client : targets)
            ipc->send(client, "event window " + std::string(change) + " " + std::to_string(window));
    };
    workspace_event = [&](uint32_t workspace) {
        if (!ipc) return;
        auto targets = subscribers;
        for (int client : targets) ipc->send(client, "event workspace " + std::to_string(workspace));
    };
    pid_t bar = -1;
    auto start_bar = [&] {
        posix_spawn_file_actions_t bar_actions;
        int bar_error = posix_spawn_file_actions_init(&bar_actions);
        if (!bar_error) {
            bar_error = posix_spawn_file_actions_addopen(&bar_actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            if (!bar_error) bar_error = posix_spawn_file_actions_addopen(&bar_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
            if (!bar_error) {
                char name[] = "mortarbar";
                std::vector<char *> args{name};
                for (const auto &arg : config.bar_status()) args.push_back(const_cast<char *>(arg.c_str()));
                args.push_back(nullptr);
                bar_error = posix_spawnp(&bar, name, &bar_actions, nullptr, args.data(), environ);
            }
            posix_spawn_file_actions_destroy(&bar_actions);
        }
        if (bar_error) std::fprintf(stderr, "mortar: cannot start mortarbar: %s\n", std::strerror(bar_error));
    };
    auto stop_bar = [&] {
        if (bar <= 0) return true;
        for (int signal : {SIGTERM, SIGKILL}) {
            if (kill(bar, signal) < 0 && errno != ESRCH)
                std::fprintf(stderr, "mortar: cannot signal mortarbar: %s\n", std::strerror(errno));
            for (int i = 0; i < 20; ++i) {
                bool reaped = false;
                int error = mortar::reap_child(bar, reaped);
                if (reaped || error == ECHILD) { bar = -1; return true; }
                if (error) {
                    std::fprintf(stderr, "mortar: cannot reap mortarbar: %s\n", std::strerror(error));
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::fputs("mortar: mortarbar did not exit; cannot restart it\n", stderr);
        return false;
    };
    start_bar();
    actions.scratch_send = [&] {
        auto &ws = workspaces[current];
        auto window = ws.focused;
        if (window && !scratchpad.get(window)) {
            set_fullscreen(window, false);
            if (is_fullscreen(window) == fullscreen.end()) {
                std::optional<mortar::Rect> saved;
                if (!ws.layout.contains(window)) {
                    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                        xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
                    if (!geometry) throw std::runtime_error("cannot query scratchpad geometry");
                    saved = mortar::Rect{geometry->x, geometry->y, geometry->width, geometry->height};
                }
                if (scratchpad.add(window, current, saved)) {
                    scratch_windows.push_back(window);
                    if (!ws.remove_floating(window)) {
                        ws.layout.remove(window);
                        std::erase(ws.windows, window);
                    }
                    if (ws.focused == window) ws.focused = ws.windows.empty() ? XCB_NONE : ws.windows.back();
                    unmap(window);
                    tile();
                    restore_focus();
                }
            }
        } else if (window && scratchpad.get(window)) {
            set_fullscreen(window, false);
            auto result = is_fullscreen(window) == fullscreen.end() ? scratchpad.toggle(current, window, window) : std::nullopt;
            if (result && result->action == mortar::ScratchpadAction::hide) {
                ws.remove_floating(window);
                unmap(window);
                tile();
                restore_focus();
            }
        }
    };
    actions.scratch_toggle = [&] {
        std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> actual_focus{
            xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
        auto active = std::find_if(fullscreen.begin(), fullscreen.end(), [&](const auto &entry) {
            return owner(entry.first) == static_cast<int>(current);
        });
        auto result = active == fullscreen.end() ? scratchpad.toggle(current, actual_focus ? actual_focus->focus : workspaces[current].focused) : std::nullopt;
        if (result) {
            auto window = result->entry.window;
            if (result->action == mortar::ScratchpadAction::hide) {
                std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                    xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
                if (geometry) scratchpad.update_geometry(window, {geometry->x, geometry->y, geometry->width, geometry->height});
                auto &ws = workspaces[current];
                ws.remove_floating(window);
                unmap(window);
                tile();
                restore_focus();
            } else {
                if (result->action == mortar::ScratchpadAction::move) {
                    int from = owner(window);
                    if (from >= 0) workspaces[from].remove_floating(window);
                }
                if (result->action != mortar::ScratchpadAction::focus) {
                    workspaces[current].add_floating(window);
                    set_desktop(window, current);
                    auto area = bounds();
                    auto rect = result->entry.geometry.value_or(mortar::Rect{area.x + area.width / 4, area.y + area.height / 4, std::max(1, area.width / 2), std::max(1, area.height / 2)});
                    const uint32_t values[] = {static_cast<uint32_t>(rect.x), static_cast<uint32_t>(rect.y), static_cast<uint32_t>(rect.width), static_cast<uint32_t>(rect.height), 0};
                    xcb_configure_window(c, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
                    xcb_map_window(c, window);
                }
                std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
                workspaces[current].focused = window;
                pending_focus.push_back(window);
                focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
                xcb_flush(c);
            }
        }
    };
    actions.toggle_fullscreen = [&] {
        auto window = workspaces[current].focused;
        set_fullscreen(window, is_fullscreen(window) == fullscreen.end());
    };
    actions.toggle_floating = [&] {
        if (is_fullscreen(workspaces[current].focused) != fullscreen.end()) return;
        auto &ws = workspaces[current];
        auto window = ws.focused;
        if (window && !ws.layout.contains(window)) {
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
            if (geometry) {
                std::erase_if(floating_geometry, [&](const auto &entry) { return entry.first == window; });
                floating_geometry.push_back({window, {static_cast<uint32_t>(geometry->x),
                    static_cast<uint32_t>(geometry->y), geometry->width, geometry->height, geometry->border_width}});
            } else throw std::runtime_error("cannot query floating geometry");
        }
        if (window && ws.toggle_floating(window)) {
            if (!ws.layout.contains(window)) {
                auto saved = std::find_if(floating_geometry.begin(), floating_geometry.end(),
                    [&](const auto &entry) { return entry.first == window; });
                auto area = bounds();
                const std::array<uint32_t, 5> centered{static_cast<uint32_t>(area.x + area.width / 4), static_cast<uint32_t>(area.y + area.height / 4),
                    static_cast<uint32_t>(std::max(1, area.width / 2)), static_cast<uint32_t>(std::max(1, area.height / 2)), 0};
                xcb_configure_window(c, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH,
                    saved != floating_geometry.end() ? saved->second.data() : centered.data());
            }
            tile();
        }
    };

    while (!stopping) {
        if (bar > 0) {
            bool reaped = false;
            int error = mortar::reap_child(bar, reaped);
            if (error || reaped) {
                if (error) std::fprintf(stderr, "mortar: cannot reap mortarbar: %s\n", std::strerror(error));
                bar = -1;
            }
        }
        std::erase_if(children, [&](pid_t child) {
            bool reaped = false;
            int error = mortar::reap_child(child, reaped);
            if (error) std::fprintf(stderr, "mortar: cannot reap child: %s\n", std::strerror(error));
            return reaped || error;
        });
        auto *event = xcb_poll_for_event(c);
        if (xcb_connection_has_error(c)) { std::free(event); break; }
        auto fds = ipc ? ipc->poll_fds() : std::vector<pollfd>{};
        fds.push_back({xcb_get_file_descriptor(c), POLLIN, 0});
        int ready = poll(fds.data(), fds.size(), event ? 0 : 100);
        if (ready < 0 && errno != EINTR) {
            std::fprintf(stderr, "mortar: poll failed: %s\n", std::strerror(errno));
            std::free(event);
            break;
        }
        bool ipc_dispatched = ipc && ready > 0;
        if (ipc_dispatched) {
            fds.pop_back();
            try {
                ipc->dispatch(fds, [&](int client, std::string_view frame) {
                    if (frame == "subscribe workspace") {
                        if (std::find(subscribers.begin(), subscribers.end(), client) == subscribers.end())
                            subscribers.push_back(client);
                        ipc->send(client, "ok subscribed");
                    } else if (frame == "subscribe window") {
                        if (std::find(window_subscribers.begin(), window_subscribers.end(), client) == window_subscribers.end())
                            window_subscribers.push_back(client);
                        ipc->send(client, "ok subscribed");
                    } else if (frame.starts_with("eval ")) {
                        actions.time = XCB_CURRENT_TIME;
                        auto result = config.evaluate(std::string(frame.substr(5)));
                        ipc->send(client, (result.success ? "ok " : "error ") + result.text);
                    } else ipc->send(client, "error unknown request");
                });
            } catch (const std::exception &error) {
                std::fprintf(stderr, "mortar: IPC failed: %s\n", error.what());
                ipc.reset();
            }
        }
        if (!event) { if (ipc_dispatched) publish(); continue; }
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
                if (*id == reload_config) {
                    if (path.empty() || !config.stage(path.c_str())) {
                        std::fprintf(stderr, "mortar: failed to reload configuration %s; keeping current configuration\n", path.c_str());
                    } else {
                        try {
                            keyboard->replace_bindings(prepare_bindings(config.staged_bindings()));
                            xcb_flush(c);
                            auto old_status = config.bar_status();
                            config.commit();
                            tile();
                            if (old_status != config.bar_status() && stop_bar()) start_bar();
                        } catch (const std::exception &error) {
                            config.discard();
                            std::fprintf(stderr, "mortar: failed to reload keybindings: %s; keeping current configuration\n", error.what());
                        }
                    }
                } else if (*id == split_horizontal || *id == split_vertical) {
                    actions.split(*id == split_horizontal ? mortar::Split::horizontal : mortar::Split::vertical);
                } else if (*id >= focus_left && *id <= focus_right) {
                    const mortar::Direction directions[] = {mortar::Direction::left, mortar::Direction::down,
                        mortar::Direction::up, mortar::Direction::right};
                    actions.focus(directions[*id - focus_left]);
                } else if (*id >= move_left && *id <= resize_right) {
                    const mortar::Direction directions[] = {mortar::Direction::left, mortar::Direction::down,
                        mortar::Direction::up, mortar::Direction::right};
                    if (*id <= move_right) actions.move_direction(directions[*id - move_left]);
                    else actions.resize(directions[*id - resize_left], 10);
                } else if (*id == close_focused) {
                    actions.close();
                } else if (*id == scratch_send) {
                    actions.scratch_send();
                } else if (*id == scratch_toggle) {
                    actions.scratch_toggle();
                } else if (*id == toggle_fullscreen) {
                    actions.toggle_fullscreen();
                } else if (*id == toggle_floating) {
                    actions.toggle_floating();
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
        if (randr && randr->present && (type == randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
            type == randr->first_event + XCB_RANDR_NOTIFY)) {
            try {
                auto old_outputs = outputs.outputs();
                auto discovered = mortar::discover_outputs(c, root);
                auto next_outputs = outputs;
                if (!next_outputs.update(discovered) || next_outputs.outputs().empty())
                    throw std::runtime_error("invalid output configuration");
                auto old_owners = outputs.owners();
                outputs = std::move(next_outputs);
                for (size_t workspace = 0; workspace < workspaces.size(); ++workspace) {
                    if (!old_owners[workspace] || !outputs.owners()[workspace]) continue;
                    auto old = std::find_if(old_outputs.begin(), old_outputs.end(), [&](const auto &output) {
                        return output.name == *old_owners[workspace];
                    });
                    auto next = std::find_if(outputs.outputs().begin(), outputs.outputs().end(), [&](const auto &output) {
                        return output.name == *outputs.owners()[workspace];
                    });
                    if (old == old_outputs.end() || next == outputs.outputs().end() || old->bounds == next->bounds) continue;
                    for (auto window : workspaces[workspace].windows) {
                        if (std::any_of(pending_floating_origins.begin(), pending_floating_origins.end(),
                            [&](const auto &entry) { return entry.first == window; })) continue;
                        migrate_window(window, old->bounds, next->bounds,
                            !workspaces[workspace].layout.contains(window));
                    }
                }
                for (const auto &old : old_outputs)
                    if (!visible(old.workspace))
                        for (auto window : workspaces[old.workspace].windows) unmap(window);
                for (const auto &output : outputs.outputs())
                    if (std::none_of(old_outputs.begin(), old_outputs.end(), [&](const auto &old) { return old.workspace == output.workspace; }))
                        for (auto window : workspaces[output.workspace].windows) xcb_map_window(c, window);
                auto previous_workspace = current;
                current = outputs.focused_workspace();
                for (const auto &output : outputs.outputs()) settle_workspace(output.workspace);
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
                tile();
                restore_focus();
                if (current != previous_workspace) workspace_event(current);
            } catch (const std::exception &error) {
                std::fprintf(stderr, "mortar: output update failed: %s; keeping previous outputs\n", error.what());
            }
        }
        switch (type) {
        case XCB_COLORMAP_NOTIFY: {
            auto *notify = reinterpret_cast<xcb_colormap_notify_event_t *>(event);
            int workspace = owner(notify->window);
            if (notify->_new && workspace >= 0)
                border_colors.apply(notify->window, static_cast<uint32_t>(workspace) == current &&
                    workspaces[workspace].focused == notify->window ? config.border().focused : config.border().unfocused);
            break;
        }
        case XCB_ENTER_NOTIFY: {
            auto *enter = reinterpret_cast<xcb_enter_notify_event_t *>(event);
            int entered_workspace = owner(enter->event);
            if (enter->mode == XCB_NOTIFY_MODE_NORMAL && enter->detail != XCB_NOTIFY_DETAIL_INFERIOR &&
                entered_workspace >= 0 && visible(entered_workspace)) {
                if (static_cast<uint32_t>(entered_workspace) != current) switch_workspace(entered_workspace);
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
            if (!(event->response_type & 0x80) && (property->atom == strut || property->atom == strut_partial) &&
                std::find(docks.begin(), docks.end(), property->window) != docks.end()) tile();
            if (!(event->response_type & 0x80) && property->atom == XCB_ATOM_WM_NORMAL_HINTS &&
                owner(property->window) >= 0 && visible(owner(property->window))) tile();
            if (!(event->response_type & 0x80) && property->window == root && property->atom == focus_time &&
                property->state == XCB_PROPERTY_NEW_VALUE && !pending_focus.empty()) {
                auto window = pending_focus.front();
                pending_focus.erase(pending_focus.begin());
                if (property->time != XCB_CURRENT_TIME &&
                    owner(window) == static_cast<int>(current)) focus(window, property->time);
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
            } else if (message->format == 32 && message->type == net_active && owner(message->window) >= 0 &&
                       (message->data.data32[0] == 2 || visible(owner(message->window)))) {
                switch_workspace(static_cast<uint32_t>(owner(message->window)));
                std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
                pending_focus.push_back(message->window);
                focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
            } else if (message->format == 32 && message->type == net_desktop &&
                       message->data.data32[0] < workspaces.size()) {
                move_window(message->window, message->data.data32[0]);
            } else if (message->format == 32 && message->type == net_state && message->data.data32[0] <= 2 &&
                       (message->data.data32[1] == fullscreen_atom || message->data.data32[2] == fullscreen_atom)) {
                bool active = is_fullscreen(message->window) != fullscreen.end();
                set_fullscreen(message->window, message->data.data32[0] == 2 ? !active : message->data.data32[0] == 1);
            } else if (message->type == wm_change_state && message->format == 32 && message->data.data32[0] == 3 &&
                owner(message->window) >= 0 && visible(owner(message->window))) {
                auto workspace = static_cast<uint32_t>(owner(message->window));
                bool was_focused = workspaces[workspace].focused == message->window;
                scratchpad.remove(message->window);
                std::erase(scratch_windows, message->window);
                set_fullscreen(message->window, false);
                if (!workspaces[workspace].remove_floating(message->window)) {
                    workspaces[workspace].layout.remove(message->window);
                    std::erase(workspaces[workspace].windows, message->window);
                }
                if (workspaces[workspace].focused == message->window)
                    workspaces[workspace].focused = workspaces[workspace].windows.empty() ? XCB_NONE : workspaces[workspace].windows.back();
                iconic.push_back(message->window);
                set_state(message->window, 3);
                unmap(message->window);
                tile();
                if (was_focused && workspace == current) restore_focus();
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
            if (std::erase(docks, window)) { std::erase(clients, window); tile(); break; }
            int workspace = owner(window);
            bool scratch_removed = scratchpad.remove(window);
            if (scratch_removed) std::erase(scratch_windows, window);
            if (type == XCB_UNMAP_NOTIFY && workspace >= 0) set_fullscreen(window, false);
            std::erase_if(fullscreen, [&](const auto &entry) { return entry.first == window; });
            bool was_tiled = workspace >= 0;
            bool was_focused = was_tiled && workspaces[workspace].focused == window;
            if (was_tiled) {
                auto &ws = workspaces[workspace];
                if (!ws.remove_floating(window)) {
                    ws.layout.remove(window);
                    std::erase(ws.windows, window);
                }
                if (ws.focused == window) ws.focused = ws.windows.empty() ? XCB_NONE : ws.windows.back();
                xcb_delete_property(c, window, net_desktop);
            }
            auto removed = static_cast<size_t>(was_tiled || scratch_removed) + std::erase(iconic, window);
            if (removed) {
                window_event("close", window);
                std::erase(clients, window);
                border_colors.remove(window);
            }
            if (removed) std::erase_if(pending_floating_origins, [&](const auto &entry) { return entry.first == window; });
            if (removed) std::erase_if(floating_geometry, [&](const auto &entry) { return entry.first == window; });
            if (type == XCB_DESTROY_NOTIFY) std::erase_if(pending_unmaps, [&](auto entry) { return entry.first == window; });
            if (removed && type == XCB_UNMAP_NOTIFY) {
                set_state(window, 0);
                xcb_flush(c);
            }
            if (was_tiled && visible(workspace)) {
                tile();
                if (was_focused && workspace == static_cast<int>(current)) restore_focus();
            }
            break;
        }
        case XCB_CONFIGURE_REQUEST: {
            auto *request = reinterpret_cast<xcb_configure_request_event_t *>(event);
            int workspace = owner(request->window);
            if (workspace >= 0 && (workspaces[workspace].layout.contains(request->window) ||
                                   is_fullscreen(request->window) != fullscreen.end())) {
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
            if (request->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[n++] = workspace >= 0 ? config.border().width : request->border_width;
            if (request->value_mask & XCB_CONFIG_WINDOW_SIBLING) values[n++] = request->sibling;
            if (request->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) values[n++] = request->stack_mode;
            xcb_configure_window(c, request->window, request->value_mask, values);
            if (scratchpad.get(request->window) && workspace == static_cast<int>(current) &&
                workspaces[current].focused == request->window) {
                std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
                pending_focus.push_back(request->window);
                focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
            }
            xcb_flush(c);
            break;
        }
        }
        if (ipc_dispatched || type == XCB_KEY_PRESS || type == XCB_ENTER_NOTIFY ||
            type == XCB_MAP_REQUEST || type == XCB_UNMAP_NOTIFY || type == XCB_DESTROY_NOTIFY ||
            type == XCB_CLIENT_MESSAGE || type == XCB_CONFIGURE_REQUEST || type == XCB_CONFIGURE_NOTIFY ||
            (type == XCB_PROPERTY_NOTIFY && reinterpret_cast<xcb_property_notify_event_t *>(event)->atom == focus_time) ||
            (randr && randr->present && (type == randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
             type == randr->first_event + XCB_RANDR_NOTIFY))) publish();
        std::free(event);
    }
    stop_bar();
    if (!xcb_connection_has_error(c)) {
        xcb_grab_server(c);
        Property check{xcb_get_property_reply(c, xcb_get_property(c, false, root, net_check,
                                                                  XCB_ATOM_WINDOW, 0, 1), nullptr), &std::free};
        if (check && check->type == XCB_ATOM_WINDOW && check->format == 32 &&
            xcb_get_property_value_length(check.get()) == 4 && check->bytes_after == 0 &&
            *static_cast<const xcb_window_t *>(xcb_get_property_value(check.get())) == support) {
            xcb_delete_property(c, root, net_check);
            xcb_delete_property(c, root, net_clients);
        xcb_delete_property(c, root, net_stacking);
        xcb_delete_property(c, root, net_active);
        xcb_delete_property(c, root, net_names);
        xcb_delete_property(c, root, net_supported);
            xcb_delete_property(c, root, net_number);
            xcb_delete_property(c, root, focus_time);
        }
        const uint32_t no_events = 0;
        xcb_change_window_attributes(c, root, XCB_CW_EVENT_MASK, &no_events);
        while (!fullscreen.empty()) {
            auto [window, geometry] = fullscreen.back();
            set_fullscreen(window, false);
            if (is_fullscreen(window) != fullscreen.end()) fullscreen.pop_back();
            else xcb_configure_window(c, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH, geometry.data());
        }
        for (const auto &workspace : workspaces)
            for (auto window : workspace.windows) xcb_map_window(c, window);
        for (auto window : scratch_windows) xcb_map_window(c, window);
        xcb_ungrab_server(c);
        std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> barrier{
            xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr), &std::free};
        return barrier ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    std::fputs("mortar: X connection closed\n", stderr);
    return EXIT_FAILURE;
}
