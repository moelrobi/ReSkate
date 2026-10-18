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
// way, and players timed from race-start to the finish; before it, racers held on their start spot
// and the server's own countdown. Everything here is pure, for the tests.
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
constexpr unsigned max_race_countdown = 10;                              // seconds
constexpr float default_hold_radius = 3.f, default_hold_drop = 4.f;      // metres
// A held racer lands this far above their spot, as the toolkit's start teleport did.
constexpr float race_hold_lift = 2.f;
// After a hold's teleport its racer's poses count only from this long on (the ones from before
// still arriving), and they are sent back at most this often: the hold does not fight their game.
constexpr std::uint64_t race_hold_grace_us = 1000000, race_hold_repeat_us = 1500000;
// A move longer than this between two poses is a teleport, not a ride: only where it ends counts.
constexpr float max_race_move = 50.f;
// Crossing times come from the poses' own timestamps, and a pose can arrive late: a place is
// given only once the race clock is this far past a time, so a crossing read later but made
// earlier still comes first.
constexpr std::uint64_t race_settle_margin_ms = 1000;
constexpr std::string_view race_zone_usage = "<x y z> <radius 1-200>";
constexpr std::string_view race_start_usage =
    "race-start <SteamID64,SteamID64,...> [timeout <seconds 1-3600>] [false-start dnf|<seconds 1-60>] [countdown <seconds 1-10>]";
constexpr std::string_view race_hold_usage =
    "race-hold <SteamID64> <x y z> [<SteamID64> <x y z> ...] [radius <m 1-200>] [drop <m 1-200>] | race-hold clear";

// One number word, or why it is not one. "1e99" is a number, and infinitely far.
inline std::string parse_race_number(std::string_view word, float &value) {
    const auto parsed = std::from_chars(word.data(), word.data() + word.size(), value);
    if (parsed.ptr != word.data() + word.size()) return "\"" + std::string(word) + "\" is not a number.";
    if (parsed.ec == std::errc::result_out_of_range) value = INFINITY;
    else if (parsed.ec != std::errc{}) return "\"" + std::string(word) + "\" is not a number.";
    return {};
}
inline bool race_coordinates_ok(const std::array<float, 3> &position) noexcept {
    return std::all_of(position.begin(), position.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= max_race_coordinate; });
}
constexpr std::string_view race_coordinates_rule = "Coordinates are x y z, each a number from -100000 to 100000.";

// "x y z radius", or why it is not one.
inline std::string parse_race_zone(std::string_view argument, RaceZone &out) {
    std::array<float, 4> values{};
    auto rest = trim(argument);
    for (auto &value : values) {
        const auto [word, remaining] = split(rest);
        if (word.empty()) return "A zone is " + std::string(race_zone_usage) + ".";
        if (auto why = parse_race_number(word, value); !why.empty()) return why;
        rest = remaining;
    }
    if (!rest.empty()) return "A zone is " + std::string(race_zone_usage) + ".";
    if (!race_coordinates_ok({values[0], values[1], values[2]})) return std::string(race_coordinates_rule);
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
    unsigned countdown{}; // seconds the server counts down before GO; 0: the race starts at once
};
// "<ids> [timeout <seconds>] [false-start dnf|<seconds>] [countdown <seconds>]", options in any
// order, or why it is not one.
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
        } else if (option == "countdown" && !out.countdown && seconds && *seconds && *seconds <= max_race_countdown) {
            out.countdown = static_cast<unsigned>(*seconds);
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
// Where along the move a -> b (0 at a, 1 at b), from `from` on, a racer is first inside or on the
// sphere: none if they are not between `from` and b. Anything that is not a number is never inside.
inline std::optional<float> zone_entry(const RaceZone &zone, const std::array<float, 3> &a, const std::array<float, 3> &b,
                                       float from = 0.f) noexcept {
    float aa{}, ab{}, cc{}; // |b - a|^2, (a - centre).(b - a), |a - centre|^2 - r^2
    for (std::size_t i = 0; i < 3; ++i) {
        const auto d = b[i] - a[i], f = a[i] - zone.centre[i];
        aa += d * d;
        ab += f * d;
        cc += f * f;
    }
    cc -= zone.radius * zone.radius;
    if (!std::isfinite(aa) || !std::isfinite(ab) || !std::isfinite(cc)) return {};
    if (aa <= 1e-12f) return cc <= 0.f ? std::optional(from) : std::nullopt; // standing still
    const auto disc = ab * ab - aa * cc; // of aa t^2 + 2 ab t + cc = 0
    if (disc < 0.f) return {};
    const auto root = std::sqrt(disc);
    const auto in = (-ab - root) / aa, out = (-ab + root) / aa;
    if (out < from || in > 1.f) return {};
    return std::max(in, from);
}
// One racer's move from `from` (their pose before; none: only where they are now counts) to `to`:
// the next checkpoints they pass on it, in order (several at once where they overlap), and whether
// they reach the finish after the last, each with where along the move (0..1) it happened.
struct RaceCrossings {
    std::vector<float> checkpoints; // one per checkpoint passed, in order
    std::optional<float> finish;
};
inline RaceCrossings race_move(std::size_t &next, const std::vector<RaceZone> &checkpoints, const RaceZone &finish,
                               const std::optional<std::array<float, 3>> &from, const std::array<float, 3> &to) {
    RaceCrossings out;
    auto a = from.value_or(to);
    float moved{};
    for (std::size_t i = 0; i < 3; ++i) moved += (to[i] - a[i]) * (to[i] - a[i]);
    float t = 0.f;
    if (!from || !(moved <= max_race_move * max_race_move)) { // a teleport (or nonsense): where they are now, at its time
        a = to;
        t = 1.f;
    }
    while (next < checkpoints.size()) {
        const auto at = zone_entry(checkpoints[next], a, to, t);
        if (!at) return out;
        t = *at;
        out.checkpoints.push_back(t);
        ++next;
    }
    out.finish = zone_entry(finish, a, to, t);
    return out;
}
// One racer at `position`: passes the next checkpoints in order (several at once where they
// overlap), then whether they are in the finish with every checkpoint behind them.
inline bool race_step(std::size_t &next, const std::vector<RaceZone> &checkpoints, const RaceZone &finish,
                      const std::array<float, 3> &position) {
    return race_move(next, checkpoints, finish, std::nullopt, position).finish.has_value();
}
// A player's game clock against the host's, from their poses' timestamps (as SpeedCheck): the
// smallest gap between when a pose arrived and when it was sent is the clocks' offset plus the
// least delay, so a pose's own time on the host's clock is where they really were then, however
// late the server read it. A clock that went back (a restarted game) or a long pause starts over.
struct PoseClock {
    static constexpr std::uint64_t gap_us = 2500000;
    std::int64_t offset{};
    std::uint64_t last_sent{}, last_arrived{};
    bool started{};
    // One pose; false when the clock started over, so an earlier pose's time cannot be compared.
    bool sample(std::uint64_t sent, std::uint64_t arrived) noexcept {
        const auto gap = static_cast<std::int64_t>(arrived) - static_cast<std::int64_t>(sent);
        const bool going = started && sent >= last_sent && arrived >= last_arrived && arrived - last_arrived <= gap_us;
        offset = going ? std::min(offset, gap) : gap;
        started = true;
        last_sent = sent;
        last_arrived = arrived;
        return going;
    }
};
// The race clock (ms since `started`) at `t` along a move between poses sent at from_sent and
// to_sent by the racer's clock, `offset` from the host's; never before the start or after `now`.
inline std::uint64_t race_crossing_ms(std::uint64_t from_sent, std::uint64_t to_sent, float t, std::int64_t offset,
                                      std::uint64_t started, std::uint64_t now) noexcept {
    const auto span = static_cast<double>(to_sent) - static_cast<double>(from_sent);
    auto host = static_cast<double>(from_sent) + std::clamp(static_cast<double>(t), 0.0, 1.0) * span + static_cast<double>(offset);
    host = std::clamp(host, static_cast<double>(started), static_cast<double>(std::max(now, started)));
    return static_cast<std::uint64_t>((host - static_cast<double>(started)) / 1000.0 + 0.5);
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
    // Their pose before, for the move to the next: where, when it arrived, when they sent it (their
    // clock, from `clock`; 0: no timestamp, then the arrival times).
    std::optional<std::array<float, 3>> last;
    std::uint64_t seen{}, last_sent{};
    PoseClock clock;
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
// Milliseconds as seconds with two decimals, rounded: 48265 -> "48.27", 48050 -> "48.05".
inline std::string race_seconds(std::uint64_t ms) {
    const auto hundredths = (ms + 5) / 10;
    const auto part = hundredths % 100;
    return std::to_string(hundredths / 100) + (part < 10 ? ".0" : ".") + std::to_string(part);
}
inline std::string race_finish_line(const Racer &racer) {
    return "[race] " + one_line(racer.name) + " (" + std::to_string(racer.id) + ") finished " + std::to_string(racer.place) +
           " in " + race_seconds(racer.ms.value_or(0)) + " s";
}
// "[race] <name> (<id>) checkpoint <k>/<total> in 12.34 s" for each checkpoint passed since `from`
// (racer.next before the step), in order, ms[i] the race clock's time of the i-th (without any
// penalty; the last one's for any without a time of its own).
inline std::vector<std::string> race_checkpoint_lines(const Racer &racer, std::size_t from, std::size_t total,
                                                      const std::vector<std::uint64_t> &ms) {
    std::vector<std::string> lines;
    for (auto k = from + 1; k <= racer.next; ++k) {
        const auto i = k - from - 1;
        const auto time = i < ms.size() ? ms[i] : ms.empty() ? 0 : ms.back();
        lines.push_back("[race] " + one_line(racer.name) + " (" + std::to_string(racer.id) + ") checkpoint " + std::to_string(k) + "/" +
                        std::to_string(total) + " in " + race_seconds(time) + " s");
    }
    return lines;
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
// race-hold: a racer kept on their start spot until race-start.
struct RaceHold {
    std::uint64_t id{};
    std::array<float, 3> spot{};
    float radius = default_hold_radius, drop = default_hold_drop;
    std::uint64_t teleported{}; // when they were last sent to the spot; 0: not yet (not in the world)
};
// "<SteamID64> <x y z> [<SteamID64> <x y z> ...] [radius <m>] [drop <m>]" (radius and drop for
// every racer in it), or why it is not one. A racer named twice is held where they are named last.
inline std::string parse_race_hold(std::string_view argument, std::vector<RaceHold> &out) {
    out.clear();
    const auto usage = std::string(race_hold_usage) + ".";
    auto rest = trim(argument);
    if (rest.empty()) return usage;
    std::optional<float> radius, drop;
    while (!rest.empty()) {
        const auto [word, after] = split(rest);
        const auto option = lower(word);
        if (option == "radius" || option == "drop") {
            auto &setting = option == "radius" ? radius : drop;
            const auto [value, remaining] = split(after);
            float metres{};
            if (setting || value.empty() || !parse_race_number(value, metres).empty()) return usage;
            if (!std::isfinite(metres) || metres < min_race_radius || metres > max_race_radius)
                return "The " + option + " is 1 to 200 m.";
            setting = metres;
            rest = remaining;
            continue;
        }
        if (radius || drop) return usage; // the options come last
        const auto id = number(word);
        if (!id || !*id) return "\"" + std::string(word) + "\" is not a SteamID64. " + usage;
        RaceHold hold{.id = *id};
        rest = after;
        for (auto &value : hold.spot) {
            const auto [coordinate, remaining] = split(rest);
            if (coordinate.empty()) return usage;
            if (auto why = parse_race_number(coordinate, value); !why.empty()) return why;
            rest = remaining;
        }
        if (!race_coordinates_ok(hold.spot)) return std::string(race_coordinates_rule);
        std::erase_if(out, [&](const RaceHold &h) { return h.id == hold.id; });
        out.push_back(hold);
        if (out.size() > max_racers) return "At most " + std::to_string(max_racers) + " racers held.";
    }
    if (out.empty()) return usage;
    for (auto &hold : out) {
        hold.radius = radius.value_or(default_hold_radius);
        hold.drop = drop.value_or(default_hold_drop);
    }
    return {};
}
// Holds from one race-hold, merged into the ones there: a racer held again is held anew.
// False (nothing changed) when that would hold more than max_racers.
inline bool merge_race_holds(std::vector<RaceHold> &holds, const std::vector<RaceHold> &more) {
    auto merged = holds;
    for (const auto &hold : more) {
        std::erase_if(merged, [&](const RaceHold &h) { return h.id == hold.id; });
        merged.push_back(hold);
    }
    if (merged.size() > max_racers) return false;
    holds = std::move(merged);
    return true;
}
// How far a held racer at `position` (a pose that arrived at pose_at) is from their spot when
// they are to be sent back to it now: more than `radius` off it across, or more than `drop` below
// it (through the ground). Not before race_hold_repeat_us since the last teleport, and only by a
// pose that arrived race_hold_grace_us after it. Never before the first teleport.
inline std::optional<float> race_hold_off(const RaceHold &hold, const std::array<float, 3> &position, std::uint64_t pose_at,
                                          std::uint64_t now) noexcept {
    if (!hold.teleported || now < hold.teleported + race_hold_repeat_us || pose_at < hold.teleported + race_hold_grace_us) return {};
    if (!std::all_of(position.begin(), position.end(), [](float v) { return std::isfinite(v); })) return {};
    const auto dx = position[0] - hold.spot[0], dy = position[1] - hold.spot[1], dz = position[2] - hold.spot[2];
    if (dx * dx + dz * dz <= hold.radius * hold.radius && dy >= -hold.drop) return {};
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// "[race] hold: 3 racer(s)".
inline std::string race_hold_line(std::size_t held) { return "[race] hold: " + std::to_string(held) + " racer(s)"; }
// "[race] <name> (<id>) back to the start: 4.2 m off".
inline std::string race_pull_line(std::string_view name, std::uint64_t id, float metres) {
    return "[race] " + one_line(name) + " (" + std::to_string(id) + ") back to the start: " + race_tenths(metres) + " m off";
}
// The countdown's seconds still to go at `now` before GO at `go`, rounded up: what its card shows
// ("5" ... "1"), 0 at GO.
inline unsigned race_countdown_left(std::uint64_t go, std::uint64_t now) noexcept {
    return now >= go ? 0 : static_cast<unsigned>((go - now + 999999) / 1000000);
}
inline std::string race_countdown_card(unsigned left) { return left ? std::to_string(left) : "GO!"; }
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
    std::string started_by;  // who started it, for the log line (at GO with a countdown)
    std::vector<Racer> racers;
    std::vector<RaceHold> holds; // race-hold, until the race starts
    // race-start ... countdown: the start waiting for GO (racers above, not yet running), and the
    // number on the card shown last.
    std::optional<RaceStart> pending;
    std::uint64_t go{};
    unsigned shown{};
};
} // namespace dingosdk::server
