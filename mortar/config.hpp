#pragma once

#include "layout.hpp"

#include <array>
#include <cstdint>
#include <libguile.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mortar {

struct ConfigRuntime;
struct RuleRegex;

class Config {
public:
    using NativeActionWithoutGuile = void (*)(void *, unsigned);
    using Action = NativeActionWithoutGuile;
    enum class Result { success, absent, failure };
    struct Binding {
        std::uint32_t keysym;
        std::uint16_t modifiers;
        std::uint32_t callback;
        bool dynamic_alt = false;
    };

    struct WindowRule {
        std::optional<std::string> window_class;
        std::optional<unsigned> workspace;
        std::optional<bool> floating;
        std::optional<std::string> instance;
        std::optional<std::string> title;
        std::shared_ptr<const RuleRegex> class_regex;
        std::shared_ptr<const RuleRegex> instance_regex;
        std::shared_ptr<const RuleRegex> title_regex;

        bool matches(std::optional<std::string_view> class_name, std::optional<std::string_view> instance_name,
                     std::optional<std::string_view> window_title) const;
    };

    struct Gaps {
        std::uint16_t inner = 0;
        std::uint16_t outer = 0;
    };

    struct Border {
        std::uint16_t width = 0;
        std::uint32_t focused = 0x4c7899;
        std::uint32_t unfocused = 0x333333;
    };

    struct Bar {
        std::uint16_t height = 0;
        std::uint16_t status_padding = 1;
        std::uint16_t status_edge_padding = 3;
        bool bottom = true;
        bool hidden = false;
        bool workspace_buttons = true;
        bool wrap_scroll = false;
        std::string font = "monospace 10";
        std::vector<std::string> outputs;
        std::uint32_t background = 0x000000;
        std::uint32_t foreground = 0xffffff;
        std::uint32_t focused = 0x285577;
        std::uint32_t unfocused = 0x333333;
        bool operator==(const Bar &) const = default;
    };

    enum class WorkspaceNavigation { next, previous, back_and_forth };
    using WorkspaceNavigationCallback = void (*)(void *, WorkspaceNavigation);
    using WorkspaceNameCallback = void (*)(void *, unsigned, const std::string &);
    enum class WindowAction { toggle_split, focus_mode_toggle, floating, scratch_move, scratch_show, fullscreen, exit, focus_output_next, focus_output_previous, move_output_next, move_output_previous };
    using WindowActionCallback = void (*)(void *, WindowAction, std::uint32_t);
    using SplitAction = void (*)(void *, Split);
    using FocusAction = void (*)(void *, Direction);
    using ResizeAction = void (*)(void *, Direction, int);
    using CloseAction = void (*)(void *, std::uint32_t);
    using SpawnAction = void (*)(void *, const std::vector<std::string> &);

    Config(void *host, Action workspace, Action move_workspace, SplitAction split = nullptr,
           FocusAction focus = nullptr, CloseAction close = nullptr, SpawnAction spawn = nullptr,
           FocusAction move = nullptr, ResizeAction resize = nullptr, WindowActionCallback window_action = nullptr,
           WorkspaceNavigationCallback workspace_navigation = nullptr);
    ~Config();
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    struct Evaluation {
        bool success;
        std::string text;
    };
    static constexpr std::size_t evaluation_limit = 4096;
    void workspace_names(WorkspaceNameCallback rename, void (*switch_name)(void *, const std::string &)) noexcept {
        rename_workspace_ = rename;
        switch_workspace_name_ = switch_name;
    }
    Evaluation evaluate(const std::string &expression);
    bool load(const char *path);
    bool stage(const char *path);
    const std::vector<Binding> &staged_bindings() const noexcept;
    void commit() noexcept;
    void discard() noexcept;
    Result run_workspace(unsigned digit);
    Result run_move_workspace(unsigned digit);
    const std::vector<Binding> &bindings() const noexcept { return bindings_; }
    Result run_binding(std::uint32_t callback);
    const std::vector<std::string> &bar_status() const noexcept { return bar_status_; }
    const std::vector<WindowRule> &window_rules() const noexcept { return window_rules_; }
    const Gaps &gaps() const noexcept { return gaps_; }
    const Border &border() const noexcept { return border_; }
    const Border &staged_border() const noexcept;
    const Bar &bar() const noexcept { return bar_; }
    const Bar &staged_bar() const noexcept;
    std::optional<std::uint16_t> mouse_modifier() const noexcept { return mouse_modifier_; }
    std::optional<std::uint16_t> staged_mouse_modifier() const noexcept;

private:
    Result run(SCM callback);
    SCM module_ = SCM_BOOL_F;
    void *host_;
    Action workspace_;
    Action move_workspace_;
    SplitAction split_;
    FocusAction focus_;
    FocusAction move_;
    ResizeAction resize_;
    CloseAction close_;
    SpawnAction spawn_;
    WindowActionCallback window_action_;
    WorkspaceNavigationCallback workspace_navigation_;
    WorkspaceNameCallback rename_workspace_ = nullptr;
    void (*switch_workspace_name_)(void *, const std::string &) = nullptr;
    std::array<SCM, 10> workspace_bindings_;
    std::array<SCM, 10> move_bindings_;
    std::vector<Binding> bindings_;
    std::vector<SCM> callbacks_;
    std::vector<std::string> bar_status_;
    Gaps gaps_;
    Border border_;
    Bar bar_;
    std::optional<std::uint16_t> mouse_modifier_;
    std::vector<WindowRule> window_rules_;

    std::unique_ptr<ConfigRuntime> staged_;
    friend struct ConfigRuntime;
};

}
