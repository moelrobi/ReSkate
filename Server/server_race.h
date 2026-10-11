#pragma once
#include "server_text.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
constexpr unsigned max_false_start_penalty = 60;                         // seconds
constexpr std::string_view race_zone_usage = "<x y z> <radius 1-200>";
constexpr std::string_view race_start_usage =
    "race-start <SteamID64,SteamID64,...> [timeout <seconds 1-3600>] [false-start dnf|<seconds 1-60>]";

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
    // What a false start costs (race-arm's start zone): 0 a DNF, else seconds added; none: no check.
    std::optional<unsigned> false_start;
};
// "<ids> [timeout <seconds>] [false-start dnf|<seconds>]", options in any order, or why it is not one.
inline std::string parse_race_start(std::string_view argument, RaceStart &out) {
    out = {};
    auto [ids, rest] = split(argument);
    if (auto why = parse_race_ids(ids, out.ids); !why.empty()) return why;
    bool timeout{};
    while (!rest.empty()) {
        const auto [word, after] = split(rest);
        const auto [value, remaining] = split(after);
        const auto option = lower(word);
        const auto seconds = number(value);
        if (option == "timeout" && !timeout && seconds && *seconds && *seconds <= max_race_timeout) {
            out.timeout = static_cast<unsigned>(*seconds);
            timeout = true;
        } else if (option == "false-start" && !out.false_start && lower(value) == "dnf") {
            out.false_start = 0;
        } else if (option == "false-start" && !out.false_start && seconds && *seconds && *seconds <= max_false_start_penalty) {
            out.false_start = static_cast<unsigned>(*seconds);
        } else {
            return std::string(race_start_usage) + ".";
        }
        rest = remaining;
    }
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
// The distance from the start zone's centre of a racer outside it at the start: a false start.
// Inside, or no position that is a number: none.
inline std::optional<float> false_start_distance(const RaceZone &start, const std::array<float, 3> &position) noexcept {
    if (!std::all_of(position.begin(), position.end(), [](float v) { return std::isfinite(v); }) || in_zone(start, position))
        return {};
    float squared{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto d = position[i] - start.centre[i];
        squared += d * d;
    }
    return std::sqrt(squared);
}
struct Racer {
    std::uint64_t id{};
    std::string name;
    std::size_t next{};               // the checkpoint they have to pass next
    std::optional<std::uint64_t> ms; // their time once they cross the finish, with any penalty
    std::uint64_t seq{};              // the order they crossed it in
    unsigned place{};                 // 0 until their place is certain (see race_settle)
    unsigned penalty{};               // seconds added for a false start
    bool false_start{};
    bool disqualified{}; // a false start under "false-start dnf": races, never placed
    bool left{};         // left the server (or the world went): did not finish
};
// A name as one log line can carry it: control characters (a line break) become spaces.
inline std::string one_line(std::string_view text) {
    std::string out(text);
    for (auto &c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    return out;
}
// Tenths, rounded: 12.34 -> "12.3".
inline std::string race_tenths(float value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", static_cast<double>(value));
    return text;
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
// "[race] <name> (<id>) false start: 12.3 m from the start".
inline std::string race_false_start_line(const Racer &racer, float metres) {
    return "[race] " + one_line(racer.name) + " (" + std::to_string(racer.id) + ") false start: " + race_tenths(metres) +
           " m from the start";
}
// Marks the racers outside the start zone at the start (positions[i] is racers[i]'s, if known)
// and gives the lines to log. No zone or no false-start rule: nobody is marked.
inline std::vector<std::string> mark_false_starts(std::vector<Racer> &racers,
                                                  const std::vector<std::optional<std::array<float, 3>>> &positions,
                                                  const std::optional<RaceZone> &start, std::optional<unsigned> rule) {
    std::vector<std::string> lines;
    if (!start || !rule) return lines;
    for (std::size_t i = 0; i < racers.size() && i < positions.size(); ++i) {
        if (!positions[i]) continue;
        const auto metres = false_start_distance(*start, *positions[i]);
        if (!metres) continue;
        auto &racer = racers[i];
        racer.false_start = true;
        racer.disqualified = *rule == 0;
        racer.penalty = *rule;
        lines.push_back(race_false_start_line(racer, *metres));
    }
    return lines;
}
// Places the finishers whose place is now certain, in order, and returns them for their finish
// lines. A time (with its penalty) is certain once the race clock has reached it: whoever crosses
// later is slower. `flush` (the race is over) places everyone left. Ties go by who crossed first.
inline std::vector<Racer *> race_settle(std::vector<Racer> &racers, unsigned &finished, std::uint64_t now_ms, bool flush) {
    std::vector<Racer *> due;
    for (auto &racer : racers)
        if (racer.ms && !racer.place && !racer.disqualified && (flush || *racer.ms <= now_ms)) due.push_back(&racer);
    std::sort(due.begin(), due.end(), [](const Racer *a, const Racer *b) { return a->ms != b->ms ? *a->ms < *b->ms : a->seq < b->seq; });
    for (auto *racer : due) racer->place = ++finished;
    return due;
}
// "[race] ended: 1. A 48.3 s, 2. B 51.0 s (+5 s false start), C DNF (false start), D DNF": finishers
// by place, then the rest as they started.
inline std::string race_results_line(const std::vector<Racer> &racers) {
    std::vector<const Racer *> order;
    for (const auto &racer : racers) order.push_back(&racer);
    std::stable_sort(order.begin(), order.end(), [](const Racer *a, const Racer *b) {
        const bool a_placed = a->ms && a->place, b_placed = b->ms && b->place;
        if (a_placed != b_placed) return a_placed;
        return a_placed && a->place < b->place;
    });
    std::string line = "[race] ended:";
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto &racer = *order[i];
        line += i ? ", " : " ";
        if (racer.ms && racer.place) {
            line += std::to_string(racer.place) + ". " + one_line(racer.name) + " " + race_seconds(*racer.ms) + " s";
            if (racer.penalty) line += " (+" + std::to_string(racer.penalty) + " s false start)";
        } else {
            line += one_line(racer.name) + (racer.false_start ? " DNF (false start)" : " DNF");
        }
    }
    if (order.empty()) line += " nobody raced";
    return line;
}
// The race state the host keeps: the course on this map, and the running race.
struct Race {
    std::optional<RaceZone> finish;
    std::optional<RaceZone> start; // race-arm: where everyone must still be at race-start
    std::vector<RaceZone> checkpoints;
    std::uint64_t world{}; // the world (map change) the course was set in
    bool running{};
    std::uint64_t started{}, deadline{}; // microseconds, as the host's clock
    unsigned finished{}; // places given
    std::uint64_t crossed{}; // finish crossings, for Racer::seq
    std::vector<Racer> racers;
};
} // namespace dingosdk::server
