#include "net_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>
#include <string_view>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace irx {
namespace {

constexpr std::uint32_t kMagic = 0x31585249u;
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 20;
constexpr std::size_t kMaximumDatagram = 1200;

enum PacketType : std::uint8_t {
    Hello = 1,
    Challenge = 2,
    Authenticate = 3,
    Welcome = 4,
    Input = 5,
    Snapshot = 6,
    Event = 7,
    Ping = 8,
    Pong = 9,
    Disconnect = 10,
    Achievement = 11
};

template <typename T>
void append(std::vector<std::uint8_t>& output, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    output.insert(output.end(), bytes, bytes + sizeof(T));
}

void appendBytes(std::vector<std::uint8_t>& output, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    output.insert(output.end(), bytes, bytes + size);
}

template <typename T>
bool read(const std::uint8_t*& cursor, const std::uint8_t* end, T& value) {
    if (static_cast<std::size_t>(end - cursor) < sizeof(T)) return false;
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

std::uint64_t clockStamp() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool wouldBlock() {
#ifdef _WIN32
    const int code = WSAGetLastError();
    return code == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void closeSocket(std::intptr_t socket) {
#ifdef _WIN32
    if (socket != -1) closesocket(static_cast<SOCKET>(socket));
#else
    if (socket != -1) close(static_cast<int>(socket));
#endif
}

}

NetClient::NetClient() {
#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

NetClient::~NetClient() {
    disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool NetClient::connect(const std::string& host, std::uint16_t port, const std::string& playerName) {
    disconnect();
    state_ = ConnectionState::Resolving;
    playerName_ = playerName.empty() ? "Player" : playerName.substr(0, 23);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
        state_ = ConnectionState::Failed;
        return false;
    }
    addrinfo* selected = results;
#ifdef _WIN32
    const SOCKET native = socket(selected->ai_family, selected->ai_socktype, selected->ai_protocol);
    if (native == INVALID_SOCKET) {
        freeaddrinfo(results);
        state_ = ConnectionState::Failed;
        return false;
    }
    socket_ = static_cast<std::intptr_t>(native);
    u_long nonBlocking = 1;
    ioctlsocket(native, FIONBIO, &nonBlocking);
#else
    const int native = socket(selected->ai_family, selected->ai_socktype, selected->ai_protocol);
    if (native < 0) {
        freeaddrinfo(results);
        state_ = ConnectionState::Failed;
        return false;
    }
    socket_ = native;
    const int flags = fcntl(native, F_GETFL, 0);
    fcntl(native, F_SETFL, flags | O_NONBLOCK);
#endif
    serverAddressSize_ = std::min<int>(static_cast<int>(selected->ai_addrlen), static_cast<int>(serverAddress_.size()));
    std::memcpy(serverAddress_.data(), selected->ai_addr, static_cast<std::size_t>(serverAddressSize_));
    freeaddrinfo(results);
    std::random_device random;
    nonce_ = (static_cast<std::uint64_t>(random()) << 32u) ^ static_cast<std::uint64_t>(random()) ^ clockStamp();
    state_ = ConnectionState::Handshake;
    retryTimer_ = 1.0f;
    silenceTimer_ = 0.0f;
    sendHello();
    return true;
}

void NetClient::disconnect() {
    if (socket_ != -1 && sessionToken_ != 0) sendPacket(PacketType::Disconnect, {}, sessionToken_);
    closeSocket(socket_);
    socket_ = -1;
    sessionToken_ = 0;
    selfId_ = 0;
    team_ = Team::Spectator;
    pendingTeam_ = Team::Spectator;
    hasFrame_ = false;
    snapshot_ = {};
    if (state_ != ConnectionState::TimedOut && state_ != ConnectionState::Failed)
        state_ = ConnectionState::Offline;
}

void NetClient::poll(float deltaTime) {
    if (socket_ == -1) return;
    receivePackets();
    retryTimer_ -= deltaTime;
    silenceTimer_ += deltaTime;
    inputTimer_ += deltaTime;
    pingTimer_ += deltaTime;
    if (state_ == ConnectionState::Handshake && retryTimer_ <= 0.0f) {
        if (cookie_[0] == 0 && cookie_[1] == 0) sendHello();
        else sendAuthentication();
        retryTimer_ = 1.0f;
    }
    if (state_ == ConnectionState::Connected) {
        if (hasFrame_ && inputTimer_ >= 1.0f / 30.0f) {
            latestFrame_.requestedTeam = pendingTeam_;
            sendInput(latestFrame_);
            latestFrame_.actions &= static_cast<std::uint8_t>(2u | 4u | 64u | 128u);
            pendingTeam_ = Team::Spectator;
            inputTimer_ = 0.0f;
        }
        if (pingTimer_ >= 1.0f) {
            sendPing();
            pingTimer_ = 0.0f;
        }
        if (silenceTimer_ > 10.0f) {
            state_ = ConnectionState::TimedOut;
            closeSocket(socket_);
            socket_ = -1;
        }
    }
}

void NetClient::submit(const LocalFrame& frame) {
    const std::uint8_t pendingOneShot = hasFrame_ ? static_cast<std::uint8_t>(latestFrame_.actions & (1u | 8u | 16u | 32u)) : 0u;
    latestFrame_ = frame;
    latestFrame_.actions |= pendingOneShot;
    hasFrame_ = true;
}

void NetClient::requestTeam(Team team) {
    pendingTeam_ = team;
}

const char* NetClient::status() const {
    switch (state_) {
        case ConnectionState::Offline: return "OFFLINE PRACTICE";
        case ConnectionState::Resolving: return "RESOLVING SERVER";
        case ConnectionState::Handshake: return "PROTECTED HANDSHAKE";
        case ConnectionState::Connected: return "IRX ONLINE";
        case ConnectionState::TimedOut: return "SERVER TIMEOUT";
        case ConnectionState::Failed: return "SERVER UNAVAILABLE";
    }
    return "OFFLINE";
}

void NetClient::sendHello() {
    std::vector<std::uint8_t> payload;
    append(payload, nonce_);
    std::array<char, 24> name{};
    std::memcpy(name.data(), playerName_.data(), std::min(playerName_.size(), name.size() - 1));
    appendBytes(payload, name.data(), name.size());
    sendPacket(PacketType::Hello, payload, 0);
}

void NetClient::sendAuthentication() {
    std::vector<std::uint8_t> payload;
    append(payload, nonce_);
    appendBytes(payload, cookie_.data(), cookie_.size());
    std::array<char, 24> name{};
    std::memcpy(name.data(), playerName_.data(), std::min(playerName_.size(), name.size() - 1));
    appendBytes(payload, name.data(), name.size());
    sendPacket(PacketType::Authenticate, payload, 0);
}

void NetClient::sendInput(const LocalFrame& frame) {
    std::vector<std::uint8_t> payload;
    append(payload, frame.position.x);
    append(payload, frame.position.y);
    append(payload, frame.position.z);
    append(payload, frame.velocity.x);
    append(payload, frame.velocity.y);
    append(payload, frame.velocity.z);
    append(payload, frame.yaw);
    append(payload, frame.pitch);
    append(payload, frame.weapon);
    append(payload, frame.actions);
    append(payload, static_cast<std::uint8_t>(frame.requestedTeam));
    append(payload, static_cast<std::uint8_t>(0));
    append(payload, snapshot_.tick);
    sendPacket(PacketType::Input, payload, sessionToken_);
}

void NetClient::sendPing() {
    lastPingStamp_ = clockStamp();
    std::vector<std::uint8_t> payload;
    append(payload, lastPingStamp_);
    sendPacket(PacketType::Ping, payload, sessionToken_);
}

void NetClient::receivePackets() {
    std::array<std::uint8_t, 2048> packet{};
    while (true) {
        sockaddr_storage source{};
#ifdef _WIN32
        int sourceSize = sizeof(source);
        const int received = recvfrom(static_cast<SOCKET>(socket_), reinterpret_cast<char*>(packet.data()),
                                      static_cast<int>(packet.size()), 0,
                                      reinterpret_cast<sockaddr*>(&source), &sourceSize);
        if (received == SOCKET_ERROR) {
#else
        socklen_t sourceSize = sizeof(source);
        const int received = static_cast<int>(recvfrom(static_cast<int>(socket_), packet.data(), packet.size(), 0,
                                                       reinterpret_cast<sockaddr*>(&source), &sourceSize));
        if (received < 0) {
#endif
            if (wouldBlock()) break;
            state_ = ConnectionState::Failed;
            break;
        }
        if (received == 0) break;
        handlePacket(packet.data(), static_cast<std::size_t>(received));
    }
}

void NetClient::handlePacket(const std::uint8_t* data, std::size_t size) {
    if (size < kHeaderSize || size > kMaximumDatagram) return;
    const std::uint8_t* cursor = data;
    const std::uint8_t* end = data + size;
    std::uint32_t magic = 0;
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    std::uint16_t payloadSize = 0;
    std::uint32_t sequence = 0;
    std::uint64_t token = 0;
    if (!read(cursor, end, magic) || !read(cursor, end, version) || !read(cursor, end, type) ||
        !read(cursor, end, payloadSize) || !read(cursor, end, sequence) || !read(cursor, end, token)) return;
    if (magic != kMagic || version != kVersion || payloadSize != static_cast<std::uint16_t>(end - cursor)) return;
    if (type == PacketType::Challenge) {
        std::uint64_t nonce = 0;
        if (!read(cursor, end, nonce) || nonce != nonce_ || static_cast<std::size_t>(end - cursor) != cookie_.size()) return;
        std::memcpy(cookie_.data(), cursor, cookie_.size());
        sendAuthentication();
        retryTimer_ = 1.0f;
        silenceTimer_ = 0.0f;
        return;
    }
    if (type == PacketType::Welcome) {
        std::uint8_t team = 0;
        std::uint8_t tickRate = 0;
        std::uint16_t reserved = 0;
        if (!read(cursor, end, selfId_) || !read(cursor, end, team) || !read(cursor, end, tickRate) ||
            !read(cursor, end, reserved) || token == 0) return;
        sessionToken_ = token;
        team_ = static_cast<Team>(std::min<std::uint8_t>(team, 2));
        state_ = ConnectionState::Connected;
        silenceTimer_ = 0.0f;
        return;
    }
    if (state_ != ConnectionState::Connected || token != sessionToken_) return;
    silenceTimer_ = 0.0f;
    if (type == PacketType::Pong) {
        std::uint64_t stamp = 0;
        if (read(cursor, end, stamp) && stamp == lastPingStamp_)
            pingMilliseconds_ = static_cast<int>(std::min<std::uint64_t>((clockStamp() - stamp) / 1000u, 999u));
        return;
    }
    if (type != PacketType::Snapshot) return;
    MatchSnapshot next;
    std::uint8_t phase = 0;
    std::uint8_t winner = 0;
    std::uint16_t count = 0;
    if (!read(cursor, end, next.tick) || !read(cursor, end, next.roundRemaining) ||
        !read(cursor, end, next.bombRemaining) || !read(cursor, end, phase) || !read(cursor, end, winner) ||
        !read(cursor, end, next.terroristScore) || !read(cursor, end, next.counterTerroristScore) ||
        !read(cursor, end, next.selfHealth) || !read(cursor, end, count)) return;
    next.phase = static_cast<RoundPhase>(std::min<std::uint8_t>(phase, 4));
    next.winner = static_cast<Team>(std::min<std::uint8_t>(winner, 2));
    count = std::min<std::uint16_t>(count, 32);
    next.players.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        RemotePlayer player;
        std::uint8_t team = 0;
        std::uint8_t alive = 0;
        if (!read(cursor, end, player.id) || !read(cursor, end, team) || !read(cursor, end, alive) ||
            !read(cursor, end, player.weapon) || !read(cursor, end, player.flags) ||
            !read(cursor, end, player.position.x) || !read(cursor, end, player.position.y) ||
            !read(cursor, end, player.position.z) || !read(cursor, end, player.yaw) ||
            !read(cursor, end, player.pitch) || !read(cursor, end, player.health)) return;
        player.team = static_cast<Team>(std::min<std::uint8_t>(team, 2));
        player.alive = alive != 0;
        if (player.id == selfId_) {
            team_ = player.team;
            next.selfPosition = player.position;
            next.hasSelfPosition = true;
        } else next.players.push_back(player);
    }
    snapshot_ = std::move(next);
}

bool NetClient::sendPacket(std::uint8_t type, const std::vector<std::uint8_t>& payload, std::uint64_t token) {
    if (socket_ == -1 || payload.size() + kHeaderSize > kMaximumDatagram) return false;
    std::vector<std::uint8_t> packet;
    packet.reserve(kHeaderSize + payload.size());
    append(packet, kMagic);
    append(packet, kVersion);
    append(packet, type);
    append(packet, static_cast<std::uint16_t>(payload.size()));
    append(packet, sequence_++);
    append(packet, token);
    appendBytes(packet, payload.data(), payload.size());
#ifdef _WIN32
    const int sent = sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(packet.data()),
                            static_cast<int>(packet.size()), 0,
                            reinterpret_cast<const sockaddr*>(serverAddress_.data()), serverAddressSize_);
    return sent == static_cast<int>(packet.size());
#else
    const auto sent = sendto(static_cast<int>(socket_), packet.data(), packet.size(), 0,
                             reinterpret_cast<const sockaddr*>(serverAddress_.data()),
                             static_cast<socklen_t>(serverAddressSize_));
    return sent == static_cast<ssize_t>(packet.size());
#endif
}

}
