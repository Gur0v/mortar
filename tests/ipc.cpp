#include "../mortar/ipc.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

void check(bool value) {
    if (!value) std::abort();
}

int main() {
    char directory[] = "/tmp/mortar-ipc-test-XXXXXX";
    check(mkdtemp(directory));
    std::string path;
    {
        mortar::Ipc ipc(std::string(directory) + "/socket");
        path = ipc.path();
        int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        check(client >= 0);
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, path.c_str());
        check(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        std::string received;
        auto handler = [&](int fd, std::string_view frame) {
            received += frame;
            check(ipc.send(fd, "reply"));
        };
        auto step = [&] {
            auto fds = ipc.poll_fds();
            check(poll(fds.data(), fds.size(), 1000) > 0);
            ipc.dispatch(fds, handler);
        };
        step();
        const char first[] = {0, 0, 0};
        check(write(client, first, sizeof(first)) == sizeof(first));
        step();
        check(received.empty());
        const char second[] = {3, 'a', 'b', 'c'};
        check(write(client, second, sizeof(second)) == sizeof(second));
        step();
        step();
        check(received == "abc");
        check(shutdown(client, SHUT_WR) == 0);
        step();
        char reply[9];
        check(read(client, reply, sizeof(reply)) == sizeof(reply));
        check(std::string(reply, sizeof(reply)) == std::string("\0\0\0\5reply", 9));
        check(read(client, reply, sizeof(reply)) == 0);
        close(client);
    }
    {
        mortar::Ipc ipc(std::string(directory) + "/socket");
        int clients[2];
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, ipc.path().c_str());
        for (int& client : clients) {
            client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            check(client >= 0);
            check(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        }
        auto fds = ipc.poll_fds();
        check(poll(fds.data(), fds.size(), 1000) > 0);
        ipc.dispatch(fds, [](int, std::string_view) {});
        const char frames[] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int client : clients) check(write(client, frames, sizeof(frames)) == sizeof(frames));
        int count = 0;
        int previous = -1;
        for (int i = 0; i < 2; ++i) {
            fds = ipc.poll_fds();
            check(poll(fds.data(), fds.size(), 1000) > 0);
            int before = count;
            ipc.dispatch(fds, [&](int fd, std::string_view) {
                check(fd != previous);
                previous = fd;
                ++count;
            });
            check(count == before + 1);
        }
        for (int client : clients) close(client);
    }
    struct stat st {};
    check(lstat(path.c_str(), &st) < 0);
    {
        std::string existing = std::string(directory) + "/existing";
        FILE* file = std::fopen(existing.c_str(), "w");
        check(file != nullptr);
        check(std::fclose(file) == 0);
        bool rejected = false;
        try { mortar::Ipc ipc(existing); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected);
        check(lstat(existing.c_str(), &st) == 0 && S_ISREG(st.st_mode));
        check(unlink(existing.c_str()) == 0);
    }
    {
        mortar::Ipc ipc(path);
        bool rejected = false;
        try { mortar::Ipc duplicate(path); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected);
        check(lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode));
        check(unlink(path.c_str()) == 0);
        FILE* replacement = std::fopen(path.c_str(), "w");
        check(replacement != nullptr);
        check(std::fclose(replacement) == 0);
    }
    check(lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
    check(unlink(path.c_str()) == 0);
    check(chmod(directory, 0755) == 0);
    bool rejected = false;
    try { mortar::Ipc ipc(std::string(directory) + "/socket"); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected);
    check(rmdir(directory) == 0);
}
