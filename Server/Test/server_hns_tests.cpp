// Hide and seek (Server/server_hns.h): reading hns-start, who may see whom, finds and how a round
// ends. Everything here is pure; the host's part is in server_hns_host_tests.cpp.
//
//   dingosdk_server_hns_tests
#include "Server/server_hns.h"
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
bool refused(std::string_view argument) {
    HnsStart s;
    return !parse_hns_start(argument, s).empty();
}
using Ids = std::vector<std::uint64_t>;
using Pairs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
} // namespace

int main() {
    // hns-start <seekers> <hiders> at <x y z|player> [hide|seek|reveal|find|hold <n>]...
    HnsStart s;
    check(parse_hns_start("1,2 3,4 at 1 2 3", s).empty(), "seekers, hiders and a spot");
    check(s.seekers == Ids{1, 2} && s.hiders == Ids{3, 4}, "the two lists, in order");
    check(s.at && (*s.at)[0] == 1.f && (*s.at)[1] == 2.f && (*s.at)[2] == 3.f && s.at_player.empty(), "the spot");
    check(s.settings.hide_s == 60 && s.settings.seek_s == 300 && s.settings.reveal == 25.f && s.settings.find == 3.f &&
              s.settings.hold == 15.f,
          "the defaults: hide 60 s, seek 300 s, reveal 25 m, find 3 m, hold 15 m");
    s = {};
    check(parse_hns_start("1 2,3 at Bob hide 30 seek 120 reveal 40 find 2.5 hold 10", s).empty(), "a player as the spot, and every value");
    check(!s.at && s.at_player == "Bob", "the player named");
    check(s.settings.hide_s == 30 && s.settings.seek_s == 120 && s.settings.reveal == 40.f && s.settings.find == 2.5f &&
              s.settings.hold == 10.f,
          "the values given");
    s = {};
    check(parse_hns_start("  1   2,3   at 76561198000000002  ", s).empty() && s.at_player == "76561198000000002",
          "extra spaces, and a SteamID as the spot");
    check(parse_hns_start("1 2,3 at 0 0 0 hide 10 seek 30 reveal 5 find 1 hold 5", s).empty(), "the lower limits accepted");
    check(parse_hns_start("1 2,3 at 0 0 0 hide 600 seek 3600 reveal 200 find 20 hold 100", s).empty(), "the upper limits accepted");
    check(refused("1 1,2 at 0 0 0"), "a seeker who is also a hider refused");
    check(refused("1,1 2,3 at 0 0 0"), "a seeker twice refused");
    check(refused("1 2,2 at 0 0 0"), "a hider twice refused");
    check(refused("1 2,3"), "no spot refused");
    check(refused("1 2,3 at"), "an empty spot refused");
    check(refused("1 2,3 at 0 0"), "two coordinates are a player name with a space: refused");
    check(refused("1 2,3 at 0 0 0 hide 5"), "hide 5 refused");
    check(refused("1 2,3 at 0 0 0 seek 4000"), "seek 4000 refused");
    check(refused("1 2,3 at 0 0 0 reveal 300"), "reveal 300 refused");
    check(refused("1 2,3 at 0 0 0 find 0.5"), "find 0.5 refused");
    check(refused("1 2,3 at 0 0 0 hold 200"), "hold 200 refused");
    check(refused("1 2,3 at 0 0 0 hide"), "a setting without a value refused");
    // The play area: a circle around the spot, and how long a hider may stay outside it.
    check(parse_hns_start("1 2,3 at 0 0 0", s).empty() && s.settings.area == 100.f && s.settings.out_s == 60,
          "area 100 m and out after 60 s by default");
    check(parse_hns_start("1 2,3 at 0 0 0 area 150 out 90", s).empty() && s.settings.area == 150.f && s.settings.out_s == 90,
          "area and out read");
    check(parse_hns_start("1 2,3 at 0 0 0 area 20 out 10", s).empty(), "the lower limits of area and out accepted");
    check(parse_hns_start("1 2,3 at 0 0 0 area 2000 out 600", s).empty(), "the upper limits of area and out accepted");
    check(parse_hns_start("1 2,3 at 0 0 0 out 0", s).empty() && s.settings.out_s == 0, "out 0: nobody is put out");
    check(refused("1 2,3 at 0 0 0 area 19"), "area 19 refused");
    check(refused("1 2,3 at 0 0 0 area 2001"), "area 2001 refused");
    check(refused("1 2,3 at 0 0 0 out 5"), "out 5 refused");
    check(refused("1 2,3 at 0 0 0 out 601"), "out 601 refused");
    check(refused("1 2,3 at 0 0 0 out 1.5"), "out is whole seconds");
    check(refused("1 2,3 at 0 0 0 hide 1.5"), "seconds are whole numbers");
    check(refused("1 2,3 at 0 0 0 speed 3"), "an unknown word refused");
    check(refused("1 2,3 at 1e99 0 0"), "an absurd coordinate refused");
    check(refused("1 2,3 at nan 0 0"), "NaN refused");
    check(refused("1 x,3 at 0 0 0"), "an ID that is not a number refused");
    check(refused("1 2,,3 at 0 0 0"), "an empty ID refused");
    check(refused(" 2,3 at 0 0 0"), "no hiders refused (the spot is not a list)");
    std::string many;
    for (unsigned i = 2; i <= 65; ++i) many += (i > 2 ? "," : "") + std::to_string(i);
    check(refused("1 " + many + " at 0 0 0"), "65 players refused");
    many.resize(many.rfind(','));
    check(!refused("1 " + many + " at 0 0 0"), "64 players accepted");

    // Who may see whom: a hider is kept from the seekers (beyond reveal, and while hiding) and from the
    // other hiders, always (2026-10-14: hiders could follow each other to the good spots).
    using P = HnsPhase;
    using R = HnsRole;
    const auto near = std::optional<float>(24.f * 24.f), far = std::optional<float>(26.f * 26.f), stale = std::optional<float>();
    for (const auto phase : {P::off, P::hide, P::seek})
        for (const auto source : {R::none, R::seeker, R::hider})
            for (const auto to : {R::none, R::seeker, R::hider})
                for (const auto distance : {near, far, stale}) {
                    const bool hidden = hns_hidden(phase, source, to, distance, 25.f);
                    const bool want = phase != P::off && source == R::hider &&
                                      (to == R::hider ||
                                       (to == R::seeker && (phase == P::hide || !distance || *distance > 25.f * 25.f)));
                    check(hidden == want, "hns_hidden, phase " + std::to_string(int(phase)) + " source " + std::to_string(int(source)) +
                                              " to " + std::to_string(int(to)) + ": " + (hidden ? "hidden" : "shown"));
                }
    check(!hns_hidden(P::seek, R::hider, R::seeker, near, 25.f), "a hider 24 m from a seeker is seen");
    check(hns_hidden(P::seek, R::hider, R::seeker, far, 25.f), "a hider 26 m from a seeker is not");
    check(hns_hidden(P::seek, R::hider, R::seeker, stale, 25.f), "without fresh positions a hider stays hidden");
    check(hns_hidden(P::hide, R::hider, R::seeker, std::optional<float>(1.f), 25.f), "while hiding, not even at 1 m");
    // Outside the play area a hider is seen by the seekers at any distance (seeking), and while hiding still by none.
    check(!hns_hidden(P::seek, R::hider, R::seeker, far, 25.f, true), "a hider outside the area is seen from afar");
    check(!hns_hidden(P::seek, R::hider, R::seeker, stale, 25.f, true), "outside the area even without a fresh distance");
    check(hns_hidden(P::hide, R::hider, R::seeker, far, 25.f, true), "outside the area while hiding: still hidden");

    // Inside or outside: horizontal distance from the spot, 5 m to come back in, so the edge does not flicker.
    const std::array<float, 3> spot{0, 0, 0};
    check(!hns_outside(false, {99, 0, 0}, spot, 100.f), "99 m: inside");
    check(hns_outside(false, {0, 0, 101}, spot, 100.f), "101 m: outside");
    check(!hns_outside(false, {0, 500, 0}, spot, 100.f), "500 m straight up: inside (only the ground counts)");
    check(hns_outside(true, {97, 0, 0}, spot, 100.f), "outside, back at 97 m: still outside");
    check(!hns_outside(true, {94, 0, 0}, spot, 100.f), "outside, back at 94 m: inside");

    // Outside for out_s in a row: out of the round. Coming back in starts it over.
    using AE = HnsAreaEvent;
    using Events = std::vector<std::pair<std::uint64_t, AE>>;
    HnsArea area;
    const std::uint64_t sec = 1000000;
    const std::vector<HnsPos> in{{2, {10, 0, 0}}}, out{{2, {150, 0, 0}}};
    check(area.step(0, in, spot, 100.f, 60 * sec).empty(), "inside: nothing");
    check(area.step(sec, out, spot, 100.f, 60 * sec) == Events{{2, AE::left}}, "leaving the area");
    check(area.step(30 * sec, out, spot, 100.f, 60 * sec).empty(), "outside 29 s: nothing new");
    check(area.step(31 * sec, in, spot, 100.f, 60 * sec) == Events{{2, AE::back}}, "back in");
    check(area.outside(2) == false, "back in: not outside");
    check(area.step(32 * sec, out, spot, 100.f, 60 * sec) == Events{{2, AE::left}}, "out again");
    check(area.outside(2), "outside");
    check(area.step(91 * sec, out, spot, 100.f, 60 * sec).empty(), "59 s outside this time: not out yet");
    check(area.step(92 * sec, out, spot, 100.f, 60 * sec) == Events{{2, AE::out}}, "60 s outside in a row: out");
    HnsArea never;
    never.step(0, out, spot, 100.f, 0);
    check(never.step(3600 * sec, out, spot, 100.f, 0).empty(), "out 0: an hour outside and still in the round");
    HnsArea quiet;
    quiet.step(0, out, spot, 100.f, 60 * sec);
    check(quiet.step(61 * sec, {}, spot, 100.f, 60 * sec).empty(), "no fresh position: no verdict");
    check(quiet.step(62 * sec, out, spot, 100.f, 60 * sec) == Events{{2, AE::out}}, "outside all along: out once seen again");

    // Finds: within `find` for a whole second.
    HnsFinds finds;
    const std::uint64_t second = 1000000;
    const std::vector<HnsPos> seeker{{1, {0, 0, 0}}}, hider_near{{2, {2.9f, 0, 0}}}, hider_out{{2, {3.5f, 0, 0}}};
    check(finds.step(0, seeker, hider_near, 3.f).empty(), "not found the moment they are near");
    check(finds.step(9 * second / 10, seeker, hider_near, 3.f).empty(), "not found at 0.9 s");
    check(finds.step(second, seeker, hider_near, 3.f) == Pairs{{1, 2}}, "found at 1 s");
    HnsFinds again;
    check(again.step(0, seeker, hider_near, 3.f).empty(), "near from 0 s");
    check(again.step(second / 2, seeker, hider_out, 3.f).empty(), "out at 0.5 s");
    check(again.step(second, seeker, hider_near, 3.f).empty(), "back at 1 s: the second starts over");
    check(again.step(2 * second - 1, seeker, hider_near, 3.f).empty(), "not yet at 1.999999 s");
    check(again.step(2 * second, seeker, hider_near, 3.f) == Pairs{{1, 2}}, "found a whole second after coming back");
    HnsFinds two;
    const std::vector<HnsPos> seekers{{7, {1, 0, 0}}, {5, {-1, 0, 0}}}, hider{{2, {0, 0, 0}}};
    two.step(0, seekers, hider, 3.f);
    check(two.step(second, seekers, hider, 3.f) == Pairs{{5, 2}}, "two seekers at one hider: one find, the smaller ID's");
    HnsFinds height;
    const std::vector<HnsPos> above{{2, {0, 2.5f, 2.5f}}};
    height.step(0, seeker, above, 3.f);
    check(height.step(second, seeker, above, 3.f).empty(), "3.5 m away diagonally is not within 3 m");

    // How a round ends.
    using E = HnsEnd;
    const auto ends = [](std::size_t seekers, Ids left, Ids found, bool time_up, E end, Ids winners) {
        const auto o = hns_outcome(seekers, left, found, time_up);
        return o.end == end && o.winners == winners;
    };
    check(ends(1, {10, 11}, {}, false, E::none, {}), "two hiders and a seeker: the round goes on");
    check(ends(1, {10}, {}, false, E::last_hider, {10}), "one hider left: they win");
    check(ends(3, {}, {10, 11}, false, E::last_hider, {10, 11}), "the last two found together: both win");
    check(ends(3, {}, {}, false, E::no_hiders, {}), "every hider left: nobody wins");
    check(ends(0, {10, 11}, {}, false, E::no_seekers, {10, 11}), "every seeker left: the hiders win");
    check(ends(1, {10, 11}, {}, true, E::time_up, {10, 11}), "time is up: every hider left wins");
    check(ends(2, {}, {10}, false, E::last_hider, {10}), "found in the tick the last other hider left: the one found wins");
    check(ends(1, {10}, {11}, true, E::last_hider, {10}), "one hider left and time up together: the last hider wins");
    return 0;
}
