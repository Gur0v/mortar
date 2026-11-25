#include "keyboard.hpp"

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
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <stdexcept>

namespace mortar {

namespace {

void check(xcb_connection_t* connection, xcb_void_cookie_t cookie)
{
    if (auto* error = xcb_request_check(connection, cookie)) {
        auto code = error->error_code;
        std::free(error);
        throw std::runtime_error("XKB request failed: " + std::to_string(code));
    }
    if (xcb_connection_has_error(connection)) {
        throw std::runtime_error("X connection failed");
    }
}

std::uint16_t modifier(xkb_keymap* map, const char* name)
{
    auto index = xkb_keymap_mod_get_index(map, name);
    return index < 8 ? static_cast<std::uint16_t>(1U << index) : 0;
}

}

Keyboard::Keyboard(xcb_connection_t* connection, xcb_window_t root)
    : connection_(connection), root_(root)
{
    if (!connection_ || !xkb_x11_setup_xkb_extension(connection_, XKB_X11_MIN_MAJOR_XKB_VERSION,
            XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
            nullptr, nullptr, &event_base_, nullptr)) {
        throw std::runtime_error("XKB extension unavailable");
    }
    context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context_) {
        throw std::runtime_error("XKB context creation failed");
    }
    try {
        device_ = xkb_x11_get_core_keyboard_device_id(connection_);
        if (device_ < 0) {
            throw std::runtime_error("XKB keyboard unavailable");
        }
        constexpr std::uint16_t events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY;
        constexpr std::uint16_t parts = 0xff;
        check(connection_, xcb_xkb_select_events_checked(connection_, XCB_XKB_ID_USE_CORE_KBD,
            events, 0, events, parts, parts, nullptr));
        reload();
    } catch (...) {
        ungrab();
        xkb_keymap_unref(keymap_);
        xkb_context_unref(context_);
        throw;
    }
}

Keyboard::~Keyboard()
{
    ungrab();
    xkb_keymap_unref(keymap_);
    xkb_context_unref(context_);
}

void Keyboard::ungrab() noexcept
{
    for (auto [key, modifiers] : grabs_) {
        xcb_ungrab_key(connection_, key, root_, modifiers);
    }
    grabs_.clear();
}

void Keyboard::reload()
{
    auto device = xkb_x11_get_core_keyboard_device_id(connection_);
    if (device < 0) throw std::runtime_error("XKB keyboard unavailable");
    auto* map = xkb_x11_keymap_new_from_device(context_, connection_, device, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map) {
        throw std::runtime_error("XKB keymap load failed");
    }
    auto alt = modifier(map, XKB_MOD_NAME_ALT);
    auto locks = modifier(map, XKB_MOD_NAME_CAPS);
    auto* state = xkb_state_new(map);
    if (!state) {
        xkb_keymap_unref(map);
        throw std::runtime_error("XKB state creation failed");
    }
    for (auto key = xkb_keymap_min_keycode(map); key <= xkb_keymap_max_keycode(map) && key <= 255; ++key) {
        const xkb_keysym_t* symbols = nullptr;
        auto count = xkb_keymap_key_get_syms_by_level(map, key, 0, 0, &symbols);
        for (int i = 0; i < count; ++i) {
            if (symbols[i] == XKB_KEY_Num_Lock || symbols[i] == XKB_KEY_Scroll_Lock) {
                xkb_state_update_key(state, key, XKB_KEY_DOWN);
                locks |= static_cast<std::uint16_t>(xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED) & 0xff);
                xkb_state_update_key(state, key, XKB_KEY_UP);
            }
        }
    }
    xkb_state_unref(state);
    if (!alt) {
        xkb_keymap_unref(map);
        throw std::runtime_error("XKB Alt modifier unavailable");
    }
    std::vector<Key> keys;
    for (auto key = xkb_keymap_min_keycode(map); key <= xkb_keymap_max_keycode(map) && key <= 255; ++key) {
        for (xkb_layout_index_t group = 0; group < xkb_keymap_num_layouts_for_key(map, key); ++group) {
            for (xkb_level_index_t level = 0; level < xkb_keymap_num_levels_for_key(map, key, group); ++level) {
                const xkb_keysym_t* symbols = nullptr;
                auto count = xkb_keymap_key_get_syms_by_level(map, key, group, level, &symbols);
                for (int i = 0; i < count; ++i) {
                    if (symbols[i] >= XKB_KEY_0 && symbols[i] <= XKB_KEY_9) {
                        auto workspace = symbols[i] == XKB_KEY_0 ? 9U : symbols[i] - XKB_KEY_1;
                        xkb_mod_mask_t masks[256];
                        auto mask_count = xkb_keymap_key_get_mods_for_level(map, key, group, level, masks, 256);
                        for (std::size_t j = 0; j < mask_count; ++j) {
                            if ((masks[j] & ~(0xffU | static_cast<unsigned>(locks))) != 0) continue;
                            auto mods = static_cast<std::uint16_t>(masks[j] & ~locks);
                            if ((mods & alt) != 0 ||
                                (mods & (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL)) ==
                                    (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL)) continue;
                            keys.push_back({static_cast<xcb_keycode_t>(key), group, workspace, mods});
                        }
                    }
                }
            }
        }
    }
    std::vector<std::pair<xcb_keycode_t, std::uint16_t>> next;
    std::vector<std::pair<xcb_keycode_t, std::uint16_t>> added;
    try {
        for (auto [key, group, workspace, modifiers] : keys) {
            (void)group;
            (void)workspace;
            auto move = (modifiers & XCB_MOD_MASK_SHIFT) ? XCB_MOD_MASK_CONTROL : XCB_MOD_MASK_SHIFT;
            for (unsigned extra : {0U, static_cast<unsigned>(move)}) {
                for (unsigned mask = 0; mask < 256; ++mask) {
                    if ((mask & ~locks) != 0) continue;
                    auto grab = std::pair{key, static_cast<std::uint16_t>(alt | modifiers | extra | mask)};
                    if (std::find(next.begin(), next.end(), grab) != next.end()) continue;
                    next.push_back(grab);
                    if (std::find(grabs_.begin(), grabs_.end(), grab) != grabs_.end()) continue;
                    added.push_back(grab);
                    try {
                        check(connection_, xcb_grab_key_checked(connection_, 1, root_, grab.second, key,
                            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
                    } catch (...) {
                        added.pop_back();
                        throw;
                    }
                }
            }
        }
    } catch (...) {
        for (auto [key, mods] : added) xcb_ungrab_key(connection_, key, root_, mods);
        xkb_keymap_unref(map);
        throw;
    }
    for (auto [key, mods] : grabs_) {
        if (std::find(next.begin(), next.end(), std::pair{key, mods}) == next.end())
            xcb_ungrab_key(connection_, key, root_, mods);
    }
    grabs_ = std::move(next);
    device_ = device;
    xkb_keymap_unref(keymap_);
    keymap_ = map;
    keys_ = std::move(keys);
    alt_ = alt;
    locks_ = locks;
}

std::optional<WorkspaceAction> Keyboard::process(const xcb_generic_event_t& event)
{
    auto type = event.response_type & 0x7f;
    if (type == event_base_) {
        const auto& xkb = reinterpret_cast<const xcb_xkb_map_notify_event_t&>(event);
        if (xkb.xkbType == XCB_XKB_MAP_NOTIFY || xkb.xkbType == XCB_XKB_NEW_KEYBOARD_NOTIFY) {
            reload();
        }
    } else if (type == XCB_KEY_PRESS) {
        const auto& press = reinterpret_cast<const xcb_key_press_event_t&>(event);
        auto mods = static_cast<std::uint16_t>(press.state & 0xff & ~locks_);
        if ((mods & alt_) != alt_) return std::nullopt;
        auto* state = xkb_state_new(keymap_);
        if (!state) throw std::runtime_error("XKB state creation failed");
        xkb_state_update_mask(state, 0, 0, 0, 0, 0, (press.state >> 13) & 3);
        auto group = xkb_state_key_get_layout(state, press.detail);
        xkb_state_unref(state);
        for (auto [key, layout, workspace, modifiers] : keys_) {
            if (key != press.detail || layout != group) continue;
            if (mods == (alt_ | modifiers)) return WorkspaceAction{workspace, false};
        }
        for (auto [key, layout, workspace, modifiers] : keys_) {
            auto move = (modifiers & XCB_MOD_MASK_SHIFT) ? XCB_MOD_MASK_CONTROL : XCB_MOD_MASK_SHIFT;
            if (key == press.detail && layout == group && mods == (alt_ | modifiers | move))
                return WorkspaceAction{workspace, true};
        }
    }
    return std::nullopt;
}

}
