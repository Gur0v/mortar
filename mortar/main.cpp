#include "actions.hpp"
#include "config.hpp"
#include "gaps.hpp"
#include "ipc.hpp"
#include "randr.hpp"
#include "restart.hpp"
#include "scratchpad.hpp"
#include "size-hints.hpp"
#include "workspaces.hpp"
#include "wm-selection.hpp"
#include "keyboard.hpp"

#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/randr.h>
#include <xcb/render.h>
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
#include <charconv>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

extern char **environ;

static volatile std::sig_atomic_t stopping = 0;

static void stop(int) { stopping = 1; }

static void pad_workspace_names(std::vector<std::string> &names, std::size_t count) {
    while (names.size() < count) {
        auto name = std::to_string(names.size() + 1);
        while (std::find(names.begin(), names.end(), name) != names.end()) name += "_";
        names.push_back(std::move(name));
    }
}

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

int main(int argc, char **argv) {
    std::error_code executable_error;
    auto executable = std::filesystem::canonical("/proc/self/exe", executable_error);
    std::optional<mortar::RestartSnapshot> restart_state;
    int restart_fd = -1, inherited_bar = -1, inherited_ipc = -1;
    std::uint64_t inherited_inode = 0;
    std::string inherited_path;
    std::vector<pid_t> inherited_children;
    auto parse = [](const char *text, auto &value) {
        auto end = text + std::strlen(text);
        auto result = std::from_chars(text, end, value);
        return result.ec == std::errc{} && result.ptr == end;
    };
    if (argc >= 7 && std::strcmp(argv[1], "--restart-handoff") == 0 &&
        parse(argv[2], restart_fd) && parse(argv[3], inherited_bar) &&
        parse(argv[4], inherited_ipc) && parse(argv[5], inherited_inode) && restart_fd >= 3) {
        auto is_child = [](pid_t pid) {
            siginfo_t info{};
            return pid > 0 && waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0;
        };
        if ((inherited_bar != -1 && !is_child(inherited_bar)) ||
            (inherited_ipc != -1 && (inherited_ipc < 3 || inherited_ipc == restart_fd))) {
            std::fputs("mortar: invalid restart handoff\n", stderr);
            return EXIT_FAILURE;
        }
        if (inherited_bar > 0) {
            siginfo_t info{};
            if (waitid(P_PID, static_cast<id_t>(inherited_bar), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid) {
                waitpid(inherited_bar, nullptr, WNOHANG);
                inherited_bar = -1;
            }
        }
        inherited_path = argv[6];
        for (int i = 7; i < argc; ++i) {
            pid_t child = -1;
            if (!parse(argv[i], child) || !is_child(child) || child == inherited_bar ||
                std::find(inherited_children.begin(), inherited_children.end(), child) != inherited_children.end()) {
                std::fputs("mortar: invalid inherited child\n", stderr);
                return EXIT_FAILURE;
            }
            inherited_children.push_back(child);
        }
        struct stat st{};
        try {
            const int seals = fcntl(restart_fd, F_GET_SEALS);
            const int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
            if (fstat(restart_fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                seals < 0 || (seals & required_seals) != required_seals || st.st_size <= 0 ||
                static_cast<std::uint64_t>(st.st_size) > mortar::restart_max_bytes)
                throw std::runtime_error("invalid restart descriptor");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(st.st_size));
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                auto n = pread(restart_fd, bytes.data() + offset, bytes.size() - offset, offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) throw std::runtime_error("cannot read restart snapshot");
                offset += static_cast<std::size_t>(n);
            }
            restart_state = mortar::decode_restart_snapshot(bytes);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "mortar: restart state unavailable: %s\n", error.what());
            close(restart_fd);
            if (inherited_ipc >= 0) close(inherited_ipc);
            return EXIT_FAILURE;
        }
        close(restart_fd);
    } else if (argc != 1) {
        std::fputs("mortar: invalid arguments\n", stderr);
        return EXIT_FAILURE;
    }

    int screen_number = 0;
    using Connection = std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)>;
    Connection connection{xcb_connect(nullptr, &screen_number), &xcb_disconnect};
    if (!connection || xcb_connection_has_error(connection.get())) {
        std::fputs("mortar: cannot connect to X server\n", stderr);
        return EXIT_FAILURE;
    }

    auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection.get()));
    for (int i = 0; i < screen_number && screens.rem > 0; ++i) {
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

    const auto selection = mortar::acquire_wm_selection(connection.get(), screens.data->root, screen_number);
    if (!selection.atom) {
        std::fputs("mortar: cannot acquire window manager selection\n", stderr);
        return EXIT_FAILURE;
    }

    std::unique_ptr<mortar::Ipc> ipc;
    try {
        if (inherited_ipc >= 3) {
            try {
                ipc = std::make_unique<mortar::Ipc>(mortar::Ipc::Handoff{inherited_ipc, inherited_inode, inherited_path});
                inherited_ipc = -1;
            } catch (const std::exception &error) {
                close(inherited_ipc);
                inherited_ipc = -1;
                std::fprintf(stderr, "mortar: cannot adopt IPC socket: %s\n", error.what());
            }
        }
        const char *override_path = std::getenv("MORTARSOCK");
        std::string socket_path;
        if (override_path && *override_path) socket_path = override_path;
        else {
            const char *runtime = std::getenv("XDG_RUNTIME_DIR");
            if (!runtime || !std::filesystem::path(runtime).is_absolute())
                throw std::runtime_error("XDG_RUNTIME_DIR must be an absolute private directory");
            socket_path = std::string(runtime) + "/mortar-ipc." + std::to_string(geteuid()) + "." +
                          std::to_string(getpid()) + ".sock";
        }
        try {
            if (!ipc) ipc = std::make_unique<mortar::Ipc>(socket_path);
        } catch (const std::system_error &error) {
            if (!override_path || !*override_path || error.code().value() != EEXIST) throw;
            const char *runtime = std::getenv("XDG_RUNTIME_DIR");
            if (!runtime || !std::filesystem::path(runtime).is_absolute())
                throw std::runtime_error("XDG_RUNTIME_DIR must be an absolute private directory");
            std::fprintf(stderr, "mortar: IPC path occupied, using default path\n");
            socket_path = std::string(runtime) + "/mortar-ipc." + std::to_string(geteuid()) + "." +
                          std::to_string(getpid()) + ".sock";
            ipc = std::make_unique<mortar::Ipc>(socket_path);
        }
        if (setenv("MORTARSOCK", ipc->path().c_str(), 1) < 0)
            throw std::system_error(errno, std::generic_category(), "setenv MORTARSOCK");
        std::fprintf(stderr, "mortar: IPC socket %s\n", ipc->path().c_str());
    } catch (const std::exception &error) {
        ipc.reset();
        if (inherited_ipc >= 0) close(inherited_ipc);
        unsetenv("MORTARSOCK");
        std::fprintf(stderr, "mortar: IPC unavailable: %s\n", error.what());
    }
    struct Actions {
        std::function<void(unsigned)> workspace;
        std::function<void(unsigned, const std::string &)> rename_workspace;
        std::function<void(const std::string &)> workspace_name;
        std::function<void(mortar::Config::WorkspaceNavigation)> workspace_navigation;
        std::function<void(unsigned, xcb_window_t)> move;
        std::function<void(mortar::Config::WindowAction)> output_action;
        std::function<void(mortar::Split)> split;
        std::function<void(mortar::Direction)> focus;
        std::function<void(bool)> focus_sibling;
        std::function<void(mortar::Direction)> move_direction;
        std::function<void(mortar::Direction, int)> resize;
        std::function<void(xcb_window_t)> close;
        std::function<void()> scratch_toggle, toggle_split, focus_mode_toggle, focus_parent, focus_child;
        std::function<void(xcb_window_t)> scratch_send;
        std::function<void(xcb_window_t)> toggle_fullscreen, toggle_floating;
        std::function<void(const std::vector<std::string> &)> spawn;
        bool restart = false;
        bool reload = false;
        xcb_timestamp_t time = XCB_CURRENT_TIME;
    } actions;
    mortar::Config config(&actions,
        [](void *host, unsigned digit) { static_cast<Actions *>(host)->workspace(digit); },
        [](void *host, unsigned digit, std::uint32_t window) { static_cast<Actions *>(host)->move(digit, window); },
        [](void *host, mortar::Split value) { static_cast<Actions *>(host)->split(value); },
        [](void *host, mortar::Direction value) { static_cast<Actions *>(host)->focus(value); },
        [](void *host, std::uint32_t window) { static_cast<Actions *>(host)->close(window); },
        [](void *host, const std::vector<std::string> &args) { static_cast<Actions *>(host)->spawn(args); },
        [](void *host, mortar::Direction value) { static_cast<Actions *>(host)->move_direction(value); },
        [](void *host, mortar::Direction value, int pixels) { static_cast<Actions *>(host)->resize(value, pixels); },
        [](void *host, mortar::Config::WindowAction action, std::uint32_t window) {
            auto &actions = *static_cast<Actions *>(host);
            switch (action) {
            case mortar::Config::WindowAction::toggle_split: actions.toggle_split(); break;
            case mortar::Config::WindowAction::focus_mode_toggle: actions.focus_mode_toggle(); break;
            case mortar::Config::WindowAction::focus_parent: actions.focus_parent(); break;
            case mortar::Config::WindowAction::focus_child: actions.focus_child(); break;
            case mortar::Config::WindowAction::focus_sibling_next: actions.focus_sibling(false); break;
            case mortar::Config::WindowAction::focus_sibling_previous: actions.focus_sibling(true); break;
            case mortar::Config::WindowAction::floating: actions.toggle_floating(window); break;
            case mortar::Config::WindowAction::scratch_move: actions.scratch_send(window); break;
            case mortar::Config::WindowAction::scratch_show: actions.scratch_toggle(); break;
            case mortar::Config::WindowAction::fullscreen: actions.toggle_fullscreen(window); break;
            case mortar::Config::WindowAction::exit: stopping = 1; break;
            case mortar::Config::WindowAction::restart: actions.restart = true; break;
            case mortar::Config::WindowAction::reload: actions.reload = true; break;
            case mortar::Config::WindowAction::focus_output_next:
            case mortar::Config::WindowAction::focus_output_previous:
            case mortar::Config::WindowAction::move_output_next:
            case mortar::Config::WindowAction::move_output_previous: actions.output_action(action); break;
            }
        },
        [](void *host, mortar::Config::WorkspaceNavigation action) {
            static_cast<Actions *>(host)->workspace_navigation(action);
        });
    config.workspace_names(
        [](void *host, unsigned digit, const std::string &name) { static_cast<Actions *>(host)->rename_workspace(digit, name); },
        [](void *host, const std::string &name) { static_cast<Actions *>(host)->workspace_name(name); });
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::filesystem::path config_dir;
    if (xdg && *xdg && std::filesystem::path(xdg).is_absolute()) config_dir = xdg;
    else if (home && *home && std::filesystem::path(home).is_absolute()) config_dir = std::filesystem::path(home) / ".config";
    const auto path = config_dir.empty() ? std::filesystem::path{} : config_dir / "mortar/config.scm";
    if (path.empty()) {
        std::fputs("mortar: cannot locate configuration: HOME is not an absolute path\n", stderr);
        return EXIT_FAILURE;
    }
    if (!config.load(path.c_str())) {
        std::fprintf(stderr, "mortar: failed to load configuration %s\n", path.c_str());
        return EXIT_FAILURE;
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
    auto prepare_bindings = [](const auto &custom) {
        std::vector<mortar::KeyboardBinding> bindings;
        for (const auto &binding : custom) {
            bindings.push_back({binding.keysym, binding.modifiers, binding.callback, binding.dynamic_alt});
        }
        return bindings;
    };
    try {
        keyboard->replace_bindings(prepare_bindings(config.bindings()));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "mortar: key grabs failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
    struct PointerDrag {
        xcb_window_t window;
        uint32_t workspace;
        int x, y;
        bool left, top, floating;
        uint8_t button;
        int gx, gy, width, height;
        int anchor_x, anchor_y, anchor_gx, anchor_gy, anchor_width, anchor_height;
        std::vector<uint32_t> hints;
        bool valid;
    };
    std::optional<PointerDrag> drag;
    std::vector<std::pair<uint8_t, uint16_t>> mouse_grabs;
    auto grab_mouse = [&](std::optional<uint16_t> configured) {
        std::vector<std::pair<uint8_t, uint16_t>> next, added;
        next.reserve(512);
        added.reserve(512);
        auto modifier = configured.value_or(keyboard->alt_modifier());
        for (uint16_t locks = 0; modifier && locks < 256; ++locks) {
            if (locks & ~keyboard->lock_modifiers()) continue;
            for (uint8_t button : {uint8_t{1}, uint8_t{3}}) {
                auto grab = std::pair{button, static_cast<uint16_t>(modifier | locks)};
                if (std::find(next.begin(), next.end(), grab) != next.end()) continue;
                next.push_back(grab);
                if (std::find(mouse_grabs.begin(), mouse_grabs.end(), grab) != mouse_grabs.end()) continue;
                auto cookie = xcb_grab_button_checked(c, false, root, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                    XCB_EVENT_MASK_POINTER_MOTION, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE, grab.first, grab.second);
                std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> error{xcb_request_check(c, cookie), &std::free};
                if (error) {
                    for (auto [button, modifiers] : added) xcb_ungrab_button(c, button, root, modifiers);
                    throw std::runtime_error("cannot grab mouse button");
                }
                added.push_back(grab);
            }
        }
        for (auto grab : mouse_grabs)
            if (std::find(next.begin(), next.end(), grab) == next.end()) xcb_ungrab_button(c, grab.first, root, grab.second);
        mouse_grabs = std::move(next);
    };
    grab_mouse(config.mouse_modifier());
    auto grab_focus = [&](xcb_window_t window) {
        for (uint8_t button : {uint8_t{1}, uint8_t{2}, uint8_t{3}}) {
            auto cookie = xcb_grab_button_checked(c, false, window, XCB_EVENT_MASK_BUTTON_PRESS,
                XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, button, XCB_MOD_MASK_ANY);
            std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> error{xcb_request_check(c, cookie), &std::free};
            if (error) std::fprintf(stderr, "mortar: cannot grab focus button %u on window %u: X error %u\n",
                button, window, error->error_code);
        }
    };
    std::vector<pid_t> children = std::move(inherited_children);
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
    const auto net_close = atom("_NET_CLOSE_WINDOW", 17);
    const auto net_workarea = atom("_NET_WORKAREA", 13);
    const auto output_workspaces = atom("_MORTAR_OUTPUT_WORKSPACES", 25);
    const auto net_desktop = atom("_NET_WM_DESKTOP", 15);
    const auto net_check = atom("_NET_SUPPORTING_WM_CHECK", 24);
    const auto net_name = atom("_NET_WM_NAME", 12);
    const auto utf8 = atom("UTF8_STRING", 11);
    const auto window_type = atom("_NET_WM_WINDOW_TYPE", 19);
    const auto dialog_type = atom("_NET_WM_WINDOW_TYPE_DIALOG", 26);
    const auto utility_type = atom("_NET_WM_WINDOW_TYPE_UTILITY", 27);
    const auto toolbar_type = atom("_NET_WM_WINDOW_TYPE_TOOLBAR", 27);
    const auto notification_type = atom("_NET_WM_WINDOW_TYPE_NOTIFICATION", 32);
    const auto splash_type = atom("_NET_WM_WINDOW_TYPE_SPLASH", 26);
    const auto modal_atom = atom("_NET_WM_STATE_MODAL", 19);
    const auto dock_type = atom("_NET_WM_WINDOW_TYPE_DOCK", 24);
    const auto strut = atom("_NET_WM_STRUT", 13);
    const auto strut_partial = atom("_NET_WM_STRUT_PARTIAL", 21);
    const auto local_bar = atom("_MORTAR_BAR_OUTPUT", 18);
    const auto net_state = atom("_NET_WM_STATE", 13);
    const auto fullscreen_atom = atom("_NET_WM_STATE_FULLSCREEN", 24);
    const auto attention_atom = atom("_NET_WM_STATE_DEMANDS_ATTENTION", 31);
    const auto urgent_workspaces = atom("_MORTAR_URGENT_WORKSPACES", 25);
    if (!output_workspaces || !net_clients || !net_stacking || !net_active || !net_names || !net_supported || !net_number || !net_current || !net_close || !net_workarea || !net_desktop || !net_check || !net_name || !utf8 || !window_type || !dialog_type || !utility_type || !toolbar_type || !notification_type || !splash_type || !modal_atom || !dock_type || !strut || !strut_partial || !local_bar || !net_state || !fullscreen_atom || !attention_atom || !urgent_workspaces) return EXIT_FAILURE;
    const auto persisted_workspaces = atom("_MORTAR_WORKSPACES", 18);
    if (!persisted_workspaces) return EXIT_FAILURE;
    using Property = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;
    std::vector<std::string> restored_names;
    {
        Property reply{xcb_get_property_reply(c, xcb_get_property(c, false, root, persisted_workspaces,
            persisted_workspaces, 0, (mortar::restart_max_bytes + 3) / 4), nullptr), &std::free};
        if (reply && reply->type == persisted_workspaces && reply->format == 8 && reply->bytes_after == 0) {
            try {
                const auto *data = static_cast<const std::uint8_t *>(xcb_get_property_value(reply.get()));
                auto state = mortar::decode_restart_snapshot({data, static_cast<std::size_t>(xcb_get_property_value_length(reply.get()))});
                if (!state.workspace_names.empty() && workspace_state.ensure(static_cast<uint32_t>(state.workspace_names.size() - 1)))
                    restored_names = std::move(state.workspace_names);
            } catch (const std::invalid_argument &) {
                std::fputs("mortar: ignoring invalid saved workspace state\n", stderr);
            }
        } else if (reply && reply->type != XCB_ATOM_NONE) {
            std::fputs("mortar: ignoring invalid saved workspace state\n", stderr);
        }
    }
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
    if (restart_state) {
        workspace_state = mortar::Workspaces::restore(restart_state->workspaces);
        scratchpad = mortar::Scratchpad::restore(restart_state->scratchpad);
        restored_names = restart_state->workspace_names;
        iconic = restart_state->iconic;
        for (const auto &entry : restart_state->scratchpad) scratch_windows.push_back(entry.window);
    }
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
    if (restart_state) {
        auto convert = [](const auto &entries, auto &destination) {
            for (const auto &entry : entries)
                destination.push_back({entry.window, {static_cast<uint32_t>(entry.geometry.x),
                    static_cast<uint32_t>(entry.geometry.y), static_cast<uint32_t>(entry.geometry.width),
                    static_cast<uint32_t>(entry.geometry.height), entry.border}});
        };
        convert(restart_state->floating_geometry, floating_geometry);
        convert(restart_state->fullscreen_geometry, fullscreen);
    }
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
        if (restart_state) {
            auto discovered = mortar::discover_outputs(c, root);
            outputs = mortar::Outputs::restore(restart_state->outputs, restart_state->owners,
                restart_state->focused_output, current);
            if (!outputs.update(discovered)) throw std::runtime_error("cannot restore outputs");
            current = outputs.focused_workspace();
        } else outputs.switch_workspace(current);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "mortar: output discovery failed: %s; using root geometry\n", error.what());
        if (!outputs.update({{"root", {0, 0, screens.data->width_in_pixels, screens.data->height_in_pixels}}}))
            return EXIT_FAILURE;
        outputs.switch_workspace(current);
    }
    if (outputs.owners().size() > workspaces.size() &&
        !workspace_state.ensure(static_cast<uint32_t>(outputs.owners().size() - 1))) return EXIT_FAILURE;
    if (!outputs.ensure(static_cast<uint32_t>(workspaces.size() - 1))) return EXIT_FAILURE;
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
    const auto *render = xcb_get_extension_data(c, &xcb_render_id);
    if (!render || !render->present) {
        std::fputs("mortar: X Render extension unavailable; borders may be transparent\n", stderr);
    }
    xcb_generic_error_t *render_error = nullptr;
    std::unique_ptr<xcb_render_query_pict_formats_reply_t, decltype(&std::free)> formats{
        render && render->present ? xcb_render_query_pict_formats_reply(c,
            xcb_render_query_pict_formats(c), &render_error) : nullptr, &std::free};
    std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> format_error{render_error, &std::free};
    if (render && render->present && !formats)
        std::fprintf(stderr, "mortar: cannot query X Render formats (X error %u); borders may be transparent\n",
            format_error ? format_error->error_code : 0);
    struct BorderColors {
        xcb_connection_t *connection;
        const xcb_render_query_pict_formats_reply_t *formats;
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
                uint32_t border_pixel = pixel;
                if (formats) {
                    for (auto screen = xcb_render_query_pict_formats_screens_iterator(formats); screen.rem; xcb_render_pictscreen_next(&screen))
                        for (auto depth = xcb_render_pictscreen_depths_iterator(screen.data); depth.rem; xcb_render_pictdepth_next(&depth))
                            for (auto visual = xcb_render_pictdepth_visuals_iterator(depth.data); visual.rem; xcb_render_pictvisual_next(&visual))
                                if (visual.data->visual == attributes->visual)
                                    for (auto format = xcb_render_query_pict_formats_formats_iterator(formats); format.rem; xcb_render_pictforminfo_next(&format))
                                        if (format.data->id == visual.data->format && format.data->type == XCB_RENDER_PICT_TYPE_DIRECT) {
                                            const auto shift = format.data->direct.alpha_shift;
                                            const uint32_t mask = format.data->direct.alpha_mask;
                                            if (shift < 32 && mask <= (UINT32_MAX >> shift)) border_pixel |= mask << shift;
                                            else std::fputs("mortar: invalid X Render alpha mask\n", stderr);
                                        }
                }
                std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> change_error{
                    xcb_request_check(connection, xcb_change_window_attributes_checked(connection, window,
                        XCB_CW_BORDER_PIXEL, &border_pixel)), &std::free};
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
    } border_colors{c, formats.get(), {}};
    std::function<void(std::string_view, xcb_window_t)> window_event = [](std::string_view, xcb_window_t) {};
    std::vector<xcb_window_t> urgent;
    auto set_urgent = [&](xcb_window_t window, bool value) {
        if (owner(window) < 0) return;
        if (owner(window) == static_cast<int>(current) && workspaces[current].focused == window) value = false;
        Property state{xcb_get_property_reply(c, xcb_get_property(c, false, window, net_state,
            XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
        if (!state || state->bytes_after || (state->type != XCB_ATOM_NONE &&
            (state->type != XCB_ATOM_ATOM || state->format != 32))) return;
        std::vector<xcb_atom_t> atoms;
        if (state->type == XCB_ATOM_ATOM) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(state.get()));
            auto count = xcb_get_property_value_length(state.get()) / 4;
            if (count) atoms.assign(values, values + count);
        }
        bool present = std::find(atoms.begin(), atoms.end(), attention_atom) != atoms.end();
        if (present != value) {
            std::erase(atoms, attention_atom);
            if (value) atoms.push_back(attention_atom);
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, net_state, XCB_ATOM_ATOM, 32, atoms.size(), atoms.data());
        }
        bool old = std::find(urgent.begin(), urgent.end(), window) != urgent.end();
        if (old != value) {
            if (value) urgent.push_back(window); else std::erase(urgent, window);
        }
    };
    auto hints_urgent = [&](xcb_window_t window) {
        Property hints{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_HINTS,
            XCB_ATOM_WM_HINTS, 0, 1), nullptr), &std::free};
        return hints && hints->type == XCB_ATOM_WM_HINTS && hints->format == 32 &&
            xcb_get_property_value_length(hints.get()) == 4 &&
            (*static_cast<const uint32_t *>(xcb_get_property_value(hints.get())) & (1u << 8));
    };
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
            set_urgent(window, false);
            for (const auto &output : outputs.outputs())
                for (auto managed : workspaces[output.workspace].windows)
                    border_colors.apply(managed, output.workspace == current && managed == window
                        ? config.border().focused : config.border().unfocused);
            workspaces[current].focused = window;
            auto &history = workspaces[current].focus_history;
            std::erase(history, window);
            history.push_back(window);
            if (!workspaces[current].layout.focus(window)) workspaces[current].layout.clear_selection();
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
            Property local{xcb_get_property_reply(c, xcb_get_property(c, false, window, local_bar,
                XCB_ATOM_CARDINAL, 0, 4), nullptr), &std::free};
            if (local && local->type == XCB_ATOM_CARDINAL && local->format == 32 && !local->bytes_after &&
                xcb_get_property_value_length(local.get()) == 16) {
                const auto *v = static_cast<const uint32_t *>(xcb_get_property_value(local.get()));
                if (v[0] == static_cast<uint32_t>(area.x) && v[1] == static_cast<uint32_t>(area.y) &&
                    v[2] == static_cast<uint32_t>(area.width) && v[3] == static_cast<uint32_t>(area.height)) {
                    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                        xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
                    if (geometry && geometry->x == area.x && geometry->width == area.width &&
                        geometry->height > 0 && geometry->height <= area.height && geometry->border_width == 0) {
                        if (geometry->y == area.y) top = std::max(top, int64_t(area.y) + geometry->height);
                        else if (int64_t(geometry->y) + geometry->height == int64_t(area.y) + area.height)
                            bottom = std::min(bottom, int64_t(geometry->y));
                    }
                }
            }
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
    std::optional<mortar::Rect> drag_gap_bounds;
    std::vector<std::pair<xcb_window_t, std::vector<uint32_t>>> drag_hints;
    auto tile_placements = [&](uint32_t workspace, mortar::Rect gap_bounds, bool cached) {
        for (auto placement : workspaces[workspace].layout.geometry(gap_bounds)) {
            if (is_fullscreen(placement.window) != fullscreen.end()) continue;
            placement.bounds = mortar::inner_gap(placement.bounds, gap_bounds, config.gaps().inner);
            auto found = std::find_if(drag_hints.begin(), drag_hints.end(), [&](const auto &entry) {
                return entry.first == placement.window;
            });
            if (!cached || found == drag_hints.end()) {
                Property hints{xcb_get_property_reply(c, xcb_get_property(c, false, placement.window,
                    XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 18), nullptr), &std::free};
                std::vector<uint32_t> values;
                if (hints && hints->type == XCB_ATOM_WM_SIZE_HINTS && hints->format == 32) {
                    const auto *data = static_cast<const uint32_t *>(xcb_get_property_value(hints.get()));
                    auto count = xcb_get_property_value_length(hints.get()) / 4;
                    if (count) values.assign(data, data + count);
                }
                if (found == drag_hints.end()) {
                    drag_hints.emplace_back(placement.window, std::move(values));
                    found = std::prev(drag_hints.end());
                } else found->second = std::move(values);
            }
            const uint32_t border = std::min<uint32_t>(config.border().width,
                std::max(0, std::min(placement.bounds.width, placement.bounds.height) - 1) / 2);
            uint32_t tile_width = std::max(1, placement.bounds.width - static_cast<int>(2 * border));
            uint32_t tile_height = std::max(1, placement.bounds.height - static_cast<int>(2 * border));
            if (!found->second.empty())
                std::tie(tile_width, tile_height) = hinted_size(tile_width, tile_height,
                    found->second.data(), found->second.size());
            const uint32_t values[] = {static_cast<uint32_t>(placement.bounds.x),
                                       static_cast<uint32_t>(placement.bounds.y), tile_width, tile_height, border};
            xcb_configure_window(c, placement.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                                 XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
        }
    };
    auto tile = [&] {
        drag_gap_bounds.reset();
        drag_hints.clear();
        if (drag && drag->floating) drag->valid = false;
        for (const auto &output : outputs.outputs()) {
        const auto workspace = output.workspace;
        for (auto window : workspaces[workspace].windows) {
            const auto &layout = workspaces[workspace].layout;
            const auto &selected = layout.selected_path();
            auto path = selected ? layout.window_path(window) : std::nullopt;
            const bool highlighted = selected ? path && path->size() > selected->size() &&
                std::equal(selected->begin(), selected->end(), path->begin()) : window == workspaces[workspace].focused;
            const uint32_t color = workspace == current && highlighted
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
        const auto gap_bounds = mortar::outer_gap(usable(output.bounds), config.gaps().outer);
        tile_placements(workspace, gap_bounds, false);
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
            workspaces[owner(window)].layout.clear_selection();
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
    auto restore_focus = [&](xcb_window_t target = XCB_NONE) {
        std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
        auto window = workspaces[current].focused;
        if (window && owner(window) == static_cast<int>(current)) {
            pending_focus.push_back(window);
            focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
        } else xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
        if (target && target != window && owner(target) == static_cast<int>(current)) {
            pending_focus.push_back(target);
            focus_sequence = static_cast<uint16_t>(xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, focus_time, XCB_ATOM_WINDOW, 32, 0, nullptr).sequence);
        }
        xcb_flush(c);
    };
    std::function<void(uint32_t)> workspace_event = [](uint32_t) {};
    std::function<void()> output_event = [] {};
    std::function<void(uint32_t)> settle_workspace = [](uint32_t) {};
    auto switch_workspace = [&](uint32_t destination, mortar::Outputs *prepared = nullptr) {
        if (destination >= workspaces.size() || destination == current) return;
        auto old_outputs = outputs.outputs();
        if (prepared) {
            outputs = std::move(*prepared);
        } else {
            if (destination < outputs.owners().size() && outputs.owners()[destination])
                outputs.focus_output(*outputs.owners()[destination]);
            if (!outputs.switch_workspace(destination)) return;
        }
        workspaces[current].layout.clear_selection();
        workspace_state.switch_to(outputs.focused_workspace());
        for (const auto &output : old_outputs)
            if (!visible(output.workspace))
                for (auto window : workspaces[output.workspace].windows) unmap(window);
        settle_workspace(destination);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
        tile();
        for (const auto &output : outputs.outputs())
            if (std::none_of(old_outputs.begin(), old_outputs.end(), [&](const auto &old) { return old.workspace == output.workspace; }))
                for (auto window : workspaces[output.workspace].windows) xcb_map_window(c, window);
        std::unique_ptr<xcb_query_pointer_reply_t, decltype(&std::free)> pointer{
            xcb_query_pointer_reply(c, xcb_query_pointer(c, root), nullptr), &std::free};
        restore_focus(pointer && pointer->same_screen && owner(pointer->child) == static_cast<int>(current)
            ? pointer->child : XCB_NONE);
        workspace_event(current);
        if (old_outputs != outputs.outputs()) output_event();
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
    actions.workspace_navigation = [&](mortar::Config::WorkspaceNavigation action) {
        using Navigation = mortar::Config::WorkspaceNavigation;
        if (action == Navigation::back_and_forth) {
            if (workspace_state.previous) switch_workspace(*workspace_state.previous);
            return;
        }
        std::vector<bool> visible_workspaces(workspaces.size());
        for (const auto &output : outputs.outputs()) visible_workspaces[output.workspace] = true;
        switch_workspace(workspace_state.adjacent(action == Navigation::next, visible_workspaces));
    };
    actions.output_action = [&](mortar::Config::WindowAction action) {
        using Action = mortar::Config::WindowAction;
        bool forward = action == Action::focus_output_next || action == Action::move_output_next;
        auto destination = outputs.adjacent_workspace(forward);
        if (!destination) return;
        if (action == Action::focus_output_next || action == Action::focus_output_previous)
            switch_workspace(*destination);
        else move_window(workspaces[current].focused, *destination);
    };
    actions.workspace = [&](unsigned digit) { switch_workspace((digit + 9) % 10); };
    actions.move = [&](unsigned digit, xcb_window_t window) {
        if (window && owner(window) < 0) throw std::runtime_error("unmanaged window");
        move_window(window ? window : workspaces[current].focused, (digit + 9) % 10);
    };
    actions.split = [&](mortar::Split direction) {
        auto &ws = workspaces[current];
        ws.layout.set_split(ws.tiled_focus(), direction);
        tile();
    };
    actions.toggle_split = [&] {
        auto &ws = workspaces[current];
        if (ws.layout.toggle_split(ws.tiled_focus())) tile();
    };
    actions.focus_mode_toggle = [&] {
        auto target = workspaces[current].focus_mode_target();
        if (target) {
            std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
            focus(target, actions.time);
        }
    };
    actions.focus_parent = [&] {
        auto &ws = workspaces[current];
        if (is_fullscreen(ws.focused) == fullscreen.end() && ws.layout.contains(ws.focused) &&
            ws.layout.select_parent(ws.focused)) tile();
    };
    actions.focus_child = [&] {
        auto &ws = workspaces[current];
        if (ws.layout.contains(ws.focused) && ws.layout.select_child()) tile();
    };
    actions.focus_sibling = [&](bool previous) {
        auto &ws = workspaces[current];
        auto target = ws.layout.sibling(ws.focused, previous);
        if (target) {
            std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
            focus(target, actions.time);
        }
    };
    actions.focus = [&](mortar::Direction direction) {
        auto &ws = workspaces[current];
        const bool tiled = ws.layout.contains(ws.focused);
        auto target = tiled ? ws.layout.neighbor(ws.focused, direction, false) : ws.tiled_focus();
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
            else if (tiled && (target = ws.layout.neighbor(ws.focused, direction))) {
                std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
                focus(target, actions.time);
            }
        }
    };
    actions.move_direction = [&](mortar::Direction direction) {
        auto &ws = workspaces[current];
        if (is_fullscreen(ws.focused) == fullscreen.end() && ws.layout.move(ws.focused, direction)) {
            window_event("move", ws.focused);
            tile();
        }
    };
    auto resize_window = [&](xcb_window_t target, mortar::Direction direction, int pixels) {
        auto &ws = workspaces[current];
        if (is_fullscreen(target) != fullscreen.end()) return;
        if (target != ws.focused && ws.layout.selected_path()) {
            ws.layout.clear_selection();
            tile();
        }
        if (ws.layout.contains(target)) {
            if (ws.layout.resize(target, direction, pixels, bounds())) tile();
        } else if (std::find(ws.floating.begin(), ws.floating.end(), target) != ws.floating.end()) {
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, target), nullptr), &std::free};
            if (!geometry) return;
            bool horizontal = direction == mortar::Direction::left || direction == mortar::Direction::right;
            bool previous = direction == mortar::Direction::left || direction == mortar::Direction::up;
            int size = (horizontal ? geometry->width : geometry->height) + pixels;
            if (size < 1 || size > UINT16_MAX) return;
            std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> hints{
                xcb_get_property_reply(c, xcb_get_property(c, false, target, XCB_ATOM_WM_NORMAL_HINTS,
                    XCB_ATOM_WM_SIZE_HINTS, 0, 18), nullptr), &std::free};
            uint32_t width = horizontal ? static_cast<uint32_t>(size) : geometry->width;
            uint32_t height = horizontal ? geometry->height : static_cast<uint32_t>(size);
            if (hints && hints->type == XCB_ATOM_WM_SIZE_HINTS && hints->format == 32) {
                std::tie(width, height) = floating_hinted_size(width, height,
                    static_cast<const uint32_t *>(xcb_get_property_value(hints.get())),
                    xcb_get_property_value_length(hints.get()) / 4);
            }
            int x = geometry->x, y = geometry->y;
            if (previous && horizontal) x += geometry->width - static_cast<int>(width);
            if (previous && !horizontal) y += geometry->height - static_cast<int>(height);
            if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX) return;
            uint32_t values[] = {static_cast<uint32_t>(x), static_cast<uint32_t>(y), width, height};
            xcb_configure_window(c, target, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
        }
    };
    actions.resize = [&](mortar::Direction direction, int pixels) {
        resize_window(workspaces[current].focused, direction, pixels);
    };
    actions.close = [&](xcb_window_t window) {
        if (window && owner(window) < 0) throw std::runtime_error("unmanaged window");
        if (!window) window = workspaces[current].focused;
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
        if (existing && std::find(iconic.begin(), iconic.end(), window) != iconic.end()) return;
        auto cookie = xcb_get_window_attributes(c, window);
        std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> attributes{
            xcb_get_window_attributes_reply(c, cookie, nullptr), &std::free};
        if (!attributes) return;
        if (attributes->override_redirect) {
            if (!existing) xcb_map_window(c, window);
            return;
        }
        using Property = std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;
        uint32_t desktop = cardinal(window, net_desktop, current);
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
                        !transient->bytes_after && xcb_get_property_value_length(transient.get()) == 4 &&
                        *static_cast<const xcb_window_t *>(xcb_get_property_value(transient.get())) != XCB_NONE;
        if (types && types->type == XCB_ATOM_ATOM && types->format == 32 && !types->bytes_after) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(types.get()));
            auto count = xcb_get_property_value_length(types.get()) / 4;
            for (auto type : {dialog_type, utility_type, toolbar_type, notification_type, splash_type})
                floating = floating || std::find(values, values + count, type) != values + count;
        }
        Property state{xcb_get_property_reply(c, xcb_get_property(c, false, window, net_state,
            XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
        if (state && state->type == XCB_ATOM_ATOM && state->format == 32 && !state->bytes_after) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(state.get()));
            auto count = xcb_get_property_value_length(state.get()) / 4;
            floating = floating || std::find(values, values + count, modal_atom) != values + count;
        }
        Property size_hints{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_NORMAL_HINTS,
            XCB_ATOM_WM_SIZE_HINTS, 0, 18), nullptr), &std::free};
        if (size_hints && size_hints->type == XCB_ATOM_WM_SIZE_HINTS && size_hints->format == 32 &&
            !size_hints->bytes_after && xcb_get_property_value_length(size_hints.get()) >= 9 * 4) {
            auto *values = static_cast<const uint32_t *>(xcb_get_property_value(size_hints.get()));
            floating = floating || ((values[0] & ((1u << 4) | (1u << 5))) == ((1u << 4) | (1u << 5)) &&
                values[5] > 0 && values[6] > 0 && values[5] <= INT32_MAX && values[6] <= INT32_MAX &&
                values[5] == values[7] && values[6] == values[8]);
        }
        Property window_class{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_CLASS,
            XCB_ATOM_STRING, 0, 1024), nullptr), &std::free};
        std::optional<std::string_view> class_name, instance_name, title;
        if (window_class && window_class->type == XCB_ATOM_STRING && window_class->format == 8 &&
            !window_class->bytes_after) {
            auto *value = static_cast<const char *>(xcb_get_property_value(window_class.get()));
            auto length = static_cast<size_t>(xcb_get_property_value_length(window_class.get()));
            auto *instance_end = static_cast<const char *>(std::memchr(value, 0, length));
            if (instance_end && instance_end + 1 < value + length) {
                auto *class_start = instance_end + 1;
                auto *class_end = static_cast<const char *>(std::memchr(class_start, 0, value + length - class_start));
                if (class_end && class_end + 1 == value + length) {
                    instance_name = std::string_view(value, instance_end - value);
                    class_name = std::string_view(class_start, class_end - class_start);
                }
            }
        }
        Property window_title{xcb_get_property_reply(c, xcb_get_property(c, false, window, net_name,
            utf8, 0, 1024), nullptr), &std::free};
        if (!window_title || window_title->type != utf8 || window_title->format != 8 || window_title->bytes_after)
            window_title = Property{xcb_get_property_reply(c, xcb_get_property(c, false, window, XCB_ATOM_WM_NAME,
                XCB_ATOM_STRING, 0, 1024), nullptr), &std::free};
        if (window_title && (window_title->type == utf8 || window_title->type == XCB_ATOM_STRING) &&
            window_title->format == 8 && !window_title->bytes_after)
            title = std::string_view(static_cast<const char *>(xcb_get_property_value(window_title.get())),
                                     xcb_get_property_value_length(window_title.get()));
        for (const auto &rule : config.window_rules()) {
            if (!rule.matches(class_name, instance_name, title)) continue;
            if (rule.workspace) desktop = (*rule.workspace + 9) % 10;
            if (rule.floating) floating = *rule.floating;
        }
        const uint32_t event_mask = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE |
                                    XCB_EVENT_MASK_COLOR_MAP_CHANGE;
        xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &event_mask);
        const bool was_iconic = std::find(iconic.begin(), iconic.end(), window) != iconic.end();
        std::erase(iconic, window);
        if (floating) {
            if (!workspaces[desktop].add_floating(window)) return;
            if (!existing && !was_iconic) {
                auto output = std::find_if(outputs.outputs().begin(), outputs.outputs().end(), [&](const auto &item) {
                    return outputs.owners()[desktop] && item.name == *outputs.owners()[desktop];
                });
                if (output == outputs.outputs().end())
                    output = std::find_if(outputs.outputs().begin(), outputs.outputs().end(), [&](const auto &item) {
                        return item.workspace == current;
                    });
                if (output != outputs.outputs().end()) {
                    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                        xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
                    if (geometry) {
                        auto area = usable(output->bounds);
                        const int width = geometry->width + 2 * geometry->border_width;
                        const int height = geometry->height + 2 * geometry->border_width;
                        const uint32_t values[] = {static_cast<uint32_t>(area.x + (area.width - width) / 2),
                                                   static_cast<uint32_t>(area.y + (area.height - height) / 2)};
                        xcb_configure_window(c, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
                        if (!workspace_bounds(desktop)) pending_floating_origins.emplace_back(window, output->bounds);
                    }
                }
            }
        } else {
            if (!workspaces[desktop].layout.insert_after(workspaces[desktop].tiled_focus(), mortar::Layout::leaf(window))) return;
            workspaces[desktop].windows.push_back(window);
        }
        if (std::find(clients.begin(), clients.end(), window) == clients.end()) clients.push_back(window);
        grab_focus(window);
        if (desktop != current || !workspaces[desktop].focused) workspaces[desktop].focused = window;
        set_desktop(window, desktop);
        set_state(window, 1);
        window_event("new", window);
        bool initial_urgent = hints_urgent(window);
        Property initial_state{xcb_get_property_reply(c, xcb_get_property(c, false, window, net_state,
            XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
        if (initial_state && !initial_state->bytes_after && initial_state->type == XCB_ATOM_ATOM && initial_state->format == 32) {
            auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(initial_state.get()));
            if (std::find(values, values + xcb_get_property_value_length(initial_state.get()) / 4, attention_atom) !=
                values + xcb_get_property_value_length(initial_state.get()) / 4) initial_urgent = true;
            if (std::find(values, values + xcb_get_property_value_length(initial_state.get()) / 4, fullscreen_atom) !=
                values + xcb_get_property_value_length(initial_state.get()) / 4) set_fullscreen(window, true);
        }
        set_urgent(window, initial_urgent);
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
    bool stack_pending = false, stack_dirty = false, stack_published = false;
    xcb_query_tree_cookie_t stack_cookie{};
    auto stack_retry = std::chrono::steady_clock::time_point{};
    std::vector<uint32_t> published_outputs, published_urgent, published_workareas;
    auto publish = [&] {
        std::vector<uint32_t> workareas;
        for (size_t i = 0; i < workspaces.size(); ++i) {
            auto output = std::find_if(outputs.outputs().begin(), outputs.outputs().end(), [&](const auto &item) {
                return outputs.owners()[i] && item.name == *outputs.owners()[i];
            });
            if (output == outputs.outputs().end())
                output = std::find_if(outputs.outputs().begin(), outputs.outputs().end(), [&](const auto &item) { return item.workspace == current; });
            if (output == outputs.outputs().end()) continue;
            auto area = usable(output->bounds);
            workareas.insert(workareas.end(), {static_cast<uint32_t>(area.x), static_cast<uint32_t>(area.y),
                static_cast<uint32_t>(area.width), static_cast<uint32_t>(area.height)});
        }
        if (workareas != published_workareas) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_workarea, XCB_ATOM_CARDINAL, 32, workareas.size(), workareas.data());
            published_workareas = std::move(workareas);
        }
        std::vector<uint32_t> urgent_ids;
        for (uint32_t i = 0; i < workspaces.size(); ++i)
            if (std::any_of(urgent.begin(), urgent.end(), [&](auto window) { return owner(window) == static_cast<int>(i); })) urgent_ids.push_back(i);
        if (!published || urgent_ids != published_urgent) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, urgent_workspaces, XCB_ATOM_CARDINAL, 32, urgent_ids.size(), urgent_ids.data());
            published_urgent = std::move(urgent_ids);
        }
        std::vector<uint32_t> metadata{1, static_cast<uint32_t>(outputs.outputs().size())};
        for (const auto &output : outputs.outputs()) {
            metadata.insert(metadata.end(), {static_cast<uint32_t>(output.bounds.x), static_cast<uint32_t>(output.bounds.y),
                static_cast<uint32_t>(output.bounds.width), static_cast<uint32_t>(output.bounds.height), output.workspace,
                output.workspace == current ? 1u : 0u});
            for (size_t i = 0; i < workspaces.size(); ++i)
                if (outputs.owners()[i] && *outputs.owners()[i] == output.name &&
                    (!workspaces[i].windows.empty() || i == output.workspace))
                    metadata.push_back(static_cast<uint32_t>(i));
            metadata.push_back(UINT32_MAX);
        }
        if (metadata != published_outputs) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, output_workspaces, XCB_ATOM_CARDINAL, 32, metadata.size(), metadata.data());
            published_outputs = std::move(metadata);
        }
        stack_dirty = true;
        auto active = workspaces[current].focused;
        if (!published || clients != published_clients) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_clients, XCB_ATOM_WINDOW, 32, clients.size(), clients.data());
            published_clients = clients;
        }
        if (!published || active != published_active) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_active, XCB_ATOM_WINDOW, 32, 1, &active);
            if (published && active) window_event("focus", active);
            published_active = active;
        }
        published = true;
        xcb_flush(c);
    };
    auto poll_stack = [&] {
        if (!stack_pending) {
            if (stack_dirty && std::chrono::steady_clock::now() >= stack_retry) {
                stack_dirty = false;
                stack_cookie = xcb_query_tree(c, root);
                stack_pending = true;
            }
            return;
        }
        void *raw = nullptr;
        xcb_generic_error_t *error = nullptr;
        if (!xcb_poll_for_reply(c, stack_cookie.sequence, &raw, &error)) return;
        std::unique_ptr<xcb_query_tree_reply_t, decltype(&std::free)> reply{
            static_cast<xcb_query_tree_reply_t *>(raw), &std::free};
        std::free(error);
        stack_pending = false;
        if (reply) {
            std::vector<xcb_window_t> stack;
            auto *children = xcb_query_tree_children(reply.get());
            for (int i = 0; i < xcb_query_tree_children_length(reply.get()); ++i)
                if (std::find(clients.begin(), clients.end(), children[i]) != clients.end()) stack.push_back(children[i]);
            if (!stack_published || stack != published_stack) {
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_stacking, XCB_ATOM_WINDOW, 32, stack.size(), stack.data());
                published_stack = std::move(stack);
                stack_published = true;
            }
        } else {
            std::fputs("mortar: cannot query stacking order\n", stderr);
            stack_dirty = true;
            stack_retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (stack_dirty && std::chrono::steady_clock::now() >= stack_retry) {
            stack_dirty = false;
            stack_cookie = xcb_query_tree(c, root);
            stack_pending = true;
        }
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
    const xcb_atom_t supported_atoms[] = {net_clients, net_stacking, net_active, net_names, net_number, net_current, net_close, net_workarea, net_desktop, net_check, net_name, net_state, fullscreen_atom, attention_atom, window_type, dock_type, strut, strut_partial};
    const uint32_t desktop_count = workspaces.size();
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_supported, XCB_ATOM_ATOM, 32, std::size(supported_atoms), supported_atoms);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_number, XCB_ATOM_CARDINAL, 32, 1, &desktop_count);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
    std::vector<std::string> workspace_names = restored_names.empty()
        ? std::vector<std::string>{"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"}
        : std::move(restored_names);
    pad_workspace_names(workspace_names, workspaces.size());
    auto save_names = [&](const std::vector<std::string> &names) {
        mortar::Workspaces empty;
        if (!empty.ensure(static_cast<uint32_t>(names.size() - 1)))
            throw std::runtime_error("workspace limit exceeded");
        mortar::RestartSnapshot saved{empty.snapshot(), {}};
        saved.owners.resize(names.size());
        saved.workspace_names = names;
        auto bytes = mortar::encode_restart_snapshot(saved);
        auto request = xcb_change_property_checked(c, XCB_PROP_MODE_REPLACE, root, persisted_workspaces,
            persisted_workspaces, 8, static_cast<uint32_t>(bytes.size()), bytes.data());
        std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> error{xcb_request_check(c, request), &std::free};
        if (error || xcb_connection_has_error(c)) throw std::runtime_error("cannot save workspace state");
    };
    auto publish_names = [&] {
        std::string names;
        for (const auto &name : workspace_names) { names += name; names += '\0'; }
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_names, utf8, 8, static_cast<uint32_t>(names.size()), names.data());
    };
    actions.rename_workspace = [&](unsigned digit, const std::string &name) {
        auto index = (digit + 9) % 10;
        for (std::size_t i = 0; i < workspace_names.size(); ++i)
            if (i != index && workspace_names[i] == name) throw std::runtime_error("duplicate workspace name");
        auto next_names = workspace_names;
        next_names[index] = name;
        save_names(next_names);
        workspace_names = std::move(next_names);
        publish_names();
        xcb_flush(c);
    };
    actions.workspace_name = [&](const std::string &name) {
        auto found = std::find(workspace_names.begin(), workspace_names.end(), name);
        if (found == workspace_names.end()) {
            auto index = static_cast<uint32_t>(workspace_names.size());
            if (index >= 16384) throw std::runtime_error("workspace limit reached");
            auto next_outputs = outputs;
            if (!next_outputs.switch_workspace(index)) throw std::runtime_error("cannot create workspace");
            auto next_names = workspace_names;
            next_names.push_back(name);
            std::string encoded_names;
            for (const auto &item : next_names) { encoded_names += item; encoded_names += '\0'; }
            if (!workspace_state.ensure(index)) throw std::runtime_error("workspace limit reached");
            try {
                save_names(next_names);
            } catch (...) {
                workspaces.resize(index);
                throw;
            }
            workspace_names = std::move(next_names);
            const auto count = static_cast<uint32_t>(workspaces.size());
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_number, XCB_ATOM_CARDINAL, 32, 1, &count);
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_names, utf8, 8,
                                static_cast<uint32_t>(encoded_names.size()), encoded_names.data());
            xcb_flush(c);
            try {
                switch_workspace(index, &next_outputs);
            } catch (...) {
                if (outputs.owners().size() == index) {
                    outputs = std::move(next_outputs);
                    workspace_state.switch_to(index);
                    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
                }
                throw;
            }
        } else {
            switch_workspace(static_cast<uint32_t>(found - workspace_names.begin()));
        }
    };
    publish_names();
    try {
    auto *children = xcb_query_tree_children(tree.get());
    if (restart_state) {
        auto exists = [&](xcb_window_t window) {
            return std::find(children, children + xcb_query_tree_children_length(tree.get()), window) !=
                children + xcb_query_tree_children_length(tree.get());
        };
        for (auto &ws : workspaces) {
            auto old = ws.windows;
            for (auto window : old) {
                if (!exists(window)) {
                    if (!ws.remove_floating(window)) {
                        ws.layout.remove(window);
                        std::erase(ws.windows, window);
                        std::erase(ws.focus_history, window);
                    }
                    if (ws.focused == window) ws.focused = ws.recent_focus();
                    continue;
                }
                const uint32_t mask = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_COLOR_MAP_CHANGE;
                xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &mask);
                clients.push_back(window);
                grab_focus(window);
                bool attention = hints_urgent(window);
                Property state{xcb_get_property_reply(c, xcb_get_property(c, false, window, net_state,
                    XCB_ATOM_ATOM, 0, 1024), nullptr), &std::free};
                if (state && !state->bytes_after && state->type == XCB_ATOM_ATOM && state->format == 32) {
                    auto *values = static_cast<const xcb_atom_t *>(xcb_get_property_value(state.get()));
                    attention = attention || std::find(values, values + xcb_get_property_value_length(state.get()) / 4,
                        attention_atom) != values + xcb_get_property_value_length(state.get()) / 4;
                }
                set_urgent(window, attention);
                set_desktop(window, static_cast<uint32_t>(&ws - workspaces.data()));
                if (visible(static_cast<uint32_t>(&ws - workspaces.data()))) xcb_map_window(c, window);
                else unmap(window);
            }
        }
        for (auto window : scratch_windows) {
            if (!exists(window)) scratchpad.remove(window);
            else if (std::find(clients.begin(), clients.end(), window) == clients.end()) {
                const uint32_t mask = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_COLOR_MAP_CHANGE;
                xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &mask);
                clients.push_back(window);
                grab_focus(window);
            }
        }
        std::erase_if(scratch_windows, [&](auto window) { return !exists(window); });
        std::erase_if(iconic, [&](auto window) { return !exists(window); });
        std::erase_if(fullscreen, [&](const auto &entry) { return !exists(entry.first); });
        std::erase_if(floating_geometry, [&](const auto &entry) { return !exists(entry.first); });
        for (auto window : iconic) {
            const uint32_t mask = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_COLOR_MAP_CHANGE;
            xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &mask);
            clients.push_back(window);
        }
    }
    for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
        manage(children[i], true);
    }

    std::unique_ptr<xcb_query_pointer_reply_t, decltype(&std::free)> pointer{
        xcb_query_pointer_reply(c, xcb_query_pointer(c, root), nullptr), &std::free};
    if (restart_state) {
        tile();
        restore_focus();
    } else if (pointer && owner(pointer->child) == static_cast<int>(current)) {
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
        xcb_delete_property(c, root, output_workspaces);
        xcb_delete_property(c, root, urgent_workspaces);
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

    std::vector<int> subscribers, window_subscribers, output_subscribers;
    if (ipc) ipc->on_close = [&](int client) {
        std::erase(subscribers, client);
        std::erase(window_subscribers, client);
        std::erase(output_subscribers, client);
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
    output_event = [&] {
        if (!ipc) return;
        auto targets = output_subscribers;
        for (int client : targets) ipc->send(client, "event output unspecified");
    };
    pid_t bar = inherited_bar;
    auto start_bar = [&] {
        posix_spawn_file_actions_t bar_actions;
        int bar_error = posix_spawn_file_actions_init(&bar_actions);
        if (!bar_error) {
            bar_error = posix_spawn_file_actions_addopen(&bar_actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            if (!bar_error) bar_error = posix_spawn_file_actions_addopen(&bar_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
            if (!bar_error) {
                char name[] = "mortarbar";
                const auto &settings = config.bar();
                std::vector<std::string> options{"--mortar-settings", settings.bottom ? "bottom" : "top", settings.font,
                    std::to_string(settings.background), std::to_string(settings.foreground),
                    std::to_string(settings.focused), std::to_string(settings.unfocused)};
                if (settings.workspace_colors) {
                    options.emplace_back("--workspace-colors");
                    for (auto color : *settings.workspace_colors) options.push_back(std::to_string(color));
                }
                if (settings.height) options.insert(options.end(), {"--height", std::to_string(settings.height)});
                options.insert(options.end(), {"--status-padding", std::to_string(settings.status_padding), "--status-edge-padding", std::to_string(settings.status_edge_padding)});
                if (settings.hidden) options.emplace_back("--hidden");
                if (!settings.workspace_buttons) options.emplace_back("--no-workspace-buttons");
                if (settings.wrap_scroll) options.emplace_back("--wrap-scroll");
                for (const auto &output : settings.outputs) {
                    options.emplace_back("--output");
                    options.push_back(output);
                }
                options.emplace_back("--");
                std::vector<char *> args{name};
                for (auto &arg : options) args.push_back(arg.data());
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
    if ((!restart_state || stop_bar()) && bar <= 0) start_bar();
    actions.scratch_send = [&](xcb_window_t window) {
        if (window && owner(window) < 0) throw std::runtime_error("unmanaged window");
        if (!window) window = workspaces[current].focused;
        if (!window) return;
        auto source = static_cast<unsigned>(owner(window));
        auto &ws = workspaces[source];
        bool was_focused = ws.focused == window;
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
                if (scratchpad.add(window, source, saved)) {
                    scratch_windows.push_back(window);
                    if (!ws.remove_floating(window)) {
                        ws.layout.remove(window);
                        std::erase(ws.windows, window);
                        std::erase(ws.focus_history, window);
                    }
                    if (ws.focused == window) ws.focused = ws.recent_focus();
                    unmap(window);
                    tile();
                    if (source == current && was_focused) restore_focus();
                }
            }
        } else if (window && scratchpad.get(window)) {
            set_fullscreen(window, false);
            auto result = is_fullscreen(window) == fullscreen.end() ? scratchpad.toggle(source, window, window) : std::nullopt;
            if (result && result->action == mortar::ScratchpadAction::hide) {
                ws.remove_floating(window);
                unmap(window);
                tile();
                if (source == current && was_focused) restore_focus();
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
    actions.toggle_fullscreen = [&](xcb_window_t window) {
        if (window && owner(window) < 0) throw std::runtime_error("unmanaged window");
        if (!window) window = workspaces[current].focused;
        set_fullscreen(window, is_fullscreen(window) == fullscreen.end());
    };
    actions.toggle_floating = [&](xcb_window_t window) {
        if (window && owner(window) < 0) throw std::runtime_error("unmanaged window");
        if (!window) window = workspaces[current].focused;
        if (!window || is_fullscreen(window) != fullscreen.end()) return;
        auto &ws = workspaces[owner(window)];
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
                auto output = std::find_if(outputs.outputs().begin(), outputs.outputs().end(),
                    [&](const auto &item) { return item.workspace == static_cast<unsigned>(owner(window)); });
                auto area = output == outputs.outputs().end() ? bounds() : output->bounds;
                const std::array<uint32_t, 5> centered{static_cast<uint32_t>(area.x + area.width / 4), static_cast<uint32_t>(area.y + area.height / 4),
                    static_cast<uint32_t>(std::max(1, area.width / 2)), static_cast<uint32_t>(std::max(1, area.height / 2)), 0};
                xcb_configure_window(c, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH,
                    saved != floating_geometry.end() ? saved->second.data() : centered.data());
            }
            tile();
        }
    };

    auto restart = [&] {
        int fd = -1, ipc_fd = -1, xflags = -1;
        try {
            if (executable_error) throw std::runtime_error("cannot resolve executable path");
            if (outputs.outputs().empty()) throw std::runtime_error("no outputs to restore");
            mortar::RestartSnapshot state{workspace_state.snapshot(), scratchpad.snapshot()};
            state.outputs = outputs.outputs();
            state.owners = outputs.owners();
            state.focused_output = outputs.focused_output();
            state.workspace_names = workspace_names;
            state.iconic = iconic;
            for (auto window : scratch_windows) {
                auto entry = scratchpad.get(window);
                if (entry && entry->visible_workspace) {
                    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                        xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr), &std::free};
                    auto previous = is_fullscreen(window);
                    for (auto &saved : state.scratchpad) if (saved.window == window) {
                        if (previous != fullscreen.end()) {
                            const auto &g = previous->second;
                            saved.geometry = mortar::Rect{static_cast<int32_t>(g[0]), static_cast<int32_t>(g[1]),
                                static_cast<int>(g[2]), static_cast<int>(g[3])};
                        } else if (geometry) {
                            saved.geometry = mortar::Rect{geometry->x, geometry->y, geometry->width, geometry->height};
                        }
                    }
                }
            }
            auto convert = [](const auto &source, auto &destination) {
                for (const auto &[window, g] : source)
                    destination.push_back({window, {static_cast<int32_t>(g[0]), static_cast<int32_t>(g[1]),
                        static_cast<int>(g[2]), static_cast<int>(g[3])}, g[4]});
            };
            convert(floating_geometry, state.floating_geometry);
            convert(fullscreen, state.fullscreen_geometry);
            auto bytes = mortar::encode_restart_snapshot(state);
            fd = static_cast<int>(syscall(SYS_memfd_create, "mortar-restart", MFD_ALLOW_SEALING));
            if (fd < 0) throw std::system_error(errno, std::generic_category(), "memfd_create");
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                auto n = write(fd, bytes.data() + offset, bytes.size() - offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) throw std::runtime_error("cannot write restart snapshot");
                offset += static_cast<std::size_t>(n);
            }
            if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
                throw std::system_error(errno, std::generic_category(), "seal restart snapshot");
            mortar::Ipc::Handoff handoff{-1, 0, ""};
            if (ipc) { handoff = ipc->handoff(); ipc_fd = handoff.fd; }
            auto fd_arg = std::to_string(fd), bar_arg = std::to_string(bar);
            auto ipc_arg = std::to_string(handoff.fd), inode_arg = std::to_string(handoff.inode);
            xflags = fcntl(xcb_get_file_descriptor(c), F_GETFD);
            if (xflags < 0 || fcntl(xcb_get_file_descriptor(c), F_SETFD, xflags | FD_CLOEXEC) < 0)
                throw std::system_error(errno, std::generic_category(), "protect X descriptor");
            std::vector<std::string> args{"mortar", "--restart-handoff", fd_arg, bar_arg, ipc_arg, inode_arg, handoff.path};
            for (auto child : children) args.push_back(std::to_string(child));
            std::vector<char *> pointers;
            for (auto &arg : args) pointers.push_back(arg.data());
            pointers.push_back(nullptr);
            execv(executable.c_str(), pointers.data());
            throw std::system_error(errno, std::generic_category(), "exec restart");
        } catch (const std::exception &error) {
            if (xflags >= 0 && fcntl(xcb_get_file_descriptor(c), F_SETFD, xflags) < 0)
                std::fprintf(stderr, "mortar: cannot restore X descriptor flags: %s\n", std::strerror(errno));
            if (fd >= 0) close(fd);
            if (ipc_fd >= 0) close(ipc_fd);
            std::fprintf(stderr, "mortar: restart failed: %s\n", error.what());
        }
    };
    auto reload = [&] {
        if (!config.stage(path.c_str())) {
            std::fprintf(stderr, "mortar: failed to reload configuration %s; keeping current configuration\n", path.c_str());
            return;
        }
        try {
            auto old_status = config.bar_status();
            auto old_bar = config.bar();
            auto bindings = prepare_bindings(config.staged_bindings());
            xcb_grab_server(c);
            try {
                grab_mouse(config.staged_mouse_modifier());
                try {
                    keyboard->replace_bindings(std::move(bindings));
                } catch (...) {
                    grab_mouse(config.mouse_modifier());
                    throw;
                }
            } catch (...) {
                xcb_ungrab_server(c);
                throw;
            }
            xcb_ungrab_server(c);
            xcb_flush(c);
            config.commit();
            try {
                tile();
                if ((old_status != config.bar_status() || old_bar != config.bar()) && stop_bar()) start_bar();
            } catch (const std::exception &error) {
                std::fprintf(stderr, "mortar: configuration reloaded, but applying changes failed: %s\n", error.what());
            }
        } catch (const std::exception &error) {
            config.discard();
            std::fprintf(stderr, "mortar: failed to reload keybindings: %s; keeping current configuration\n", error.what());
        }
    };
    std::unique_ptr<xcb_generic_event_t, decltype(&std::free)> deferred_event{nullptr, &std::free};
    while (!stopping) {
        if (actions.reload) { actions.reload = false; reload(); }
        if (actions.restart) { actions.restart = false; restart(); }
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
        auto *event = deferred_event ? deferred_event.release() : xcb_poll_for_event(c);
        if (event && drag && (event->response_type & 0x7f) == XCB_MOTION_NOTIFY) {
            for (int count = 0; count < 64; ++count) {
                auto *next = xcb_poll_for_queued_event(c);
                if (!next) break;
                if ((next->response_type & 0x7f) != XCB_MOTION_NOTIFY) {
                    deferred_event.reset(next);
                    break;
                }
                auto *previous = reinterpret_cast<xcb_motion_notify_event_t *>(event);
                auto *motion = reinterpret_cast<xcb_motion_notify_event_t *>(next);
                int px = previous->root_x - drag->x, py = previous->root_y - drag->y;
                int nx = motion->root_x - previous->root_x, ny = motion->root_y - previous->root_y;
                if ((px && nx && (px < 0) != (nx < 0)) || (py && ny && (py < 0) != (ny < 0))) {
                    deferred_event.reset(next);
                    break;
                }
                std::free(event);
                event = next;
            }
        }
        if (xcb_connection_has_error(c)) { std::free(event); break; }
        try {
            poll_stack();
        } catch (const std::bad_alloc &) {
            std::fputs("mortar: cannot allocate stacking order\n", stderr);
            stack_dirty = true;
            stack_retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (!event) event = xcb_poll_for_queued_event(c);
        auto fds = ipc ? ipc->poll_fds() : std::vector<pollfd>{};
        fds.push_back({xcb_get_file_descriptor(c), POLLIN, 0});
        xcb_flush(c);
        int ready = poll(fds.data(), fds.size(), event ? 0 : 100);
        if (ready < 0 && errno != EINTR) {
            std::fprintf(stderr, "mortar: poll failed: %s\n", std::strerror(errno));
            std::free(event);
            break;
        }
        bool ipc_dispatched = false;
        if (ipc && ready >= 0) {
            fds.pop_back();
            if (drag && std::any_of(fds.begin(), fds.end(), [](const auto &fd) { return fd.revents != 0; })) {
                drag->valid = false;
                drag_gap_bounds.reset();
            }
            try {
                ipc->dispatch(fds, [&](int client, std::string_view frame) {
                    ipc_dispatched = true;
                    if (frame == "subscribe workspace") {
                        if (std::find(subscribers.begin(), subscribers.end(), client) == subscribers.end())
                            subscribers.push_back(client);
                        ipc->subscribe(client);
                        ipc->send(client, "ok subscribed");
                    } else if (frame == "subscribe window") {
                        if (std::find(window_subscribers.begin(), window_subscribers.end(), client) == window_subscribers.end())
                            window_subscribers.push_back(client);
                        ipc->subscribe(client);
                        ipc->send(client, "ok subscribed");
                    } else if (frame == "subscribe output") {
                        if (std::find(output_subscribers.begin(), output_subscribers.end(), client) == output_subscribers.end())
                            output_subscribers.push_back(client);
                        ipc->subscribe(client);
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
                unsetenv("MORTARSOCK");
            }
        }
        if (!event) { if (ipc_dispatched) publish(); continue; }
        if ((event->response_type & 0x80) == 0 && (event->response_type & 0x7f) == XCB_SELECTION_CLEAR) {
            auto *clear = reinterpret_cast<xcb_selection_clear_event_t *>(event);
            if (clear->owner == selection.window && clear->selection == selection.atom) {
                std::fputs("mortar: window manager selection lost\n", stderr);
                std::free(event);
                break;
            }
        }
        try {
            if (drag && (event->response_type & 0x7f) == XCB_KEY_PRESS) drag->valid = false;
            if ((event->response_type & 0x7f) == XCB_KEY_PRESS)
                actions.time = reinterpret_cast<xcb_key_press_event_t *>(event)->time;
            auto old_alt = keyboard->alt_modifier();
            auto old_locks = keyboard->lock_modifiers();
            keyboard->process(*event);
            if (old_alt != keyboard->alt_modifier() || old_locks != keyboard->lock_modifiers()) grab_mouse(config.mouse_modifier());
            if (auto id = keyboard->callback(); id && config.run_binding(*id) != mortar::Config::Result::success) {
                std::fprintf(stderr, "mortar: key binding %u failed\n", *id);
            }
        } catch (const std::exception &error) {
            std::fprintf(stderr, "mortar: keyboard update failed: %s\n", error.what());
        }
        const auto type = event->response_type & 0x7f;
        if (randr && randr->present && (type == randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
            type == randr->first_event + XCB_RANDR_NOTIFY)) {
            if (drag) drag->valid = false;
            try {
                auto old_outputs = outputs.outputs();
                auto discovered = mortar::discover_outputs(c, root);
                auto next_outputs = outputs;
                if (!next_outputs.update(discovered) || next_outputs.outputs().empty())
                    throw std::runtime_error("invalid output configuration");
                auto old_owners = outputs.owners();
                if (next_outputs.owners().size() > workspaces.size()) {
                    auto names = workspace_names;
                    pad_workspace_names(names, next_outputs.owners().size());
                    const auto previous_size = workspaces.size();
                    if (!workspace_state.ensure(static_cast<uint32_t>(next_outputs.owners().size() - 1)))
                        throw std::runtime_error("workspace limit reached");
                    try {
                        save_names(names);
                    } catch (...) {
                        workspaces.resize(previous_size);
                        throw;
                    }
                    workspace_names = std::move(names);
                    const auto count = static_cast<uint32_t>(workspaces.size());
                    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_number, XCB_ATOM_CARDINAL, 32, 1, &count);
                    publish_names();
                }
                outputs = std::move(next_outputs);
                for (size_t workspace = 0; workspace < workspaces.size(); ++workspace) {
                    if (workspace >= old_owners.size() || !old_owners[workspace] || !outputs.owners()[workspace]) continue;
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
                workspaces[current].layout.clear_selection();
        workspace_state.switch_to(outputs.focused_workspace());
                for (const auto &output : outputs.outputs()) settle_workspace(output.workspace);
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, net_current, XCB_ATOM_CARDINAL, 32, 1, &current);
                tile();
                restore_focus();
                if (current != previous_workspace) workspace_event(current);
                if (old_outputs != outputs.outputs()) output_event();
            } catch (const std::exception &error) {
                std::fprintf(stderr, "mortar: output update failed: %s; keeping previous outputs\n", error.what());
            }
        }
        switch (type) {
        case XCB_BUTTON_PRESS: {
            auto *press = reinterpret_cast<xcb_button_press_event_t *>(event);
            if (press->event != root) {
                struct ReplayPointer {
                    xcb_connection_t *connection;
                    xcb_timestamp_t time;
                    ~ReplayPointer() {
                        xcb_allow_events(connection, XCB_ALLOW_REPLAY_POINTER, time);
                        xcb_flush(connection);
                    }
                } replay{c, press->time};
                int workspace = owner(press->event);
                if (workspace >= 0 && visible(workspace)) {
                    if (static_cast<uint32_t>(workspace) != current) switch_workspace(workspace);
                    focus(press->event, press->time);
                }
                break;
            }
            int workspace = owner(press->child);
            if (workspace < 0 || !visible(workspace) || is_fullscreen(press->child) != fullscreen.end()) break;
            auto &ws = workspaces[workspace];
            bool floating = std::find(ws.floating.begin(), ws.floating.end(), press->child) != ws.floating.end();
            if ((!floating && !ws.layout.contains(press->child)) || (press->detail == 1 && !floating)) break;
            std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                xcb_get_geometry_reply(c, xcb_get_geometry(c, press->child), nullptr), &std::free};
            if (!geometry) break;
            if (static_cast<uint32_t>(workspace) != current) switch_workspace(workspace);
            focus(press->child, press->time);
            drag_gap_bounds.reset();
            drag = PointerDrag{press->child, static_cast<uint32_t>(workspace), press->root_x, press->root_y,
                press->root_x <= geometry->x + geometry->width / 2,
                press->root_y <= geometry->y + geometry->height / 2, floating, press->detail,
                geometry->x, geometry->y, geometry->width, geometry->height,
                press->root_x, press->root_y, geometry->x, geometry->y, geometry->width, geometry->height, {}, press->detail == 1};
            break;
        }
        case XCB_MOTION_NOTIFY: {
            if (!drag) break;
            auto &ws = workspaces[drag->workspace];
            bool floating = std::find(ws.floating.begin(), ws.floating.end(), drag->window) != ws.floating.end();
            if (owner(drag->window) != static_cast<int>(drag->workspace) || current != drag->workspace ||
                is_fullscreen(drag->window) != fullscreen.end() || floating != drag->floating ||
                (!floating && !ws.layout.contains(drag->window))) {
                drag.reset();
                xcb_ungrab_pointer(c, XCB_CURRENT_TIME);
                break;
            }
            auto *motion = reinterpret_cast<xcb_motion_notify_event_t *>(event);
            int dx = motion->root_x - drag->x, dy = motion->root_y - drag->y;
            if (drag->floating) {
                if (!drag->valid) {
                    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry{
                        xcb_get_geometry_reply(c, xcb_get_geometry(c, drag->window), nullptr), &std::free};
                    if (!geometry) { drag.reset(); xcb_ungrab_pointer(c, XCB_CURRENT_TIME); break; }
                    drag->gx = geometry->x; drag->gy = geometry->y;
                    drag->width = geometry->width; drag->height = geometry->height;
                    drag->anchor_x = drag->x; drag->anchor_y = drag->y;
                    drag->anchor_gx = drag->gx; drag->anchor_gy = drag->gy;
                    drag->anchor_width = drag->width; drag->anchor_height = drag->height;
                    drag->hints.clear();
                    if (drag->button != 1) {
                        std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> hints{
                            xcb_get_property_reply(c, xcb_get_property(c, false, drag->window, XCB_ATOM_WM_NORMAL_HINTS,
                                XCB_ATOM_WM_SIZE_HINTS, 0, 18), nullptr), &std::free};
                        if (hints && hints->type == XCB_ATOM_WM_SIZE_HINTS && hints->format == 32) {
                            auto *values = static_cast<const uint32_t *>(xcb_get_property_value(hints.get()));
                            drag->hints.assign(values, values + xcb_get_property_value_length(hints.get()) / 4);
                        }
                    }
                    drag->valid = true;
                }
                dx = motion->root_x - drag->anchor_x; dy = motion->root_y - drag->anchor_y;
                drag->gx = drag->anchor_gx; drag->gy = drag->anchor_gy;
                drag->width = drag->anchor_width; drag->height = drag->anchor_height;
                if (drag->button == 1) {
                    int x = drag->gx + dx, y = drag->gy + dy;
                    drag->gx = std::clamp(x, int(INT16_MIN), int(INT16_MAX));
                    drag->gy = std::clamp(y, int(INT16_MIN), int(INT16_MAX));
                    if (x != drag->gx) { drag->anchor_x = motion->root_x; drag->anchor_gx = drag->gx; }
                    if (y != drag->gy) { drag->anchor_y = motion->root_y; drag->anchor_gy = drag->gy; }
                    uint32_t values[] = {static_cast<uint32_t>(drag->gx), static_cast<uint32_t>(drag->gy)};
                    xcb_configure_window(c, drag->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
                } else {
                    workspaces[current].layout.clear_selection();
                    int requested_width = drag->width + dx * (drag->left ? -1 : 1);
                    int requested_height = drag->height + dy * (drag->top ? -1 : 1);
                    int min_width = 1, min_height = 1, max_width = UINT16_MAX, max_height = UINT16_MAX;
                    if (drag->hints.size() >= 15) {
                        auto flags = drag->hints[0];
                        auto bound = [&](size_t index, uint32_t flag, int fallback) {
                            auto value = drag->hints[index];
                            return (flags & flag) && value > 0 && value <= INT32_MAX
                                ? static_cast<int>(std::min(value, uint32_t{UINT16_MAX})) : fallback;
                        };
                        min_width = bound(5, 16, 1); min_height = bound(6, 16, 1);
                        max_width = std::max(min_width, bound(7, 32, UINT16_MAX));
                        max_height = std::max(min_height, bound(8, 32, UINT16_MAX));
                    }
                    uint32_t width = std::clamp(requested_width, min_width, max_width);
                    uint32_t height = std::clamp(requested_height, min_height, max_height);
                    if (drag->hints.size() >= 15 && (drag->hints[0] & 128)) {
                        auto &h = drag->hints;
                        if (h[11] && h[12] && h[13] && h[14] && h[11] <= INT32_MAX && h[12] <= INT32_MAX &&
                            h[13] <= INT32_MAX && h[14] <= INT32_MAX && uint64_t{h[11]} * h[14] <= uint64_t{h[13]} * h[12]) {
                            if (dx && !dy) {
                                uint64_t low = (uint64_t{width} * h[14] + h[13] - 1) / h[13];
                                uint64_t high = uint64_t{width} * h[12] / h[11];
                                if (low <= high) height = static_cast<uint32_t>(std::min<uint64_t>(UINT16_MAX, std::clamp<uint64_t>(height, low, high)));
                            } else if (dy && !dx) {
                                uint64_t low = (uint64_t{height} * h[11] + h[12] - 1) / h[12];
                                uint64_t high = uint64_t{height} * h[13] / h[14];
                                if (low <= high) width = static_cast<uint32_t>(std::min<uint64_t>(UINT16_MAX, std::clamp<uint64_t>(width, low, high)));
                            }
                        }
                    }
                    if (!drag->hints.empty())
                        std::tie(width, height) = floating_hinted_size(width, height, drag->hints.data(), drag->hints.size());
                    int x = drag->gx + (drag->left ? drag->width - static_cast<int>(width) : 0);
                    int y = drag->gy + (drag->top ? drag->height - static_cast<int>(height) : 0);
                    if (x >= INT16_MIN && x <= INT16_MAX && y >= INT16_MIN && y <= INT16_MAX) {
                        drag->gx = x; drag->gy = y; drag->width = width; drag->height = height;
                    }
                    if (requested_width < min_width || requested_width > max_width || x < INT16_MIN || x > INT16_MAX) {
                        drag->anchor_x = motion->root_x; drag->anchor_gx = drag->gx; drag->anchor_width = drag->width;
                    }
                    if (requested_height < min_height || requested_height > max_height || y < INT16_MIN || y > INT16_MAX) {
                        drag->anchor_y = motion->root_y; drag->anchor_gy = drag->gy; drag->anchor_height = drag->height;
                    }
                    uint32_t values[] = {static_cast<uint32_t>(drag->gx), static_cast<uint32_t>(drag->gy),
                        static_cast<uint32_t>(drag->width), static_cast<uint32_t>(drag->height)};
                    xcb_configure_window(c, drag->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
                    drag->x = motion->root_x;
                    drag->y = motion->root_y;
                    break;
                }
            } else {
                auto &layout = workspaces[drag->workspace].layout;
                if (!drag_gap_bounds) {
                    drag_hints.clear();
                    drag_gap_bounds = mortar::outer_gap(usable(bounds()), config.gaps().outer);
                }
                bool horizontal = layout.resize(drag->window, drag->left ? mortar::Direction::left : mortar::Direction::right,
                                                drag->left ? -dx : dx, *drag_gap_bounds, true);
                bool vertical = layout.resize(drag->window, drag->top ? mortar::Direction::up : mortar::Direction::down,
                                              drag->top ? -dy : dy, *drag_gap_bounds, true);
                drag->x = motion->root_x;
                drag->y = motion->root_y;
                if (horizontal || vertical) {
                    tile_placements(drag->workspace, *drag_gap_bounds, true);
                    xcb_flush(c);
                }
                break;
            }
            drag->x = motion->root_x;
            drag->y = motion->root_y;
            break;
        }
        case XCB_BUTTON_RELEASE: {
            auto *release = reinterpret_cast<xcb_button_release_event_t *>(event);
            if (drag && drag->button == release->detail) drag.reset();
            break;
        }
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
                if (enter->event != workspaces[current].focused && enter->time != XCB_CURRENT_TIME &&
                    (pending_focus.empty() || static_cast<int16_t>(enter->sequence - focus_sequence) >= 0)) {
                    std::fill(pending_focus.begin(), pending_focus.end(), XCB_NONE);
                    focus(enter->event, enter->time);
                }
            }
            break;
        }
        case XCB_PROPERTY_NOTIFY: {
            auto *property = reinterpret_cast<xcb_property_notify_event_t *>(event);
            if (drag && property->atom == XCB_ATOM_WM_NORMAL_HINTS) {
                drag_gap_bounds.reset();
                if (property->window == drag->window) drag->valid = false;
            }
            if (!(event->response_type & 0x80) && (property->atom == strut || property->atom == strut_partial || property->atom == local_bar) &&
                std::find(docks.begin(), docks.end(), property->window) != docks.end()) tile();
            if (!(event->response_type & 0x80) && property->atom == XCB_ATOM_WM_HINTS && owner(property->window) >= 0)
                set_urgent(property->window, hints_urgent(property->window));
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
            } else if (message->format == 32 && message->type == net_active && owner(message->window) >= 0) {
                set_urgent(message->window, true);
            } else if (message->format == 32 && message->type == net_close && owner(message->window) >= 0) {
                if (mortar::close_window(c, message->window, protocols, delete_window, message->data.data32[0]) != mortar::CloseResult::sent)
                    std::fprintf(stderr, "mortar: cannot close window %#x\n", message->window);
            } else if (message->format == 32 && message->type == net_desktop &&
                       message->data.data32[0] < workspaces.size()) {
                move_window(message->window, message->data.data32[0]);
            } else if (message->format == 32 && message->type == net_state && message->data.data32[0] <= 2) {
                if (message->data.data32[1] == fullscreen_atom || message->data.data32[2] == fullscreen_atom) {
                    bool active = is_fullscreen(message->window) != fullscreen.end();
                    set_fullscreen(message->window, message->data.data32[0] == 2 ? !active : message->data.data32[0] == 1);
                }
                if (message->data.data32[1] == attention_atom || message->data.data32[2] == attention_atom) {
                    bool active = std::find(urgent.begin(), urgent.end(), message->window) != urgent.end();
                    set_urgent(message->window, message->data.data32[0] == 2 ? !active : message->data.data32[0] == 1);
                }
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
                    std::erase(workspaces[workspace].focus_history, message->window);
                }
                if (workspaces[workspace].focused == message->window)
                    workspaces[workspace].focused = workspaces[workspace].recent_focus();
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
            if (drag && drag->window == window) {
                drag.reset();
                xcb_ungrab_pointer(c, XCB_CURRENT_TIME);
            }
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
                    std::erase(ws.focus_history, window);
                }
                if (ws.focused == window) ws.focused = ws.recent_focus();
                xcb_delete_property(c, window, net_desktop);
            }
            auto removed = static_cast<size_t>(was_tiled || scratch_removed) + std::erase(iconic, window);
            if (removed) {
                if (type == XCB_UNMAP_NOTIFY)
                    xcb_ungrab_button(c, XCB_BUTTON_INDEX_ANY, window, XCB_MOD_MASK_ANY);
                std::erase(urgent, window);
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
            if (drag && request->window == drag->window) drag->valid = false;
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
        if (ipc_dispatched || type == XCB_KEY_PRESS || type == XCB_BUTTON_PRESS || type == XCB_ENTER_NOTIFY ||
            type == XCB_MAP_REQUEST || type == XCB_UNMAP_NOTIFY || type == XCB_DESTROY_NOTIFY ||
            type == XCB_CLIENT_MESSAGE || type == XCB_CONFIGURE_REQUEST || type == XCB_CONFIGURE_NOTIFY ||
            (type == XCB_PROPERTY_NOTIFY && (reinterpret_cast<xcb_property_notify_event_t *>(event)->atom == focus_time || reinterpret_cast<xcb_property_notify_event_t *>(event)->atom == XCB_ATOM_WM_HINTS)) ||
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
            try {
                save_names(workspace_names);
            } catch (const std::exception &error) {
                std::fprintf(stderr, "mortar: cannot save workspace state: %s\n", error.what());
            }
            xcb_delete_property(c, root, net_check);
            xcb_delete_property(c, root, net_clients);
        xcb_delete_property(c, root, net_stacking);
        xcb_delete_property(c, root, net_active);
        xcb_delete_property(c, root, net_names);
        xcb_delete_property(c, root, output_workspaces);
        xcb_delete_property(c, root, urgent_workspaces);
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
