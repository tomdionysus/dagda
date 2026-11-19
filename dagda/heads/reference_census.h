//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/heads/tree.h"
#include "dagda/ledger/reference_counts.h"
#include "dannan/sched/work.h"
#include "dannan/status/log.h"

namespace dagda::heads {

// Brings ledger::ReferenceCounts to a tree's root (the plan's section 2.5, moved
// from Macha's FS census, filesystem.cpp:2498-2576 at 9a08846): what the new tree
// holds that the last one did not goes in, what it no longer holds comes out. The
// whole tree is walked the first time, and if the counts and the trees ever
// disagree, or the tree they stand at can no longer be read. What a value
// references is the owner's (TreeValues::references).
//
// Single owner: called under the reachability provider's inventory and release.
// `pause` runs between the entries a walk visits and the differences a follow
// takes, and may throw contract::MaintenanceStopping; that goes through to the
// caller and leaves the counts where they were. Waits on the state device and on
// the node store (for a control store, the network).
template <class Value>
class ReferenceCensus {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::single_owner;
  static constexpr dannan::sched::Waits count_waits = dannan::sched::local_waits | dannan::sched::Waits::network;

  // The counts and the tree must outlive the census.
  ReferenceCensus(ledger::ReferenceCounts& counts, const Tree<Value>& tree, dannan::status::ScopedLogger logger)
      : _counts(counts), _tree(tree), _logger(std::move(logger)) {}

  // The counts follow to `root`: same root, nothing to do; else follow; else walk.
  // Throws DecodeError if a walk cannot read the tree: an incomplete count is
  // never installed.
  void count(const ObjectId& root, const TreeNodeStore& nodes, const TreePause& pause = {}) {
    const auto counted = _counts.root();
    if (counted) {
      if (*counted == root) return;
      try {
        if (follow(root, nodes, pause)) return;
        _logger.warn("census did not match the tree; walking it again");
      } catch (const contract::MaintenanceStopping&) {
        throw;
      } catch (const std::exception& error) {
        _logger.warn(std::string("census cannot follow the tree; walking it again: ") + error.what());
      }
    }
    walk(root, nodes, pause);
  }

  // An owner's state that has no tree (a genesis before the first commit): the
  // counts hold nothing and no root, with the totals the owner says it has.
  void count_no_tree(ledger::ReferenceCounts::Totals totals = {}) { _counts.reset(std::nullopt, {}, {}, totals); }

 private:
  void walk(const ObjectId& root, const TreeNodeStore& nodes, const TreePause& pause) {
    std::vector<ObjectId> data;
    std::vector<ObjectId> tree_nodes;
    ledger::ReferenceCounts::Totals totals;
    _tree.walk(
        root, nodes,
        [&](const std::string&, const Value& value) {
          const auto references = _tree.references(value);
          ++totals.entries;
          totals.extent_count += references.items;
          data.insert(data.end(), references.data.begin(), references.data.end());
        },
        pause, &tree_nodes);
    _counts.reset(root, std::move(data), std::move(tree_nodes), totals);
  }

  bool follow(const ObjectId& root, const TreeNodeStore& nodes, const TreePause& pause) {
    std::vector<ObjectId> data_in, data_out, nodes_in, nodes_out;
    const auto from = *_counts.root();
    _tree.diff_nodes(from, root, nodes, nodes_in, nodes_out);
    auto totals = _counts.totals();
    for (const auto& [key, difference] : _tree.diff(from, root, nodes)) {
      if (pause) pause();
      if (difference.before) {
        const auto references = _tree.references(*difference.before);
        --totals.entries;
        totals.extent_count -= references.items;
        data_out.insert(data_out.end(), references.data.begin(), references.data.end());
        nodes_out.insert(nodes_out.end(), references.children.begin(), references.children.end());
      }
      if (difference.after) {
        const auto references = _tree.references(*difference.after);
        ++totals.entries;
        totals.extent_count += references.items;
        data_in.insert(data_in.end(), references.data.begin(), references.data.end());
        nodes_in.insert(nodes_in.end(), references.children.begin(), references.children.end());
      }
    }
    return _counts.change(root, std::move(data_in), std::move(data_out), std::move(nodes_in), std::move(nodes_out), totals);
  }

  ledger::ReferenceCounts& _counts;
  const Tree<Value>& _tree;
  const dannan::status::ScopedLogger _logger;
};

}  // namespace dagda::heads
