#include "server_host.h"
#include "server_hns.h"
#include <cstdio>

// Hide and seek: hns-start, hns-stop, hns-found and hns-status, the phases on their timers, finds
// and the seekers' wait at the start every tick, and who is sent whose poses (hns_hides, from
// broadcast). Only the log carries what happens; the round lives in memory and goes with the map.
namespace dingosdk::server {
namespace {
std::string hns_seconds(std::uint64_t us) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f s", static_cast<double>(us) / 1000000.0);
    return text;
}
std::string joined_names(const std::vector<std::string> &names) {
    std::string text;
    for (const auto &name : names) text += (text.empty() ? "" : ", ") + name;
    return text;
}
} // namespace

std::string Host::hns_name(const Guest &guest) const {
    auto name = guest_name(guest);
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
    return name;
}

std::optional<std::array<float, 3>> Host::hns_position(const Guest &guest) const {
    if (!guest.handshaken || !guest.world_ready || !guest.latest_root || !guest.pose_arrival || now_ < guest.pose_arrival ||
        now_ - guest.pose_arrival > hns_fresh_us)
        return std::nullopt;
    return guest.latest_root->position;
}

bool Host::hns_hides(std::uint64_t source, std::uint64_t recipient) const {
    if (hns_.phase == HnsPhase::off) return false;
    const auto role = [&](std::uint64_t id) {
        const auto found = hns_.roles.find(id);
        return found == hns_.roles.end() ? HnsRole::none : found->second;
    };
    const auto from = role(source), to = role(recipient);
    if (from != HnsRole::hider || to != HnsRole::seeker) return false;
    std::optional<float> distance2;
    const auto *a = guests_.contains(source) ? guests_.at(source).get() : nullptr;
    const auto *b = guests_.contains(recipient) ? guests_.at(recipient).get() : nullptr;
    const auto at = a ? hns_position(*a) : std::nullopt, there = b ? hns_position(*b) : std::nullopt;
    if (at && there) {
        float d{};
        for (unsigned i = 0; i < 3; ++i) d += ((*at)[i] - (*there)[i]) * ((*at)[i] - (*there)[i]);
        distance2 = d;
    }
    return hns_hidden(hns_.phase, from, to, distance2, hns_.settings.reveal);
}

std::string Host::hns_command(std::string_view name, std::string_view argument, std::uint64_t admin) {
    tick_hns(); // a map change or players leaving since come first
    auto &round = hns_;
    const auto by = [&] {
        const auto *who = admin ? find(admin) : nullptr;
        return !admin ? std::string("console") : who ? hns_name(*who) : std::to_string(admin);
    };
    const auto list = [&](HnsRole wanted) {
        std::vector<std::string> names;
        for (const auto &[id, role] : round.roles)
            if (role == wanted) names.push_back(round.names[id]);
        return joined_names(names);
    };
    if (name == "hns-status") {
        if (round.phase == HnsPhase::off) return "No hide and seek round.";
        const auto left = round.phase_ends > now_ ? (round.phase_ends - now_ + 999999) / 1000000 : 0;
        return std::string(round.phase == HnsPhase::hide ? "Hide" : "Seek") + " phase, " + std::to_string(left) +
               " s left. Seekers: " + list(HnsRole::seeker) + ". Hiders: " + list(HnsRole::hider) + ".";
    }
    if (name == "hns-stop") {
        if (round.phase == HnsPhase::off) return "No hide and seek round is running.";
        log_("[hns] stopped by " + by());
        round = {};
        return "Hide and seek stopped.";
    }
    if (name == "hns-found") {
        if (round.phase == HnsPhase::off) return "No hide and seek round is running.";
        if (round.phase != HnsPhase::seek) return "Hide and seek is not in the seek phase.";
        auto *guest = match_player(argument);
        if (!guest) return "No single connected player matches \"" + std::string(trim(argument)) + "\".";
        const auto id = guest->member.id;
        const auto found = round.roles.find(id);
        if (found == round.roles.end() || found->second != HnsRole::hider) return hns_name(*guest) + " is not hiding.";
        found->second = HnsRole::seeker;
        log_("[hns] " + hns_name(*guest) + " (" + std::to_string(id) + ") found by " + by());
        hns_end({id});
        return hns_name(*guest) + " is found.";
    }
    // hns-start
    if (round.phase != HnsPhase::off) return "A hide and seek round is running; hns-stop first.";
    HnsStart start;
    if (auto why = parse_hns_start(argument, start); !why.empty()) return why;
    std::array<float, 3> at{};
    if (start.at) {
        at = *start.at;
    } else {
        const auto *spot = match_player(start.at_player);
        const auto position = spot ? hns_position(*spot) : std::nullopt;
        if (!position) return "No position for " + (spot ? hns_name(*spot) : start.at_player) + ".";
        at = *position;
    }
    std::string skipped;
    std::map<std::uint64_t, HnsRole> roles;
    std::map<std::uint64_t, std::string> names;
    std::vector<Guest *> movers;
    unsigned seekers{}, hiders{};
    const auto take = [&](std::uint64_t id, HnsRole role) {
        auto *guest = find(id);
        const auto why = !guest || !guest->handshaken ? "not connected" : !guest->world_ready ? "not in the world yet" : nullptr;
        if (why) {
            skipped += (skipped.empty() ? " Skipped: " : ", ") + (guest ? hns_name(*guest) : std::to_string(id)) + " (" + why + ")";
            return;
        }
        roles[id] = role;
        names[id] = hns_name(*guest);
        movers.push_back(guest);
        ++(role == HnsRole::seeker ? seekers : hiders);
    };
    for (const auto id : start.seekers) take(id, HnsRole::seeker);
    for (const auto id : start.hiders) take(id, HnsRole::hider);
    if (!skipped.empty()) skipped += ".";
    if (seekers < 1 || hiders < 2) return "Not enough players: at least 1 seeker and 2 hiders." + skipped;
    round = {};
    round.phase = HnsPhase::hide;
    round.world = world_;
    round.started = round.last_hold = now_;
    round.phase_ends = now_ + std::uint64_t{start.settings.hide_s} * 1000000;
    round.at = at;
    round.settings = start.settings;
    round.roles = std::move(roles);
    round.names = std::move(names);
    teleport_ring(movers, at);
    const auto count = std::to_string(seekers) + " seeker(s), " + std::to_string(hiders) + " hider(s)";
    log_("[hns] started: " + count + ", hide " + std::to_string(start.settings.hide_s) + " s, seek " +
         std::to_string(start.settings.seek_s) + " s, by " + by());
    return "Hide and seek started: " + count + "." + skipped;
}

void Host::hns_end(const std::vector<std::uint64_t> &found_now) {
    auto &round = hns_;
    std::size_t seekers{};
    std::vector<std::uint64_t> hiders;
    for (const auto &[id, role] : round.roles) {
        if (role == HnsRole::seeker) ++seekers;
        if (role == HnsRole::hider) hiders.push_back(id);
    }
    const auto outcome = hns_outcome(seekers, hiders, found_now, round.phase == HnsPhase::seek && now_ >= round.phase_ends);
    if (outcome.end == HnsEnd::none) return;
    std::vector<std::string> winners;
    for (const auto id : outcome.winners) winners.push_back(round.names[id]);
    log_("[hns] ended: " + (winners.empty() ? std::string("nobody wins") : joined_names(winners) + " win"));
    round = {};
}

void Host::tick_hns() {
    auto &round = hns_;
    if (round.phase == HnsPhase::off) return;
    if (round.world != world_) {
        // A new map: the spot was on the old one, and so was everyone's hiding place.
        log_("[hns] the map changed");
        round = {};
        return;
    }
    // Gone: out of the round, even if they come back.
    for (auto it = round.roles.begin(); it != round.roles.end();) {
        const auto *guest = find(it->first);
        if (guest && guest->handshaken) {
            ++it;
            continue;
        }
        log_("[hns] " + round.names[it->first] + " (" + std::to_string(it->first) + ") left");
        it = round.roles.erase(it);
    }
    std::vector<std::uint64_t> found_now;
    if (round.phase == HnsPhase::hide) {
        // Seekers wait at the spot: one who has skated off is sent back, once a second at most.
        if (now_ - round.last_hold >= 1000000) {
            std::vector<Guest *> movers;
            for (const auto &[id, role] : round.roles) {
                if (role != HnsRole::seeker) continue;
                auto *guest = find(id);
                const auto position = guest ? hns_position(*guest) : std::nullopt;
                if (!position) continue;
                float d{};
                for (unsigned i = 0; i < 3; ++i) d += ((*position)[i] - round.at[i]) * ((*position)[i] - round.at[i]);
                if (d > round.settings.hold * round.settings.hold) movers.push_back(guest);
            }
            if (!movers.empty()) {
                teleport_ring(movers, round.at);
                round.last_hold = now_;
            }
        }
        if (now_ >= round.phase_ends) {
            round.phase = HnsPhase::seek;
            round.seek_started = now_;
            round.phase_ends = now_ + std::uint64_t{round.settings.seek_s} * 1000000;
            round.finds = {};
            log_("[hns] seek phase");
        }
    } else {
        std::vector<HnsPos> seekers, hiders;
        for (const auto &[id, role] : round.roles) {
            const auto *guest = find(id);
            const auto position = guest ? hns_position(*guest) : std::nullopt;
            if (!position) continue;
            (role == HnsRole::seeker ? seekers : hiders).push_back({id, *position});
        }
        for (const auto &[seeker, hider] : round.finds.step(now_, seekers, hiders, round.settings.find)) {
            round.roles[hider] = HnsRole::seeker;
            found_now.push_back(hider);
            log_("[hns] " + round.names[seeker] + " (" + std::to_string(seeker) + ") found " + round.names[hider] + " (" +
                 std::to_string(hider) + ") in " + hns_seconds(now_ - round.seek_started));
        }
    }
    hns_end(found_now);
}
} // namespace dingosdk::server
