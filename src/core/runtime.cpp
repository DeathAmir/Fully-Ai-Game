#include "core/runtime.hpp"

#include <cstdlib>

#ifndef IRX_SERVER_HOST
#define IRX_SERVER_HOST "irautox.ir"
#endif
#ifndef IRX_SERVER_PORT
#define IRX_SERVER_PORT 9832
#endif

namespace irx {

Runtime& Runtime::instance() {
    static Runtime runtime;
    return runtime;
}

Runtime::~Runtime() { stop(); }

std::string Runtime::defaultPlayerName() const {
    if (const char* configured = std::getenv("IRX_PLAYER_NAME"); configured && *configured) return configured;
    if (const char* username = std::getenv("USERNAME"); username && *username) return username;
    return "Player";
}

void Runtime::start() {
    if (started_.exchange(true)) return;
    if (const char* disabled = std::getenv("IRX_DISABLE_NETWORK"); disabled && std::string(disabled) == "1") return;
    network_.start(IRX_SERVER_HOST, static_cast<unsigned short>(IRX_SERVER_PORT), defaultPlayerName());
}

void Runtime::stop() {
    if (!started_.exchange(false)) return;
    network_.stop();
}

bool Runtime::started() const noexcept { return started_.load(); }
net::Client& Runtime::network() noexcept { return network_; }
const net::Client& Runtime::network() const noexcept { return network_; }

}