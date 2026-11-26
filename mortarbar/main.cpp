#include <tuple>
#include "status.hpp"
#include "../mortar/randr.hpp"

#include <xcb/randr.h>
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define explicit explicit_field
#include <xcb/xkb.h>
#undef explicit
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <xcb/xcb.h>
#include <cairo/cairo-xcb.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <csignal>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

xcb_atom_t atom(xcb_connection_t* c, const char* name) {
    auto cookie = xcb_intern_atom(c, 0, static_cast<std::uint16_t>(std::char_traits<char>::length(name)), name);
    auto* reply = xcb_intern_atom_reply(c, cookie, nullptr);
    if (!reply) return XCB_ATOM_NONE;
    auto result = reply->atom;
    free(reply);
    return result;
}

std::vector<std::uint8_t> property(xcb_connection_t* c, xcb_window_t window, xcb_atom_t name, xcb_atom_t type, std::uint8_t format) {
    auto cookie = xcb_get_property(c, 0, window, name, type, 0, 4096);
    auto* reply = xcb_get_property_reply(c, cookie, nullptr);
    if (!reply) return {};
    std::vector<std::uint8_t> result;
    if (reply->type == type && reply->format == format && !reply->bytes_after) {
        auto* data = static_cast<const std::uint8_t*>(xcb_get_property_value(reply));
        int size = xcb_get_property_value_length(reply);
        if (size > 0) result.assign(data, data + size);
    }
    free(reply);
    return result;
}
}

int main(int argc, char **argv) {
    std::uint16_t height = 0;
    int status_padding = 1;
    int status_edge_padding = 6;
    bool bottom = true;
    bool hidden = false;
    bool workspace_buttons = true;
    bool wrap_scroll = false;
    std::string font_name = "monospace 10";
    std::array<std::uint32_t, 4> colors{0x000000, 0xffffff, 0x285577, 0x222222};
    std::optional<std::array<std::uint32_t, 8>> workspace_colors;
    std::vector<std::string_view> selected_outputs;
    int command = 1;
    if (argc > 1 && std::string_view(argv[1]) == "--mortar-settings") {
        if (argc < 9 || (std::string_view(argv[2]) != "top" && std::string_view(argv[2]) != "bottom") ||
            !*argv[3]) {
            std::cerr << "mortarbar: invalid settings\n";
            return 1;
        }
        bottom = std::string_view(argv[2]) == "bottom";
        font_name = argv[3];
        for (std::size_t i = 0; i < colors.size(); ++i) {
            std::string_view value(argv[4 + i]);
            auto result = std::from_chars(value.data(), value.data() + value.size(), colors[i]);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || colors[i] > 0xffffff) {
                std::cerr << "mortarbar: invalid color\n";
                return 1;
            }
        }
        int index = 8;
        if (index < argc && std::string_view(argv[index]) == "--workspace-colors") {
            ++index;
            workspace_colors.emplace();
            for (auto &color : *workspace_colors) {
                if (index >= argc) { std::cerr << "mortarbar: invalid color\n"; return 1; }
                std::string_view value(argv[index++]);
                auto result = std::from_chars(value.data(), value.data() + value.size(), color);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || color > 0xffffff) {
                    std::cerr << "mortarbar: invalid color\n"; return 1;
                }
            }
        }
        for (auto [name, target, minimum, maximum] : {std::tuple{"--height", static_cast<int*>(nullptr), 1, 65535},
                std::tuple{"--status-padding", &status_padding, 0, 32767}, std::tuple{"--status-edge-padding", &status_edge_padding, 0, 65535}}) {
            if (index < argc && std::string_view(argv[index]) == name) {
                if (++index >= argc) { std::cerr << "mortarbar: invalid settings\n"; return 1; }
                std::string_view value(argv[index++]);
                int number = 0;
                auto result = std::from_chars(value.data(), value.data() + value.size(), number);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number < minimum || number > maximum) {
                    std::cerr << "mortarbar: invalid settings\n"; return 1;
                }
                if (target) *target = number; else height = static_cast<std::uint16_t>(number);
            }
        }
        if (index < argc && std::string_view(argv[index]) == "--hidden") { hidden = true; ++index; }
        if (index < argc && std::string_view(argv[index]) == "--no-workspace-buttons") { workspace_buttons = false; ++index; }
        if (index < argc && std::string_view(argv[index]) == "--wrap-scroll") { wrap_scroll = true; ++index; }
        while (index < argc && std::string_view(argv[index]) == "--output") {
            if (++index >= argc || !*argv[index] || std::string_view(argv[index]).size() > 4096 || selected_outputs.size() >= 64) {
                std::cerr << "mortarbar: invalid output\n";
                return 1;
            }
            selected_outputs.emplace_back(argv[index++]);
        }
        if (index >= argc || std::string_view(argv[index]) != "--") {
            std::cerr << "mortarbar: invalid settings\n";
            return 1;
        }
        command = index + 1;
    }
    std::array<std::string, 4> color_names;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        char text[8];
        std::snprintf(text, sizeof(text), "#%06x", colors[i]);
        color_names[i] = text;
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    int screen_number = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_number);
    if (xcb_connection_has_error(c)) {
        std::cerr << "mortarbar: cannot connect to X11\n";
        xcb_disconnect(c);
        return 1;
    }
    auto iter = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen_number && iter.rem; ++i) xcb_screen_next(&iter);
    if (!iter.rem) { xcb_disconnect(c); return 1; }
    auto* screen = iter.data;
    auto names = atom(c, "_NET_DESKTOP_NAMES");
    auto current = atom(c, "_NET_CURRENT_DESKTOP");
    auto output_workspaces = atom(c, "_MORTAR_OUTPUT_WORKSPACES");
    auto urgent_workspaces = atom(c, "_MORTAR_URGENT_WORKSPACES");
    auto utf8 = atom(c, "UTF8_STRING");
    auto clients = atom(c, "_NET_CLIENT_LIST");
    auto desktop = atom(c, "_NET_WM_DESKTOP");
    auto type = atom(c, "_NET_WM_WINDOW_TYPE");
    auto dock = atom(c, "_NET_WM_WINDOW_TYPE_DOCK");
    auto strut = atom(c, "_NET_WM_STRUT_PARTIAL");
    auto local_bar = atom(c, "_MORTAR_BAR_OUTPUT");
    if (!urgent_workspaces || !output_workspaces || !names || !current || !utf8 || !clients || !desktop || !type || !dock || !strut || !local_bar) {
        std::cerr << "mortarbar: cannot intern X11 atoms\n";
        xcb_disconnect(c);
        return 1;
    }
    bool revealed = false;
    const xcb_query_extension_reply_t* xkb = nullptr;
    if (hidden) {
        xkb = xcb_get_extension_data(c, &xcb_xkb_id);
        auto* version = xcb_xkb_use_extension_reply(c, xcb_xkb_use_extension(c, 1, 0), nullptr);
        if (!xkb || !xkb->present || !version || !version->supported) {
            free(version);
            std::cerr << "mortarbar: XKB unavailable\n";
            xcb_disconnect(c);
            return 1;
        }
        free(version);
        constexpr auto events = XCB_XKB_EVENT_TYPE_STATE_NOTIFY;
        auto* error = xcb_request_check(c, xcb_xkb_select_events_checked(c, XCB_XKB_ID_USE_CORE_KBD,
            events, 0, events, 0, 0, nullptr));
        auto* state = xcb_xkb_get_state_reply(c, xcb_xkb_get_state(c, XCB_XKB_ID_USE_CORE_KBD), nullptr);
        if (error || !state) {
            free(error); free(state);
            std::cerr << "mortarbar: cannot watch XKB state\n";
            xcb_disconnect(c);
            return 1;
        }
        revealed = (state->mods & XCB_MOD_MASK_4) != 0;
        free(state);
    }
    std::uint32_t root_mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(c, screen->root, XCB_CW_EVENT_MASK, &root_mask);
    const auto* randr = xcb_get_extension_data(c, &xcb_randr_id);
    if (randr && randr->present)
        xcb_randr_select_input(c, screen->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
            XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
            XCB_RANDR_NOTIFY_MASK_RESOURCE_CHANGE);
    xcb_visualtype_t* visual = nullptr;
    for (auto depths = xcb_screen_allowed_depths_iterator(screen); depths.rem; xcb_depth_next(&depths))
        for (auto visuals = xcb_depth_visuals_iterator(depths.data); visuals.rem; xcb_visualtype_next(&visuals))
            if (visuals.data->visual_id == screen->root_visual) visual = visuals.data;
    if (!visual) { std::cerr << "mortarbar: cannot find root visual\n"; xcb_disconnect(c); return 1; }
    if (!height) {
        auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        auto* cr = cairo_create(surface);
        if (cairo_status(cr) != CAIRO_STATUS_SUCCESS || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            std::cerr << "mortarbar: cannot measure font\n";
            cairo_destroy(cr); cairo_surface_destroy(surface); xcb_disconnect(c); return 1;
        }
        auto* layout = pango_cairo_create_layout(cr);
        auto* font = pango_font_description_from_string(font_name.c_str());
        pango_layout_set_font_description(layout, font);
        int text_height = 0;
        pango_layout_get_pixel_size(layout, nullptr, &text_height);
        height = static_cast<std::uint16_t>(std::clamp(static_cast<std::int64_t>(text_height) + 6, std::int64_t{1}, std::int64_t{65535}));
        pango_font_description_free(font);
        g_object_unref(layout);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }
    struct Bar {
        xcb_window_t window;
        int width;
        mortar::Rect bounds;
        std::uint32_t selected = 0;
        std::vector<std::uint32_t> workspace_ids;
        cairo_surface_t* surface;
        cairo_t* cr;
        PangoLayout* layout;
        std::vector<std::pair<int, int>> workspace_bounds;
        std::vector<std::pair<int, int>> status_bounds;
    };
    std::vector<Bar> bars;
    std::vector<mortar::Rect> bar_bounds;
    auto clear_bars = [&] {
        for (auto& bar : bars) {
            g_object_unref(bar.layout);
            cairo_destroy(bar.cr);
            cairo_surface_destroy(bar.surface);
            xcb_destroy_window(c, bar.window);
        }
        bars.clear();
    };
    auto update_bars = [&] {
        std::vector<mortar::OutputGeometry> outputs;
        try { outputs = mortar::discover_outputs(c, screen->root); }
        catch (const std::exception& e) {
            std::cerr << "mortarbar: cannot discover outputs: " << e.what() << '\n';
            return false;
        }
        std::vector<mortar::Rect> next_bounds;
        for (const auto& output : outputs) {
            if (!selected_outputs.empty() && std::find(selected_outputs.begin(), selected_outputs.end(), "*") == selected_outputs.end() &&
                std::find(selected_outputs.begin(), selected_outputs.end(), output.name) == selected_outputs.end()) continue;
            const auto& bounds = output.bounds;
            if (bounds.x < 0 || bounds.y < 0 || bounds.width <= 0 || bounds.width > 65535 || bounds.x > 32767 || bounds.y > 32767 || bounds.height < height ||
                (bottom && int64_t(bounds.y) + bounds.height - height > 32767)) continue;
            if (std::find(next_bounds.begin(), next_bounds.end(), bounds) == next_bounds.end()) next_bounds.push_back(bounds);
        }
        bool urgent = !property(c, screen->root, urgent_workspaces, XCB_ATOM_CARDINAL, 32).empty();
        if (next_bounds == bar_bounds && (hidden && !revealed && !urgent ? bars.empty() : !bars.empty())) return true;
        clear_bars();
        bar_bounds = next_bounds;
        if (hidden && !revealed && !urgent) {
            xcb_flush(c);
            return true;
        }
        for (const auto& bounds : next_bounds) {
            auto window = xcb_generate_id(c);
            std::uint32_t values[] = {colors[0], XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_PROPERTY_CHANGE};
            const int y = bottom ? bounds.y + bounds.height - height : bounds.y;
            xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, static_cast<std::int16_t>(bounds.x), static_cast<std::int16_t>(y),
                              static_cast<std::uint16_t>(bounds.width), height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                              screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, type, XCB_ATOM_ATOM, 32, 1, &dock);
            const std::array<std::uint32_t, 4> output = {static_cast<std::uint32_t>(bounds.x), static_cast<std::uint32_t>(bounds.y),
                static_cast<std::uint32_t>(bounds.width), static_cast<std::uint32_t>(bounds.height)};
            if (!hidden) xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, local_bar, XCB_ATOM_CARDINAL, 32, output.size(), output.data());
            if (!hidden && ((!bottom && bounds.y == 0) || (bottom && int64_t(bounds.y) + bounds.height == screen->height_in_pixels))) {
                std::array<std::uint32_t, 12> reservation {};
                reservation[bottom ? 3 : 2] = height;
                reservation[bottom ? 10 : 8] = static_cast<std::uint32_t>(bounds.x);
                reservation[bottom ? 11 : 9] = static_cast<std::uint32_t>(bounds.x + bounds.width - 1);
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, strut, XCB_ATOM_CARDINAL, 32, reservation.size(), reservation.data());
            }
            auto* surface = cairo_xcb_surface_create(c, window, visual, bounds.width, height);
            auto* cr = cairo_create(surface);
            if (cairo_status(cr) != CAIRO_STATUS_SUCCESS || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
                std::cerr << "mortarbar: cannot create drawing surface\n";
                cairo_destroy(cr); cairo_surface_destroy(surface); xcb_destroy_window(c, window);
                clear_bars(); return false;
            }
            auto* layout = pango_cairo_create_layout(cr);
            auto* font = pango_font_description_from_string(font_name.c_str());
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);
            bars.push_back({window, bounds.width, bounds, 0, {}, surface, cr, layout, {}, {}});
            xcb_map_window(c, window);
        }
        xcb_flush(c);
        return true;
    };
    if (!update_bars()) { xcb_disconnect(c); return 1; }
    int input_fd = -1, output_fd = -1;
    pid_t child = -1, watchdog = -1;
    int watchdog_fd = -1;
    if (argc > command) {
        int from_child[2] = {-1, -1}, to_child[2] = {-1, -1}, lifetime[2] = {-1, -1}, ready_pipe[2] = {-1, -1};
        int error = 0;
        if (pipe2(from_child, O_CLOEXEC) < 0 || pipe2(to_child, O_CLOEXEC) < 0 || pipe2(lifetime, O_CLOEXEC) < 0 || pipe2(ready_pipe, O_CLOEXEC) < 0) error = errno;
        if (!error) {
            watchdog = fork();
            if (watchdog < 0) error = errno;
            else if (watchdog == 0) {
                close(xcb_get_file_descriptor(c));
                close(lifetime[1]);
                close(from_child[0]); close(from_child[1]);
                close(to_child[0]); close(to_child[1]);
                close(ready_pipe[0]);
                if (setpgid(0, 0) < 0) _exit(1);
                char byte = 1;
                if (write(ready_pipe[1], &byte, 1) != 1) _exit(1);
                close(ready_pipe[1]);
                while (read(lifetime[0], &byte, 1) < 0 && errno == EINTR) {}
                kill(0, SIGKILL);
                _exit(1);
            }
        }
        if (ready_pipe[1] >= 0) close(ready_pipe[1]);
        if (!error) {
            pollfd ready{ready_pipe[0], POLLIN, 0};
            int result;
            do { result = poll(&ready, 1, 1000); } while (result < 0 && errno == EINTR);
            char byte;
            if (result <= 0 || read(ready_pipe[0], &byte, 1) != 1) error = EIO;
        }
        if (ready_pipe[0] >= 0) close(ready_pipe[0]);
        if (lifetime[0] >= 0) close(lifetime[0]);
        watchdog_fd = lifetime[1];
        posix_spawn_file_actions_t actions;
        bool initialized = false;
        if (!error) { error = posix_spawn_file_actions_init(&actions); initialized = !error; }
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
        posix_spawnattr_t attributes;
        bool attr_initialized = false;
        if (!error) { error = posix_spawnattr_init(&attributes); attr_initialized = !error; }
        if (!error) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (!error) error = posix_spawnattr_setpgroup(&attributes, watchdog);
        if (!error) error = posix_spawnp(&child, argv[command], &actions, &attributes, argv + command, environ);
        if (attr_initialized) posix_spawnattr_destroy(&attributes);
        if (initialized) posix_spawn_file_actions_destroy(&actions);
        if (from_child[1] >= 0) close(from_child[1]);
        if (to_child[0] >= 0) close(to_child[0]);
        if (error) {
            if (from_child[0] >= 0) close(from_child[0]);
            if (to_child[1] >= 0) close(to_child[1]);
            if (watchdog_fd >= 0) { close(watchdog_fd); watchdog_fd = -1; }
            if (watchdog > 0) {
                kill(watchdog, SIGKILL);
                while (waitpid(watchdog, nullptr, 0) < 0 && errno == EINTR) {}
                watchdog = -1;
            }
            std::cerr << "mortarbar: cannot start status command: " << std::strerror(error) << '\n';
        } else {
            input_fd = from_child[0];
            output_fd = to_child[1];
        }
    }
    mortarbar::StatusParser parser;
    std::vector<mortarbar::StatusBlock> blocks;
    bool first_click = true;
    bool output_open = false;
    std::string output;
    std::signal(SIGPIPE, SIG_IGN);
    int flags = fcntl(output_fd, F_GETFL);
    if (flags >= 0 && fcntl(output_fd, F_SETFL, flags | O_NONBLOCK) == 0) output_open = true;
    else if (output_fd >= 0) { close(output_fd); output_fd = -1; }
    bool input_open = input_fd >= 0;
    std::vector<xcb_window_t> watched_clients;
    auto watch_clients = [&] {
        auto raw = property(c, screen->root, clients, XCB_ATOM_WINDOW, 32);
        std::vector<xcb_window_t> next(raw.size() / sizeof(xcb_window_t));
        if (!next.empty()) std::memcpy(next.data(), raw.data(), next.size() * sizeof(xcb_window_t));
        auto own_bar = [&](auto window) {
            return std::any_of(bars.begin(), bars.end(), [&](const auto& bar) { return bar.window == window; });
        };
        std::uint32_t none = 0, mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        for (auto window : watched_clients)
            if (!own_bar(window) && std::find(next.begin(), next.end(), window) == next.end())
                xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &none);
        for (auto window : next)
            if (!own_bar(window) && std::find(watched_clients.begin(), watched_clients.end(), window) == watched_clients.end())
                xcb_change_window_attributes(c, window, XCB_CW_EVENT_MASK, &mask);
        watched_clients = std::move(next);
    };
    watch_clients();
    auto draw = [&](Bar& bar) {
    auto* cr = bar.cr;
    auto* layout = bar.layout;
    auto* surface = bar.surface;
    auto& workspace_bounds = bar.workspace_bounds;
    auto& status_bounds = bar.status_bounds;
    auto set_color = [&](const std::optional<std::string>& color, const char* fallback) {
        const std::string value = color.value_or(fallback);
        unsigned r = 255, g = 255, b = 255, alpha = 255;
        auto hex = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        if ((value.size() == 7 || value.size() == 9) && value[0] == '#') {
            unsigned parts[4] = {0, 0, 0, 255};
            bool valid = true;
            for (std::size_t i = 0; i < (value.size() - 1) / 2; ++i) {
                int hi = hex(value[1 + i * 2]), lo = hex(value[2 + i * 2]);
                if (hi < 0 || lo < 0) { valid = false; break; }
                parts[i] = static_cast<unsigned>(hi * 16 + lo);
            }
            if (valid) { r = parts[0]; g = parts[1]; b = parts[2]; alpha = parts[3]; }
        }
        cairo_set_source_rgba(cr, r / 255.0, g / 255.0, b / 255.0, alpha / 255.0);
    };
    auto measure = [&](const std::string& text, bool markup = false) {
        auto* valid = g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
        PangoAttrList* attributes = nullptr;
        char* plain = nullptr;
        if (markup && pango_parse_markup(valid, -1, 0, &attributes, &plain, nullptr, nullptr)) {
            pango_layout_set_text(layout, plain, -1);
            pango_layout_set_attributes(layout, attributes);
            pango_attr_list_unref(attributes);
            g_free(plain);
        } else {
            pango_layout_set_text(layout, valid, -1);
            pango_layout_set_attributes(layout, nullptr);
        }
        g_free(valid);
        int width = 0, text_height = 0;
        pango_layout_get_pixel_size(layout, &width, &text_height);
        return std::pair{width, text_height};
    };
    auto text_y = [&] {
        PangoRectangle logical;
        pango_layout_get_pixel_extents(layout, nullptr, &logical);
        return (static_cast<double>(height) - logical.height) / 2.0 - logical.y;
    };
        cairo_push_group(cr);
        cairo_set_source_rgb(cr, ((colors[0] >> 16) & 255) / 255.0, ((colors[0] >> 8) & 255) / 255.0,
                             (colors[0] & 255) / 255.0);
        cairo_paint(cr);
        auto raw = property(c, screen->root, names, utf8, 8);
        auto active = property(c, screen->root, current, XCB_ATOM_CARDINAL, 32);
        auto urgent_bytes = property(c, screen->root, urgent_workspaces, XCB_ATOM_CARDINAL, 32);
        std::vector<std::uint32_t> urgent(urgent_bytes.size() / 4);
        if (!urgent.empty()) std::memcpy(urgent.data(), urgent_bytes.data(), urgent_bytes.size());
        auto& selected = bar.selected;
        selected = 0;
        workspace_bounds.clear();
        bar.workspace_ids.clear();
        if (active.size() == sizeof(selected)) std::memcpy(&selected, active.data(), sizeof(selected));
        auto bytes = property(c, screen->root, output_workspaces, XCB_ATOM_CARDINAL, 32);
        std::vector<std::uint32_t> metadata(bytes.size() / 4);
        if (!metadata.empty()) std::memcpy(metadata.data(), bytes.data(), bytes.size());
        std::vector<std::uint32_t> allowed;
        bool valid = metadata.size() >= 2 && metadata[0] == 1 && metadata[1] > 0 && metadata[1] <= 10;
        bool matched = false;
        bool bar_focused = true;
        unsigned focus_count = 0;
        std::size_t pos = 2;
        for (std::uint32_t n = 0; valid && n < metadata[1]; ++n) {
            if (metadata.size() - pos < 7) { valid = false; break; }
            auto x = metadata[pos++], y = metadata[pos++], width = metadata[pos++], h = metadata[pos++];
            auto visible = metadata[pos++], focused = metadata[pos++];
            if (!width || !h || visible >= 10 || focused > 1) { valid = false; break; }
            focus_count += focused;
            bool match = x == static_cast<std::uint32_t>(bar.bounds.x) && y == static_cast<std::uint32_t>(bar.bounds.y) &&
                width == static_cast<std::uint32_t>(bar.bounds.width) && h == static_cast<std::uint32_t>(bar.bounds.height);
            if (match && matched) { valid = false; break; }
            std::vector<std::uint32_t> ids;
            while (pos < metadata.size() && metadata[pos] != UINT32_MAX) {
                auto id = metadata[pos++];
                if (id >= 10 || std::find(ids.begin(), ids.end(), id) != ids.end()) { valid = false; break; }
                ids.push_back(id);
            }
            if (!valid || pos == metadata.size() || std::find(ids.begin(), ids.end(), visible) == ids.end()) { valid = false; break; }
            ++pos;
            if (match) { matched = true; allowed = std::move(ids); selected = visible; bar_focused = focused; }
        }
        valid = valid && pos == metadata.size() && focus_count == 1 && matched;
        if (!valid) {
            allowed.clear();
            selected = 0;
            if (active.size() == sizeof(selected)) std::memcpy(&selected, active.data(), sizeof(selected));
        }
        std::size_t start = 0;
        unsigned index = 0;
        int workspace_width = 0;
        std::size_t label_start = 0;
        unsigned label_index = 0;
        for (std::size_t i = 0; workspace_buttons && i < raw.size(); ++i) {
            if (raw[i] != 0) continue;
            if (valid && std::find(allowed.begin(), allowed.end(), label_index) == allowed.end()) {
                label_start = i + 1;
                ++label_index;
                continue;
            }
            std::string label(reinterpret_cast<const char*>(raw.data() + label_start), i - label_start);
            workspace_width = std::min<int>(bar.width, workspace_width + std::min<int>(bar.width, measure(label).first + 12));
            label_start = i + 1;
            ++label_index;
        }
        auto block_width = [&](const mortarbar::StatusBlock& block, bool short_text) {
            const auto& text = short_text && block.short_text && !block.short_text->empty() ? *block.short_text : block.full_text;
            int width = measure(text, block.markup == "pango").first;
            int minimum = block.min_width_text ? measure(*block.min_width_text, block.markup == "pango").first : block.min_width.value_or(0);
            auto border_width = [&](int value) { return value > 0 ? std::min(value, bar.width + 1) + 3 : 0; };
            int borders = (block.border || block.urgent) ?
                border_width(block.border_left.value_or(1)) + border_width(block.border_right.value_or(1)) : 0;
            return std::min<int>(bar.width + 1, std::max(std::min<int>(width, bar.width + 1), std::clamp(minimum, 0, static_cast<int>(bar.width) + 1)) + borders);
        };
        auto total_width = [&](bool short_text) {
            int total = 0;
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                int gap = i + 1 < blocks.size() ? std::clamp(blocks[i].separator_block_width.value_or(9), 0, static_cast<int>(bar.width) + 1) : 0;
                total = std::min<int>(bar.width + 1, total + block_width(blocks[i], short_text) + gap);
            }
            return total;
        };
        bool use_short = total_width(false) > std::max(0, static_cast<int>(bar.width) - 8 - status_edge_padding - workspace_width);
        int status_left = std::max(8, static_cast<int>(bar.width) - status_edge_padding - total_width(use_short));
        int workspace_right = 0;
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, status_left, height);
        cairo_clip(cr);
        for (std::size_t i = 0; workspace_buttons && i < raw.size() && workspace_right < status_left; ++i) {
            if (raw[i] != 0) continue;
            if (valid && std::find(allowed.begin(), allowed.end(), index) == allowed.end()) {
                start = i + 1;
                ++index;
                continue;
            }
            std::string label(reinterpret_cast<const char*>(raw.data() + start), i - start);
            int text_width = measure(label).first;
            int width = text_width + 12;
            bool is_urgent = std::find(urgent.begin(), urgent.end(), index) != urgent.end();
            bool focused = index == selected && (!valid || bar_focused);
            bool visible = index == selected && valid && !bar_focused;
            std::size_t color_index = is_urgent ? 6 : focused ? 0 : visible ? 2 : 4;
            char custom_background[8];
            char custom_text[8];
            if (workspace_colors) {
                std::snprintf(custom_background, sizeof(custom_background), "#%06x", (*workspace_colors)[color_index]);
                std::snprintf(custom_text, sizeof(custom_text), "#%06x", (*workspace_colors)[color_index + 1]);
            }
            set_color({}, workspace_colors ? custom_background : is_urgent ? "#900000" : focused ? color_names[2].c_str() : visible ? "#5f676a" : color_names[3].c_str());
            cairo_rectangle(cr, workspace_right, 0, width, height);
            cairo_fill(cr);
            set_color({}, workspace_colors ? custom_background : is_urgent ? "#2f343a" : focused ? "#4c7899" : "#333333");
            cairo_rectangle(cr, workspace_right, 0, width, height);
            cairo_rectangle(cr, workspace_right + 1, 1, std::max(0, width - 2), std::max(0, static_cast<int>(height) - 2));
            cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
            cairo_fill(cr);
            cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
            set_color({}, workspace_colors ? custom_text : is_urgent || focused || visible ? color_names[1].c_str() : "#888888");
            cairo_move_to(cr, workspace_right + 6, text_y());
            pango_cairo_show_layout(cr, layout);
            workspace_bounds.emplace_back(workspace_right, std::min(workspace_right + width, status_left));
            bar.workspace_ids.push_back(index);
            workspace_right = std::min(workspace_right + width, status_left);
            start = i + 1;
            ++index;
        }
        cairo_restore(cr);
        status_bounds.assign(blocks.size(), {0, 0});
        int right = static_cast<int>(bar.width) - status_edge_padding;
        for (std::size_t i = blocks.size(); i > 0; --i) {
            const auto& block = blocks[i - 1];
            const auto& text = use_short && block.short_text && !block.short_text->empty() ? *block.short_text : block.full_text;
            int width = block_width(block, use_short);
            int text_width = measure(text, block.markup == "pango").first;
            int gap = i < blocks.size() ? std::clamp(block.separator_block_width.value_or(9), 0, static_cast<int>(bar.width) + 1) : 0;
            right -= gap;
            int left = right - width;
            if (right <= workspace_right) break;
            status_bounds[i - 1] = {std::max(left, workspace_right), right};
            cairo_save(cr);
            cairo_rectangle(cr, workspace_right, 0, bar.width - workspace_right, height);
            cairo_clip(cr);
            if (i < blocks.size() && block.separator) {
                set_color({}, "#666666");
                cairo_set_line_width(cr, 1);
                cairo_move_to(cr, right + gap / 2, 3);
                cairo_line_to(cr, right + gap / 2, static_cast<int>(height) - 3);
                cairo_stroke(cr);
            }
            int padding = std::min(status_padding, static_cast<int>(height) / 2);
            int content_height = height - 2 * padding;
            if (block.background || block.urgent) {
                set_color(block.urgent ? std::optional<std::string>{"#900000"} : block.background, color_names[0].c_str());
                cairo_rectangle(cr, left, padding, width, content_height);
                cairo_fill(cr);
            }
            if (block.border || block.urgent) {
                set_color(block.urgent ? std::optional<std::string>{"#2f343a"} : block.border, "#ffffff");
                int top = std::clamp(block.border_top.value_or(1), 0, content_height);
                int bottom = std::clamp(block.border_bottom.value_or(1), 0, content_height);
                int l = std::clamp(block.border_left.value_or(1), 0, width);
                int r = std::clamp(block.border_right.value_or(1), 0, width);
                cairo_rectangle(cr, left, padding, width, top);
                cairo_rectangle(cr, left, height - padding - bottom, width, bottom);
                cairo_rectangle(cr, left, padding, l, content_height);
                cairo_rectangle(cr, right - r, padding, r, content_height);
                cairo_fill(cr);
            }
            set_color(block.urgent ? std::optional<std::string>{"#ffffff"} : block.color, color_names[1].c_str());
            int left_border = (block.border || block.urgent) && block.border_left.value_or(1) > 0 ? std::min(block.border_left.value_or(1), bar.width + 1) + 3 : 0;
            int right_border = (block.border || block.urgent) && block.border_right.value_or(1) > 0 ? std::min(block.border_right.value_or(1), bar.width + 1) + 3 : 0;
            int extra = std::max(0, width - left_border - right_border - text_width);
            int offset = block.align == "right" ? extra : block.align == "center" ? extra / 2 : 0;
            cairo_move_to(cr, left + left_border + offset, text_y());
            pango_cairo_show_layout(cr, layout);
            cairo_restore(cr);
            right = left;
        }
        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
        cairo_surface_flush(surface);
        xcb_flush(c);
    };
    auto draw_all = [&] { for (auto& bar : bars) draw(bar); };
    draw_all();
    bool running = true;
    bool dirty = false;
    while (running && !stopping && !xcb_connection_has_error(c)) {
        if (watchdog > 0) {
            siginfo_t info {};
            int result;
            do { result = waitid(P_PID, static_cast<id_t>(watchdog), &info, WEXITED | WNOHANG | WNOWAIT); } while (result < 0 && errno == EINTR);
            if (result < 0 || info.si_pid != 0) {
                std::cerr << "mortarbar: status watchdog exited unexpectedly\n";
                kill(-watchdog, SIGKILL);
                if (watchdog_fd >= 0) { close(watchdog_fd); watchdog_fd = -1; }
                running = false;
                break;
            }
        }
        if (child > 0) {
            pid_t result;
            do { result = waitpid(child, nullptr, WNOHANG); } while (result < 0 && errno == EINTR);
            if (result == child || (result < 0 && errno == ECHILD)) child = -1;
        }
        while (auto* event = xcb_poll_for_event(c)) {
            auto event_type = event->response_type & 0x7f;
            if (randr && randr->present && (event_type == randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
                event_type == randr->first_event + XCB_RANDR_NOTIFY)) {
                if (update_bars()) dirty = true;
            }
            if (xkb && event_type == xkb->first_event) {
                auto* state = reinterpret_cast<xcb_xkb_state_notify_event_t*>(event);
                if (state->xkbType == XCB_XKB_STATE_NOTIFY) {
                    bool next = (state->mods & XCB_MOD_MASK_4) != 0;
                    if (next != revealed) {
                        revealed = next;
                        if (update_bars()) dirty = true;
                    }
                }
            }
            if (event_type == XCB_EXPOSE) {
                auto* expose = reinterpret_cast<xcb_expose_event_t*>(event);
                for (auto& bar : bars) if (bar.window == expose->window) dirty = true;
            }
            if (event_type == XCB_PROPERTY_NOTIFY) {
                auto* changed = reinterpret_cast<xcb_property_notify_event_t*>(event);
                if (changed->window == screen->root && changed->atom == clients) {
                    watch_clients();
                    dirty = true;
                } else if (changed->window == screen->root && changed->atom == urgent_workspaces) {
                    if (update_bars()) dirty = true;
                } else if ((changed->window == screen->root && (changed->atom == names || changed->atom == current || changed->atom == output_workspaces)) ||
                           (changed->atom == desktop && std::find(watched_clients.begin(), watched_clients.end(), changed->window) != watched_clients.end())) dirty = true;
            }
            if (event_type == XCB_BUTTON_PRESS) {
                if (dirty) { draw_all(); dirty = false; }
                auto* button = reinterpret_cast<xcb_button_press_event_t*>(event);
                auto found = std::find_if(bars.begin(), bars.end(), [&](const auto& bar) { return bar.window == button->event; });
                if (found == bars.end()) { free(event); continue; }
                auto& status_bounds = found->status_bounds;
                auto& workspace_bounds = found->workspace_bounds;
                bool status_click = false;
                if (parser.click_events()) {
                    for (std::size_t i = 0; i < status_bounds.size(); ++i) {
                        auto [left, right] = status_bounds[i];
                        if (button->event_x < left || button->event_x >= right) continue;
                        mortarbar::ClickEvent click;
                        click.name = blocks[i].name;
                        click.instance = blocks[i].instance;
                        click.button = button->detail;
                        click.x = button->root_x; click.y = button->root_y;
                        click.relative_x = button->event_x - left; click.relative_y = button->event_y;
                        click.width = right - left; click.height = height;
                        for (auto [mask, name] : {std::pair{XCB_MOD_MASK_SHIFT, "Shift"}, {XCB_MOD_MASK_CONTROL, "Control"}, {XCB_MOD_MASK_1, "Mod1"}, {XCB_MOD_MASK_2, "Mod2"}, {XCB_MOD_MASK_3, "Mod3"}, {XCB_MOD_MASK_4, "Mod4"}, {XCB_MOD_MASK_5, "Mod5"}})
                            if (button->state & mask) click.modifiers.emplace_back(name);
                        if (output_open) {
                            auto message = mortarbar::click_event_json(click, first_click);
                            if (message.size() > 65536 || output.size() > 65536 - message.size()) {
                                output_open = false;
                                close(output_fd); output_fd = -1;
                                output.clear();
                            } else {
                                output += message;
                                first_click = false;
                            }
                        }
                        status_click = true;
                        break;
                    }
                }
                std::size_t target = workspace_bounds.size();
                if (status_click) {}
                else if (button->detail == 1) {
                    for (std::size_t i = 0; i < workspace_bounds.size(); ++i)
                        if (button->event_x >= workspace_bounds[i].first && button->event_x < workspace_bounds[i].second) target = i;
                } else if (button->detail == 4 || button->detail == 5) {
                    auto it = std::find(found->workspace_ids.begin(), found->workspace_ids.end(), found->selected);
                    if (it != found->workspace_ids.end()) {
                        auto index = static_cast<std::size_t>(it - found->workspace_ids.begin());
                        if (button->detail == 4 && index > 0) target = index - 1;
                        if (button->detail == 5 && index + 1 < workspace_bounds.size()) target = index + 1;
                        if (wrap_scroll && button->detail == 4 && index == 0) target = workspace_bounds.size() - 1;
                        if (wrap_scroll && button->detail == 5 && index + 1 == workspace_bounds.size()) target = 0;
                    }
                }
                if (target < workspace_bounds.size() && found->workspace_ids[target] != found->selected) {
                    xcb_client_message_event_t message {};
                    message.response_type = XCB_CLIENT_MESSAGE;
                    message.format = 32;
                    message.window = screen->root;
                    message.type = current;
                    message.data.data32[0] = found->workspace_ids[target];
                    message.data.data32[1] = button->time;
                    xcb_send_event(c, 0, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                                   reinterpret_cast<const char*>(&message));
                    xcb_flush(c);
                }
            }
            free(event);
        }
        if (!running) break;
        if (dirty) { draw_all(); dirty = false; continue; }
        pollfd fds[] = {{xcb_get_file_descriptor(c), POLLIN, 0}, {input_open ? input_fd : -1, POLLIN, 0},
                        {output_open && !output.empty() ? output_fd : -1, POLLOUT, 0}};
        int ready = poll(fds, 3, watchdog > 0 ? 1000 : -1);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            output_open = false;
            close(output_fd); output_fd = -1;
            output.clear();
        } else if (fds[2].revents & POLLOUT) {
            auto n = write(output_fd, output.data(), output.size());
            if (n > 0) output.erase(0, static_cast<std::size_t>(n));
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                output_open = false;
                close(output_fd); output_fd = -1;
                output.clear();
            }
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            std::array<char, 1024> buffer {};
            auto n = read(input_fd, buffer.data(), buffer.size());
            auto save = [&](auto frame) {
                std::erase_if(frame, [](const auto& block) { return block.full_text.empty(); });
                blocks = std::move(frame);
                dirty = true;
            };
            if (n == 0) {
                input_open = false;
                if (!parser.finish(save)) std::cerr << "mortarbar: " << parser.error() << '\n';
            } else if (n < 0) {
                if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::cerr << "mortarbar: status read failed: " << std::strerror(errno) << '\n';
                    input_open = false;
                }
            } else if (!parser.feed(std::string_view(buffer.data(), static_cast<std::size_t>(n)), save)) {
                std::cerr << "mortarbar: " << parser.error() << '\n';
                input_open = false;
                close(input_fd);
                input_fd = -1;
                if (watchdog_fd >= 0) { close(watchdog_fd); watchdog_fd = -1; }
                if (child > 0) kill(child, SIGKILL);
                if (watchdog > 0) {
                    while (waitpid(watchdog, nullptr, 0) < 0 && errno == EINTR) {}
                    watchdog = -1;
                }
            }
        }
    }
    if (input_fd >= 0) close(input_fd);
    if (output_fd >= 0) close(output_fd);
    if (watchdog_fd >= 0) close(watchdog_fd);
    if (child > 0) kill(child, SIGKILL);
    if (watchdog > 0) {
        kill(-watchdog, SIGKILL);
        kill(watchdog, SIGKILL);
        while (waitpid(watchdog, nullptr, 0) < 0 && errno == EINTR) {}
    }
    if (child > 0) while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    clear_bars();
    pango_cairo_font_map_set_default(nullptr);
    FcFini();
    xcb_disconnect(c);
    return 0;
}
