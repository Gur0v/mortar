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
    bool fail = false;
    mortar::Config *config = nullptr;
    const char *path = nullptr;
    bool reenter = false;
    mortar::Split split = mortar::Split::horizontal;
    mortar::Direction direction = mortar::Direction::left;
    bool closed = false;
    std::vector<mortar::Config::WindowAction> window_actions;
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
void close_action(void *data) { static_cast<Host *>(data)->closed = true; }
void spawn(void *data, const std::vector<std::string> &args) {
    auto &host = *static_cast<Host *>(data);
    if (host.fail) throw std::runtime_error("failure");
    host.arguments = args;
}

void window_action(void *data, mortar::Config::WindowAction action) {
    auto &host = *static_cast<Host *>(data);
    if (host.fail) throw std::runtime_error("failure");
    host.window_actions.push_back(action);
}

int main() {
    char path[] = "/tmp/mortar-config-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0);
    close(fd);
    auto write = [&](const char *text) { std::ofstream file(path); file << text; check(file.good()); };
    Host host;
    mortar::Config config(&host, workspace, move, split, focus, close_action, spawn, nullptr, nullptr, window_action);
    host.config = &config;
    host.path = path;
    using Result = mortar::Config::Result;
    check(config.bar_status().empty());
    check(!config.evaluate("1").success);
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
    write("(bind-key 1 0 (lambda () (floating-toggle) (scratchpad-move) (scratchpad-show) (fullscreen-toggle)))");
    check(config.load(path) && config.run_binding(0) == Result::success);
    check(host.window_actions == std::vector<mortar::Config::WindowAction>({
        mortar::Config::WindowAction::floating, mortar::Config::WindowAction::scratch_move,
        mortar::Config::WindowAction::scratch_show, mortar::Config::WindowAction::fullscreen}));
    host.fail = true;
    check(config.run_binding(0) == Result::failure);
    host.fail = false;
    for (const char *action : {"(floating-toggle)", "(scratchpad-move)", "(scratchpad-show)", "(fullscreen-toggle)"}) {
        write(action);
        check(!config.load(path));
        check(config.evaluate(action).success);
    }
    write("#t");
    check(config.load(path) && config.border().width == 0);
    std::remove(path);
}
