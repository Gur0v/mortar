#include "../mortar/keyboard.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

}

int main()
{
    auto* connection = xcb_connect(nullptr, nullptr);
    try {
        require(connection && !xcb_connection_has_error(connection), "X connection failed");
        auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
        require(screen != nullptr, "X screen unavailable");
        {
            mortar::Keyboard keyboard(connection, screen->root);
            auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            require(context != nullptr, "XKB context unavailable");
            auto device = xkb_x11_get_core_keyboard_device_id(connection);
            auto* map = xkb_x11_keymap_new_from_device(context, connection, device, XKB_KEYMAP_COMPILE_NO_FLAGS);
            require(map != nullptr, "XKB keymap unavailable");
            auto letter = static_cast<xcb_keycode_t>(xkb_keymap_key_by_name(map, "AC01"));
            xcb_keycode_t digits[10]{};
            for (auto key = xkb_keymap_min_keycode(map); key <= xkb_keymap_max_keycode(map) && key <= 255; ++key) {
                const xkb_keysym_t* symbols = nullptr;
                auto count = xkb_keymap_key_get_syms_by_level(map, key, 0, 0, &symbols);
                for (int i = 0; i < count; ++i) {
                    if (symbols[i] >= XKB_KEY_0 && symbols[i] <= XKB_KEY_9)
                        digits[symbols[i] - XKB_KEY_0] = static_cast<xcb_keycode_t>(key);
                }
            }
            auto alt_index = xkb_keymap_mod_get_index(map, XKB_MOD_NAME_ALT);
            require(alt_index < 8, "Alt modifier unavailable");
            auto alt = static_cast<std::uint16_t>(1U << alt_index);
            xkb_keymap_unref(map);
            xkb_context_unref(context);

            xcb_key_press_event_t press{};
            press.response_type = XCB_KEY_PRESS;
            auto process = [&] { return keyboard.process(reinterpret_cast<const xcb_generic_event_t&>(press)); };
            for (unsigned digit = 0; digit < 10; ++digit) {
                require(digits[digit] != 0, "digit key unavailable");
                press.detail = digits[digit];
                auto expected = digit == 0 ? 9U : digit - 1;
                press.state = alt;
                auto action = process();
                require(action && action->workspace == expected && !action->move, "workspace mapping failed");
                press.state = alt | XCB_MOD_MASK_SHIFT;
                action = process();
                require(action && action->workspace == expected && action->move, "move mapping failed");
                press.state = alt | XCB_MOD_MASK_LOCK;
                action = process();
                require(action && action->workspace == expected && !action->move, "Caps Lock mapping failed");
                press.state = alt | XCB_MOD_MASK_CONTROL;
                require(!process(), "unexpected Control binding");
                press.state = 0;
                require(!process(), "binding without Alt");
            }
            keyboard.replace_bindings({{XKB_KEY_1, alt, 41}});
            press.detail = digits[1];
            press.state = alt;
            require(!process() && keyboard.callback() == 41, "custom workspace override failed");
            keyboard.replace_bindings({{XKB_KEY_a, 0, 40, true}, {XKB_KEY_a, XCB_MOD_MASK_CONTROL, 42}});
            press.detail = letter;
            press.state = alt;
            require(!process() && keyboard.callback() == 40, "dynamic Alt binding failed");
            keyboard.reload();
            require(!process() && keyboard.callback() == 40, "dynamic Alt binding lost on reload");
            auto* mapping = xcb_get_modifier_mapping_reply(connection, xcb_get_modifier_mapping(connection), nullptr);
            require(mapping != nullptr && mapping->keycodes_per_modifier != 0, "modifier mapping unavailable");
            auto count = mapping->keycodes_per_modifier;
            auto* keycodes = xcb_get_modifier_mapping_keycodes(mapping);
            std::vector<xcb_keycode_t> remapped(keycodes, keycodes + 8 * count);
            require(alt == XCB_MOD_MASK_1, "unexpected initial Alt modifier");
            for (unsigned i = 0; i < count; ++i) std::swap(remapped[3 * count + i], remapped[4 * count + i]);
            auto* reply = xcb_set_modifier_mapping_reply(connection,
                xcb_set_modifier_mapping(connection, count, remapped.data()), nullptr);
            require(reply && reply->status == XCB_MAPPING_STATUS_SUCCESS, "modifier remapping failed");
            std::free(reply);
            keyboard.reload();
            require(keyboard.alt_modifier() == XCB_MOD_MASK_2, "Alt modifier did not change");
            press.state = XCB_MOD_MASK_2;
            require(!process() && keyboard.callback() == 40, "dynamic Alt binding did not follow mapping");
            press.state = alt;
            require(!process() && !keyboard.callback(), "dynamic Alt binding retained old mapping");
            reply = xcb_set_modifier_mapping_reply(connection,
                xcb_set_modifier_mapping(connection, count, keycodes), nullptr);
            require(reply && reply->status == XCB_MAPPING_STATUS_SUCCESS, "modifier mapping restore failed");
            std::free(reply);
            std::free(mapping);
            keyboard.reload();
            press.detail = letter;
            press.state = XCB_MOD_MASK_CONTROL;
            require(!process() && keyboard.callback() == 42, "custom binding failed");
            keyboard.reload();
            require(!process() && keyboard.callback() == 42, "custom binding lost on reload");
            auto* other = xcb_connect(nullptr, nullptr);
            require(other && !xcb_connection_has_error(other), "second X connection failed");
            auto* error = xcb_request_check(other, xcb_grab_key_checked(other, 1, screen->root,
                XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_SHIFT, letter, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            require(!error, "conflict setup failed");
            bool failed = false;
            try {
                keyboard.replace_bindings({{XKB_KEY_a, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_SHIFT, 43}});
            } catch (const std::exception&) {
                failed = true;
            }
            require(failed, "conflicting grab succeeded");
            require(!process() && keyboard.callback() == 42, "failed replacement lost binding");
            error = xcb_request_check(other, xcb_grab_key_checked(other, 1, screen->root,
                XCB_MOD_MASK_CONTROL, letter, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            require(error && error->error_code == XCB_ACCESS, "failed replacement lost old grab");
            std::free(error);
            xcb_disconnect(other);
            keyboard.replace_bindings({});
            require(!process() && !keyboard.callback(), "binding removal failed");
            press.detail = digits[9];
            press.response_type = XCB_KEY_RELEASE;
            press.state = alt;
            require(!process(), "key release triggered binding");
            keyboard.reload();
            press.response_type = XCB_KEY_PRESS;
            auto action = process();
            require(action && action->workspace == 8 && !action->move, "mapping failed after reload");
        }
        require(std::system("setxkbmap -layout fr") == 0, "French layout setup failed");
        {
            mortar::Keyboard keyboard(connection, screen->root);
            auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            require(context != nullptr, "XKB context unavailable");
            auto* map = xkb_x11_keymap_new_from_device(context, connection,
                xkb_x11_get_core_keyboard_device_id(connection), XKB_KEYMAP_COMPILE_NO_FLAGS);
            require(map != nullptr, "XKB keymap unavailable");
            auto key = xkb_keymap_key_by_name(map, "AE01");
            auto alt_index = xkb_keymap_mod_get_index(map, XKB_MOD_NAME_ALT);
            require(alt_index < 8, "Alt modifier unavailable");
            auto alt = static_cast<std::uint16_t>(1U << alt_index);
            xkb_keymap_unref(map);
            xkb_context_unref(context);
            xcb_key_press_event_t press{};
            press.response_type = XCB_KEY_PRESS;
            press.detail = static_cast<xcb_keycode_t>(key);
            auto process = [&] { return keyboard.process(reinterpret_cast<const xcb_generic_event_t&>(press)); };
            press.state = alt;
            require(!process(), "shifted digit matched without Shift");
            press.state = alt | XCB_MOD_MASK_SHIFT;
            auto action = process();
            require(action && action->workspace == 0 && !action->move, "shifted digit switch failed");
            press.state |= XCB_MOD_MASK_CONTROL;
            action = process();
            require(action && action->workspace == 0 && action->move, "shifted digit move failed");
            auto* other = xcb_connect(nullptr, nullptr);
            require(other && !xcb_connection_has_error(other), "second X connection failed");
            for (auto mods : {static_cast<std::uint16_t>(alt | XCB_MOD_MASK_SHIFT), press.state}) {
                auto* error = xcb_request_check(other, xcb_grab_key_checked(other, 1, screen->root,
                    mods, press.detail, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
                require(error && error->error_code == XCB_ACCESS, "shifted digit shortcut was not grabbed");
                std::free(error);
            }
            xcb_disconnect(other);
        }
        require(std::system("setxkbmap -layout ir -variant ku_ara") == 0, "custom layout setup failed");
        {
            mortar::Keyboard keyboard(connection, screen->root);
            auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            require(context != nullptr, "XKB context unavailable");
            auto* map = xkb_x11_keymap_new_from_device(context, connection,
                xkb_x11_get_core_keyboard_device_id(connection), XKB_KEYMAP_COMPILE_NO_FLAGS);
            require(map != nullptr, "XKB keymap unavailable");
            auto key = xkb_keymap_key_by_name(map, "AE01");
            xkb_mod_mask_t masks[256];
            auto count = xkb_keymap_key_get_mods_for_level(map, key, 0, 2, masks, 256);
            require(count != 0 && masks[0] != 0 && masks[0] < 256, "level modifier unavailable");
            auto alt_index = xkb_keymap_mod_get_index(map, XKB_MOD_NAME_ALT);
            require(alt_index < 8, "Alt modifier unavailable");
            auto alt = static_cast<std::uint16_t>(1U << alt_index);
            auto level = static_cast<std::uint16_t>(masks[0]);
            xkb_keymap_unref(map);
            xkb_context_unref(context);
            xcb_key_press_event_t press{};
            press.response_type = XCB_KEY_PRESS;
            press.detail = static_cast<xcb_keycode_t>(key);
            auto process = [&] { return keyboard.process(reinterpret_cast<const xcb_generic_event_t&>(press)); };
            press.state = alt;
            require(!process(), "digit matched without level modifier");
            auto* other = xcb_connect(nullptr, nullptr);
            require(other && !xcb_connection_has_error(other), "second X connection failed");
            auto* error = xcb_request_check(other, xcb_grab_key_checked(other, 1, screen->root,
                alt | level, press.detail, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            require(error && error->error_code == XCB_ACCESS, "level modifier was not grabbed");
            std::free(error);
            error = xcb_request_check(other, xcb_grab_key_checked(other, 1, screen->root,
                alt | level | XCB_MOD_MASK_SHIFT, press.detail, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            require(error && error->error_code == XCB_ACCESS, "level modifier move was not grabbed");
            std::free(error);
            xcb_disconnect(other);
            press.state = alt | level;
            auto action = process();
            require(action && action->workspace == 0 && !action->move, "level modifier mapping failed");
            press.state = alt | level | XCB_MOD_MASK_SHIFT;
            action = process();
            require(action && action->workspace == 0 && action->move, "level modifier move failed");
            press.state = alt | level | XCB_MOD_MASK_CONTROL;
            require(!process(), "unexpected level modifier binding");
        }
        xcb_disconnect(connection);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (connection) xcb_disconnect(connection);
        return 1;
    }
}
