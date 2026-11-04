//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// Macha's TorrentRequest (src/torrent/torrent_request.{hpp,cpp} at Macha
// develop 5a06d3c) as the first instantiation of the claimed-collection
// template: Macha's record, its field groups and their clocks, its codec, its
// duplicate rule and its 7-day tombstone, supplied as the owner supplies them.
// It is Macha's, so it lives in the tests; the library names no torrent.
//
// TorrentOwner is the part of Macha's TorrentCoordinator
// (src/torrent/torrent_coordinator.cpp) that is about records and leases,
// expressed through the lease template: adds and the operator's intent as
// writes to this node's copy of the collection, claims, takeover after
// absence, standing down, and the tombstone sweep. Its engine is a stand-in
// (a map of jobs); the HTTP API, the engine and the head are Macha's.

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

#include "dagda/lease/claimed_collection.h"
#include "dagda/lease/keeper.h"
#include "dagda/lease/lease.h"
#include "dagda/lease/membership.h"
#include "dannan/contract/time_source.h"
#include "dannan/net/codec.h"

namespace dagda::test::macha {

using NodeId = dannan::identity::NodeId;
using lease::Lease;

enum class TorrentPhase : uint8_t {
  awaiting_node = 0,
  downloading = 1,
  importing = 2,
  completed = 3,
  failed = 4,
  cancelled = 5,
};
bool torrent_phase_terminal(TorrentPhase phase) noexcept;

enum class TorrentDesired : uint8_t {
  active = 0,
  paused = 1,
  cancelled = 2,
};

// Macha's TorrentClaim {node_id, epoch, claimed_unix_ms} is the lease
// {holder, epoch, since}: the same members in the same order, so the
// record's order and bytes are Macha's.
using TorrentClaim = Lease;

struct TorrentRequest {
  std::string id;
  std::string info_hash;
  std::string source;
  uint64_t created_unix_ms{};
  NodeId created_by{};

  std::optional<NodeId> pinned_node_id;
  std::optional<uint64_t> remove_after_ms;
  uint64_t settings_changed_unix_ms{};
  NodeId settings_changed_by{};

  TorrentDesired desired{TorrentDesired::active};
  uint64_t desired_changed_unix_ms{};
  NodeId desired_changed_by{};

  std::optional<TorrentClaim> claim;

  TorrentPhase phase{TorrentPhase::awaiting_node};
  uint64_t phase_epoch{};
  uint64_t progress_unix_ms{};
  std::string name;
  uint64_t bytes_total{};
  std::string ingest_job_id;
  std::string error_code;
  std::string error;
  uint64_t completed_unix_ms{};

  uint64_t removed_unix_ms{};

  auto operator<=>(const TorrentRequest&) const = default;
};

inline constexpr uint32_t max_torrent_requests = 65536;
inline constexpr size_t max_torrent_request_text = 8192;

bool torrent_request_live(const TorrentRequest& request);

struct TorrentRequestTraits {
  using Record = TorrentRequest;
  using Key = std::string;
  using DuplicateKey = std::string;

  static const std::string& key_of(const TorrentRequest& request) { return request.id; }

  // Settings: last writer wins by (settings_changed_unix_ms, node). Intent:
  // the same, with cancelled first, so it dominates. Phase: the owner's
  // group, by (phase_epoch, phase, progress_unix_ms).
  static constexpr auto groups = std::make_tuple(
      lease::field_group([](const TorrentRequest& r) { return lease::written(r.settings_changed_unix_ms, r.settings_changed_by); },
                         [](TorrentRequest& out, const TorrentRequest& from) {
                           out.pinned_node_id = from.pinned_node_id;
                           out.remove_after_ms = from.remove_after_ms;
                           out.settings_changed_unix_ms = from.settings_changed_unix_ms;
                           out.settings_changed_by = from.settings_changed_by;
                         }),
      lease::field_group(
          [](const TorrentRequest& r) { return lease::dominating(r.desired == TorrentDesired::cancelled, r.desired_changed_unix_ms, r.desired_changed_by); },
          [](TorrentRequest& out, const TorrentRequest& from) {
            out.desired = from.desired;
            out.desired_changed_unix_ms = from.desired_changed_unix_ms;
            out.desired_changed_by = from.desired_changed_by;
          }),
      lease::field_group([](const TorrentRequest& r) { return std::make_tuple(r.phase_epoch, static_cast<uint8_t>(r.phase), r.progress_unix_ms); },
                         [](TorrentRequest& out, const TorrentRequest& from) {
                           out.phase = from.phase;
                           out.phase_epoch = from.phase_epoch;
                           out.progress_unix_ms = from.progress_unix_ms;
                           out.name = from.name;
                           out.bytes_total = from.bytes_total;
                           out.ingest_job_id = from.ingest_job_id;
                           out.error_code = from.error_code;
                           out.error = from.error;
                           out.completed_unix_ms = from.completed_unix_ms;
                         }));

  static const std::optional<Lease>& lease_of(const TorrentRequest& request) { return request.claim; }
  static void set_lease(TorrentRequest& request, std::optional<Lease> lease) { request.claim = lease; }
  static uint64_t removed_unix_ms(const TorrentRequest& request) { return request.removed_unix_ms; }
  static void set_removed_unix_ms(TorrentRequest& request, uint64_t ms) { request.removed_unix_ms = ms; }

  // Two live requests for one torrent: the earlier (created_unix_ms, id)
  // keeps it, the other is cancelled as duplicate_torrent.
  static std::optional<std::string> duplicate_key(const TorrentRequest& request);
  static bool keeps(const TorrentRequest& x, const TorrentRequest& y) { return std::tie(x.created_unix_ms, x.id) < std::tie(y.created_unix_ms, y.id); }
  static void lose(TorrentRequest& loser, const TorrentRequest& keeper);

  static void encode(dannan::net::Writer& writer, const TorrentRequest& request);
  static TorrentRequest decode(dannan::net::Reader& reader);
  static void encode_key(dannan::net::Writer& writer, const std::string& id) { writer.string(id); }
  static std::string decode_key(dannan::net::Reader& reader) { return reader.string(max_torrent_request_text); }

  static constexpr uint32_t max_records = max_torrent_requests;
  // TorrentCoordinator::tombstone_grace.
  static constexpr std::chrono::milliseconds retention = std::chrono::hours(24 * 7);
};

using TorrentRequests = lease::ClaimedCollection<TorrentRequestTraits>;
using TorrentRequestMap = TorrentRequests::Map;

// Macha's names, for the moved cases.
inline void encode_torrent_request(dannan::net::Writer& writer, const TorrentRequest& request) { TorrentRequestTraits::encode(writer, request); }
inline TorrentRequest decode_torrent_request(dannan::net::Reader& reader) { return TorrentRequestTraits::decode(reader); }
inline TorrentRequest merge_torrent_request(const TorrentRequest& a, const TorrentRequest& b) { return TorrentRequests::merge(a, b); }
inline TorrentRequestMap merge_torrent_requests(const TorrentRequestMap& base, const TorrentRequestMap& left, const TorrentRequestMap& right) {
  return TorrentRequests::merge(&base, left, right);
}

// One node's coordinator, over its copy of the collection (what its head
// holds). Not thread-safe: one pass at a time, as Macha's pass mutex keeps.
class TorrentOwner {
 public:
  struct Outcome {
    int status = 200;
    std::string code;
    std::optional<TorrentRequest> request;
    std::optional<NodeId> node;
  };
  // The engine's job here: its phase and whether it is paused.
  struct Job {
    TorrentPhase phase = TorrentPhase::downloading;
    bool paused = false;
  };

  TorrentOwner(TorrentRequests& head, lease::LeaseKeeper& keeper, const lease::Membership& membership, const dannan::contract::WallClock& wall)
      : _head(head), _keeper(keeper), _membership(membership), _wall(wall) {}

  // Whether this node runs torrents (Macha: a torrent service published).
  bool capable = false;
  // Nodes the roster still knows: a pin to one holds; to a forgotten one, no
  // longer.
  std::set<NodeId> known;
  // This node's rank among the capable nodes for a request (Macha:
  // rendezvous over the torrent nodes); 0 unless set.
  std::map<std::string, size_t, std::less<>> ranks;
  std::map<std::string, Job, std::less<>> jobs;

  Outcome add(const std::string& id, const std::string& info_hash, const std::string& name, bool paused = false, std::optional<NodeId> pin = std::nullopt);
  Outcome act(std::string_view id, std::string_view action);
  Outcome patch_remove_after(std::string_view id, std::optional<uint64_t> remove_after);
  // Live (not removed) requests.
  std::optional<TorrentRequest> request(std::string_view id) const;
  std::vector<TorrentRequest> requests() const;
  // One scheduler pass.
  void pass();

 private:
  bool pinned_elsewhere(const TorrentRequest& request) const;
  void record(const TorrentRequest& request) { _head.join(request); }

  TorrentRequests& _head;
  lease::LeaseKeeper& _keeper;
  const lease::Membership& _membership;
  const dannan::contract::WallClock& _wall;
  // The leases this node adopted a job under: what it stands down from.
  std::map<std::string, Lease, std::less<>> _held;
};

}  // namespace dagda::test::macha
