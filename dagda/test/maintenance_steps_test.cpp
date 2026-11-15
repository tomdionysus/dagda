//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The steps of Repair and Collector driven in Macha's order and to Macha's
// decision trace (Tom: Macha's loop stays until stage 8c and calls Dagda's
// steps, and its test_maintenance_trace fixtures stay unchanged).
//
// The loop here is neutral and is the test's: Macha's src/service/maintenance.cpp
// at 9a08846 (Maintenance::run), its gates, deadlines and catches, with its
// metadata and catalogue stages left out, over real Dagda nodes on an in-process
// cluster. The fixtures are data/maintenance-traces, byte for byte Macha's at
// 9a08846; TraceLog and the scripts are its tests/test_maintenance_trace.cpp.
//
// What a fixture proves here and what it cannot: the steps' order, the gates, the
// claims, the claim walk, the pull, the tombstones and the horizons are Dagda's
// own, on real stores. What Macha's filesystem and catalogue answered (generation
// numbers, what is live, what is retired) is Owner, a script with those answers:
// a file written moves the generation by two, an unlink by one, an erasure by one.
// Where Macha's fixture isolates one node, the traced node here hosts extents
// alone, with a peer beside it that hosts none. The peer fixture has two hosts.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/ledger/collector.h"
#include "dagda/store/messages.h"
#include "dagda/store/repair.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda {
namespace {

using namespace std::chrono_literals;
using contract::Garbage;
using contract::GarbageBatch;
using contract::Hash256;
using contract::MaintenanceStopping;
using contract::Reachability;
using contract::RetentionClass;
using dannan::contract::TimePoint;
using store::Bytes;
using store::ObjectId;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::pattern;
using test::wait_until;

// ---- Macha's trace, as its test keeps it -------------------------------------

// Ids differ on every run, so each distinct object (64 hex) or node (32 hex) id
// becomes #1, #2, ... in order of first appearance.
std::vector<std::string> normalise(const std::vector<std::string>& lines) {
  static const std::regex id("[0-9a-f]{64}|[0-9a-f]{32}");
  std::map<std::string, std::string> names;
  std::vector<std::string> out;
  for (const auto& line : lines) {
    std::string result;
    auto rest = line.cbegin();
    for (std::sregex_iterator it(line.begin(), line.end(), id), end; it != end; ++it) {
      result.append(rest, line.cbegin() + it->position());
      const auto [named, inserted] = names.try_emplace(it->str(), "#" + std::to_string(names.size() + 1));
      (void)inserted;
      result += named->second;
      rest = line.cbegin() + it->position() + it->length();
    }
    result.append(rest, line.cend());
    out.push_back(std::move(result));
  }
  return out;
}

// A gate's node-state inputs, without the scheduling ones (open/shut, due,
// rebuilt, share, quiescent), which depend on how many passes ran.
std::string node_conditions(std::string_view detail) {
  std::istringstream words{std::string(detail)};
  std::string kept;
  for (std::string word; words >> word;) {
    if (word == "open" || word == "shut" || word.starts_with("due=") || word.starts_with("rebuilt=") || word.starts_with("share=") ||
        word.starts_with("quiescent="))
      continue;
    kept += (kept.empty() ? "" : " ") + word;
  }
  return kept;
}

// Per step: the outcome, the actions since the previous step grouped by kind,
// then the settled state: each gate's verdict and the horizons last built.
class TraceLog {
 public:
  void skip_verdicts() { _compare_verdicts = false; }
  void ignore(std::string kind) { _ignored.insert(std::move(kind)); }
  void add(std::string_view kind, std::string_view detail) {
    const std::scoped_lock lock(_mutex);
    if (_ignored.contains(std::string(kind))) return;
    if (kind == "gate.repair") return;
    if (kind.starts_with("gate.")) {
      const bool decided = detail.find("due=1") != std::string_view::npos && detail.find("rebuilt=1") == std::string_view::npos;
      auto& verdict = _verdicts[std::string(kind)];
      if (decided) {
        verdict = std::string(detail.substr(0, 4)) + " when " + node_conditions(detail);
      } else if (verdict.empty()) {
        verdict = "not yet decided";
      }
      _gates[std::string(kind)] = _compare_verdicts ? node_conditions(detail) + "; decided " + verdict : node_conditions(detail);
      return;
    }
    if (kind == "claim-walk" && detail.ends_with("waiting-for-credit")) return;
    if (kind == "inventory" || kind == "release-horizon") {
      _gates[std::string(kind)] = std::string(detail);
    } else {
      _actions.push_back(std::string(kind) + ": " + std::string(detail));
    }
  }
  void step(std::string_view name, const std::vector<std::string>& state) {
    const std::scoped_lock lock(_mutex);
    _lines.push_back("step: " + std::string(name));
    for (const auto& line : state) _lines.push_back("  " + line);
    std::stable_sort(_actions.begin(), _actions.end(), [](const auto& a, const auto& b) { return a.substr(0, a.find(':')) < b.substr(0, b.find(':')); });
    std::set<std::string> seen;
    std::erase_if(_actions, [&](const std::string& action) { return !seen.insert(action).second; });
    for (auto& action : _actions) _lines.push_back("  " + std::move(action));
    _actions.clear();
    for (const auto& [gate, verdict] : _gates) _lines.push_back("  " + gate + " = " + verdict);
  }
  std::vector<std::string> lines() {
    const std::scoped_lock lock(_mutex);
    return _lines;
  }
  std::vector<std::string> actions() {
    const std::scoped_lock lock(_mutex);
    return _actions;
  }

 private:
  std::mutex _mutex;
  std::set<std::string> _ignored;
  std::vector<std::string> _lines;
  std::vector<std::string> _actions;
  std::map<std::string, std::string> _gates;
  std::map<std::string, std::string> _verdicts;
  bool _compare_verdicts = true;
};

void check_against_fixture(std::string_view name, const std::vector<std::string>& trace) {
  std::ifstream in(std::filesystem::path(DAGDA_SOURCE_DIR) / "dagda/test/data/maintenance-traces" / (std::string(name) + ".txt"));
  ASSERT_TRUE(in.good()) << name;
  std::vector<std::string> expected;
  for (std::string line; std::getline(in, line);) expected.push_back(line);
  for (size_t line = 0; line < std::min(trace.size(), expected.size()); ++line) {
    ASSERT_EQ(trace[line], expected[line]) << "line " << line + 1 << " of " << name;
  }
  EXPECT_EQ(trace.size(), expected.size());
}

// ---- What Macha's filesystem and catalogue answered ---------------------------

Hash256 head_of(uint64_t generation) {
  Hash256 out{};
  for (size_t i = 0; i < 8; ++i) out.bytes[i] = static_cast<uint8_t>(generation >> (8 * i));
  return out;
}

class Owner final : public contract::ReachabilityProvider {
 public:
  Owner(ClusterNode& node, dannan::contract::TimeSource& time, dannan::contract::WallClock& wall)
      : _node(node), _time(time), _wall(wall), _metadata(node.context().events().kind("metadata")) {
    _control_refs.push_back(object_of(0xc0));
    _epoch_since = _time.now();
  }

  // A file: its object in the pool, claimed by a commit this head has seen.
  ObjectId write(uint8_t salt) {
    const auto bytes = pattern(64 * 1024, salt);
    const auto id = store::object_id(bytes);
    EXPECT_TRUE(_node.data().put(id, bytes));
    _node.claims().retain(RetentionClass::data, id, next_dot());
    _live.push_back(id);
    commit(2);
    return id;
  }
  // The same, on a peer too, as a write of two copies is.
  ObjectId write_on_both(uint8_t salt, ClusterNode& peer) {
    const auto id = write(salt);
    const auto bytes = pattern(64 * 1024, salt);
    EXPECT_TRUE(peer.data().put(id, bytes));
    peer.claims().retain(RetentionClass::data, id, {_node.id(), _dot});
    return id;
  }
  // The object is no longer referred to; its retirement is a claimed CONTROL
  // batch the head names.
  void unlink(const ObjectId& id) {
    std::erase(_live, id);
    const auto retired_at = static_cast<int64_t>(_wall.unix_ms()) * 1'000'000;
    auto bytes = pattern(512, static_cast<uint8_t>(++_batches_made));
    const auto batch = store::object_id(bytes);
    EXPECT_TRUE(_node.control().put(batch, bytes));
    _node.claims().retain(RetentionClass::control, batch, next_dot());
    _garbage.push_back(Garbage{id, retired_at, {}});
    _batches.push_back(GarbageBatch{batch, retired_at, 1});
    _batch_of[id] = batch;
    commit(1);
  }
  // A state referring to `count` more objects nobody holds, enough for the
  // survey's join to ask its pause.
  void refer_to_many(size_t count) {
    for (size_t i = 0; i < count; ++i) {
      ObjectId id{};
      for (size_t b = 0; b < 8; ++b) id.bytes[b] = static_cast<uint8_t>(i >> (8 * b));
      id.bytes[31] = 0xee;
      _live.push_back(id);
    }
    commit(1);
  }
  // The catalogue: an item whose root and shard are CONTROL objects it claims.
  ObjectId add_catalogue() {
    ObjectId root{};
    for (const uint8_t salt : {uint8_t{0xa1}, uint8_t{0xa2}}) {
      const auto bytes = pattern(1024, salt);
      const auto id = store::object_id(bytes);
      EXPECT_TRUE(_node.control().put(id, bytes));
      _node.claims().retain(RetentionClass::control, id, next_dot());
      _control_refs.push_back(id);
      if (salt == 0xa1) root = id;
    }
    commit(1);
    return root;
  }
  // The catalogue's root is gone: from here no read of the state is whole. The
  // inventory still names what the tree does, the release less than that.
  void lose_catalogue(const ObjectId& root) {
    _complete = false;
    std::erase(_control_refs, root);
    _release_control = {_control_refs.front()};
  }

  uint64_t current() const { return _generation; }
  bool throw_on_inventory = false;
  bool throw_on_collect = false;
  bool throw_on_release = false;
  std::function<void(const std::function<void()>&)> walk;

  uint64_t generation() override { return _generation; }
  uint64_t known_generation() override { return _generation; }
  Reachability inventory(const std::function<void()>& pause) override {
    if (throw_on_inventory) throw std::runtime_error("a node of the state is unreadable");
    if (pause) pause();
    if (walk) walk(pause);
    Reachability out;
    out.data = _live;
    out.control = _control_refs;
    out.garbage = _garbage;
    out.batches = _batches;
    out.complete = _complete;
    out.generation = _generation;
    return out;
  }
  std::optional<Hash256> release_head() override { return head_of(_generation); }
  Reachability release(const Hash256& head) override {
    if (throw_on_release) throw std::runtime_error("head unreadable");
    Reachability out;
    out.data = _live;
    out.control = _release_control.empty() ? _control_refs : _release_control;
    out.batches = _batches;
    out.complete = _complete;
    out.head = head;
    out.clock[_node.id()] = _dot;
    return out;
  }
  std::optional<contract::ControlEpoch> control_epoch() override { return contract::ControlEpoch{_generation, _epoch_since}; }
  bool within_control_epoch(uint64_t sequence, const std::function<void()>& removal) override {
    if (sequence != _generation) return false;
    removal();
    return true;
  }
  void collect_garbage(const contract::GarbageCollection& collection) override {
    if (throw_on_collect) throw std::runtime_error("the commit could not be made");
    bool changed = false;
    for (const auto& erased : collection.erase) {
      std::erase_if(_garbage, [&](const Garbage& retirement) { return retirement.id == erased.id; });
      if (const auto batch = _batch_of.find(erased.id); batch != _batch_of.end()) {
        std::erase_if(_batches, [&](const GarbageBatch& held) { return held.id == batch->second; });
        _batch_of.erase(batch);
      }
      changed = true;
    }
    if (changed) commit(1);
  }

 private:
  static ObjectId object_of(uint8_t fill) {
    ObjectId out{};
    out.bytes.fill(fill);
    return out;
  }
  contract::RetentionDot next_dot() { return {_node.id(), ++_dot}; }
  void commit(uint64_t generations) {
    _generation += generations;
    _epoch_since = _time.now();
    _node.context().events().notify(_metadata);
  }

  ClusterNode& _node;
  dannan::contract::TimeSource& _time;
  dannan::contract::WallClock& _wall;
  const dannan::status::NodeEvents::Kind _metadata;
  uint64_t _generation = 2;
  uint64_t _dot = 0;
  int _batches_made = 0;
  bool _complete = true;
  std::vector<ObjectId> _live;
  std::vector<ObjectId> _control_refs;
  std::vector<ObjectId> _release_control;
  std::vector<Garbage> _garbage;
  std::vector<GarbageBatch> _batches;
  std::map<ObjectId, ObjectId> _batch_of;
  TimePoint _epoch_since{};
};

// ---- A node, its collector, and Macha's order over its steps -------------------

// Macha's figures for a trace: maintenance.cpp's policy as apply_trace_settings
// sets it.
constexpr auto foreground_quiet = 50ms;
constexpr auto garbage_grace = std::chrono::milliseconds(1h);
constexpr auto no_progress_backoff = std::chrono::milliseconds(5min);
constexpr auto interval = 1000ms;

const char* const order_inventory = "inventory";
const char* const order_survey = "survey";

// Everything a pass does, in the order Macha's does it.
const std::vector<std::string> macha_order{"credit", "gate.repair", order_inventory, order_survey, "walks", "transfer",
                                           "settle", "tombstones",  "release",       "control",    "data",  "local"};

std::string flag(std::string_view name, bool value) { return std::string(name) + (value ? "=1" : "=0"); }

// The traced node's bed: the node's own collector, repair and survey on stepped
// time, as Macha's pass runs on its manual clock while reachability is on real time.
struct Traced {
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall{dannan::contract::system_wall_clock().unix_ms()};
  TraceLog log;
  // What a test hangs on a trace line: a stop, or a failure.
  std::function<void(std::string_view kind)> on_trace;

  void trace(std::string_view kind, std::string_view detail) {
    log.add(kind, detail);
    if (on_trace) on_trace(kind);
  }
  ClusterNodeOptions options(size_t replication, size_t write_copies) {
    ClusterNodeOptions out;
    out.store.replication = replication;
    out.store.write_copies = write_copies;
    // Macha's test nodes' extent (tests/test_support.hpp).
    out.store.extent_size = 1024 * 1024;
    out.repair.extent_size = out.store.extent_size;
    out.garbage_grace = garbage_grace;
    out.maintenance.collector.retry_window = no_progress_backoff;
    out.maintenance.time = &time;
    out.maintenance.wall = &wall;
    out.maintenance.trace = [this](std::string_view kind, std::string_view detail) { trace(kind, detail); };
    return out;
  }
};

// Macha's Maintenance::run over the steps: one call of pass() is one turn of its
// loop, up to its wait.
class MachaLoop {
 public:
  MachaLoop(ClusterNode& node, Traced& traced, Owner& owner) : _node(node), _traced(traced), _owner(owner), _last_generation(owner.current()) {
    _observed_event = events();
    _node.dagda_node().register_reachability(domain::domain_v1, _owner);
  }
  ~MachaLoop() { _node.dagda_node().unregister_reachability(domain::domain_v1); }

  // The node's own.
  ledger::Collector& collector() { return *_node.dagda_node().collector(); }
  std::stop_source stop;
  // In place of the stop's pause for the survey, for a failure.
  std::function<void()> survey_pause;
  // Told the name of each step as it is about to run.
  std::function<void(const std::string&)> before_step;
  std::vector<std::string> sequence;
  // How the last pass ended: a stop thrown out of a step, a failure thrown out
  // of a step (Macha's catch logs it), or neither.
  bool stopped_by_throw = false;
  std::string failure;
  std::string survey_failure;
  // Macha's metadata_dirty and metadata_ready_for_dependants.
  bool metadata_dirty = false;
  bool metadata_ready = true;
  bool busy_for_test = false;
  size_t passes = 0;
  store::RepairSettled settled = store::RepairSettled::more_work;
  std::optional<ledger::InventoryStep> last_inventory;
  std::optional<ledger::TombstoneStep> last_tombstones;
  std::optional<ledger::ControlStep> last_control;
  std::optional<ledger::DataStep> last_data;

  // While not asked to stop: Macha's while (!stop.stop_requested()).
  void run_until_stopped(size_t bound) {
    for (size_t i = 0; i < bound && !stop.stop_requested(); ++i) pass();
  }

  // A pass whenever Macha's would wake: an event, or a deadline this clock has
  // reached.
  void settle() {
    for (int i = 0; i < 256; ++i) {
      if (events() == _observed_event && _traced.time.now() < _deadline) return;
      pass();
    }
    ADD_FAILURE() << "the loop never parked";
  }

  // Macha's TracedNode::advance: past the quiet window, then `by`, then the
  // follow-ups a rebuild and a repair that spanned a change are owed.
  void advance(std::chrono::milliseconds by) {
    const std::chrono::milliseconds quiet = foreground_quiet * 2;
    const auto follow_up = std::max(quiet, interval);
    settle();
    move(quiet);
    settle();
    move(by);
    settle();
    move(follow_up);
    settle();
    move(follow_up);
    settle();
  }
  void move(std::chrono::milliseconds by) {
    _traced.time.advance(by);
    _traced.wall.advance(by);
  }

  void pass() {
    ++passes;
    sequence.clear();
    stopped_by_throw = false;
    failure.clear();
    survey_failure.clear();
    auto& repair = _node.repair();
    const auto now = _traced.time.now();
    const auto current_event = events();
    const bool event_changed = current_event != _observed_event;
    _observed_event = current_event;
    if (_owner.current() != _last_generation) {
      _last_generation = _owner.current();
      _gc_quiet_until = now + foreground_quiet;
    }
    if (event_changed) {
      _network_quiet_until = TimePoint{};
      _gc_quiet_until = now + foreground_quiet;
    }
    enter("credit");
    _earned = repair.earn_credit(now, interval);
    const auto paced = repair.paced_by();
    const bool busy = busy_for_test || (paced & (store::paced_by_foreground | store::paced_by_loader)) != 0;

    bool gc_due_this_pass = false;
    bool repair_continue = false;
    try {
      enter("gate.repair");
      const bool allow = repair.may_start(now, paced);
      const bool network_due = allow && now >= _network_quiet_until;
      repair.note_gate(network_due ? store::RepairGate::ran : !allow ? store::RepairGate::share : store::RepairGate::quiescent, paced);
      _traced.trace("gate.repair", std::string(network_due ? "open " : "shut ") + flag("share", allow) + " " + flag("quiescent", now < _network_quiet_until));
      const bool garbage_due = !busy && !metadata_dirty;
      const bool gc_due = !busy && now >= _gc_quiet_until;
      gc_due_this_pass = gc_due;

      if (network_due || garbage_due || gc_due) {
        enter(order_inventory);
        const bool repair_only = network_due && !garbage_due && !gc_due;
        const auto inventory = collector().inventory_step(pause(), !repair_only || metadata_ready);
        last_inventory = inventory;
        if (inventory.rebuilt) _gc_quiet_until = _traced.time.now() + foreground_quiet;
        survey();
        if (network_due) {
          auto turn = repair.begin_turn(_traced.time.now(), paced, store::PeerReading::as_begun);
          enter("walks");
          const auto walks = repair.claim_walks(turn);
          enter("transfer");
          const auto step = repair.transfer(turn, repair.lists());
          enter("settle");
          settled = repair.settle(walks, step);
          if (settled == store::RepairSettled::credit) repair.note_gate(store::RepairGate::credit, paced);
          // Yielded, work is left. The higher class may have arrived after the pass
          // began, so neither the busy wake nor the share's next turn may be set:
          // look again after an interval, when the share decides (Macha 0.93.6).
          if (settled == store::RepairSettled::yielded || settled == store::RepairSettled::more_work) repair_continue = true;
          if (settled == store::RepairSettled::quiescent) _network_quiet_until = TimePoint::max();
        }

        ledger::PassDue due;
        due.garbage_due = garbage_due;
        due.gc_due = gc_due;
        due.busy = busy;
        due.gc_waiting_for_event = _gc_quiet_until == TimePoint::max();
        enter("tombstones");
        last_tombstones = collector().tombstone_step(due, inventory);
        enter("release");
        const auto release = collector().release_step();
        enter("control");
        last_control = collector().control_step(due, inventory, release);
        enter("data");
        last_data = collector().data_step(due, inventory, release);
        if (last_data->gate.permitted && !last_data->swept.yielded && last_data->swept.complete)
          _gc_quiet_until = last_data->swept.deferred ? _traced.time.now() + last_data->orphan_grace : TimePoint::max();
      } else {
        survey();
      }
      enter("local");
      (void)_node.dagda_node().run_local_stages();
    } catch (const MaintenanceStopping&) {
      stopped_by_throw = true;
    } catch (const std::exception& error) {
      failure = error.what();
    }

    if (gc_due_this_pass && !busy) arm_retirement_wake();
    park(gc_due_this_pass, busy, paced, repair_continue);
  }

 private:
  // Macha's four events.
  uint64_t events() const { return _node.dagda_node().maintenance_events().total(); }
  void enter(const std::string& step) {
    if (before_step) before_step(step);
    sequence.push_back(step);
  }
  std::function<void()> pause() {
    return [token = stop.get_token()] {
      if (token.stop_requested()) throw MaintenanceStopping{};
    };
  }
  // Macha's survey lambda: a stop goes on, anything else is logged and the pass
  // carries on without it (maintenance.cpp:595-612).
  void survey() {
    enter(order_survey);
    try {
      (void)_node.repair().survey(survey_pause ? survey_pause : pause());
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception& error) {
      survey_failure = error.what();
    }
  }
  // An immature retirement is future work: one exact wake for the earliest.
  void arm_retirement_wake() {
    const auto grace = std::chrono::duration_cast<std::chrono::nanoseconds>(garbage_grace).count();
    const auto now_ns = static_cast<int64_t>(_traced.wall.unix_ms()) * 1'000'000;
    std::optional<int64_t> earliest;
    const auto inventory = _node.ledger().inventory();
    if (!inventory) return;
    for (const auto& candidate : inventory->garbage()) {
      if (candidate.retired_at_ns <= 0) continue;
      const auto elapsed = now_ns - candidate.retired_at_ns;
      if (elapsed >= grace) continue;
      const auto remaining = grace - elapsed;
      if (!earliest || remaining < *earliest) earliest = remaining;
    }
    if (earliest) _gc_quiet_until = _traced.time.now() + std::chrono::nanoseconds(*earliest);
  }
  // `paced`: as the pass found it when it began, Macha's repair_busy (maintenance.cpp:1362).
  void park(bool gc_due_this_pass, bool busy, uint8_t paced, bool repair_continue) {
    const auto now = _traced.time.now();
    auto deadline = TimePoint::max();
    if (_gc_quiet_until != TimePoint{} && _gc_quiet_until != TimePoint::max()) {
      if (_gc_quiet_until > now) {
        deadline = std::min(deadline, _gc_quiet_until);
      } else if (!gc_due_this_pass && !busy) {
        deadline = now;
      }
    }
    if (busy) {
      // A busy pass suppressed GC, repair and rebalance; it re-evaluates as soon as
      // the foreground quiet period is met, or at once if it already has, rather
      // than sleeping until an unrelated event (maintenance.cpp:1327-1336). The
      // window is the one `busy` was read against, repair's.
      auto& activity = _node.dagda_node().activity();
      const auto idle = std::min(activity.idle_for(dannan::sched::WorkClass::foreground), activity.idle_for(dannan::sched::WorkClass::loader));
      const auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(_node.options().repair.foreground_quiet);
      deadline = std::min(deadline, now + (idle < quiet ? quiet - idle : std::chrono::milliseconds{}));
    }
    auto& repair = _node.repair();
    if (_network_quiet_until != TimePoint::max()) {
      const auto turn = repair.turn_wait(now, paced);
      if (turn > 0ms) deadline = std::min(deadline, now + turn);
      const double extent = static_cast<double>(_node.options().repair.extent_size);
      if (_earned.rate > 0.0 && static_cast<double>(repair.credit()) < extent) {
        const auto seconds = (extent - static_cast<double>(repair.credit())) / _earned.rate;
        deadline = std::min(deadline, now + std::chrono::duration_cast<dannan::contract::Clock::duration>(std::chrono::duration<double>(seconds)));
      }
      if (repair_continue) deadline = std::min(deadline, now + interval);
    }
    // A deferred survey, or a peer to ask again.
    if (const auto due = _node.availability().due()) deadline = std::min(deadline, std::max(*due, now));
    _deadline = deadline;
  }

  ClusterNode& _node;
  Traced& _traced;
  Owner& _owner;
  uint64_t _observed_event = 0;
  uint64_t _last_generation = 0;
  TimePoint _network_quiet_until{};
  TimePoint _gc_quiet_until{};
  TimePoint _deadline{};
  store::RepairCredit _earned;
};

// Macha's TracedNode: one traced node, what it holds, and its steps.
class TracedNode {
 public:
  // `hosts_beside`: the peer beside it hosts extents too (the two-host fixture).
  explicit TracedNode(bool hosts_beside = false, size_t replication = 1, size_t write_copies = 1) {
    _node = &_cluster.add("traced", _traced.options(replication, write_copies));
    auto peer = _traced.options(replication, write_copies);
    peer.maintenance.trace = {};
    peer.maintenance.time = nullptr;
    peer.maintenance.wall = nullptr;
    peer.hosts_extents = hosts_beside;
    _peer = &_cluster.add("peer", peer);
    _traced.log.ignore("release.control");
  }
  void start() {
    _cluster.start_all();
    ASSERT_TRUE(_cluster.converged());
    ASSERT_TRUE(wait_until([&] { return _node->data().held_view().identity.complete; }));
    _owner = std::make_unique<Owner>(*_node, _traced.time, _traced.wall);
    _loop = std::make_unique<MachaLoop>(*_node, _traced, *_owner);
    _loop->advance(foreground_quiet * 2);
    step("started");
  }

  ClusterNode& node() { return *_node; }
  ClusterNode& peer() { return *_peer; }
  Cluster& cluster() { return _cluster; }
  Owner& owner() { return *_owner; }
  MachaLoop& loop() { return *_loop; }
  Traced& traced() { return _traced; }
  TraceLog& log() { return _traced.log; }
  void advance(std::chrono::milliseconds by) { _loop->advance(by); }
  void watch(std::string name, const ObjectId& id) { _watched.emplace_back(std::move(name), id); }
  void step(std::string_view name) {
    _loop->settle();
    auto& claims = _node->claims();
    std::vector<std::string> state{"state: data_objects=" + std::to_string(_node->data().list().size()) +
                                   " data_claims=" + std::to_string(claims.retained_ids(RetentionClass::data).size()) +
                                   " control_claims=" + std::to_string(claims.retained_ids(RetentionClass::control).size())};
    for (const auto& [watched, id] : _watched)
      state.push_back("object " + watched + ": held=" + (_node->data().has(id) ? "1" : "0") +
                      " claimed=" + (claims.retained(RetentionClass::data, id) ? "1" : "0"));
    _traced.log.step(name, state);
  }
  std::vector<std::string> trace() { return normalise(_traced.log.lines()); }

 private:
  Traced _traced;
  Cluster _cluster;
  ClusterNode* _node = nullptr;
  ClusterNode* _peer = nullptr;
  std::unique_ptr<Owner> _owner;
  std::unique_ptr<MachaLoop> _loop;
  std::vector<std::pair<std::string, ObjectId>> _watched;
};

// ---- Macha's fixtures, unchanged -----------------------------------------------

// test_trace_quiet_node: a node that has only started: what the pass decides with
// nothing to do, through the quiet window and past the no-progress back-off.
TEST(MaintenanceSteps, TheQuietNodeFixture) {
  TracedNode node;
  node.start();
  node.advance(100ms);
  node.step("quiet window passed");
  node.advance(10min);
  node.step("back-off passed");
  check_against_fixture("quiet-node", node.trace());
}

// test_trace_tombstones_maturing: a file deleted: its tombstone is protected for
// the whole grace and erased once it has matured, measured on the pass's clock.
TEST(MaintenanceSteps, TheTombstonesMaturingFixture) {
  TracedNode node;
  node.start();
  auto& owner = node.owner();
  const auto kept = owner.write(1);
  const auto deleted = owner.write(2);
  node.watch("kept", kept);
  node.watch("deleted", deleted);
  node.advance(100ms);
  node.step("two files written");
  owner.unlink(deleted);
  node.advance(100ms);
  node.step("one deleted");
  node.advance(30min);
  // An unrelated event wakes the pass mid-grace, so the grace check, not the
  // armed deadline, must keep the tombstone.
  (void)owner.write(30);
  node.advance(100ms);
  node.step("half the grace, an unrelated write");
  node.advance(31min);
  node.step("grace passed");
  node.advance(100ms);
  node.step("the sweep after the erasure");
  check_against_fixture("tombstones-maturing", node.trace());
}

// test_trace_revived_tombstone: a deleted file's content is written again before
// its tombstone matures: the object is live again, so the tombstone is stale and
// is erased at once, not collected.
TEST(MaintenanceSteps, TheRevivedTombstoneFixture) {
  TracedNode node;
  node.start();
  auto& owner = node.owner();
  const auto first = owner.write(4);
  node.watch("first", first);
  node.advance(100ms);
  node.step("file written");
  owner.unlink(first);
  node.advance(100ms);
  node.step("file deleted");
  const auto again = owner.write(4);
  node.watch("again", again);
  node.advance(100ms);
  node.step("same content written again");
  node.advance(100ms);
  node.step("the pass after");
  check_against_fixture("revived-tombstone", node.trace());
}

// test_trace_backend_offline_and_back: the DATA backend disappears and comes back.
TEST(MaintenanceSteps, TheBackendOfflineFixture) {
  TracedNode node;
  node.start();
  auto& traced = node.node();
  const auto file = node.owner().write(3);
  node.watch("file", file);
  node.advance(100ms);
  node.step("file written");
  auto away = traced.backend();
  away += ".away";
  std::filesystem::rename(traced.backend(), away);
  ASSERT_TRUE(wait_until([&] { return traced.data().online_backends() == 0; }));
  node.advance(100ms);
  node.step("backend offline");
  node.advance(10min);
  node.step("offline past the back-off");
  std::filesystem::rename(away, traced.backend());
  ASSERT_TRUE(wait_until([&] { return traced.data().online_backends() == 1; }));
  node.advance(100ms);
  node.step("backend back");
  check_against_fixture("backend-offline", node.trace());
}

// test_trace_claimed_objects_lost: claimed objects vanish from the only node that
// held them. The claim walk and the pull each visit in their own order, with no
// peer to fetch from; a change to either order changes this trace.
TEST(MaintenanceSteps, TheClaimedObjectsLostFixture) {
  TracedNode node;
  node.start();
  auto& owner = node.owner();
  for (uint8_t index = 0; index < 3; ++index) node.watch("lost-" + std::to_string(index), owner.write(static_cast<uint8_t>(10 + index)));
  node.advance(100ms);
  node.step("three files written");
  for (const auto& id : node.node().claims().retained_ids(RetentionClass::data)) ASSERT_TRUE(node.node().data().remove(id));
  node.advance(10min);
  // Nothing is walked: repair waits for an event, and a lost file is not one.
  node.step("their objects lost");
  (void)owner.write(20);
  node.advance(100ms);
  node.step("an unrelated file written");
  check_against_fixture("claimed-objects-lost", node.trace());
}

// test_trace_peer_unreachable_and_back: a peer drops out of a two-node cluster.
// Both own every object, which copies first is a race by design, so repair is not
// compared, and whether a GC-due pass falls while it is away depends on when
// topology events land, so verdicts are not either.
TEST(MaintenanceSteps, ThePeerUnreachableFixture) {
  TracedNode node(true, 2, 2);
  node.log().ignore("repair");
  node.log().skip_verdicts();
  node.start();
  auto& owner = node.owner();
  const auto doomed = owner.write_on_both(4, node.peer());
  node.watch("doomed", doomed);
  node.advance(100ms);
  node.step("peer joined, file written");

  const auto peer_id = node.peer().id();
  node.peer().stop();
  ASSERT_TRUE(node.cluster().dead_to(node.node(), peer_id));
  owner.unlink(doomed);
  node.advance(100ms);
  node.step("peer gone, file deleted");
  node.advance(2h);
  node.step("peer gone past the grace");

  node.peer().start();
  ASSERT_TRUE(node.cluster().converged());
  node.advance(100ms);
  node.advance(100ms);
  node.step("peer back");
  node.advance(100ms);
  node.step("the pass after the peer returned");
  check_against_fixture("peer-unreachable", node.trace());
}

// test_trace_incomplete_catalogue_and_release_horizon: the catalogue's root is lost
// from the only node that held it: the inventory is incomplete, so every
// destructive gate stays shut, and the release cannot be completed either, so a
// file deleted afterwards keeps its claim.
TEST(MaintenanceSteps, TheIncompleteCatalogueFixture) {
  TracedNode node;
  node.start();
  auto& owner = node.owner();
  const auto root = owner.add_catalogue();
  const auto claimed = owner.write(5);
  node.watch("claimed", claimed);
  node.advance(100ms);
  node.step("catalogue item and a file added");
  ASSERT_TRUE(node.node().control().remove(root));
  owner.lose_catalogue(root);
  owner.unlink(claimed);
  node.advance(100ms);
  node.step("catalogue root lost, file deleted");
  node.advance(2h);
  node.step("past the grace");
  check_against_fixture("incomplete-catalogue", node.trace());
}

// ---- The order itself, and what can fall in it ---------------------------------

// Two hosts and an object to survey, parked: the next pass is due in every part
// and every step runs, to Macha's order.
class StepsBed : public testing::Test {
 protected:
  StepsBed() : node(true, 2, 1) {}
  void SetUp() override {
    node.log().ignore("repair");
    node.start();
    next_write();
    node.advance(100ms);
  }
  // Something new for the next pass's inventory, survey and turn.
  ObjectId next_write() { return node.owner().write(++salt); }
  MachaLoop& loop() { return node.loop(); }
  // After this a pass finds every step due.
  void make_due() {
    (void)next_write();
    // Past the quiet window, and past any survey's cost at its share, as a fixture's
    // advance moves past it.
    loop().move(1min);
    loop().stop = std::stop_source{};
    loop().before_step = {};
    loop().survey_pause = {};
    node.traced().on_trace = {};
  }

  TracedNode node;
  uint8_t salt = 100;
};

TEST_F(StepsBed, EveryStepRunsInMachasOrder) {
  make_due();
  loop().pass();
  EXPECT_EQ(loop().sequence, macha_order);
  EXPECT_FALSE(loop().stopped_by_throw);
  EXPECT_EQ(loop().failure, "");
  ASSERT_TRUE(loop().last_inventory);
  EXPECT_TRUE(loop().last_inventory->rebuilt);
}

// A stop between two steps, as Macha's falls: nothing looks at it until a step
// with a walk, which throws it; past the survey nothing can, so the pass runs to
// its end. Either way the loop's condition ends it, and no second pass starts.
TEST_F(StepsBed, AStopBetweenAnyTwoStepsEndsTheLoopAsMachasDoes) {
  for (size_t index = 1; index < macha_order.size(); ++index) {
    SCOPED_TRACE(macha_order[index]);
    make_due();
    const auto passes = loop().passes;
    loop().before_step = [&, at = macha_order[index]](const std::string& step) {
      if (step == at) loop().stop.request_stop();
    };
    loop().run_until_stopped(4);
    EXPECT_EQ(loop().passes, passes + 1);
    EXPECT_EQ(loop().failure, "");
    // The first step after the stop with a walk in it throws it.
    const auto inventory_at = static_cast<size_t>(std::find(macha_order.begin(), macha_order.end(), order_inventory) - macha_order.begin());
    const auto survey_at = static_cast<size_t>(std::find(macha_order.begin(), macha_order.end(), order_survey) - macha_order.begin());
    const size_t thrown_at = index <= inventory_at ? inventory_at : index <= survey_at ? survey_at : macha_order.size();
    EXPECT_EQ(loop().stopped_by_throw, thrown_at < macha_order.size());
    const auto last = std::min(thrown_at + 1, macha_order.size());
    EXPECT_EQ(loop().sequence, std::vector<std::string>(macha_order.begin(), macha_order.begin() + static_cast<std::ptrdiff_t>(last)));
  }
}

// Inside the inventory's walk: the owner's walk asks its pause, the stop comes
// through the step, and nothing is published.
TEST_F(StepsBed, AStopInsideTheInventorysWalkComesThroughTheStep) {
  make_due();
  const auto before = node.node().ledger().inventory();
  node.owner().walk = [&](const std::function<void()>& pause) {
    pause();
    loop().stop.request_stop();
    pause();
    ADD_FAILURE() << "the walk went on past its stop";
  };
  loop().run_until_stopped(4);
  node.owner().walk = {};
  EXPECT_TRUE(loop().stopped_by_throw);
  EXPECT_EQ(loop().sequence.back(), order_inventory);
  EXPECT_EQ(node.node().ledger().inventory(), before);
  // Directly, as a caller sees it.
  std::stop_source direct;
  direct.request_stop();
  EXPECT_THROW((void)loop().collector().inventory_step([&direct] {
    if (direct.stop_requested()) throw MaintenanceStopping{};
  }),
               MaintenanceStopping);
}

// Inside the survey: at the walk over what this node holds, and at a question to a
// peer in mid-diff, whose own catch does not take it for the peer's failure.
TEST_F(StepsBed, AStopInsideTheSurveyComesThroughTheStep) {
  make_due();
  node.owner().refer_to_many(5000);
  size_t pauses = 0;
  loop().survey_pause = [&] {
    if (++pauses == 1) loop().stop.request_stop();
    if (loop().stop.stop_requested()) throw MaintenanceStopping{};
  };
  loop().run_until_stopped(4);
  EXPECT_TRUE(loop().stopped_by_throw);
  EXPECT_EQ(loop().sequence.back(), order_survey);
  EXPECT_EQ(loop().survey_failure, "");

  make_due();
  bool asked = false;
  node.peer().interpose(
      [&](dannan::net::MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<dannan::net::Handled> {
        if (type == store::message::trie_diff && !asked) {
          asked = true;
          loop().stop.request_stop();
        }
        return std::nullopt;
      });
  loop().run_until_stopped(4);
  node.peer().interpose({});
  EXPECT_TRUE(asked);
  EXPECT_TRUE(loop().stopped_by_throw);
  EXPECT_EQ(loop().sequence.back(), order_survey);
  EXPECT_EQ(loop().survey_failure, "");
}

// Inside the steps with no walk Macha's stop cannot reach: the claim walk, the
// transfer and the sweep run to their bounds, the pass to its end, and the loop's
// condition ends it there.
TEST_F(StepsBed, AStopInsideABoundedStepWaitsForItsBound) {
  // The claim walk: a claim lost here, its line the stop.
  make_due();
  const auto lost = next_write();
  ASSERT_TRUE(node.node().data().remove(lost));
  node.traced().on_trace = [&](std::string_view kind) {
    if (kind == "claim-walk") loop().stop.request_stop();
  };
  loop().run_until_stopped(4);
  EXPECT_TRUE(loop().stop.stop_requested());
  EXPECT_FALSE(loop().stopped_by_throw);
  EXPECT_EQ(loop().sequence, macha_order);

  // The transfer: a push's line the stop; the push it is in lands, and the pass
  // goes on to its end.
  make_due();
  const auto pushed = next_write();
  node.traced().on_trace = [&](std::string_view kind) {
    if (kind == "repair") loop().stop.request_stop();
  };
  loop().run_until_stopped(4);
  EXPECT_TRUE(loop().stop.stop_requested());
  EXPECT_FALSE(loop().stopped_by_throw);
  EXPECT_EQ(loop().sequence, macha_order);
  EXPECT_TRUE(node.peer().data().has(pushed));

  // The sweep's should_yield is asked for foreground work, never for the stop.
  make_due();
  loop().pass();
  loop().move(foreground_quiet * 2);
  ledger::PassDue due;
  due.garbage_due = true;
  due.gc_due = true;
  size_t asked = 0;
  due.should_yield = [&] {
    ++asked;
    loop().stop.request_stop();
    return false;
  };
  const auto inventory = loop().collector().inventory_step();
  ASSERT_FALSE(inventory.rebuilt);
  const auto release = loop().collector().release_step();
  const auto swept = loop().collector().data_step(due, inventory, release);
  EXPECT_TRUE(swept.gate.permitted);
  EXPECT_GT(asked, 1u);
  EXPECT_TRUE(swept.swept.complete);
}

// The loop's sweep works from the node's own sightings, in the node's own files.
TEST_F(StepsBed, TheSweepUsesTheNodesOwnSightings) {
  auto& dagda = node.node().dagda_node();
  const auto bytes = pattern(64 * 1024, 0xee);
  ASSERT_TRUE(node.node().data().put(store::object_id(bytes), bytes));
  ASSERT_EQ(dagda.data_sightings().size(), 0u);
  make_due();
  loop().pass();
  loop().move(foreground_quiet * 2);
  loop().pass();
  ASSERT_TRUE(loop().last_data);
  ASSERT_TRUE(loop().last_data->gate.permitted) << loop().last_data->gate.reason;
  EXPECT_GT(dagda.data_sightings().size(), 0u);
  EXPECT_TRUE(std::filesystem::exists(node.node().state() / "retention" / "unreferenced-data.bin"));
}

// second-cutover B6: what Macha's pass records (maintenance.cpp:642-655, 812-821,
// 919-920, 1049-1054), as its steps record it here, split as Macha's is by whether a
// higher class was present as the transfer ended.
TEST_F(StepsBed, APassRecordsMachasObservations) {
  auto& observations = node.node().context().observations();
  const auto series = [&](const std::string& name) {
    const auto snapshot = observations.snapshot();
    const auto found = snapshot.histograms.find(name);
    return found == snapshot.histograms.end() ? uint64_t{0} : found->second.count;
  };
  const auto counter = [&](const std::string& name) -> std::optional<uint64_t> {
    const auto snapshot = observations.snapshot();
    const auto found = snapshot.counters.find(name);
    if (found == snapshot.counters.end()) return std::nullopt;
    return found->second;
  };

  make_due();
  const auto built = series("maintenance.inventory.build_us");
  const auto horizon = series("maintenance.release_horizon.build_us");
  const auto steps = series("maintenance.repair.step_us.idle");
  const auto bytes = counter("maintenance.repair.bytes.idle").value_or(0);
  loop().pass();
  EXPECT_GT(series("maintenance.inventory.build_us"), built);
  EXPECT_GT(series("maintenance.release_horizon.build_us"), horizon);
  EXPECT_GT(series("maintenance.repair.step_us.idle"), steps);
  EXPECT_GT(counter("maintenance.repair.bytes.idle").value_or(0), bytes);
  EXPECT_GT(counter("maintenance.repair.push_examined.idle").value_or(0), 0u);
  EXPECT_TRUE(counter("maintenance.repair.pull_examined.idle").has_value());
  EXPECT_TRUE(counter("maintenance.repair.retained_repairs").has_value());

  // The sweep's, once the gate is open and it has something to examine.
  loop().move(foreground_quiet * 2);
  loop().pass();
  ASSERT_TRUE(loop().last_data);
  ASSERT_TRUE(loop().last_data->gate.permitted) << loop().last_data->gate.reason;
  ASSERT_GT(loop().last_data->swept.objects, 0u);
  EXPECT_GT(series("maintenance.gc.step_us"), 0u);
  EXPECT_GT(series("maintenance.gc.per_object_ns"), 0u);

  // A higher class present as the step ends.
  make_due();
  node.node().dagda_node().activity().note(dannan::sched::FrameType::foreground, 64 * 1024);
  loop().pass();
  EXPECT_GT(series("maintenance.repair.step_us.loaded"), 0u);
  EXPECT_TRUE(counter("maintenance.repair.bytes.loaded").has_value());
}

// A failure in the inventory: Macha's outer catch logs it and the rest of the pass
// is skipped; the horizon stands and the next pass asks again.
TEST_F(StepsBed, AFailedInventoryEndsThePassAndTheNextAsksAgain) {
  make_due();
  const auto before = node.node().ledger().inventory();
  node.owner().throw_on_inventory = true;
  loop().pass();
  EXPECT_EQ(loop().failure, "a node of the state is unreadable");
  EXPECT_EQ(loop().sequence.back(), order_inventory);
  EXPECT_EQ(node.node().ledger().inventory(), before);
  node.owner().throw_on_inventory = false;
  loop().pass();
  EXPECT_EQ(loop().failure, "");
  EXPECT_EQ(loop().sequence, macha_order);
  EXPECT_NE(node.node().ledger().inventory(), before);
}

// A failure in the survey: Macha's own catch logs it and repair runs on the lists
// it has. It falls in the join against what this node holds: one in a peer's diff
// is that peer's and the survey's own.
TEST_F(StepsBed, AFailedSurveyLeavesThePassRunning) {
  make_due();
  node.owner().refer_to_many(5000);
  loop().survey_pause = [] { throw std::runtime_error("the survey could not read"); };
  loop().pass();
  EXPECT_EQ(loop().survey_failure, "the survey could not read");
  EXPECT_EQ(loop().failure, "");
  EXPECT_EQ(loop().sequence, macha_order);
}

// A failure inside a turn: it goes out to the outer catch, and the turn still
// closes, or the share would count it active for ever and never compute a rest.
TEST_F(StepsBed, AFailedTurnStillClosesTheShare) {
  make_due();
  const auto lost = next_write();
  ASSERT_TRUE(node.node().data().remove(lost));
  // A class above it present, so the turn earns a rest for the time it ran.
  node.node().dagda_node().activity().note(dannan::sched::FrameType::foreground, 64 * 1024);
  node.traced().on_trace = [&](std::string_view kind) {
    if (kind != "claim-walk") return;
    node.traced().time.advance(100ms);
    throw std::runtime_error("the walk failed");
  };
  loop().pass();
  EXPECT_EQ(loop().failure, "the walk failed");
  EXPECT_EQ(loop().sequence.back(), "walks");
  const auto rest = node.node().repair().turn_wait(node.traced().time.now(), store::paced_by_foreground);
  EXPECT_GT(rest, 0ms);
}

// A failure committing what tombstone collection decided: to the outer catch, and
// neither release nor sweep runs on it.
TEST_F(StepsBed, AFailedTombstoneCommitEndsThePass) {
  make_due();
  loop().pass();
  loop().move(foreground_quiet * 2);
  node.owner().throw_on_collect = true;
  loop().pass();
  node.owner().throw_on_collect = false;
  EXPECT_EQ(loop().failure, "the commit could not be made");
  EXPECT_EQ(loop().sequence.back(), "tombstones");
}

// A release the owner cannot read is an incomplete build, not a failure: the
// horizon stays and the pass goes on.
TEST_F(StepsBed, AnUnreadableReleaseIsIncompleteAndThePassGoesOn) {
  make_due();
  const auto before = node.node().ledger().release();
  node.owner().throw_on_release = true;
  loop().pass();
  node.owner().throw_on_release = false;
  EXPECT_EQ(loop().failure, "");
  EXPECT_EQ(loop().sequence, macha_order);
  EXPECT_EQ(node.node().ledger().release(), before);
}

// Nothing due: no inventory, no gate reached, only the survey and the local stages,
// as Macha's else branch.
TEST_F(StepsBed, WhenNoTurnIsDueOnlyTheSurveyRuns) {
  loop().advance(100ms);
  // Repair has gone quiet, the sweep has its wait, and the owner's state is dirty.
  loop().metadata_dirty = true;
  ASSERT_EQ(loop().settled, store::RepairSettled::quiescent);
  loop().busy_for_test = true;
  loop().pass();
  loop().busy_for_test = false;
  loop().metadata_dirty = false;
  EXPECT_EQ(loop().sequence, (std::vector<std::string>{"credit", "gate.repair", order_survey, "local"}));
}

// Repair alone due, with the owner's state not ready: a stale inventory is held
// back, the first never is, and every gate is reached and stays shut.
TEST_F(StepsBed, ARepairOnlyPassHoldsAStaleInventoryBack) {
  (void)next_write();
  loop().metadata_dirty = true;
  loop().metadata_ready = false;
  loop().busy_for_test = true;
  const auto before = node.node().ledger().inventory();
  loop().pass();
  loop().metadata_dirty = false;
  loop().metadata_ready = true;
  loop().busy_for_test = false;
  ASSERT_TRUE(loop().last_inventory);
  EXPECT_TRUE(loop().last_inventory->held_back);
  EXPECT_FALSE(loop().last_inventory->rebuilt);
  EXPECT_EQ(node.node().ledger().inventory(), before);
  EXPECT_EQ(loop().sequence, macha_order);
  EXPECT_FALSE(loop().last_tombstones->gate.permitted);
  EXPECT_EQ(loop().last_tombstones->gate.reason, "not due");
  EXPECT_FALSE(loop().last_control->gate.permitted);
  EXPECT_EQ(loop().last_control->gate.reason, "foreground busy");
  EXPECT_FALSE(loop().last_data->gate.permitted);
  EXPECT_EQ(loop().last_data->swept.objects, 0u);
}

// The repair's own turn not due: the share holds it while a class above it is
// present and a rest has been earned; the gate is counted and nothing runs.
TEST_F(StepsBed, ATurnTheShareHoldsIsNotWalked) {
  make_due();
  auto& repair = node.node().repair();
  auto& time = node.traced().time;
  node.node().dagda_node().activity().note(dannan::sched::FrameType::foreground, 64 * 1024);
  const auto paced = repair.paced_by();
  ASSERT_TRUE(paced & store::paced_by_foreground);
  {
    auto turn = repair.begin_turn(time.now(), paced);
    time.advance(100ms);
  }
  const auto gated = repair.gates().share;
  loop().pass();
  EXPECT_EQ(repair.gates().share, gated + 1);
  EXPECT_EQ(std::find(loop().sequence.begin(), loop().sequence.end(), "walks"), loop().sequence.end());
  // Busy, so nothing destructive is due either; only the survey and the local
  // stages ran.
  EXPECT_EQ(loop().sequence, (std::vector<std::string>{"credit", "gate.repair", order_survey, "local"}));
}

// What a turn leaves owing, in the order Macha's pass asks (maintenance.cpp:835-854),
// and only nothing left to do drops the credit.
TEST_F(StepsBed, SettleAsksInMachasOrder) {
  auto& repair = node.node().repair();
  const auto now = node.traced().time.now();
  const auto expect = [&](const store::RepairWalks& walks, const store::RepairResult& step, store::RepairSettled settled) {
    (void)repair.earn_credit(now, interval);
    const auto credit = repair.credit();
    ASSERT_GT(credit, 0u);
    EXPECT_EQ(repair.settle(walks, step), settled);
    EXPECT_EQ(repair.credit(), settled == store::RepairSettled::quiescent ? 0u : credit);
  };
  store::RepairResult done;
  done.complete = true;
  store::RepairWalks idle;
  store::RepairWalks waiting;
  waiting.waiting_for_credit = true;
  store::RepairWalks unfinished;
  unfinished.unfinished = true;
  unfinished.waiting_for_credit = true;
  store::RepairWalks restored;
  restored.restored = 1;
  auto limited = done;
  limited.credit_limited = true;
  limited.yielded = true;
  auto yielded = done;
  yielded.yielded = true;
  auto moved = done;
  moved.bytes_transferred = 1;
  auto partial = done;
  partial.complete = false;

  expect(idle, limited, store::RepairSettled::credit);
  expect(restored, yielded, store::RepairSettled::yielded);
  expect(idle, partial, store::RepairSettled::more_work);
  expect(idle, moved, store::RepairSettled::more_work);
  expect(restored, done, store::RepairSettled::more_work);
  expect(unfinished, done, store::RepairSettled::more_work);
  expect(waiting, done, store::RepairSettled::waiting_for_credit);
  expect(idle, done, store::RepairSettled::quiescent);
}

// Macha's busy wake (maintenance.cpp:1327-1336): a pass that began with a viewer
// or the loader present held GC, repair and rebalance back, and looks again when
// the quiet window ends, not at the next unrelated event.
TEST_F(StepsBed, ABusyPassWakesWhenTheQuietWindowEnds) {
  // Repair quiet and the sweep waiting: nothing else is due.
  loop().advance(100ms);
  ASSERT_EQ(loop().settled, store::RepairSettled::quiescent);
  node.node().dagda_node().activity().note(dannan::sched::FrameType::foreground, 64 * 1024);
  loop().pass();
  ASSERT_TRUE(node.node().repair().paced_by() & store::paced_by_foreground);
  const auto passes = loop().passes;
  const auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(node.node().options().repair.foreground_quiet);
  loop().move(quiet);
  loop().settle();
  EXPECT_GT(loop().passes, passes);
}

// test_repair_resumes_after_yielding_to_a_viewer_that_arrived_mid_step (Macha
// a931fe5, 0.93.6): a viewer arriving while a repair step runs makes the step
// yield. The pass began with no viewer, so neither its busy wake nor the share's
// next turn is set, and the credit is past an extent: repair must still come back
// within an interval, not sleep until an unrelated event.
TEST_F(StepsBed, RepairResumesAfterYieldingToAViewerThatArrivedMidStep) {
  // The peer never pulls (its maintenance is off): only this node's push delivers it.
  const auto id = next_write();
  ASSERT_FALSE(node.peer().data().has(id));
  bool viewer_arrives = true;
  loop().before_step = [&](const std::string& step) {
    if (step != "transfer" || !viewer_arrives) return;
    // Past repair's 25 ms slice, so the step yields to the viewer whether or not
    // one was present when the pass began.
    node.traced().time.advance(30ms);
    node.node().dagda_node().activity().note(dannan::sched::FrameType::foreground, 64 * 1024);
  };
  // The viewer meets every step, past the write's own wakes (the collector's quiet
  // window and the turns a pass that began busy earns), each pass beginning once
  // repair's quiet window has passed, until one that began with no viewer present
  // yields with nothing else due.
  const auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(node.node().options().repair.foreground_quiet);
  loop().settle();
  for (int step = 0; step < 3; ++step) {
    loop().move(quiet + interval);
    loop().settle();
  }
  ASSERT_EQ(loop().settled, store::RepairSettled::yielded);
  ASSERT_FALSE(node.peer().data().has(id));

  // The viewer has gone. Nothing else is due, so only repair's own wake can
  // deliver the copy.
  viewer_arrives = false;
  loop().settle();
  loop().move(interval * 3);
  loop().settle();
  EXPECT_TRUE(node.peer().data().has(id));
}

// Macha reads a peer's foreground traffic once a pass: a viewer who appears during
// the turn does not end it, where a turn reading telemetry each time yields at once.
TEST(MaintenanceStepsPeers, ATurnKeepsThePeerReadingItBeganWithWhenAsked) {
  Cluster cluster;
  ClusterNodeOptions hosting;
  hosting.store.replication = 2;
  auto& node = cluster.add("node", hosting);
  auto& peer = cluster.add("peer", hosting);
  auto watching = hosting;
  watching.hosts_extents = false;
  auto& viewer = cluster.add("viewer", watching);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  // What the peer lacks, and so what a transfer sends.
  const auto lacking = [&](uint8_t salt) {
    const auto bytes = pattern(64 * 1024, salt);
    const auto id = store::object_id(bytes);
    EXPECT_TRUE(node.data().put(id, bytes));
    const std::vector<ObjectId> live{id};
    ledger::PeerLacks lacks;
    lacks.generation = 1;
    lacks.complete = true;
    lacks.lacks.emplace(peer.id(), std::make_shared<const std::vector<ObjectId>>(live));
    store::RepairLists lists;
    lists.inventory = std::make_shared<const contract::InventoryHorizon>(uint64_t{1}, true, live, std::vector<ObjectId>{}, std::vector<Garbage>{});
    lists.lacks = std::make_shared<const ledger::PeerLacks>(std::move(lacks));
    return std::pair{id, lists};
  };
  const auto [first, first_lists] = lacking(7);
  const auto [second, second_lists] = lacking(8);

  auto& repair = node.repair();
  auto& time = node.context().time().source();
  const auto paced = repair.paced_by();
  ASSERT_EQ(paced, 0);
  (void)repair.earn_credit(time.now(), std::chrono::seconds(60));
  const auto playing = pattern(256 * 1024, 9);
  ASSERT_TRUE(peer.data().put(store::object_id(playing), playing));
  std::atomic<bool> watching_on{true};
  std::thread player;
  bool seen = false;
  bool sent = false;
  store::RepairResult kept;
  store::RepairResult reading;
  {
    auto turn = repair.begin_turn(time.now(), paced, store::PeerReading::as_begun);
    // Another node plays from the peer, until this node's telemetry shows it.
    player = std::thread([&] {
      store::ReadOptions read;
      read.frame = dannan::sched::FrameType::foreground;
      while (watching_on.load()) (void)viewer.store().get(store::object_id(playing), read);
    });
    seen = wait_until([&] { return (repair.paced_by() & store::paced_by_peer_foreground) != 0; });
    if (seen) kept = repair.transfer(turn, first_lists);
  }
  sent = peer.data().has(first);
  if (seen) {
    auto turn = repair.begin_turn(time.now(), uint8_t{0});
    reading = repair.transfer(turn, second_lists);
  }
  watching_on = false;
  player.join();
  ASSERT_TRUE(seen);
  EXPECT_FALSE(kept.yielded);
  EXPECT_TRUE(sent);
  EXPECT_TRUE(reading.yielded);
  EXPECT_FALSE(peer.data().has(second));
}

// Macha earns a whole interval when no time has passed; pass() earns nothing. The
// step does either.
TEST_F(StepsBed, CreditEarnsAnIntervalOnlyWhenAsked) {
  auto& repair = node.node().repair();
  const auto now = node.traced().time.now();
  (void)repair.earn_credit(now);
  const auto before = repair.credit();
  EXPECT_EQ(repair.earn_credit(now).credit, before);
  const auto earned = repair.earn_credit(now, interval);
  EXPECT_GT(earned.credit, before);
  EXPECT_DOUBLE_EQ(earned.seconds, 1.0);
  EXPECT_NEAR(static_cast<double>(earned.credit - before), earned.rate, 1.0);
}

}  // namespace
}  // namespace dagda
