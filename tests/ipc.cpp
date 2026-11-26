#include "../mortar/ipc.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

bool fail_socket_stat = false;
bool truncate_diag = false;
bool ambiguous_device = false;
extern "C" ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
    auto result = syscall(SYS_recvmsg, fd, msg, flags);
    if (truncate_diag && result > 0) msg->msg_flags |= MSG_TRUNC;
    return result;
}

extern "C" int fstatat(int dirfd, const char* path, struct stat* st, int flags) noexcept {
    if (fail_socket_stat && std::strcmp(path, "socket") == 0) {
        errno = EIO;
        return -1;
    }
    int result = static_cast<int>(syscall(SYS_newfstatat, dirfd, path, st, flags));
    if (result == 0 && ambiguous_device && S_ISSOCK(st->st_mode)) st->st_dev = makedev(4096, 0);
    return result;
}

void check(bool value) {
    if (!value) std::abort();
}

int main(int argc, char** argv) {
    if (argc == 4) {
        mortar::Ipc::Handoff inherited {std::stoi(argv[1]), std::stoull(argv[2]), argv[3]};
        mortar::Ipc ipc(inherited);
        check((fcntl(inherited.fd, F_GETFD) & FD_CLOEXEC) != 0);
        int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, argv[3]);
        check(client >= 0 && connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        auto fds = ipc.poll_fds();
        check(poll(fds.data(), fds.size(), 1000) > 0);
        ipc.dispatch(fds, [](int, std::string_view) {});
        check(ipc.poll_fds().size() == 2);
        close(client);
        return 0;
    }
    char directory[] = "/tmp/mortar-ipc-test-XXXXXX";
    check(mkdtemp(directory));
    std::string path;
    {
        std::string target = std::string(directory) + "/socket";
        fail_socket_stat = true;
        bool rejected = false;
        try { mortar::Ipc ipc(target); } catch (const std::runtime_error&) { rejected = true; }
        fail_socket_stat = false;
        check(rejected);
        DIR* contents = opendir(directory);
        check(contents != nullptr);
        while (auto* entry = readdir(contents))
            check(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0);
        check(closedir(contents) == 0);
    }
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
    {
        mortar::Ipc ipc(path);
        int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        check(client >= 0);
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, path.c_str());
        check(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        auto fds = ipc.poll_fds();
        check(poll(fds.data(), fds.size(), 1000) > 0);
        struct rlimit original {};
        check(getrlimit(RLIMIT_NOFILE, &original) == 0);
        struct rlimit limited = original;
        limited.rlim_cur = 0;
        check(setrlimit(RLIMIT_NOFILE, &limited) == 0);
        ipc.dispatch(fds, [](int, std::string_view) {});
        check(ipc.poll_fds().empty());
        check(setrlimit(RLIMIT_NOFILE, &original) == 0);
        usleep(150000);
        fds = ipc.poll_fds();
        check(poll(fds.data(), fds.size(), 1000) > 0);
        ipc.dispatch(fds, [](int, std::string_view) {});
        check(ipc.poll_fds().size() == 2);
        close(client);
    }
    {
        mortar::Ipc ipc(path);
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, path.c_str());
        int idle = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int partial = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int subscriber = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int ordinary = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        for (int fd : {idle, partial, subscriber, ordinary}) {
            check(fd >= 0);
            check(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        }
        auto step = [&] {
            auto fds = ipc.poll_fds();
            poll(fds.data(), fds.size(), 100);
            ipc.dispatch(fds, [&](int fd, std::string_view frame) {
                if (frame == "x") ipc.subscribe(fd);
            });
        };
        step();
        const char frame[] = {0, 0, 0, 1, 'x'};
        check(write(partial, frame, 1) == 1);
        check(write(subscriber, frame, sizeof(frame)) == sizeof(frame));
        const char ordinary_frame[] = {0, 0, 0, 1, 'y'};
        check(write(ordinary, ordinary_frame, sizeof(ordinary_frame)) == sizeof(ordinary_frame));
        for (int i = 0; i < 5; ++i) step();
        usleep(5100000);
        step();
        check(ipc.poll_fds().size() == 2);
        check(ipc.send(ipc.poll_fds()[1].fd, "event"));
        step();
        char reply[9];
        check(read(subscriber, reply, sizeof(reply)) == sizeof(reply));
        check(std::string(reply, sizeof(reply)) == std::string("\0\0\0\5event", 9));
        close(idle);
        close(partial);
        close(subscriber);
        close(ordinary);
    }
    struct stat st {};
    check(lstat(path.c_str(), &st) < 0);
    {
        mortar::Ipc ipc(path);
        auto handoff = ipc.handoff();
        check((fcntl(handoff.fd, F_GETFD) & FD_CLOEXEC) == 0);
        auto invalid = handoff;
        ++invalid.inode;
        bool rejected = false;
        try { mortar::Ipc adopted(invalid); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected && fcntl(handoff.fd, F_GETFD) >= 0);
        invalid = handoff;
        invalid.fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        check(invalid.fd >= 0);
        rejected = false;
        try { mortar::Ipc adopted(invalid); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected);
        close(invalid.fd);
        for (bool* fault : {&truncate_diag, &ambiguous_device}) {
            auto uncertain = ipc.handoff();
            *fault = true;
            { mortar::Ipc adopted(uncertain); }
            *fault = false;
            check(lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode));
        }
        pid_t child = fork();
        check(child >= 0);
        if (child == 0) {
            auto fd = std::to_string(handoff.fd);
            auto inode = std::to_string(handoff.inode);
            execl("/proc/self/exe", "ipc-test", fd.c_str(), inode.c_str(), path.c_str(), nullptr);
            _exit(127);
        }
        close(handoff.fd);
        int status = 0;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        if (lstat(path.c_str(), &st) == 0) {
            check(S_ISSOCK(st.st_mode));
            check(unlink(path.c_str()) == 0);
        } else {
            check(errno == ENOENT);
        }
    }
    {
        mortar::Ipc ipc(path);
        check(lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode));
        int abstract_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        check(abstract_fd >= 0);
        sockaddr_un abstract_address {};
        abstract_address.sun_family = AF_UNIX;
        std::memcpy(abstract_address.sun_path + 1, "mortar-ipc-test", 15);
        check(bind(abstract_fd, reinterpret_cast<sockaddr*>(&abstract_address),
                   offsetof(sockaddr_un, sun_path) + 16) == 0);
        check(listen(abstract_fd, 1) == 0);
        mortar::Ipc::Handoff mismatch {abstract_fd, static_cast<std::uint64_t>(st.st_ino), path};
        bool rejected = false;
        try { mortar::Ipc adopted(mismatch); } catch (const std::runtime_error&) { rejected = true; }
        if (rejected) {
            check(fcntl(abstract_fd, F_GETFD) >= 0);
            close(abstract_fd);
        } else {
            check(fcntl(abstract_fd, F_GETFD) < 0 && errno == EBADF);
        }
        check(lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode));
        mortar::Ipc other(std::string(directory) + "/other");
        auto wrong = other.handoff();
        wrong.path = path;
        wrong.inode = st.st_ino;
        rejected = false;
        try { mortar::Ipc adopted(wrong); } catch (const std::runtime_error&) { rejected = true; }
        if (rejected) {
            check(fcntl(wrong.fd, F_GETFD) >= 0);
            close(wrong.fd);
        } else {
            check(fcntl(wrong.fd, F_GETFD) < 0 && errno == EBADF);
        }
        check(lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode));
    }
    check(lstat(path.c_str(), &st) < 0 && errno == ENOENT);
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
