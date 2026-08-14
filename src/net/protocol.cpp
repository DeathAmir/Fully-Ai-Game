#include "net/protocol.hpp"

#include <charconv>
#include <cstdio>

namespace irx::net {

namespace {
std::string fieldString(std::string_view line, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":\"";
    const auto start = line.find(needle);
    if (start == std::string_view::npos) return {};
    std::string out;
    bool escaped = false;
    for (std::size_t i = start + needle.size(); i < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(c); break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') return out;
        out.push_back(c);
    }
    return {};
}

int fieldInt(std::string_view line, std::string_view key, int fallback) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const auto start = line.find(needle);
    if (start == std::string_view::npos) return fallback;
    int value = fallback;
    const char* begin = line.data() + start + needle.size();
    const char* end = line.data() + line.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} ? value : fallback;
}
}

std::string escapeJson(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
                break;
        }
    }
    return out;
}

std::string helloPacket(std::string_view playerName) {
    return "{\"type\":\"hello\",\"name\":\"" + escapeJson(playerName) + "\",\"client\":\"iRx\",\"protocol\":2}\n";
}

std::string pingPacket(std::string_view token, std::uint64_t timestampMs) {
    return "{\"type\":\"ping\",\"token\":\"" + escapeJson(token) + "\",\"time\":" + std::to_string(timestampMs) + "}\n";
}

std::string joinPacket(std::string_view token, std::string_view team) {
    return "{\"type\":\"join\",\"token\":\"" + escapeJson(token) + "\",\"team\":\"" + escapeJson(team) + "\"}\n";
}

std::string movePacket(std::string_view token, float x, float y, float z, float yaw, float pitch) {
    char buffer[384]{};
    std::snprintf(buffer, sizeof(buffer), "{\"type\":\"move\",\"token\":\"%s\",\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"yaw\":%.3f,\"pitch\":%.3f}\n", escapeJson(token).c_str(), x, y, z, yaw, pitch);
    return buffer;
}

std::string buyPacket(std::string_view token, std::string_view item) {
    return "{\"type\":\"buy\",\"token\":\"" + escapeJson(token) + "\",\"item\":\"" + escapeJson(item) + "\"}\n";
}

std::string votePacket(std::string_view token, std::string_view choice) {
    return "{\"type\":\"vote\",\"token\":\"" + escapeJson(token) + "\",\"choice\":\"" + escapeJson(choice) + "\"}\n";
}

bool parseWelcome(std::string_view line, Welcome& out) {
    if (packetType(line) != "welcome") return false;
    out.playerId = fieldInt(line, "id", 0);
    out.protocol = fieldInt(line, "protocol", 0);
    out.tickRate = fieldInt(line, "tick", 20);
    out.token = fieldString(line, "token");
    return out.playerId > 0 && out.protocol == 2 && !out.token.empty();
}

std::string packetType(std::string_view line) {
    return fieldString(line, "type");
}

}