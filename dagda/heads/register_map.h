//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/heads/codec.h"
#include "dagda/heads/space.h"
#include "dagda/heads/tree.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/walk.h"

// The register map (D6, TODO/2026-11-20-register-map.md): Dagda's own owner of a head
// space, so a hosted system opens one rather than writing its codec, merge and mutation.
// A key maps to a register, a value or a tombstone, ordered by (stamp, node id); the merge
// of two heads is the greater register per key and nothing else.
namespace dagda::heads {

// The stamp seam. Lamport stands in for the cases until Dannan's HlcClock::admit is
// released; no space is opened on it in a release.
enum class StampKind : uint8_t { lamport = 1, hlc = 2 };

// The three-systems design's window: Dannan's forget_after, the exchange's garbage_grace.
inline constexpr std::chrono::milliseconds register_tombstone_window{std::chrono::hours(24 * 30)};

struct TombstonePolicy {
  enum class Kind : uint8_t { window = 1, never = 2 };
  Kind kind = Kind::window;
  std::chrono::milliseconds window = register_tombstone_window;
  static TombstonePolicy within(std::chrono::milliseconds length) { return {Kind::window, length}; }
  static TombstonePolicy never() { return {Kind::never, std::chrono::milliseconds{0}}; }
  bool operator==(const TombstonePolicy&) const = default;
};

// A register's place in the order, and what a conditional write names.
struct RegisterVersion {
  uint64_t stamp = 0;
  NodeId node{};
  auto operator<=>(const RegisterVersion&) const = default;
};

struct Register {
  Bytes value;
  // Objects the register names, claimed by the commit that writes it.
  std::vector<ObjectId> refs;
  bool tombstone = false;
  uint64_t stamp = 0;
  NodeId node{};
  // Wall nanoseconds; past it the key reads as absent.
  std::optional<int64_t> expires_ns;
  // The wall time of the commit that wrote it: the Lamport window's clock.
  int64_t written_ns = 0;
  RegisterVersion version() const noexcept { return {stamp, node}; }
  bool operator==(const Register&) const = default;
};

// Whether `a` is ordered before `b`. With `tombstone_wins` a tombstone is above any value
// of its key, and (stamp, node id) orders only value against value and tombstone against
// tombstone. Equal order is neither.
bool register_before(const Register& a, const Register& b, bool tombstone_wins) noexcept;

// What a space records of its options in every head, so two nodes cannot disagree.
struct RegisterForm {
  StampKind stamp = StampKind::lamport;
  TombstonePolicy tombstones;
  bool tombstone_wins = false;
  bool operator==(const RegisterForm&) const = default;
};

struct RegisterOptions {
  RegisterForm form;
  // The founding entries' values, written with stamp zero and no node.
  std::map<std::string, Bytes> founding;
  Policy policy;
};

// The head as the codec decodes it.
struct RegisterHead {
  // Zero only on genesis, which records no form.
  uint32_t floor = 1;
  HeadClock clock;
  std::vector<Hash256> merge_parents;
  std::optional<ObjectId> root;
  RegisterForm form;
  // The highest stamp any register in the map has carried: Lamport's next is one more.
  uint64_t lamport = 0;
  bool operator==(const RegisterHead&) const = default;
};

// What a conditional write names it read: nothing, a version, or that the key was absent.
struct RegisterExpect {
  enum class Kind : uint8_t { any, version, absent };
  Kind kind = Kind::any;
  RegisterVersion version{};
  static RegisterExpect any() { return {}; }
  static RegisterExpect at(RegisterVersion read) { return {Kind::version, read}; }
  static RegisterExpect absent() { return {Kind::absent, {}}; }
};

struct RegisterOp {
  enum class Kind : uint8_t { put, remove };
  Kind kind = Kind::put;
  std::string key;
  Bytes value;
  std::vector<ObjectId> refs;
  std::optional<int64_t> expires_ns;
  RegisterExpect expect;

  // Throws std::invalid_argument past the bounds: the put is refused before any commit.
  static RegisterOp put(std::string name, Bytes bytes, std::vector<ObjectId> objects = {}, std::optional<int64_t> expiry = {}, RegisterExpect expected = {});
  static RegisterOp remove(std::string name, RegisterExpect expected = {});
};

// A conditional write refused on this node's view: nothing was committed.
class RegisterConflict final : public std::runtime_error {
 public:
  RegisterConflict(std::string key, std::optional<Register> found, const std::string& why);
  const std::string& key() const noexcept { return _key; }
  // The parent's register, tombstone included; none when absent or expired.
  const std::optional<Register>& found() const noexcept { return _found; }

 private:
  std::string _key;
  std::optional<Register> _found;
};

// A head recording other options than this node opened the space with, or a stamp kind
// this build cannot issue.
class RegisterRefused final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// What each key a commit wrote now holds, for the next conditional write. Filled on
// every attempt; the one that committed is what is left.
struct RegisterWritten {
  std::map<std::string, RegisterVersion> versions;
};

struct RegisterCommit {
  CommitOutcome outcome;
  std::map<std::string, RegisterVersion> written;
};

// One key a change touched, both sides as held: a tombstone is a register here.
struct RegisterChange {
  std::string key;
  std::optional<Register> before;
  std::optional<Register> after;
  bool operator==(const RegisterChange&) const = default;
};

class RegisterTree;

// The owner. Immutable once made, so thread-safe; every read takes the view it reads and
// the node store the space reads through (ControlTreeNodeStore::for_reading on a node), as
// Space does not lend its own. A read whose tree node cannot be had throws DecodeError,
// never reads as absent. Reads wait as Space::entries_waits; a commit as mutate_waits.
class RegisterMap {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::thread_safe;
  static constexpr size_t value_max_bytes = 64 * 1024;
  static constexpr size_t refs_max = 4096;

  // Throws RegisterRefused for StampKind::hlc: HlcClock::admit is not in Dannan 0.10.0, and
  // a peer's stamp must pass it at acceptance before any is applied. `wall` judges expiry
  // (SpaceServices::wall on a node), `time` a read's budget deadline; both outlive this.
  explicit RegisterMap(RegisterOptions options, const dannan::contract::WallClock& wall = dannan::contract::system_wall_clock(),
                       const dannan::contract::TimeSource& time = dannan::contract::steady_time_source());
  ~RegisterMap();
  RegisterMap(const RegisterMap&) = delete;
  RegisterMap& operator=(const RegisterMap&) = delete;

  // The codec, the merge, the tree and the founding, under `name`.
  SpaceSpec spec(std::string name) const;
  static SpaceSpec spec(std::string name, RegisterOptions options);

  const RegisterOptions& options() const noexcept { return _options; }
  const Tree<Register>& tree() const noexcept;
  const std::shared_ptr<const Codec>& codec() const noexcept { return _codec; }
  const std::shared_ptr<const Merge>& merge() const noexcept { return _merge; }

  // The ops in one commit, all or nothing. A conditional op whose parent register differs
  // throws RegisterConflict from the edit; a head with other options throws RegisterRefused.
  // Throws std::invalid_argument at once for an op past the bounds or with no key.
  MutationFactory mutation(std::vector<RegisterOp> ops, std::shared_ptr<RegisterWritten> written = nullptr) const;
  // The same through Space::mutate_delta, with each key's new version.
  RegisterCommit commit(Space& space, std::vector<RegisterOp> ops, std::optional<ExternalIdentity> identity = {}) const;

  // Nothing, or why this node may not read or write `view` with these options.
  std::optional<std::string> refusal(const SpaceView& view) const;
  static const RegisterHead& head_of(const SpaceView& view);
  static const RegisterHead& head_of(const State& state);

  // Absent for a key never written, a tombstone or an expired register.
  std::optional<Register> get(const SpaceView& view, std::string_view key, const TreeNodeStore& nodes) const;
  // The register with its tombstone flag; absent for none or an expired one.
  std::optional<Register> get_register(const SpaceView& view, std::string_view key, const TreeNodeStore& nodes) const;
  // Live registers under `prefix` after the cursor, one budget operation per key visited,
  // tombstones and expired ones passed over but paid for.
  dannan::sched::Page<std::pair<std::string, Register>, std::string> list(const SpaceView& view, std::string_view prefix,
                                                                          dannan::sched::Cursor<std::string> cursor, dannan::sched::Budget& budget,
                                                                          const TreeNodeStore& nodes) const;
  // The keys under `prefix` whose registers differ between two views, over the tree's diff,
  // so it costs the change; each page reads that difference again.
  dannan::sched::Page<RegisterChange, std::string> changed(const SpaceView& before, const SpaceView& after, std::string_view prefix,
                                                           dannan::sched::Cursor<std::string> cursor, dannan::sched::Budget& budget,
                                                           const TreeNodeStore& nodes) const;
  // Every object a register in `view` names: what the space's reachability answers, so the
  // collector frees an object once no register names it.
  std::vector<ObjectId> named_objects(const SpaceView& view, const TreeNodeStore& nodes, const TreePause& pause = {}) const;

  // The expiry clock, in wall nanoseconds: what an owner computes time left against.
  int64_t now() const noexcept;
  bool expired(const Register& value) const noexcept { return expired_at(value, now()); }
  static bool expired_at(const Register& value, int64_t now_ns) noexcept { return value.expires_ns && *value.expires_ns <= now_ns; }

 private:
  std::optional<Register> held(const SpaceView& view, std::string_view key, const TreeNodeStore& nodes) const;
  void check(const SpaceView& view) const;

  const RegisterOptions _options;
  const dannan::contract::WallClock& _wall;
  const dannan::contract::TimeSource& _time;
  std::shared_ptr<const RegisterTree> _tree;
  std::shared_ptr<const Codec> _codec;
  std::shared_ptr<const Merge> _merge;
};

}  // namespace dagda::heads
