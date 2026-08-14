#pragma once

#include <atomic>
#include <string>

#include "net/client.hpp"

namespace irx {

class Runtime {
public:
    static Runtime& instance();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    void start();
    void stop();
    bool started() const noexcept;
    net::Client& network() noexcept;
    const net::Client& network() const noexcept;

private:
    Runtime() = default;
    ~Runtime();
    std::string defaultPlayerName() const;

    std::atomic<bool> started_{false};
    net::Client network_;
};

}