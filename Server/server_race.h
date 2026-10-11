#pragma once
#include "server_text.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Races (server_race.cpp): a finish zone on the current map, checkpoints to pass in order on the
// way, and players timed from race-start to the finish. Everything here is pure, for the tests.
namespace dingosdk::server {
// A sphere in world units as the log prints them (metres, y up).
struct RaceZone {
    std::array<float, 3> centre{};
    float radius{};
};
constexpr std::size_t max_racers = 64;
constexpr std::size_t max_race_checkpoints = 16;
constexpr float max_race_coordinate = 100000.f; // as teleport: far beyond any map, short of nonsense
constexpr float min_race_radius = 1.f, max_race_radius = 200.f;
constexpr unsigned default_race_timeout = 300, max_race_timeout = 3600; // seconds
constexpr std::string_view race_zone_usage = "<x y z> <radius 1-200>";
constexpr std::string_view race_start_usage = "race-start <SteamID64,SteamID64,...> [timeout <seconds>]";

// "x y z radius", or why it is not one.
inline std::string parse_race_zone(std::string_view argument, RaceZone &out) {
    std::array<float, 4> values{};
    auto rest = trim(argument);
    for (auto &value : values) {
        const auto [word, remaining] = split(rest);
        if (word.empty()) return "A zone is " + std::string(race_zone_usage) + ".";
        const auto parsed = std::from_chars(word.data(), word.data() + word.size(), value);
        if (parsed.ptr != word.data() + word.size()) return "\"" + std::string(word) + "\" is not a number.";
        if (parsed.ec == std::errc::result_out_of_range) value = INFINITY; // "1e99": a number, and too far
        else if (parsed.ec != std::errc{}) return "\"" + std::string(word) + "\" is not a number.";
        rest = remaining;
    }
    if (!rest.empty()) return "A zone is " + std::string(race_zone_usage) + ".";
    for (std::size_t i = 0; i < 3; ++i)
        if (!std::isfinite(values[i]) || std::abs(values[i]) > max_race_coordinate)
            return "Coordinates are x y z, each a number from -100000 to 100000.";
    if (!std::isfinite(values[3]) || values[3] < min_race_radius || values[3] > max_race_radius)
        return "The radius is 1 to 200 m.";
    out = {{values[0], values[1], values[2]}, values[3]};
    return {};
}
// One SteamID64, or several joined by commas with no spaces (teleport's rules): no empty
// entries, no 0, a repeat counted once, at most max_racers.
inline std::string parse_race_ids(std::string_view text, std::vector<std::uint64_t> &out) {
    out.clear();
    if (text.empty()) return std::string(race_start_usage);
    for (auto rest = text;;) {
        const auto comma = rest.find(',');
        const auto part = rest.substr(0, comma);
        const auto id = number(part);
        if (!id || !*id) return "\"" + std::string(part) + "\" is not a SteamID64. A list is SteamID64s joined by commas, no spaces.";
        if (std::find(out.begin(), out.end(), *id) == out.end()) out.push_back(*id);
        if (out.size() > max_racers) return "At most " + std::to_string(max_racers) + " players per race.";
        if (comma == std::string_view::npos) return {};
        rest = rest.substr(comma + 1);
    }
}
struct RaceStart {
    std::vector<std::uint64_t> ids;
    unsigned timeout = default_race_timeout; // seconds
};
// "<ids> [timeout <seconds>]", or why it is not one.
inline std::string parse_race_start(std::string_view argument, RaceStart &out) {
    out = {};
    const auto [ids, rest] = split(argument);
    if (auto why = parse_race_ids(ids, out.ids); !why.empty()) return why;
    if (rest.empty()) return {};
    const auto [word, seconds_text] = split(rest);
    const auto seconds = number(seconds_text);
    if (lower(word) != "timeout" || !seconds || !*seconds || *seconds > max_race_timeout)
        return std::string(race_start_usage) + " (timeout 1 to " + std::to_string(max_race_timeout) + ").";
    out.timeout = static_cast<unsigned>(*seconds);
    return {};
}
// Inside or on the sphere. A position that is not a number is never inside.
inline bool in_zone(const RaceZone &zone, const std::array<float, 3> &position) noexcept {
    float squared{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto d = position[i] - zone.centre[i];
        squared += d * d;
    }
    return squared <= zone.radius * zone.radius;
}
// One racer at `position`: passes the next checkpoints in order (several at once where they
// overlap), then whether they are in the finish with every checkpoint behind them.
inline bool race_step(std::size_t &next, const std::vector<RaceZone> &checkpoints, const RaceZone &finish,
                      const std::array<float, 3> &position) noexcept {
    while (next < checkpoints.size() && in_zone(checkpoints[next], position)) ++next;
    return next >= checkpoints.size() && in_zone(finish, position);
}
struct Racer {
    std::uint64_t id{};
    std::string name;
    std::size_t next{};               // the checkpoint they have to pass next
    std::optional<std::uint64_t> ms; // their time, once they finish
    unsigned place{};
    bool left{}; // left the server (or the world went): did not finish
};
// A name as one log line can carry it: control characters (a line break) become spaces.
inline std::string one_line(std::string_view text) {
    std::string out(text);
    for (auto &c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    return out;
}
// Milliseconds as seconds with one decimal, rounded: 48250 -> "48.3".
inline std::string race_seconds(std::uint64_t ms) {
    const auto tenths = (ms + 50) / 100;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}
inline std::string race_finish_line(const Racer &racer) {
    return "[race] " + one_line(racer.name) + " (" + std::to_string(racer.id) + ") finished " + std::to_string(racer.place) +
           " in " + race_seconds(racer.ms.value_or(0)) + " s";
}
// "[race] ended: 1. A 48.3 s, 2. B 51.0 s, C DNF": finishers by place, then the rest as they started.
inline std::string race_results_line(const std::vector<Racer> &racers) {
    std::vector<const Racer *> order;
    for (const auto &racer : racers) order.push_back(&racer);
    std::stable_sort(order.begin(), order.end(), [](const Racer *a, const Racer *b) {
        if (a->ms.has_value() != b->ms.has_value()) return a->ms.has_value();
        return a->ms && a->place < b->place;
    });
    std::string line = "[race] ended:";
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto &racer = *order[i];
        line += i ? ", " : " ";
        line += racer.ms ? std::to_string(racer.place) + ". " + one_line(racer.name) + " " + race_seconds(*racer.ms) + " s"
                         : one_line(racer.name) + " DNF";
    }
    if (order.empty()) line += " nobody raced";
    return line;
}
// The race state the host keeps: the course on this map, and the running race.
struct Race {
    std::optional<RaceZone> finish;
    std::vector<RaceZone> checkpoints;
    std::uint64_t world{}; // the world (map change) the course was set in
    bool running{};
    std::uint64_t started{}, deadline{}; // microseconds, as the host's clock
    unsigned finished{};
    std::vector<Racer> racers;
};
} // namespace dingosdk::server
