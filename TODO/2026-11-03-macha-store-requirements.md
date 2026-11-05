# Macha's store: what Dagda must do

Status: requirements, 2026-11-03 night, re-based 2026-11-05 on Macha's `develop`
at `00861dd` (0.92.3, both live nodes, live-a and live-b). Macha's code as it
is outranks the design documents: where they differ, Macha's behaviour is the
requirement (the maintainer, 2026-11-05). What is built is in `COMPLETED.md`.
This is Dagda's specification for its first customer: the maintainer's intent is that
Dagda replaces Macha's distributed store (its DHT) by extracting how
Macha's store works and generalising it, so that Lugh and Lir can use it
too. Macha's needs come first. Lugh's and Lir's needs are generalisations
that Dagda's shape must also carry (`2026-11-03-stack.md`, "Waiting on the maintainer"
1). **Macha is ported onto Dagda with no loss of function**
(`../dannan/TODO/ACTIVE.md:87-102`).

## 0. How to read this

**Which Macha.** Macha's `develop` is `00861dd` (0.92.3). This document was
first written against `5a06d3c` (0.91.5); between them the store changed only in
0.92.3 (section 2). Macha's unmerged `feature/tree-native-namespace` was at
`8fc5a68` when written and is now at `b080c38`, green, merging after Macha's two
P0s; genesis and the final head forms settle at that merge.

**How to read a citation.**
- A citation is `file:line` under `../macha/src/` unless another root is
  named.
- Line numbers are `5a06d3c`'s, and move in the files 0.92.3 changed:
  `storage/local_store.*`, `storage/storage_pool.*`, `contract/object_ledger.hpp`,
  `cluster/distributed_store.cpp`, `cluster/storage_server.cpp`. Read those by
  symbol at `00861dd`. The rows of section 3.7 cite `00861dd`.
- A citation marked **branch:** is at `8fc5a68` and names the branch's
  direction, which this document specifies (section 2). Re-read at the merge.
- Tests are named by file and case. Lines are given where known; for
  `test_rpc_cluster.cpp`, `test_storage_metadata.cpp` and
  `test_catalogue_maintenance.cpp` they are the checkout's.

**Sources, in order:**
1. The store rows of `../dannan/TODO/2026-11-03-macha-function-inventory.md:174-199`, cited as "inventory".
2. `../dannan/TODO/2026-11-03-macha-extraction.md`, cited as "extraction": its file table at 63-101 and seams 6 to 9 at 390-534.
3. The design in `../dannan/TODO/2026-11-02-dannan-plan.md:100-171` and `:205-221`, cited as "plan".
4. `../dannan/TODO/ACTIVE.md` steps 7 and 8 (`:690-730`) and its Decisions.
5. Macha's own `docs/`, `VALIDATION.md`, `TODO/ACTIVE.md`, `TODO/BACKLOG.md`, `TODO/2026-10-09-replication-by-diff.md` and its code.

**Scope rulings from the Macha session (the owner of `../macha`), 2026-11-03.**

1. **The tree form only.** The operator ruled that there is no legacy
   before a popular public release.
   - Specified: the tree-backed snapshot forms SM14, SM16, SM17 and SM19
     (all written by `encode_snapshot_v14`), the deltas DLT5 to DLT11, the
     Merkle tree over keys, and the hashes.
   - Not specified:
     - inline-namespace snapshots (SM5 to SM13, SM15, SM18, `encode_snapshot` v7 and v8);
     - DLT1 to DLT4;
     - the legacy metadata authority: write floor 0, `metadata_voters`, the `cas`, `seed` and `install_committed_delta` write API, and the floor-0 upgrade in `maybe_reconfigure`;
     - whole-snapshot `mutate()`;
     - the offline namespace migration tool.
2. ~~**No layout bump and no protocol bump** are planned on Macha's side:
   layout v18, protocol 23.~~ Superseded: layout v19 by `dagda-migrate` (the maintainer,
   2026-11-04 night), and wire v1 by the transport cutover.
3. **Do not reproduce** the defects listed in section 7.

## 1. What Macha's store is, in one page

**Objects and local stores.**
- Macha stores immutable objects named by the SHA-256 of their plaintext
  (`types.hpp:23-28`, `crypto.cpp:62-66`).
- Each object is sealed with AES-256-GCM under one storage key derived from
  the cluster key (`crypto.cpp:103-154`). The object id is the associated
  data (`storage/local_store.cpp:830`, `:1091`).
- A `LocalStore` (`storage/local_store.{hpp,cpp}`) keeps an object in one
  of two forms. Above `packing.threshold` it is a loose file
  (`objects/xx/yy/<hex>.obj`). At or below it, the object is a record
  appended to a pack (`packs/pack-<seq>.pack`).
- Packs are rebuilt by scan at start, their torn tails truncated, and
  compacted copy-on-write. A removal in a pack is a tombstone record
  (`local_store.cpp:921-1057`, `:1671-1863`, `:1413-1430`).
- `has(id)` is a cheap presence answer from an index or ledger, never a
  decrypt (`contract/object_store.hpp:45-52`).

**The pool and the two classes.**
- DATA is a `StoragePool` (`storage/storage_pool.{hpp,cpp}`) of several
  backends. Each backend has its own `limit` and `reserve_free`. An object
  ranks the backends with the cluster's capacity placement
  (`storage_pool.cpp:365-398`). A refused put falls through to the next
  backend.
- CONTROL is one separate `LocalStore` with its own quota
  (`cluster/local_state.cpp:134-142`). DATA exhaustion cannot block a
  CONTROL write, and CONTROL never enters the DATA credit arbiter
  (`cluster/data_work.hpp:23-24`, `:42-43`).
- A third, ephemeral store is the persistent block cache. It never counts
  toward durability (`storage/persistent_cache.hpp:16-19`).

**Durability domains and tickets.**
- Backends on one device share a `DurabilityDomain`
  (`storage/durability_domain.{hpp,cpp}`, `storage_pool.cpp:92-107`).
- The domain numbers each completed mutation with a generation
  (`durability_domain.cpp:91-104`) and makes a batch of generations durable
  with one `syncfs`, or with an fsync per file where `syncfs` is not
  available (`:159-205`).
- A ticket is `{process epoch, domain, generation, backend incarnation}`
  (`storage_pool.hpp:44-50`, `cluster/storage_server.cpp:247-252`), so a
  ticket from an old process or a re-mounted disk satisfies nothing.
- `put` is strict. `put_deferred` plus the barrier batch the sync
  (`docs/durability.md:21-43`).

**The distributed store and placement.**
- `DistributedStore` (`cluster/distributed_store.{hpp,cpp}`) places DATA by
  capacity-aware rendezvous over a 2^32 shard space, spread across failure
  domains (`cluster/placement.cpp:327-403`). Only nodes that host extents
  are candidates (`distributed_store.cpp:114-134`).
- A write seeks `write_copies` durable copies, local first. It spills past a
  peer stalled beyond `write_stall`, returns on what landed, and leaves the
  rest to prompt replication and repair (`distributed_store.cpp:201-486`,
  `:1211-1316`).
- A read goes local, then cache, then remote owners ordered by a latency
  selector (`:1694-1808`, `cluster/replica_selector.cpp`).
- CONTROL objects are offered to every node and claimed on
  `metadata_write_copies` nodes (`:1064-1134`). They are fetched from any
  peer at speculative priority without DATA credit
  (`cluster/control_objects.{hpp,cpp}`).
- The serving side is `cluster/storage_server.cpp`.

**The trie and diff repair.**
- Each DATA backend keeps a `held` trie: a radix-256 Merkle trie over the
  ids, whose root depends only on the ids (`ledger/object_trie.{hpp,cpp}`,
  `ledger/held_ledger.*`).
- A node's holdings are the union of its disks' tries
  (`ledger/trie_union.*`). Their identity, `{root, removals, complete}`, is
  the only signal that holdings changed (`contract/object_store.hpp:15-39`).
- Two nodes compare holdings by `trie_diff`: rounds of prefix questions
  against one pinned snapshot (`ledger/trie_diff.*`,
  `service/availability_service.cpp:333-374`).
- The survey joins the diff with what is referenced to give `missing_here`,
  `peer_lacks` and `unavailable` (`ledger/availability.cpp:66-110`).
- Repair pushes from the diff's lists, pulls what is missing, and is paced,
  never stopped (`distributed_store.cpp:2421-3087`,
  `service/maintenance.cpp:309-313`).

**Claims and retention.**
- A claim is a durable promise to keep an object, recorded as an
  observed-remove set of dots `(origin, sequence)` per object and class
  (`contract/claim_store.hpp:23-92`, `storage/retention.{hpp,cpp}`).
- Release removes only dots that the releasing head's clock dominates
  (`retention.cpp:292-322`). So a claim from a branch this node has not
  seen survives a partition.
- Claims journal to a sealed WAL (`retention/claims.log`) and checkpoint
  into two tries (`ledger-data`, `ledger-control`).

**The horizons and GC sweeps.**
- The maintenance pass builds two reachability horizons
  (`contract/horizon.hpp:65-118`):
  - the inventory, at the converged generation;
  - the release horizon, at the sole accepted head.
- Three destructive gates decide whether tombstone collection, CONTROL GC
  and the DATA sweep may run (`contract/gates.{hpp,cpp}`).
- The two sweeps (`service/maintenance.cpp:938-1071`) remove only objects
  that this node has itself seen unreferenced and unclaimed for
  `garbage_grace` (30 days), on its own clock
  (`storage/unreferenced_since.*`).
- A copy with any local claim is untouchable (`storage_pool.cpp:1033-1050`).
- Deletion is lazily durable (`local_store.cpp:1446-1461`).

**The metadata commit DAG.**
- The namespace is a DAG of immutable commits, each a full snapshot or a
  delta, with one primary parent and optional merge parents
  (`metadata/metadata.hpp:142-266`, `metadata/metadata_manager.{hpp,cpp}`).
- A commit is accepted when the committing node durably holds it and its
  acceptance certificate. A merge commit first seeks `metadata_write_copies`
  holders (`metadata_manager.cpp:831-920`).
- Accepted heads are a set. Ancestors collapse
  (`metadata.cpp:3248-3301`). Divergent heads are folded pairwise by the
  merge in `metadata/metadata_merge.cpp` with no common ancestor
  (`metadata_manager.cpp:1051-1348`). Concurrent changes become durable
  conflict records.
- Mutations carry dots, so retries are idempotent. History is truncated out
  of the hot path at 256 records or 64 MiB (`metadata.cpp` develop
  4961-4994).
- `MetadataServer` serves the history, heads and acceptance
  (`metadata/metadata_server.cpp:109-197`).

**The Merkle tree over keys.**
- The namespace is a history-independent, content-addressed tree keyed by
  path (`metadata/namespace_tree.{hpp,cpp}`). The record carries only its
  root. Its nodes are CONTROL objects (`metadata/namespace_control_store.*`).
- A change rewrites one leaf and the branches above it. Two trees are diffed
  and merged by what differs.

**Catalogue reachability.**
- The media catalogue is CONTROL objects (a manifest and 64 shards) and
  DATA (artwork and media indexes) named by `catalogue_root` in the head.
- The ledger asks the catalogue and the filesystem directly for what they
  reference (`ledger/node_horizon_builder.cpp:86-113`). This is the coupling
  that seam 7 cuts.

**The torrent-request CRDT.**
- `torrent_requests` in the head is a claimed collection
  (`torrent/torrent_request.{hpp,cpp}`):
  - field groups merged last-writer-wins, with a dominating cancel;
  - claims ordered by (epoch, earlier, lower node id);
  - a max-wins tombstone kept 7 days;
  - a duplicate rule by info hash.
- Takeover after absence happens in `torrent/torrent_coordinator.cpp`.
  Seam 6 makes it Dagda's lease and claimed-collection template.

## 2. What changed in the store since the baseline (`6c7ec64`)

`git -C ../macha log --oneline 6c7ec64..develop` shows these commits.

| Commit | Release | What it changes in or near the store |
|---|---|---|
| `4bf0b26`, `cbf986d`, `c71370e` | - | Transport proofs (21 cases in `tests/test_rpc_cluster.cpp`) and the extraction TODO. No store code. |
| `d61eb02` | 0.91.3 | A reply the transport cannot queue is answered 503 `transport_busy`, never dropped (`cluster/net.cpp`). This is Dannan's. It matters to the store because every store RPC now gets an answer. |
| `353e600` | 0.91.4 | Documentation and the example config. |
| `0342ea3` | 0.91.5 | `DistributedStore::put` and `put_deferred` (the id-returning forms) throw `StorageUnavailable` instead of `runtime_error` (`cluster/distributed_store.hpp:26-31`, `distributed_store.cpp:146-161`, `:178-188`). The FS maps it to `FsStorageUnavailable` (EIO) (`filesystem/filesystem.hpp:45-49`). Ingest parks a job as blocked `storage_unavailable` and retries (`acquisition/ingest.cpp:1417`). |

Nothing changed in `src/storage`, `src/ledger`, `src/catalogue`,
`src/torrent` or `src/contract` on `develop`. The metadata files did not
change on `develop` either.

**On the branch** `feature/tree-native-namespace`, not merged:

- **`623c202`**
  - New namespaces are founded as a tree from generation 2:
    `discover_or_form` detaches the tree, then calls `encode_snapshot_v14`
    (branch: `metadata_manager.cpp:1499-1508`).
  - `MetadataManager` takes and requires its `DistributedStore` for
    namespace nodes. The null-store branches are removed.
  - A fast path makes a repeat acceptance a lookup (`certificate_adds_nothing`,
    branch: `metadata.cpp:4283-4325`).
- **`5f6101c`**
  - Removes the namespace migration tool (`tools/namespace_migrate.cpp`).
  - Removes `install_migrated_head` (develop: `metadata.hpp:715-722`,
    `metadata.cpp:4778-4847`) and `plan_namespace_migration` (develop:
    `namespace_tree.hpp:367-382`).
  - `tests/test_namespace_migration.cpp` becomes
    `tests/test_namespace_service.cpp`.
- **`8fc5a68`**
  - Removes `MetadataView::mutate`, `MetadataManager::mutate` and
    `mutate_impl`.
  - Every mutation is an exact delta through `mutate_delta`
    (branch: `metadata_manager.cpp:1682-1953`), which refuses a head without
    a tree (branch: `:1745-1746`).
- **Not done yet:**
  - Genesis is still an inline record decoded at founding
    (`metadata.cpp:2290-2301`; branch: `metadata_manager.cpp:1477-1481`).
    The direction is a constant hash, authoritative by definition and never
    decoded.
  - `maybe_reconfigure` keeps an inline branch (branch: `:1373-1374`).
  - `read_group` still merges mixed inline and tree pairs (branch:
    `:1214-1226`).
- **No encoding changed:** no SM or DLT magic, encoder or decoder.

Dagda specifies the branch's direction: tree only, exact deltas only, the
store required, and genesis a constant.

**Since `5a06d3c`, re-based 2026-11-05.** Nothing in the store changed from
`5a06d3c` to `5776392` (0.92.0 to 0.92.2 are books). 0.92.3 (`8e0308e`, merged
at `00861dd`) changed it, fixing what the port found; its rows are section 3.7.

## 3. Requirements by Dagda subsystem

Each row is a behaviour Dagda must provide. **Where** is Macha's location,
read for what it does and never copied. **Proof** is Macha's existing proof:
a test case, a `VALIDATION.md` section, or a row of the crash matrix in
`docs/durability.md:142-162`. That proof becomes the gate of the port step,
and the specification of the `dagda_tests` case written before the code.
**Disposition** is one of three:
- **Generic**: the behaviour is generic as it stands.
- **FS**: FS-specific. It stays in Macha, behind an owner-supplied codec,
  merge or provider.
- **Generalise**: needs generalising, and the cell says how, per the design.

### 3.1 `domain/`

| # | Behaviour | Where | Proof | Disposition |
|---|---|---|---|---|
| D1 | One storage key per cluster: HKDF-SHA256 with info `macha/storage/v1` and no salt, over a master that is the SHA-256 of the cluster key file with trailing CR/LF removed. Given to the DATA pool, the control store, the cache, the retention store and the reference counts. | `crypto.cpp:18-20`, `:103-119`; `cluster/local_state.cpp:90`, `:141`, `:148`, `:153`, `:156` | `foundations/test_codec_and_crypto` (seal, open, a flipped bit refused) | **Generalise**: `storage[domain]` (plan 106). Macha is one domain with every node a member (plan 114). The key comes from the domain, held in Dannan's `SecureBytes`. Byte compatibility needs the Macha domain's key derived exactly so. |
| D2 | Each object sealed AES-256-GCM, random 12-byte nonce, 16-byte tag, AAD = the 32-byte object id; on open the size and `object_id(plain) == id` are checked. | `crypto.cpp:121-154`; `local_store.cpp:830`, `:910-911`, `:1091`, `:1257-1258`, `:1337-1338` | `storage_v18/test_packs_store_compact_and_survive_restart` (`test_storage_v18.cpp:109`); `test_local_store` (`test_storage_metadata.cpp:3602`) | **Generalise**: bind the domain into the AAD as well as the id (stack document, Lugh requirement a). Today a ciphertext could move between DATA and CONTROL, or between backends, undetected. Adding the domain changes the bytes, so this is a format decision (section 4.4). |
| D3 | Roles per domain: member, blind, none. A blind node verifies ciphertext only. | none in Macha | none | **Generalise**: new (plan 106-124). Specified, not exercised in v1 (`../dannan/TODO/ACTIVE.md:694-695`). The pack header's SHA-256 and the GCM tag can serve as the ciphertext digest (section 4.1). The binding of that digest to the plaintext id is new. |
| D4 | Node-local domains: a key only this node has, under Dannan's node key custody, never wrapped to another node. | none in Macha | none | **Generalise**: new, a Lugh requirement (stack document, "Node-local spaces"). Macha does not need it. |

### 3.2 `store/`

| # | Behaviour | Where | Proof | Disposition |
|---|---|---|---|---|
| S1 | Objects are immutable bytes named by the SHA-256 of their plaintext. `put` refuses a mismatched id ("object hash mismatch"). | `types.hpp:23-28`; `crypto.cpp:62-66`; `local_store.cpp:1188` | `test_local_store` | **Generic** |
| S2 | Loose form above `packing.threshold`: `objects/xx/yy/<hex>.obj`, written to a temp file with `O_EXCL`, write-behind, then rename. No fsync there: durability comes from the domain. | `local_store.cpp:277-303`, `:627-630`, `:1063`, `:1092-1096` | VALIDATION "Physical representation"; `test_storage_v18.cpp:109` | **Generic** (format in section 4.1) |
| S3 | Packed form at or below the threshold. One record per put, remove or touch, each independently authenticated. Threshold and target are both zero or both set, and target is at least the threshold. Defaults are 1 MiB and 64 MiB. | `local_store.cpp:139-185`, `:250-267`, `:442-445`, `:1304`; `docs/storage.md:107-123` | `test_packs_store_compact_and_survive_restart` | **Generic** |
| S4 | The pack index is rebuilt by scanning records at every open, in pack order, counting dead bytes. Leftover temp and compaction files are deleted. | `local_store.cpp:921-1057` | VALIDATION "Physical representation"; `test_storage_v18.cpp:109` | **Generic** |
| S5 | A torn final record is truncated. A corrupt middle region is skipped as dead bytes and counted (`pack_recovery_skipped_regions`); its objects count as absent and the backend stays online. A failed append is rolled back to the last boundary. | `local_store.cpp:197-227`, `:857-869`, `:961-1017` | `storage_v18/test_pack_recovery_settles_each_kind_of_damage` (`test_storage_v18.cpp:217`); crash matrix "Pack tail torn" | **Generic** |
| S6 | Compaction is copy-on-write, one victim per call (the pack with the most dead bytes). It preserves `reserve_free`, writes and fsyncs the replacement, renames, syncs the directory, switches the index, waits for reader leases, then unlinks the victim. | `local_store.cpp:1671-1863`, `:886-918` | `test_storage_v18.cpp:109`; crash matrix "Pack compaction after replacement install but before old deletion" | **Generic** |
| S7 | Removing a packed object appends a tombstone. Re-putting identical bytes, or a touch, appends a touch record carrying the GC age. Removing a loose object unlinks it. | `local_store.cpp:1201-1224`, `:1413-1462`, `:1626-1643` | `test_storage_v18.cpp:109` | **Generic** |
| S8 | Byte accounting: A/B checkpoint slots (`.macha.accounting`), clean or dirty. A dirty store reconciles the physical store before admission is trusted. Store lock `.macha.lock` by `flock`. | `local_store.cpp:36-41`, `:96-137`, `:411-423`, `:632-662` | crash matrix "DATA accounting dirty"; `docs/durability.md:123-127` | **Generic** |
| S9 | Admission: `used + reserved + need <= limit`, and the filesystem's free space must stay above `reserve_free`. | `local_store.cpp:702-722` | VALIDATION "Admission and capacity"; `storage_v18/test_admission_respects_reserve_and_capacity` (`test_storage_v18.cpp:163`) | **Generic** |
| S10 | Several DATA backends ranked per object by capacity placement over the backends. A refused put falls through, a throwing backend is deactivated, and rebalance converges each object onto its preferred backend. | `storage_pool.cpp:365-476`, `:569-633`, `:846-967` | VALIDATION "Admission and capacity"; `test_storage_v18.cpp:163`; `test_invariants.cpp:2259`, `:2281` | **Generic**. Uses Dannan's placement function (`placement::Scheme`) with Dagda's own salts. |
| S11 | Two classes with separate quotas. DATA exhaustion cannot block a CONTROL write. CONTROL never enters the DATA arbiter. I/O pressure is per pool, not on the control store. | `local_state.cpp:134-142`; `cluster/data_work.hpp:23-24`, `:42-43`, `:256-257`; `storage_pool.hpp:115-117` | VALIDATION "Class separation"; `storage_v18/test_metadata_control_store_is_independent_of_data_quota` (`:539`); `storage_v18/test_a_full_preferred_node_spills_data_but_not_control` (`:736`); `test_control_cannot_enter_the_data_arbiter_at_all` (`test_io_pressure.cpp:370`) | **Generic**. Note that the control store has no `reserve_free` (`local_state.cpp:134`); section 7 lists the missing disk control reserve. |
| S12 | Backend identity: a backend marker and a mirrored state record. A disk belonging to another node, the wrong disk, or a non-empty unmarked disk is refused; an empty one is adopted. Backend incarnation is a counter reset on deactivate. | `storage_pool.cpp:22-23`, `:35-37`, `:66`, `:109-113`, `:134-135`, `:148-294` | VALIDATION "Refusing unversioned storage"; `storage_v18/test_unversioned_state_is_refused` (`:574`) | **Generic**. The marker text is a format question (section 4). |
| S13 | Durability domain per device, so backends on one device share a barrier. A generation is issued only after the syscalls complete. `immediate` urgency wakes the worker; `batchable` waits for the 500 ms window. One `syncfs` per cut, or an fsync per file where there is no `syncfs`. | `durability_domain.hpp:65-77`; `durability_domain.cpp:61-62`, `:91-131`, `:159-285`; `storage_pool.cpp:92-107` | `test_invariants.cpp:2386`, `:2401`, `:2453`, `:2488`, `:2526`, `:2596`; crash matrix rows 3 and 4 | **Generic** |
| S14 | A failed barrier is sticky: every later generation and wait rethrows, the domain is not reused, and the backend is deactivated. A ticket whose backend is gone throws. | `durability_domain.cpp:94-99`, `:112-113`, `:261-265`; `storage_pool.cpp:504-510` | `test_invariants.cpp:2725`, `:2777` | **Generic**. "Lost" in the Tickets noun (plan 220). |
| S15 | Tickets are `{epoch, domain, generation, backend incarnation}`. `put_deferred` re-checks the incarnation. The barrier matches incarnation and domain. A barrier from an old process epoch gets 409 `epoch_changed`, or re-derived tokens when it carries probe ids. | `storage_pool.hpp:44-50`; `storage_pool.cpp:458-525`, `:635-665`; `storage_server.cpp:247-253`, `:273-326`; `distributed_store.cpp:504-780` | `..._tokens_name_physical_domain_and_backend_incarnation` (`test_invariants.cpp:2824`); `test_deferred_object_barrier_rejects_stale_process_epoch` (`:3045`); `storage_v18/test_durability_barrier_across_peer_restarts` (`test_storage_v18.cpp:969`) | **Generic**. The epoch is Dannan's `durability_epoch`, a fresh random id per process (`cluster/node_identity.cpp:46`). |
| S16 | `LocalStore::put` is strict: it returns after its generation is durable, with immediate urgency. A re-put of an existing loose object waits only for its provisional generation if that is not yet durable. | `local_store.cpp:1279-1288`, `:1312-1315`, `:1465-1469` | `test_invariants.cpp:2949`, `:3100`, `:3179` | **Generic** |
| S17 | Deletion is lazily durable: a remove registers a generation and never waits on it. A lost unlink leaves garbage. | `local_store.cpp:1446-1447`, `:1459-1461`; `docs/durability.md:129-140` | `test_authoritative_delete_is_lazy_durability` (`test_invariants.cpp:2690`); crash matrix "GC unlink lost" | **Generic** |
| S18 | `has(id)` is cheap presence: once the held ledger is seeded it answers from the ledger or the pack index; before warm-up it stats under the object's lock. It never decrypts and never reports an object a put is still writing. | `contract/object_store.hpp:41-52`; `local_store.cpp:1356-1401`, `:1989-2042`; `storage/presence_index.hpp:13-63` | `storage_v18/test_has_is_a_cheap_presence_check_not_a_decrypt` (inventory); `test_presence.cpp:95-371`; `test_baseline_data_store_has` (`test_baselines.cpp:70`) | **Generic** |
| S19 | Persistent block cache: an ephemeral `LocalStore` with no domain, no barriers and no accounting, LRU by `max_blocks`, a corrupt entry removed. It never counts toward durability. | `persistent_cache.cpp:70-73`, `:166-277`; `persistent_cache.hpp:16-19`; `local_store.cpp:451-473` | `test_persistent_cache_is_explicitly_ephemeral` (`test_invariants.cpp:2986`); `test_storage_pool_and_persistent_cache` (`test_storage_metadata.cpp:3699`); crash matrix "Cache missing/corrupt" | **Generic**. The cache's metadata snapshot (`current.meta`, `persistent_cache.cpp:325-390`) couples it to `MetadataRecord`; it becomes a heads concern (H25). |
| S20 | DATA credit arbiter: byte credit with a viewer reserve. Lower classes are refused under I/O pressure once `min_background` leases are active. `background_concurrency` caps them. A wait with no release anywhere fails at the no-progress deadline. | `cluster/data_work.hpp:84-416`; `cluster/node_resources.cpp:9-18` | `test_io_pressure.cpp:25-390`; `test_storage_data_credit_reserves_viewer_headroom_and_control` (`test_rpc_cluster.cpp:2825`) | **Generalise**: "viewer" becomes Dannan's `foreground` class. The credit is Dagda's I/O budget, handed out by class through Dannan's `Budget` noun (plan 179). |
| S21 | I/O pressure from service time: expected time 25 ms + 120 ms/MiB, EWMA 1/16, pressured above 300% and released below 150%, tripped at once by a single sample above 1000%. | `storage/io_pressure.hpp:35-46`, `:107-116`, `:163-199`; `storage_pool.cpp:418`, `:452`, `:588` | `test_io_pressure.cpp` | **Generic** |
| S22 | Distributed put. Candidates are ranked with the local node first. `target = min(replicas, n)`, `need = max(1, min(write_copies, n))`. A peer idle past `write_stall` is spilled to the next candidate. The put returns once `need` copies landed, or on what landed when nothing more can be had. Below target, prompt replication is queued. A put is never refused for lack of peers. | `distributed_store.cpp:201-486` | VALIDATION "Placement and the write floor"; `test_store_put_publishes_at_the_floor_and_copies_promptly`, `test_store_put_replaces_a_replica_that_cannot_be_launched`, `test_store_put_to_a_silent_replica_returns_on_the_local_copy` (`test_rpc_cluster.cpp:448`, `:505`, `:585`) | **Generalise**: return `Evidence{holders, role and tier per holder, ticket}` (plan 130, 216) instead of `bool` or an id. Macha's facade keeps its `bool` and `ObjectId` forms over it. |
| S23 | The id-returning `put` and `put_deferred` throw `StorageUnavailable` when nothing landed (0.91.5). The `bool` forms return false. | `distributed_store.hpp:26-31`; `distributed_store.cpp:146-199` | none (CHANGELOG only); owed | **Generic**: an Evidence with no holder. The throw is Macha's facade. A case is owed before the port. |
| S24 | `put_deferred` plus `durability_barrier` across nodes: replicas are coalesced by (node, epoch, domain, instance); a stale epoch is re-derived by probe; definitive absences are reported as unsatisfiable. | `distributed_store.hpp:118-139`; `distributed_store.cpp:253-281`, `:504-780` | `test_storage_v18.cpp:969`; crash matrix rows 3 and 4 | **Generic**: the Tickets noun (`wait`, `status`: durable, pending or lost). |
| S25 | DATA placement: capacity-aware rendezvous over the shard space, failure domains first, deterministic fallbacks, R=1 by weighted rendezvous. Only nodes hosting extents are candidates. Peers without room for one extent are skipped by repair. | `distributed_store.cpp:114-134`, `:1285-1288`, `:2534-2537`; `placement.cpp:215-403` | VALIDATION "Placement and the write floor", "Admission and capacity" | **Generalise**: the candidate filter asks the capability `store.data` (extraction seam 5) and later the domain role (plan 118). The placement salts and the capacity advert are Dagda's (`ACTIVE.md`, "Dannan, as Dagda finds it"). |
| S26 | Prompt replication: one worker pushes to the first ranked owner that lacks the object and has room, as speculative work, until two holders exist. Retries back off 30, 60, 120 and 240 s, at most 5 attempts and 4096 queued. | `distributed_store.cpp:1211-1316` | `test_rpc_cluster.cpp:448` | **Generic** |
| S27 | `get`: local, then the cache (promoted if this node owns the object), then remote owners and the other ranked nodes, ordered by the replica selector. Concurrent readers share one fetch, with priority promotion. `not_found` only when every candidate answered 404. | `distributed_store.cpp:1436-1808`; `replica_selector.cpp:11-75` | `test_store_concurrent_readers_share_one_fetch_and_buffer` (`test_rpc_cluster.cpp:627`) | **Generalise**: return the serving node (plan 130, 216). Macha does not expose it today. |
| S28 | `put_here` and `put_deferred_here`: local first, prompt replication after. With no extents held here they fall back to `put`. | `distributed_store.cpp:1011-1047` | `test_store_put_is_loader_activity_not_viewer_activity` (`:430`) | **Generic** |
| S29 | CONTROL objects: made local, offered to every active node (`have_control_objects`, then `put_control_object`), claimed on `metadata_write_copies` nodes ordered local first then nearest. Fetched from any peer at speculative priority, verified by id, without DATA credit. | `distributed_store.cpp:1064-1134`, `:2142-2218`; `control_objects.hpp:21-40`; `control_objects.cpp:15-60`; `placement.hpp:11-16` | `test_store_control_objects_take_no_data_credit_and_commit_alone` (`:1198`); `test_commit_replicas_ordered_local_then_nearest` (`:4238`); crash matrix "Catalogue control object missing from one replica" | **Generalise**: Macha offers every CONTROL object to every node, because every node holds its one space. With `replicas: N` (Decisions), a space's CONTROL objects go to that space's holders (H20). |
| S30 | The serving side: `have_object` (full read, decrypt and hash), `get_object` (404, or 503 when a backend is offline), `put_object` (413 too large, 507 full), `delete_object` (409 if claimed), the control-object pair, `retain_objects`, `have_objects` and `have_control_objects` (index lookups), and `have_valid_objects` (at most 16 ids, answering intact, absent or cannot say). | `storage_server.cpp:74-392`; `cluster/net.hpp:140-142` | `test_rpc_cluster.cpp` store cases | **Generic**. The message types are Dagda's registration on Dannan (section 4.2). |
| S31 | Local state recovery plane by plane: `data_storage`, `control_storage`, `cache`, `retention`, `metadata`. DATA on one thread; control, then cache, retention and metadata on another. Cancellable between stages. | `local_state.hpp:38-95`; `local_state.cpp:22-27`, `:57-184` | `test_service_root.cpp`; `test_node_services.cpp` | **Generalise**: `RecoveryProgress` is Dannan's mechanism; Dagda names its planes (extraction seam 2). |
| S32 | Fresh genesis: a non-empty state directory or backend without the current layout marker is refused, never adopted. | `node_identity.cpp:16-46`; `storage_pool.cpp:212-219`; `docs/storage.md:178-182` | VALIDATION "Refusing unversioned storage"; `test_unversioned_state_is_refused` | **Generic**. The marker strings are a format decision (section 4.4). |
| S33 | Reload: `write_copies`, cache and backends. | `cluster/cluster.cpp:1144-1152`; `service_config.cpp:23-25` | `test_replication_policy_change_on_restart` (`:4677`) | **Generalise**: every store setting reloads (section 7). |

### 3.3 `ledger/`

| # | Behaviour | Where | Proof | Disposition |
|---|---|---|---|---|
| L1 | `ObjectTrie`: radix-256 over the id bytes. Leaves hold at most 128 records with values up to 16 KiB. The shape depends only on the ids. Leaf hash `sha256('L' ‖ (id ‖ bytes(value))*)`; interior hash `sha256('I' ‖ (slot ‖ hash ‖ u64 count)*)` over non-empty slots; the empty trie hashes to zero. | `ledger/object_trie.hpp:21-79`; `object_trie.cpp:33-57`, `:208-238` | `test_object_ledger.cpp:253` (the hash depends only on the records), `:341`, `:433` | **Generic**. The plan calls it "the single most reusable piece" (plan 138); it is exposed as is. |
| L2 | Trie persistence: a sealed journal fsynced before apply, a copy-on-write checkpoint at 1 MiB, a rewrite when superseded bytes exceed live bytes, a 64 MiB node cache. Frozen snapshots readable from any thread. | `object_trie.hpp:31-41`, `:71-77`; `object_trie.cpp:21-22`, `:480-483` | `test_object_ledger.cpp:52`, `:73`, `:103`, `:142`, `:768` | **Generic** |
| L3 | Held ledger per DATA backend: changes queued under the store's lock and journalled before install; `held()` answers only once seeded; `removals` counts every not-held install. Packed objects are in it. | `held_ledger.hpp:15-86`; `held_ledger.cpp:79-131`; `local_store.cpp:521-525` | `test_the_ledger_lists_what_the_packs_hold` (`test_object_ledger.cpp:1168`), `:1060`, `:1112`, `:1217` | **Generic** |
| L4 | The holdings identity `{root, removals, complete}`: the only signal that holdings changed. `removals` is monotonic across backend changes. `complete` is false while any online disk seeds. | `contract/object_store.hpp:15-39`; `storage_pool.cpp:69-71`, `:683-726` | `test_object_ledger.cpp:1274`; replication-by-diff stage 0 | **Generic** |
| L5 | `TrieUnion`: a node's held trie is the union of its online, seeded disks' tries, with the summaries one trie would have. Only subtrees that disks hold differently are recomputed. | `ledger/trie_union.hpp:14-51`; `trie_union.cpp` | `test_object_ledger.cpp:920` | **Generic** |
| L6 | `trie_diff`: compare roots, then descend one level a round over every differing prefix; read records at or below 128; compare by sorted merge. `records_not_held` is a paged merge join. | `ledger/trie_diff.cpp:13-147` | `test_object_ledger.cpp:484`, `:510`, `:551` (one change among 20,000 costs two rounds and two leaves) | **Generic** |
| L7 | `trie_diff` over the wire. A root question pins a snapshot (at most 4 pins, each dropped after 2 minutes unused). Later questions name the pinned hash. The server answers 503 `holdings_seeding` while seeding and 410 `snapshot_gone` for an unknown snapshot. Sent only to peers new enough. | `trie_diff.hpp:77-106`; `trie_diff.cpp:176-303`; `availability_service.hpp:172-180`; `availability_service.cpp:28-111`, `:333-374` | `test_availability.cpp:264`, `:322`, `:360`, `:454`, `:714`, `:792`, `:1052` | **Generalise**: the trie selector byte has one value today (`held_data`). It names any trie: held per class, claimed, or an owner's holdings (plan 138: "a KV namespace or a mailbox can diff its own holdings"). |
| L8 | The survey by diff: per peer, lacks = held here, not there, and referenced; `unavailable = missing_here − held elsewhere`, or `unknown` if any peer failed; `keep_unavailable` carries the previous answer forward. `missing_here = inventory.not_held(held)`. Re-asked on a topology change, a peer set change, a peer's storage shrinking, or a retry; paced at a duty share of 20; gains alone ask nobody. | `ledger/availability.cpp:51-110`; `availability_service.cpp:376-585` | `test_availability.cpp`; inventory row "The trie, the diff ..." | **Generic** for the mechanism. **FS** for the path table and `file_facts` (`availability_service.cpp:9`, `:122-244`). `ledger/availability` stays Macha's (extraction table). |
| L9 | Repair push. The source is the diff's push list when `peer_lacks` is complete and of the same generation, otherwise a cursor walk. Objects enough owners hold are settled with no probe (`known_present`). Unknown presence is probed with `have_valid_objects` batches. At most 8 objects and 2 extents of bytes a batch. Credit is spent on bytes sent. The local copy is dropped once enough owners hold it and it is unclaimed. | `distributed_store.cpp:2540-2916`; `maintenance.cpp:754-783` | `test_rpc_cluster.cpp:741`-`:1090`; `test_repair_step_is_bounded_and_yields` (`test_storage_metadata.cpp:4442`) | **Generic**. The cursor walk and the `have_*` probes are left to retire (replication-by-diff stage 5). Dagda may ship with the diff path only if every peer runs Dagda (open question 9). |
| L10 | Repair pull. The source is the pull list (`missing_here`) or the live set. Skip objects not owned or already held. Promote from cache. Skip with no RPC when `unavailable`. Otherwise fetch on the speculative frame. `try_later` backs off exponentially from 1 s to 10 min. | `distributed_store.cpp:2922-3070`; `distributed_store.hpp:197-209` | `test_rpc_cluster.cpp` repair cases | **Generic** |
| L11 | Repair is paced, never stopped. It yields at operation boundaries on the repair share. A peer's foreground viewers pace it like local ones. The operation budget is 16. | `maintenance.cpp:309-313`, `:738`, `:790-795`; `distributed_store.cpp:2421-2440` | `rpc_cluster/test_repair_is_paced_not_stopped_while_a_peer_serves_viewers`; `test_repair_is_paced_not_stopped_while_higher_classes_stay_busy` (`:1325`) | **Generic** |
| L12 | Claims are an observed-remove set per object and class. Add is ignored if `removed[origin] >= seq`. Release raises `removed[origin]` and erases an add only if the observed clock dominates it. Claims from unseen origins or sequences survive. | `contract/claim_store.hpp:23-92`; `storage/retention.hpp:19-31`; `retention.cpp:70-112`, `:270-322` | `test_retention_claims_are_causal_durable_and_observed_remove` (`test_storage_metadata.cpp:79`), `:175`, `:205`; `test_absent_nodes.cpp`; `users/test_a_node_that_was_down_learns_a_deletion_not_a_resurrection` | **Generic**: the Claims noun (plan 134, 217). |
| L13 | Release in bounded slices from a cursor, one journal frame a slice. `prune_unclaimed` forgets tombstone-only records once the object is absent, rechecking under the lock. Checkpoint at 8192 changed records or a 4 MiB journal. | `retention.cpp:33-34`, `:337-351`, `:544-646` | `test_retention_ledger_forwards_claims_releases_prunes_and_compacts` (`test_claim_walk.cpp:174`) | **Generic** |
| L14 | Retention ledger: forwards claims, refuses an incomplete release horizon and keeps the previous one, releases against the horizon's clock. | `ledger/retention_ledger.cpp:38-54` | `test_retention_ledger_publishes_horizons_and_refuses_an_incomplete_release` (`test_claim_walk.cpp:133`) | **Generic** |
| L15 | Reference counts: two tries (extents and tree nodes), a count per reference at a saved root, moved from root to root by a tree diff. | `ledger/reference_counts.hpp:13-58` | `test_namespace_tree.cpp:1182` | **FS** today: it counts a namespace's references. **Generalise**: the provider may hand Dagda a counted view (H22), so the counting machinery is Dagda's and what is counted is the owner's. |
| L16 | `ReferencedSets` is a value type per class: an optional frozen trie view plus a sorted, unique "others" list. API: `referenced`, `lookup`, `next`, `all`, `not_held`, `size`. | `contract/horizon.hpp:26-63`; `horizon.cpp:22-35` | `test_referenced_sets_are_sorted_unique_and_per_class` (`test_ledger_gates.cpp:33`) | **Generic**. The name clashes with the design's provider (open question 3). |
| L17 | The inventory horizon is stamped with the generation and `catalogue_complete`. It splits tombstones into collectable and stale (still referenced as DATA) and carries tombstone batches. It is never used destructively in the pass that built it. | `horizon.hpp:65-94`; `horizon.cpp:126-141`; `maintenance.cpp:620-660` | `test_inventory_splits_tombstones_by_whether_data_still_refers_to_them` (`test_ledger_gates.cpp:51`) | **Generalise**: `catalogue_complete` becomes "provider complete". Garbage and tombstone batches are FS state (H23). |
| L18 | The release horizon is stamped with the head hash and the head's mutation clock. Only a complete one is published. | `horizon.hpp:96-118` | `test_release_horizon_carries_its_head_and_clock` (`:69`) | **Generic** |
| L19 | The three destructive gates read only this node's state. Tombstone gate: due, not rebuilt, catalogue complete. Control gate: due, not rebuilt, a release view, catalogue complete, an inventory, generation at or past the known one. Data gate: the control gate's conditions; the reason names the first that failed. Before any inventory, the catalogue counts as complete and the generation as 0. | `contract/gates.hpp:14-54`; `gates.cpp:14-94` | `test_ledger_gates.cpp:125`, `:137`, `:154`, `:187` | **Generic**, with `catalogue complete` read as "every provider complete". |
| L20 | GC: two sweeps. CONTROL: release (64), the step, prune (64). DATA: release (64), the pool's `gc_step`, prune (64). The orphan grace is `max(garbage_grace, 300 s)`. The step yields to foreground or loader activity within `foreground_quiet`. | `maintenance.cpp:938-1071`; `storage_pool.cpp:1033-1062`; `docs/storage.md:168-176` | `test_retention_claim_is_physical_gc_barrier` (`test_storage_metadata.cpp:447`); `test_a_delete_is_reclaimed_while_a_node_is_away` (`test_rpc_cluster.cpp:4270`) | **Generalise**: the CONTROL step is `CatalogueManager::control_gc_step` (`catalogue/catalogue.cpp:2355-2409`) today. It moves into Dagda; the catalogue's "touched since the current root epoch" protection becomes a provider answer. |
| L21 | A copy with any local claim is untouchable: the claim is checked before deferral and again just before the irreversible delete. | `storage_pool.cpp:1033-1050` | `test_storage_metadata.cpp:447` | **Generic** |
| L22 | Unreferenced-since sightings: the first sighting on this node's own clock, persisted (`retention/unreferenced-{data,control}.bin`). A backwards clock resets maturity. A sighting not renewed in a pass is dropped. Nothing goes until `garbage_grace` (30 days) has passed on this node. | `storage/unreferenced_since.cpp:18-113`; `service/node_services.cpp:48-49`; `config.hpp:57-61` | `test_absent_nodes.cpp:29`-`:190` | **Generic**. `garbage_grace_ms` is defined by Dagda and read by Dannan as the roster's forgetting horizon (extraction, seam 1 risk). |
| L23 | Claim walk: pages `claimed` from a cursor under a budget of 16 and restores claimed objects not held. On `waiting_for_credit` it stays before that claim. | `service/claim_walk.cpp:8-43`; `claim_walk.hpp:37`; `maintenance.cpp:684-707` | `test_claim_walk.cpp:41`, `:98`, `:114`, `:336`, `:356`, `:373` | **Generic** |
| L24 | The horizon builder obtains its sets from the filesystem and the catalogue. | `ledger/node_horizon_builder.hpp:4`; `node_horizon_builder.cpp:11-113`; `contract/horizon_builder.hpp:4`, `:7`, `:20-47` | `test_horizon_builder.cpp:154`, `:189`, `:246`, `:284`, `:306`, `:325` | **FS**: cut at seam 7 (section 5.3). |
| L25 | Budgets, cursors and pages for long work. | `contract/walk.hpp:19-112` | `test_predicates.cpp` | **Generic**, and Dannan's (`sched/`). Dagda uses it. |

### 3.4 `heads/`

Re-read at Macha's merge. `2026-11-05-heads-plan.md` section 1 corrects the proof cells of
H1, H12, H15, H16 and H25, and names the rows with no case (H20, H29, H30). In particular
`test_metadata_history_checkpoint_concurrent_proposers_converge` does not exist at
`00861dd` or `b080c38`.

In the table, the head record is the store's and the FS state is the
owner's blob, as divided at extraction seam 8. Line numbers are develop's
unless marked.

| # | Behaviour | Where | Proof | Disposition |
|---|---|---|---|---|
| H1 | A commit is immutable and content-identified: `hash = SHA-256(u64be generation ‖ previous ‖ u32be length ‖ payload)`, streamed, recomputed on validation. | `metadata.cpp:535-540`, `:2114-2121`, `:2302-2304`; `metadata.hpp:246-250` | `test_storage_metadata.cpp:578`, `:619` | **Generic**. The payload is the owner's codec output. |
| H2 | A body is a full snapshot or a delta against its primary parent, whichever is smaller. Delta succession needs only `parent.generation < child.generation`. | `metadata.hpp:290-293`; branch: `metadata_manager.cpp:1875-1886`; `metadata.cpp:488-514` | `test_storage_metadata.cpp:2848` (DLT7), `:2938` (DLT8) | **Generalise**: the codec supplies `encode_full`, `encode_delta` and `apply_delta`; Dagda chooses by size. |
| H3 | Multi-parent: `merge_parents` (at most 64), the lower hash the primary, `generation = max(parents) + 1`. | `metadata.hpp:190`; `metadata_manager.cpp` develop 1242-1262 | `test_storage_metadata.cpp:737` | **Generic** (a head-record field) |
| H4 | Acceptance: the committing node stores the exact commit and persists a certificate `{required, replicas}`. A local mutation publishes to `{self}`. A merge first seeks `metadata_write_copies` holders, local first then nearest, and its certificate names them. The local replica is mandatory. A peer is sent the parent history before it is asked to accept. | `metadata_manager.cpp:177-185`, `:831-920`, `:1311`; `metadata.hpp:260-266` | `test_commit_replicas_ordered_local_then_nearest` (`test_rpc_cluster.cpp:4238`); `test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim` (`:5581`); `docs/metadata.md:18-28` | **Generalise** per space: the floor is a space's policy. Macha's space keeps "author suffices for a local commit; `metadata_write_copies` for a merge". This differs from the plan's wording (open question 1). |
| H5 | Certificate rules: replicas ≥ required; a floor record needs `required >= 1` and a witness; repeat certificates take min(required) and the union of replicas. Sealed under `DHTMACC1` in `heads.meta`. No vote and no prepare phase. | `metadata.cpp:2131-2217`, `:3225-3246`, `:3353-3379`; develop 4354-4365 | `test_storage_metadata.cpp:737`; `docs/metadata.md:28` | **Generic** |
| H6 | Accepted heads are a set: a hash-to-certificate map. A head that is an ancestor of another, by history or by clock domination, is dropped; a head with an empty clock is no one's ancestor. The preferred committed head is the highest generation, ties to the higher hash. | `metadata.hpp:558`; `metadata.cpp:3248-3301`, `:3430-3432` | `test_storage_metadata.cpp:1678`, `:1697` | **Generic** |
| H7 | Heads exchanged: surveyed on every node, history imported from each head's owners and certificate witnesses, certificates accepted, a 30 s cooldown for heads that cannot be. | `metadata_manager.cpp:922-943`, `:1086-1130` | `test_rpc_cluster.cpp:3667` (unknown history merged) | **Generic** |
| H8 | Reconciliation: fold divergent heads pairwise in hash order under one lock. Materialise both through the codec, call the owner's merge, commit the result with `merge_parents`. No common ancestor is located. Every reconciler computes the same commit. | `metadata_manager.cpp:1149-1348`; `docs/metadata.md:60-71` | `test_rpc_cluster.cpp:3784` (one reconciliation), `:4511` (disjoint pairs), `:5069`; `test_metadata_merge.cpp:495` (three heads, any order) | **Generalise**: Dagda calls `merge(base?, a, b) -> state | conflict` (plan 142). Macha passes no base today (open question 2). Determinism (a pure function of the two heads, no dots minted) is a requirement on the owner's merge, which Dagda states and tests. |
| H9 | Tree-backed reconciliation merges only what differs: diff the trees, collect the differing paths, conflict subjects and their parent directories, merge, and express the result as a delta against the primary parent. | `namespace_tree.cpp` develop 1981-2045; `metadata_manager.cpp` develop ~1213, 1276-1284 | `test_namespace_tree.cpp:1517` (tree merge equals the materialised merge); `test_namespace_service.cpp:487` | **Generalise**: Dagda supplies the tree diff over keys. The owner's merge is called per differing key, or on a sparse working set, never on whole materialised states (section 7). |
| H10 | A merge that cannot be made (`MergeUnavailable`, `DecodeError`) sets aside the head that is not this node's own. Set-aside times are persisted in `set-aside.meta`. A head set aside for `garbage_grace` is dropped. | `metadata_manager.cpp:1186-1189`, develop 1346-1351, 963-977; `metadata.hpp:559-571`; `metadata.cpp:3488-3556`; `docs/metadata.md:38` | `test_rpc_cluster.cpp:3731`; `test_storage_metadata.cpp:1982` | **Generic** |
| H11 | Conflicts are durable records, never an overwrite. They are superseded by a later change of their subject, resolvable by a commit, and GC roots while they stand. | `metadata.hpp:106-120`; `metadata.cpp:296-354`, develop 5029-5062; `docs/metadata.md:132-138` | `test_storage_metadata.cpp:2140` (conflict GC roots); `test_metadata_merge.cpp:162`, `:421` | **FS** today: `MetadataConflict` holds `FsEntry`, so a conflict is part of the blob (extraction seam 8). **Generalise** later: Dagda's own `conflicts(space)` and `resolve` (plan 218), for the first non-FS owner that returns `conflict`. |
| H12 | Dots: `mutation_sequences`, a clock of `NodeId → u64`. An author id and sequences persisted in `author.meta`. A new author id whenever the chain cannot be shown to continue. Retries keep their dot and return at once if the head already covers it. | `metadata.hpp:38-41`, `:164`, `:597-616`; branch: `metadata_manager.cpp:1686-1741`, `:1937-1948`; `docs/metadata.md:40-50` | `test_storage_metadata.cpp:2346`, `:2364` | **Generic** |
| H13 | `legacy_clock`: the clock as it stood when the lineage first recorded provenance. It judges entries without provenance. | `metadata.hpp:169`; `metadata_merge.cpp:33-58`, `:183-186` | `test_metadata_merge.cpp:369` | **FS**: only the FS merge reads it. The SM17, SM19 and DLT10 codecs carry it, so it is blob-side in a v18-compatible codec (open question 6). |
| H14 | `mutate_delta`: pick the head (extend this node's own when there are several), decode, check idempotency, reserve a durable dot, refuse a change to the policy fields, let the owner stamp its state, bump the clock, apply the delta to the tree, encode. An unchanged payload is a no-op. Then claim locally and accept locally. | branch: `metadata_manager.cpp:1682-1953`; `:1012-1042` (own head) | `test_namespace_service.cpp:592`; `test_namespace_tree.cpp:733` | **Generalise**: `mutate_local(space, fn)` (plan 153), with `fn` producing an exact delta through the owner's codec. Linearisable on one node, not across nodes. |
| H15 | The replicator offers the newest head to every active node, with history first, and the caller does not wait. | `metadata_manager.cpp:99-175` | `test_rpc_cluster.cpp:5069`, `:6364` | **Generalise**: to a space's holders (H20). |
| H16 | History is truncated by this node alone. Once it holds one accepted head and history passes 256 records or 64 MiB, that head becomes the root (`previous_known = false`). Never with two heads. Journal compaction at 128 records or 8 MiB. | `metadata.cpp` develop 4568-4600, 4961-4994; `metadata.hpp` develop 777-778 | `rpc_cluster/test_metadata_history_checkpoint_concurrent_proposers_converge`; `test_rpc_cluster.cpp:6299`; `test_storage_metadata.cpp:2636` | **Generic** |
| H17 | Bounded snapshot memory: payloads share immutable backing (`SharedBytes`), hashes are streamed, deltas applied in place, and a materialisation cache (128 MiB, 64 entries) pins the current and committed heads. | `metadata.hpp:197-244`, `:689`; `metadata.cpp:1985-2105`, `:2114-2121`, `:3625-3672` | VALIDATION "Memory"; `test_metadata_record_payload_copy_is_shared` (`test_storage_metadata.cpp:488`) | **Generic**, without the per-delta whole-state encode (section 7). |
| H18 | The Merkle tree over keys. Node boundaries are chosen by `top64(SHA-256(domain ‖ level ‖ key)) < max/target`, so they are history independent. Leaf and branch target 32, max 128. A branch holds `(first_key?, id, item count)` per child. Node id = SHA-256 of its bytes. An empty map is one empty leaf. | `namespace_tree.hpp:19-62`; `namespace_tree.cpp:18-101`, `:409-428`; `namespace_control_store.cpp:18` | `test_namespace_tree.cpp:136` (history independence), `:159`, `:236`, `:261`, `:641`, `:1050`, `:1137` | **Generic** for branches and boundaries. **FS** for the leaf entry codec and the extent spine (`namespace_tree.cpp:244-334`). Macha's domain strings (`macha/namespace-tree/.../v1`) are part of the bytes (section 4.3). |
| H19 | Tree nodes are CONTROL objects. A commit may name a new root only once this node holds every node it wrote; only the nodes a commit wrote are offered. Replay rebuilds the tree locally. | `namespace_control_store.hpp:17-57`; `namespace_control_store.cpp:15-44`; `local_state.cpp:165-171`; `docs/durability.md:67-76` | `test_namespace_tree.cpp:792`, `:1203` | **Generic** |
| H20 | Replica policy per space: `replicas: N` or every-node at `open()`. Holders are `rendezvous_nodes(space, roster, N)`. A non-holder forwards reads over CONTROL and says so in the evidence. | none: Macha has one every-node space | none | **Generalise**: new (Decisions 2026-11-03; extraction seam 8, "Replica policy"). Proof to write: a space at three replicas on a five-node cluster, read from a non-holder, written while a holder is partitioned, and repaired. Lir asks that `open()` take the owner's placement salt and that the replica set be returned as evidence (stack document, "Lir on Dagda"). |
| H21 | Reads are memory reads on a holder: the current head set and the materialised state are in memory. | `contract/metadata_view.hpp`; `docs/metadata.md:7` | `test_namespace_entries.cpp:88`-`:242` | **Generic** |
| H22 | The commit claims what it introduces. DATA it holds is claimed here; DATA it does not hold is claimed on a node present, or the commit is refused ("held by no node present"). New CONTROL objects are claimed here. A merge claims under a fresh dot that is not in its clock. | `node_services.cpp` ~380-460; branch: `metadata_manager.cpp:1893-1898`; `metadata_manager.cpp` develop 1292-1309; `docs/durability.md:49-51` | `test_a_file_touch_commits_while_a_holder_is_away` (`test_rpc_cluster.cpp:4135`); `test_namespace_service.cpp:774` | **Generalise**: what a commit introduces is the owner's (provider and codec); claiming it before acceptance is Dagda's. |
| H23 | Tombstone batches: one immutable CONTROL object per retiring mutation (`MTOMB001`), named by the head, claimed like tree nodes, dropped whole after the grace. | `metadata.hpp:76-104`; branch: `metadata_manager.cpp:1826-1845`; `maintenance.cpp:149-226`; `docs/metadata.md:119-131` | `test_namespace_service.cpp:336`; `test_storage_metadata.cpp:4750` | **FS** (what is retired). **Generalise**: retirement with a grace is a reachability-provider answer ("garbage with a retirement time"). |
| H24 | The generation is advertised and settled; heads are announced when the set changes. | `metadata_server.cpp:84-102`; `cluster.cpp:43`, `:863`, `:877` | `test_rpc_cluster.cpp` | **Generalise**: a Dannan advert per space (`ACTIVE.md`, "Store figures travel as adverts"). |
| H25 | The cache seed: the last metadata record in the cache, used to recover a head set. | `persistent_cache.cpp:325-390`; `docs/metadata.md:44-48` | `test_storage_metadata.cpp` | **Generic**, per space. |
| H26 | Per-peer ordered execution of history import, commit store and accept, on a bounded executor with a quarter kept for control. | `net.cpp:3810-3870` | `test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers`; `test_rpc_metadata_control_mutations_go_first` | **Generic**: Dagda registers these types `ordered` (Dannan Decisions, "how a request is executed"). |
| H27 | Genesis: virgin founders build the same deterministic generation-2 root, publishable by any founder. | `metadata.cpp:2290-2301`; branch: `metadata_manager.cpp:1477-1508`; `docs/metadata.md:89` | `test_a_new_namespace_is_a_tree_from_its_first_commit` (branch: `test_storage_metadata.cpp:2617`) | **Generalise**: genesis as a constant hash per space (owner, name, codec version), never decoded (the branch's direction). |
| H28 | Write-floor advertisement: nodes advertise `metadata_write_copies`. A mismatch marks membership changed and does **not** fail closed. | `cluster.cpp:44-45`, `:870`; `types.hpp:61-64` (a stale comment) | `test_nodes_seeking_different_copy_counts_share_one_namespace` (`test_rpc_cluster.cpp:2993`) | **Generalise**: one cluster answer for a space's policy (section 7, item 1). |
| H29 | The scope check at the write floor: every replica that counts checks the writer's scope before storing. | none in Macha | none | **Generalise**: new (Decisions: looked up in the Lugh head; stack document, "Scope"). With no source installed, node principals are allowed, which is Macha today. |
| H30 | No rollback: an ancestor of the held set changes nothing, and an unrelated commit is a divergence, never a replacement. | `metadata.cpp:3248-3301` | owed | **Generic** in Macha's shape. A case is owed (Lugh requirement a). |

### 3.5 `lease/`

| # | Behaviour | Where | Proof | Disposition |
|---|---|---|---|---|
| E1 | A claimed collection, keyed per record. Settings, intent and phase are separate field groups. Settings: last-writer-wins by `(changed_ms, node)`. Intent: last-writer-wins with `cancelled` ordered first, so it dominates. Phase: the group with the higher `(phase_epoch, phase, progress_ms)` wins whole. Set-once fields: the whole-record maximum. | `torrent/torrent_request.cpp:36-47`, `:163-203`; `torrent_request.hpp:24-95` | `test_torrent_requests_join_without_conflicts` (`test_storage_metadata.cpp:4660`): commutative, associative and idempotent over 7 variants; cancel beats a later pause (`:4719`) | **Generalise**: the template's traits name the field groups and their keys. There is no single `changed_ms` (open question 4). |
| E2 | Claim order: higher epoch, then earlier claim, then lower node id. One side's claim is kept. | `torrent_request.cpp:43-47`, `:184-187` | `:4720`-`:4722` | **Generic**: the `Lease` record `{holder, epoch, since}` (plan 163). |
| E3 | A max-wins tombstone (`removed_unix_ms`) kept for a 7-day window. Only the sweeper erases expired ones: the node with no active member of a lower id. | `torrent_request.cpp:201`; `torrent_coordinator.hpp:36`; `torrent_coordinator.cpp:1062-1100` | `:4723`; `test_hydration_catalogue.cpp:2378` | **Generalise**: the retention window is the template's. The sweeper becomes a lease (as AthenaSIP's orphan sweep, extraction 587). |
| E4 | Three-way merge over the key set: a one-sided key is kept unless it equals its base (so an erasure stands). | `torrent_request.cpp:205-244` | `:4727`-`:4734` | **Generalise**. In production the base is always empty (`metadata_merge.cpp:354-355`), so an erasure never stands by base. It is held by the tombstone (open question 2). |
| E5 | Duplicate rule: among live records with the same key (`info_hash`), the earliest `(created_ms, id)` wins. Losers are cancelled with `duplicate_torrent`; their phase is left for the owner. | `torrent_request.cpp:49-52`, `:225-242` | `:4736`-`:4743` | **FS** rule, **generic** hook: a per-collection duplicate rule (plan 167). |
| E6 | Hard bounds at decode: at most 65,536 records, text at most 8,192 bytes, flag bytes 0 or 1, enums in range, a record format byte of 1. | `torrent_request.hpp:114-115`; `torrent_request.cpp:89-161`; `metadata.cpp:856-876` | `test_torrent_requests_ride_the_snapshot_and_its_deltas` (`test_storage_metadata.cpp:4856`) | **Generalise**: bounds are the template's parameters; the codec is the owner's. |
| E7 | Takeover after absence: absence is measured on the local clock from local membership (10 min). Ranked takers wait 30 s × rank. The new claim is epoch + 1. A job is adopted only after the commit and a re-read show the claim standing. A superseded holder stands down. | `torrent_coordinator.hpp:32-37`; `torrent_coordinator.cpp:837-1126` | `test_torrent_requests_belong_to_the_cluster_and_are_driven_by_their_owner` (`test_hydration_catalogue.cpp:2378`) | **Generalise** to `acquire(name, scope, absence)` and `holder`. Renewal and `release` are new (plan 219); Macha never renews. |
| E8 | An owner whose phase moves back, or revives, re-claims itself at epoch + 1. | `torrent_coordinator.cpp:994-1000` | the same | **FS** (a rule of the phase group) |
| E9 | A fence: every action under a lease carries its epoch and the receiver rejects a lower one. | **none**: `handle_intent`, `apply_intent_locally` and `ClusterJobView::torrent_action` carry no epoch (`torrent_coordinator.cpp:650-705`; `acquisition/cluster_jobs.cpp:198-229`) | none | **Generalise**: new (plan 165). Macha's port need not use it. Lir and AthenaSIP need it. |
| E10 | Per-origin counters merged by sum, with a window. | none in Macha | none | **Generalise**: new (plan 169); Lugh and AthenaSIP. |

### 3.6 Context (`dagda::Context`)

| # | Behaviour | Where | Proof | Disposition |
|---|---|---|---|---|
| C1 | Every call completes on the caller's executor and takes a deadline and a work class. The blocking forms Macha calls (`put`, `commit`) wait on those completions with their declared waits. | `contract/work.hpp` `Waits` (e.g. `claim_store.hpp:46-91`) | `test_contract.cpp`; `test_work_class.cpp` | **Generalise**: Dannan's callback shape (stack document, "Async shape"). |
| C2 | Nouns: Domains, Objects, Claims, Heads, Leases, Tickets, Retained, plus node-local spaces. | plan 215-221 | none | **Generalise**. Macha needs Objects, Claims, Heads, Leases and Tickets. |
| C3 | A reachability provider registered per space or domain (`register_reachability`). | `ledger/node_horizon_builder.*` | seam 7 gates | **Generalise** (section 5.3) |
| C4 | Status: storage, cache, metadata and repair figures, published into Dannan's one Status surface. Macha's Status JSON is unchanged. | `api/status_api.cpp`; `telemetry.hpp` | the Status golden | **Generalise**: Dagda publishes; Macha's facade renders. |
| C5 | Message types registered on Dannan's route table: lanes, classes, notification and `ordered`. | `net.cpp:407-452`, `:819-831`, `:2027-2037`, `:3805-3819` | seam 4 gate | **Generalise** (section 4.2) |

### 3.7 Macha 0.92.3 (`00861dd`)

Behaviour Macha gained by fixing defects Dagda found. Dagda matches these shapes
(the maintainer, 2026-11-04 night). Proof names Macha's case where 0.92.3 added or changed one.

| Row | Behaviour | Where (`00861dd`) | Proof | Dagda |
|---|---|---|---|---|
| S34 | A writer waits for the accounting scan before it takes the object's lock, in put, remove and remove-if-older | `storage/local_store.cpp` `await_accounting`, `put_impl`, `remove`, `remove_if_older_than` | No new case; `test_verification_resumes_after_a_restart` adjusted | Built: `LocalStore::await_accounting` |
| S35 | The held ledger is built before the scan thread starts | `storage/local_store.cpp` constructor | No case | Built |
| S36 | The scan's completion and failure are flagged under the mutex, and the wait takes the stop token | `storage/local_store.cpp` `mark_scan_failed` | No case | Built |
| L25 | A seed installs what was queued, then writes the durable marker, then sets the flag, under the flush lock | `HeldLedger::seed` | No case | Built, no case: the window needs a fault seam |
| L26 | A striped claim guard (64 stripes) is held across every no-claim check and its delete, and every presence check and its claim | `contract/claim_store.hpp`, `contract/object_ledger.hpp`; `gc_step`, CONTROL sweep, `erase_all`, `delete_object`, `retain_objects`, the two local retains | No case | Built: `ClaimLocks`. Dagda also bounds it to 4096 ids at a time (M8), which Macha's holds for a whole batch. Sent to Macha |
| S37 | Repair tells a refilled list from the last by a version, never a buffer address | `DistributedStore::repair_step` `pull_version`, `push_version` | No case | Equivalent: lists held by `shared_ptr` identity |
| L29 | (0.92.4, `cac5998`) A delete holds the stripe only to check no claim stands and mark the id, then unlinks under the mark; a claim finding the mark treats the object as not present. No DATA-device wait under a stripe | `contract/claim_store.hpp` `ClaimDeletion`; `ObjectLedger::claim_deletion` | `test_a_delete_marks_its_object_and_holds_no_stripe` | Built: `StoreClaimLocks.ADeleteMarksItsObjectAndHoldsNoStripe`, `AClaimFindsAMarkedObjectNotPresent`. A multiset of marks, with the maintainer via Macha |
| S38 | (0.92.4) The survey's walks take a pause that throws a stop, caught by the pass | `service/maintenance.cpp` `MaintenanceStopping` | No case | Built: `StoreRepair.AStopEndsTheSurveysWalk`, with the rethrow Macha's per-peer catch lacks, with the maintainer via Macha |
| L27 | A removal stamped by a clock ahead is fresh, not expired | the torrent tombstone in `storage/` metadata | `test_torrent_requests_ride_the_snapshot_and_its_deltas` | Built: `LeaseClaimedCollection.ATombstoneFromAClockAheadIsKeptItsWindow` |
| L28 | The delta decoder refuses a request with no id, as the snapshot decoder does | DLT9 decoder | `test_a_delta_refuses_a_torrent_request_with_no_id` | Built: `LeaseClaimedCollection.ADeltaCannotInstallARecordTheSnapshotRefuses` |

## 4. Formats

### 4.1 On disk (layout v18)

All integers are big-endian (`codec.cpp:9-24`). `bytes` means a u32 length
followed by the bytes (`codec.cpp:30-35`).

| Thing | Path | Format | Where |
|---|---|---|---|
| State layout marker | `<state>/storage-layout` | `macha-state-layout-v18` | `node_identity.cpp:16-37` |
| Node id | `<state>/node-id` | 32 hex characters | `local_store.cpp:2154-2182` |
| Backend marker | `<backend>/.macha.backend` | `macha-backend-v18 <node-hex> <token-hex>`, token 16 random bytes | `storage_pool.cpp:22-37` |
| Backend record | `<state>/backend-identities/<sha256(path)>.id` | mirror of the marker | `storage_pool.cpp:109-113` |
| Loose object | `<root>/objects/xx/yy/<hex>.obj` | `DHTOBJ01`, u64 plain size, nonce 12, tag 16, ciphertext; AAD = id | `local_store.cpp:35`, `:1063`, `:1091-1096` |
| Pack record | `<root>/packs/pack-%020llu.pack` | 125-byte header followed by the payload (fields below) | `local_store.cpp:45-47`, `:139-185` |
| Accounting | `<root>/.macha.accounting` | two 128-byte slots: `MACHAAC1`, seq, used, op, size, id, padding, SHA-256 | `local_store.cpp:36-41`, `:96-137` |
| Sealed journal frame | various | `be32 length ‖ nonce ‖ tag ‖ ciphertext`, an 8-byte AAD per journal, no file magic | `sealed_journal.hpp:16-25`; `sealed_journal.cpp:59-195` |
| Retention WAL | `<state>/retention/claims.log` | AAD `MACHRTJ1`; ops add, release and prune with class, dot or clocks, and ids; frames at most 4 MiB | `retention.cpp:18`, `:25-26`, `:257`, `:476-640` |
| Trie journal and nodes | `<dir>/journal.log`, `<dir>/nodes-<gen>` | AAD `MACHLJ01` and `MACHLN01`; node kinds leaf 1 and interior 2 | `object_trie.cpp:21-22`, `:208-238`, `:480-483` |
| Tries | `<state>/ledger/data-<token>`; `<state>/retention/ledger-{data,control}`; the reference counts | `ObjectTrie` | `local_store.cpp:247`; `retention.cpp:455-456`; `local_state.cpp:155-157` |
| Sightings | `<state>/retention/unreferenced-{data,control}.bin` | u32 schema 1, u64 n, (id, u64 ms)* | `unreferenced_since.cpp:18-43` |
| Metadata files | `<state>/metadata/`: `history.log` (`DHTMHST1`), `heads.meta` (`DHTMACC1`), `author.meta` (`DHTMAUT1`), the checkpoint (`DHTMDB01`), the journal (`DHTMJNL1`), `set-aside.meta` | sealed | `metadata.cpp:22-41`, `:2335-2342`, `:3225-3246` |
| Cache metadata seed | `<cache>/metadata/current.meta` | `DHTCMET1`, nonce, tag, ciphertext; AAD = the magic | `persistent_cache.cpp:17`, `:325-390` |
| Legacy retention checkpoints | `claims.meta` (`MRTS0001`), `checkpoints/gen-*/xx.meta` (`MRTS0002`) | read once in migration | `retention.cpp:19-21`, `:214-235` |

The legacy retention checkpoints are not to be specified. Under the
operator's no-legacy ruling, Dagda need not read them (open question 7).

**Pack record header.** Each record is a 125-byte header followed by its
payload.

| Offset | Field | Size |
|---|---|---|
| 0 | magic `MACHPK01` | 8 |
| 8 | type: 1 put, 2 remove, 3 touch | 1 |
| 9 | `touched_ms` | u64 |
| 17 | id | 32 |
| 49 | `plain_size` | u64 |
| 57 | `payload_size` | u64 |
| 65 | nonce | 12 |
| 77 | tag | 16 |
| 93 | SHA-256 of bytes 0 to 92 | 32 |
| 125 | ciphertext (put only), at most 128 MiB | `payload_size` |

### 4.2 On the wire (protocol 23 store traffic)

Frame: magic `MC13`, frames 4 KiB to 4 MiB, protocol 23 (`net.cpp:96-99`).
Frame types: control 1, foreground 2, read_ahead 3, speculative 4, loader 5
(`frame_type.hpp:11-17`).

Lanes (`net.cpp:2027-2037`):
- DATA: `get_object`, `put_object`, `put_object_deferred` and
  `object_durability_barrier`.
- CONTROL: everything else. Metadata travels on CONTROL even when it rides
  DATA frame priorities.

Default frames (`net.cpp:819-831`):
- foreground: the four DATA-lane types;
- speculative: `get_control_object`, `put_control_object`, the `have_*`
  types, `trie_diff`, `retain_objects` and `delete_object`;
- control: everything else.

**Store messages.** The extraction (seam 4) assigns these to the store.

| # | Name | Reply | Lane | Payload |
|---|---|---|---|---|
| 3 | `have_object` | `bool_reply` 103 | CONTROL | id → u8 (full read and hash) |
| 4 | `get_object` | `object_reply` 104 | DATA | id → id, bytes |
| 5 | `put_object` | `ok` 100 | DATA | id, bytes |
| 13 | `delete_object` | `ok` | CONTROL | id; 409 if claimed |
| 19 | `get_control_object` | `control_object_reply` 107 | CONTROL | id → id, bytes |
| 20 | `put_control_object` | `ok` | CONTROL | id, bytes |
| 22 | `put_object_deferred` | `ok` | DATA | → epoch 16, domain u64, generation u64, instance u64 |
| 23 | `object_durability_barrier` | `ok` | DATA | epoch, domain, generation, instance [, u32 n, ids ≤ 4096] → [fresh epoch, (id, domain, generation, instance)*] |
| 32 | `retain_objects` | `ok` | CONTROL | u8 class, origin 16, u64 seq, u32 n, ids; 409 `not_present` |
| 40 | `have_objects` | `have_objects_reply` 118 | CONTROL | u32 n ≤ 200,000, ids → u8 each |
| 45 | `have_control_objects` | `have_control_objects_reply` 121 | CONTROL | the same, control store; control frame only |
| 47 | `have_valid_objects` | `have_valid_objects_reply` 123 | CONTROL | u32 n ≤ 16, ids → u8 {1 intact, 0 absent, 2 cannot say} |
| 49 | `trie_diff` | `trie_diff_reply` 125 | CONTROL | u8 kind {root, children, records}, u8 trie, at 32, u32 n, prefixes (≤ 64 or ≤ 512) |

Sources: `storage_server.cpp:74-392`, `trie_diff.hpp:77-106`,
`distributed_store.cpp:424-431`, `:553-705`, `:824-834`.

**Metadata messages**, served by `metadata_server.cpp:109-197`:

| # | Name |
|---|---|
| 6 | `get_metadata` |
| 10 | `get_committed_metadata` |
| 21 | `get_metadata_identity` |
| 26 | `get_metadata_history_entry` |
| 27 | `has_metadata_history_entry` |
| 28 | `put_metadata_history_entry` |
| 29 | `get_metadata_heads` |
| 30 | `put_metadata_commit` |
| 31 | `accept_metadata_commit` |
| 41 | `get_metadata_history_record` |

Their replies are 105, 108, 111 and 112 (`net.hpp:116-123`).

Types 28, 30 and 31 run on the ordered metadata executor (`net.cpp:3810-3819`).

Types 7, 8, 11, 17 and 18 (`cas_metadata`, `seed_metadata`,
`checkpoint_metadata`, `commit_metadata`, `cas_metadata_delta`) are not
bound by the server. They are legacy and not specified.

Retired numbers, never reused: 37, 38, 48 and 124 (`net.hpp:65`, `:82`).

### 4.3 Head encodings (tree form only)

Magics are at `metadata.cpp:22-41`.
- Snapshots: SM14 `DHTMETB4`, SM16 `DHTMETB6`, SM17 `DHTMETB7`, SM19 `DHTMETB9`.
- Deltas: DLTn is `DHTMDLTn`, with DLT10 written `DHTMDLTA` and DLT11 `DHTMDLTB`.
- Tombstone batch: `MTOMB001`.

The encoder is `encode_snapshot_v14` (`metadata.cpp:984-1071`). It picks the
form at `:1006`:
- **SM14**: the tree base.
- **SM16**: SM14 with torrent requests (written when there are any).
- **SM17**: SM16 with the legacy clock and the catalogue dot (written when a
  legacy clock is set).
- **SM19**: SM14 with torrent requests, a legacy-clock flag and optional
  clock, and tombstone batches (written when the head names a batch).

**SM14+ field order** (`:984-1069`). Head and FS fields interleave, which is
why the FS codec is a function of both halves (extraction 461-468).

| # | Field | Side |
|---|---|---|
| 1 | magic | head |
| 2 | voters | head, legacy slot |
| 3 | `data_replication` u32, `extent_size` u64 | policy / FS |
| 4 | `mutation_sequences` | head |
| 5 | `namespace_root` (32 bytes) | FS |
| 6 | `catalogue_root` (flag plus 32 bytes) | FS |
| 7 | inline garbage (u32 count, normally zero) | FS, reserved slot |
| 8 | `node_status` | FS |
| 9 | `identity_resets` | cluster |
| 10 | `merge_parents` | head |
| 11 | conflicts | FS |
| 12 | write floor (u32, non-zero) | head |
| 13 | participants | head |
| 14 | branch floor (32 bytes) | head |
| 15 | baseline (u8) | head |
| 16 | SM16, SM17, SM19: torrent requests | FS |
| 17 | SM19: legacy-clock flag | head |
| 18 | `legacy_clock` and `catalogue_dot` | head / FS |
| 19 | SM19: tombstone batches | FS |

Decode bounds are at `:1078-1206`, and trailing bytes are refused.

**Deltas** (encoder `:1363-1530`; it picks the smallest version that fits,
`:1378-1405`):

| Version | Content |
|---|---|
| DLT5 | the base: clock; erase and upsert entries; catalogue; erase and upsert garbage; node status; identity resets |
| DLT6 | adds `merge_parents` and conflicts |
| DLT7 | adds a flags byte giving each its own presence |
| DLT8 | adds appends |
| DLT9 | adds torrent requests |
| DLT10 | adds the append dot, set legacy clock, set catalogue dot |
| DLT11 | adds and drops tombstone batches |

**Tree nodes** (`namespace_tree.cpp:18-101`, `:244-334`):
- Magics: `MNL1` leaf, `MNB1` branch, `MNX1` and `MNY1` the extent spine.
- Boundary domains: `.../entry/v1`, `.../branch/v1`, `.../extent-branch/v1`.

**Hashes:**
- the commit (H1);
- the conflict id: hex SHA-256 of `MACHCFL1` ‖ the record (`metadata.cpp:2322-2328`);
- the tree node id: SHA-256 of its bytes;
- the namespace signature: SHA-256 of `macha/namespace-signature/tree/v1` ‖ root (`metadata.cpp` develop 5068-5072).

**What "reserved slots" means here.** A commit's hash is over its stored
payload, and replay re-encodes the snapshot and checks it
(`metadata.cpp:3757-3758`, `:4232-4242`). So the tree-form codec must
re-emit, byte for byte, every slot that existing records carry:
- the voters slot;
- the inline-garbage count (zero);
- the entries erase and upsert slots in DLT5 to DLT11, which a tree-form
  delta uses for the changed paths it applies to the tree;
- the participants, branch floor and baseline slots.

These stay in the codec even though Dagda never interprets them. The
Macha session's phrase "the entries and inline-garbage wire slots stay
reserved" is read this way (open question 5).

### 4.4 Grandfathering, or a fresh format

The decision is the maintainer's (`ACTIVE.md`, Waiting on the maintainer 6; stack document, 9).

**If Dagda keeps byte compatibility with v18 on disk:**
- **Keys.** The Macha domain's key is HKDF of the cluster key with
  `macha/storage/v1`, and the AAD is the object id alone. Binding the
  domain into the AAD (D2) then applies only to new domains. Macha's domain
  keeps the v18 binding, an exception Lugh's requirement (a) would have to
  accept for Macha's data.
- **Every format in 4.1 unchanged:** the markers `macha-state-layout-v18`
  and `macha-backend-v18`, the file paths under `state_path`, the sealed
  journal AADs (`MACHRTJ1`, `MACHLJ01`, `MACHLN01`), and the trie node
  encoding.
- **Heads.** The FS codec is the SM14/16/17/19 and DLT5-11 encoders. Dagda
  hands the codec the head record and the blob together, because the fields
  interleave (extraction 461-468). The head record keeps the voters,
  participants, branch-floor and baseline slots as opaque values. The
  certificate and head-set files (`heads.meta`, `history.log`,
  `author.meta`) keep their magics and seals, and they are Dagda's, not the
  codec's.
- **Spaces.** A second space, Lugh's or Lir's, needs a layout that holds
  several spaces. v18 holds one namespace in `<state>/metadata/`. So
  grandfathering means: Macha's space where it is today, and every other
  space under a new directory that v18 never had. Since v18 refuses a
  non-empty unversioned state, Dagda must define the new directory as part
  of the v18 state, or bump.
- **The wire is not grandfathered whatever is decided.** Dannan's wire v1
  is not protocol 23. Dagda's types register freely from 512, and Dannan
  refuses 1 to 511 (the maintainer, 2026-11-04; `ACTIVE.md`, "Dannan, as Dagda finds
  it"). So a Dagda node can
  never speak protocol 23's store messages, and "protocol 23 byte for byte"
  for the store is impossible on Dannan.
- **What can be kept on the wire** are the payload encodings inside the new
  types: the deferred-put reply, the barrier, `retain_objects`, `trie_diff`
  and the metadata history frames. The two-process mixed-build gate of
  extraction seam 8 (a 0.91 node against a ported node) cannot be met. It
  becomes a flag day (Dannan Decisions, 2026-11-03 night: "Macha's move to
  the new wire is a flag day"). This contradicts the Macha session's "no
  protocol bump planned" (open question 8).

**If Dagda takes a fresh format:**
- New markers (`dagda-state-v1`, `dagda-backend-v1`).
- The domain bound into every AAD. Per-domain keys under Dannan's custody.
- A layout with spaces from the start.
- A generic head record whose owner blob is a separate byte range: the
  layout v19 the extraction reserves (extraction 465-467).
- Macha's port is then a migration of state and backends, which Macha's
  rule refuses (`docs/storage.md:178-182`: "not a live migration layer").
  It would need either a one-time conversion tool (the operator has just
  removed one, `5f6101c`) or a fresh cluster. With the node live when this was written and
  about 2 TB of DATA (now live-a, 4 TB, and live-b, 583 GB), a re-seal of every object is the cost.
- A middle course:
  - keep the object formats (loose, pack, the object-id AAD) for Macha's
    domain, so DATA is not rewritten;
  - make heads, claims and tries fresh;
  - rebuild them from the held DATA and a one-time export of the head.
  This is open question 8 for the maintainer.

## 5. The FS-specific seams to cut

Each item is FS shape today and becomes owner-supplied. Dagda declares the
interface; Macha implements it in `filesystem/` or `catalogue/`.

### 5.1 The head codec (seam 8)

- **`MetadataSnapshot`** (`metadata/metadata.hpp:142-196`) mixes two halves:
  - Head record: `mutation_sequences` `:164`, `merge_parents` `:190`,
    `metadata_write_replicas_required` `:150`, and the legacy slots
    `metadata_voters` `:145`, `metadata_participants` `:154`,
    `metadata_branch_floor` `:158`, `retention_baseline_complete` `:161`.
    The record's `generation`, `previous` and `hash` (`:246-250`) and the
    certificate (`:260-266`) are outside the snapshot already.
  - FS blob: `data_replication` `:146`, `extent_size` `:147`,
    `legacy_clock` `:169`, `catalogue_dot` `:172`, `catalogue_root` `:173`,
    `namespace_root` `:176`, `entries` `:177`, `garbage` `:180`,
    `tombstone_batches` `:182`, `node_status` `:184`, `identity_resets`
    `:187`, `conflicts` `:193`, `torrent_requests` `:195`.
- **FS types:** `FsEntry`, `ExtentRef`, `EntryType` and provenance
  (`metadata.hpp:15-74`); `GarbageRef`, `TombstoneBatch` (`:76-104`);
  `MetadataConflict` (`:106-120`); `PersistedNodeStatus` (`:122-140`); the
  delta's FS fields (`:311-363`).
- **FS codecs:** the entry codec (`metadata.cpp:215-268`), the conflict
  codec (`:296-354`), `encode_snapshot_v14` (`:984-1071`),
  `decode_snapshot_v14` (`:1078-1206`), the delta codec (`:1363-1640`) and
  the torrent request section (`:856-876`).
- **FS helpers:** `retire_objects`, `tombstones_of`, `same_content`,
  `later_entry`, `prune_superseded_conflicts` (`metadata.hpp:386-413`);
  `metadata_conflict_extent_roots`, `metadata_catalogue_root_set`,
  `metadata_namespace_signature` (`:464-468`); `apply_entry_append`,
  `stamp_entry_provenance` (`metadata.cpp:106-153`); the "/" root checks
  (`:1357-1359`, `:2099-2104`); `extent_size` against config
  (`metadata_manager.cpp:1154-1156`, `:1227-1229`).
- **`MetadataView`** (`contract/metadata_view.hpp`) is FS-typed (`FsEntry`
  paging). It becomes Macha's facade over `read`, `mutate_local` and
  `commit` (extraction 457-459).

### 5.2 The merge (seam 8)

- `merge_metadata_heads_over(left, right, left_entries, right_entries,
  left_head, right_head)` (`metadata.hpp:457-462`;
  `metadata_merge.cpp:69-360`) moves to `filesystem/` as the owner's merge.
- Inside it, the parts that read head fields are the policy max
  (`:91-96`), the clock join (`:97-117`) and the legacy-clock judgement.
  Dagda joins the clock (a head-record field) itself. The owner joins
  everything else.
- The mutate-side FS logic inside `mutate_delta` moves to the owner's
  mutation function: provenance stamping, the catalogue dot, garbage
  canonicalisation, tombstone batches, conflict pruning (branch:
  `metadata_manager.cpp:1771-1861`). So does `resolve_conflict`.
- **The tree.** Branches and boundaries are generic (H18). These stay with
  the owner:
  - the leaf entry codec (`namespace_tree.cpp:244-334`);
  - the extent spine;
  - `NamespaceChanges`;
  - `merge_tree_backed_heads` (develop 1981-2045).

  The cut: Dagda's tree takes keys and opaque values, with an
  owner-supplied value codec and an optional per-value child-tree hook (the
  spine). The tree merge calls the owner's merge on the differing keys
  only. The extraction's open question ("whether `namespace_tree` is
  generic enough", extraction 639-640) is answered: generic in its branches
  and boundaries, FS in its leaves. The domain strings
  `macha/namespace-tree/...` become a parameter of the space.

### 5.3 The `ReferencedSets` provider (seam 7)

The ledger depends on the FS and the catalogue at these points:
- `ledger/node_horizon_builder.hpp:4` and `contract/horizon_builder.hpp:4`
  include `catalogue/catalogue.hpp`; `contract/horizon_builder.hpp:7`
  includes `filesystem/filesystem.hpp`.
- `contract/horizon.hpp:6` includes `metadata/metadata.hpp` for
  `GarbageRef` and `TombstoneBatch`.
- `NodeHorizonBuilder` calls `FileSystem::maintenance_objects_cached`
  (`node_horizon_builder.cpp:91`), `CatalogueManager::maintenance_objects`
  (`:97`), `FileSystem::namespace_references` (`:105`) and
  `CatalogueManager::retention_objects` (`:111`).
- `build_release` walks `FsEntry.extents` when no counted view is given
  (`:39-48`).
- In the other direction, `CatalogueManager` calls `ledger_.retained` in its
  CONTROL GC step (`catalogue.cpp:833`, `:2355-2418`).

The provider Macha registers must supply, to reproduce today exactly:
1. **Namespace DATA (extents).** For the inventory: a complete frozen view
   at the converged generation. For the release: a view brought to the
   release head, or a full walk when the census fails. A walk failure
   throws.
2. **Namespace CONTROL (tree nodes).** The same views. An unreadable node
   marks the release incomplete, and never yields a silently shorter set
   (`test_inventory_control_set_holds_the_namespace_tree`,
   `test_horizon_builder.cpp:189`).
3. **Conflict alternatives' extents (DATA).** Taken from the head, always
   complete (`node_horizon_builder.cpp:49-50`).
4. **Catalogue DATA** (artwork and media indexes) for every root in the
   head's root set, the current root and its conflict alternatives
   (`catalogue.cpp:2184-2353`, `metadata.cpp` develop 5007-5021). For the
   inventory, the installed view too, with completeness reported apart.
5. **Catalogue CONTROL** (root ids, manifests, shards), with the same rules
   as item 4.
6. **Tombstone batch ids (CONTROL)** and the garbage list with retirement
   times. Unreadable batches are counted and left to the sighting grace
   (`filesystem.cpp:2598-2605`).
7. **Stamps.** Inventory: the generation and one completeness bit.
   Release: the head hash, the clock, and a completeness bit covering
   catalogue roots and tree nodes.
8. **The CONTROL GC protection** the catalogue applies today: anything
   touched since the current root epoch.

The proposed shape: `ReferencedSets provider(space_head, purpose) ->
{data, control, garbage[], complete, stamp}`.
- `purpose` is inventory or release.
- Views may be counted tries (L15), which Dagda keeps.
- Several providers per domain are unioned, complete only if all are
  complete.
- Dagda's `ReferencedSets` value type is renamed or kept (open question 3).

### 5.4 The lease template instantiation (seam 6)

- The template takes over the field-group merge
  (`torrent_request.cpp:36-47`, `:163-203`), the collection merge with
  three-way keep (`:205-224`), and the tombstone window
  (`torrent_coordinator.hpp:36`).
- `TorrentRequest`'s traits supply:
  - the groups and their keys (settings by `(settings_changed_unix_ms,
    settings_changed_by)`; intent by `(cancelled, desired_changed_unix_ms,
    desired_changed_by)`; phase by `(phase_epoch, phase, progress_unix_ms)`);
  - the claim (`TorrentClaim` as `Lease`);
  - the duplicate key and rule (`:225-242`);
  - the record codec (`:89-161`, format byte 1);
  - the bounds (`torrent_request.hpp:114-115`).
- The collection stays inside Macha's head blob, encoded at the SM16, SM17
  and SM19 position and in DLT9 (`metadata.cpp:856-876`, `:975-976`,
  `:1499-1507`). So the template is a library the FS codec and merge call.
  It is not a separate Dagda space for Macha.
- The coordinator's takeover (`torrent_coordinator.cpp:837-1126`) stays
  Macha's in the first port. It moves onto `Leases::acquire` only when
  renew and release exist.

## 6. The Macha port onto Dagda (`../dannan/TODO/ACTIVE.md` step 8)

Run in the extraction's order, seams 6 to 9. Each step runs on a
`feature/` branch off Macha's `develop`, with the port gate before and
after: the nine items of "The seam gate" (extraction 103-133).

Two items of that gate change under Dannan's wire v1:
- **Item 7**, wire and disk, cannot be "protocol 23 in the handshake".
  After Macha's port onto Dannan's transport (step 3) Macha speaks wire v1.
  The mixed-build test becomes "a ported node against a ported node built
  from the previous step" (open question 8).
- **Item 9** points at this document as well as the inventory.

### Step 8a: the reachability provider (extraction seam 7)

- **Changes.** `NodeHorizonBuilder` takes the provider of section 5.3
  instead of `CatalogueManager&` and `FileSystem&`. Macha registers one
  provider combining `FileSystem::maintenance_objects_cached`,
  `namespace_references` and the catalogue's `maintenance_objects` and
  `retention_objects`. The ledger stops including `catalogue/` and
  `filesystem/`. `horizon.hpp` stops including `metadata/metadata.hpp`;
  garbage becomes the provider's type. CONTROL GC moves from
  `CatalogueManager::control_gc_step` into Dagda's sweep, with the
  catalogue's protection as a provider answer.
- **Must not change.** The three gates (`contract/gates.hpp`), the grace,
  "a copy with any local claim is untouchable", and release only of dots
  the observed clock dominates (extraction 425-428). Also: an incomplete
  release is never published, and an inventory is never used destructively
  in the pass that built it.
- **Gate.** `test_horizon_builder.cpp`, `test_ledger_gates.cpp`,
  `test_claim_walk.cpp`, `test_catalogue_maintenance.cpp`; the GC cases in
  `test_storage_metadata.cpp` and `test_absent_nodes.cpp`; a claim from an
  unseen branch survives (extraction 430-433). Add an include-direction case
  that fails on any `ledger/` or `contract/` include of `catalogue/` or
  `filesystem/`.

### Step 8b: the lease template (extraction seam 6)

- **Changes.** `TorrentRequest` becomes the first instantiation of
  `dagda::lease::ClaimedCollection`, with identical bytes and answers.
  `merge_torrent_requests` is the template's merge.
- **Must not change.** The codec bytes, the bounds, cancel dominates, the
  claim order, the 7-day tombstone, and the duplicate rule landing as
  `duplicate_torrent` with the earlier `(created_unix_ms, id)` keeping the
  torrent.
- **Gate.**
  - `test_torrent_requests_join_without_conflicts` and
    `test_torrent_requests_ride_the_snapshot_and_its_deltas`
    (`test_storage_metadata.cpp:4660`, `:4856`);
  - `test_torrent_requests_belong_to_the_cluster_and_are_driven_by_their_owner`
    (`test_hydration_catalogue.cpp:2378`);
  - the acquisition cases;
  - a byte-for-byte round trip of the torrent section against a fixture
    captured before the step.
- **Correction to the extraction's gate.** It names "the SM15/SM16
  encoding" (extraction 410). SM15 is inline and no longer specified, so
  the fixture is SM16, SM17, SM19 and DLT9 (open question 5).

### Step 8c: opaque heads (extraction seam 8)

- **Changes.**
  - `MetadataSnapshot` divides into Dagda's head record and Macha's blob
    (section 5.1).
  - Macha opens `macha/namespace` every-node, with:
    - the codec: the tree-form encoders, as a function of both halves;
    - the merge: `metadata_merge.cpp` in `filesystem/`;
    - the provider from 8a;
    - the tree value codec: the leaf entry codec and the spine.
  - Dagda owns commits, acceptance, certificates, head sets, dots,
    truncation, the tree's branches and the reconciler.
  - `MetadataView` becomes Macha's facade.
  - Genesis becomes a constant per space (H27).
  - The per-delta whole-state encode is not carried over (section 7).
- **Must not change** (extraction 483-487, minus what the operator ruled out):
  - every byte of every tree-form record (SM14/16/17/19, DLT5-11), so that
    every existing hash verifies;
  - the acceptance rule as Macha has it (H4);
  - heads as a set;
  - reconciliation with no common ancestor;
  - conflicts as durable records;
  - idempotent retries by dot;
  - the truncation thresholds;
  - the tombstone batches.

  `macha-namespace-migrate`'s re-rooting is no longer a constraint: the
  tool is removed on the branch.
- **Gate.**
  - `test_metadata_merge.cpp`, `test_namespace_tree.cpp`,
    `test_namespace_service.cpp` (formerly `test_namespace_migration.cpp`),
    `test_storage_metadata.cpp`, `test_absent_nodes.cpp`,
    `test_namespace_entries.cpp`;
  - the `rpc_cluster` metadata cases, naming
    `test_metadata_history_checkpoint_concurrent_proposers_converge`;
  - `macha-metadata-dump` output identical on a fixture state directory;
  - the mixed-build test as redefined above: commits both ways, a
    divergence merged, the result identical from either side;
  - a space opened at `replicas: 3` on a five-node in-process cluster,
    proving H20 (extraction 477-481);
  - the soak on live-a and live-b (fi-1 no longer runs Macha).

### Step 8d: the context as the API (extraction seam 9)

- **Changes.** `SubsystemContext` (`subsystem/subsystem.hpp:41`) stops
  handing out `filesystem`, `hydration`, `ingest` and `local_state`. The
  FUSE and torrent plugins look them up by name. Macha's root
  (`service/node_services.hpp`) builds the FS through `dannan::Context`
  and `dagda::Context` alone. The blocking forms Macha calls are waits on
  completions.
- **Must not change.** Every supervised plugin behaviour, the FS's start and
  stop order, the fault sink and restart policy, and the FUSE `startup_stop`
  token (extraction 524-527).
- **Gate.** `test_subsystem_supervisor.cpp`, `test_fuse_subsystem.cpp`,
  `test_node_services.cpp`, `test_service_root.cpp`,
  `test_filesystem_fuse.cpp`. Macha's root compiles against the two
  contexts' headers alone. The FS runs as a hosted system on a two-node
  cluster with the full gate. Macha release `0.94.0`.

**Before step 8a.** The store itself (local stores, pool, domains,
distributed store, ledger, repair) is replaced by Dagda's under Macha's
names. The extraction's seam 3 is "dissolved into the pins" (extraction
13-14), so this replacement has no seam of its own.

It needs its own gate, proposed here:
- VALIDATION "Physical representation", "Admission and capacity",
  "Placement and the write floor", "Class separation", "Refusing
  unversioned storage" and "Memory";
- every crash-matrix row;
- `test_storage_v18.cpp`, `test_object_ledger.cpp`, `test_availability.cpp`,
  `test_io_pressure.cpp`, `test_presence.cpp`, the `test_invariants.cpp`
  durability cases, and the store cases of `test_rpc_cluster.cpp`;
- on a fixture state directory and backend, if v18 is kept, Dagda opening
  them and answering `has` and `get` for every object.

## 7. Do not reproduce

Revisited 2026-11-05. **Fixed in Macha 0.92.3, so no longer here:** the accounting
deadlock and scan races, the seed order, the claim check-then-delete windows, the
repair list identity, the future tombstone and the unkeyed delta record (section
3.7).

**What remains waits on the maintainer's ruling in Macha, and Dagda follows the result**
(the maintainer, 2026-11-04 night). Where a ruling changes a format or an answer, Macha takes
it first.

| Item | What Macha does | Where | What Dagda has now |
|---|---|---|---|
| Policy disagreement loops | Each node commits its own `dht.replicas` back, about 150 times a minute until configs match | `../macha/TODO/ACTIVE.md`; branch: `metadata_manager.cpp:1749-1768` | Nothing: heads |
| O(state) per delta | Replay and commit re-encode and rehash the whole snapshot | `metadata.cpp:3720`, `:3757-3758`, `:4225-4242` | Nothing: heads. Baselines are planned regardless |
| Allocations sized from the wire | A reserve sized only by a constant from the wire | `metadata.cpp:1951` | Nothing: heads |
| Settings reload | SIGHUP refuses replicas; a `write_copies` change is accepted and ignored | `service/service_config.cpp:23-25`, `cluster/cluster.cpp:1144-1152` | Backends and cache live, the rest at restart (M5). The old text here asked every setting to reload; that is withdrawn unless the maintainer rules it |
| I/O control reserve | No disk I/O reserve for control work | `cluster/data_work.hpp:23-24` | **Differs:** `IoReserve` exists. The maintainer ruled the control store's 256 MiB free-space reserve (taken by Macha at the port); the device reserve is on Macha's list |
| Zero storage reported healthy | `data_storage` ready with no backend online, and nothing says so | `local_state.cpp:85` | Ruled (the maintainer, 2026-11-05): ready, but degraded, in both. Built here: `Node.NoBackendOnlineIsReadyButDegraded` |
| A read error deleting a good object | Any read exception removes the object | `storage_pool.cpp:603` | Ruled (the maintainer, 2026-11-05): a read failing on I/O keeps it, in both. Built here: `StoreStoragePool.AReadErrorKeepsTheObject`; to conform to Macha's shape when it lands |
| Non-associative three-way torrent merge | Three concurrent duplicates settle only at the next merge | `5.4` | Kept in the template for byte identity |
| The lease fence | No epoch fence in Macha | `5.4` | **Differs:** Dagda's `LeaseKeeper` has one (new). Follows the ruling |
| The local barrier's deadline | `StoragePool::durability_barrier` waits with none | `storage/storage_pool.*` | Kept as Macha's, no deadline (M4) |
| Wall versus steady age | The CONTROL step compares a wall-clock age with a steady span | `COMPLETED.md`, findings | Kept as Macha's |

## 8. Open questions and contradictions

**The maintainer's answers, 2026-11-04** (they settle items 1 to 4, 7 and 9 below,
and the naming clash; the items keep their text as the record of the
question):

- **Four durability levels per write**, on every write (objects, head
  commits, claims, node-local spaces):
  1. **Ephemeral**: received in memory and queued; the call returns at
     once. Best effort only: it may be lost.
  2. **Persisted**: written to disk on at least one node.
  3. **Replicated**: written to disk on at least k nodes, k set per
     space (default 2).
  4. **Durable**: written to disk on all available nodes of the
     replication set. A node Dannan already sees as dead (not in
     `Nodes::active()`, the roster heard of within `dead_after`, or a
     "left" seen on `Nodes::watch()`, which reports within one heartbeat
     of a node going silent past `dead_after`) is not waited on and is
     named pending in the evidence; a live node that does not
     answer is waited on to the call's deadline, never longer. The write
     is never undone; evidence names who acknowledged and who is
     pending, and repair completes it.
  On a node-local space the upper three are the same.
- **Merges get the common ancestor** when Dagda still holds it:
  `(base?, a, b)`; an owner that ignores it (Macha today) is unaffected.
- **Lease and claimed-collection records keep field groups with their own
  clocks**, as Macha's `TorrentRequest` does, so its bytes stay identical.
- **Repair is diff only**: the cursor walks, the `repair_position` file
  and the `have_*` probes are not moved.
- **Legacy retention checkpoint readers are dropped now** (`MRTS0001`,
  `MRTS0002` and the other read-once legacy readers); the reserved slots
  stay so the formats remain byte-compatible.
- **The owner-supplied GC provider is `ReachabilityProvider`**; Macha's
  `ReferencedSets` keeps its name as the value type it returns.

1. **Acceptance.**
   - The plan (145) says a commit is complete when
     `metadata_min_write_replicas` distinct replicas hold it.
   - Macha accepts a local commit on its author alone (`required = 1`) and
     seeks `metadata_write_copies` only for a merge commit
     (`metadata_manager.cpp:831-920`, `:1311`; `docs/metadata.md:7`, `:24`).
   - The inventory row "acceptance at the write floor with a certificate"
     reads as the plan.
   - The membership row says a write-floor mismatch "fails closed so a
     weaker node cannot mint a certificate"
     (`../dannan/TODO/2026-11-03-macha-function-inventory.md:94`;
     `types.hpp:61-64`). In fact it does not
     (`test_nodes_seeking_different_copy_counts_share_one_namespace`).

   Macha's behaviour is what the port keeps. Does Dagda's floor mean
   "copies before acceptance" (the plan) or "copies a merge seeks" (Macha),
   per space?

   **Answered in part (the maintainer, 2026-11-03 night): every write carries a
   durability flag the caller chooses.**
   - **Ephemeral**: the write is queued and the call returns at once.
   - **Persisted**: written and acknowledged on at least one node.
   - **Durable**: written and acknowledged on every node in the
     replication set.

   The flag replaces a per-space floor as the thing a caller waits on:
   the space's replication set says where copies go, the flag says how
   many acknowledgements the call waits for. Macha's local commit on its
   author maps to Persisted.

   Clarifications put to the maintainer (the Dagda session's recommendations in
   brackets):
   - (a) **What "acknowledged" means.** [Synced to stable storage on that
     node (Macha's durable tier), not merely received.]
   - (b) **Ephemeral's guarantee.** [Still placed, replicated and repaired
     like any write, just not waited on; lost only if the writing node
     dies before it is flushed. The call's evidence says "queued".]
   - (c) **Durable with a replica down.** [The call waits to its deadline,
     never longer (discipline 2), and returns evidence naming who
     acknowledged and who did not; the write is not undone, and repair
     completes it later. Durable is "all acknowledged", not "all or
     nothing".]
   - (d) **Macha's floor between the two.** Macha waits for
     `min_write_replicas` (k of the replication set) before naming an
     extent in a commit, and Lir's PUBACK and Lugh's credential writes
     want "more than one, not necessarily all". Is a fourth level,
     "at least k" with k per space, wanted, or does such a caller ask
     for Durable over a replication set of k? [A per-space k under
     Persisted, so Persisted means "at least k, default 1".]
     The Lir session needs this level (`../lir/TODO/ACTIVE.md`): it
     acknowledges QoS 1/2 and retained updates at `write_copies`, default
     2 of 3. Persisted at one node loses an acknowledged message if that
     node dies before replicating, which breaks its failover; Durable
     stalls a whole shard while one holder is down, which defeats
     multimaster. Lir's mapping: QoS 0 Ephemeral, QoS 1/2 "k of N",
     Durable unused. It agrees with (a) and (c).
     The Lugh session's mapping (`../lugh/TODO/ACTIVE.md`): Durable for
     credential and factor changes, revocations, disables, groups,
     memberships, policies and the audit chain; Persisted for node-local
     spaces; no Ephemeral. It would move credential changes and
     revocations to "k of N" if added. Note: `lugh/scope` is every-node,
     so Durable there waits on every node in the cluster, and one node
     down holds each revocation to its deadline (it still lands
     everywhere reachable at once, per (c)).
   - (f) **Evidence and catching up.** [A write's evidence names every
     node of its replication set by Dannan node id with its state
     (acknowledged, pending, refused and why). Every write returns a
     ticket that outlives the call's deadline (plan 132): `status(ticket)`
     reads the current acknowledged set, `wait(ticket, deadline)` may be
     called again, and a ticket reports once more when the last pending
     node acknowledges or the write is known lost. That is how a caller
     clears "pending".] The Lugh session's use: its API reports a Durable
     write with pending nodes as success, "applied, n nodes pending",
     never as a failure; only a write applied nowhere, or refused, fails.
   - (e) **Scope.** [The flag applies to every write: objects, head
     commits, claims and node-local spaces. On a node-local space
     Persisted and Durable are the same, since its set is one node.]
2. **Three-way merge.**
   - The plan's merge signature is `(base?, a, b)`, and the extraction
     describes the torrent collection merge as three-way.
   - Macha's head merge uses no base ("a merge reads the two heads and
     nothing else", `docs/metadata.md:62`), and calls
     `merge_torrent_requests` with an empty base (`metadata_merge.cpp:354`).
     Only the catalogue-root merge uses a common ancestor, while history
     holds it.

   Does Dagda ever supply `base`, and from where once history is truncated?
3. **`ReferencedSets` is already a Macha class.** It is the value type the
   horizons inherit (`contract/horizon.hpp:26`), not the provider the plan
   names (plan 134, extraction 416). Rename the provider
   (`ReachabilityProvider`) and keep the value type?
4. **The lease template's LWW key.** The plan and the extraction say
   field-wise LWW by `(changed_ms, node_id)`. `TorrentRequest` has three
   field groups with their own clocks, and the phase group is ordered by
   epoch first (`torrent_request.cpp:168-199`). The template must take
   field groups, not one clock.
5. **Reserved slots and hashes.**
   - The Macha session says the entries and inline-garbage wire slots stay
     reserved so existing hashes verify.
   - In SM14 the entries block is replaced by `namespace_root` (not
     reserved). The inline-garbage count is still emitted. The DLT5-11
     entry slots carry the tree delta's paths.
   - Because replay re-encodes and compares (`metadata.cpp:3757-3758`), the
     codec must be byte-exact for every existing record. This conflicts with
     "verify in O(delta)" (section 7, item 3) unless verification stops
     re-encoding, for example by trusting the stored bytes, hashed as
     stored, and checking only the successor's tree root.
   - Also, the extraction's seam 6 gate names SM15, which is now out of
     scope.
6. **`legacy_clock`** is head-like (a clock) but read only by the FS merge,
   for entries without provenance. Under "no legacy", should a new space
   carry it at all? For Macha's space it stays in the bytes.
7. **Legacy retention checkpoints** (`MRTS0001`, `MRTS0002`,
   `retention.cpp:214-235`) and the voters, participants, branch-floor and
   baseline slots are legacy. They are read once or carried. Confirm Dagda
   drops the readers and keeps only the slots.
8. **Grandfathering and the wire.**
   - The Macha session plans no layout bump and no protocol bump.
   - Dannan's Decisions (2026-11-03 night) free Dannan of protocol 23 and
     make Macha's move to wire v1 a flag day.
   - Dagda's types register freely from 512 (the maintainer, 2026-11-04).

   So the store's wire cannot be protocol 23 on Dannan, and the extraction's
   mixed-build gates (seam 8 item 7: "a 0.91 node and a seam-8 node
   together") cannot run.

   **Settled (the maintainer, 2026-11-03 night):** formats stay compatible "because
   they work". Dagda keeps v18 on disk (section 4.4) and the store payload
   encodings byte for byte (packs, the tree snapshot and delta forms, the
   Merkle tree over keys), carried in Dannan's wire v1 framing under
   Dagda's numbers from 512; protocol 23's framing is gone (Dannan's flag
   day, agreed by the Dannan session). Macha's store code moves into Dagda
   and is generalised there. *Proposed (Dagda session):* the mixed-build
   gates are replaced by a gate that a ported node opens a 0.91 node's
   state directory unchanged. Still open: when Macha takes the flag day,
   which is the port's timing and the maintainer's.
9. **Repair's fallback paths.** Repair still has the cursor walks, the
   `repair_position` file and the `have_*` probes for peers surveyed "the
   old way" (replication-by-diff, stage 5 "Left"). With every node on
   Dagda, should Dagda ship the diff path only and drop the walks and
   probes? The doc's line 7 ("Nothing here is built") is stale against its
   own stage statuses.
10. **Inventory claims to correct.**
    - "`put` returns the holders and tiers" (inventory 184) is not true of
      Macha: `put` returns a `bool` or an id (S22). Evidence is new.
    - The inventory's "the namespace tree re-rooting" (195) and
      `test_namespace_migration.cpp` refer to the migration, which is
      removed on the branch.
    - The extraction's file table puts `service/claim_walk` and
      `ledger/node_horizon_builder` in the store, but `ledger/availability`
      and `service/availability_service` in Macha. The survey by diff (L8)
      straddles the two: the mechanism is Dagda's, the path table Macha's.
11. **Fencing.** The plan's lease guarantee is the receiver's epoch fence.
    Macha has none (E9). Macha's port does not need one, but Lir and
    AthenaSIP do, so Dagda builds it before either uses a lease.
12. **`StorageUnavailable`** (0.91.5) has no test (S23). A case is owed in
    Macha before the port can name it as a gate.
13. **The CONTROL GC step lives in the catalogue**
    (`catalogue.cpp:2355-2409`), not in the store. Moving it into Dagda
    (L20) changes which subsystem owns a destructive step. The extraction
    does not mention it.
14. **Placement of CONTROL for `replicas: N`.**
    - The extraction says `rendezvous_nodes(space, roster, N)`.
    - Macha offers CONTROL objects to every node and claims them on
      `metadata_write_copies` nodes ordered by latency (S29).
    - Lir asks that a space's replicas match its lease holder's placement.

    One placement function per space, with the owner's salt, is the
    proposed answer (H20).
