//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/test/fixtures/torrent_request.h"

#include <algorithm>
#include <vector>

namespace dagda::test::macha {

namespace {

constexpr uint8_t request_format = 1;

void write_node(dannan::net::Writer& writer, const NodeId& node) { writer.fixed(node.bytes); }

NodeId read_node(dannan::net::Reader& reader) {
  NodeId node;
  node.bytes = reader.fixed<16>();
  return node;
}

void write_optional_u64(dannan::net::Writer& writer, const std::optional<uint64_t>& value) {
  writer.u8(value ? 1 : 0);
  if (value) writer.u64(*value);
}

std::optional<uint64_t> read_optional_u64(dannan::net::Reader& reader) {
  const auto present = reader.u8();
  if (present > 1) throw dannan::net::DecodeError("bad torrent request optional");
  if (!present) return std::nullopt;
  return reader.u64();
}

}  // namespace

bool torrent_phase_terminal(TorrentPhase phase) noexcept {
  return phase == TorrentPhase::completed || phase == TorrentPhase::failed || phase == TorrentPhase::cancelled;
}

bool torrent_request_live(const TorrentRequest& request) {
  return !request.removed_unix_ms && request.phase != TorrentPhase::cancelled && request.phase != TorrentPhase::failed &&
         request.desired != TorrentDesired::cancelled;
}

std::optional<std::string> TorrentRequestTraits::duplicate_key(const TorrentRequest& request) {
  if (!torrent_request_live(request) || request.info_hash.empty()) return std::nullopt;
  return request.info_hash;
}

void TorrentRequestTraits::lose(TorrentRequest& loser, const TorrentRequest& keeper) {
  loser.desired = TorrentDesired::cancelled;
  loser.desired_changed_unix_ms = std::max(loser.desired_changed_unix_ms, keeper.created_unix_ms);
  loser.desired_changed_by = NodeId{};
  loser.error_code = "duplicate_torrent";
  loser.error = "request " + keeper.id + " holds the same torrent";
}

void TorrentRequestTraits::encode(dannan::net::Writer& writer, const TorrentRequest& r) {
  writer.u8(request_format);
  writer.string(r.id);
  writer.string(r.info_hash);
  writer.string(r.source);
  writer.u64(r.created_unix_ms);
  write_node(writer, r.created_by);
  writer.u8(r.pinned_node_id ? 1 : 0);
  if (r.pinned_node_id) write_node(writer, *r.pinned_node_id);
  write_optional_u64(writer, r.remove_after_ms);
  writer.u64(r.settings_changed_unix_ms);
  write_node(writer, r.settings_changed_by);
  writer.u8(static_cast<uint8_t>(r.desired));
  writer.u64(r.desired_changed_unix_ms);
  write_node(writer, r.desired_changed_by);
  // The claim is the lease's own encoding.
  lease::encode(writer, r.claim);
  writer.u8(static_cast<uint8_t>(r.phase));
  writer.u64(r.phase_epoch);
  writer.u64(r.progress_unix_ms);
  writer.string(r.name);
  writer.u64(r.bytes_total);
  writer.string(r.ingest_job_id);
  writer.string(r.error_code);
  writer.string(r.error);
  writer.u64(r.completed_unix_ms);
  writer.u64(r.removed_unix_ms);
}

TorrentRequest TorrentRequestTraits::decode(dannan::net::Reader& reader) {
  if (reader.u8() != request_format) throw dannan::net::DecodeError("unknown torrent request format");
  TorrentRequest r;
  r.id = reader.string(max_torrent_request_text);
  r.info_hash = reader.string(max_torrent_request_text);
  r.source = reader.string(max_torrent_request_text);
  r.created_unix_ms = reader.u64();
  r.created_by = read_node(reader);
  if (const auto pinned = reader.u8(); pinned > 1)
    throw dannan::net::DecodeError("bad torrent request pin");
  else if (pinned)
    r.pinned_node_id = read_node(reader);
  r.remove_after_ms = read_optional_u64(reader);
  r.settings_changed_unix_ms = reader.u64();
  r.settings_changed_by = read_node(reader);
  const auto desired = reader.u8();
  if (desired > static_cast<uint8_t>(TorrentDesired::cancelled)) throw dannan::net::DecodeError("bad torrent request desired");
  r.desired = static_cast<TorrentDesired>(desired);
  r.desired_changed_unix_ms = reader.u64();
  r.desired_changed_by = read_node(reader);
  r.claim = lease::decode_optional_lease(reader);
  const auto phase = reader.u8();
  if (phase > static_cast<uint8_t>(TorrentPhase::cancelled)) throw dannan::net::DecodeError("bad torrent request phase");
  r.phase = static_cast<TorrentPhase>(phase);
  r.phase_epoch = reader.u64();
  r.progress_unix_ms = reader.u64();
  r.name = reader.string(max_torrent_request_text);
  r.bytes_total = reader.u64();
  r.ingest_job_id = reader.string(max_torrent_request_text);
  r.error_code = reader.string(max_torrent_request_text);
  r.error = reader.string(max_torrent_request_text);
  r.completed_unix_ms = reader.u64();
  r.removed_unix_ms = reader.u64();
  return r;
}

TorrentOwner::Outcome TorrentOwner::add(const std::string& id, const std::string& info_hash, const std::string& name, bool paused, std::optional<NodeId> pin) {
  Outcome out;
  for (const auto& [_, existing] : _head.records())
    if (!existing.removed_unix_ms && existing.info_hash == info_hash) {
      out.status = 409;
      out.code = "torrent_already_added";
      out.request = existing;
      if (existing.claim) out.node = existing.claim->holder;
      return out;
    }
  const auto self = _membership.self();
  TorrentRequest request;
  request.id = id;
  request.info_hash = info_hash;
  request.source = "magnet:?xt=urn:btih:" + info_hash;
  request.name = name;
  request.created_unix_ms = request.progress_unix_ms = _wall.unix_ms();
  request.created_by = self;
  request.pinned_node_id = pin;
  request.settings_changed_unix_ms = request.desired_changed_unix_ms = request.created_unix_ms;
  request.settings_changed_by = request.desired_changed_by = self;
  if (paused) request.desired = TorrentDesired::paused;
  record(request);
  out.status = 202;
  out.code = "ok";
  out.request = request;
  return out;
}

TorrentOwner::Outcome TorrentOwner::act(std::string_view id, std::string_view action) {
  Outcome out;
  const auto current = request(id);
  if (!current) return {404, "not_found", std::nullopt, std::nullopt};
  const auto& r = *current;
  const Outcome invalid{409, "invalid_state", r, std::nullopt};
  const auto now = _wall.unix_ms();
  if (action == "clear") {
    if (!torrent_phase_terminal(r.phase) && r.claim) return invalid;
    auto updated = r;
    updated.removed_unix_ms = now;
    record(updated);
    return {202, "ok", std::nullopt, std::nullopt};
  }
  TorrentDesired desired;
  if (action == "pause") {
    if (torrent_phase_terminal(r.phase) || r.desired == TorrentDesired::cancelled) return invalid;
    desired = TorrentDesired::paused;
  } else if (action == "resume") {
    if (torrent_phase_terminal(r.phase) || r.desired != TorrentDesired::paused) return invalid;
    desired = TorrentDesired::active;
  } else if (action == "cancel") {
    if (r.phase == TorrentPhase::completed || r.phase == TorrentPhase::cancelled) return invalid;
    desired = TorrentDesired::cancelled;
  } else {
    return {404, "not_found", std::nullopt, std::nullopt};
  }
  auto updated = r;
  updated.desired = desired;
  updated.desired_changed_unix_ms = std::max(now, updated.desired_changed_unix_ms + 1);
  updated.desired_changed_by = _membership.self();
  // Nothing holds an unclaimed request, so cancelling it is the whole job.
  if (desired == TorrentDesired::cancelled && !updated.claim) {
    updated.phase = TorrentPhase::cancelled;
    updated.progress_unix_ms = updated.desired_changed_unix_ms;
  }
  record(updated);
  return {202, "ok", updated, std::nullopt};
}

TorrentOwner::Outcome TorrentOwner::patch_remove_after(std::string_view id, std::optional<uint64_t> remove_after) {
  const auto current = request(id);
  if (!current) return {404, "not_found", std::nullopt, std::nullopt};
  auto updated = *current;
  updated.remove_after_ms = remove_after;
  updated.settings_changed_unix_ms = std::max(_wall.unix_ms(), updated.settings_changed_unix_ms + 1);
  updated.settings_changed_by = _membership.self();
  record(updated);
  return {200, "ok", updated, std::nullopt};
}

std::optional<TorrentRequest> TorrentOwner::request(std::string_view id) const {
  const auto* found = _head.find(id);
  if (!found || found->removed_unix_ms) return std::nullopt;
  return *found;
}

std::vector<TorrentRequest> TorrentOwner::requests() const {
  std::vector<TorrentRequest> out;
  for (const auto& [_, request] : _head.records())
    if (!request.removed_unix_ms) out.push_back(request);
  return out;
}

bool TorrentOwner::pinned_elsewhere(const TorrentRequest& request) const {
  return request.pinned_node_id && *request.pinned_node_id != _membership.self() && known.contains(*request.pinned_node_id);
}

void TorrentOwner::pass() {
  const auto self = _membership.self();
  const auto now = _wall.unix_ms();
  std::vector<TorrentRequest> updates;

  for (const auto& [id, r] : TorrentRequestMap(_head.records())) {
    auto job = jobs.find(id);
    const bool mine = capable && r.claim && r.claim->holder == self;

    if (r.removed_unix_ms) {
      if (job != jobs.end()) jobs.erase(job);
      _held.erase(id);
      continue;
    }

    if (!mine) {
      // A lease this node acted under that no longer stands: stand down.
      if (const auto held = _held.find(id); held != _held.end()) {
        if (!_keeper.standing(held->second, r.claim).holds) {
          jobs.erase(id);
          _held.erase(held);
        }
      }
      if (!capable || r.desired != TorrentDesired::active || pinned_elsewhere(r)) continue;
      const auto rank = ranks.contains(id) ? ranks.at(id) : size_t{0};
      std::optional<lease::Decision> decision;
      if (r.phase == TorrentPhase::awaiting_node)
        decision = _keeper.vacant(id, r.claim, rank);
      else if (!torrent_phase_terminal(r.phase))
        decision = _keeper.decide(id, r.claim, rank);
      if (!decision || decision->kind != lease::Decision::Kind::take) continue;
      auto claimed = r;
      claimed.claim = decision->proposed;
      claimed.phase = TorrentPhase::downloading;
      claimed.phase_epoch = claimed.claim->epoch;
      claimed.progress_unix_ms = std::max(now, r.progress_unix_ms + 1);
      claimed.error_code.clear();
      claimed.error.clear();
      updates.push_back(std::move(claimed));
      continue;
    }

    const auto desired = r.desired;
    if (job == jobs.end()) {
      if (!torrent_phase_terminal(r.phase) && desired != TorrentDesired::cancelled) {
        job = jobs.emplace(id, Job{TorrentPhase::downloading, desired == TorrentDesired::paused}).first;
        _held[id] = *r.claim;
      } else if (desired == TorrentDesired::cancelled && !torrent_phase_terminal(r.phase)) {
        auto cancelled = r;
        cancelled.phase = TorrentPhase::cancelled;
        cancelled.phase_epoch = r.claim->epoch;
        cancelled.progress_unix_ms = now;
        updates.push_back(cancelled);
        continue;
      }
    }
    if (job == jobs.end()) continue;

    if (!torrent_phase_terminal(job->second.phase)) {
      if (desired == TorrentDesired::paused) job->second.paused = true;
      if (desired == TorrentDesired::active) job->second.paused = false;
      if (desired == TorrentDesired::cancelled) job->second.phase = TorrentPhase::cancelled;
    }

    auto updated = r;
    const auto phase = job->second.phase;
    if (phase != r.phase) {
      // Backwards (a retried or revived import) needs a new epoch: the join
      // never moves a phase back within one (requirement E8).
      if (static_cast<uint8_t>(phase) < static_cast<uint8_t>(r.phase) || (torrent_phase_terminal(r.phase) && !torrent_phase_terminal(phase))) {
        updated.claim = _keeper.next(r.claim);
        _held[id] = *updated.claim;
      }
      updated.phase = phase;
      updated.phase_epoch = updated.claim->epoch;
      if (phase == TorrentPhase::completed && !updated.completed_unix_ms) updated.completed_unix_ms = now;
    }
    if (r.phase == TorrentPhase::completed && r.remove_after_ms && now >= r.completed_unix_ms + *r.remove_after_ms) {
      jobs.erase(job);
      updated.removed_unix_ms = now;
    }
    if (updated != r) {
      updated.progress_unix_ms = std::max(now, r.progress_unix_ms + 1);
      updates.push_back(std::move(updated));
    }
  }

  // Tombstones past the window are erased by one node, the lowest active id.
  std::vector<std::string> expired;
  const auto active = _membership.active();
  if (std::none_of(active.begin(), active.end(), [&](const NodeId& node) { return node < self; })) expired = _head.expired(now);

  // Joined onto what is there now: an operator's newer intent, or another
  // node's newer claim, is kept.
  for (const auto& update : updates) record(update);
  for (const auto& id : expired) _head.erase(id);

  // Adopt what was just claimed, now that a re-read shows the claim stands.
  for (const auto& update : updates) {
    const auto* r = _head.find(update.id);
    if (!r || !r->claim || r->claim->holder != self || torrent_phase_terminal(r->phase) || r->removed_unix_ms) continue;
    if (jobs.contains(r->id)) continue;
    jobs.emplace(r->id, Job{TorrentPhase::downloading, r->desired == TorrentDesired::paused});
    _held[r->id] = *r->claim;
  }
}

}  // namespace dagda::test::macha
