#include "discord_rpc.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#ifndef IRX_DISCORD_APP_ID
#define IRX_DISCORD_APP_ID ""
#endif

namespace irx {
namespace {

std::string escapeJson(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            escaped.push_back('\\');
            escaped.push_back(static_cast<char>(character));
        } else if (character >= 0x20) escaped.push_back(static_cast<char>(character));
    }
    return escaped;
}

}

DiscordRpc::DiscordRpc() {
    const char* environment = std::getenv("IRX_DISCORD_APP_ID");
    applicationId_ = environment != nullptr && *environment != '\0' ? environment : IRX_DISCORD_APP_ID;
    startedAt_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

DiscordRpc::~DiscordRpc() {
    closePipe();
}

void DiscordRpc::update(const std::string& details, const std::string& state, int players, int capacity) {
#ifdef _WIN32
    if (applicationId_.empty()) return;
    if (pipe_ == -1 && !connectPipe()) return;
    std::ostringstream activity;
    activity << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << GetCurrentProcessId()
             << ",\"activity\":{\"details\":\"" << escapeJson(details)
             << "\",\"state\":\"" << escapeJson(state)
             << "\",\"timestamps\":{\"start\":" << startedAt_
             << "},\"assets\":{\"large_image\":\"irx\",\"large_text\":\"iRx Tactical Strike\"}"
             << ",\"party\":{\"size\":[" << std::max(0, players) << ',' << std::max(1, capacity)
             << "]},\"instance\":true}},\"nonce\":\"" << nonce_++ << "\"}";
    if (!sendFrame(1, activity.str())) closePipe();
#else
    static_cast<void>(details);
    static_cast<void>(state);
    static_cast<void>(players);
    static_cast<void>(capacity);
#endif
}

bool DiscordRpc::connectPipe() {
#ifdef _WIN32
    for (int index = 0; index < 10; ++index) {
        const std::string path = "\\\\.\\pipe\\discord-ipc-" + std::to_string(index);
        const HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        pipe_ = reinterpret_cast<std::intptr_t>(handle);
        const std::string handshake = "{\"v\":1,\"client_id\":\"" + escapeJson(applicationId_) + "\"}";
        if (sendFrame(0, handshake)) return true;
        closePipe();
    }
#endif
    return false;
}

bool DiscordRpc::sendFrame(std::uint32_t operation, const std::string& json) {
#ifdef _WIN32
    if (pipe_ == -1 || json.size() > 65535) return false;
    std::array<std::uint32_t, 2> header{operation, static_cast<std::uint32_t>(json.size())};
    DWORD written = 0;
    const HANDLE handle = reinterpret_cast<HANDLE>(pipe_);
    if (!WriteFile(handle, header.data(), static_cast<DWORD>(sizeof(header)), &written, nullptr) || written != sizeof(header)) return false;
    return WriteFile(handle, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size();
#else
    static_cast<void>(operation);
    static_cast<void>(json);
    return false;
#endif
}

void DiscordRpc::closePipe() {
#ifdef _WIN32
    if (pipe_ != -1) CloseHandle(reinterpret_cast<HANDLE>(pipe_));
#endif
    pipe_ = -1;
}

}
