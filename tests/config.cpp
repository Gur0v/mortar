#include "../mortar/config.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

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
        assert(!host.config->load(host.path));
        assert(host.config->run_workspace(1) == mortar::Config::Result::failure);
        assert(host.config->run_workspace(9) == mortar::Config::Result::failure);
    }
    host.workspace = digit;
}

void move(void *data, unsigned digit) { static_cast<Host *>(data)->move = digit; }

int main() {
    char path[] = "/tmp/mortar-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    auto write = [&](const char *text) { std::ofstream file(path); file << text; assert(file.good()); };
    Host host;
    mortar::Config config(&host, workspace, move);
    host.config = &config;
    host.path = path;
    using Result = mortar::Config::Result;
    assert(config.run_workspace(1) == Result::absent);
    assert(config.run_workspace(10) == Result::failure);
    write("(bind-workspace 1 (lambda () (workspace 4)))\n"
          "(bind-move-workspace 2 (lambda () (move-to-workspace 7)))\n"
          "(bind-workspace 3 (lambda () (error \"failure\")))\n");
    assert(config.load(path));
    scm_gc();
    assert(config.run_workspace(1) == Result::success && host.workspace == 4);
    host.reenter = true;
    assert(config.run_workspace(1) == Result::success);
    host.reenter = false;
    assert(config.run_move_workspace(2) == Result::success && host.move == 7);
    assert(config.run_workspace(3) == Result::failure);
    host.fail = true;
    assert(config.run_workspace(1) == Result::failure);
    host.fail = false;
    write("(bind-workspace 1 (lambda () (workspace 8))) (error \"bad\")");
    assert(!config.load(path));
    assert(config.run_workspace(1) == Result::success && host.workspace == 4);
    write("(workspace 8)");
    assert(!config.load(path) && host.workspace == 4);
    write("(move-to-workspace 8)");
    assert(!config.load(path) && host.move == 7);
    assert(config.run_workspace(1) == Result::success && host.workspace == 4);
    write("(bind-workspace 10 (lambda () #t))");
    assert(!config.load(path));
    write("(bind-workspace 1 42)");
    assert(!config.load(path));
    write("(throw 'misc-error)");
    assert(!config.load(path));
    assert(config.run_workspace(1) == Result::success && host.workspace == 4);
    write("#t");
    assert(config.load(path));
    assert(config.run_workspace(1) == Result::absent);
    assert(!config.load("/nonexistent/mortar-config"));
    std::remove(path);
}
