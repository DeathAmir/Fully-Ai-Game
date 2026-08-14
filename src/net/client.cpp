#include "net/client.hpp"

#include <chrono>
#include <cstdlib>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace irx::net {

Client::Client() = default;
Client::~Client() { stop(); }

bool Client::start(std::string host, std::uint16_t port, std::string playerName) {
    if (running_.exchange(true)) return false;
    thread_ = std::thread(&Client::worker, this, std::move(host), port, std::move(playerName));
    return true;
}

void Client::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    connected_ = false;
}

bool Client::connected() const noexcept { return connected_.load(); }
int Client::playerId() const noexcept { return playerId_.load(); }
int Client::latencyMs() const noexcept { return latencyMs_.load(); }

std::string Client::token() const {
    std::scoped_lock lock(mutex_);
    return token_;
}

std::string Client::lastError() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

void Client::setError(std::string value) {
    std::scoped_lock lock(mutex_);
    error_ = std::move(value);
}

bool Client::sendPacket(std::string packet) {
    if (!running_) return false;
    std::scoped_lock lock(mutex_);
    if (outbound_.size() >= 256) return false;
    outbound_.push_back(std::move(packet));
    return true;
}

bool Client::poll(std::string& line) {
    std::scoped_lock lock(mutex_);
    if (inbound_.empty()) return false;
    line = std::move(inbound_.front());
    inbound_.pop_front();
    return true;
}

void Client::worker(std::string host, std::uint16_t port, std::string playerName) {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        setError("WSAStartup failed");
        running_ = false;
        return;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
        setError("DNS lookup failed");
        WSACleanup();
        running_ = false;
        return;
    }
    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* current = result; current && running_; current = current->ai_next) {
        sock = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        DWORD timeout = 2500;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        if (connect(sock, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) break;
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    if (sock == INVALID_SOCKET) {
        setError("Could not connect to iRx server");
        WSACleanup();
        running_ = false;
        return;
    }
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    connected_ = true;
    std::string hello = helloPacket(playerName);
    send(sock, hello.data(), static_cast<int>(hello.size()), 0);
    std::string receiveBuffer;
    receiveBuffer.reserve(8192);
    auto lastPing = std::chrono::steady_clock::now();
    while (running_) {
        std::deque<std::string> pending;
        {
            std::scoped_lock lock(mutex_);
            pending.swap(outbound_);
        }
        for (const auto& packet : pending) {
            const char* data = packet.data();
            int remaining = static_cast<int>(packet.size());
            while (remaining > 0 && running_) {
                const int sent = send(sock, data, remaining, 0);
                if (sent > 0) {
                    data += sent;
                    remaining -= sent;
                } else {
                    const int error = WSAGetLastError();
                    if (error == WSAEWOULDBLOCK) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        continue;
                    }
                    running_ = false;
                    break;
                }
            }
        }
        char buffer[4096];
        const int received = recv(sock, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received > 0) {
            receiveBuffer.append(buffer, buffer + received);
            for (;;) {
                const auto newline = receiveBuffer.find('\n');
                if (newline == std::string::npos) break;
                std::string line = receiveBuffer.substr(0, newline);
                receiveBuffer.erase(0, newline + 1);
                Welcome welcome;
                if (parseWelcome(line, welcome)) {
                    playerId_ = welcome.playerId;
                    std::scoped_lock lock(mutex_);
                    token_ = welcome.token;
                }
                {
                    std::scoped_lock lock(mutex_);
                    if (inbound_.size() >= 512) inbound_.pop_front();
                    inbound_.push_back(std::move(line));
                }
            }
        } else if (received == 0) {
            setError("Server closed connection");
            break;
        } else {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAETIMEDOUT) {
                setError("Network receive failed");
                break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPing >= std::chrono::seconds(5)) {
            std::string currentToken;
            {
                std::scoped_lock lock(mutex_);
                currentToken = token_;
            }
            if (!currentToken.empty()) {
                const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                const std::string ping = pingPacket(currentToken, static_cast<std::uint64_t>(stamp));
                send(sock, ping.data(), static_cast<int>(ping.size()), 0);
            }
            lastPing = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    shutdown(sock, SD_BOTH);
    closesocket(sock);
    WSACleanup();
#else
    (void)host;
    (void)port;
    (void)playerName;
    setError("Network client is implemented for Windows builds");
#endif
    connected_ = false;
    running_ = false;
}

}