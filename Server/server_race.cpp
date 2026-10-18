#include "server_host.h"
#include "server_race.h"
#include <cstdio>

// Races: race-finish, race-checkpoint, race-arm, race-hold, race-start and race-stop, and timing them every tick.
// Only the log carries the results; the course lives in memory and goes with the map.
namespace dingosdk::server {
namespace {
std::string zone_text(const RaceZone &zone) {
    char text[96];
    std::snprintf(text, sizeof text, "(%.1f, %.1f, %.1f) radius %.1f m", zone.centre[0], zone.centre[1], zone.centre[2], zone.radius);
    return text;
}
// The race is over: the finishers still waiting for a certain place get it, then the results.
void end_race(Race &race, std::uint64_t now, const Host::Log &log) {
    for (const auto *racer : race_settle(race.racers, race.finished, (now - race.started) / 1000, true)) log(race_finish_line(*racer));
    log(race_results_line(race.racers));
    race.running = false;
}
// A countdown that never reached GO: no race, no results.
void drop_countdown(Race &race) {
    race.pending.reset();
    race.go = 0;
    race.shown = 0;
    race.racers.clear();
    race.holds.clear();
    race.start.reset();
}
std::string spot_text(const std::array<float, 3> &spot) {
    char text[64];
    std::snprintf(text, sizeof text, "(%.1f, %.1f, %.1f)", spot[0], spot[1], spot[2]);
    return text;
}
} // namespace

// The newest pose's timestamp by the player's own clock (the pose kept when it went on to the
// others, in the same pass it arrived in), or 0 where there is none.
std::uint64_t Host::race_pose_sent(const Guest &guest) {
    return !guest.kept_poses.empty() && guest.kept_poses.back().kept_at == guest.pose_arrival ? guest.kept_poses.back().time_us : 0;
}

std::string Host::race_command(std::string_view name, std::string_view argument, std::uint64_t admin) {
    tick_race(); // a map change since drops the course first
    auto &race = race_;
    const auto set_course = [&]() -> std::string {
        if (race.running) return "A race is running; race-stop first.";
        if (race.pending) return "A race is counting down; race-stop first.";
        race.world = world_; // tick_race() has just dropped any course from an earlier map
        return {};
    };
    if (name == "race-finish") {
        if (argument.empty())
            return race.finish ? "Finish: " + zone_text(*race.finish) + ", " + std::to_string(race.checkpoints.size()) +
                                     " checkpoint(s) before it, on " + map_name() + "."
                               : "No finish zone. race-finish " + std::string(race_zone_usage);
        if (auto why = set_course(); !why.empty()) return why;
        if (lower(argument) == "off") {
            race.finish.reset();
            return "Finish zone cleared.";
        }
        RaceZone zone;
        if (auto why = parse_race_zone(argument, zone); !why.empty()) return why;
        race.finish = zone;
        return "Finish: " + zone_text(zone) + " on " + map_name() + ".";
    }
    if (name == "race-arm") {
        // The start zone, set as the countdown begins: whoever is outside it at race-start
        // has a false start. Only race-start reads it, so it may be armed while a race runs.
        if (argument.empty())
            return race.start ? "Start zone: " + zone_text(*race.start) + "." : "No start zone armed. race-arm " + std::string(race_zone_usage);
        if (lower(argument) == "off") {
            race.start.reset();
            return "Start zone cleared.";
        }
        RaceZone zone;
        if (auto why = parse_race_zone(argument, zone); !why.empty()) return why;
        race.start = zone;
        race.world = world_; // tick_race() has just dropped any course from an earlier map
        return "Start zone armed: " + zone_text(zone) + ".";
    }
    if (name == "race-checkpoint") {
        const auto [what, rest] = split(argument);
        if (what.empty()) {
            if (race.checkpoints.empty()) return "No checkpoints. race-checkpoint add " + std::string(race_zone_usage);
            std::string text = "Checkpoints in order:";
            for (std::size_t i = 0; i < race.checkpoints.size(); ++i)
                text += (i ? ", " : " ") + std::to_string(i + 1) + ". " + zone_text(race.checkpoints[i]);
            return text + ".";
        }
        if (auto why = set_course(); !why.empty()) return why;
        if (lower(what) == "clear") {
            race.checkpoints.clear();
            return "Checkpoints cleared.";
        }
        if (lower(what) != "add") return "race-checkpoint [add " + std::string(race_zone_usage) + " | clear]";
        if (race.checkpoints.size() >= max_race_checkpoints)
            return "At most " + std::to_string(max_race_checkpoints) + " checkpoints.";
        RaceZone zone;
        if (auto why = parse_race_zone(rest, zone); !why.empty()) return why;
        race.checkpoints.push_back(zone);
        return "Checkpoint " + std::to_string(race.checkpoints.size()) + ": " + zone_text(zone) + ".";
    }
    const auto by = [&] {
        const auto *who = admin ? find(admin) : nullptr;
        return !admin ? std::string("console") : who ? one_line(guest_name(*who)) : std::to_string(admin);
    };
    if (name == "race-hold") {
        // Racers kept on their start spot until race-start: sent there now, and back whenever
        // they leave it (tick_race).
        if (argument.empty()) {
            if (race.holds.empty()) return "Nobody is held. " + std::string(race_hold_usage);
            std::string text = "Held:";
            for (std::size_t i = 0; i < race.holds.size(); ++i) {
                const auto &hold = race.holds[i];
                const auto *guest = find(hold.id);
                char rule[64];
                std::snprintf(rule, sizeof rule, " radius %.1f m, drop %.1f m", hold.radius, hold.drop);
                text += (i ? ", " : " ") + (guest ? one_line(guest_name(*guest)) : std::to_string(hold.id)) + " at " +
                        spot_text(hold.spot) + rule;
            }
            return text + ".";
        }
        if (lower(argument) == "clear") {
            const auto held = race.holds.size();
            race.holds.clear();
            return held ? "Holds cleared (" + std::to_string(held) + ")." : "Nobody was held.";
        }
        if (race.running) return "A race is running; race-stop first.";
        std::vector<RaceHold> holds;
        if (auto why = parse_race_hold(argument, holds); !why.empty()) return why;
        std::string skipped;
        std::erase_if(holds, [&](const RaceHold &hold) {
            const auto *guest = find(hold.id);
            if (guest && guest->handshaken) return false;
            skipped += (skipped.empty() ? " Skipped: " : ", ") + std::to_string(hold.id) + " (not connected)";
            return true;
        });
        if (!skipped.empty()) skipped += ".";
        if (holds.empty()) return "Nobody to hold." + skipped;
        if (!merge_race_holds(race.holds, holds)) return "At most " + std::to_string(max_racers) + " racers held.";
        race.world = world_; // tick_race() has just dropped any course from an earlier map
        log_(race_hold_line(race.holds.size()));
        tick_race(); // sends them to their spots now
        return "Holding " + std::to_string(race.holds.size()) + " racer(s)." + skipped;
    }
    if (name == "race-stop") {
        if (race.pending) {
            log_("[race] countdown stopped by " + by());
            drop_countdown(race);
            return "Countdown stopped.";
        }
        if (!race.running) {
            if (race.holds.empty()) return "No race is running.";
            race.holds.clear();
            return "No race is running. Holds cleared.";
        }
        log_("[race] stopped by " + by());
        end_race(race, now_, log_);
        race.start.reset();
        race.holds.clear();
        return "Race stopped.";
    }
    // race-start
    if (race.running) return "A race is running; race-stop first.";
    if (race.pending) return "A race is counting down; race-stop first.";
    if (!race.finish) return "Set the finish first: race-finish " + std::string(race_zone_usage);
    RaceStart start;
    if (auto why = parse_race_start(argument, start); !why.empty()) return why;
    std::vector<Racer> racers;
    std::vector<std::optional<std::array<float, 3>>> positions; // where each racer is at the start
    std::string skipped;
    for (const auto id : start.ids) {
        const auto *guest = find(id);
        const auto why = !guest || !guest->handshaken ? "not connected" : !guest->world_ready ? "not in the world yet" : nullptr;
        if (why)
            skipped += (skipped.empty() ? " Skipped: " : ", ") + (guest ? one_line(guest_name(*guest)) : std::to_string(id)) + " (" + why + ")";
        else {
            racers.push_back({.id = id, .name = guest_name(*guest)});
            positions.push_back(guest->latest_root ? std::optional(guest->latest_root->position) : std::nullopt);
        }
    }
    if (!skipped.empty()) skipped += ".";
    if (racers.empty()) return "Nobody to race." + skipped;
    race.racers = std::move(racers);
    race.started_by = by();
    if (start.countdown) {
        // GO from the server's own clock: the cards each second, the start in tick_race().
        race.pending = start;
        race.go = now_ + std::uint64_t{start.countdown} * 1000000;
        race.shown = 0;
        log_("[race] countdown " + std::to_string(start.countdown) + " s");
        tick_race(); // the first card now
        return "Countdown " + std::to_string(start.countdown) + " s: " + std::to_string(race.racers.size()) + " racer(s), timeout " +
               std::to_string(start.timeout) + " s." + (start.false_start && !race.start ? " No start zone armed yet." : "") + skipped;
    }
    return begin_race(start) + skipped;
}

// The start: holds end, then the false-start check, then the clock runs from now.
std::string Host::begin_race(const RaceStart &start) {
    auto &race = race_;
    race.holds.clear();
    std::vector<std::optional<std::array<float, 3>>> positions; // where each racer is at the start
    for (auto &racer : race.racers) {
        const auto *guest = find(racer.id);
        if (!guest || !guest->handshaken) racer.left = true; // went during the countdown
        const bool placed = guest && guest->handshaken && guest->world_ready && guest->latest_root;
        positions.push_back(placed ? std::optional(guest->latest_root->position) : std::nullopt);
        if (!placed) continue;
        // Their first move is from where they are at GO.
        racer.last = guest->latest_root->position;
        racer.seen = guest->pose_arrival;
        racer.last_sent = race_pose_sent(*guest);
        if (racer.last_sent) racer.clock.sample(racer.last_sent, racer.seen);
    }
    const auto false_starts = mark_false_starts(race.racers, positions, race.start, start.false_start);
    std::string false_named;
    for (const auto &racer : race.racers)
        if (racer.false_start) false_named += (false_named.empty() ? " False start: " : ", ") + one_line(racer.name);
    if (!false_named.empty()) false_named += ".";
    if (start.false_start && !race.start) false_named = " No start zone armed; no false-start check.";
    race.pending.reset();
    race.go = 0;
    race.running = true;
    race.finished = 0;
    race.crossed = 0;
    race.start.reset(); // armed for this start only
    race.started = now_;
    race.deadline = now_ + std::uint64_t{start.timeout} * 1000000;
    const auto count = std::to_string(race.racers.size()) + " racer(s)";
    log_("[race] started: " + count + ", " + std::to_string(race.checkpoints.size()) + " checkpoint(s), timeout " +
         std::to_string(start.timeout) + " s, by " + race.started_by);
    for (const auto &line : false_starts) log_(line);
    return "Race started: " + count + ", timeout " + std::to_string(start.timeout) + " s." + false_named;
}

void Host::tick_race() {
    auto &race = race_;
    if (race.world != world_ && (race.finish || race.start || !race.checkpoints.empty() || race.running || race.pending ||
                                 !race.holds.empty())) {
        // A new map: the course was on the old one, and so was everyone's position.
        if (race.running) {
            log_("[race] the map changed");
            end_race(race, now_, log_);
        }
        if (race.pending) log_("[race] countdown dropped: the map changed");
        race = {};
        race.world = world_;
        return;
    }
    // Held racers: sent to their spot once in the world, and back whenever they are off it.
    std::erase_if(race.holds, [&](RaceHold &hold) {
        auto *guest = find(hold.id);
        if (!guest || !guest->handshaken) return true; // left: their hold goes with them
        if (!guest->world_ready) return false;
        const auto metres = guest->latest_root ? race_hold_off(hold, guest->latest_root->position, guest->pose_arrival, now_) : std::nullopt;
        if (hold.teleported && !metres) return false;
        if (metres) log_(race_pull_line(guest_name(*guest), hold.id, *metres));
        teleport_ring({guest}, {hold.spot[0], hold.spot[1] + race_hold_lift - 1.f, hold.spot[2]}, 0.f); // it adds 1 m
        hold.teleported = std::max<std::uint64_t>(now_, 1);
        return false;
    });
    if (race.pending) {
        const auto left = race_countdown_left(race.go, now_);
        if (race.shown && left >= race.shown) return;
        race.shown = left;
        if (left) {
            announce(race_countdown_card(left));
            return;
        }
        const auto start = *race.pending;
        begin_race(start); // the clock starts with the GO card
        announce(race_countdown_card(0));
    }
    if (!race.running) return;
    const auto elapsed = (now_ - race.started) / 1000;
    for (auto &racer : race.racers) {
        if (racer.ms || racer.left || racer.disqualified) continue; // a DNF false start races but holds nothing up
        const auto *guest = find(racer.id);
        if (!guest || !guest->handshaken) {
            racer.left = true; // once gone, a DNF even if they come back
            continue;
        }
        if (!guest->world_ready || !guest->latest_root || guest->pose_arrival == racer.seen) continue;
        // Their move since the pose before, timed by their poses' own timestamps: a crossing
        // between two poses counts at the moment it happened, not when the server read it.
        // Without timestamps (never, as the server is now), the arrival times; a clock that
        // started over, or one pose with and one without: only where they are now counts.
        const auto &position = guest->latest_root->position;
        const auto sent = race_pose_sent(*guest);
        const bool going = sent && racer.clock.sample(sent, guest->pose_arrival);
        auto from_position = racer.last;
        if (sent ? !going || !racer.last_sent : racer.last_sent != 0) from_position.reset();
        const auto to_sent = sent ? sent : guest->pose_arrival;
        const auto from_sent = sent ? racer.last_sent : racer.seen;
        const auto offset = sent ? racer.clock.offset : 0;
        const auto at = [&](float t) { return race_crossing_ms(from_position ? from_sent : to_sent, to_sent, t, offset, race.started, now_); };
        const auto from = racer.next;
        const auto crossings = race_move(racer.next, race.checkpoints, *race.finish, from_position, position);
        racer.last = position;
        racer.seen = guest->pose_arrival;
        racer.last_sent = sent;
        std::vector<std::uint64_t> times;
        for (const auto t : crossings.checkpoints) times.push_back(at(t));
        for (const auto &line : race_checkpoint_lines(racer, from, race.checkpoints.size(), times)) log_(line);
        if (!crossings.finish) continue;
        racer.ms = at(*crossings.finish) + std::uint64_t{racer.penalty} * 1000;
        racer.seq = ++race.crossed;
    }
    // A finish line once its place is certain: at once, or for a penalised time once the clock reaches it.
    for (const auto *racer : race_settle(race.racers, race.finished, elapsed > race_settle_margin_ms ? elapsed - race_settle_margin_ms : 0, false))
        log_(race_finish_line(*racer));
    // Everyone finished or left, or time is up: the rest did not finish.
    if (now_ >= race.deadline || std::none_of(race.racers.begin(), race.racers.end(),
                                              [](const Racer &r) { return !r.ms && !r.left && !r.disqualified; }))
        end_race(race, now_, log_);
}
} // namespace dingosdk::server
