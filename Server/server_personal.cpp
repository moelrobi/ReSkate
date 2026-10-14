#include "server_host.h"
#include "server_text.h"
#include <algorithm>

// Personal polls and cards: a poll only one player sees and answers (poll-to), and a card for
// one or several players in each one's own slot (announce-to). Only those players' rosters carry
// them (send_roster asks personal_roster); the global vote, poll and card are untouched, and
// show again for a player once their own has ended.
namespace dingosdk::server {
namespace {
std::uint64_t microseconds(unsigned seconds) { return static_cast<std::uint64_t>(seconds) * 1000000; }
std::string joined(const std::vector<std::string> &items, std::string_view between) {
    std::string text;
    for (const auto &item : items) text += (text.empty() ? "" : std::string(between)) + item;
    return text;
}
} // namespace

void Host::announce_to(const std::vector<Guest *> &players, std::string_view text) {
    const auto line = clean_chat_text(text);
    if (line.empty() || players.empty()) return;
    std::vector<std::string> names;
    for (const auto *guest : players) names.push_back(guest_name(*guest));
    log_("[announcement -> " + joined(names, ", ") + "] " + line);
    // Up as long as a global card of the same text; one card (one id) for everyone it is for.
    const auto seconds = static_cast<std::uint16_t>(std::clamp<std::size_t>(5 + line.size() / 20, 6, 15));
    if (!++announcement_ids_) ++announcement_ids_;
    for (const auto *guest : players)
        personal_cards_[guest->member.id] = {{announcement_ids_, seconds, line}, now_ + microseconds(seconds)};
    roster_dirty_ = true;
}

std::string Host::announce_to_command(std::string_view argument) {
    const std::string usage = "announce-to <player|SteamID64>[,<player|SteamID64>...] <text>";
    const auto [who, text] = split(argument);
    if (trim(text).empty()) return usage;
    std::vector<Guest *> players;
    std::string missing;
    for (std::size_t at = 0; at <= who.size();) {
        const auto comma = who.find(',', at);
        const auto wanted = trim(who.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
        if (!wanted.empty()) {
            auto *guest = match_player(wanted);
            if (!guest) missing += "No single connected player matches \"" + std::string(wanted) + "\".\n";
            else if (std::find(players.begin(), players.end(), guest) == players.end()) players.push_back(guest);
        }
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
    if (players.empty()) return missing.empty() ? usage : missing.substr(0, missing.size() - 1);
    announce_to(players, text);
    std::vector<std::string> names;
    for (const auto *guest : players) names.push_back(guest_name(*guest));
    return missing + "Announced to " + joined(names, ", ") + ".";
}

std::string Host::poll_to_command(std::string_view argument) {
    const std::string usage = "poll-to <player|SteamID64> <question> | <answer> | <answer>... (2 to " +
                              std::to_string(multiplayer::max_vote_answers) + " answers) | poll-to <player|SteamID64> end";
    const auto [who, text] = split(argument);
    if (who.empty() || trim(text).empty()) return usage;
    auto *guest = match_player(who);
    if (!guest) return "No single connected player matches \"" + std::string(who) + "\".";
    const auto id = guest->member.id;
    const auto name = guest_name(*guest);
    if (lower(trim(text)) == "end") {
        if (!personal_polls_.contains(id)) return name + " has no poll open.";
        end_personal_poll(id, "closed");
        return "Closed " + name + "'s poll.";
    }
    const auto parts = poll_parts(text);
    if (parts.size() < 3 || parts.size() > multiplayer::max_vote_answers + 1) return usage;
    if (personal_polls_.contains(id)) end_personal_poll(id, "replaced");
    PersonalPoll poll;
    // From the same count as the global polls: no game mistakes one poll for another.
    if (!++vote_ids_) ++vote_ids_;
    poll.id = vote_ids_;
    poll.question = parts.front();
    poll.answers.assign(parts.begin() + 1, parts.end());
    poll.ends = now_ + microseconds(config_.votes.poll_seconds);
    log_("[poll-to] " + name + " (" + std::to_string(id) + ") asked #" + std::to_string(poll.id) + ": " + poll.question + " | " +
         joined(poll.answers, " | "));
    personal_polls_[id] = std::move(poll);
    roster_dirty_ = true;
    return "Asked " + name + ".";
}

void Host::end_personal_poll(std::uint64_t id, std::string_view why) {
    const auto found = personal_polls_.find(id);
    if (found == personal_polls_.end()) return;
    const auto *guest = find(id);
    log_("[poll-to] " + (guest ? guest_name(*guest) : std::to_string(id)) + " (" + std::to_string(id) + ") #" +
         std::to_string(found->second.id) + " ended: " + std::string(why));
    personal_polls_.erase(found);
    roster_dirty_ = true;
}

bool Host::answer_personal_poll(Guest &guest, std::size_t answer) {
    const auto id = guest.member.id;
    const auto found = personal_polls_.find(id);
    if (found == personal_polls_.end()) return false;
    const auto &poll = found->second;
    if (answer >= poll.answers.size()) {
        reply(guest, "The poll has " + std::to_string(poll.answers.size()) + " answers.");
        return true;
    }
    // One answer ends it: the card closes, and the global vote or poll shows again if one runs.
    log_("[poll-to] " + guest_name(guest) + " (" + std::to_string(id) + ") answered #" + std::to_string(poll.id) + ": " +
         std::to_string(answer + 1) + " " + poll.answers[answer]);
    personal_polls_.erase(found);
    roster_dirty_ = true;
    return true;
}

void Host::tick_personal() {
    for (auto card = personal_cards_.begin(); card != personal_cards_.end();)
        if (now_ >= card->second.until) {
            card = personal_cards_.erase(card);
            roster_dirty_ = true;
        } else {
            ++card;
        }
    std::vector<std::uint64_t> ended;
    for (const auto &[id, poll] : personal_polls_)
        if (now_ >= poll.ends) ended.push_back(id);
    for (const auto id : ended) end_personal_poll(id, "timeout");
}

void Host::personal_left(std::uint64_t id, const std::string &name) {
    personal_cards_.erase(id);
    const auto found = personal_polls_.find(id);
    if (found == personal_polls_.end()) return;
    log_("[poll-to] " + name + " (" + std::to_string(id) + ") #" + std::to_string(found->second.id) + " ended: left");
    personal_polls_.erase(found);
}

void Host::personal_roster(std::uint64_t id, multiplayer::ServerAnnouncement &card, multiplayer::ServerVote &vote) const {
    if (const auto found = personal_cards_.find(id); found != personal_cards_.end()) {
        card = found->second.card;
        card.seconds = static_cast<std::uint16_t>(found->second.until > now_ ? (found->second.until - now_ + 999999) / 1000000 : 0);
    }
    if (const auto found = personal_polls_.find(id); found != personal_polls_.end()) {
        const auto &poll = found->second;
        vote = {};
        vote.id = poll.id;
        vote.kind = multiplayer::server_vote_poll;
        vote.outcome = multiplayer::vote_running;
        vote.label = poll.question;
        vote.answers = poll.answers;
        vote.counts.assign(poll.answers.size(), 0);
        vote.seconds = static_cast<std::uint16_t>(std::min<std::uint64_t>(poll.ends > now_ ? (poll.ends - now_ + 999999) / 1000000 : 0, 65535));
    }
}

} // namespace dingosdk::server
