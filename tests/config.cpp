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
};

void workspace(void *data, unsigned digit) {
    auto &host = *static_cast<Host *>(data);
    if (host.fail) throw std::runtime_error("failure");
    if (host.reenter) {
        check(!host.config->load(host.path));
        check(host.config->run_workspace(1) == mortar::Config::Result::failure);
        check(host.config->run_workspace(9) == mortar::Config::Result::failure);
    }
    host.workspace = digit;
}

void move(void *data, unsigned digit) { static_cast<Host *>(data)->move = digit; }

int main() {
    char path[] = "/tmp/mortar-config-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0);
    close(fd);
    auto write = [&](const char *text) { std::ofstream file(path); file << text; check(file.good()); };
    Host host;
    mortar::Config config(&host, workspace, move);
    host.config = &config;
    host.path = path;
    using Result = mortar::Config::Result;
    check(config.run_workspace(1) == Result::absent);
    check(config.run_workspace(10) == Result::failure);
    write("(bind-workspace 1 (lambda () (workspace 4)))\n"
          "(bind-move-workspace 2 (lambda () (move-to-workspace 7)))\n"
          "(bind-workspace 3 (lambda () (error \"failure\")))\n");
    check(config.load(path));
    scm_gc();
    check(config.run_workspace(1) == Result::success && host.workspace == 4);
    host.reenter = true;
    check(config.run_workspace(1) == Result::success);
    host.reenter = false;
    check(config.run_move_workspace(2) == Result::success && host.move == 7);
    check(config.run_workspace(3) == Result::failure);
    host.fail = true;
    check(config.run_workspace(1) == Result::failure);
    host.fail = false;
    write("(bind-workspace 1 (lambda () (workspace 8))) (error \"bad\")");
    check(!config.load(path));
    check(config.run_workspace(1) == Result::success && host.workspace == 4);
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
    write("#t");
    check(config.load(path));
    check(config.run_workspace(1) == Result::absent);
    check(!config.load("/nonexistent/mortar-config"));
    std::remove(path);
}
