#include "status.hpp"

#include <xcb/xcb.h>

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
    auto window = xcb_generate_id(c);
    std::uint32_t values[] = {0x222222, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0,
                      screen->width_in_pixels, height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, type, XCB_ATOM_ATOM, 32, 1, &dock);
    std::array<std::uint32_t, 12> reservation {};
    reservation[2] = height;
    reservation[9] = screen->width_in_pixels - 1;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, strut, XCB_ATOM_CARDINAL, 32, reservation.size(), reservation.data());
    std::uint32_t root_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(c, screen->root, XCB_CW_EVENT_MASK, &root_mask);
    auto gc = xcb_generate_id(c);
    std::uint32_t foreground = 0xffffff;
    xcb_create_gc(c, gc, window, XCB_GC_FOREGROUND, &foreground);
    xcb_map_window(c, window);
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
    std::vector<std::pair<int, int>> status_bounds;
    bool first_click = true;
    bool output_open = false;
    std::string output;
    std::signal(SIGPIPE, SIG_IGN);
    int flags = fcntl(output_fd, F_GETFL);
    if (flags >= 0 && fcntl(output_fd, F_SETFL, flags | O_NONBLOCK) == 0) output_open = true;
    bool input_open = input_fd >= 0;
    std::vector<std::pair<int, int>> workspace_bounds;
    std::uint32_t selected = 0;
    auto draw = [&] {
        xcb_clear_area(c, 0, window, 0, 0, 0, 0);
        auto raw = property(c, screen->root, names, utf8, 8);
        auto active = property(c, screen->root, current, XCB_ATOM_CARDINAL, 32);
        selected = 0;
        workspace_bounds.clear();
        if (active.size() == sizeof(selected)) std::copy(active.begin(), active.end(), reinterpret_cast<std::uint8_t*>(&selected));
        std::string text;
        std::size_t start = 0;
        unsigned index = 0;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != 0) continue;
            if (text.size() < 220) {
                int left = 8 + static_cast<int>(text.size()) * 6;
                text += index == selected ? "[" : " ";
                for (std::size_t j = start; j < i && text.size() < 220; ++j)
                    text += raw[j] >= 32 && raw[j] < 127 ? static_cast<char>(raw[j]) : '?';
                text += index == selected ? "] " : "  ";
                workspace_bounds.emplace_back(left, 8 + static_cast<int>(text.size()) * 6);
            }
            start = i + 1;
            ++index;
        }
        if (text.empty()) {
            text = "[1] ";
            workspace_bounds.emplace_back(8, 32);
        }
        xcb_image_text_8(c, static_cast<std::uint8_t>(text.size()), window, gc, 8, 16, text.data());
        status_bounds.assign(blocks.size(), {0, 0});
        int right = screen->width_in_pixels - 8;
        for (std::size_t i = blocks.size(); i > 0; --i) {
            const auto& block = blocks[i - 1];
            std::string label;
            for (unsigned char ch : block.full_text) {
                if (label.size() == 255) break;
                label += ch >= 32 && ch < 127 ? static_cast<char>(ch) : '?';
            }
            int width = static_cast<int>(label.size()) * 6 + 12;
            int left = right - width;
            if (left < 8 + static_cast<int>(text.size()) * 6) break;
            status_bounds[i - 1] = {left, right};
            xcb_image_text_8(c, static_cast<std::uint8_t>(label.size()), window, gc,
                             static_cast<std::int16_t>(left + 6), 16, label.data());
            right = left;
        }
        xcb_flush(c);
    };
    draw();
    bool running = true;
    while (running && !stopping && !xcb_connection_has_error(c)) {
        while (auto* event = xcb_poll_for_event(c)) {
            auto event_type = event->response_type & 0x7f;
            if (event_type == XCB_DESTROY_NOTIFY && reinterpret_cast<xcb_destroy_notify_event_t*>(event)->window == window) {
                running = false;
                free(event);
                break;
            }
            if (event_type == XCB_EXPOSE) draw();
            if (event_type == XCB_PROPERTY_NOTIFY) {
                auto* changed = reinterpret_cast<xcb_property_notify_event_t*>(event);
                if (changed->window == screen->root && (changed->atom == names || changed->atom == current)) draw();
            }
            if (event_type == XCB_BUTTON_PRESS) {
                auto* button = reinterpret_cast<xcb_button_press_event_t*>(event);
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
                draw();
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
    xcb_disconnect(c);
    return 0;
}
