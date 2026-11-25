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
    const char *path;
    SCM callback;
    SCM previous_module;
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

    static SCM focus_action(SCM value) {
        require_action("focus");
        Direction direction;
        if (scm_is_eq(value, scm_from_utf8_symbol("left"))) direction = Direction::left;
        else if (scm_is_eq(value, scm_from_utf8_symbol("right"))) direction = Direction::right;
        else if (scm_is_eq(value, scm_from_utf8_symbol("up"))) direction = Direction::up;
        else if (scm_is_eq(value, scm_from_utf8_symbol("down"))) direction = Direction::down;
        else scm_wrong_type_arg_msg("focus", 1, value, "direction symbol");
        bool failed = false;
        try {
            if (active->config->focus_) active->config->focus_(active->config->host_, direction);
            else failed = true;
        } catch (...) { failed = true; }
        if (failed) scm_misc_error("focus", "native action failed", SCM_EOL);
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

    static SCM load_body(void *data) {
        auto *runtime = static_cast<ConfigRuntime *>(data);
        SCM module = scm_c_eval_string("(make-fresh-user-module)");
        runtime->previous_module = scm_set_current_module(module);
        runtime->changed_module = true;
        scm_c_define_gsubr("bind-key", 3, 0, 0, reinterpret_cast<scm_t_subr>(bind_key));
        scm_c_define_gsubr("bind-workspace", 2, 0, 0, reinterpret_cast<scm_t_subr>(bind_workspace));
        scm_c_define_gsubr("bind-move-workspace", 2, 0, 0, reinterpret_cast<scm_t_subr>(bind_move));
        scm_c_define_gsubr("workspace", 1, 0, 0, reinterpret_cast<scm_t_subr>(workspace_action));
        scm_c_define_gsubr("move-to-workspace", 1, 0, 0, reinterpret_cast<scm_t_subr>(move_action));
        scm_c_define_gsubr("split", 1, 0, 0, reinterpret_cast<scm_t_subr>(split_action));
        scm_c_define_gsubr("focus", 1, 0, 0, reinterpret_cast<scm_t_subr>(focus_action));
        scm_c_define_gsubr("close", 0, 0, 0, reinterpret_cast<scm_t_subr>(close_action));
        scm_c_define_gsubr("spawn", 0, 0, 1, reinterpret_cast<scm_t_subr>(spawn_action));
        return scm_c_primitive_load(runtime->path);
    }

    static SCM run_body(void *data) {
        return scm_call_0(static_cast<ConfigRuntime *>(data)->callback);
    }

    struct Error {
        SCM key;
        SCM arguments;
    };

    static SCM report(void *data) {
        auto *error = static_cast<Error *>(data);
        return scm_handle_by_message_noexit(nullptr, error->key, error->arguments);
    }

    static SCM ignore_report_error(void *, SCM, SCM) {
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
               FocusAction focus, CloseAction close, SpawnAction spawn)
    : host_(host), workspace_(workspace), move_workspace_(move_workspace), split_(split),
      focus_(focus), close_(close), spawn_(spawn) {
    scm_init_guile();
    workspace_bindings_.fill(SCM_BOOL_F);
    move_bindings_.fill(SCM_BOOL_F);
}

Config::~Config() {
    for (SCM callback : callbacks_) scm_gc_unprotect_object(callback);
    for (SCM callback : workspace_bindings_) {
        if (!scm_is_false(callback)) scm_gc_unprotect_object(callback);
    }
    for (SCM callback : move_bindings_) {
        if (!scm_is_false(callback)) scm_gc_unprotect_object(callback);
    }
}

bool Config::load(const char *path) {
    if (!path || ConfigRuntime::active) return false;
    ConfigRuntime runtime{};
    runtime.config = this;
    runtime.path = path;
    runtime.workspace.fill(SCM_BOOL_F);
    runtime.move.fill(SCM_BOOL_F);
    ConfigRuntime::active = &runtime;
    scm_c_catch(SCM_BOOL_T, ConfigRuntime::load_body, &runtime, ConfigRuntime::handle, &runtime, nullptr, nullptr);
    if (runtime.changed_module) scm_set_current_module(runtime.previous_module);
    ConfigRuntime::active = nullptr;
    if (!runtime.failed) {
        workspace_bindings_.swap(runtime.workspace);
        move_bindings_.swap(runtime.move);
        bindings_.swap(runtime.bindings);
        callbacks_.swap(runtime.callbacks);
    }
    for (SCM callback : runtime.callbacks) scm_gc_unprotect_object(callback);
    for (SCM callback : runtime.workspace) {
        if (!scm_is_false(callback)) scm_gc_unprotect_object(callback);
    }
    for (SCM callback : runtime.move) {
        if (!scm_is_false(callback)) scm_gc_unprotect_object(callback);
    }
    return !runtime.failed;
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
