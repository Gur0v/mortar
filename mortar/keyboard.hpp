#pragma once

#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace mortar {

struct WorkspaceAction {
    std::uint32_t workspace;
    bool move;
};

class Keyboard {
public:
    Keyboard(xcb_connection_t* connection, xcb_window_t root);
    ~Keyboard();
    Keyboard(const Keyboard&) = delete;
    Keyboard& operator=(const Keyboard&) = delete;

    std::optional<WorkspaceAction> process(const xcb_generic_event_t& event);
    void reload();

private:
    void ungrab() noexcept;
    xcb_connection_t* connection_;
    xcb_window_t root_;
    xkb_context* context_ = nullptr;
    xkb_keymap* keymap_ = nullptr;
    std::uint8_t event_base_ = 0;
    int device_ = -1;
    std::uint16_t alt_ = 0;
    std::uint16_t locks_ = 0;
    std::vector<std::pair<xcb_keycode_t, std::uint16_t>> grabs_;
    struct Key {
        xcb_keycode_t key;
        xkb_layout_index_t group;
        std::uint32_t workspace;
        std::uint16_t modifiers;
    };
    std::vector<Key> keys_;
};

}
