#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

namespace mortar {

class Ipc {
public:
    using Handler = std::function<void(int, std::string_view)>;
    static constexpr std::size_t max_frame = 65536;
    static constexpr std::size_t max_queue = 262144;
    static constexpr std::size_t max_clients = 32;

    explicit Ipc(const std::string& path);
    ~Ipc();
    Ipc(const Ipc&) = delete;
    Ipc& operator=(const Ipc&) = delete;

    const std::string& path() const { return path_; }
    std::vector<pollfd> poll_fds() const;
    void dispatch(const std::vector<pollfd>& ready, const Handler& handler);
    void subscribe(int client);
    bool send(int client, std::string_view frame);
    void broadcast(std::string_view frame);
    std::function<void(int)> on_close;

private:
    struct Client {
        int fd;
        std::string input;
        std::string output;
        bool eof = false;
        bool subscribed = false;
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    };
    int listener_ = -1;
    int dir_ = -1;
    std::string directory_;
    std::string path_;
    std::string name_;
    std::uint64_t socket_inode_ = 0;
    std::vector<Client> clients_;
    std::size_t next_client_ = 0;
    std::chrono::steady_clock::time_point accept_retry_ {};
    void close_client(int fd);
};

}
