//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement H18 as bytes: Dagda's tree, given Macha's parameters and a codec that
// knows a leaf record only as a layout, makes every node Macha made, in the order it
// made them, reachable or not (no change in what CONTROL holds), reads what Macha
// reads and answers as Macha answers.
//
// Macha's known answers, from its own code at 9a08846 (tag 0.93.4):
// - tree-vectors-0.93.4: 25 cases by build_namespace_tree and update_namespace_tree.
//   20 of them, the index and the README are checked in as they are under
//   dagda/test/data/tree-vectors-0.93.4 (2.3 MB); the five largest (20 MB) are read
//   from Macha's fixtures, and skipped, saying so, on a machine without them.
// - state-scratch-0.93.4: real objects two real nodes wrote. The root's three nodes
//   are checked in plain (tree-state-scratch-0.93.4.txt); every tree node in the
//   sealed store is read under the fixture's own test key where that exists.
//
// No record here is parsed for what it means: head is opaque, whatever Macha's
// codec puts in it. Nothing in Dagda, nor in these tests, knows the fields.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/heads/tree.h"
#include "dagda/migrate/legacy_v18.h"
#include "dagda/store/local_format.h"
#include "dagda/test/fixtures/json.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::heads {
namespace {

using dannan::identity::hex;
using dannan::identity::unhex;
using test::Json;

// A leaf record's value as a layout and nothing more: a head of a known length, with
// a marked optional section, and a list of records with their inline or external
// forms. Equal bytes are equal values.
struct OpaqueValue {
  Bytes head;
  Bytes records;
  bool operator==(const OpaqueValue&) const = default;
};

class OpaqueValues final : public TreeValues<OpaqueValue> {
 public:
  void encode(dannan::net::Writer& out, const OpaqueValue& value, TreeChildWriter& children) const override {
    out.raw(value.head);
    if (value.records.empty()) {
      out.u8(form_none);
      return;
    }
    const auto count = value.records.size() / record_bytes;
    if (count <= children.inline_max()) {
      out.u8(form_inline);
      out.u32(static_cast<uint32_t>(count));
      out.raw(value.records);
      return;
    }
    out.u8(form_external);
    out.u64(count);
    out.fixed(children.build(value.records).bytes);
  }

  OpaqueValue decode(dannan::net::Reader& in, const TreeChildReader& children, bool with_children) const override {
    OpaqueValue value;
    value.head = read_head(in);
    const auto form = in.u8();
    if (form == form_inline) {
      const auto count = in.u32();
      value.records = in.raw(static_cast<size_t>(count) * record_bytes);
    } else if (form == form_external) {
      (void)in.u64();
      const ObjectId root{in.fixed<32>()};
      if (with_children) children.read(root, value.records);
    } else if (form != form_none) {
      throw DecodeError("bad form");
    }
    return value;
  }

  TreeReferences references(const OpaqueValue& value, TreeChildWriter& children) const override {
    TreeReferences out;
    const auto count = value.records.size() / record_bytes;
    out.items = count;
    for (size_t i = 0; i < count; ++i) {
      const auto id = child_data(std::span<const uint8_t>(value.records).subspan(i * record_bytes, record_bytes));
      if (id) out.data.push_back(*id);
    }
    if (count > children.inline_max()) (void)children.build(value.records);
    return out;
  }

  void skip(dannan::net::Reader& in) const override { (void)shape(in); }

  TreeRecordShape shape(dannan::net::Reader& in) const override {
    TreeRecordShape out;
    (void)read_head(in);
    const auto form = in.u8();
    if (form == form_inline) {
      out.items = in.u32();
      for (uint64_t i = 0; i < out.items; ++i)
        if (const auto id = child_data(in.view(record_bytes))) out.data.push_back(*id);
    } else if (form == form_external) {
      out.items = in.u64();
      out.child_root = ObjectId{in.fixed<32>()};
    } else if (form != form_none) {
      throw DecodeError("bad form");
    }
    return out;
  }

  std::span<const uint8_t> child_boundary(std::span<const uint8_t> record) const override { return record.subspan(17, 32); }

  std::optional<ObjectId> child_data(std::span<const uint8_t> record) const override {
    if (record[16] != 0) return std::nullopt;
    ObjectId id;
    std::copy_n(record.begin() + 17, 32, id.bytes.begin());
    return id;
  }

  static constexpr size_t record_bytes = 49;

 private:
  static constexpr uint8_t form_none = 0;
  static constexpr uint8_t form_inline = 1;
  static constexpr uint8_t form_external = 2;
  static constexpr size_t head_bytes = 44;
  static constexpr size_t section_bytes = 64;
  static constexpr uint8_t section_mark = 0x80;

  static Bytes read_head(dannan::net::Reader& in) {
    const auto first = in.u8();
    const auto rest = in.view(head_bytes + ((first & section_mark) ? section_bytes : 0));
    Bytes head;
    head.reserve(rest.size() + 1);
    head.push_back(first);
    head.insert(head.end(), rest.begin(), rest.end());
    return head;
  }
};

// Macha's forms, as parameters a test gives (H18). The library holds none of them.
TreeForm macha_form(TreeLimits limits = {}) {
  TreeForm form;
  form.leaf_magic = {'M', 'N', 'L', '1'};
  form.branch_magic = {'M', 'N', 'B', '1'};
  form.child_leaf_magic = {'M', 'N', 'X', '1'};
  form.child_branch_magic = {'M', 'N', 'Y', '1'};
  form.entry_domain = "macha/namespace-tree/entry/v1";
  form.branch_domain = "macha/namespace-tree/branch/v1";
  form.child_domain = "macha/namespace-tree/extent/v1";
  form.child_branch_domain = "macha/namespace-tree/extent-branch/v1";
  form.child_record_bytes = OpaqueValues::record_bytes;
  form.limits = limits;
  return form;
}

using OpaqueTree = Tree<OpaqueValue>;

const OpaqueValues& opaque_values() {
  static const OpaqueValues values;
  return values;
}

ObjectId id_of(const std::string& text) {
  const auto bytes = unhex(text);
  if (!bytes || bytes->size() != 32) throw std::runtime_error("not an id: " + text);
  ObjectId id;
  std::copy_n(bytes->begin(), 32, id.bytes.begin());
  return id;
}

std::string key_of_hex(const std::string& text) {
  const auto bytes = unhex(text);
  if (!bytes) throw std::runtime_error("not hex: " + text);
  return std::string(bytes->begin(), bytes->end());
}

std::vector<ObjectId> ids_of(const Json& list) {
  std::vector<ObjectId> out;
  for (const auto& id : list.array()) out.push_back(id_of(id.string()));
  return out;
}

std::string hex_of(const std::vector<ObjectId>& ids) {
  std::string out;
  for (const auto& id : ids) out += hex(id) + "\n";
  return out;
}

const std::filesystem::path checked_in = std::filesystem::path(DAGDA_SOURCE_DIR) / "dagda/test/data/tree-vectors-0.93.4";

// Where Macha's own fixtures are: DAGDA_MACHA_FIXTURES, or this machine's copy.
std::filesystem::path macha_fixtures() {
  if (const char* given = std::getenv("DAGDA_MACHA_FIXTURES")) return given;
  return "../../macha/build/macha-fixtures";
}

void expect_stats(const TreeStats& stats, const Json& json) {
  EXPECT_EQ(stats.entries, json["entries"].u64());
  EXPECT_EQ(stats.items, json["extents"].u64());
  EXPECT_EQ(stats.leaves, json["leaves"].u64());
  EXPECT_EQ(stats.branches, json["branches"].u64());
  EXPECT_EQ(stats.child_nodes, json["extent_nodes"].u64());
  EXPECT_EQ(stats.bytes, json["bytes"].u64());
  EXPECT_EQ(stats.largest_node_bytes, json["largest_node_bytes"].u64());
  EXPECT_EQ(stats.depth, json["depth"].u64());
}

// Macha's node record: its id names its bytes, and what the tree says about it is
// what Macha's did.
void admit_node(const OpaqueTree& tree, const Json& node, MemoryTreeNodeStore& reference) {
  const auto id = id_of(node["id"].string());
  const auto bytes = unhex(node["bytes_hex"].string()).value();
  ASSERT_EQ(store::object_id(bytes), id);
  std::vector<TreeNodeChild> expected;
  for (const auto& child : node["direct_children"].array()) expected.push_back({child["extent"].boolean(), id_of(child["id"].string())});
  EXPECT_TRUE(tree.node_children(bytes) == expected) << node["id"].string();
  reference.put_at(id, bytes);
}

// What was written must be Macha's nodes in Macha's order: ids, and since the ids are
// of these bytes, the bytes.
void expect_written(const std::vector<ObjectId>& written, const Json& nodes, const MemoryTreeNodeStore& store) {
  std::vector<ObjectId> expected;
  for (const auto& node : nodes.array()) expected.push_back(id_of(node["id"].string()));
  EXPECT_EQ(hex_of(written), hex_of(expected));
  for (const auto& node : nodes.array()) {
    const auto held = store.get(id_of(node["id"].string()));
    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(hex(*held), node["bytes_hex"].string());
  }
}

class HeadsTreeVectors : public testing::TestWithParam<std::string> {};

// Each case's whole record. A step's `nodes` are what was new to the store, so the
// store is kept across the steps, as Macha's generator kept it.
TEST_P(HeadsTreeVectors, ReproducesEveryNodeMachaWrote) {
  auto path = checked_in / (GetParam() + ".json");
  if (!std::filesystem::exists(path)) path = macha_fixtures() / "tree-vectors-0.93.4" / (GetParam() + ".json");
  if (!std::filesystem::exists(path)) GTEST_SKIP() << GetParam() << " is too large to check in and Macha's fixtures are not here (DAGDA_MACHA_FIXTURES)";
  const auto vector = Json::read_file(path);
  const auto& limits = vector["limits"];
  TreeLimits given;
  given.entry_target_fanout = static_cast<size_t>(limits["entry_target_fanout"].u64());
  given.entry_max_fanout = static_cast<size_t>(limits["entry_max_fanout"].u64());
  given.branch_target_fanout = static_cast<size_t>(limits["branch_target_fanout"].u64());
  given.branch_max_fanout = static_cast<size_t>(limits["branch_max_fanout"].u64());
  given.child_inline_max = static_cast<size_t>(limits["extent_inline_max"].u64());
  given.child_target_fanout = static_cast<size_t>(limits["extent_target_fanout"].u64());
  given.child_max_fanout = static_cast<size_t>(limits["extent_max_fanout"].u64());
  const OpaqueTree tree(opaque_values(), macha_form(given));

  // Every node Macha made, to read the values from: the input as a leaf holds it.
  MemoryTreeNodeStore reference;
  for (const auto& node : vector["nodes"].array()) admit_node(tree, node, reference);
  for (const auto& step : vector["updates"].array())
    for (const auto& node : step["nodes"].array()) admit_node(tree, node, reference);
  ASSERT_FALSE(HasFailure());

  const auto root = id_of(vector["root"].string());
  const auto entries = tree.read(root, reference);
  // The keys Macha was given, in order.
  std::vector<std::string> keys;
  for (const auto& [key, _] : entries) keys.push_back(key);
  std::vector<std::string> input;
  for (const auto& entry : vector["entries"].array()) input.push_back(key_of_hex(entry["key_hex"].string()));
  ASSERT_EQ(keys, input);

  MemoryTreeNodeStore store;
  ASSERT_EQ(hex(tree.build(entries, store)), vector["root"].string());
  expect_written(store.written(), vector["nodes"], store);
  EXPECT_EQ(store.written().size(), vector["nodes_written"].u64());
  expect_stats(tree.stats(root, store), vector["stats"]);

  // Reachable or not, as marked: what a root reaches is the collector's set, and what
  // it does not is still written.
  std::vector<ObjectId> reached;
  tree.collect_nodes(root, store, reached);
  const std::set<ObjectId> reached_set(reached.begin(), reached.end());
  EXPECT_EQ(reached_set.size(), vector["nodes_reachable"].u64());
  for (const auto& node : vector["nodes"].array())
    EXPECT_EQ(reached_set.contains(id_of(node["id"].string())), node["reachable"].boolean()) << node["id"].string();

  // A full build's reads are none; each lookup is depth or less, without children.
  const auto depth = vector["stats"]["depth"].u64();
  for (const auto& key : keys) {
    store.forget_reads();
    EXPECT_TRUE(tree.lookup(root, key, store, false).has_value());
    EXPECT_LE(store.reads(), depth);
  }

  auto at = root;
  for (const auto& step : vector["updates"].array()) {
    ASSERT_EQ(hex(at), step["root_before"].string()) << step["name"].string();
    const auto after = id_of(step["root_after"].string());
    OpaqueTree::Changes changes;
    for (const auto& change : step["changes"].array()) {
      const auto key = key_of_hex(change["key_hex"].string());
      if (change["entry"].is_null()) {
        changes[key] = std::nullopt;
      } else {
        const auto value = tree.lookup(after, key, reference);
        ASSERT_TRUE(value.has_value()) << step["name"].string();
        changes[key] = *value;
      }
    }
    store.forget_written();
    at = tree.update(at, store, changes);
    ASSERT_EQ(hex(at), step["root_after"].string()) << step["name"].string();
    expect_written(store.written(), step["nodes"], store);
    expect_stats(tree.stats(at, store), step["stats_after"]);

    // Asserted again against Macha's own diff of the two.
    std::vector<std::string> differing;
    for (const auto& [key, _] : tree.diff(id_of(step["root_before"].string()), at, store))
      differing.push_back(hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(key.data()), key.size())));
    std::vector<std::string> expected_keys;
    for (const auto& key : step["differing_keys_hex"].array()) expected_keys.push_back(key.string());
    EXPECT_EQ(differing, expected_keys) << step["name"].string();
    std::vector<ObjectId> added, removed;
    tree.diff_nodes(id_of(step["root_before"].string()), at, store, added, removed);
    EXPECT_EQ(hex_of(added), hex_of(ids_of(step["nodes_added"]))) << step["name"].string();
    EXPECT_EQ(hex_of(removed), hex_of(ids_of(step["nodes_removed"]))) << step["name"].string();

    // And the rebuild of what the update made agrees.
    MemoryTreeNodeStore fresh;
    EXPECT_EQ(tree.build(tree.read(at, store), fresh), at) << step["name"].string();
  }
}

// One test per case, by name, so a skip says which: index.json lists 25.
INSTANTIATE_TEST_SUITE_P(Macha093, HeadsTreeVectors,
                         testing::Values("empty", "root_only", "few", "library_small", "library_medium", "library_large", "library_small_fanout",
                                         "library_tiny_fanout", "remove_everything", "leaf_cap", "every_key_a_boundary", "branch_cap", "packed_fallback_keyed",
                                         "packed_fallback_extents", "extents_0", "extents_1", "extents_8", "extents_9", "extents_600", "extent_chunk_cap",
                                         "holes", "shared_spine", "provenance", "field_ranges", "key_bytes"),
                         [](const testing::TestParamInfo<std::string>& info) { return info.param; });

// The index's roots and counts agree with the case list above, and the checked-in copy
// keeps every one of them named.
TEST(HeadsTreeVectorsIndex, NamesTheTwentyFiveCases) {
  const auto index = Json::read_file(checked_in / "index.json");
  EXPECT_EQ(index["commit"].string(), "9a08846");
  EXPECT_EQ(index["cases"].array().size(), 25u);
  uint64_t orphans = 0;
  for (const auto& entry : index["cases"].array()) orphans += entry["orphans"].u64();
  EXPECT_GT(orphans, 0u);
}

// The boundary rule's parameters: each domain separated from the others and each level
// from the next, u64 max / target as the test.
TEST(HeadsTreeVectorsIndex, TheBoundaryTestIsTheFirstEightBytesOfTheHash) {
  const std::string domain = "macha/namespace-tree/branch/v1";
  const Bytes key{'/', 'k'};
  Bytes whole(domain.begin(), domain.end());
  whole.push_back(3);
  whole.insert(whole.end(), key.begin(), key.end());
  const auto digest = dannan::identity::sha256(whole);
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value = (value << 8) | digest.bytes[i];
  EXPECT_EQ(tree_boundary_hash(domain, 3, key), value);
  EXPECT_NE(tree_boundary_hash(domain, 4, key), value);
  EXPECT_TRUE(is_tree_boundary(0, 2));
  EXPECT_TRUE(is_tree_boundary(UINT64_MAX, 1));
  EXPECT_FALSE(is_tree_boundary(UINT64_MAX / 2, 2));
  EXPECT_TRUE(is_tree_boundary(UINT64_MAX / 2 - 1, 2));
}

const char* const scratch_root = "ade47a9a0e531964cc0cc02c3dddb9a4b8ef68efb873aaba1d50eee44c870a4d";

// The root's three nodes as a real node wrote them, plain, in a file of `<hex id>
// <hex bytes>` lines (extracted from state-scratch-0.93.4/a).
TEST(HeadsTreeState, TheScratchFixturesRootIsReachedByteForByte) {
  std::ifstream in(std::filesystem::path(DAGDA_SOURCE_DIR) / "dagda/test/data/tree-state-scratch-0.93.4.txt");
  ASSERT_TRUE(in.good());
  const OpaqueTree tree(opaque_values(), macha_form());
  MemoryTreeNodeStore reference;
  std::map<std::string, std::string> expected;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto space = line.find(' ');
    const auto id = line.substr(0, space);
    const auto bytes = unhex(line.substr(space + 1)).value();
    ASSERT_EQ(hex(store::object_id(bytes)), id);
    reference.put_at(id_of(id), bytes);
    expected[id] = line.substr(space + 1);
  }
  ASSERT_EQ(expected.size(), 3u);
  const auto root = id_of(scratch_root);
  // Macha's dump: 7 entries, 2 leaves, 1 branch, 1 extent, 1044 bytes, largest 622.
  const auto stats = tree.stats(root, reference);
  EXPECT_EQ(stats.entries, 7u);
  EXPECT_EQ(stats.leaves, 2u);
  EXPECT_EQ(stats.branches, 1u);
  EXPECT_EQ(stats.items, 1u);
  EXPECT_EQ(stats.depth, 2u);
  EXPECT_EQ(stats.bytes, 1044u);
  EXPECT_EQ(stats.largest_node_bytes, 622u);

  MemoryTreeNodeStore store;
  EXPECT_EQ(hex(tree.build(tree.read(root, reference), store)), scratch_root);
  EXPECT_EQ(store.written().size(), 3u);
  for (const auto& id : store.written()) EXPECT_EQ(hex(*store.get(id)), expected.at(hex(id)));
}

// What the fixture's store holds, opened as Macha's store opened it: v18's associated
// data, domain v1's key from the fixture's test cluster key.
std::map<ObjectId, Bytes> open_sealed_store(const std::filesystem::path& root) {
  dannan::test::TempDir dir;
  {
    std::ofstream key(dir.path() / "cluster.key", std::ios::binary);
    key << "macha-fixture-cluster-key\n";
  }
  const auto storage = domain::StorageKey::derive_v1(dannan::identity::load_cluster_key(dir.path() / "cluster.key"));
  std::map<ObjectId, Bytes> out;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "packs")) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    const Bytes pack{std::istreambuf_iterator<char>(in), {}};
    size_t at = 0;
    while (at + store::local_format::pack_header_size <= pack.size()) {
      const auto header = store::local_format::decode_pack_header(std::span<const uint8_t>(pack).subspan(at, store::local_format::pack_header_size));
      if (!header) throw std::runtime_error("damaged pack " + entry.path().string());
      at += store::local_format::pack_header_size;
      if (header->type == store::local_format::pack_put) {
        out[header->id] = migrate::open_object_v18(storage, header->id, header->nonce, header->tag,
                                                   std::span<const uint8_t>(pack).subspan(at, header->payload_size), header->plain_size);
      } else if (header->type == store::local_format::pack_remove) {
        out.erase(header->id);
      }
      at += header->payload_size;
    }
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "objects")) {
    if (!entry.is_regular_file()) continue;
    const auto id = store::local_format::object_id_from_file_name(entry.path().filename().string());
    if (!id) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    const Bytes file{std::istreambuf_iterator<char>(in), {}};
    const auto loose = store::local_format::decode_loose(file);
    out[*id] = migrate::open_object_v18(storage, *id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size);
  }
  return out;
}

// Every tree node a real node wrote: a leaf is the single leaf of the tree of its own
// records, and a branch is the tree of everything beneath it.
TEST(HeadsTreeState, EveryTreeNodeInTheSealedStoreIsMadeAgain) {
  const auto root = macha_fixtures() / "state-scratch-0.93.4/a/metadata-objects";
  if (!std::filesystem::exists(root)) GTEST_SKIP() << "Macha's state-scratch-0.93.4 fixture is not here (DAGDA_MACHA_FIXTURES)";
  const auto objects = open_sealed_store(root);
  const OpaqueTree tree(opaque_values(), macha_form());
  MemoryTreeNodeStore held;
  for (const auto& [id, bytes] : objects) held.put_at(id, bytes);
  ASSERT_TRUE(objects.contains(id_of(scratch_root)));
  size_t leaves = 0, branches = 0, partial = 0;
  for (const auto& [id, bytes] : objects) {
    const bool leaf = bytes.size() >= 4 && std::equal(bytes.begin(), bytes.begin() + 4, macha_form().leaf_magic.begin());
    const bool branch = bytes.size() >= 4 && std::equal(bytes.begin(), bytes.begin() + 4, macha_form().branch_magic.begin());
    if (!leaf && !branch) continue;
    OpaqueTree::Entries entries;
    try {
      entries = tree.read(id, held);
    } catch (const DecodeError&) {
      // A branch over a node since collected: not a thing to make again.
      ASSERT_TRUE(branch) << hex(id);
      ++partial;
      continue;
    }
    MemoryTreeNodeStore fresh;
    EXPECT_EQ(hex(tree.build(entries, fresh)), hex(id));
    EXPECT_EQ(hex(*fresh.get(id)), hex(bytes));
    (leaf ? leaves : branches)++;
  }
  // All of it tree nodes, one root per generation that changed the tree.
  EXPECT_EQ(objects.size(), 19u);
  EXPECT_EQ(leaves, 11u);
  EXPECT_EQ(branches, 8u);
  EXPECT_EQ(partial, 0u);
}

}  // namespace
}  // namespace dagda::heads
