#include "config.hpp"

#include <utility>

#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-volatile"
#endif

namespace mortar {

struct ConfigRuntime {
    Config *config;
    std::array<SCM, 10> workspace;
    std::array<SCM, 10> move;
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

    static SCM load_body(void *data) {
        auto *runtime = static_cast<ConfigRuntime *>(data);
        SCM module = scm_c_eval_string("(make-fresh-user-module)");
        runtime->previous_module = scm_set_current_module(module);
        runtime->changed_module = true;
        scm_c_define_gsubr("bind-workspace", 2, 0, 0, reinterpret_cast<scm_t_subr>(bind_workspace));
        scm_c_define_gsubr("bind-move-workspace", 2, 0, 0, reinterpret_cast<scm_t_subr>(bind_move));
        scm_c_define_gsubr("workspace", 1, 0, 0, reinterpret_cast<scm_t_subr>(workspace_action));
        scm_c_define_gsubr("move-to-workspace", 1, 0, 0, reinterpret_cast<scm_t_subr>(move_action));
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

Config::Config(void *host, Action workspace, Action move_workspace)
    : host_(host), workspace_(workspace), move_workspace_(move_workspace) {
    scm_init_guile();
    workspace_bindings_.fill(SCM_BOOL_F);
    move_bindings_.fill(SCM_BOOL_F);
}

Config::~Config() {
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
    }
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

Config::Result Config::run_workspace(unsigned digit) {
    return digit < workspace_bindings_.size() ? run(workspace_bindings_[digit]) : Result::failure;
}

Config::Result Config::run_move_workspace(unsigned digit) {
    return digit < move_bindings_.size() ? run(move_bindings_[digit]) : Result::failure;
}

}
