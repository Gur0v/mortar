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
            auto process = [&] { keyboard.process(reinterpret_cast<const xcb_generic_event_t&>(press)); };
            for (unsigned digit = 0; digit < 10; ++digit) {
                require(digits[digit] != 0, "digit key unavailable");
                press.detail = digits[digit];
                press.state = alt;
                process();
                require(!keyboard.callback(), "unexpected default digit binding");
                press.state = alt | XCB_MOD_MASK_SHIFT;
                process();
                require(!keyboard.callback(), "unexpected default shifted digit binding");
                press.state = alt | XCB_MOD_MASK_LOCK;
                process();
                require(!keyboard.callback(), "unexpected default locked digit binding");
                press.state = alt | XCB_MOD_MASK_CONTROL;
                process();
                require(!keyboard.callback(), "unexpected Control binding");
                press.state = 0;
                process();
                require(!keyboard.callback(), "binding without Alt");
            }
            keyboard.replace_bindings({{XKB_KEY_1, alt, 41}});
            press.detail = digits[1];
            press.state = alt;
            process();
            require(keyboard.callback() == 41, "custom workspace override failed");
            keyboard.replace_bindings({{XKB_KEY_a, XCB_MOD_MASK_SHIFT, 39, true},
                {XKB_KEY_A, 0, 44, true}, {XKB_KEY_a, 0, 40, true},
                {XKB_KEY_a, XCB_MOD_MASK_CONTROL, 42}});
            press.detail = letter;
            press.state = alt | XCB_MOD_MASK_SHIFT;
            process();
            require(keyboard.callback() == 44, "shifted keysym override failed");
            press.detail = letter;
            press.state = alt;
            process();
            require(keyboard.callback() == 40, "dynamic Alt binding failed");
            keyboard.reload();
            process();
            require(keyboard.callback() == 40, "dynamic Alt binding lost on reload");
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
            press.state = XCB_MOD_MASK_2 | XCB_MOD_MASK_SHIFT;
            process();
            require(keyboard.callback() == 44, "shifted override lost after remapping");
            press.state = XCB_MOD_MASK_2;
            process();
            require(keyboard.callback() == 40, "dynamic Alt binding did not follow mapping");
            press.state = alt;
            process();
            require(!keyboard.callback(), "dynamic Alt binding retained old mapping");
            reply = xcb_set_modifier_mapping_reply(connection,
                xcb_set_modifier_mapping(connection, count, keycodes), nullptr);
            require(reply && reply->status == XCB_MAPPING_STATUS_SUCCESS, "modifier mapping restore failed");
            std::free(reply);
            std::free(mapping);
            keyboard.reload();
            press.detail = letter;
            press.state = XCB_MOD_MASK_CONTROL;
            process();
            require(keyboard.callback() == 42, "custom binding failed");
            keyboard.reload();
            process();
            require(keyboard.callback() == 42, "custom binding lost on reload");
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
            process();
            require(keyboard.callback() == 42, "failed replacement lost binding");
            error = xcb_request_check(other, xcb_grab_key_checked(other, 1, screen->root,
                XCB_MOD_MASK_CONTROL, letter, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            require(error && error->error_code == XCB_ACCESS, "failed replacement lost old grab");
            std::free(error);
            xcb_disconnect(other);
            keyboard.replace_bindings({});
            process();
            require(!keyboard.callback(), "binding removal failed");
            press.detail = digits[9];
            press.response_type = XCB_KEY_RELEASE;
            press.state = alt;
            process();
            require(!keyboard.callback(), "key release triggered binding");
            keyboard.reload();
            press.response_type = XCB_KEY_PRESS;
            process();
            require(!keyboard.callback(), "default binding appeared after reload");
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
            auto process = [&] { keyboard.process(reinterpret_cast<const xcb_generic_event_t&>(press)); };
            press.state = alt;
            process();
            require(!keyboard.callback(), "unexpected default digit binding");
            auto* other = xcb_connect(nullptr, nullptr);
            require(other && !xcb_connection_has_error(other), "second X connection failed");
            auto mods = static_cast<std::uint16_t>(alt | XCB_MOD_MASK_SHIFT);
            auto* error = xcb_request_check(other, xcb_grab_key_checked(other, 1, screen->root,
                mods, press.detail, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
            require(!error, "unconfigured shifted digit was grabbed");
            xcb_ungrab_key(other, press.detail, screen->root, mods);
            xcb_disconnect(other);
            keyboard.replace_bindings({{XKB_KEY_1, alt, 51}});
            press.state = alt | XCB_MOD_MASK_SHIFT;
            process();
            require(keyboard.callback() == 51, "configured shifted digit failed");
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
            auto process = [&] { keyboard.process(reinterpret_cast<const xcb_generic_event_t&>(press)); };
            press.state = alt;
            process();
            require(!keyboard.callback(), "digit matched without level modifier");
            keyboard.replace_bindings({{XKB_KEY_1, alt, 52}});
            press.state = alt | level;
            process();
            require(keyboard.callback() == 52, "configured level modifier mapping failed");
            press.state = alt | level | XCB_MOD_MASK_CONTROL;
            process();
            require(!keyboard.callback(), "unexpected level modifier binding");
        }
        xcb_disconnect(connection);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (connection) xcb_disconnect(connection);
        return 1;
    }
}
