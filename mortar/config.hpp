#pragma once

#include <array>
#include <libguile.h>

namespace mortar {

class Config {
public:
    using NativeActionWithoutGuile = void (*)(void *, unsigned);
    using Action = NativeActionWithoutGuile;
    enum class Result { success, absent, failure };

    Config(void *host, Action workspace, Action move_workspace);
    ~Config();
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    bool load(const char *path);
    Result run_workspace(unsigned digit);
    Result run_move_workspace(unsigned digit);

private:
    Result run(SCM callback);
    void *host_;
    Action workspace_;
    Action move_workspace_;
    std::array<SCM, 10> workspace_bindings_;
    std::array<SCM, 10> move_bindings_;

    friend struct ConfigRuntime;
};

}
