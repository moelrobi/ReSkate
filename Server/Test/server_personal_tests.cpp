// Personal polls and cards on the dedicated server (server_personal.cpp): poll-to, a poll only one
// player's roster carries and only they answer, and announce-to for several players, each card in
// that player's own slot beside the global one. Read the way a player's game reads its roster.
// The server runs over a transport that is not Steam's, with simulated players.
//
//   dingosdk_server_personal_tests
#include "Server/server_host.h"
#include "Engine/Game/World/world_names.h"
#include "Extension/Multiplayer/Net/pose_batch.h"
#include "Extension/Multiplayer/Net/pose_codec.h"
#include "Extension/Multiplayer/Net/sound_codec.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Extension/Multiplayer/Session/room.h"
#include "Extension/Multiplayer/Steam/steam_transport.h"
#include "Engine/Game/Build/supported_build.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
// What the fake transport holds: what each player is sent, and what they send.
struct Wire {
    TransportStatus status;
    std::vector<TransportMessage> inbound;
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> sent;
    std::map<std::uint64_t, std::string> closed;
};
Wire &wire() {
    static Wire value;
    return value;
}
} // namespace

// A game server's SteamID, as Steam gives the server one: never a player's.
constexpr std::uint64_t server_steam_id = (1ULL << 56) | (4ULL << 52) | 1;

struct SteamTransport::Impl {};
SteamTransport::SteamTransport() = default;
SteamTransport::~SteamTransport() = default;
bool SteamTransport::open() { return true; }
bool SteamTransport::open_game_server(void *) { return true; }
bool SteamTransport::host(unsigned) {
    wire().status.ready = wire().status.hosting = true;
    wire().status.local_id = server_steam_id;
    return true;
}
bool SteamTransport::join(std::uint64_t, std::uint32_t, std::uint16_t) { return false; }
bool SteamTransport::listen_direct(std::uint16_t) { return true; }
void SteamTransport::set_packing(unsigned) {}
std::vector<std::string> SteamTransport::take_direct_notes() { return {}; }
bool SteamTransport::set_steam_debug(bool) { return false; }
bool SteamTransport::connect_peer(std::uint64_t) { return false; }
void SteamTransport::allow_peers(std::span<const Member>) {}
bool SteamTransport::socket_test() { return true; }
void SteamTransport::stop() {}
void SteamTransport::disconnect(std::uint64_t id, const char *reason) {
    wire().closed[id] = reason ? reason : "";
    std::erase_if(wire().status.peers, [&](const TransportPeer &peer) { return peer.id == id; });
}
void SteamTransport::poll() {}
bool SteamTransport::send(std::uint64_t id, std::span<const std::uint8_t> bytes, bool, bool, TrafficLane) {
    if (std::none_of(wire().status.peers.begin(), wire().status.peers.end(), [&](const auto &peer) { return peer.id == id; })) return false;
    wire().sent[id].emplace_back(bytes.begin(), bytes.end());
    return true;
}
void SteamTransport::send_batch(std::span<TransportSend> messages) {
    for (auto &message : messages) message.sent = send(message.id, message.bytes, message.reliable, message.fresh, message.lane);
}
std::vector<TransportMessage> SteamTransport::receive() { return std::exchange(wire().inbound, {}); }
std::string SteamTransport::name(std::uint64_t) { return {}; }
const TransportStatus &SteamTransport::status() const { return wire().status; }
std::string SteamTransport::take_closed(std::uint64_t) { return {}; }
std::string SteamTransport::link_report(std::uint64_t) { return {}; }
std::int64_t SteamTransport::pending(std::uint64_t) const { return 0; }
std::string SteamTransport::relay_status() const { return "OK"; }
bool SteamTransport::set_send_rate(int) { return true; }
int SteamTransport::send_rate() const { return 900 * 1024; }
std::vector<TransportLink> SteamTransport::links() { return {}; }
bool SteamTransport::bind(void *, void *, void *) { return true; }
} // namespace dingosdk::multiplayer

namespace {
using namespace dingosdk;
using namespace dingosdk::multiplayer;
using namespace dingosdk::server;

int failures{};
void check(bool ok, const std::string &what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}
bool has(const std::vector<std::string> &lines, std::string_view text) {
    return std::any_of(lines.begin(), lines.end(), [&](const auto &line) { return line.find(text) != std::string::npos; });
}

// One player's game: it stands at `at` and sends a pose there every pass (unless `silent`), and
// keeps the vote or poll and the card of the last roster it was sent, and its chat.
struct Game {
    std::uint64_t id{}, epoch{}, secret{}, map{}, world = 1;
    std::uint32_t sequence{}, pose_sequence{};
    std::string name;
    DeltaReceiver receiver;
    bool joined{}, silent{};
    std::array<float, 3> at{};
    std::vector<std::array<float, 3>> teleports;
    std::vector<std::string> chat;
    ServerVote vote;          // the last roster's
    ServerAnnouncement card;  // the last roster's
    std::map<std::uint64_t, unsigned> poses_from;    // poses read of each other player
    std::map<std::uint16_t, std::uint64_t> streams; // the server's pose streams and whose they are
    pose_batch::Ack read_batches;

    Packet header(PacketKind kind, std::uint64_t now) {
        Packet p;
        p.kind = kind;
        p.session = secret;
        p.map = map;
        p.world = world;
        p.epoch = epoch;
        p.source = id;
        p.time_us = now;
        p.sequence = ++sequence;
        p.build = supported_build::game_sha256_bytes;
        return p;
    }
    void send(const Packet &p) { wire().inbound.push_back({id, encode_wire(p), p.time_us}); }
    void hello(std::uint64_t now) {
        wire().status.peers.push_back({id, true});
        auto p = header(PacketKind::hello, now);
        p.text = name;
        send(p);
    }
    void say(std::string text, std::uint64_t now) {
        auto p = header(PacketKind::chat, now);
        p.text = std::move(text);
        send(p);
    }
    void leave() {
        std::erase_if(wire().status.peers, [&](const TransportPeer &peer) { return peer.id == id; });
    }
    void ready(std::uint64_t now) {
        auto p = header(PacketKind::world_ready, now);
        p.world_ready = true;
        send(p);
    }
    void pose(std::uint64_t now) {
        if (silent || !joined) return;
        auto p = header(PacketKind::pose, now);
        p.sequence = ++pose_sequence;
        p.pose.root.position = at;
        p.pose.root.rotation = {0.f, 0.f, 0.f, 1.f};
        p.pose.skater.resize(pose_codec::finger_skeleton);
        for (auto &bone : p.pose.skater) bone.rotation = {0.f, 0.f, 0.f, 1.f};
        p.pose.skater[1].position = at;
        p.pose.board.resize(16);
        for (auto &bone : p.pose.board) bone.rotation = {0.f, 0.f, 0.f, 1.f};
        p.pose.board[0].position = at;
        send(p);
    }
    void read(std::uint64_t now) {
        bool acked{};
        for (const auto &bytes : std::exchange(wire().sent[id], {})) {
            if (const auto batch = pose_batch::read(bytes)) {
                for (const auto &entry : batch->entries) {
                    if (entry.whole) streams[entry.stream] = entry.source;
                    if (const auto found = streams.find(entry.stream); found != streams.end()) ++poses_from[found->second];
                }
                read_batches.note(batch->number);
                acked = true;
                continue;
            }
            if (pose_batch::Ack::read(bytes) || sound_codec::is_sound(bytes)) continue;
            bool missing{};
            const auto p = receiver.receive(bytes, missing, 0);
            if (!p) continue;
            if (p->kind == PacketKind::welcome) joined = true;
            if (p->kind == PacketKind::world_state) {
                world = p->world;
                if (p->map) map = p->map;
                streams.clear();
            }
            if (p->kind == PacketKind::teleport) {
                teleports.push_back(p->teleport);
                at = p->teleport;
            }
            if (p->kind == PacketKind::chat) chat.push_back(p->text);
            if (p->kind == PacketKind::roster) {
                vote = p->vote;
                card = p->announcement;
            }
        }
        if (acked) {
            const auto ack = read_batches.bytes();
            wire().inbound.push_back({id, {ack.begin(), ack.end()}, now});
        }
    }
};

int run() {
    const auto empty = std::filesystem::temp_directory_path() / "reskate_server_personal_tests_mods";
    std::filesystem::create_directories(empty);
    load_levels(empty); // the retail maps
    ServerConfig config;
    config.file = std::filesystem::temp_directory_path() / "reskate_server_personal_tests" / "ReSkateServer.json";
    config.mods = empty;
    config.auto_update = false;
    config.speed_check = "off";
    check(config_error(config).empty(), "The test's config was refused: " + config_error(config));
    std::vector<std::string> log;
    SteamTransport transport;
    Host host(config, transport, [&](const std::string &line) { log.push_back(line); });
    std::string error;
    if (!host.start(error)) {
        std::fprintf(stderr, "The server did not start: %s\n", error.c_str());
        return 1;
    }
    const auto invite = parse_invite(host.invite());
    if (!invite) {
        std::fprintf(stderr, "No invite.\n");
        return 1;
    }
    std::map<std::uint64_t, Game> games;
    const auto add_game = [&](std::uint64_t id, std::string name, std::array<float, 3> at) -> Game & {
        auto &game = games[id];
        game = {};
        game.id = id;
        game.epoch = 5000 + id % 1000 + games.size();
        game.secret = invite->secret;
        game.map = map_hash(map_destination(config.map));
        game.name = std::move(name);
        game.at = at;
        return game;
    };
    std::uint64_t now = 1000000000ULL, next_ready = now;
    const auto run_for = [&](std::uint64_t microseconds) {
        for (const auto end = now + microseconds; now < end; now += 50000) {
            if (now >= next_ready) {
                next_ready = now + 1000000;
                for (auto &[id, game] : games)
                    if (game.joined) game.ready(now);
            }
            for (auto &[id, game] : games) game.pose(now);
            host.tick(now);
            for (auto &[id, game] : games) game.read(now);
        }
    };
    const auto command = [&](std::string_view line) {
        const auto reply = host.command(line, 0);
        log.push_back(reply); // as the console writes its replies to the log
        return reply;
    };
    const auto logged = [&](std::string_view text) { return has(log, text); };
    const auto id = [](const Game &g) { return std::to_string(g.id); };
    const auto name_id = [&](const Game &g) { return g.name + " (" + id(g) + ")"; };
    // The global poll or vote a roster carries is the one every player without a personal poll has.
    const auto say = [&](Game &g, std::string text) {
        g.say(std::move(text), now);
        run_for(600000);
    };

    const std::uint64_t base = 76561198000000001ULL;
    auto &a = add_game(base, "Alice", {0, 0, 0});
    auto &b = add_game(base + 1, "Bob", {5, 0, 0});
    auto &c = add_game(base + 2, "Carol", {0, 0, 5});
    for (auto *g : {&a, &b, &c}) g->hello(now);
    run_for(6000000);
    for (auto *g : {&a, &b, &c}) check(g->joined, g->name + " did not join");
    check(command("help").find("poll-to <player>") != std::string::npos, "help does not list poll-to");
    check(command("help").find("announce-to <player>[,<player>...]") != std::string::npos, "help does not list announce-to for several players");

    // ---- poll-to: only the player asked sees it.
    check(command("poll-to") .find("poll-to <player|SteamID64> <question> | <answer> | <answer>...") == 0, "poll-to without arguments: " + command("poll-to"));
    check(command("poll-to Zed Snack? | Yes | No") == "No single connected player matches \"Zed\".", "poll-to to nobody");
    check(command("poll-to Alice Snack? | Yes").find("poll-to <player|SteamID64>") == 0, "poll-to with one answer was taken");
    auto reply = command("poll-to Alice Where to? | Race | Hide and seek | Stats");
    check(reply == "Asked Alice.", "poll-to reply: " + reply);
    run_for(600000);
    const auto first = a.vote.id;
    check(first && a.vote.kind == server_vote_poll && a.vote.outcome == vote_running && a.vote.label == "Where to?" &&
              a.vote.answers == std::vector<std::string>{"Race", "Hide and seek", "Stats"},
          "Alice's roster does not carry her poll");
    check(a.vote.seconds > 0, "Alice's poll has no time on it");
    check(!b.vote.id && !c.vote.id, "Players not asked see Alice's poll");
    check(logged("[poll-to] " + name_id(a) + " asked #" + std::to_string(first) + ": Where to? | Race | Hide and seek | Stats"),
          "The question was not logged");

    // Two at once; each sees their own.
    check(command("poll-to " + id(b) + " Music? | On | Off") == "Asked Bob.", "poll-to by SteamID64");
    run_for(600000);
    check(b.vote.id && b.vote.id != first && b.vote.label == "Music?", "Bob does not see his poll");
    check(a.vote.id == first, "Bob's poll replaced Alice's");
    check(!c.vote.id, "Carol sees a personal poll");

    // An answer out of range is refused, and the poll stays.
    say(a, "/7");
    check(has(a.chat, "The poll has 3 answers."), "An answer out of range was not refused");
    check(a.vote.id == first, "An answer out of range ended the poll");
    // An answer ends it at once: the card goes.
    say(a, "/2");
    check(logged("[poll-to] " + name_id(a) + " answered #" + std::to_string(first) + ": 2 Hide and seek"), "The answer was not logged");
    check(!a.vote.id, "Alice's card is still up after she answered");
    check(b.vote.label == "Music?", "Alice's answer ended Bob's poll");
    // With none open, a number is about the global poll again.
    a.chat.clear();
    say(a, "/1");
    check(has(a.chat, "No poll is running."), "A number without a poll: " + (a.chat.empty() ? std::string("nothing") : a.chat.back()));

    // Ended by the console, by a new one, by leaving.
    check(command("poll-to Bob end") == "Closed Bob's poll.", "poll-to end");
    run_for(600000);
    check(logged("[poll-to] " + name_id(b) + " #") && logged(" ended: closed"), "Closing was not logged");
    check(!b.vote.id, "Bob's poll is up after it was closed");
    check(command("poll-to Bob end") == "Bob has no poll open.", "poll-to end without a poll");
    command("poll-to Carol One? | A | B");
    run_for(600000);
    const auto carol_first = c.vote.id;
    command("poll-to Carol Two? | A | B");
    run_for(600000);
    check(logged("[poll-to] " + name_id(c) + " #" + std::to_string(carol_first) + " ended: replaced"), "Replacing was not logged");
    check(c.vote.label == "Two?" && c.vote.id != carol_first, "The new poll did not replace the old one");
    const auto carol_second = c.vote.id;
    // Timed out after votes.poll_seconds.
    run_for(std::uint64_t{config.votes.poll_seconds} * 1000000 + 1000000);
    check(logged("[poll-to] " + name_id(c) + " #" + std::to_string(carol_second) + " ended: timeout"), "The timeout was not logged");
    check(!c.vote.id, "Carol's poll outlived its time");

    // ---- Beside a global poll: personal answers go to the personal poll, everyone else's to the global one.
    command("poll Lunch? | Pizza | Pasta");
    run_for(600000);
    const auto global = b.vote.id;
    check(global && b.vote.label == "Lunch?", "The global poll did not start");
    check(command("poll-to Alice Snack? | Yes | No") == "Asked Alice.", "poll-to beside a global poll");
    run_for(600000);
    check(a.vote.label == "Snack?", "Alice sees the global poll over her own");
    check(b.vote.id == global && c.vote.id == global, "Others lost the global poll");
    check(command("poll Again? | A | B") != "", "A second global poll was not refused");
    say(a, "/1");
    check(logged("[poll-to] " + name_id(a) + " answered #") && logged(": 1 Yes"), "Alice's answer did not go to her poll");
    check(a.vote.id == global, "After her answer, Alice does not see the global poll again");
    check(a.vote.counts == std::vector<std::uint16_t>{0, 0}, "Alice's personal answer was counted in the global poll");
    say(b, "/2");
    check(b.vote.counts == std::vector<std::uint16_t>{0, 1}, "Bob's answer to the global poll was not counted");
    // A global poll does not end personal ones, and leaving does.
    command("poll-to Carol Stay? | Yes | No");
    run_for(600000);
    const auto carol_third = c.vote.id;
    command("poll end");
    run_for(600000);
    check(c.vote.id == carol_third, "Ending the global poll ended Carol's");
    c.leave();
    run_for(3000000);
    check(logged("[poll-to] " + name_id(c) + " #" + std::to_string(carol_third) + " ended: left"), "Leaving did not end Carol's poll");
    add_game(base + 2, "Carol", {0, 0, 5}).hello(now);
    run_for(6000000);
    check(c.joined, "Carol did not come back");
    check(c.vote.id != carol_third, "Carol's poll came back with her");

    // ---- announce-to for several players.
    reply = command("announce-to Alice,Zed," + id(b) + " Hello you two");
    check(reply == "No single connected player matches \"Zed\".\nAnnounced to Alice, Bob.", "announce-to reply: " + reply);
    run_for(600000);
    check(logged("[announcement -> Alice, Bob] Hello you two"), "The cards were not logged");
    check(a.card.text == "Hello you two" && b.card.text == "Hello you two", "Not both were shown the card");
    check(!c.card.id, "Carol was shown a card for others");
    check(command("announce-to Zed Hi") == "No single connected player matches \"Zed\".", "announce-to to nobody");
    run_for(8000000); // up 6 s
    check(!a.card.id && !b.card.id, "The personal cards outlived their time");

    // A personal card leaves the global card for the others, and is followed by it.
    // 190 characters: up 14 s.
    command("announce This is the global announcement everyone sees for a while, long enough to outlast two personal cards that come and go "
            "while it is up, and then it is still there for the players those were for.");
    run_for(600000);
    check(c.card.text.starts_with("This is the global"), "Carol does not see the global card");
    command("announce-to Alice Yours");
    run_for(600000);
    check(a.card.text == "Yours", "Alice does not see her card over the global one");
    check(b.card.text.starts_with("This is the global") && c.card.text.starts_with("This is the global"),
          "Alice's card hid the global card from the others");
    command("announce-to Bob Yours too");
    run_for(600000);
    check(a.card.text == "Yours" && b.card.text == "Yours too", "Bob's card replaced Alice's");
    run_for(6500000);
    check(a.card.text.starts_with("This is the global") && b.card.text.starts_with("This is the global"),
          "After their own cards, Alice and Bob do not see the global card again");
    // A new global card leaves a personal one up.
    command("announce-to Alice Mine again");
    run_for(600000);
    command("announce Second global");
    run_for(600000);
    check(a.card.text == "Mine again", "A global card replaced Alice's own");
    check(c.card.text == "Second global", "Carol does not see the new global card");

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
