#pragma once

#include "layout.hpp"

#include <array>
#include <cstdint>
#include <libguile.h>
#include <string>
#include <vector>

namespace mortar {

class Config {
public:
    using NativeActionWithoutGuile = void (*)(void *, unsigned);
    using Action = NativeActionWithoutGuile;
    enum class Result { success, absent, failure };
    struct Binding {
        std::uint32_t keysym;
        std::uint16_t modifiers;
        std::uint32_t callback;
    };

    using SplitAction = void (*)(void *, Split);
    using FocusAction = void (*)(void *, Direction);
    using CloseAction = void (*)(void *);
    using SpawnAction = void (*)(void *, const std::vector<std::string> &);

    Config(void *host, Action workspace, Action move_workspace, SplitAction split = nullptr,
           FocusAction focus = nullptr, CloseAction close = nullptr, SpawnAction spawn = nullptr);
    ~Config();
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    bool load(const char *path);
    Result run_workspace(unsigned digit);
    Result run_move_workspace(unsigned digit);
    const std::vector<Binding> &bindings() const noexcept { return bindings_; }
    Result run_binding(std::uint32_t callback);

private:
    Result run(SCM callback);
    void *host_;
    Action workspace_;
    Action move_workspace_;
    SplitAction split_;
    FocusAction focus_;
    CloseAction close_;
    SpawnAction spawn_;
    std::array<SCM, 10> workspace_bindings_;
    std::array<SCM, 10> move_bindings_;
    std::vector<Binding> bindings_;
    std::vector<SCM> callbacks_;

    friend struct ConfigRuntime;
};

}
