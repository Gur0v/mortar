#include "ipc.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace mortar {
namespace {
[[noreturn]] void fail(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
}

Ipc::Ipc(const std::string& runtime_dir) {
    struct stat st {};
    if (lstat(runtime_dir.c_str(), &st) < 0) fail("runtime directory");
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0)
        throw std::runtime_error("runtime directory must be private and owned by the current user");
    std::string pattern = runtime_dir + "/mortar-XXXXXX";
    if (!mkdtemp(pattern.data())) fail("mkdtemp");
    directory_ = pattern;
    try {
        dir_ = open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dir_ < 0) fail("open IPC directory");
        path_ = directory_ + "/socket";
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(address.sun_path)) throw std::runtime_error("IPC path too long");
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        listener_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listener_ < 0) fail("socket");
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) fail("bind");
        socket_inode_ = 1;
        if (fstatat(dir_, "socket", &st, AT_SYMLINK_NOFOLLOW) < 0) fail("stat socket");
        socket_inode_ = st.st_ino;
        if (listen(listener_, 32) < 0) fail("listen");
    } catch (...) {
        if (listener_ >= 0) close(listener_);
        if (dir_ >= 0) {
            if (socket_inode_) unlinkat(dir_, "socket", 0);
            close(dir_);
        }
        rmdir(directory_.c_str());
        throw;
    }
}

Ipc::~Ipc() {
    for (const auto& client : clients_) close(client.fd);
    if (listener_ >= 0) close(listener_);
    struct stat st {};
    if (dir_ >= 0) {
        if (fstatat(dir_, "socket", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISSOCK(st.st_mode) && st.st_ino == socket_inode_)
            unlinkat(dir_, "socket", 0);
        close(dir_);
    }
    rmdir(directory_.c_str());
}

std::vector<pollfd> Ipc::poll_fds() const {
    std::vector<pollfd> result {{listener_, POLLIN, 0}};
    for (const auto& client : clients_)
        result.push_back({client.fd, static_cast<short>((client.eof ? 0 : POLLIN) | (client.output.empty() ? 0 : POLLOUT)), 0});
    return result;
}

void Ipc::close_client(int fd) {
    auto it = std::find_if(clients_.begin(), clients_.end(), [fd](const Client& c) { return c.fd == fd; });
    if (it != clients_.end()) {
        close(it->fd);
        clients_.erase(it);
        if (on_close) on_close(fd);
    }
}

bool Ipc::send(int fd, std::string_view frame) {
    auto it = std::find_if(clients_.begin(), clients_.end(), [fd](const Client& c) { return c.fd == fd; });
    if (it == clients_.end()) return false;
    if (frame.size() > max_frame || it->output.size() + 4 + frame.size() > max_queue) {
        close_client(fd);
        return false;
    }
    const auto size = static_cast<std::uint32_t>(frame.size());
    for (int shift = 24; shift >= 0; shift -= 8) it->output.push_back(static_cast<char>((size >> shift) & 255));
    it->output.append(frame);
    return true;
}

void Ipc::broadcast(std::string_view frame) {
    std::vector<int> fds;
    for (const auto& client : clients_) fds.push_back(client.fd);
    for (int fd : fds) send(fd, frame);
}

void Ipc::dispatch(const std::vector<pollfd>& ready, const Handler& handler) {
    std::vector<pollfd> ordered;
    for (const auto& event : ready) if (event.fd == listener_) ordered.push_back(event);
    if (!clients_.empty()) {
        next_client_ %= clients_.size();
        for (std::size_t i = 0; i < clients_.size(); ++i) {
            int fd = clients_[(next_client_ + i) % clients_.size()].fd;
            auto it = std::find_if(ready.begin(), ready.end(), [fd](const pollfd& event) { return event.fd == fd; });
            if (it != ready.end()) ordered.push_back(*it);
        }
        next_client_ = (next_client_ + 1) % clients_.size();
    }
    bool evaluated = false;
    for (const auto& event : ordered) {
        if (event.fd == listener_) {
            if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("IPC listener failed");
            if (!(event.revents & POLLIN)) continue;
            for (std::size_t i = 0; i < max_clients; ++i) {
                int fd = accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM || errno == ECONNABORTED) break;
                    fail("accept");
                }
                ucred cred {};
                socklen_t length = sizeof(cred);
                if (clients_.size() >= max_clients || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) < 0 ||
                    length != sizeof(cred) || cred.uid != geteuid()) close(fd);
                else clients_.push_back({fd, {}, {}});
            }
            continue;
        }
        auto find = [&] { return std::find_if(clients_.begin(), clients_.end(), [&](const Client& c) { return c.fd == event.fd; }); };
        auto it = find();
        if (it == clients_.end()) continue;
        if (event.revents & (POLLERR | POLLNVAL)) { close_client(event.fd); continue; }
        if ((event.revents & POLLOUT) && !it->output.empty()) {
            auto n = ::send(event.fd, it->output.data(), it->output.size(), MSG_NOSIGNAL);
            if (n > 0) it->output.erase(0, static_cast<std::size_t>(n));
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { close_client(event.fd); continue; }
        }
        if (it->eof && it->output.empty()) { close_client(event.fd); continue; }
        if (!evaluated && !it->eof && (event.revents & (POLLIN | POLLHUP))) {
            std::array<char, 4096> buffer {};
            const std::size_t needed = it->input.size() < 4 ? 4 - it->input.size() : [&] {
                std::uint32_t size = 0;
                for (int i = 0; i < 4; ++i) size = (size << 8) | static_cast<unsigned char>(it->input[i]);
                return static_cast<std::size_t>(4 + size) - it->input.size();
            }();
            auto n = recv(event.fd, buffer.data(), std::min(buffer.size(), needed), 0);
            if (n == 0) {
                it->eof = true;
                if (it->output.empty()) close_client(event.fd);
                continue;
            }
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close_client(event.fd);
                continue;
            }
            it->input.append(buffer.data(), static_cast<std::size_t>(n));
            while ((it = find()) != clients_.end() && it->input.size() >= 4) {
                std::uint32_t size = 0;
                for (int i = 0; i < 4; ++i) size = (size << 8) | static_cast<unsigned char>(it->input[i]);
                if (size > max_frame) { close_client(event.fd); break; }
                if (it->input.size() < 4 + size) break;
                std::string frame = it->input.substr(4, size);
                it->input.erase(0, 4 + size);
                evaluated = true;
                handler(event.fd, frame);
            }
        }
    }
}

}
