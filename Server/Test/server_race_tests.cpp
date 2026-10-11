#include "Server/server_race.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace dingosdk::server;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
Racer finished(std::uint64_t id, std::string name, unsigned place, std::uint64_t ms) {
    Racer r{.id = id, .name = std::move(name)};
    r.place = place;
    r.ms = ms;
    return r;
}
} // namespace

int main() {
    // race-finish / race-checkpoint <x y z> <radius>
    RaceZone z;
    check(parse_race_zone("-247 2172 -259 10", z).empty() && z.centre[0] == -247.f && z.centre[1] == 2172.f &&
              z.centre[2] == -259.f && z.radius == 10.f,
          "x y z r, in order");
    check(parse_race_zone("  0.5 -1.25 1e3   2.5 ", z).empty() && z.centre[2] == 1000.f && z.radius == 2.5f, "decimals, extra spaces");
    check(parse_race_zone("100000 -100000 0 1", z).empty(), "the coordinate limit and radius 1 accepted");
    check(parse_race_zone("0 0 0 200", z).empty(), "radius 200 accepted");
    check(!parse_race_zone("", z).empty(), "empty refused");
    check(!parse_race_zone("0 0 0", z).empty(), "no radius refused");
    check(!parse_race_zone("0 0 0 5 5", z).empty(), "five numbers refused");
    check(!parse_race_zone("0 0 x 5", z).empty(), "a word refused");
    check(!parse_race_zone("0 0 1x 5", z).empty(), "a number with junk refused");
    check(!parse_race_zone("nan 0 0 5", z).empty(), "NaN refused");
    check(!parse_race_zone("0 inf 0 5", z).empty(), "inf refused");
    check(!parse_race_zone("0 0 -infinity 5", z).empty(), "-inf refused");
    check(!parse_race_zone("1e9 0 0 5", z).empty(), "an absurd coordinate refused");
    check(!parse_race_zone("0 1e99 0 5", z).empty(), "an out-of-range coordinate refused");
    check(!parse_race_zone("100001 0 0 5", z).empty(), "just past the limit refused");
    check(!parse_race_zone("0 0 0 0.5", z).empty(), "radius under 1 refused");
    check(!parse_race_zone("0 0 0 201", z).empty(), "radius over 200 refused");
    check(!parse_race_zone("0 0 0 nan", z).empty(), "NaN radius refused");
    check(!parse_race_zone("0 0 0 -5", z).empty(), "negative radius refused");

    // race-start <ids> [timeout <s>]
    RaceStart s;
    check(parse_race_start("76561198000000001", s).empty() && s.ids == std::vector<std::uint64_t>{76561198000000001} &&
              s.timeout == default_race_timeout && default_race_timeout == 300,
          "one SteamID64, default timeout 300 s");
    check(parse_race_start("1,2,3 timeout 60", s).empty() && s.ids == std::vector<std::uint64_t>{1, 2, 3} && s.timeout == 60,
          "a list and a timeout");
    check(parse_race_start("  5,5,6   TIMEOUT 3600 ", s).empty() && s.ids == std::vector<std::uint64_t>{5, 6} && s.timeout == 3600,
          "duplicates dropped, extra spaces, timeout limit");
    check(!parse_race_start("", s).empty(), "empty refused");
    check(!parse_race_start("1,,2", s).empty(), "empty list entry refused");
    check(!parse_race_start("1,", s).empty(), "trailing comma refused");
    check(!parse_race_start(",1", s).empty(), "leading comma refused");
    check(!parse_race_start("0,1", s).empty(), "id 0 refused");
    check(!parse_race_start("1,-2", s).empty(), "a negative id refused");
    check(!parse_race_start("Bob", s).empty(), "a name refused");
    check(!parse_race_start("1, 2", s).empty(), "a space in the list refused");
    check(!parse_race_start("1 timeout", s).empty(), "timeout without seconds refused");
    check(!parse_race_start("1 timeout 0", s).empty(), "timeout 0 refused");
    check(!parse_race_start("1 timeout 3601", s).empty(), "timeout over an hour refused");
    check(!parse_race_start("1 timeout 1.5", s).empty(), "a fractional timeout refused");
    check(!parse_race_start("1 later 60", s).empty(), "another word refused");
    std::string list = "1";
    for (std::uint64_t i = 2; i <= max_racers; ++i) list += "," + std::to_string(i);
    check(parse_race_start(list, s).empty() && s.ids.size() == max_racers, "64 racers accepted");
    check(!parse_race_start(list + ",65", s).empty(), "65 racers refused");
    check(parse_race_start(list + ",1", s).empty(), "a repeat does not count twice");

    // The zone check.
    const RaceZone finish{{100, 0, 0}, 10};
    check(in_zone(finish, {100, 0, 0}), "the centre is inside");
    check(in_zone(finish, {105, 3, -4}), "inside");
    check(in_zone(finish, {110, 0, 0}), "the surface counts");
    check(!in_zone(finish, {110.5f, 0, 0}), "just outside");
    check(!in_zone(finish, {100, 8, 8}), "outside on a diagonal");
    check(!in_zone(finish, {NAN, 0, 0}), "a NaN position is never inside");

    // Checkpoints in order, then the finish.
    std::size_t next{};
    check(race_step(next, {}, finish, {100, 0, 0}), "no checkpoints: the finish counts at once");
    const std::vector<RaceZone> checkpoints{{{0, 0, 0}, 5}, {{50, 0, 0}, 5}};
    next = 0;
    check(!race_step(next, checkpoints, finish, {100, 0, 0}) && next == 0, "the finish before any checkpoint does not count");
    check(!race_step(next, checkpoints, finish, {50, 0, 0}) && next == 0, "the second checkpoint first does not count");
    check(!race_step(next, checkpoints, finish, {1, 0, 0}) && next == 1, "the first checkpoint");
    check(!race_step(next, checkpoints, finish, {1, 0, 0}) && next == 1, "staying in it changes nothing");
    check(!race_step(next, checkpoints, finish, {100, 0, 0}) && next == 1, "the finish with one checkpoint left does not count");
    check(!race_step(next, checkpoints, finish, {50, 2, 0}) && next == 2, "the second checkpoint");
    check(race_step(next, checkpoints, finish, {95, 0, 0}), "then the finish counts");
    const std::vector<RaceZone> overlapping{{{0, 0, 0}, 5}, {{2, 0, 0}, 5}, {{90, 0, 0}, 1}};
    next = 0;
    check(!race_step(next, overlapping, finish, {1, 0, 0}) && next == 2, "overlapping checkpoints pass in one step, in order");
    check(!race_step(next, overlapping, finish, {95, 0, 0}) && next == 2, "a checkpoint inside the finish is still needed");
    check(race_step(next, overlapping, finish, {90, 0, 0}), "passing it inside the finish finishes");

    // Times, lines and results.
    check(race_seconds(48250) == "48.3", "48250 ms is 48.3 s");
    check(race_seconds(51000) == "51.0", "51000 ms is 51.0 s");
    check(race_seconds(0) == "0.0", "0 ms");
    check(race_seconds(49) == "0.0" && race_seconds(50) == "0.1", "rounding at the twentieth");
    check(race_seconds(59999) == "60.0", "rounding up to the minute");
    check(race_seconds(3600000) == "3600.0", "an hour");
    check(race_finish_line(finished(76561198000000001, "Kush", 1, 48300)) ==
              "[race] Kush (76561198000000001) finished 1 in 48.3 s",
          "the finish line");
    check(race_finish_line(finished(1, "Bad\nName\r", 2, 1000)) == "[race] Bad Name  (1) finished 2 in 1.0 s", "a name on one line");
    std::vector<Racer> racers{Racer{.id = 1, .name = "Carl"}, finished(2, "Bea", 2, 51000), Racer{.id = 3, .name = "Dee"},
                              finished(4, "Al", 1, 48300)};
    racers[2].left = true;
    check(race_results_line(racers) == "[race] ended: 1. Al 48.3 s, 2. Bea 51.0 s, Carl DNF, Dee DNF",
          "finishers by place, then DNF as they started");
    // Two finished in one tick: same time, place decides.
    std::vector<Racer> tie{finished(1, "B", 2, 5000), finished(2, "A", 1, 5000)};
    check(race_results_line(tie) == "[race] ended: 1. A 5.0 s, 2. B 5.0 s", "a tie by place");
    check(race_results_line({Racer{.id = 1, .name = "X"}}) == "[race] ended: X DNF", "nobody finished");
    check(race_results_line({}) == "[race] ended: nobody raced", "no racers");
    check(race_results_line({finished(1, "a\nb", 1, 1)}).find('\n') == std::string::npos, "never more than one line");
    return 0;
}
