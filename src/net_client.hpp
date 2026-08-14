#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace irx {

enum class ConnectionState : std::uint8_t { Offline, Resolving, Handshake, Connected, TimedOut, Failed };
enum class Team : std::uint8_t { Spectator, Terrorist, CounterTerrorist };
enum class RoundPhase : std::uint8_t { Waiting, Warmup, Live, BombPlanted, RoundEnd };

struct RemotePlayer {
    std::uint32_t id = 0;
    Team team = Team::Spectator;
    bool alive = false;
    std::uint8_t weapon = 0;
    std::uint8_t flags = 0;
    glm::vec3 position{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    std::uint16_t health = 0;
};

struct MatchSnapshot {
    std::uint32_t tick = 0;
    float roundRemaining = 0.0f;
    float bombRemaining = 0.0f;
    RoundPhase phase = RoundPhase::Waiting;
    Team winner = Team::Spectator;
    std::uint8_t terroristScore = 0;
    std::uint8_t counterTerroristScore = 0;
    std::uint16_t selfHealth = 100;
    glm::vec3 selfPosition{};
    bool hasSelfPosition = false;
    std::vector<RemotePlayer> players;
};

struct LocalFrame {
    glm::vec3 position{};
    glm::vec3 velocity{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    std::uint8_t weapon = 0;
    std::uint8_t actions = 0;
    Team requestedTeam = Team::Spectator;
};

class NetClient {
public:
    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    bool connect(const std::string& host, std::uint16_t port, const std::string& playerName);
    void disconnect();
    void poll(float deltaTime);
    void submit(const LocalFrame& frame);
    void requestTeam(Team team);

    ConnectionState state() const { return state_; }
    bool connected() const { return state_ == ConnectionState::Connected; }
    const char* status() const;
    std::uint32_t selfId() const { return selfId_; }
    Team team() const { return team_; }
    int pingMilliseconds() const { return pingMilliseconds_; }
    const MatchSnapshot& snapshot() const { return snapshot_; }

private:
    void sendHello();
    void sendAuthentication();
    void sendInput(const LocalFrame& frame);
    void sendPing();
    void receivePackets();
    void handlePacket(const std::uint8_t* data, std::size_t size);
    bool sendPacket(std::uint8_t type, const std::vector<std::uint8_t>& payload, std::uint64_t token);

    std::intptr_t socket_ = -1;
    std::array<std::uint8_t, 128> serverAddress_{};
    int serverAddressSize_ = 0;
    ConnectionState state_ = ConnectionState::Offline;
    std::string playerName_ = "Player";
    std::uint64_t nonce_ = 0;
    std::uint64_t sessionToken_ = 0;
    std::array<std::uint8_t, 32> cookie_{};
    std::uint32_t sequence_ = 1;
    std::uint32_t selfId_ = 0;
    Team team_ = Team::Spectator;
    Team pendingTeam_ = Team::Spectator;
    MatchSnapshot snapshot_{};
    float retryTimer_ = 0.0f;
    float silenceTimer_ = 0.0f;
    float inputTimer_ = 0.0f;
    float pingTimer_ = 0.0f;
    std::uint64_t lastPingStamp_ = 0;
    int pingMilliseconds_ = 0;
    bool hasFrame_ = false;
    LocalFrame latestFrame_{};
};

}
