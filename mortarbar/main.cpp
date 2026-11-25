#include "status.hpp"
#include "../mortar/randr.hpp"

#include <xcb/randr.h>

#include <xcb/xcb.h>
#include <cairo/cairo-xcb.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <fcntl.h>
#include <cstdlib>
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
    auto utf8 = atom(c, "UTF8_STRING");
    auto type = atom(c, "_NET_WM_WINDOW_TYPE");
    auto dock = atom(c, "_NET_WM_WINDOW_TYPE_DOCK");
    auto strut = atom(c, "_NET_WM_STRUT_PARTIAL");
    if (!names || !current || !utf8 || !type || !dock || !strut) {
        std::cerr << "mortarbar: cannot intern X11 atoms\n";
        xcb_disconnect(c);
        return 1;
    }
    constexpr std::uint16_t height = 24;
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
    struct Bar {
        xcb_window_t window;
        int width;
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
            const auto& bounds = output.bounds;
            if (bounds.y != 0 || bounds.x < 0 || bounds.width <= 0 || bounds.width > 65535 || bounds.x > 32767 || bounds.height < height) continue;
            if (std::find(next_bounds.begin(), next_bounds.end(), bounds) == next_bounds.end()) next_bounds.push_back(bounds);
        }
        if (next_bounds == bar_bounds && !bars.empty()) return true;
        clear_bars();
        bar_bounds = next_bounds;
        for (const auto& bounds : next_bounds) {
            auto window = xcb_generate_id(c);
            std::uint32_t values[] = {0x222222, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS};
            xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, static_cast<std::int16_t>(bounds.x), 0,
                              static_cast<std::uint16_t>(bounds.width), height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                              screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, type, XCB_ATOM_ATOM, 32, 1, &dock);
            std::array<std::uint32_t, 12> reservation {};
            reservation[2] = height;
            reservation[8] = static_cast<std::uint32_t>(bounds.x);
            reservation[9] = static_cast<std::uint32_t>(bounds.x + bounds.width - 1);
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, strut, XCB_ATOM_CARDINAL, 32, reservation.size(), reservation.data());
            auto* surface = cairo_xcb_surface_create(c, window, visual, bounds.width, height);
            auto* cr = cairo_create(surface);
            if (cairo_status(cr) != CAIRO_STATUS_SUCCESS || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
                std::cerr << "mortarbar: cannot create drawing surface\n";
                cairo_destroy(cr); cairo_surface_destroy(surface); xcb_destroy_window(c, window);
                clear_bars(); return false;
            }
            auto* layout = pango_cairo_create_layout(cr);
            auto* font = pango_font_description_from_string("monospace 10");
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);
            bars.push_back({window, bounds.width, surface, cr, layout, {}, {}});
            xcb_map_window(c, window);
        }
        xcb_flush(c);
        return true;
    };
    if (!update_bars()) { xcb_disconnect(c); return 1; }
    int input_fd = -1, output_fd = -1;
    pid_t child = -1;
    if (argc > 1) {
        int from_child[2] = {-1, -1}, to_child[2] = {-1, -1};
        int error = 0;
        if (pipe2(from_child, O_CLOEXEC) < 0 || pipe2(to_child, O_CLOEXEC) < 0) error = errno;
        posix_spawn_file_actions_t actions;
        bool initialized = false;
        if (!error) { error = posix_spawn_file_actions_init(&actions); initialized = !error; }
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
        posix_spawnattr_t attributes;
        bool attr_initialized = false;
        if (!error) { error = posix_spawnattr_init(&attributes); attr_initialized = !error; }
        if (!error) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (!error) error = posix_spawnattr_setpgroup(&attributes, 0);
        if (!error) error = posix_spawnp(&child, argv[1], &actions, &attributes, argv + 1, environ);
        if (attr_initialized) posix_spawnattr_destroy(&attributes);
        if (initialized) posix_spawn_file_actions_destroy(&actions);
        if (from_child[1] >= 0) close(from_child[1]);
        if (to_child[0] >= 0) close(to_child[0]);
        if (error) {
            if (from_child[0] >= 0) close(from_child[0]);
            if (to_child[1] >= 0) close(to_child[1]);
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
    bool input_open = input_fd >= 0;
    std::uint32_t selected = 0;
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
        cairo_set_source_rgb(cr, 0x22 / 255.0, 0x22 / 255.0, 0x22 / 255.0);
        cairo_paint(cr);
        auto raw = property(c, screen->root, names, utf8, 8);
        auto active = property(c, screen->root, current, XCB_ATOM_CARDINAL, 32);
        selected = 0;
        workspace_bounds.clear();
        if (active.size() == sizeof(selected)) std::copy(active.begin(), active.end(), reinterpret_cast<std::uint8_t*>(&selected));
        std::size_t start = 0;
        unsigned index = 0;
        int workspace_width = 0;
        std::size_t label_start = 0;
        unsigned label_index = 0;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != 0) continue;
            std::string label = label_index == selected ? "[" : " ";
            label.append(reinterpret_cast<const char*>(raw.data() + label_start), i - label_start);
            label += label_index == selected ? "] " : "  ";
            workspace_width = std::min<int>(bar.width, workspace_width + std::min<int>(bar.width, measure(label).first));
            label_start = i + 1;
            ++label_index;
        }
        if (!label_index) workspace_width = measure("[1] ").first;
        auto block_width = [&](const mortarbar::StatusBlock& block, bool short_text) {
            const auto& text = short_text && block.short_text && !block.short_text->empty() ? *block.short_text : block.full_text;
            int width = measure(text, block.markup == "pango").first;
            int minimum = block.min_width_text ? measure(*block.min_width_text, block.markup == "pango").first : block.min_width.value_or(0);
            return std::min<int>(bar.width + 1, std::max(std::min<int>(width, bar.width + 1), std::clamp(minimum, 0, static_cast<int>(bar.width) + 1)) + 12);
        };
        auto total_width = [&](bool short_text) {
            int total = 0;
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                int gap = i + 1 < blocks.size() ? std::clamp(blocks[i].separator_block_width.value_or(9), 0, static_cast<int>(bar.width) + 1) : 0;
                total = std::min<int>(bar.width + 1, total + block_width(blocks[i], short_text) + gap);
            }
            return total;
        };
        bool use_short = total_width(false) > std::max(0, static_cast<int>(bar.width) - 16 - workspace_width);
        int status_left = std::max(8, static_cast<int>(bar.width) - 8 - total_width(use_short));
        int workspace_right = 8;
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, status_left, height);
        cairo_clip(cr);
        for (std::size_t i = 0; i < raw.size() && workspace_right < status_left; ++i) {
            if (raw[i] != 0) continue;
            std::string label = index == selected ? "[" : " ";
            label.append(reinterpret_cast<const char*>(raw.data() + start), i - start);
            label += index == selected ? "] " : "  ";
            auto [width, text_height] = measure(label);
            set_color({}, "#ffffff");
            cairo_move_to(cr, workspace_right, (height - text_height) / 2);
            pango_cairo_show_layout(cr, layout);
            workspace_bounds.emplace_back(workspace_right, std::min(workspace_right + width, status_left));
            workspace_right = std::min(workspace_right + width, status_left);
            start = i + 1;
            ++index;
        }
        if (workspace_bounds.empty()) {
            auto [width, text_height] = measure("[1] ");
            set_color({}, "#ffffff");
            cairo_move_to(cr, workspace_right, (height - text_height) / 2);
            pango_cairo_show_layout(cr, layout);
            workspace_bounds.emplace_back(workspace_right, std::min(workspace_right + width, status_left));
            workspace_right = std::min(workspace_right + width, status_left);
        }
        cairo_restore(cr);
        status_bounds.assign(blocks.size(), {0, 0});
        int right = bar.width - 8;
        for (std::size_t i = blocks.size(); i > 0; --i) {
            const auto& block = blocks[i - 1];
            const auto& text = use_short && block.short_text && !block.short_text->empty() ? *block.short_text : block.full_text;
            int width = block_width(block, use_short);
            auto [text_width, text_height] = measure(text, block.markup == "pango");
            int gap = i < blocks.size() ? std::clamp(block.separator_block_width.value_or(9), 0, static_cast<int>(bar.width) + 1) : 0;
            right -= gap;
            int left = right - width;
            if (left < workspace_right) break;
            status_bounds[i - 1] = {left, right};
            if (gap && block.separator) {
                set_color({}, "#666666");
                cairo_rectangle(cr, right + gap / 2, 3, 1, height - 6);
                cairo_fill(cr);
            }
            if (block.background || block.urgent) {
                set_color(block.urgent ? std::optional<std::string>{"#900000"} : block.background, "#222222");
                cairo_rectangle(cr, left, 0, width, height);
                cairo_fill(cr);
            }
            if (block.border || block.urgent) {
                set_color(block.urgent ? std::optional<std::string>{"#2f343a"} : block.border, "#ffffff");
                int top = std::clamp(block.border_top.value_or(1), 0, static_cast<int>(height));
                int bottom = std::clamp(block.border_bottom.value_or(1), 0, static_cast<int>(height));
                int l = std::clamp(block.border_left.value_or(1), 0, width);
                int r = std::clamp(block.border_right.value_or(1), 0, width);
                cairo_rectangle(cr, left, 0, width, top);
                cairo_rectangle(cr, left, height - bottom, width, bottom);
                cairo_rectangle(cr, left, 0, l, height);
                cairo_rectangle(cr, right - r, 0, r, height);
                cairo_fill(cr);
            }
            set_color(block.urgent ? std::optional<std::string>{"#ffffff"} : block.color, "#ffffff");
            int extra = std::max(0, width - 12 - text_width);
            int offset = block.align == "right" ? extra : block.align == "center" ? extra / 2 : 0;
            cairo_move_to(cr, left + 6 + offset, (height - text_height) / 2);
            pango_cairo_show_layout(cr, layout);
            right = left;
        }
        cairo_surface_flush(surface);
        xcb_flush(c);
    };
    auto draw_all = [&] { for (auto& bar : bars) draw(bar); };
    draw_all();
    bool running = true;
    while (running && !stopping && !xcb_connection_has_error(c)) {
        while (auto* event = xcb_poll_for_event(c)) {
            auto event_type = event->response_type & 0x7f;
            if (randr && randr->present && (event_type == randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
                event_type == randr->first_event + XCB_RANDR_NOTIFY)) {
                if (update_bars()) draw_all();
            }
            if (event_type == XCB_EXPOSE) {
                auto* expose = reinterpret_cast<xcb_expose_event_t*>(event);
                for (auto& bar : bars) if (bar.window == expose->window) draw(bar);
            }
            if (event_type == XCB_PROPERTY_NOTIFY) {
                auto* changed = reinterpret_cast<xcb_property_notify_event_t*>(event);
                if (changed->window == screen->root && (changed->atom == names || changed->atom == current)) draw_all();
            }
            if (event_type == XCB_BUTTON_PRESS) {
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
                } else if (button->detail == 4 && selected > 0) target = selected - 1;
                else if (button->detail == 5 && selected + 1 < workspace_bounds.size()) target = selected + 1;
                if (target < workspace_bounds.size() && target != selected) {
                    xcb_client_message_event_t message {};
                    message.response_type = XCB_CLIENT_MESSAGE;
                    message.format = 32;
                    message.window = screen->root;
                    message.type = current;
                    message.data.data32[0] = static_cast<std::uint32_t>(target);
                    message.data.data32[1] = button->time;
                    xcb_send_event(c, 0, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                                   reinterpret_cast<const char*>(&message));
                    xcb_flush(c);
                }
            }
            free(event);
        }
        if (!running) break;
        pollfd fds[] = {{xcb_get_file_descriptor(c), POLLIN, 0}, {input_open ? input_fd : -1, POLLIN, 0},
                        {output_open && !output.empty() ? output_fd : -1, POLLOUT, 0}};
        int ready = poll(fds, 3, -1);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            output_open = false;
            output.clear();
        } else if (fds[2].revents & POLLOUT) {
            auto n = write(output_fd, output.data(), output.size());
            if (n > 0) output.erase(0, static_cast<std::size_t>(n));
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                output_open = false;
                output.clear();
            }
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            std::array<char, 1024> buffer {};
            auto n = read(input_fd, buffer.data(), buffer.size());
            if (n <= 0) {
                input_open = false;
                if (!parser.finish()) std::cerr << "mortarbar: " << parser.error() << '\n';
            } else if (!parser.feed(std::string_view(buffer.data(), static_cast<std::size_t>(n)), [&](auto frame) {
                blocks = std::move(frame);
                draw_all();
            })) {
                std::cerr << "mortarbar: " << parser.error() << '\n';
                input_open = false;
                close(input_fd);
                input_fd = -1;
                if (child > 0) kill(-child, SIGKILL);
            }
        }
    }
    if (input_fd >= 0) close(input_fd);
    if (output_fd >= 0) close(output_fd);
    if (child > 0) {
        kill(-child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }
    clear_bars();
    pango_cairo_font_map_set_default(nullptr);
    FcFini();
    xcb_disconnect(c);
    return 0;
}
