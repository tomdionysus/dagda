//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "dagda/lease/lease.h"
#include "dagda/lease/membership.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

// The lease logic (plan, "Store: leases and claimed collections";
// requirement E7): a lease holds while its holder is a member; after the
// holder has been absent for the absence window another member takes it at
// epoch + 1; a holder that returns and finds a higher epoch stands down.
// Takers ranked by the owner wait rank times a step, so the preferred one
// goes first. Macha's coordinator does this over the claims in its claimed
// collection; here it is any owner's.
namespace dagda::lease {

struct LeaseOptions {
  // How long a holder may be absent before another member takes the lease
  // (Macha's claim lease: ten minutes, so a restart or a brief drop does not
  // move the work).
  std::chrono::milliseconds absence{std::chrono::minutes(10)};
  // How long a taker the owner ranks r waits, r times this, from when it
  // first found the lease takeable (Macha: 30 s).
  std::chrono::milliseconds rank_step{std::chrono::seconds(30)};
};

// What this node may do about a lease now.
struct Decision {
  enum class Kind : uint8_t {
    held_here,       // this node holds it
    held_elsewhere,  // a member holds it, or one absent for less than the window
    waiting,         // takeable, but this node's rank has not come
    take,            // propose `proposed`
  };
  Kind kind = Kind::held_elsewhere;
  // With take: {self, the record's epoch + 1 (or 1), now on the wall clock}.
  std::optional<Lease> proposed;
  std::string why;
};

// A holder's check before it acts: does its lease still stand against the
// record as this node holds it?
struct Standing {
  bool holds = false;
  std::optional<Lease> current;
  std::string why;
};

// What acquire() did, as evidence.
struct Acquisition {
  enum class Outcome : uint8_t {
    held,            // this node already held it; nothing written
    taken,           // written, and the record as it stands names this node
    held_elsewhere,  // a member holds it; nothing written
    waiting,         // takeable, this node's rank has not come; nothing written
    lost,            // written, but another's claim stands
    failed,          // the write did not happen (why)
  };
  Outcome outcome = Outcome::failed;
  // The record as this node holds it afterwards: {holder, epoch, since}.
  // Empty when there is none and nothing was written.
  std::optional<Lease> lease;
  std::string why;
};

const char* outcome_name(Acquisition::Outcome outcome) noexcept;

// Where lease records live: THE STEP-4 SEAM. A lease record is a key in a
// head space the owner owns (`scope` names it), merged by lease::merge (as a
// LeaseTable, lease_table.h, is). Step 4's head spaces implement this; until
// then a test implements it in memory.
class LeaseRecords {
 public:
  struct Joined {
    // The record as it stands after the join, re-read: what the caller
    // decides by.
    std::optional<Lease> standing;
    // Empty when the join was written; else why it was not.
    std::string failure;
  };
  using JoinDone = std::function<void(Joined)>;

  virtual ~LeaseRecords() = default;
  // The record as this node holds it now; empty for none. Waits on nothing;
  // thread-safe.
  virtual std::optional<Lease> read(const std::string& scope, const std::string& name) const = 0;
  // Joins `proposed` into the record, written as the space writes, and
  // completes on `executor` (never inline) by `deadline`. Thread-safe.
  virtual void join(const std::string& scope, const std::string& name, const Lease& proposed, std::chrono::milliseconds deadline,
                    boost::asio::any_io_executor executor, JoinDone done) = 0;
};

// One per owner and node. Absence comes from `membership` (Dannan's roster
// in a node), steady time from `time`, claim times from `wall`; each must
// outlive this. Thread-safe; decide(), vacant() and standing() wait on
// nothing.
class LeaseKeeper {
 public:
  using Done = std::function<void(Acquisition)>;

  LeaseKeeper(const Membership& membership, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall, LeaseOptions options = {});

  // Whether this node may take the lease `name` whose record is `current`:
  // a lease with no record is takeable; a lease held by an absent member is
  // takeable once the absence window has passed. `rank` is the owner's
  // ranking of this node among the takers (0 first).
  Decision decide(const std::string& name, const std::optional<Lease>& current, size_t rank = 0);
  // As decide(), when the owner itself says nobody is doing the work (Macha's
  // request awaiting a node): takeable whoever the record names.
  Decision vacant(const std::string& name, const std::optional<Lease>& current, size_t rank = 0);
  // `mine` stands while the record names this node at its epoch or a later
  // one of its own; otherwise the holder stands down.
  Standing standing(const Lease& mine, const std::optional<Lease>& current) const;
  // This node's claim over `current`: {self, epoch + 1 (or 1), now}. Also a
  // holder's own re-claim (requirement E8).
  Lease next(const std::optional<Lease>& current) const;

  // acquire(name, scope) -> {holder, epoch, since}: decides on the record
  // `records` holds, and when this node may take it, joins its claim there
  // and decides by the record as it then stands. Completes on `executor`,
  // never inline, by `deadline`.
  void acquire(LeaseRecords& records, const std::string& scope, const std::string& name, std::chrono::milliseconds deadline,
               boost::asio::any_io_executor executor, Done done, size_t rank = 0);

  const LeaseOptions& options() const noexcept { return _options; }

 private:
  Decision takeable(const std::string& name, const std::optional<Lease>& current, size_t rank, std::string why);
  void forget(const std::string& name);

  const Membership& _membership;
  const dannan::contract::TimeSource& _time;
  const dannan::contract::WallClock& _wall;
  const LeaseOptions _options;
  const NodeId _self;

  mutable dannan::contract::Mutex _mutex;
  // When each lease first became takeable here, for rank waits.
  std::map<std::string, dannan::contract::TimePoint, std::less<>> _takeable_since DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dagda::lease
