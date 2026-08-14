#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "net/protocol.hpp"

namespace irx::net {

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool start(std::string host, std::uint16_t port, std::string playerName);
    void stop();
    bool connected() const noexcept;
    int playerId() const noexcept;
    int latencyMs() const noexcept;
    std::string token() const;
    std::string lastError() const;
    bool sendPacket(std::string packet);
    bool poll(std::string& line);

private:
    void worker(std::string host, std::uint16_t port, std::string playerName);
    void setError(std::string value);

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> playerId_{0};
    std::atomic<int> latencyMs_{-1};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::deque<std::string> inbound_;
    std::deque<std::string> outbound_;
    std::string token_;
    std::string error_;
};

}