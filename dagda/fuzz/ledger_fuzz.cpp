//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The ledgers' bytes: a claims trie's record (ledger/retention.h), the trie diff's
// questions and answers a peer sends (ledger/trie_diff.h), and the sightings table a
// disk holds (ledger/unreferenced_since.h). Each may throw only net::DecodeError, but
// the sightings file's open, which also says std::runtime_error.

#include <map>
#include <stdexcept>
#include <vector>

#include "dagda/fuzz/samples.h"
#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/trie_diff.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dannan/fuzz/fuzz.h"

namespace dagda::fuzz {
namespace {

using dannan::fuzz::Registration;
using dannan::net::Bytes;
using dannan::net::DecodeError;
using ledger::ObjectTrie;

const Registration retention_state(
    "ledger_retention_state",
    [](std::span<const uint8_t> data) {
      try {
        const auto state = ledger::retention_format::decode_state(data);
        (void)state.empty();
      } catch (const DecodeError&) {
      }
      (void)ledger::retention_format::claimed(data);
    },
    [] {
      ledger::retention_format::ObjectState state;
      state.adds = {{sample_node(1), 3}, {sample_node(2), 4}};
      state.removed = {{sample_node(3), 1}};
      return std::vector<Bytes>{ledger::retention_format::encode_state(state), ledger::retention_format::encode_state({})};
    });

const Registration trie_question(
    "ledger_trie_question",
    [](std::span<const uint8_t> data) {
      try {
        (void)ledger::decode_trie_question(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      ledger::TrieQuestion root;
      root.at = sample_id(1);
      ledger::TrieQuestion children = root;
      children.kind = ledger::TrieQuestionKind::children;
      children.prefixes = {ObjectTrie::Prefix{}, ObjectTrie::Prefix{}.child(5)};
      ledger::TrieQuestion records = root;
      records.kind = ledger::TrieQuestionKind::records;
      records.prefixes = {ObjectTrie::Prefix{}.child(5).child(6)};
      return std::vector<Bytes>{ledger::encode_trie_question(root), ledger::encode_trie_question(children), ledger::encode_trie_question(records)};
    });

const Registration trie_root(
    "ledger_trie_root",
    [](std::span<const uint8_t> data) {
      try {
        (void)ledger::decode_trie_root(data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{ledger::encode_trie_root({10, sample_id(1)})}; });

const Registration trie_children(
    "ledger_trie_children",
    [](std::span<const uint8_t> data) {
      try {
        (void)ledger::decode_trie_children(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      std::vector<ObjectTrie::Children> answers(2);
      answers[0][0] = {3, sample_id(1)};
      answers[0][7] = {200, sample_id(2)};
      answers[1][255] = {1, sample_id(3)};
      return std::vector<Bytes>{ledger::encode_trie_children(answers)};
    });

const Registration trie_records(
    "ledger_trie_records",
    [](std::span<const uint8_t> data) {
      try {
        (void)ledger::decode_trie_records(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      std::vector<std::vector<ObjectTrie::Record>> answers(2);
      answers[0].emplace_back(sample_id(1), Bytes{1, 2});
      answers[0].emplace_back(sample_id(2), Bytes{});
      return std::vector<Bytes>{ledger::encode_trie_records(answers)};
    });

// The table, and the file it is sealed in.
const Registration unreferenced_since(
    "ledger_unreferenced_since",
    [](std::span<const uint8_t> data) {
      try {
        (void)ledger::decode_unreferenced_since(data);
      } catch (const DecodeError&) {
      }
      try {
        (void)ledger::open_unreferenced_since(sample_key(), data);
      } catch (const std::runtime_error&) {
      }
    },
    [] {
      const std::map<store::ObjectId, uint64_t> sightings{{sample_id(1), 100}, {sample_id(2), 200}};
      return std::vector<Bytes>{ledger::encode_unreferenced_since(sightings), ledger::seal_unreferenced_since(sample_key(), sightings)};
    });

}  // namespace
}  // namespace dagda::fuzz
