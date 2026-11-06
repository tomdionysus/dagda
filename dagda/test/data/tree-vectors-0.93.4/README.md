# Namespace tree known answers at Macha 0.93.4

Commit `9a08846` (tag 0.93.4). `src/`, `tests/` and `CMakeLists.txt` have no diff between it and
`e5b9991`, where this was built. From `../gen/tree_vectors.cpp`, linked against `libmacha_core`,
calling `build_namespace_tree`, `update_namespace_tree`, `diff_namespace_trees`,
`diff_namespace_tree_nodes`, `namespace_tree_children`, `namespace_tree_stats` and
`read_namespace_tree`. Taken 2026-11-06 on the MacBook (Intel x86_64), Release. **Not run on arm64.**
Nothing here depends on floating point, but no run has shown the Pis give the same bytes.

**Synthetic namespaces, not the live tree.** No case has been compared with a tree from a node. Nodes
here are the plain encoded bytes the node store receives: sealing and CONTROL storage are not in them.

## Checked

- The generator exits non-zero if a tree does not read back as its input, an update's root differs
  from a full build of the same namespace, or a case misses the shape it exists for.
- `../gen/check_tree_vectors.py`, without Macha's code: every id is the SHA-256 of its bytes; every
  node parses by hand with no bytes left; reachable flags and orphan counts match a walk from the
  root; each child's `items` is the count beneath it; keys under each root equal the input, in byte
  order; a boundary key never sits inside a leaf, and a leaf other than the last ends on a boundary
  or the cap. The entry boundary rule only: branch and extent grouping are shown by the bytes, and
  their rule by the cases built for them. Result: 25 cases, 7584 node records, 42 updates.
- A second run gave identical files.

## Files

`index.json` lists every case, its root, stats, and root after its updates. One `<case>.json` each:

- `limits`, `root`, `stats`, `nodes_written`, `nodes_reachable`.
- `entries`: the input in full. `key_hex` is the key. `key` is for reading only: bytes outside
  printable ASCII are written as `\u00xx` per byte, so it is lossy for UTF-8.
- `nodes`: every node written by the build: `id`, `kind` (magic), `reachable`, `count`, `level`,
  `keys_hex` (a branch's first keys, a leaf's paths), `children` (a spine node's ids and items),
  `direct_children` (`namespace_tree_children`'s answer), `bytes_hex`.
- `updates`: applied in order, each from the root before it. `changes` (`entry` null is a delete),
  `root_before`, `root_after`, `stats_after`, `differing_keys_hex`, `nodes_added`, `nodes_removed`,
  and `nodes`. A step's `nodes` lists only what was **new to the store**: a node from the build or an
  earlier step is not repeated, so keep the store across the steps of a case.

## Cases

| Case | Exercises |
|---|---|
| `empty`, `root_only`, `few` | The empty leaf; one leaf, no branch |
| `library_small`, `library_medium`, `library_large` | Library shapes, default limits; the first two with updates; `large` reaches depth 3 |
| `library_small_fanout`, `library_tiny_fanout` | Limits 4,8,4,8,2,4,8 and 2,3,2,3,1,2,3: depth 6 and 7, with updates |
| `remove_everything` | Every key removed, then one back |
| `leaf_cap`, `every_key_a_boundary` | Leaves closed by `entry_max_fanout`, and by a boundary on every key |
| `branch_cap` | Branches closed by `branch_max_fanout` |
| `packed_fallback_keyed`, `packed_fallback_extents` | A level where nothing reduces, regrouped by count |
| `extents_0`, `_1`, `_8`, `_9`, `_600` | Either side of `extent_inline_max`; an extent spine |
| `extent_chunk_cap` | Chunks closed by `extent_max_fanout`; an extent branch |
| `holes`, `shared_spine` | Holes encoded and not children; two paths on one spine |
| `provenance` | The type byte's high bit; provenance whole and each part alone |
| `field_ranges` | Each stat field at its extremes |
| `key_bytes` | Byte order against text order, a NUL, invalid UTF-8, the empty key, an 8192-byte key |

Each library update list: a value changed, a key inserted and removed, the first and last keys
removed, a key before all and after all, a run of 60 removed, a run of 200 inserted, an entry's
extents growing from inline to external and shrinking back.

## What a port must reproduce, and one behaviour to decide

The root and the reachable nodes are the known answer. **Macha also writes nodes no root reaches.**
When a spine level reduces nothing, the first pass's nodes are already in the store when the level is
regrouped by count (`build_spine_level`). `reachable: false` marks them; `packed_fallback_keyed` has
5, `library_small_fanout` 200. In production they would be CONTROL objects no root names. Whether
they ever occur at the default limits on a real namespace was not measured here: every default-limit
library case shows 0, and only cases built to force it show any at default limits.

Not covered: `apply_delta_to_namespace_tree` and `merge_tree_backed_heads`, which need metadata deltas
and heads; they belong with the state fixtures.

## Regenerate

```
cd build/macha-fixtures/gen
c++ -std=c++20 -O1 -isysroot "$(xcrun --show-sdk-path)" -I ../../../src \
    -I /opt/local/libexec/boost/1.88/include -I /opt/local/include \
    -o tree_vectors tree_vectors.cpp -L ../../../build-nobooks -lmacha_core \
    -Wl,-rpath,$PWD/../../../build-nobooks
./tree_vectors 9a08846 "<host>" <output directory>
python3 -I check_tree_vectors.py <output directory>
```
