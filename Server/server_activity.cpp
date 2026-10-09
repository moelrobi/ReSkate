#include "server_activity.h"
#include "Extension/Throwdowns/throwdown_wire.h"
#include <algorithm>
#include <iterator>
#include <format>

namespace dingosdk::server {
using multiplayer::ThrowdownMessage;
using Kind = ThrowdownMessage::Kind;

namespace {
constexpr std::string_view jam = "JamSession", spot_battle = "SpotBattle", skate = "ThrowdownSkate";
// A queue's leader repeats its offer every 10 s; one not heard from for this long is gone.
constexpr std::uint64_t queue_forget_us = 60'000'000;
// A running throwdown sends rows, turns or attempts at least every turn (at most a minute),
// and a Jam a score whenever anyone scores; after this long without any it has finished.
constexpr std::uint64_t jam_quiet_us = 60'000'000, turns_quiet_us = 90'000'000;
constexpr std::uint64_t lead_interval_us = 15'000'000;
// A decided S.K.A.T.E. is remembered this long, so what its players still send about it is ignored.
constexpr std::uint64_t finished_forget_us = 600'000'000;
// S.K.A.T.E.: each failed copy is a letter; this many and the player is out.
constexpr unsigned letters_to_lose = 5;
// Chat about drops placed: at most one line per leader this often (offers are cheap to send).
constexpr std::uint64_t announce_interval_us = 30'000'000;
// More objects than this in one change are summed up instead of listed.
constexpr std::size_t listed_objects = 3;

std::string mode(std::string_view series) {
    if (series == jam) return "Jam";
    if (series == spot_battle) return "Spot Battle";
    if (series == skate) return "S.K.A.T.E.";
    return "throwdown"; // the series is peer text: never echo it into chat
}
std::string points(std::int64_t value) {
    auto digits = std::to_string(value < 0 ? -value : value);
    for (auto at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<std::size_t>(at), ",");
    return value < 0 ? "-" + digits : digits;
}
std::string place(const std::array<float, 3> &at) {
    return std::format("({:.0f}, {:.0f}, {:.0f})", at[0], at[1], at[2]);
}
std::string item_name(const NetworkObject &object) {
    auto text = object.item.empty() ? std::string("an object") : object.item;
    if (text.size() > 64) text.resize(64);
    return text;
}
} // namespace

std::string ActivityLog::name(std::uint64_t player) {
    if (auto current = name_ ? name_(player) : std::string{}; !current.empty()) return names_[player] = current;
    const auto known = names_.find(player);
    return known != names_.end() ? known->second : std::to_string(player);
}
std::string ActivityLog::title(const Key &key, const Throwdown &t) { return name(key.first) + "'s " + mode(t.series); }

void ActivityLog::throwdown(std::uint64_t sender, std::span<const std::uint8_t> bytes, const std::array<float, 3> *at,
                            std::uint64_t now) {
    const auto decoded = multiplayer::decode_throwdown(bytes);
    if (!decoded) return;
    const auto &m = *decoded;
    const Key key{m.leader, m.id};
    if (finished_.contains(key)) return;
    auto found = throwdowns_.find(key);
    if (m.kind == Kind::offer) {
        if (sender != m.leader) return;
        if (found != throwdowns_.end()) {
            found->second.last = now;
            if (found->second.series.empty()) found->second.series = m.series;
            return;
        }
        // A leader has one drop at a time: one of theirs still here has finished.
        for (auto it = throwdowns_.begin(); it != throwdowns_.end();) {
            if (it->first.first != m.leader) { ++it; continue; }
            if (it->second.started) finish(it->first, it->second);
            it = throwdowns_.erase(it);
        }
        auto &t = throwdowns_[key];
        t.series = m.series;
        t.last = now;
        for (const auto player : m.order)
            if (player != m.leader) t.queue.push_back(player);
        log_("[throwdown] " + name(m.leader) + " placed a " + mode(m.series) + " drop" + (at ? " near " + place(*at) : ""));
        if (const auto last = announced_.find(m.leader);
            announce_ && (last == announced_.end() || now - last->second >= announce_interval_us)) {
            announced_[m.leader] = now;
            announce_(name(m.leader) + " placed a " + mode(m.series) + " throwdown. Find it on the map to join.");
        }
        return;
    }
    if (found == throwdowns_.end()) {
        // Started before the server saw its offer (it restarted, or the map changed back).
        if (m.kind == Kind::close || m.kind == Kind::leave) return;
        found = throwdowns_.emplace(key, Throwdown{}).first;
        // Only a running S.K.A.T.E. sends attempts: keep its name and give it a result.
        if (m.kind == Kind::attempt) {
            found->second.series = skate;
            found->second.started = true;
        }
    }
    auto &t = found->second;
    t.last = now;
    const auto who = name(sender);
    switch (m.kind) {
    case Kind::offer: break;
    case Kind::close:
        if (sender != m.leader || t.started) return;
        log_("[throwdown] " + title(key, t) + " drop was removed");
        throwdowns_.erase(found);
        return;
    case Kind::join:
        if (t.started || sender == m.leader || std::ranges::find(t.queue, sender) != t.queue.end()) return;
        t.queue.push_back(sender);
        log_(std::format("[throwdown] {} joined {} ({} in the queue)", who, title(key, t), t.queue.size() + 1));
        return;
    case Kind::leave:
        if (t.started) {
            if (!t.quit.insert(sender).second) return;
            log_("[throwdown] " + who + " quit " + title(key, t));
            if (decide(found)) return;
            if (std::ranges::all_of(t.players, [&](std::uint64_t p) { return t.quit.contains(p); })) throwdowns_.erase(found);
            return;
        }
        if (const auto it = std::ranges::find(t.queue, sender); it != t.queue.end()) {
            t.queue.erase(it);
            log_("[throwdown] " + who + " left the queue for " + title(key, t));
        }
        return;
    case Kind::start: {
        if (sender != m.leader || t.started) return;
        t.started = true;
        t.players = m.order;
        t.queue.clear();
        std::string list;
        for (const auto player : t.players) list += (list.empty() ? "" : ", ") + name(player);
        log_(std::format("[throwdown] {} started with {} player{}: {}", title(key, t), t.players.size(),
                         t.players.size() == 1 ? "" : "s", list));
        return;
    }
    case Kind::score: {
        // Jam: the sender's running total.
        t.total[sender] = m.value;
        const auto top = std::ranges::max_element(t.total, [&](const auto &a, const auto &b) {
            // Ties keep the player already in front.
            return a.second < b.second || (a.second == b.second && b.first == t.leading);
        });
        if (top == t.total.end() || top->first == t.leading || top->second <= 0) return;
        t.leading = top->first;
        if (t.lead_logged && now - t.lead_logged < lead_interval_us) return;
        t.lead_logged = now;
        log_("[throwdown] " + name(top->first) + " takes the lead in " + title(key, t) + " with " + points(top->second));
        return;
    }
    case Kind::row:
        // Spot Battle: the round board adds each line; the overall board is set to the best turn.
        if (m.add) t.turn[sender] += m.value;
        else t.total[sender] = m.value;
        return;
    case Kind::turn_end: {
        const auto scored = std::exchange(t.turn[sender], 0);
        const auto best = t.total.find(sender);
        log_(std::format("[throwdown] {}: {} scored {} in round {}{}", title(key, t), who, points(scored), m.value,
                         best != t.total.end() ? " (best " + points(best->second) + ")" : ""));
        return;
    }
    case Kind::attempt: {
        // One attempt per turn: a second one for a turn already logged is a repeat.
        if (const auto last = t.attempted.find(sender); last != t.attempted.end() && m.value <= last->second) return;
        t.attempted[sender] = m.value;
        ++t.tries[sender][m.add ? 0 : 1];
        using Role = ThrowdownMessage::Role;
        log_(std::format("[throwdown] {}: {} {} (turn {}{}{})", title(key, t), who, m.add ? "landed" : "missed", m.value,
                         m.role == Role::set ? ", set" : m.role == Role::copy ? ", copy" : "",
                         m.timed_out ? ", timed out" : ""));
        // A failed set gives no letter; a failed copy (timed out too) gives one.
        if (m.role == Role::copy && !m.add && !t.out.contains(sender) && ++t.letters[sender] >= letters_to_lose) {
            t.out.insert(sender);
            log_(std::format("[throwdown] {}: {} is out (S.K.A.T.E.)", title(key, t), who));
        }
        decide(found);
        return;
    }
    }
}

// S.K.A.T.E. as each player's game plays it: it goes on until one player is left standing
// (not out on letters, not quit, still here). When everyone else quit or left instead, the
// last player's game ends it with no winner (Extension/Throwdowns/throwdown_relay.cpp, end_alone).
bool ActivityLog::decide(std::map<Key, Throwdown>::iterator it) {
    auto &t = it->second;
    if (t.series != skate || !t.started || t.players.size() < 2) return false;
    std::vector<std::uint64_t> standing;
    bool lettered{};
    for (const auto player : t.players) {
        if (t.out.contains(player)) lettered = true;
        else if (!t.quit.contains(player) && !t.gone.contains(player)) standing.push_back(player);
    }
    if (standing.size() > 1) return false;
    finish(it->first, t, standing.size() == 1 && lettered ? standing.front() : 0);
    finished_[it->first] = t.last;
    throwdowns_.erase(it);
    return true;
}

void ActivityLog::finish(const Key &key, const Throwdown &t, std::uint64_t winner) {
    std::vector<std::uint64_t> order = t.players;
    for (const auto &[player, value] : t.total)
        if (std::ranges::find(order, player) == order.end()) order.push_back(player);
    for (const auto &[player, tries] : t.tries)
        if (std::ranges::find(order, player) == order.end()) order.push_back(player);
    const auto total = [&](std::uint64_t player) {
        const auto it = t.total.find(player);
        return it == t.total.end() ? std::int64_t{} : it->second;
    };
    // S.K.A.T.E. is won on letters: its players stay in turn order.
    const bool ranked = t.series != skate;
    if (ranked) std::ranges::stable_sort(order, [&](std::uint64_t a, std::uint64_t b) { return total(a) > total(b); });
    std::string results;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto player = order[i];
        std::string entry;
        if (ranked) entry = std::format("{}. {} {}", i + 1, name(player), points(total(player)));
        else {
            const auto it = t.tries.find(player);
            const auto tries = it == t.tries.end() ? std::array<unsigned, 2>{} : it->second;
            entry = std::format("{} {} landed / {} missed", name(player), tries[0], tries[1]);
        }
        if (t.quit.contains(player)) entry += " (quit)";
        results += (results.empty() ? "" : ", ") + entry;
    }
    std::string decided;
    if (!ranked) {
        // Appended, so the counts before it read as they always did.
        std::string letters;
        for (const auto player : order) {
            const auto it = t.letters.find(player);
            letters += std::format("{}{} {}", letters.empty() ? "" : ", ", name(player), it == t.letters.end() ? 0U : it->second);
        }
        decided = "; winner " + (winner ? name(winner) : std::string("none")) + (letters.empty() ? "" : "; letters " + letters);
    }
    log_("[throwdown] " + title(key, t) + " has finished" + (results.empty() ? "" : ": " + results) + decided);
}

void ActivityLog::tick(std::uint64_t now) {
    std::erase_if(finished_, [&](const auto &entry) { return now > entry.second && now - entry.second > finished_forget_us; });
    for (auto it = throwdowns_.begin(); it != throwdowns_.end();) {
        const auto &t = it->second;
        const auto quiet = now > t.last ? now - t.last : 0;
        if (!t.started && quiet > queue_forget_us) {
            it = throwdowns_.erase(it); // its leader stopped offering it without saying so
            continue;
        }
        if (t.started && quiet > (t.series == jam ? jam_quiet_us : turns_quiet_us)) {
            finish(it->first, t);
            it = throwdowns_.erase(it);
            continue;
        }
        ++it;
    }
}

void ActivityLog::left(std::uint64_t player) {
    name(player); // keep the name for results after they left
    for (auto it = throwdowns_.begin(); it != throwdowns_.end();) {
        auto &t = it->second;
        std::erase(t.queue, player);
        // A queue goes with its leader; a running throwdown carries on for the others.
        if (!t.started && it->first.first == player) {
            it = throwdowns_.erase(it);
            continue;
        }
        // A S.K.A.T.E. player who left plays no more turns: it may be over for the others.
        if (t.started && std::ranges::find(t.players, player) != t.players.end() && t.gone.insert(player).second) {
            const auto next = std::next(it);
            if (decide(it)) {
                it = next;
                continue;
            }
        }
        ++it;
    }
}

void ActivityLog::objects(std::uint64_t owner, const std::vector<NetworkObject> &before,
                          const std::vector<NetworkObject> &after) {
    std::map<std::uint64_t, const NetworkObject *> old;
    for (const auto &object : before) old.emplace(object.id, &object);
    std::vector<const NetworkObject *> added, removed;
    for (const auto &object : after)
        if (!old.erase(object.id)) added.push_back(&object);
    for (const auto &[id, object] : old) removed.push_back(object);
    if (added.empty() && removed.empty()) return; // moved, turned or resized
    const auto who = name(owner);
    const auto total = std::format(" ({} in total)", after.size());
    if (added.size() > listed_objects)
        log_(std::format("[objects] {} {} {} objects{}", who, before.empty() ? "brought" : "placed", added.size(), total));
    else
        for (const auto *object : added)
            log_("[objects] " + who + " placed " + item_name(*object) + " at " + place(object->position));
    if (removed.size() > listed_objects)
        log_(std::format("[objects] {} removed {} objects{}", who, removed.size(), total));
    else
        for (const auto *object : removed)
            log_("[objects] " + who + " removed " + item_name(*object) + " at " + place(object->position));
}
} // namespace dingosdk::server
