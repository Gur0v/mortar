#include "../mortar/config.hpp"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

void check(bool value) {
    if (!value) std::abort();
}

struct Host {
    unsigned workspace = 99;
    unsigned move = 99;
    std::string workspace_name;
    bool fail = false;
    mortar::Config *config = nullptr;
    const char *path = nullptr;
    bool reenter = false;
    mortar::Split split = mortar::Split::horizontal;
    mortar::Direction direction = mortar::Direction::left;
    bool closed = false;
    std::uint32_t target = 0;
    std::vector<mortar::Config::WindowAction> window_actions;
    std::vector<mortar::Config::WorkspaceNavigation> navigation;
    std::vector<std::string> arguments;
};

void workspace(void *data, unsigned digit) {
    auto &host = *static_cast<Host *>(data);
    if (host.fail) throw std::runtime_error("failure");
    if (host.reenter) {
        check(!host.config->load(host.path));
        check(host.config->run_workspace(1) == mortar::Config::Result::failure);
        check(host.config->run_workspace(9) == mortar::Config::Result::failure);
        check(!host.config->evaluate("1").success);
    }
    host.workspace = digit;
}

void move(void *data, unsigned digit) { static_cast<Host *>(data)->move = digit; }

void split(void *data, mortar::Split value) { static_cast<Host *>(data)->split = value; }
void focus(void *data, mortar::Direction value) { static_cast<Host *>(data)->direction = value; }
void close_action(void *data, std::uint32_t target) { auto &host = *static_cast<Host *>(data); host.closed = true; host.target = target; }
void spawn(void *data, const std::vector<std::string> &args) {
    auto &host = *static_cast<Host *>(data);
    if (host.fail) throw std::runtime_error("failure");
    host.arguments = args;
}

void window_action(void *data, mortar::Config::WindowAction action, std::uint32_t target) {
    auto &host = *static_cast<Host *>(data);
    if (host.fail) throw std::runtime_error("failure");
    host.window_actions.push_back(action);
    host.target = target;
}

void navigate(void *data, mortar::Config::WorkspaceNavigation action) {
    auto &host = *static_cast<Host *>(data);
    if (host.fail) throw std::runtime_error("failure");
    host.navigation.push_back(action);
}

int main() {
    char path[] = "/tmp/mortar-config-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0);
    close(fd);
    auto write = [&](const char *text) { std::ofstream file(path); file << text; check(file.good()); };
    Host host;
    mortar::Config config(&host, workspace, move, split, focus, close_action, spawn, nullptr, nullptr, window_action, navigate);
    host.config = &config;
    host.path = path;
    using Result = mortar::Config::Result;
    check(config.bar_status().empty());
    check(!config.evaluate("1").success);
    check(config.gaps().inner == 0 && config.gaps().outer == 0);
    write("(gaps 5 9)");
    check(config.stage(path) && config.gaps().inner == 0);
    config.commit();
    check(config.gaps().inner == 5 && config.gaps().outer == 9);
    for (const char *invalid : {"(gaps -1 0)", "(gaps 0 65536)", "(gaps 1 2) (error \"bad\")"}) {
        write(invalid);
        check(!config.load(path) && config.gaps().inner == 5 && config.gaps().outer == 9);
    }
    check(!config.evaluate("(gaps 1 2)").success);
    write("#t");
    check(config.load(path) && config.gaps().inner == 0 && config.gaps().outer == 0);
    check(!config.mouse_modifier());
    write("(floating-modifier 64)");
    check(config.stage(path) && config.staged_mouse_modifier() == 64 && !config.mouse_modifier());
    config.commit();
    check(config.mouse_modifier() == 64);
    for (const char *invalid : {"(floating-modifier -1)", "(floating-modifier 256)", "(floating-modifier 'alt)", "(floating-modifier 8) (error \"bad\")"}) {
        write(invalid);
        check(!config.load(path) && config.mouse_modifier() == 64);
    }
    check(!config.evaluate("(floating-modifier 8)").success);
    write("(floating-modifier 0)");
    check(config.load(path) && config.mouse_modifier() == 0);
    write("#t");
    check(config.load(path) && !config.mouse_modifier());
    check(config.bar().bottom && config.bar().font == "monospace 10" &&
          config.bar().background == 0x000000 && config.bar().foreground == 0xffffff &&
          config.bar().focused == 0x285577 && config.bar().unfocused == 0x333333);
    check(config.bar().workspace_buttons && !config.bar().wrap_scroll);
    check(config.bar().height == 0 && config.bar().status_padding == 1 && config.bar().status_edge_padding == 3);
    write("(bar-height 40) (bar-status-padding 4) (bar-status-edge-padding 12) (bar-workspace-buttons #f) (bar-wrap-scroll #t) (bar-mode 'hide) (bar-position 'top) (bar-font \"sans 12\") (bar-colors 1 2 3 4) (bar-output \"left\") (bar-output \"right\")");
    check(config.stage(path) && config.staged_bar().hidden && !config.bar().hidden && !config.staged_bar().bottom && config.staged_bar().font == "sans 12" &&
          config.staged_bar().background == 1 && config.staged_bar().foreground == 2 &&
          config.staged_bar().focused == 3 && config.staged_bar().unfocused == 4 && config.bar().bottom);
    check(config.staged_bar().outputs == std::vector<std::string>({"left", "right"}) && config.bar().outputs.empty());
    check(config.staged_bar().height == 40 && config.staged_bar().status_padding == 4 && config.staged_bar().status_edge_padding == 12 && config.bar().height == 0);
    check(!config.staged_bar().workspace_buttons && config.staged_bar().wrap_scroll && config.bar().workspace_buttons);
    config.discard();
    check(config.bar().bottom && config.staged_bar().bottom && config.bar().outputs.empty());
    check(config.load(path) && !config.bar().bottom && config.bar().font == "sans 12");
    check(config.bar().outputs == std::vector<std::string>({"left", "right"}));
    check(!config.bar().workspace_buttons && config.bar().wrap_scroll);
    for (const char *invalid : {"(bar-height 0)", "(bar-height 65536)", "(bar-status-padding -1)", "(bar-status-padding 32768)", "(bar-status-edge-padding 65536)", "(bar-workspace-buttons 1)", "(bar-wrap-scroll 'yes)", "(bar-mode 'show)", "(bar-output \"\")", "(bar-output 1)", "(bar-output (string #\\nul))", "(bar-position 'left)", "(bar-font \"\")", "(bar-font 1)",
                                "(bar-font (string #\\nul))", "(bar-colors 0 0 0 #x1000000)",
                                "(bar-colors 0 -1 0 0)", "(bar-position 'top) (error \"bad\")"}) {
        write(invalid);
        check(!config.load(path) && !config.bar().bottom && config.bar().font == "sans 12");
    }
    check(!config.evaluate("(bar-height 30)").success);
    check(!config.evaluate("(bar-position 'top)").success);
    check(!config.evaluate("(bar-wrap-scroll #f)").success);
    write("#t");
    check(config.load(path) && config.bar().bottom && config.bar().font == "monospace 10");
    write("(window-rule \"Editor\" 2 (quote floating))");
    check(config.load(path));
    check(config.window_rules().size() == 1 && config.window_rules()[0].workspace == 2 &&
          config.window_rules()[0].floating == true);
    write("(window-rule \"Other\" #f (quote tiled)) (error \"bad\")");
    check(!config.load(path) && config.window_rules().size() == 1 &&
          config.window_rules()[0].window_class == "Editor");
    write("(window-rule \"Other\" #f (quote tiled))");
    check(config.stage(path) && config.window_rules()[0].window_class == "Editor");
    config.commit();
    check(config.window_rules().size() == 1 && !config.window_rules()[0].workspace &&
          config.window_rules()[0].floating == false);
    write("(window-rule \"Bad\" #f #f)");
    check(!config.load(path) && config.window_rules()[0].window_class == "Other");
    write("(window-rule #f 3 'floating \"instance\" \"title\")");
    check(config.stage(path) && config.window_rules()[0].window_class == "Other");
    config.discard();
    check(config.window_rules()[0].window_class == "Other");
    check(config.load(path) && config.window_rules().size() == 1 &&
          !config.window_rules()[0].window_class && config.window_rules()[0].instance == "instance" &&
          config.window_rules()[0].title == "title");
    for (const char *invalid : {"(window-rule #f 1 #f)", "(window-rule #f 1 #f #f #f)",
                                "(window-rule #f 1 #f 1)", "(window-rule #f 1 #f \"\")",
                                "(window-rule #f 1 #f #f (string #\\nul))"}) {
        write(invalid);
        check(!config.load(path) && config.window_rules()[0].title == "title");
    }
    check(config.run_workspace(1) == Result::absent);
    check(config.run_workspace(10) == Result::failure);
    write("(bind-workspace 1 (lambda () (workspace 4)))\n"
          "(bind-move-workspace 2 (lambda () (move-to-workspace 7)))\n"
          "(bind-workspace 3 (lambda () (error \"failure\")))\n"
          "(bind-key 65 8 (lambda () (workspace 2)))\n"
          "(bind-key 65 8 (lambda () (workspace 5)))\n"
          "(bind-key 66 9 (lambda () (error \"failure\")))\n");
    check(config.load(path));
    auto evaluation = config.evaluate("(begin (define evaluation-value 42) (+ evaluation-value 1))");
    check(evaluation.success && evaluation.text == "43");
    check(config.evaluate("evaluation-value").text == "42");
    check(config.evaluate("(workspace 6)").success && host.workspace == 6);
    config.workspace_names(
        [](void *data, unsigned digit, const std::string &name) { auto &h = *static_cast<Host *>(data); h.workspace = digit; h.workspace_name = name; },
        [](void *data, const std::string &name) { static_cast<Host *>(data)->workspace_name = name; });
    check(config.evaluate("(rename-workspace 0 \"web\")").success && host.workspace == 0 && host.workspace_name == "web");
    check(config.evaluate("(workspace \"web\")").success && host.workspace_name == "web");
    check(!config.evaluate("(rename-workspace 10 \"bad\")").success);
    check(!config.evaluate("(workspace \"\")").success);
    check(!config.evaluate("(workspace (string #\\a #\\nul))").success);
    check(!config.evaluate("(bind-key 70 0 (lambda () #t))").success);
    check(!config.evaluate("(error \"evaluation failure\")").success);
    check(!config.evaluate("(").success);
    check(!config.evaluate("(make-string 10000 #\\x)").success);
    check(config.evaluate("42").success);
    check(config.evaluate("(begin (use-modules (srfi srfi-9) (srfi srfi-9 gnu)) (define saved-port #f) "
                          "(define-record-type <capture> (make-capture) capture?) "
                          "(set-record-type-printer! <capture> "
                          "(lambda (value port) (set! saved-port port) (display \"ok\" port))) "
                          "(make-capture))").success);
    check(!config.evaluate("(begin (display \"bad\" saved-port) (force-output saved-port))").success);
    scm_gc();
    check(config.bindings().size() == 2);
    check(config.bindings()[0].keysym == 65 && config.bindings()[0].modifiers == 8);
    check(config.run_binding(config.bindings()[0].callback) == Result::success && host.workspace == 5);
    check(config.run_binding(config.bindings()[1].callback) == Result::failure);
    check(config.run_binding(2) == Result::absent);
    check(config.run_workspace(1) == Result::success && host.workspace == 4);
    host.reenter = true;
    check(config.run_workspace(1) == Result::success);
    host.reenter = false;
    check(config.run_move_workspace(2) == Result::success && host.move == 7);
    check(config.run_workspace(3) == Result::failure);
    host.fail = true;
    check(config.run_workspace(1) == Result::failure);
    host.fail = false;
    write("(bind-workspace 1 (lambda () (workspace 8))) "
          "(bind-key 67 0 (lambda () (workspace 8))) (error \"bad\")");
    check(!config.load(path));
    scm_gc();
    check(config.bindings().size() == 2 && config.bindings()[0].keysym == 65);
    check(config.run_binding(0) == Result::success && host.workspace == 5);
    check(config.run_workspace(1) == Result::success && host.workspace == 4);
    write("(bind-key 67 0 (lambda () (workspace 8)))");
    check(config.stage(path));
    scm_gc();
    check(config.staged_bindings().size() == 1 && config.staged_bindings()[0].keysym == 67);
    check(config.run_binding(0) == Result::success && host.workspace == 5);
    config.discard();
    check(config.run_binding(0) == Result::success && host.workspace == 5);
    check(config.stage(path));
    scm_gc();
    config.commit();
    check(config.run_binding(0) == Result::success && host.workspace == 8);
    write("(bind-workspace 1 (lambda () (workspace 4))) "
          "(bind-key 65 8 (lambda () (workspace 5)))");
    check(config.load(path));
    check(config.run_workspace(1) == Result::success && host.workspace == 4);
    write("(bind-key 0 0 (lambda () #t))");
    check(!config.load(path));
    write("(bind-key 65 65536 (lambda () #t))");
    check(!config.load(path));
    write("(bind-key 65 0 42)");
    check(!config.load(path));
    write("(bind-key '(alt shift j) (lambda () (workspace 6))) "
          "(bind-key '(control mod4 Return) (lambda () #t))");
    check(config.load(path) && config.bindings().size() == 2);
    check(config.bindings()[0].keysym == 106 && config.bindings()[0].modifiers == 1 &&
          config.bindings()[0].dynamic_alt);
    check(config.run_binding(0) == Result::success && host.workspace == 6);
    check(config.bindings()[1].keysym == 0xff0d && config.bindings()[1].modifiers == 68);
    for (const char *invalid : {"(bind-key '() (lambda () #t))", "(bind-key '(alt) (lambda () #t))",
                                "(bind-key '(j k) (lambda () #t))", "(bind-key '(not-a-keysym) (lambda () #t))",
                                "(bind-key '(alt 5) (lambda () #t))", "(bind-key '(alt . j) (lambda () #t))"}) {
        write(invalid);
        check(!config.load(path) && config.bindings().size() == 2);
    }
    write("(bind-workspace 1 (lambda () (workspace 4))) (bind-key 65 8 (lambda () (workspace 5)))");
    check(config.load(path));
    host.workspace = 4;
    write("(workspace 8)");
    check(!config.load(path) && host.workspace == 4);
    write("(move-to-workspace 8)");
    check(!config.load(path) && host.move == 7);
    check(config.run_workspace(1) == Result::success && host.workspace == 4);
    write("(bind-workspace 10 (lambda () #t))");
    check(!config.load(path));
    write("(bind-workspace 1 42)");
    check(!config.load(path));
    write("(throw 'misc-error)");
    check(!config.load(path));
    check(config.run_workspace(1) == Result::success && host.workspace == 4);
    write("(bind-key 1 0 (lambda () (split 'vertical) (focus 'down) (close) (spawn \"printf\" \"a b\" \"\"))) "
          "(bind-key 2 0 (lambda () (spawn 5))) "
          "(bind-key 3 0 (lambda () (spawn \"\"))) "
          "(bind-key 4 0 (lambda () (split 'bad))) "
          "(bind-key 5 0 (lambda () (focus 'bad))) "
          "(bind-key 6 0 (lambda () (spawn \"ok\" (string #\\nul))))");
    check(config.load(path));
    check(config.run_binding(0) == Result::success);
    check(host.split == mortar::Split::vertical && host.direction == mortar::Direction::down && host.closed);
    check(host.arguments == std::vector<std::string>({"printf", "a b", ""}));
    for (unsigned i = 1; i < 6; ++i) check(config.run_binding(i) == Result::failure);
    host.fail = true;
    check(config.run_binding(0) == Result::failure);
    host.fail = false;
    write("(split 'horizontal)");
    check(!config.load(path));
    write("(focus 'left)");
    check(!config.load(path));
    write("(close)");
    check(!config.load(path));
    write("(spawn \"true\")");
    check(!config.load(path));
    write("#t");
    check(config.load(path));
    check(config.run_workspace(1) == Result::absent);
    check(config.bindings().empty() && config.run_binding(0) == Result::absent);
    check(!config.load("/nonexistent/mortar-config"));
    write("(bar-status \"program\" \"a b\" \"\")");
    check(config.load(path));
    check(config.bar_status() == std::vector<std::string>({"program", "a b", ""}));
    write("(bar-status \"other\") (error \"bad\")");
    check(!config.load(path) && config.bar_status()[0] == "program");
    write("(bar-status \"other\")");
    check(config.stage(path) && config.bar_status()[0] == "program");
    config.discard();
    check(config.bar_status()[0] == "program");
    write("(bar-status \"\")");
    check(!config.load(path));
    write("(bar-status 42)");
    check(!config.load(path));
    check(!config.evaluate("(bar-status \"bad\")").success);
    write("#t");
    check(config.load(path) && config.bar_status().empty());
    check(config.border().width == 0 && config.border().focused == 0x4c7899 &&
          config.border().unfocused == 0x333333);
    write("(border 0 #x123456 #xabcdef) (bar-status \"kept\")");
    check(config.stage(path));
    check(config.staged_border().width == 0 && config.staged_border().focused == 0x123456 &&
          config.staged_border().unfocused == 0xabcdef && config.border().width == 0);
    config.discard();
    check(config.border().width == 0 && config.staged_border().width == 0);
    check(config.load(path));
    check(config.border().width == 0 && config.bar_status()[0] == "kept");
    write("(border 65535 0 #xffffff) (error \"bad\")");
    check(!config.load(path) && config.border().focused == 0x123456 && config.bar_status()[0] == "kept");
    write("(border 65535 0 #xffffff)");
    check(config.stage(path) && config.staged_border().width == 65535);
    config.commit();
    check(config.border().width == 65535 && config.border().focused == 0 &&
          config.border().unfocused == 0xffffff);
    for (const char *invalid : {"(border -1 0 0)", "(border 65536 0 0)",
                                "(border 1 -1 0)", "(border 1 0 #x1000000)",
                                "(border 1 0 #f)"}) {
        write(invalid);
        check(!config.load(path) && config.border().width == 65535);
    }
    check(!config.evaluate("(border 1 0 0)").success);
    write("(bind-key 1 0 (lambda () (toggle-split) (floating-toggle) (scratchpad-move) (scratchpad-show) (fullscreen-toggle)))");
    check(config.load(path) && config.run_binding(0) == Result::success);
    check(host.window_actions == std::vector<mortar::Config::WindowAction>({
        mortar::Config::WindowAction::toggle_split, mortar::Config::WindowAction::floating, mortar::Config::WindowAction::scratch_move,
        mortar::Config::WindowAction::scratch_show, mortar::Config::WindowAction::fullscreen}));
    host.fail = true;
    check(config.run_binding(0) == Result::failure);
    host.fail = false;
    for (const char *action : {"close", "floating-toggle", "fullscreen-toggle"}) {
        check(config.evaluate(std::string("(") + action + " 42)").success && host.target == 42);
        for (const char *value : {"0", "-1", "4294967296", "#f", "'bad"})
            check(!config.evaluate(std::string("(") + action + " " + value + ")").success);
    }
    for (const char *action : {"(toggle-split)", "(floating-toggle)", "(scratchpad-move)", "(scratchpad-show)", "(fullscreen-toggle)", "(exit)"}) {
        write(action);
        check(!config.load(path));
        check(config.evaluate(action).success);
    }
    check(host.window_actions.back() == mortar::Config::WindowAction::exit);
    write("(bind-key 1 0 (lambda () (exit)))");
    check(config.load(path) && config.run_binding(0) == Result::success);
    check(host.window_actions.back() == mortar::Config::WindowAction::exit);
    for (const char *action : {"(focus-output-next)", "(focus-output-previous)", "(move-to-output-next)", "(move-to-output-previous)"}) {
        write(action);
        check(!config.load(path));
        check(config.evaluate(action).success);
    }
    using WindowAction = mortar::Config::WindowAction;
    check(std::vector<WindowAction>(host.window_actions.end() - 4, host.window_actions.end()) ==
          std::vector<WindowAction>({WindowAction::focus_output_next, WindowAction::focus_output_previous,
                                     WindowAction::move_output_next, WindowAction::move_output_previous}));
    host.fail = true;
    check(!config.evaluate("(focus-output-next)").success);
    host.fail = false;
    for (const char *action : {"(workspace-next)", "(workspace-previous)", "(workspace-back-and-forth)"}) {
        write(action);
        check(!config.load(path));
        check(config.evaluate(action).success);
    }
    using Navigation = mortar::Config::WorkspaceNavigation;
    check(host.navigation == std::vector<Navigation>({Navigation::next, Navigation::previous, Navigation::back_and_forth}));
    host.fail = true;
    check(!config.evaluate("(workspace-next)").success);
    host.fail = false;
    write("#t");
    check(config.load(path) && config.border().width == 0);
    std::remove(path);
}
