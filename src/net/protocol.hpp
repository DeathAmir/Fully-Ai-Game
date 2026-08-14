#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace irx::net {

struct Welcome {
    int playerId = 0;
    int protocol = 0;
    int tickRate = 20;
    std::string token;
};

std::string escapeJson(std::string_view value);
std::string helloPacket(std::string_view playerName);
std::string pingPacket(std::string_view token, std::uint64_t timestampMs);
std::string joinPacket(std::string_view token, std::string_view team);
std::string movePacket(std::string_view token, float x, float y, float z, float yaw, float pitch);
std::string buyPacket(std::string_view token, std::string_view item);
std::string votePacket(std::string_view token, std::string_view choice);
bool parseWelcome(std::string_view line, Welcome& out);
std::string packetType(std::string_view line);

}