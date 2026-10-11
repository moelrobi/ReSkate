#include "server_host.h"
#include "server_race.h"
#include <cstdio>

// Races: race-finish, race-checkpoint, race-arm, race-start and race-stop, and timing them every tick.
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
} // namespace

std::string Host::race_command(std::string_view name, std::string_view argument, std::uint64_t admin) {
    tick_race(); // a map change since drops the course first
    auto &race = race_;
    const auto set_course = [&]() -> std::string {
        if (race.running) return "A race is running; race-stop first.";
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
    if (name == "race-stop") {
        if (!race.running) return "No race is running.";
        log_("[race] stopped by " + by());
        end_race(race, now_, log_);
        race.start.reset();
        return "Race stopped.";
    }
    // race-start
    if (race.running) return "A race is running; race-stop first.";
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
    const auto false_starts = mark_false_starts(racers, positions, race.start, start.false_start);
    std::string false_named;
    for (const auto &racer : racers)
        if (racer.false_start) false_named += (false_named.empty() ? " False start: " : ", ") + one_line(racer.name);
    if (!false_named.empty()) false_named += ".";
    if (start.false_start && !race.start) false_named = " No start zone armed; no false-start check.";
    race.racers = std::move(racers);
    race.running = true;
    race.finished = 0;
    race.crossed = 0;
    race.start.reset(); // armed for this start only
    race.started = now_;
    race.deadline = now_ + std::uint64_t{start.timeout} * 1000000;
    const auto count = std::to_string(race.racers.size()) + " racer(s)";
    log_("[race] started: " + count + ", " + std::to_string(race.checkpoints.size()) + " checkpoint(s), timeout " +
         std::to_string(start.timeout) + " s, by " + by());
    for (const auto &line : false_starts) log_(line);
    return "Race started: " + count + ", timeout " + std::to_string(start.timeout) + " s." + false_named + skipped;
}

void Host::tick_race() {
    auto &race = race_;
    if (race.world != world_ && (race.finish || race.start || !race.checkpoints.empty() || race.running)) {
        // A new map: the course was on the old one, and so was everyone's position.
        if (race.running) {
            log_("[race] the map changed");
            end_race(race, now_, log_);
        }
        race = {};
        race.world = world_;
        return;
    }
    if (!race.running) return;
    for (auto &racer : race.racers) {
        if (racer.ms || racer.left || racer.disqualified) continue; // a DNF false start races but holds nothing up
        const auto *guest = find(racer.id);
        if (!guest || !guest->handshaken) {
            racer.left = true; // once gone, a DNF even if they come back
            continue;
        }
        if (!guest->world_ready || !guest->latest_root) continue;
        if (!race_step(racer.next, race.checkpoints, *race.finish, guest->latest_root->position)) continue;
        racer.ms = (now_ - race.started) / 1000 + std::uint64_t{racer.penalty} * 1000;
        racer.seq = ++race.crossed;
    }
    // A finish line once its place is certain: at once, or for a penalised time once the clock reaches it.
    for (const auto *racer : race_settle(race.racers, race.finished, (now_ - race.started) / 1000, false))
        log_(race_finish_line(*racer));
    // Everyone finished or left, or time is up: the rest did not finish.
    if (now_ >= race.deadline || std::none_of(race.racers.begin(), race.racers.end(),
                                              [](const Racer &r) { return !r.ms && !r.left && !r.disqualified; }))
        end_race(race, now_, log_);
}
} // namespace dingosdk::server
