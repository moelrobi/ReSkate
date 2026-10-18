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
    check(race_seconds(48265) == "48.27", "48265 ms is 48.27 s");
    check(race_seconds(48250) == "48.25", "48250 ms is 48.25 s");
    check(race_seconds(48050) == "48.05", "a hundredth below ten keeps its zero");
    check(race_seconds(51000) == "51.00", "51000 ms is 51.00 s");
    check(race_seconds(0) == "0.00", "0 ms");
    check(race_seconds(4) == "0.00" && race_seconds(5) == "0.01", "rounding at the half hundredth");
    check(race_seconds(59995) == "60.00", "rounding up to the minute");
    check(race_seconds(3600000) == "3600.00", "an hour");
    check(race_finish_line(finished(76561198000000001, "Kush", 1, 48300)) ==
              "[race] Kush (76561198000000001) finished 1 in 48.30 s",
          "the finish line");
    check(race_finish_line(finished(1, "Bad\nName\r", 2, 1000)) == "[race] Bad Name  (1) finished 2 in 1.00 s", "a name on one line");
    std::vector<Racer> racers{Racer{.id = 1, .name = "Carl"}, finished(2, "Bea", 2, 51000), Racer{.id = 3, .name = "Dee"},
                              finished(4, "Al", 1, 48300)};
    racers[2].left = true;
    check(race_results_line(racers) == "[race] ended: 1. Al 48.30 s, 2. Bea 51.00 s, Carl DNF, Dee DNF",
          "finishers by place, then DNF as they started");
    // Two finished in one tick: same time, place decides.
    std::vector<Racer> tie{finished(1, "B", 2, 5000), finished(2, "A", 1, 5000)};
    check(race_results_line(tie) == "[race] ended: 1. A 5.00 s, 2. B 5.00 s", "a tie by place");
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
    check(race_finish_line(field[1]) == "[race] B (2) finished 2 in 53.00 s" && std::regex_match(race_finish_line(field[1]), finish_format),
          "a penalised finish line: the penalised time and place, no suffix");
    check(race_results_line(field) == "[race] ended: 1. A 50.00 s, 2. B 53.00 s (+5 s false start), C DNF", "results with a penalty");
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
    check(race_results_line(field) == "[race] ended: 1. A 40.00 s, B DNF, C DNF (false start)", "results with a dnf false start");
    // A penalised racer who never finishes is a DNF, still marked.
    field = three();
    field[0].false_start = true;
    field[0].penalty = 5;
    check(race_results_line(field) == "[race] ended: A DNF (false start), B DNF, C DNF", "a penalised DNF");
    check(race_results_line(field).find('\n') == std::string::npos, "still one line");
    // Checkpoint lines.
    Racer runner{.id = 76561198000000001, .name = "Kush"};
    const std::vector<RaceZone> course{{{0, 0, 0}, 5}, {{50, 0, 0}, 5}};
    std::size_t from = runner.next;
    check(!race_step(runner.next, course, finish, {1, 0, 0}) &&
              race_checkpoint_lines(runner, from, course.size(), {12345}) ==
                  std::vector<std::string>{"[race] Kush (76561198000000001) checkpoint 1/2 in 12.35 s"},
          "one checkpoint, one line");
    from = runner.next;
    check(!race_step(runner.next, course, finish, {1, 0, 0}) && race_checkpoint_lines(runner, from, course.size(), {13000}).empty(),
          "staying in it logs nothing");
    check(!race_step(runner.next, course, finish, {50, 0, 0}) && race_checkpoint_lines(runner, from, 2, {20000}) ==
              std::vector<std::string>{"[race] Kush (76561198000000001) checkpoint 2/2 in 20.00 s"},
          "resuming logs only the next one");
    Racer quick{.id = 9, .name = "Bad\nName"};
    const std::vector<RaceZone> stacked{{{0, 0, 0}, 5}, {{2, 0, 0}, 5}, {{90, 0, 0}, 1}};
    from = quick.next;
    race_step(quick.next, stacked, finish, {1, 0, 0});
    check(race_checkpoint_lines(quick, from, stacked.size(), {4049, 4051}) ==
              std::vector<std::string>{"[race] Bad Name (9) checkpoint 1/3 in 4.05 s", "[race] Bad Name (9) checkpoint 2/3 in 4.05 s"},
          "overlapping checkpoints: a line each, in order, names on one line");
    from = quick.next;
    check(race_step(quick.next, stacked, finish, {90, 0, 0}) &&
              race_checkpoint_lines(quick, from, stacked.size(), {9000}) ==
                  std::vector<std::string>{"[race] Bad Name (9) checkpoint 3/3 in 9.00 s"},
          "the last checkpoint is logged in the tick it finishes");

    // race-start ... countdown <seconds>
    check(parse_race_start("1 countdown 5", s).empty() && s.countdown == 5, "a countdown");
    check(parse_race_start("1 countdown 1 timeout 60", s).empty() && s.countdown == 1 && s.timeout == 60, "countdown 1, then a timeout");
    check(parse_race_start("1 false-start dnf COUNTDOWN 10", s).empty() && s.countdown == 10 && s.false_start == 0u, "countdown 10, any case");
    check(!parse_race_start("1 countdown 0", s).empty(), "countdown 0 refused");
    check(!parse_race_start("1 countdown 11", s).empty(), "countdown 11 refused");
    check(!parse_race_start("1 countdown", s).empty(), "countdown alone refused");
    check(!parse_race_start("1 countdown 3 countdown 4", s).empty(), "countdown twice refused");
    check(parse_race_start("1,2 timeout 60 false-start 5", s).empty() && !s.countdown && s.timeout == 60 && s.false_start == 5u,
          "without countdown: as before, no countdown");
    check(race_countdown_left(10000000, 5000000) == 5 && race_countdown_left(10000000, 5000001) == 5 &&
              race_countdown_left(10000000, 9000000) == 1 && race_countdown_left(10000000, 9999999) == 1,
          "the countdown's seconds round up");
    check(race_countdown_left(10000000, 10000000) == 0 && race_countdown_left(10000000, 12000000) == 0, "0 at and after GO");
    check(race_countdown_card(5) == "5" && race_countdown_card(1) == "1" && race_countdown_card(0) == "GO!", "the cards");
    // ... countdown <seconds> go <text>: the GO card's text, to the end of the line
    check(parse_race_start("1 timeout 300 false-start 5 countdown 5 go :pog1: GO GO GO! First to the finish line wins.", s).empty() &&
              s.countdown == 5 && s.timeout == 300 && s.false_start == 5u && s.go == ":pog1: GO GO GO! First to the finish line wins.",
          "go takes the rest of the line, spaces included");
    check(parse_race_start("1 countdown 3 GO  timeout 60  ", s).empty() && s.go == "timeout 60" && s.timeout == default_race_timeout,
          "after go every word is text");
    check(!parse_race_start("1 countdown 3 go", s).empty() && !parse_race_start("1 countdown 3 go   ", s).empty(), "go without text refused");
    check(!parse_race_start("1 go Hi", s).empty() && !parse_race_start("1 timeout 60 go Hi", s).empty(), "go without countdown refused");
    check(parse_race_start("1 countdown 5", s).empty() && s.go.empty(), "without go: no text");
    check(race_countdown_card(0, ":pog1: GO GO GO!") == ":pog1: GO GO GO!" && race_countdown_card(0, "") == "GO!" &&
              race_countdown_card(2, "Go!") == "2",
          "the GO card's text, only at GO");
    check(race_start_usage.find("go <text>") != std::string_view::npos, "the usage names go <text>");
    {
        // Ticks at an uneven rate: each number once, in order, then GO.
        std::vector<std::string> cards;
        unsigned shown{};
        for (std::uint64_t now = 0; now <= 3100000; now += 70000) {
            const auto left = race_countdown_left(3000000, now);
            if (shown && left >= shown) continue;
            shown = left;
            cards.push_back(race_countdown_card(left));
            if (!left) break;
        }
        check(cards == std::vector<std::string>{"3", "2", "1", "GO!"}, "3, 2, 1, GO! once each");
    }

    // race-hold <SteamID64> <x y z> ... [radius <m>] [drop <m>]
    std::vector<RaceHold> holds;
    check(parse_race_hold("76561198000000001 10 2 -3.5", holds).empty() && holds.size() == 1 && holds[0].id == 76561198000000001 &&
              holds[0].spot == std::array<float, 3>{10, 2, -3.5f} && holds[0].radius == 3.f && holds[0].drop == 4.f && !holds[0].teleported,
          "one racer, the defaults");
    check(parse_race_hold("1 0 0 0 2 5 5 5 radius 6 drop 10", holds).empty() && holds.size() == 2 && holds[1].spot[0] == 5.f &&
              holds[0].radius == 6.f && holds[1].radius == 6.f && holds[0].drop == 10.f && holds[1].drop == 10.f,
          "two racers, radius and drop for both");
    check(parse_race_hold("1 0 0 0 DROP 2", holds).empty() && holds[0].drop == 2.f && holds[0].radius == 3.f, "drop alone, any case");
    check(parse_race_hold("1 0 0 0 2 1 1 1 1 9 9 9", holds).empty() && holds.size() == 2 && holds[1].id == 1 && holds[1].spot[0] == 9.f,
          "a racer named twice is held where named last");
    check(!parse_race_hold("", holds).empty(), "empty refused");
    check(!parse_race_hold("1 0 0", holds).empty(), "two coordinates refused");
    check(!parse_race_hold("1 0 0 0 2", holds).empty(), "an id without a spot refused");
    check(!parse_race_hold("Bob 0 0 0", holds).empty(), "a name refused");
    check(!parse_race_hold("0 0 0 0", holds).empty(), "id 0 refused");
    check(!parse_race_hold("1 0 nan 0", holds).empty() && !parse_race_hold("1 0 1e9 0", holds).empty(), "bad coordinates refused");
    check(!parse_race_hold("1 0 0 0 radius 0.5", holds).empty() && !parse_race_hold("1 0 0 0 drop 201", holds).empty(),
          "radius and drop are 1 to 200 m");
    check(!parse_race_hold("1 0 0 0 radius 3 radius 4", holds).empty(), "radius twice refused");
    check(!parse_race_hold("1 0 0 0 radius 3 2 0 0 0", holds).empty(), "a racer after the options refused");
    check(!parse_race_hold("radius 3", holds).empty(), "options without a racer refused");
    {
        std::string many;
        for (unsigned i = 1; i <= 65; ++i) many += std::to_string(i) + " 0 0 0 ";
        check(!parse_race_hold(many, holds).empty(), "65 racers in one call refused");
        std::vector<RaceHold> held, one;
        for (unsigned i = 1; i <= 64; ++i) {
            check(parse_race_hold(std::to_string(i) + " 0 0 0", one).empty() && merge_race_holds(held, one), "one call per racer");
        }
        check(held.size() == 64, "64 held");
        check(parse_race_hold("7 1 1 1", one).empty() && merge_race_holds(held, one) && held.size() == 64 && held.back().id == 7,
              "holding one again replaces their hold");
        check(parse_race_hold("65 0 0 0", one).empty() && !merge_race_holds(held, one) && held.size() == 64, "a 65th refused, nothing changed");
    }
    // Pulled back: across more than radius, or more than drop below; grace and repeat.
    RaceHold hold{.id = 5, .spot = {100, 50, 0}};
    check(!race_hold_off(hold, {150, 50, 0}, 9000000, 9000000), "never before the first teleport");
    hold.teleported = 1000000;
    const std::uint64_t later = 1000000 + race_hold_repeat_us;
    check(!race_hold_off(hold, {150, 50, 0}, 1500000, later), "a pose from within the grace time is not counted");
    check(!race_hold_off(hold, {150, 50, 0}, 2100000, 2100000), "at most one pull per repeat time");
    const auto off = race_hold_off(hold, {104, 50, 3}, 2000000, later);
    check(off && *off == 5.f, "off across: the distance from the spot");
    check(!race_hold_off(hold, {102, 50, 2}, 2000000, later), "within the radius: held");
    check(!race_hold_off(hold, {102.9f, 70, 0}, 2000000, later), "above the spot (they land 2 m up, or jump): held");
    check(!race_hold_off(hold, {100, 46, 0}, 2000000, later), "4 m below: still held (drop 4)");
    const auto fell = race_hold_off(hold, {100, 45, 0}, 2000000, later);
    check(fell && *fell == 5.f, "more than drop below: through the ground");
    check(!race_hold_off(hold, {NAN, 50, 0}, 2000000, later), "a position that is not a number: nothing");
    check(race_hold_line(3) == "[race] hold: 3 racer(s)", "the hold line");
    check(race_pull_line("Kush\n", 76561198000000001, 4.24f) == "[race] Kush  (76561198000000001) back to the start: 4.2 m off",
          "the pull-back line");

    // Moves between two poses: crossing a zone between them.
    const RaceZone gate{{50, 0, 0}, 2};
    auto t = zone_entry(gate, {0, 0, 0}, {100, 0, 0});
    check(t && std::abs(*t - 0.48f) < 1e-5f, "passed through between two poses: where it entered");
    check(!zone_entry(gate, {0, 0, 0}, {40, 0, 0}), "short of it: nothing");
    check(!zone_entry(gate, {0, 3, 0}, {100, 3, 0}), "past it: nothing");
    t = zone_entry(gate, {0, 2, 0}, {100, 2, 0});
    check(t && std::abs(*t - 0.5f) < 1e-5f, "touching it counts");
    t = zone_entry(gate, {50, 1, 0}, {100, 1, 0});
    check(t && *t == 0.f, "starting inside: at once");
    t = zone_entry(gate, {50, 1, 0}, {50, 1, 0});
    check(t && *t == 0.f && !zone_entry(gate, {0, 0, 0}, {0, 0, 0}), "standing still: inside or not");
    t = zone_entry(gate, {0, 0, 0}, {100, 0, 0}, 0.6f);
    check(!t, "left behind before `from`: nothing");
    t = zone_entry(gate, {0, 0, 0}, {100, 0, 0}, 0.5f);
    check(t && *t == 0.5f, "inside at `from`: at `from`");
    check(!zone_entry(gate, {NAN, 0, 0}, {100, 0, 0}), "not a number: nothing");
    {
        // Two checkpoints and the finish in one move, in order; a teleport counts where it ends.
        const std::vector<RaceZone> line{{{20, 0, 0}, 1}, {{40, 0, 0}, 1}};
        const RaceZone end{{45, 0, 0}, 1};
        std::size_t next{};
        auto crossings = race_move(next, line, end, std::array<float, 3>{0, 0, 0}, {48, 0, 0});
        check(next == 2 && crossings.checkpoints.size() == 2 && std::abs(crossings.checkpoints[0] - 19.f / 48) < 1e-5f &&
                  std::abs(crossings.checkpoints[1] - 39.f / 48) < 1e-5f && crossings.finish && std::abs(*crossings.finish - 44.f / 48) < 1e-5f,
              "checkpoints and finish in one move, each where it was passed");
        next = 0;
        crossings = race_move(next, line, end, std::array<float, 3>{40, 0, 0}, {20, 0, 0});
        check(next == 1 && crossings.checkpoints.size() == 1 && !crossings.finish, "the second checkpoint before the first does not count");
        next = 0;
        crossings = race_move(next, {{{20, 0, 0}, 1}}, end, std::array<float, 3>{0, 0, 0}, {45, 60, 0});
        check(next == 0 && !crossings.finish, "a teleport (over 50 m): what it passed does not count");
        next = 1;
        crossings = race_move(next, line, end, std::array<float, 3>{-100, 0, 0}, {44.5f, 0, 0});
        check(next == 1 && !crossings.checkpoints.size() && !crossings.finish, "a teleport into the finish with a checkpoint left: nothing");
        next = 2;
        crossings = race_move(next, line, end, std::array<float, 3>{-100, 0, 0}, {44.5f, 0, 0});
        check(crossings.finish && *crossings.finish == 1.f, "a teleport into the finish: at the pose's time");
        next = 0;
        crossings = race_move(next, {}, end, std::nullopt, {45, 0, 0});
        check(crossings.finish && *crossings.finish == 1.f, "no pose before: where they are now");
    }
    // Their clock against the host's, and the race clock at a crossing.
    {
        PoseClock clock;
        check(!clock.sample(1000000, 51000000) && clock.offset == 50000000, "the first pose starts the clock");
        check(clock.sample(1033000, 51040000) && clock.offset == 50000000, "a later arrival does not move the offset");
        check(clock.sample(1066000, 51060000) && clock.offset == 49994000, "an earlier one does");
        check(!clock.sample(500000, 51100000) && clock.offset == 50600000, "a clock that went back starts over");
        check(!clock.sample(600000, 54000000), "a long pause starts over");
        // Poses sent at 10.0 s and 10.1 s of their clock, 2 s behind the host; started at 3 s.
        check(race_crossing_ms(10000000, 10100000, 0.5f, 2000000, 3000000, 20000000) == 9050, "halfway between the poses");
        check(race_crossing_ms(10000000, 10100000, 0.f, 2000000, 3000000, 20000000) == 9000 &&
                  race_crossing_ms(10000000, 10100000, 1.f, 2000000, 3000000, 20000000) == 9100,
              "at either pose");
        check(race_crossing_ms(10000000, 10100000, 0.5f, -10000000, 3000000, 20000000) == 0, "never before the start");
        check(race_crossing_ms(10000000, 10100000, 0.5f, 20000000, 3000000, 20000000) == 17000, "never after now");
        // The same ride read at 20 ticks a second or at 60: the same time.
        const RaceZone flag{{30.2f, 0, 0}, 0.5f};
        for (const std::uint64_t tick : {16667ULL, 50000ULL, 120000ULL}) {
            // 10 m/s, a pose every 1/30 s by their clock (offset 0, no delay), read at each tick.
            std::size_t next{};
            Racer r;
            std::optional<std::uint64_t> crossed_ms;
            for (std::uint64_t now = 0; now < 5000000 && !crossed_ms; now += tick) {
                const std::uint64_t pose = now / 33333 * 33333; // their newest pose by now
                const std::array<float, 3> at{static_cast<float>(pose) / 100000.f, 0, 0};
                if (r.last && pose == r.last_sent) continue;
                const auto c = race_move(next, {}, flag, r.last, at);
                if (c.finish) crossed_ms = race_crossing_ms(r.last ? r.last_sent : pose, pose, *c.finish, 0, 0, now);
                r.last = at;
                r.last_sent = pose;
            }
            check(crossed_ms && *crossed_ms >= 2969 && *crossed_ms <= 2971, "the time does not depend on the tick rate");
        }
    }
    {
        // A crossing read late (their pose arrived late) but made earlier still places first:
        // the host settles a time only race_settle_margin_ms after the clock passed it.
        std::vector<Racer> two{Racer{.id = 1, .name = "A"}, Racer{.id = 2, .name = "B"}};
        unsigned given{};
        two[0].ms = 10000;
        two[0].seq = 1;
        check(race_settle(two, given, 10300 - race_settle_margin_ms, false).empty(), "not yet within the margin");
        two[1].ms = 9990;
        two[1].seq = 2;
        const auto order = race_settle(two, given, 11100 - race_settle_margin_ms, false);
        check(order.size() == 2 && order[0]->id == 2 && two[1].place == 1 && two[0].place == 2, "the earlier time first, though read later");
    }
    check(std::regex_match(race_finish_line(finished(76561198000000001, "Kush", 1, 48265)),
                           std::regex(R"(^\[race\] (.+) \((\d+)\) finished (\d+) in ([\d.]+) s$)")) &&
              race_finish_line(finished(76561198000000001, "Kush", 1, 48265)) == "[race] Kush (76561198000000001) finished 1 in 48.27 s",
          "the finish line, two decimals, as the parser reads it");
    return 0;
}
