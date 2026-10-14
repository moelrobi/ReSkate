#pragma once
#include "server_text.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Hide and seek (server_hns.cpp): seekers wait at a spot while hiders hide, then hunt them. A
// seeker is sent a hider's poses, sound, voice and effects only within `reveal` of them while
// seeking, and none while hiding; a seeker who stays within `find` of a hider for a second finds
// them, and the hider seeks from then on. The last hider wins. Everything here is pure, for the tests.
namespace dingosdk::server {
enum class HnsPhase { off, hide, seek };
enum class HnsRole { none, seeker, hider };
struct HnsSettings {
    unsigned hide_s = 60, seek_s = 300;
    float reveal = 25.f, find = 3.f, hold = 15.f;
};
constexpr std::uint64_t hns_dwell_us = 1000000; // within `find` this long is a find
constexpr std::uint64_t hns_fresh_us = 1000000; // a position older than this is not trusted
constexpr std::size_t max_hns_players = 64;
constexpr float max_hns_coordinate = 100000.f; // as teleport: far beyond any map, short of nonsense
struct HnsStart {
    std::vector<std::uint64_t> seekers, hiders;
    std::optional<std::array<float, 3>> at; // "at x y z"
    std::string at_player;                  // "at <player>": a name without spaces or a SteamID64
    HnsSettings settings;
};
constexpr std::string_view hns_start_usage =
    "hns-start <SteamID64,...> <SteamID64,...> at <x y z|player> [hide <s>] [seek <s>] [reveal <m>] [find <m>] [hold <m>]";

namespace hns_detail {
inline std::optional<float> decimal(std::string_view word) {
    float value{};
    const auto parsed = std::from_chars(word.data(), word.data() + word.size(), value);
    if (parsed.ptr != word.data() + word.size()) return {};
    if (parsed.ec == std::errc::result_out_of_range) return INFINITY; // "1e99": a number, and too far
    if (parsed.ec != std::errc{}) return {};
    return value;
}
// "1,2,3", or why it is not a list of SteamIDs.
inline std::string ids(std::string_view word, std::vector<std::uint64_t> &out) {
    if (word.empty()) return "Name the seekers and the hiders: " + std::string(hns_start_usage);
    while (true) {
        const auto comma = word.find(',');
        const auto id = number(word.substr(0, comma));
        if (!id || !*id) return "\"" + std::string(word.substr(0, comma)) + "\" is not a SteamID64.";
        out.push_back(*id);
        if (comma == std::string_view::npos) return {};
        word.remove_prefix(comma + 1);
    }
}
} // namespace hns_detail

// The argument of hns-start, or why it is not one.
inline std::string parse_hns_start(std::string_view argument, HnsStart &out) {
    out = {};
    auto [seekers, rest] = split(argument);
    if (auto why = hns_detail::ids(seekers, out.seekers); !why.empty()) return why;
    auto [hiders, after] = split(rest);
    if (auto why = hns_detail::ids(hiders, out.hiders); !why.empty()) return why;
    std::set<std::uint64_t> seen;
    for (const auto id : out.seekers)
        if (!seen.insert(id).second) return std::to_string(id) + " is named twice.";
    for (const auto id : out.hiders)
        if (!seen.insert(id).second) return std::to_string(id) + " is named twice.";
    if (seen.size() > max_hns_players) return "At most " + std::to_string(max_hns_players) + " players.";
    auto [at, spot] = split(after);
    if (lower(at) != "at" || spot.empty()) return "Say where everyone starts: " + std::string(hns_start_usage);
    // Three numbers are a spot; anything else is one player.
    std::array<float, 3> xyz{};
    auto settings = spot;
    bool coordinates = true;
    for (auto &value : xyz) {
        const auto [word, remaining] = split(settings);
        const auto parsed = hns_detail::decimal(word);
        if (word.empty() || !parsed) {
            coordinates = false;
            break;
        }
        value = *parsed;
        settings = remaining;
    }
    if (coordinates) {
        for (const auto value : xyz)
            if (!std::isfinite(value) || std::abs(value) > max_hns_coordinate)
                return "Coordinates are x y z, each a number from -100000 to 100000.";
        out.at = xyz;
    } else {
        const auto [player, remaining] = split(spot);
        out.at_player = std::string(player);
        settings = remaining;
    }
    while (!settings.empty()) {
        const auto [name, rest_after_name] = split(settings);
        const auto [value, remaining] = split(rest_after_name);
        settings = remaining;
        const auto key = lower(name);
        if (value.empty()) return "\"" + std::string(name) + "\" needs a value.";
        if (key == "hide" || key == "seek") {
            const auto seconds = number(value);
            const unsigned least = key == "hide" ? 10 : 30, most = key == "hide" ? 600 : 3600;
            if (!seconds || *seconds < least || *seconds > most)
                return key + " is " + std::to_string(least) + " to " + std::to_string(most) + " seconds.";
            (key == "hide" ? out.settings.hide_s : out.settings.seek_s) = static_cast<unsigned>(*seconds);
            continue;
        }
        const float least = key == "reveal" ? 5.f : key == "find" ? 1.f : 5.f;
        const float most = key == "reveal" ? 200.f : key == "find" ? 20.f : 100.f;
        float *target = key == "reveal" ? &out.settings.reveal : key == "find" ? &out.settings.find
                                                              : key == "hold"   ? &out.settings.hold
                                                                                : nullptr;
        if (!target) return "\"" + std::string(name) + "\" is not a setting: " + std::string(hns_start_usage);
        const auto metres = hns_detail::decimal(value);
        if (!metres || !std::isfinite(*metres) || *metres < least || *metres > most) {
            char text[64];
            std::snprintf(text, sizeof text, " is %g to %g m.", least, most);
            return key + text;
        }
        *target = *metres;
    }
    return {};
}

// Whether a packet of a skater whose role is `source` must not reach a player whose role is
// `recipient`. distance2: the square of the distance between them, none when either position is stale.
inline bool hns_hidden(HnsPhase phase, HnsRole source, HnsRole recipient, std::optional<float> distance2, float reveal) {
    if (phase == HnsPhase::off || source != HnsRole::hider || recipient != HnsRole::seeker) return false;
    if (phase == HnsPhase::hide) return true;
    return !distance2 || *distance2 > reveal * reveal;
}

struct HnsPos {
    std::uint64_t id{};
    std::array<float, 3> at{};
};
struct HnsFinds {
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> near_since; // (seeker, hider): near since
    // The (seeker, hider) pairs within `find` of each other for hns_dwell_us, at most one per hider
    // (the seeker with the smallest ID). Pairs no longer near are forgotten.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> step(std::uint64_t now, const std::vector<HnsPos> &seekers,
                                                               const std::vector<HnsPos> &hiders, float find) {
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> kept;
        std::map<std::uint64_t, std::uint64_t> found; // hider -> seeker
        for (const auto &s : seekers)
            for (const auto &h : hiders) {
                float distance{};
                for (unsigned i = 0; i < 3; ++i) distance += (s.at[i] - h.at[i]) * (s.at[i] - h.at[i]);
                if (distance > find * find) continue;
                const std::pair key{s.id, h.id};
                const auto earlier = near_since.find(key);
                const auto since = earlier == near_since.end() ? now : earlier->second;
                kept[key] = since;
                if (now - since >= hns_dwell_us) {
                    auto [it, added] = found.emplace(h.id, s.id);
                    if (!added) it->second = std::min(it->second, s.id);
                }
            }
        near_since = std::move(kept);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
        for (const auto &[hider, seeker] : found) {
            pairs.emplace_back(seeker, hider);
            std::erase_if(near_since, [&](const auto &entry) { return entry.first.second == hider; });
        }
        return pairs;
    }
};

enum class HnsEnd { none, last_hider, time_up, no_seekers, no_hiders };
struct HnsOutcome {
    HnsEnd end = HnsEnd::none;
    std::vector<std::uint64_t> winners;
};
// Whether the round is over, and who won: the last hider; the last ones, found together; or
// whoever is still hidden when the seekers are gone or time is up.
inline HnsOutcome hns_outcome(std::size_t seekers, const std::vector<std::uint64_t> &hiders_left,
                              const std::vector<std::uint64_t> &found_now, bool time_up) {
    if (hiders_left.size() <= 1) {
        if (!hiders_left.empty()) return {HnsEnd::last_hider, hiders_left};
        if (!found_now.empty()) return {HnsEnd::last_hider, found_now};
        return {HnsEnd::no_hiders, {}};
    }
    if (!seekers) return {HnsEnd::no_seekers, hiders_left};
    if (time_up) return {HnsEnd::time_up, hiders_left};
    return {};
}
} // namespace dingosdk::server
