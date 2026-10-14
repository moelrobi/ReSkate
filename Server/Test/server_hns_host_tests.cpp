// Hide and seek on the dedicated server (server_hns.cpp): what each player is sent of the others
// in each phase of a round, finds, the seekers' wait at the start, and how rounds end, read the
// way a player's game reads it. The server runs over a transport that is not Steam's, with
// simulated players who send poses where they stand.
//
//   dingosdk_server_hns_host_tests
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

// One player's game: it stands at `at` and sends a pose there every pass (unless `silent`), goes
// where the server teleports it, and counts the poses the server sends it of each other player.
struct Game {
    std::uint64_t id{}, epoch{}, secret{}, map{}, world = 1;
    std::uint32_t sequence{}, pose_sequence{};
    std::string name;
    DeltaReceiver receiver;
    bool joined{}, silent{};
    std::array<float, 3> at{};
    std::vector<std::array<float, 3>> teleports;
    std::vector<std::string> chat;
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
        }
        if (acked) {
            const auto ack = read_batches.bytes();
            wire().inbound.push_back({id, {ack.begin(), ack.end()}, now});
        }
    }
};

int run() {
    const auto empty = std::filesystem::temp_directory_path() / "reskate_server_hns_tests_mods";
    std::filesystem::create_directories(empty);
    load_levels(empty); // the retail maps
    ServerConfig config;
    config.file = std::filesystem::temp_directory_path() / "reskate_server_hns_tests" / "ReSkateServer.json";
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
    // Whether `to` is sent `from`'s poses now: counted over a second and a half.
    const auto sees = [&](Game &to, const Game &from) {
        to.poses_from.clear();
        run_for(1500000);
        return to.poses_from[from.id] > 0;
    };
    const auto id = [](const Game &g) { return std::to_string(g.id); };

    const std::uint64_t base = 76561198000000001ULL;
    auto &s = add_game(base, "Seeker Sam", {0, 0, 0});
    auto &h1 = add_game(base + 1, "Hider One", {2, 0, 0});
    auto &h2 = add_game(base + 2, "Hider Two", {0, 0, 40});
    auto &h3 = add_game(base + 3, "Hider Three", {0, 0, -40});
    auto &x = add_game(base + 4, "Watcher", {5, 0, 5});
    for (auto *g : {&s, &h1, &h2, &h3, &x}) g->hello(now);
    run_for(6000000); // joined, and every player introduced to every other
    for (auto *g : {&s, &h1, &h2, &h3, &x}) check(g->joined, g->name + " did not join");
    check(sees(s, h1), "Before a round, the seeker is not sent a nearby player's poses");
    check(command("hns-status") == "No hide and seek round.", "hns without a round");

    // Refused starts.
    check(command("hns-start " + id(s) + " " + id(h1) + " at 0 0 0").find("Not enough players") == 0,
          "A round with one hider started");
    auto reply = command("hns-start " + id(s) + ",99 " + id(h1) + "," + id(h2) + "," + id(h3) + " at 0 0 0 hide 30 seek 60");
    check(reply.find("Hide and seek started: 1 seeker(s), 3 hider(s).") == 0 && reply.find("Skipped: 99 (not connected).") != std::string::npos,
          "hns-start reply: " + reply);
    check(logged("[hns] started: 1 seeker(s), 3 hider(s), hide 30 s, seek 60 s, by console"), "The start was not logged");
    check(command("hns-start " + id(s) + " " + id(h1) + "," + id(h2) + " at 0 0 0").find("A hide and seek round is running") == 0,
          "A second round started over the first");
    check(command("hns-found " + id(h1)).find("Hide and seek is not in the seek phase") == 0, "A find was called while hiding");
    check(command("hns-status").find("Hide phase, ") == 0, "hns while hiding: " + command("hns-status"));

    // Hiding: everyone was sent to the spot; the seeker sees no hider, even right beside them.
    run_for(500000);
    for (auto *g : {&s, &h1, &h2, &h3})
        check(!g->teleports.empty(), g->name + " was not sent to the start");
    check(x.teleports.empty(), "A player outside the round was teleported");
    h1.at = {2, 0, 0};
    s.at = {0, 0, 0};
    check(!sees(s, h1), "While hiding, the seeker was sent a hider 2 m away");
    check(sees(h1, s), "A hider is not sent the seeker");
    check(sees(x, h1) && sees(x, s), "A player outside the round is not sent everyone");
    check(sees(s, x) && sees(h1, x), "The players of the round are not sent a player outside it");
    check(sees(h1, h2), "A hider is not sent another hider");

    // A seeker who leaves the spot while hiding is sent back, once a second at most.
    s.teleports.clear();
    s.at = {20, 0, 0};
    run_for(1500000);
    check(!s.teleports.empty(), "A seeker 20 m from the start was not sent back");
    check(s.teleports.size() <= 2, "The seeker was sent back more than once a second");
    check(s.teleports.empty() || std::abs(s.teleports.back()[0]) < 5.f, "The seeker was not sent back to the start");

    // Seeking: only within 25 m.
    // The hiders hide: far from the spot, where the seeker waits.
    h1.at = {30, 0, 0};
    h2.at = {0, 0, 40};
    h3.at = {0, 0, -40};
    s.at = {0, 0, 0};
    check(!logged("[hns] seek phase"), "The seek phase began before the hide time was up");
    for (unsigned i = 0; i < 400 && !logged("[hns] seek phase"); ++i) run_for(100000);
    check(logged("[hns] seek phase"), "The seek phase did not begin after the hide time");
    check(command("hns-status").find("Seek phase, ") == 0, "hns while seeking");
    s.at = {0, 0, 0};
    h1.at = {30, 0, 0};
    check(!sees(s, h1), "A hider 30 m away was sent to the seeker");
    h1.at = {20, 0, 0};
    check(sees(s, h1), "A hider 20 m away was not sent to the seeker");
    // Without a fresh position the hider stays hidden, and is not found.
    h1.at = {2, 0, 0};
    h1.silent = true;
    run_for(1200000); // their last pose is now over a second old
    check(!sees(s, h1), "A hider without a fresh position was sent to the seeker");
    check(!logged("found Hider One"), "A hider without a fresh position was found");
    h1.silent = false;

    // Found within 3 m for a second: they seek from then on.
    run_for(1500000);
    check(logged("[hns] Seeker Sam (" + id(s) + ") found Hider One (" + id(h1) + ") in "), "The find was not logged");
    h2.at = {30, 0, 30};
    h1.at = {30, 0, 0};
    check(!sees(h1, h2), "A found hider, now seeking, is still sent a hider 30 m away");
    h2.at = {30, 0, 15};
    check(sees(h1, h2), "A found hider, now seeking, is not sent a hider 15 m away");
    h2.at = {0, 0, 40};

    // A hider who leaves is out of the round, and is no one's hider when they come back.
    h3.leave();
    run_for(2000000);
    check(logged("[hns] Hider Three (" + id(h3) + ") left"), "A hider leaving was not logged");
    // One hider left: they win, and everyone sees everyone again.
    check(logged("[hns] ended: Hider Two win"), "The last hider did not win");
    check(command("hns-status") == "No hide and seek round.", "The round did not end");
    h2.at = {0, 0, 40};
    check(sees(s, h2), "After the round, the seeker is still not sent a hider far away");
    h3.hello(now);
    run_for(4000000);
    check(h3.joined, "Hider Three did not come back");

    // Time up: every hider still hidden wins.
    h1.at = {0, 0, 60};
    h2.at = {0, 0, -60};
    reply = command("hns-start " + id(s) + " " + id(h1) + "," + id(h2) + " at 0 0 0 hide 10 seek 30");
    check(reply.find("Hide and seek started: 1 seeker(s), 2 hider(s).") == 0, "Second round: " + reply);
    run_for(1000000);
    h1.at = {0, 0, 60};
    h2.at = {0, 0, -60};
    s.at = {0, 0, 0};
    run_for(42000000);
    check(logged("[hns] ended: Hider One, Hider Two win"), "Time up did not end the round with both hiders winning");

    // Stopped from the console: everyone sees everyone.
    reply = command("hns-start " + id(s) + " " + id(h1) + "," + id(h2) + " at 0 0 0 hide 10 seek 30");
    run_for(11000000);
    check(command("hns-found " + id(h2)) == "Hider Two is found.", "hns-found reply");
    check(logged("[hns] Hider Two (" + id(h2) + ") found by console"), "A find from the console was not logged");
    check(logged("[hns] ended: Hider One win"), "The last hider after a console find did not win");
    reply = command("hns-start " + id(s) + " " + id(h1) + "," + id(h2) + " at 0 0 0 hide 10 seek 30");
    run_for(1000000);
    check(command("hns-stop") == "Hide and seek stopped.", "hns-stop reply");
    check(logged("[hns] stopped by console"), "hns-stop was not logged");
    h1.at = {0, 0, 60};
    check(sees(s, h1), "After hns-stop, the seeker is still not sent a hider far away");
    check(command("hns-stop") == "No hide and seek round is running.", "hns-stop without a round");

    // The last seeker leaves: a hider seeks instead and the round goes on (2026-10-14: a lone seeker
    // quit a second into the seek phase, and every hider won).
    reply = command("hns-start " + id(s) + " " + id(h1) + "," + id(h2) + "," + id(h3) + " at 0 0 0 hide 10 seek 30");
    check(reply.find("Hide and seek started: 1 seeker(s), 3 hider(s).") == 0, "Round with three hiders: " + reply);
    run_for(1000000);
    s.leave();
    run_for(2000000);
    check(logged("[hns] Seeker Sam (" + id(s) + ") left"), "The seeker leaving was not logged");
    check(logged(") seeks now: the seekers left"), "No hider was made the seeker when the last seeker left");
    check(!logged("[hns] ended: Hider One, Hider Two, Hider Three win"), "The round ended when the last seeker left");
    check(command("hns-status").find("Hide phase") == 0, "The round did not go on without its seeker");
    {
        const auto status = command("hns-status");
        const auto seekers = status.substr(status.find("Seekers: "));
        check(seekers.find("Hider") != std::string::npos && seekers.find("Seeker Sam") == std::string::npos,
              "The new seeker is not listed: " + status);
    }
    check(command("hns-stop") == "Hide and seek stopped.", "hns-stop after a new seeker");
    s.hello(now);
    run_for(4000000);
    check(s.joined, "Seeker Sam did not come back");

    // A new map ends the round.
    reply = command("hns-start " + id(s) + " " + id(h1) + "," + id(h2) + " at 0 0 0 hide 10 seek 30");
    run_for(1000000);
    reply = command("map Isle of Grom");
    run_for(1000000);
    check(logged("[hns] the map changed"), "A new map did not end the round: " + reply);
    check(command("hns-status") == "No hide and seek round.", "A round outlived the map");

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
