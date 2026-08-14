#pragma once

#include <cstdint>
#include <string>

namespace irx {

class DiscordRpc {
public:
    DiscordRpc();
    ~DiscordRpc();
    DiscordRpc(const DiscordRpc&) = delete;
    DiscordRpc& operator=(const DiscordRpc&) = delete;

    void update(const std::string& details, const std::string& state, int players, int capacity);

private:
    bool connectPipe();
    bool sendFrame(std::uint32_t operation, const std::string& json);
    void closePipe();

    std::intptr_t pipe_ = -1;
    std::string applicationId_;
    std::uint64_t startedAt_ = 0;
    std::uint64_t nonce_ = 1;
};

}
