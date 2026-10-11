#include "Server/server_race.h"
#include <cstdlib>
#include <regex>
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
    // race-start ... false-start dnf|<seconds>
    check(parse_race_start("1,2 false-start dnf", s).empty() && s.false_start == 0u && s.timeout == default_race_timeout,
          "false-start dnf");
    check(parse_race_start("1 false-start 5 timeout 120", s).empty() && s.false_start == 5u && s.timeout == 120,
          "a penalty, then a timeout");
    check(parse_race_start("1 timeout 120 FALSE-START DNF", s).empty() && s.false_start == 0u && s.timeout == 120,
          "a timeout, then false-start, any case");
    check(parse_race_start("1 false-start 60", s).empty() && s.false_start == 60u, "a 60 s penalty");
    check(parse_race_start("1 timeout 60", s).empty() && !s.false_start, "no false-start: no check");
    check(!parse_race_start("1 false-start 0", s).empty(), "a 0 s penalty refused");
    check(!parse_race_start("1 false-start 61", s).empty(), "a 61 s penalty refused");
    check(!parse_race_start("1 false-start x", s).empty(), "a word penalty refused");
    check(!parse_race_start("1 false-start", s).empty(), "false-start alone refused");
    check(!parse_race_start("1 false-start dnf false-start 5", s).empty(), "false-start twice refused");
    check(!parse_race_start("1 timeout 60 timeout 90", s).empty(), "timeout twice refused");

    // The start zone.
    const RaceZone start{{0, 0, 0}, 5};
    check(!false_start_distance(start, {1, 2, 3}), "inside the start: no false start");
    check(!false_start_distance(start, {5, 0, 0}), "on its surface: no false start");
    const auto away = false_start_distance(start, {3, 4, 12});
    check(away && *away == 13.f, "outside: the distance from the centre");
    check(!false_start_distance(start, {NAN, 99, 0}), "no position that is a number: no false start");

    const auto three = [] {
        return std::vector<Racer>{Racer{.id = 1, .name = "A"}, Racer{.id = 2, .name = "B"}, Racer{.id = 3, .name = "C"}};
    };
    const std::vector<std::optional<std::array<float, 3>>> at{std::array<float, 3>{1, 0, 0}, std::array<float, 3>{0, 12.34f, 0},
                                                              std::nullopt};
    auto field = three();
    check(mark_false_starts(field, at, std::nullopt, 5u).empty() && !field[1].false_start, "no start zone: nobody marked");
    check(mark_false_starts(field, at, start, std::nullopt).empty() && !field[1].false_start, "no false-start rule: nobody marked");
    auto lines = mark_false_starts(field, at, start, 5u);
    check(lines == std::vector<std::string>{"[race] B (2) false start: 12.3 m from the start"}, "the false start line");
    check(!field[0].false_start && field[1].false_start && field[1].penalty == 5 && !field[1].disqualified && !field[2].false_start,
          "only the one outside is marked; no position is not a false start");
    field = three();
    lines = mark_false_starts(field, at, start, 0u);
    check(lines.size() == 1 && field[1].false_start && field[1].disqualified && !field[1].penalty, "dnf marks a disqualification");
    check(race_false_start_line(Racer{.id = 7, .name = "x\ny"}, 2.06f) == "[race] x y (7) false start: 2.1 m from the start",
          "a false start line is one line");

    // Places with penalties: B false-started (+5 s) and crosses first at 48 s, so 53 s.
    field = three();
    field[1].penalty = 5;
    field[1].false_start = true;
    unsigned placed{};
    std::uint64_t crossed{};
    field[1].ms = 48000 + 5000;
    field[1].seq = ++crossed;
    check(race_settle(field, placed, 48000, false).empty() && !field[1].place, "a penalised time waits for the clock");
    field[0].ms = 50000;
    field[0].seq = ++crossed;
    auto due = race_settle(field, placed, 50000, false);
    check(due.size() == 1 && due[0] == &field[0] && field[0].place == 1, "a clean racer behind is placed first");
    due = race_settle(field, placed, 53000, false);
    check(due.size() == 1 && due[0] == &field[1] && field[1].place == 2, "the penalised one once the clock reaches 53 s");
    const std::regex finish_format(R"(^\[race\] (.+) \((\d+)\) finished (\d+) in ([\d.]+) s$)");
    check(race_finish_line(field[1]) == "[race] B (2) finished 2 in 53.0 s" && std::regex_match(race_finish_line(field[1]), finish_format),
          "a penalised finish line: the penalised time and place, no suffix");
    check(race_results_line(field) == "[race] ended: 1. A 50.0 s, 2. B 53.0 s (+5 s false start), C DNF", "results with a penalty");
    // The race ends before the clock reaches a penalised time: placed then, in time order, ties by crossing.
    field = three();
    placed = 0;
    field[0].ms = 60000;
    field[0].seq = 2;
    field[0].penalty = 10;
    field[1].ms = 60000;
    field[1].seq = 1;
    field[1].penalty = 10;
    field[2].ms = 55000;
    field[2].seq = 3;
    field[2].penalty = 5;
    check(race_settle(field, placed, 52000, false).empty(), "nothing certain yet");
    due = race_settle(field, placed, 52000, true);
    check(due.size() == 3 && due[0] == &field[2] && due[1] == &field[1] && due[2] == &field[0] && field[1].place == 2,
          "the end places the rest by time, ties by who crossed first");
    check(race_settle(field, placed, 99000, true).empty(), "nobody is placed twice");
    // dnf: they race but are never placed.
    field = three();
    placed = 0;
    field[2].false_start = field[2].disqualified = true;
    field[2].ms = 30000;
    field[0].ms = 40000;
    check(race_settle(field, placed, 99000, true).size() == 1 && !field[2].place && field[0].place == 1, "a dnf false start is not placed");
    field[1].left = true;
    check(race_results_line(field) == "[race] ended: 1. A 40.0 s, B DNF, C DNF (false start)", "results with a dnf false start");
    // A penalised racer who never finishes is a DNF, still marked.
    field = three();
    field[0].false_start = true;
    field[0].penalty = 5;
    check(race_results_line(field) == "[race] ended: A DNF (false start), B DNF, C DNF", "a penalised DNF");
    check(race_results_line(field).find('\n') == std::string::npos, "still one line");
    return 0;
}
