#include "config.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-volatile"
#endif

namespace mortar {

struct ConfigRuntime {
    Config *config;
    std::array<SCM, 10> workspace;
    std::array<SCM, 10> move;
    std::vector<Config::Binding> bindings;
    std::vector<SCM> callbacks;
    std::vector<std::string> bar_status;
    Config::Border border;
    Config::Bar bar;
    std::vector<Config::WindowRule> window_rules;
    const char *path;
    SCM callback;
    SCM previous_module;
    SCM module;
    SCM output_port_value;
    const std::string *expression;
    std::string *output;
    bool changed_module;
    bool failed;

    static thread_local ConfigRuntime *active;

    static unsigned digit(SCM value) {
        if (!scm_is_integer(value) || scm_is_false(scm_geq_p(value, scm_from_int(0))) ||
            scm_is_false(scm_leq_p(value, scm_from_int(9)))) {
            scm_out_of_range("workspace digit", value);
        }
        return scm_to_uint(value);
    }

    static SCM bind(SCM value, SCM procedure, bool move_binding) {
        if (!active || !active->path) {
            scm_misc_error("workspace binding", "binding is only allowed while loading", SCM_EOL);
        }
        unsigned index = digit(value);
        if (scm_is_false(scm_procedure_p(procedure))) {
            scm_wrong_type_arg_msg("workspace binding", 2, procedure, "procedure");
        }
        auto &slot = move_binding ? active->move[index] : active->workspace[index];
        scm_gc_protect_object(procedure);
        if (!scm_is_false(slot)) {
            scm_gc_unprotect_object(slot);
        }
        slot = procedure;
        return SCM_UNSPECIFIED;
    }

    static SCM bind_key(SCM keysym, SCM modifiers, SCM procedure) {
        if (!active || !active->path) {
            scm_misc_error("bind-key", "binding is only allowed while loading", SCM_EOL);
        }
        if (!scm_is_unsigned_integer(keysym, 1, UINT32_MAX)) {
            scm_out_of_range("bind-key", keysym);
        }
        if (!scm_is_unsigned_integer(modifiers, 0, 0xff)) {
            scm_out_of_range("bind-key", modifiers);
        }
        if (scm_is_false(scm_procedure_p(procedure))) {
            scm_wrong_type_arg_msg("bind-key", 3, procedure, "procedure");
        }
        auto key = scm_to_uint32(keysym);
        auto mods = static_cast<std::uint16_t>(scm_to_uint(modifiers));
        for (std::size_t i = 0; i < active->bindings.size(); ++i) {
            if (active->bindings[i].keysym == key && active->bindings[i].modifiers == mods) {
                scm_gc_protect_object(procedure);
                scm_gc_unprotect_object(active->callbacks[i]);
                active->callbacks[i] = procedure;
                return SCM_UNSPECIFIED;
            }
        }
        bool failed = false;
        try {
            if (active->callbacks.size() >= UINT32_MAX) throw std::bad_alloc();
            active->bindings.push_back({key, mods, static_cast<std::uint32_t>(active->callbacks.size())});
            try {
                active->callbacks.push_back(procedure);
            } catch (...) {
                active->bindings.pop_back();
                throw;
            }
        } catch (...) {
            failed = true;
        }
        if (failed) scm_misc_error("bind-key", "cannot register binding", SCM_EOL);
        scm_gc_protect_object(procedure);
        return SCM_UNSPECIFIED;
    }

    static SCM bind_workspace(SCM value, SCM procedure) { return bind(value, procedure, false); }
    static SCM bind_move(SCM value, SCM procedure) { return bind(value, procedure, true); }

    static SCM action(SCM value, bool move_action) {
        if (!active) {
            scm_misc_error("workspace action", "no active configuration", SCM_EOL);
        }
        if (active->path) {
            scm_misc_error("workspace action", "action is not allowed while loading", SCM_EOL);
        }
        unsigned index = digit(value);
        Config *config = active->config;
        Config::Action function = move_action ? config->move_workspace_ : config->workspace_;
        bool failed = false;
        try {
            if (function) {
                function(config->host_, index);
            }
        } catch (...) {
            failed = true;
        }
        if (failed || !function) {
            scm_misc_error("workspace action", "native action failed", SCM_EOL);
        }
        return SCM_UNSPECIFIED;
    }

    static SCM workspace_action(SCM value) { return action(value, false); }
    static SCM move_action(SCM value) { return action(value, true); }

    static void require_action(const char *name) {
        if (!active || active->path) scm_misc_error(name, "action is only allowed in a binding", SCM_EOL);
    }

    static SCM workspace_navigation(const char *name, Config::WorkspaceNavigation action) {
        require_action(name);
        bool failed = false;
        try {
            if (active->config->workspace_navigation_) active->config->workspace_navigation_(active->config->host_, action);
            else failed = true;
        } catch (...) { failed = true; }
        if (failed) scm_misc_error(name, "native action failed", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM workspace_next() { return workspace_navigation("workspace-next", Config::WorkspaceNavigation::next); }
    static SCM workspace_previous() { return workspace_navigation("workspace-previous", Config::WorkspaceNavigation::previous); }
    static SCM workspace_back_and_forth() { return workspace_navigation("workspace-back-and-forth", Config::WorkspaceNavigation::back_and_forth); }

    static SCM split_action(SCM value) {
        require_action("split");
        Split direction;
        if (scm_is_eq(value, scm_from_utf8_symbol("horizontal"))) direction = Split::horizontal;
        else if (scm_is_eq(value, scm_from_utf8_symbol("vertical"))) direction = Split::vertical;
        else scm_wrong_type_arg_msg("split", 1, value, "horizontal or vertical symbol");
        bool failed = false;
        try {
            if (active->config->split_) active->config->split_(active->config->host_, direction);
            else failed = true;
        } catch (...) { failed = true; }
        if (failed) scm_misc_error("split", "native action failed", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static Direction direction(SCM value, const char *name) {
        if (scm_is_eq(value, scm_from_utf8_symbol("left"))) return Direction::left;
        if (scm_is_eq(value, scm_from_utf8_symbol("right"))) return Direction::right;
        if (scm_is_eq(value, scm_from_utf8_symbol("up"))) return Direction::up;
        if (scm_is_eq(value, scm_from_utf8_symbol("down"))) return Direction::down;
        scm_wrong_type_arg_msg(name, 1, value, "direction symbol");
        return Direction::left;
    }

    static SCM directional_action(SCM value, bool move) {
        const char *name = move ? "move" : "focus";
        require_action(name);
        auto dir = direction(value, name);
        bool failed = false;
        try {
            auto function = move ? active->config->move_ : active->config->focus_;
            if (function) function(active->config->host_, dir);
            else failed = true;
        } catch (...) { failed = true; }
        if (failed) scm_misc_error(name, "native action failed", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM focus_action(SCM value) { return directional_action(value, false); }
    static SCM move_direction_action(SCM value) { return directional_action(value, true); }

    static SCM resize_action(SCM value, SCM amount) {
        require_action("resize");
        auto dir = direction(value, "resize");
        if (!scm_is_signed_integer(amount, -65535, 65535) || scm_is_true(scm_zero_p(amount)))
            scm_out_of_range("resize", amount);
        int pixels = scm_to_int(amount);
        bool failed = false;
        try {
            if (active->config->resize_) active->config->resize_(active->config->host_, dir, pixels);
            else failed = true;
        } catch (...) { failed = true; }
        if (failed) scm_misc_error("resize", "native action failed", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM close_action() {
        require_action("close");
        bool failed = false;
        try {
            if (active->config->close_) active->config->close_(active->config->host_);
            else failed = true;
        } catch (...) { failed = true; }
        if (failed) scm_misc_error("close", "native action failed", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM window_action(const char *name, Config::WindowAction action) {
        require_action(name);
        bool failed = false;
        try {
            if (active->config->window_action_) active->config->window_action_(active->config->host_, action);
            else failed = true;
        } catch (...) { failed = true; }
        if (failed) scm_misc_error(name, "native action failed", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM floating_toggle() { return window_action("floating-toggle", Config::WindowAction::floating); }
    static SCM scratchpad_move() { return window_action("scratchpad-move", Config::WindowAction::scratch_move); }
    static SCM scratchpad_show() { return window_action("scratchpad-show", Config::WindowAction::scratch_show); }
    static SCM fullscreen_toggle() { return window_action("fullscreen-toggle", Config::WindowAction::fullscreen); }
    static SCM exit_action() { return window_action("exit", Config::WindowAction::exit); }
    static SCM focus_output_next() { return window_action("focus-output-next", Config::WindowAction::focus_output_next); }
    static SCM focus_output_previous() { return window_action("focus-output-previous", Config::WindowAction::focus_output_previous); }
    static SCM move_output_next() { return window_action("move-to-output-next", Config::WindowAction::move_output_next); }
    static SCM move_output_previous() { return window_action("move-to-output-previous", Config::WindowAction::move_output_previous); }

    struct SpawnArguments {
        SCM values;
        std::vector<std::string> arguments;
        bool failed;
    };

    static SCM collect_spawn(void *data) {
        auto *state = static_cast<SpawnArguments *>(data);
        for (SCM rest = state->values; scm_is_pair(rest); rest = scm_cdr(rest)) {
            SCM value = scm_car(rest);
            if (!scm_is_string(value)) scm_wrong_type_arg_msg("spawn", 0, value, "string");
            size_t length = 0;
            char *text = scm_to_utf8_stringn(value, &length);
            bool invalid = std::memchr(text, '\0', length) != nullptr;
            bool failed = false;
            try {
                if (!invalid) state->arguments.emplace_back(text, length);
            } catch (...) { failed = true; }
            std::free(text);
            if (invalid || failed) scm_misc_error("spawn", "invalid argument or allocation failure", SCM_EOL);
        }
        return SCM_UNSPECIFIED;
    }

    static SCM spawn_error(void *data, SCM, SCM) {
        static_cast<SpawnArguments *>(data)->failed = true;
        return SCM_UNSPECIFIED;
    }

    static SCM spawn_action(SCM values) {
        require_action("spawn");
        if (scm_is_null(values)) scm_misc_error("spawn", "expected executable", SCM_EOL);
        bool failed = false;
        {
            SpawnArguments state{values, {}, false};
            scm_c_catch(SCM_BOOL_T, collect_spawn, &state, spawn_error, &state, nullptr, nullptr);
            if (!state.failed && !state.arguments.front().empty()) {
                try {
                    if (active->config->spawn_) active->config->spawn_(active->config->host_, state.arguments);
                    else state.failed = true;
                } catch (...) { state.failed = true; }
            } else state.failed = true;
            failed = state.failed;
        }
        if (failed) scm_misc_error("spawn", "invalid arguments or native action failed", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM bar_status_action(SCM values) {
        if (!active || !active->path) scm_misc_error("bar-status", "only allowed while loading", SCM_EOL);
        if (scm_is_null(values)) scm_misc_error("bar-status", "expected executable", SCM_EOL);
        bool failed = false;
        {
            SpawnArguments state{values, {}, false};
            scm_c_catch(SCM_BOOL_T, collect_spawn, &state, spawn_error, &state, nullptr, nullptr);
            if (!state.failed && !state.arguments.front().empty()) active->bar_status.swap(state.arguments);
            else failed = true;
        }
        if (failed) scm_misc_error("bar-status", "invalid arguments", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM window_rule(SCM name, SCM workspace, SCM floating) {
        if (!active || !active->path) scm_misc_error("window-rule", "only allowed while loading", SCM_EOL);
        if (!scm_is_string(name)) scm_wrong_type_arg_msg("window-rule", 1, name, "string");
        if (!scm_is_false(workspace)) digit(workspace);
        if (!scm_is_false(floating) && !scm_is_eq(floating, scm_from_utf8_symbol("floating")) &&
            !scm_is_eq(floating, scm_from_utf8_symbol("tiled")))
            scm_wrong_type_arg_msg("window-rule", 3, floating, "#f, floating or tiled");
        if (scm_is_false(workspace) && scm_is_false(floating))
            scm_misc_error("window-rule", "expected an assignment", SCM_EOL);
        size_t length = 0;
        char *text = scm_to_utf8_stringn(name, &length);
        bool failed = !length || std::memchr(text, 0, length);
        try {
            if (!failed) active->window_rules.push_back({std::string(text, length),
                scm_is_false(workspace) ? std::nullopt : std::optional<unsigned>(digit(workspace)),
                scm_is_false(floating) ? std::nullopt :
                    std::optional<bool>(scm_is_eq(floating, scm_from_utf8_symbol("floating")))});
        } catch (...) { failed = true; }
        std::free(text);
        if (failed) scm_misc_error("window-rule", "invalid class or allocation failure", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM set_border(SCM width, SCM focused, SCM unfocused) {
        if (!active || !active->path) scm_misc_error("border", "only allowed while loading", SCM_EOL);
        if (!scm_is_unsigned_integer(width, 0, UINT16_MAX)) scm_out_of_range("border", width);
        if (!scm_is_unsigned_integer(focused, 0, 0xffffff)) scm_out_of_range("border", focused);
        if (!scm_is_unsigned_integer(unfocused, 0, 0xffffff)) scm_out_of_range("border", unfocused);
        active->border = {static_cast<std::uint16_t>(scm_to_uint(width)),
                          scm_to_uint32(focused), scm_to_uint32(unfocused)};
        return SCM_UNSPECIFIED;
    }

    static SCM bar_mode(SCM value) {
        if (!active || !active->path) scm_misc_error("bar-mode", "only allowed while loading", SCM_EOL);
        if (scm_is_eq(value, scm_from_utf8_symbol("dock"))) active->bar.hidden = false;
        else if (scm_is_eq(value, scm_from_utf8_symbol("hide"))) active->bar.hidden = true;
        else scm_wrong_type_arg_msg("bar-mode", 1, value, "dock or hide symbol");
        return SCM_UNSPECIFIED;
    }

    static SCM bar_workspace_buttons(SCM value) {
        if (!active || !active->path) scm_misc_error("bar-workspace-buttons", "only allowed while loading", SCM_EOL);
        if (!scm_is_bool(value)) scm_wrong_type_arg_msg("bar-workspace-buttons", 1, value, "boolean");
        active->bar.workspace_buttons = scm_is_true(value);
        return SCM_UNSPECIFIED;
    }

    static SCM bar_wrap_scroll(SCM value) {
        if (!active || !active->path) scm_misc_error("bar-wrap-scroll", "only allowed while loading", SCM_EOL);
        if (!scm_is_bool(value)) scm_wrong_type_arg_msg("bar-wrap-scroll", 1, value, "boolean");
        active->bar.wrap_scroll = scm_is_true(value);
        return SCM_UNSPECIFIED;
    }

    static SCM bar_position(SCM value) {
        if (!active || !active->path) scm_misc_error("bar-position", "only allowed while loading", SCM_EOL);
        if (scm_is_eq(value, scm_from_utf8_symbol("top"))) active->bar.bottom = false;
        else if (scm_is_eq(value, scm_from_utf8_symbol("bottom"))) active->bar.bottom = true;
        else scm_wrong_type_arg_msg("bar-position", 1, value, "top or bottom symbol");
        return SCM_UNSPECIFIED;
    }

    static SCM bar_output(SCM value) {
        if (!active || !active->path) scm_misc_error("bar-output", "only allowed while loading", SCM_EOL);
        if (!scm_is_string(value)) scm_wrong_type_arg_msg("bar-output", 1, value, "string");
        size_t length = 0;
        char *text = scm_to_utf8_stringn(value, &length);
        bool failed = !length || length > 4096 || std::memchr(text, 0, length) || active->bar.outputs.size() >= 64;
        try {
            if (!failed) active->bar.outputs.emplace_back(text, length);
        } catch (...) { failed = true; }
        std::free(text);
        if (failed) scm_misc_error("bar-output", "invalid output or allocation failure", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM bar_font(SCM value) {
        if (!active || !active->path) scm_misc_error("bar-font", "only allowed while loading", SCM_EOL);
        if (!scm_is_string(value)) scm_wrong_type_arg_msg("bar-font", 1, value, "string");
        size_t length = 0;
        char *text = scm_to_utf8_stringn(value, &length);
        bool failed = !length || length > 4096 || std::memchr(text, 0, length);
        try {
            if (!failed) active->bar.font.assign(text, length);
        } catch (...) { failed = true; }
        std::free(text);
        if (failed) scm_misc_error("bar-font", "invalid font or allocation failure", SCM_EOL);
        return SCM_UNSPECIFIED;
    }

    static SCM bar_colors(SCM background, SCM foreground, SCM focused, SCM unfocused) {
        if (!active || !active->path) scm_misc_error("bar-colors", "only allowed while loading", SCM_EOL);
        for (SCM value : {background, foreground, focused, unfocused})
            if (!scm_is_unsigned_integer(value, 0, 0xffffff)) scm_out_of_range("bar-colors", value);
        active->bar.background = scm_to_uint32(background);
        active->bar.foreground = scm_to_uint32(foreground);
        active->bar.focused = scm_to_uint32(focused);
        active->bar.unfocused = scm_to_uint32(unfocused);
        return SCM_UNSPECIFIED;
    }

    static SCM load_body(void *data) {
        auto *runtime = static_cast<ConfigRuntime *>(data);
        SCM module = scm_c_eval_string("(make-fresh-user-module)");
        runtime->module = module;
        scm_gc_protect_object(module);
        runtime->previous_module = scm_set_current_module(module);
        runtime->changed_module = true;
        scm_c_define_gsubr("bind-key", 3, 0, 0, reinterpret_cast<scm_t_subr>(bind_key));
        scm_c_define_gsubr("bind-workspace", 2, 0, 0, reinterpret_cast<scm_t_subr>(bind_workspace));
        scm_c_define_gsubr("bind-move-workspace", 2, 0, 0, reinterpret_cast<scm_t_subr>(bind_move));
        scm_c_define_gsubr("workspace", 1, 0, 0, reinterpret_cast<scm_t_subr>(workspace_action));
        scm_c_define_gsubr("workspace-next", 0, 0, 0, reinterpret_cast<scm_t_subr>(workspace_next));
        scm_c_define_gsubr("workspace-previous", 0, 0, 0, reinterpret_cast<scm_t_subr>(workspace_previous));
        scm_c_define_gsubr("workspace-back-and-forth", 0, 0, 0, reinterpret_cast<scm_t_subr>(workspace_back_and_forth));
        scm_c_define_gsubr("move-to-workspace", 1, 0, 0, reinterpret_cast<scm_t_subr>(move_action));
        scm_c_define_gsubr("split", 1, 0, 0, reinterpret_cast<scm_t_subr>(split_action));
        scm_c_define_gsubr("move", 1, 0, 0, reinterpret_cast<scm_t_subr>(move_direction_action));
        scm_c_define_gsubr("resize", 2, 0, 0, reinterpret_cast<scm_t_subr>(resize_action));
        scm_c_define_gsubr("focus", 1, 0, 0, reinterpret_cast<scm_t_subr>(focus_action));
        scm_c_define_gsubr("close", 0, 0, 0, reinterpret_cast<scm_t_subr>(close_action));
        scm_c_define_gsubr("exit", 0, 0, 0, reinterpret_cast<scm_t_subr>(exit_action));
        scm_c_define_gsubr("focus-output-next", 0, 0, 0, reinterpret_cast<scm_t_subr>(focus_output_next));
        scm_c_define_gsubr("focus-output-previous", 0, 0, 0, reinterpret_cast<scm_t_subr>(focus_output_previous));
        scm_c_define_gsubr("move-to-output-next", 0, 0, 0, reinterpret_cast<scm_t_subr>(move_output_next));
        scm_c_define_gsubr("move-to-output-previous", 0, 0, 0, reinterpret_cast<scm_t_subr>(move_output_previous));
        scm_c_define_gsubr("floating-toggle", 0, 0, 0, reinterpret_cast<scm_t_subr>(floating_toggle));
        scm_c_define_gsubr("scratchpad-move", 0, 0, 0, reinterpret_cast<scm_t_subr>(scratchpad_move));
        scm_c_define_gsubr("scratchpad-show", 0, 0, 0, reinterpret_cast<scm_t_subr>(scratchpad_show));
        scm_c_define_gsubr("fullscreen-toggle", 0, 0, 0, reinterpret_cast<scm_t_subr>(fullscreen_toggle));
        scm_c_define_gsubr("bar-status", 0, 0, 1, reinterpret_cast<scm_t_subr>(bar_status_action));
        scm_c_define_gsubr("bar-mode", 1, 0, 0, reinterpret_cast<scm_t_subr>(bar_mode));
        scm_c_define_gsubr("bar-workspace-buttons", 1, 0, 0, reinterpret_cast<scm_t_subr>(bar_workspace_buttons));
        scm_c_define_gsubr("bar-wrap-scroll", 1, 0, 0, reinterpret_cast<scm_t_subr>(bar_wrap_scroll));
        scm_c_define_gsubr("bar-position", 1, 0, 0, reinterpret_cast<scm_t_subr>(bar_position));
        scm_c_define_gsubr("bar-font", 1, 0, 0, reinterpret_cast<scm_t_subr>(bar_font));
        scm_c_define_gsubr("bar-output", 1, 0, 0, reinterpret_cast<scm_t_subr>(bar_output));
        scm_c_define_gsubr("bar-colors", 4, 0, 0, reinterpret_cast<scm_t_subr>(bar_colors));
        scm_c_define_gsubr("window-rule", 3, 0, 0, reinterpret_cast<scm_t_subr>(window_rule));
        scm_c_define_gsubr("border", 3, 0, 0, reinterpret_cast<scm_t_subr>(set_border));
        scm_c_define_gsubr("spawn", 0, 0, 1, reinterpret_cast<scm_t_subr>(spawn_action));
        return scm_c_primitive_load(runtime->path);
    }

    static size_t write_output(SCM port, SCM bytes, size_t start, size_t count) {
        if (!active || !active->output || !scm_is_eq(port, active->output_port_value))
            scm_misc_error("evaluate", "output port is no longer active", SCM_EOL);
        auto *output = active->output;
        if (count > Config::evaluation_limit - output->size()) {
            scm_misc_error("evaluate", "rendered output exceeds limit", SCM_EOL);
        }
        bool failed = false;
        try {
            output->append(reinterpret_cast<const char *>(SCM_BYTEVECTOR_CONTENTS(bytes)) + start, count);
        } catch (...) { failed = true; }
        if (failed) scm_misc_error("evaluate", "cannot render output", SCM_EOL);
        return count;
    }

    static SCM output_port() {
        static char name[] = "mortar-evaluation";
        static scm_t_port_type *type = scm_make_port_type(name, nullptr, write_output);
        return scm_c_make_port(type, SCM_WRTNG, 0);
    }

    static SCM evaluate_body(void *data) {
        auto *runtime = static_cast<ConfigRuntime *>(data);
        SCM source = scm_from_utf8_stringn(runtime->expression->data(), runtime->expression->size());
        SCM result = scm_eval_string_in_module(source, runtime->config->module_);
        SCM port = output_port();
        runtime->output_port_value = port;
        scm_write(result, port);
        scm_force_output(port);
        return SCM_UNSPECIFIED;
    }

    struct Error {
        SCM key;
        SCM arguments;
    };

    static SCM evaluate_error_body(void *data) {
        auto *error = static_cast<Error *>(data);
        SCM port = output_port();
        active->output_port_value = port;
        scm_print_exception(port, SCM_BOOL_F, error->key, error->arguments);
        scm_force_output(port);
        return SCM_UNSPECIFIED;
    }

    static SCM run_body(void *data) {
        return scm_call_0(static_cast<ConfigRuntime *>(data)->callback);
    }

    static SCM report(void *data) {
        auto *error = static_cast<Error *>(data);
        return scm_handle_by_message_noexit(nullptr, error->key, error->arguments);
    }

    static SCM ignore_report_error(void *, SCM, SCM) {
        return SCM_UNSPECIFIED;
    }

    static SCM evaluate_error(void *data, SCM key, SCM arguments) {
        auto *runtime = static_cast<ConfigRuntime *>(data);
        runtime->failed = true;
        runtime->output->clear();
        Error error{key, arguments};
        scm_c_catch(SCM_BOOL_T, evaluate_error_body, &error, ignore_report_error, nullptr, nullptr, nullptr);
        return SCM_UNSPECIFIED;
    }

    static SCM handle(void *data, SCM key, SCM arguments) {
        static_cast<ConfigRuntime *>(data)->failed = true;
        Error error{key, arguments};
        scm_c_catch(SCM_BOOL_T, report, &error, ignore_report_error, nullptr, nullptr, nullptr);
        return SCM_UNSPECIFIED;
    }
};

thread_local ConfigRuntime *ConfigRuntime::active = nullptr;

Config::Config(void *host, Action workspace, Action move_workspace, SplitAction split,
               FocusAction focus, CloseAction close, SpawnAction spawn, FocusAction move, ResizeAction resize,
               WindowActionCallback window_action, WorkspaceNavigationCallback workspace_navigation)
    : host_(host), workspace_(workspace), move_workspace_(move_workspace), split_(split),
      focus_(focus), move_(move), resize_(resize), close_(close), spawn_(spawn), window_action_(window_action),
      workspace_navigation_(workspace_navigation) {
    scm_init_guile();
    workspace_bindings_.fill(SCM_BOOL_F);
    move_bindings_.fill(SCM_BOOL_F);
}

Config::~Config() {
    discard();
    if (!scm_is_false(module_)) scm_gc_unprotect_object(module_);
    for (SCM callback : callbacks_) scm_gc_unprotect_object(callback);
    for (SCM callback : workspace_bindings_) {
        if (!scm_is_false(callback)) scm_gc_unprotect_object(callback);
    }
    for (SCM callback : move_bindings_) {
        if (!scm_is_false(callback)) scm_gc_unprotect_object(callback);
    }
}

bool Config::load(const char *path) {
    if (!stage(path)) return false;
    commit();
    return true;
}

bool Config::stage(const char *path) {
    if (!path || ConfigRuntime::active) return false;
    discard();
    staged_ = std::make_unique<ConfigRuntime>();
    auto &runtime = *staged_;
    runtime.config = this;
    runtime.module = SCM_BOOL_F;
    runtime.path = path;
    runtime.workspace.fill(SCM_BOOL_F);
    runtime.move.fill(SCM_BOOL_F);
    ConfigRuntime::active = &runtime;
    scm_c_catch(SCM_BOOL_T, ConfigRuntime::load_body, &runtime, ConfigRuntime::handle, &runtime, nullptr, nullptr);
    if (runtime.changed_module) scm_set_current_module(runtime.previous_module);
    ConfigRuntime::active = nullptr;
    if (runtime.failed) {
        discard();
        return false;
    }
    return true;
}

const std::vector<Config::Binding> &Config::staged_bindings() const noexcept {
    return staged_ ? staged_->bindings : bindings_;
}

const Config::Border &Config::staged_border() const noexcept {
    return staged_ ? staged_->border : border_;
}

const Config::Bar &Config::staged_bar() const noexcept {
    return staged_ ? staged_->bar : bar_;
}

void Config::commit() noexcept {
    if (!staged_) return;
    std::swap(module_, staged_->module);
    workspace_bindings_.swap(staged_->workspace);
    move_bindings_.swap(staged_->move);
    bindings_.swap(staged_->bindings);
    callbacks_.swap(staged_->callbacks);
    bar_status_.swap(staged_->bar_status);
    std::swap(border_, staged_->border);
    std::swap(bar_, staged_->bar);
    window_rules_.swap(staged_->window_rules);
    discard();
}

void Config::discard() noexcept {
    if (!staged_) return;
    auto &runtime = *staged_;
    if (!scm_is_false(runtime.module)) scm_gc_unprotect_object(runtime.module);
    for (SCM callback : runtime.callbacks) scm_gc_unprotect_object(callback);
    for (SCM callback : runtime.workspace) {
        if (!scm_is_false(callback)) scm_gc_unprotect_object(callback);
    }
    for (SCM callback : runtime.move) {
        if (!scm_is_false(callback)) scm_gc_unprotect_object(callback);
    }
    staged_.reset();
}

Config::Evaluation Config::evaluate(const std::string &expression) {
    if (ConfigRuntime::active) return {false, "configuration is busy"};
    if (scm_is_false(module_)) return {false, "no configuration loaded"};
    Evaluation result{true, {}};
    ConfigRuntime runtime{};
    runtime.config = this;
    runtime.expression = &expression;
    runtime.output = &result.text;
    ConfigRuntime::active = &runtime;
    scm_c_catch(SCM_BOOL_T, ConfigRuntime::evaluate_body, &runtime, ConfigRuntime::evaluate_error, &runtime, nullptr, nullptr);
    ConfigRuntime::active = nullptr;
    result.success = !runtime.failed;
    if (!result.success && result.text.empty()) result.text = "evaluation failed";
    return result;
}

Config::Result Config::run(SCM callback) {
    if (ConfigRuntime::active) return Result::failure;
    if (scm_is_false(callback)) return Result::absent;
    ConfigRuntime runtime{};
    runtime.config = this;
    runtime.callback = callback;
    ConfigRuntime::active = &runtime;
    scm_c_catch(SCM_BOOL_T, ConfigRuntime::run_body, &runtime, ConfigRuntime::handle, &runtime, nullptr, nullptr);
    ConfigRuntime::active = nullptr;
    return runtime.failed ? Result::failure : Result::success;
}

Config::Result Config::run_binding(std::uint32_t callback) {
    return callback < callbacks_.size() ? run(callbacks_[callback]) : Result::absent;
}

Config::Result Config::run_workspace(unsigned digit) {
    return digit < workspace_bindings_.size() ? run(workspace_bindings_[digit]) : Result::failure;
}

Config::Result Config::run_move_workspace(unsigned digit) {
    return digit < move_bindings_.size() ? run(move_bindings_[digit]) : Result::failure;
}

}
