//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/availability.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>

#include "dagda/ledger/trie_diff.h"
#include "dagda/store/messages.h"
#include "dagda/store/placement.h"

namespace dagda::store {
namespace {

using dannan::contract::Clock;
using dannan::contract::TimePoint;
using dannan::identity::NodeId;
using dannan::membership::NodeRecord;
using ledger::ObjectTrie;
using ledger::TrieQuestion;
using ledger::TrieQuestionKind;

// A peer's held trie asked over the network, every round of a diff from the
// snapshot its root question pinned there.
class RemoteTrie final : public ledger::TrieSource {
 public:
  RemoteTrie(dannan::Routes& routes, NodeRecord peer, std::chrono::milliseconds deadline, std::chrono::milliseconds no_progress)
      : _routes(routes), _peer(std::move(peer)), _deadline(deadline), _watch{no_progress, std::nullopt} {}

  ObjectTrie::Summary root() override {
    const auto summary = ledger::decode_trie_root(ask(question(TrieQuestionKind::root, {})));
    _at = summary.hash;
    return summary;
  }
  std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix> p) override {
    std::vector<ObjectTrie::Children> out;
    for (size_t from = 0; from < p.size(); from += ledger::trie_question_max) {
      const auto part = p.subspan(from, std::min(ledger::trie_question_max, p.size() - from));
      auto answers = ledger::decode_trie_children(ask(question(TrieQuestionKind::children, part)));
      if (answers.size() != part.size()) throw std::runtime_error("peer answered a different number of prefixes");
      std::move(answers.begin(), answers.end(), std::back_inserter(out));
    }
    return out;
  }
  std::vector<std::vector<ObjectTrie::Record>> records(std::span<const ObjectTrie::Prefix> p) override {
    std::vector<std::vector<ObjectTrie::Record>> out;
    for (size_t from = 0; from < p.size(); from += ledger::trie_records_max) {
      const auto part = p.subspan(from, std::min(ledger::trie_records_max, p.size() - from));
      auto answers = ledger::decode_trie_records(ask(question(TrieQuestionKind::records, part)));
      if (answers.size() != part.size()) throw std::runtime_error("peer answered a different number of prefixes");
      std::move(answers.begin(), answers.end(), std::back_inserter(out));
    }
    return out;
  }
  uint64_t questions() const noexcept { return _questions; }
  uint64_t reply_bytes() const noexcept { return _bytes; }
  std::chrono::milliseconds waited() const noexcept { return _waited; }

 private:
  TrieQuestion question(TrieQuestionKind kind, std::span<const ObjectTrie::Prefix> p) const {
    TrieQuestion out;
    out.kind = kind;
    out.at = _at;
    out.prefixes.assign(p.begin(), p.end());
    return out;
  }
  Bytes ask(const TrieQuestion& question) {
    const auto started = std::chrono::steady_clock::now();
    auto outcome =
        _routes.call_and_wait(_peer, message::trie_diff, ledger::encode_trie_question(question), dannan::sched::FrameType::speculative, _deadline, _watch);
    _waited += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    ++_questions;
    if (!outcome.reply) throw std::runtime_error("peer did not answer trie_diff: " + (outcome.failure.empty() ? std::string("no reply") : outcome.failure));
    if (outcome.reply->type != message::trie_diff_reply) {
      std::string reason = "no reason given";
      if (const auto refusal = dannan::net::refusal_of(*outcome.reply)) reason = std::to_string(refusal->status) + " " + refusal->code;
      throw std::runtime_error("peer refused trie_diff: " + reason);
    }
    _bytes += outcome.reply->payload.size();
    return std::move(outcome.reply->payload);
  }

  dannan::Routes& _routes;
  const NodeRecord _peer;
  const std::chrono::milliseconds _deadline;
  const dannan::net::CallWatch _watch;
  dannan::identity::Hash256 _at{};
  uint64_t _questions = 0;
  uint64_t _bytes = 0;
  std::chrono::milliseconds _waited{0};
};

}  // namespace

AvailabilityService::AvailabilityService(dannan::Context& context, const contract::ObjectLedger& ledger, AvailabilityOptions options)
    : _context(context),
      _ledger(ledger),
      _options(options),
      _time(options.time ? *options.time : context.time().source()),
      _logger(context.logger("dagda.availability")) {}

bool AvailabilityService::unavailable(const ObjectId& id) const {
  const auto current = _snapshot.handle();
  return current && current->survey.is_unavailable(id);
}

bool AvailabilityService::refresh(const std::function<void()>& pause) {
  const auto now = _time.now();
  const auto membership = _context.events().count(dannan::status::NodeEvents::membership);
  const auto inventory = _ledger.inventory();
  auto view = _ledger.held_view(contract::RetentionClass::data);
  const auto later = [&](TimePoint at) { _due = _due ? std::min(*_due, at) : at; };
  // A retry still ahead stays due; one whose time has come is this refresh's to make,
  // and what it leads to (a new retry, or none) is set below. Kept from a passed
  // retry, due() would name a time already gone after a second failure or an answer,
  // and wake the owner at once for nothing.
  _due.reset();
  if (_retry_at && *_retry_at > now) _due = _retry_at;
  // A disk being seeded: what this node holds cannot be named yet. Before
  // the first inventory there is nothing referenced to join.
  if (!view.identity.complete || !view.trie || !inventory) {
    later(now + _options.seed_retry);
    return false;
  }
  const auto refresh_started = Clock::now();
  const bool lost = _surveyed && view.identity.removals != _joined_identity.removals;
  const bool changed = !_surveyed || inventory != _joined_inventory || view.identity.hash != _joined_identity.hash || lost;
  const bool gained_only = _surveyed && changed && !lost && inventory == _joined_inventory;

  const auto self = _context.identity().id();
  std::vector<NodeRecord> hosts;
  std::vector<std::pair<NodeId, uint64_t>> peers;
  for (const auto& peer : _context.nodes().active()) {
    if (peer.id == self || !peer.advert(extents_advert)) continue;
    hosts.push_back(peer);
    peers.emplace_back(peer.id, used_of(peer));
  }
  std::sort(peers.begin(), peers.end());
  const auto previous = _snapshot.handle();
  const bool retry = _retry_at && now >= *_retry_at;
  const bool same_peers = peers.size() == _surveyed_peers.size() &&
                          std::equal(peers.begin(), peers.end(), _surveyed_peers.begin(), [](const auto& a, const auto& b) { return a.first == b.first; });
  bool shrank = false;
  bool grew = false;
  for (size_t i = 0; same_peers && i < peers.size(); ++i) {
    shrank = shrank || peers[i].second < _surveyed_peers[i].second;
    grew = grew || peers[i].second > _surveyed_peers[i].second;
  }
  // What must be asked of the peers again, whatever it costs.
  const bool must_ask = !_surveyed || membership != _surveyed_membership || !same_peers || shrank || retry;
  const bool missing = _surveyed && previous && (!previous->survey.unavailable.empty() || !previous->survey.unknown.empty());
  // What the inventory or this node's holdings changed, or a peer holding
  // more while something is missing (a peer taking bulk writes grows all
  // the time), waits for the duty share.
  const auto survey_due = _surveyed_at + _surveyed_cost * _options.share;
  const bool paced = changed || (grew && missing);
  if (!must_ask && paced && now < survey_due) {
    later(survey_due);
    return false;
  }
  if (!must_ask && !paced) {
    _surveyed_peers = std::move(peers);
    return false;
  }
  // A gain alone asks nobody: what this node gained was already available.
  const bool ask = must_ask || !gained_only || (grew && missing);

  if (changed) {
    _missing.publish(inventory->not_held(contract::RetentionClass::data, view.trie->parts(), pause));
    _joined_inventory = inventory;
    _joined_identity = view.identity;
  }
  const auto missing_here = _missing.handle();
  AvailabilitySnapshot next;
  next.generation = inventory->generation();
  next.surveyed_unix_ms = _context.time().unix_ms();
  bool retry_needed = false;
  if (ask) {
    // A host out of reach holds nothing reachable: with none in reach, what
    // this node lacks is unavailable until one comes back.
    std::vector<RemoteTrie> tries;
    tries.reserve(hosts.size());
    std::vector<ledger::TrieSource*> sources;
    for (const auto& host : hosts) sources.push_back(&tries.emplace_back(_context.routes(), host, _options.call, _options.call_no_progress));
    std::vector<ledger::PeerDiff> diffs;
    next.survey = ledger::survey_by_diff(view.trie, *missing_here, inventory->lookup(contract::RetentionClass::data), sources, diffs, pause);
    ledger::PeerLacks lacks;
    lacks.generation = inventory->generation();
    lacks.complete = true;
    // Macha's lines (ledger/availability.cpp:101, service/availability_service.cpp:473-479
    // at 98b816d): each peer that could not finish, then each diff.
    for (size_t i = 0; i < hosts.size(); ++i)
      if (!diffs[i].answered) _logger.debug("availability: a peer could not finish a trie diff: " + diffs[i].why);
    for (size_t i = 0; i < hosts.size(); ++i) {
      _logger.debug("availability diff " + std::string(diffs[i].answered ? "done" : "failed") +
                    " peer=" + dannan::identity::to_string(hosts[i].id).substr(0, 12) + " questions=" + std::to_string(tries[i].questions()) +
                    " reply_bytes=" + std::to_string(tries[i].reply_bytes()) + " waited_ms=" + std::to_string(tries[i].waited().count()));
      if (!diffs[i].answered) {
        lacks.complete = false;
        retry_needed = true;
        continue;
      }
      lacks.lacks.emplace(hosts[i].id, std::make_shared<const std::vector<ObjectId>>(std::move(diffs[i].lacks)));
    }
    if (next.survey.peers_failed && _surveyed && previous) ledger::keep_unavailable(next.survey, previous->survey, view.held);
    _lacks.publish(std::move(lacks));
    _surveyed_at = now;
  } else {
    // Gained alone: what was missing is still missing, less what this node
    // now holds.
    next.survey = previous->survey;
    const auto kept = [&](std::vector<ObjectId>& ids) {
      std::vector<ObjectId> out;
      std::set_intersection(ids.begin(), ids.end(), missing_here->begin(), missing_here->end(), std::back_inserter(out));
      ids = std::move(out);
    };
    kept(next.survey.unavailable);
    kept(next.survey.unknown);
  }
  if (ask || changed) _surveyed_cost = Clock::now() - refresh_started;
  // Macha's line but its path table's word (table=), which stays in Macha's.
  const auto referenced = inventory->size(contract::RetentionClass::data);
  _logger.debug("availability surveyed generation=" + std::to_string(next.generation) + " referenced=" + std::to_string(referenced) +
                " local=" + std::to_string(referenced - std::min<size_t>(referenced, missing_here->size())) +
                " unavailable=" + std::to_string(next.survey.unavailable.size()) + " unknown=" + std::to_string(next.survey.unknown.size()) +
                " peers=" + std::to_string(next.survey.peers_asked) + " peers_failed=" + std::to_string(next.survey.peers_failed) + " rounds=" +
                std::to_string(next.survey.rounds) + " prefixes_asked=" + std::to_string(next.survey.nodes_asked) + " method=" + (ask ? "diff" : "gained"));
  _snapshot.publish(std::move(next));
  if (!ask) return false;
  if (retry_needed) {
    _retry_backoff = std::clamp<Clock::duration>(_retry_backoff * 2, _options.retry_floor, _options.retry_ceiling);
    _retry_at = now + _retry_backoff;
    later(*_retry_at);
  } else {
    _retry_backoff = {};
    _retry_at.reset();
  }
  _surveyed = true;
  _surveyed_membership = membership;
  _surveyed_peers = std::move(peers);
  return true;
}

}  // namespace dagda::store
