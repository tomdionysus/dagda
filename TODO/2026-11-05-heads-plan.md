# Dagda step 4: the heads plan

Status: draft, 2026-11-05. Nothing here is built. It plans `dagda/heads/` and what
goes with it (M1 in `ACTIVE.md`). It is read against Macha's
`feature/tree-native-namespace` at `b080c38`, and Macha's `develop` at `00861dd`.
`b080c38` branches from `5a06d3c`, so it does not contain develop's 0.92.3.

**What this rests on, and what it could not check.**
- Citations are `file:line` under `../macha/src/` at `b080c38` unless another root is
  named. Tests are `file:line` at `b080c38`. Every test name below was checked to
  exist there by a script; none was run. Nothing in Macha was built.
- The requirements document changed in the working tree while this was written
  (uncommitted, re-based on `00861dd`). Both were read. Its old "section 7 item 3" is
  now the row "O(state) per delta", which says baselines are planned regardless and
  the fix waits on the maintainer's ruling in Macha. "Macha's code as it is outranks the design
  documents" (the maintainer, 2026-11-05) is applied throughout.
- Citations will move at the merge: the branch still has to take develop, whose
  changes touch `metadata.cpp` (+4 at `:1735`, the DLT9 unkeyed check) and
  `maintenance.cpp`. "Settled at the merge" below means the row can change in
  substance, not only in line numbers.
- Basemind's MCP tools were not available to this session; lookups were by script over
  an export of `b080c38` under `build/scratch/`.
- Two sibling drafts appeared while this was written and were read for what touches
  heads: `2026-11-05-second-cutover.md` and `2026-11-05-test-mapping.md`. This plan
  follows the first's split (S, 8a and 8b deploy together on the flag day; 8c and 8d
  later) and differs from it in one place, B11, said at 2.2.
- Not verified: whether Macha's `node_.call` in `repair_unreconstructable_heads` has
  an effective bound (`metadata_manager.cpp:538`, no deadline argument; the bound if
  any is the transport's). Dagda gives it a deadline either way.

## 1. H1 to H30

Sessions are named in section 3: S1 the seed, S2 tree, S3 replica on disk, S4 one
node commits, S5 exchange, S6 reconciler, S7 conformance; L1 onward are after Macha's
path. "M" in the last column means settled at the merge. "PARTIAL" means the cases
named touch the row but none pins it whole. Paths: `md` = `metadata/metadata`,
`mgr` = `metadata/metadata_manager.cpp`, `tree` = `metadata/namespace_tree`.
Test files: `SM` = `test_storage_metadata.cpp`, `RPC` = `test_rpc_cluster.cpp`,
`MRG` = `test_metadata_merge.cpp`, `NT` = `test_namespace_tree.cpp`,
`NS` = `test_namespace_service.cpp`, `NE` = `test_namespace_entries.cpp`.

**45 of `SM`'s 69 cases use a legacy form in their bodies** (`genesis_metadata()`,
`encode_snapshot`, `seed`, `metadata_voters`, an entry map), so a name surviving the
merge does not mean its body did. They are to be re-read then (section 4).

| # | Requirement, restated | Code at `b080c38` | Macha cases that gate it | Sessions | M |
|---|---|---|---|---|---|
| H1 | A commit is immutable and content-identified: SHA-256 of generation, previous, length, payload; streamed; recomputed on validation. | `md.hpp:246-250`; `md.cpp:535-540`, `:2114-2121`, `:2302-2304` | `test_metadata_hash_streaming_matches_canonical_encoding` (SM:564). The requirements' SM:578 and SM:619 are other subjects. | S3 | |
| H2 | A body is a full record or a delta on its primary parent, whichever is smaller; a delta needs only parent generation below child's. | `md.hpp:290-293`; `mgr:1875-1886`; `md.cpp:488-514` | `test_metadata_dlt7_presence_flags_round_trip` (SM:2848), `test_metadata_dlt8_append_extents_delta` (SM:2938), `test_merge_delta_primary_may_precede_merge_generation` (SM:931), `test_a_tree_backed_delta_reconstructs_its_record` (NS:592). PARTIAL: no case found that pins "smaller wins". | S3 | M |
| H3 | Multi-parent: at most 64 merge parents, lower hash primary, generation max(parents)+1. | `md.hpp:190`; `mgr:1237-1241`, `:1257`; `md.cpp:2246-2247`, `:2268` | `test_metadata_branch_merge_history_roundtrip` (SM:619), `test_metadata_commit_store_acceptance_heads_roundtrip` (SM:737). NO CASE for the 64 bound. | S3, S6 | M |
| H4 | Acceptance: this node stores the exact commit and persists a certificate; a local mutation publishes to self; a merge seeks `metadata_write_copies` local first then nearest; peers get parent history before accept; the local replica is mandatory. | `mgr:177-185`, `:831-920`, `:1306`; `md.hpp:258-266`; `docs/metadata.md:18-28` | `test_commit_replicas_ordered_local_then_nearest` (RPC:4248), `test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim` (RPC:5591), `test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again` (RPC:5689) | S4, S5 | |
| H5 | Certificate rules: replicas at least required; repeats take min(required) and the union; sealed `DHTMACC1` in `heads.meta`; no vote. | `md.cpp:2131-2217`, `:3225-3246`, `:3353-3379`, `:4384-4395` | `test_metadata_commit_store_acceptance_heads_roundtrip` (SM:737), `test_accepting_a_held_head_again_rebuilds_nothing` (SM:2547) | S3 | M (floor 0 goes) |
| H6 | Heads are a set; an ancestor by history or clock is dropped; an empty clock is nobody's ancestor; preferred head is highest generation, then higher hash. | `md.hpp:558`; `md.cpp:3248-3301`, `:3430-3432` | `test_a_head_with_no_clock_is_nobodys_ancestor` (SM:1678), `test_a_head_from_before_truncation_is_recognised_by_its_clock` (SM:1697), `test_compacted_direct_predecessor_cannot_resurface_as_head` (SM:1353) | S3 | M (genesis clause `:3276-3283`) |
| H7 | Heads surveyed on every node, history imported from owners and witnesses, certificates accepted, 30 s cooldown for those that cannot be. | `mgr:922-943`, `:1076-1131` | `test_a_head_from_an_unknown_history_is_merged` (RPC:3670), `test_pristine_joiner_adopts_compacted_cluster_head` (SM:1539), `test_history_transfer_follows_only_materialization_dependencies` (SM:1519) | S5 | M (`discover_or_form` path) |
| H8 | Divergent heads folded pairwise in hash order under one lock; owner's merge; commit with merge parents; no ancestor needed; every reconciler computes the same commit. | `mgr:1137-1347`; `docs/metadata.md:60-71` | `test_concurrent_reads_during_divergence_produce_one_reconciliation` (RPC:3792), `test_disjoint_metadata_pairs_branch_and_reconcile` (RPC:4521), `test_three_heads_merge_to_the_same_namespace_in_any_order` (MRG:495) | S6 | M (mixed branch `:1214-1226` goes) |
| H9 | Tree reconciliation merges only what differs and expresses the result as a delta on the primary parent. | `tree.cpp:1934-2000+`; `mgr:1208-1213`, `:1245-1253`, `:1271-1279` | `test_a_tree_merge_is_the_merge_of_the_materialised_heads` (NT:1517), `test_reconciling_two_tree_backed_branches_keeps_the_namespace` (NS:487), `test_distant_changes_read_no_leaf_between_them` (NT:1489), `test_baseline_reconcile_materialised_against_tree` (NT:1640) | S6 | M |
| H10 | A merge that cannot be made sets aside the other head; times persisted in `set-aside.meta`; dropped after `garbage_grace`. | `mgr:1186-1189`, `:1341-1346`, `:963-977`; `md.hpp:559-571`; `md.cpp:3488-3556` | `test_a_head_that_cannot_be_merged_is_set_aside` (RPC:3739), `test_a_head_set_aside_for_the_horizon_is_dropped` (SM:1923), `test_the_set_aside_time_of_a_head_no_longer_held_is_forgotten` (SM:1982) | S3, S6 | |
| H11 | Conflicts are durable records, superseded by a later change, resolvable by a commit, GC roots while they stand. FS blob today; generic `conflicts(space)` later. | `md.hpp:106-120`; `md.cpp:299-354`, `:818`; `mgr:1851-1861`, `:1955-2000` | `test_metadata_conflict_alternatives_are_gc_roots` (SM:2140), `test_metadata_superseded_conflicts_leave_the_snapshot` (SM:3036), `test_metadata_merge_delta_preserves_standing_conflicts` (SM:3138), `test_concurrent_edits_install_the_later_and_keep_both` (MRG:162), `test_a_decision_is_kept_against_a_head_that_has_not_seen_it` (MRG:421), `test_standing_conflicts_are_listed_and_resolved_over_http` (`test_service_root.cpp:496`) | S6 (seam); L4 (generic) | |
| H12 | Dots: a clock of author to sequence; author and sequences persisted in `author.meta`; a new author whenever the chain cannot be shown; a retry keeps its dot and returns at once if covered. | `md.hpp:38-41`, `:164`, `:597-616`; `md.cpp:2551-2688`; `mgr:1686-1741`, `:1937-1948` | `test_an_author_sequence_never_regresses` (SM:2286), `test_a_broken_author_chain_starts_a_new_author` (SM:2312), `test_a_lost_author_record_starts_a_new_author` (SM:2346). PARTIAL: no named case for the retry; the requirements' SM:2364 is `test_protocol20_delta_preserves_governance_snapshot_encoding`, another subject. | S3, S4 | |
| H13 | `legacy_clock` judges entries without provenance. FS: only the FS merge reads it. | `md.hpp:169`; `metadata_merge.cpp:33-58`, `:183-186` | `test_a_returning_node_from_before_provenance_loses_nothing` (MRG:369), `test_entries_from_before_provenance_merge_by_the_legacy_clock` (MRG:337), `test_the_legacy_clock_survives_snapshot_and_delta` (SM:3512) | Owner's; carried in the blob; S7 pins the bytes | M |
| H14 | `mutate_delta`: pick the head (own head among several), decode, idempotency check, reserve a durable dot, refuse a policy change, owner stamps, bump clock, apply to tree, encode, claim, accept locally. | `mgr:1682-1953`, `:1012-1042` | `test_a_tree_backed_delta_reconstructs_its_record` (NS:592), `test_a_delta_applied_to_the_tree_lands_where_the_map_lands` (NT:733), `test_every_write_records_where_it_came_from` (NS:113), `test_operations_waiting_on_a_commit_share_the_next_one` (NS:419). The "unchanged payload" clause looks unreachable: finding F3. | S4 | M (`maybe_reconfigure`, `:1745`) |
| H15 | The replicator offers the newest head to every active node, history first; the caller does not wait. | `mgr:99-175` | `test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim` (RPC:5591), `test_a_file_touch_commits_while_a_holder_is_away` (RPC:4145), `test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` (RPC:3968), `test_metadata_commits_with_whichever_nodes_remain` (RPC:5079). The requirements' RPC:6364 is head repair. | S5 | |
| H16 | Each node truncates alone: one accepted head and past 256 records or 64 MiB, that head becomes the root; never with two heads. Journal compaction at 128 records or 8 MiB. | `md.hpp:769-770`; `md.cpp:4598-4631`, `:4921-4954`; `mgr:963-977` | `test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges` (RPC:6224), `test_history_is_not_truncated_while_two_heads_are_held` (RPC:6309), `test_metadata_history_payloads_are_disk_backed_and_byte_bounded` (SM:2636), `test_a_journal_compaction_holds_no_reader_of_the_replica` (SM:1867). The requirements' `test_metadata_history_checkpoint_concurrent_proposers_converge` does not exist: finding F1. | S3; stages S5 | M (`migrate_legacy_head_locked`) |
| H17 | Bounded memory: shared payload bytes, streamed hash, deltas in place, a materialisation cache (128 MiB, 64 entries) pinning current and committed. | `md.hpp:197-244`, `:689`; `md.cpp:1985-2105`, `:2114-2121`, `:3625-3672` | `test_metadata_record_payload_copy_is_shared` (SM:488), `test_metadata_delta_chain_reuses_bounded_materialized_head` (SM:2420), `test_metadata_materialization_cache_never_validates_corrupt_delta` (SM:2758); VALIDATION "Memory" | S3 | |
| H18 | The Merkle tree over keys: boundaries by hash of domain, level and key, target 32, max 128; a branch holds (first key?, id, count); node id is SHA-256; an empty map is one empty leaf. Generic branches, owner's leaves. | `tree.hpp:38-51`; `tree.cpp:18-101`, `:365`, `:409-428`; `metadata/namespace_control_store.cpp:18` | `test_the_root_is_a_function_of_the_entry_set_and_nothing_else` (NT:136), `test_changing_one_file_rewrites_a_path_to_the_root_and_not_the_library` (NT:159), `test_a_leaf_stays_small_however_many_extents_a_file_has` (NT:236), `test_a_corrupt_node_is_refused_rather_than_trusted` (NT:261), `test_an_incremental_update_produces_the_tree_a_rebuild_would` (NT:641), `test_an_update_reaches_the_root_a_full_build_reaches` (NT:1050), `test_an_update_reads_the_path_it_changes_not_the_tree` (NT:1137) | S2 | M (final forms) |
| H19 | Tree nodes are CONTROL objects; a commit names a root only once this node holds every node it wrote; only those are offered; replay rebuilds locally. | `metadata/namespace_control_store.hpp:17-57`, `.cpp:15-44`; `cluster/local_state.cpp:165-171`; `docs/durability.md:68` | `test_history_replay_applies_a_delta_to_the_tree_not_the_map` (NT:792), `test_a_commit_claims_the_nodes_it_introduced_and_prunes_the_rest` (NT:1203), `test_the_control_collector_keeps_an_unclaimed_namespace` (NS:662) | S2, S4 | |
| H20 | Replica policy per space: `replicas: N` or every-node at `open()`; holders by rendezvous; a non-holder forwards reads and says so. Lir: owner's salt and a reported replica set. | none: Macha has one every-node space | NO CASE (new; proof to write: three replicas on five nodes) | Interface S4; L2 | |
| H21 | Reads are memory reads on a holder. | `contract/metadata_view.hpp:79-86`; `docs/metadata.md:7` | `test_the_metadata_view_pages_a_live_namespace` (NE:187), `test_a_checkpoint_write_holds_no_reader_of_the_replica` (SM:1807). PARTIAL: none says "no I/O". The requirements' NE:88 and NE:242 are paging; NE:242's name is stale (F8). | S4 | |
| H22 | A commit claims what it introduces: DATA here, or on a node present, or refused; new CONTROL here; a merge under a fresh dot not in its clock. | `service/node_services.cpp:319-507`, `:509-525`; `mgr:1889-1898`, `:1282-1304`; `docs/durability.md:49-51` | `test_a_file_touch_commits_while_a_holder_is_away` (RPC:4145), `test_a_merge_claims_what_it_introduces` (NS:774) | S4, S6 | |
| H23 | Tombstone batches: one immutable CONTROL object per retiring mutation, named by the head, dropped whole after grace. FS what, generic retirement. | `md.hpp:76-104`; `mgr:1824-1845`; `service/maintenance.cpp:149-226`; `docs/metadata.md:119-131` | `test_tombstones_live_in_batches_in_a_tree_backed_head` (NS:336), `test_tombstone_batches_ride_the_head_and_its_deltas` (SM:4748), `test_trace_tombstones_maturing` (`test_maintenance_trace.cpp:479`) | S4 | M |
| H24 | Generation advertised and settled; heads announced when the set changes. | `metadata_server.cpp:54-64`, `:84-102`; `cluster/cluster.cpp:43`, `:731-744`, `:863`, `:877` | `test_service_metadata_repair_coalesces_real_generation_burst` (RPC:3145), `test_service_same_generation_sibling_notice_triggers_reconciliation` (RPC:3489) | S5 | |
| H25 | The cache seed: the last committed record kept in the cache, used to recover a head set, never authority. | `storage/persistent_cache.cpp:325-390`; `cluster/local_state.cpp:166`, `:172`; `metadata_server.cpp:95-96`; `md.cpp:2348-2357`, `:2384-2389`, `:2458-2499`; `docs/metadata.md:47` | `test_metadata_recovery_cache_seed_is_not_accepted_authority` (SM:2019), `test_storage_pool_and_persistent_cache` (SM:3699, the seed at `:3913-3927`), `test_metadata_codec_and_replica` (SM:3931, recovery at `:4333-4342`). `test_metadata_seed_sibling_replays_after_restart` (SM:518) is the legacy `seed()` API, another subject. | S1 (file), S3 (recovery) | M (what `valid_seed` decodes) |
| H26 | Per-peer ordered execution of history import, commit store and accept, on a bounded executor with a quarter kept for control. | `cluster/net.cpp:3810-3819`, `:3850-3870`; `cluster/net.hpp:239-241` | `test_rpc_metadata_mutations_use_bounded_isolated_executor` (RPC:2215), `test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers` (RPC:2361), `test_rpc_metadata_control_mutations_go_first` (RPC:2540), `test_rpc_metadata_executor_cancellation_disconnect_and_shutdown` (RPC:2614) | S5 | |
| H27 | Genesis: virgin founders build the same root, publishable by any; a constant per space, never decoded (the branch's direction). | `md.cpp:2290-2301`; `mgr:1446-1515`; `service/maintenance.cpp:513-532`; `docs/metadata.md:89` | `test_a_new_namespace_is_a_tree_from_its_first_commit` (SM:2617), `test_genesis_root_configuration` (SM:4514), `test_joiner_cannot_form_genesis` (RPC:2911), `test_bootstrap_joiner_requires_complete_checkpoint_survey` (RPC:2932), `test_a_durable_trio_founds_commits_and_reads_through_every_node` (RPC:5049) | S4, S5 | M (all of it) |
| H28 | Nodes advertise `metadata_write_copies`; a mismatch marks membership changed and does not fail closed. | `cluster/cluster.cpp:44-45`, `:870`; `types.hpp:60-64`; `mgr:1350-1381` | `test_nodes_seeking_different_copy_counts_share_one_namespace` (RPC:2993). NO CASE for "at most one policy commit"; that follows the maintainer's ruling (Q5). | S5 | M (`maybe_reconfigure`) |
| H29 | The scope check at the write floor: every replica that counts checks the writer's scope before storing. No source installed: node principals allowed. | none in Macha | NO CASE (new; Dagda's objects have `store_scope_test.cpp` as the pattern) | S5 | |
| H30 | No rollback: an ancestor of the held set changes nothing; an unrelated commit is a divergence, never a replacement. | `md.cpp:3248-3301`, `:4351-4367` | NO CASE as such (owed; Lugh's requirement a). Touching it: `test_compacted_direct_predecessor_cannot_resurface_as_head` (SM:1353), `test_a_head_from_an_unknown_history_is_merged` (RPC:3670). | S3 | M (genesis, legacy clauses) |

**Rows with NO CASE, whole or in part:** H20, H29 and H30 whole; the 64-parent bound
in H3; "at most one policy commit" in H28. PARTIAL: H2 (smaller wins), H12 (the
retry), H21 (no I/O). Each gets a `dagda_tests` case written first; H29's and H30's
are also proposed to the Macha session only if the ruling makes them Macha's.

## 2. Plans

### 2.0 The seams every plan below assumes

`dagda::heads`, in `dagda/heads/`, with a `Space` per opened head space. The owner
supplies, per `open(space, domain, codec, merge, policy)`:

- **`Codec`**: `decode_full`, `encode_full`, `encode_delta`, `apply_delta(parent state,
  delta bytes, tree)`, and `head_fields(state)`: the clock, merge parents and floor
  Dagda reads. For Macha these are a function of both halves, because head and FS
  fields interleave in SM14+ (requirements 4.3). State is an opaque shared handle, so
  a delta can share what it did not touch.
- **`Merge`**: `merge(base?, a, b) -> state | conflict`. Macha ignores `base`.
- **`TreeValues`**: the leaf value codec, what a value references, and the optional
  child tree (the extent spine). The domain strings are a parameter of the space.
- **`ReachabilityProvider`**: exists (`dagda/contract/reachability.h`).

**The test owner is a neutral record** in `dagda/test/fixtures/`, as the lease template
did. Q3 asks the maintainer to confirm, because `ACTIVE.md` step 4 says otherwise.

### 2.1 The `metadata` readiness plane

**Macha.** Stage "metadata" comes after control, cache and retention
(`cluster/local_state.cpp:161-174`). The plane is marked once `MetadataReplica` has
opened and replayed locally with no peer asked, and a failure fails startup with its
reason (`:179-182`). Status reports it as `startup.metadata`
(`api/status_api.cpp:828`). NO CASE names the plane: a search of `tests/` for
`metadata_ready` and `startup["metadata"]` finds nothing; `wait_local_state_ready`
covers it implicitly.

**Dannan 0.4.1** has what is needed: `Readiness::declare` until the node starts,
idempotent, and `ready` throws for an undeclared plane
(`dannan/status/readiness.h:35-42`).

**Plan.**
1. `plane_metadata` joins `readiness_planes()` (`dagda/node.cpp:53-56`), so it is
   declared at `:97` with the rest.
2. Stage "open heads" goes after `open distributed-store` (`node.cpp:216`) and before
   `open storage-server`. Macha's replay needs the control store and a
   `ControlObjectFetch` (`local_state.cpp:143`, `:169`); in Dagda that fetch is the
   distributed store's `ensure_control_local`.
3. Replay runs the owner's codec, so the plane cannot be marked honestly unless the
   space is known then. `NodeOptions::spaces` names the spaces whose replay readiness
   waits on; `Node::open` opens them in that stage and marks the plane after them. An
   empty list marks it at once, so a Lugh or Lir node with no boot-time space is not
   held. `Heads::open` opens further spaces at any time and they do not gate readiness.
4. A space that cannot open (primary state bad and no valid seed, `md.cpp:2385-2388`)
   is `NodeRefused` with the reason, through `Node::Node`'s catch, which calls
   `readiness().fail`.
5. **From the flag day to 8c, Macha's own `MetadataReplica` runs** (second-cutover plan,
   S.1). So `NodeOptions::heads` is an off switch: off, the node declares no `metadata`
   plane, registers no head type and runs no head stage, and Macha's facade declares and
   marks the plane itself, which `declare` allows for any hosted system. That keeps
   `startup.metadata` honest through the flag day. On is the default.

**Gate.** `node_test.cpp:239` and `:273` assert the planes by name and gain
`metadata` (changed, not removed). New: not ready before the seam's replay returns;
ready at once with no spaces; a refusal fails readiness naming the space; off, the
node declares nothing and an owner's own plane works.

### 2.2 The cache seed

**Macha, exactly.** `<cache>/metadata/current.meta`: magic `DHTCMET1`, nonce 12, tag 16,
then the ciphertext as a Writer blob (`persistent_cache.cpp:17`, `:344-350`). Sealed
under the domain's storage key (`identity.keys.storage`, `local_state.cpp:148`), AAD
the magic alone. The plaintext is `encode_metadata_record`: generation, previous, hash,
payload blob (`md.cpp:2122-2129`). Written atomically by `remember_metadata`, deduplicated
by hash, off when `prefer_metadata` is false (`:340`). Written at open from the committed
record (`local_state.cpp:172`) and on each acceptance that moves the committed head
(`metadata_server.cpp:95-96`). Read once at open and used **only** when the primary
state fails or a recovery marker stands (`md.cpp:2361-2391`, `:2457-2462`); then it is
never authority: no certificate, the recovery marker set, the author chain broken
(`:2465-2499`). A write failure is a debug line; a read failure is an absent seed.

**Dagda has** the attachment point and nothing else: `with_seed`, `SeedPlace{directory,
opening}` and `CacheOptions::seed` (`dagda/store/persistent_cache.h:57-64`, `:94-99`,
`:34`). `dagda-migrate` walks only `objects/` and `packs/` under a cache root
(`dagda/migrate/migrate.cpp:365`, `:380`), so today `current.meta` is left as it was,
sealed under an AAD no v19 reader has.

**Plan: `heads::CacheSeed`**, session S1, over `with_seed`: `remember(space, record
bytes)` and `load(space)`.
- It takes the **encoded record as opaque bytes**, so Macha's own replica can use it
  from the flag day to 8c, when the cache is already Dagda's and the manager is not. The
  second-cutover plan's B11 has the facade keep the seed itself in that place; this
  replaces that with a library call, so there is one writer of one format.
- Written and read under the seed lock, atomically by `durable_replace_file`, written
  again when `opening` moves (Macha resets its dedupe on reconfigure the same way,
  `persistent_cache.cpp:87`).
- v19's AAD: `"dagda/seed/19" || domain id || space id`, by the maintainer's "the domain in every
  AAD" and Lugh's binding requirement a. The magic in the file stays `DHTCMET1`, and the
  key stays the domain's (Q7).
- Per space: `<cache>/metadata/<space id hex>.meta`. Space id:
  `SHA-256("dagda/space/1" || domain id || name)` (proposed; Macha's name is
  `macha/namespace`).

**What `dagda-migrate` must do**, as a pass on `--cache`, after the cache's objects and
before the markers, forward and `--reverse`:
1. Look for `<cache>/metadata/current.meta`; nothing there is "seed: none".
2. Check the magic and open it under the domain key and v18's AAD. The key has already
   been proved on the objects.
3. Derive the tag for v19's AAD with `migrate::TagDeriver`; nonce and ciphertext are
   unchanged, as with every other sealed record. Write the new file by durable replace,
   then remove the old. Peak extra space: one record, small in tree form.
4. Resume: both files present means the old is redone and removed; new alone is
   "already". `--reverse` runs it backward and restores the file byte for byte.
5. A seed that opens under neither is **not a refusal**: listed and left, and the store
   treats it as absent. That is the crash matrix's "cache missing/corrupt: discard/rebuild"
   (`docs/durability.md`). The evidence gains `seed: migrated | already | unreadable |
   none`. `--dry-run` writes nothing.

**Why migrate rather than discard**, which would be simpler since the next open rewrites
it (`local_state.cpp:172`): the first open after a migration is the likeliest moment for
primary state to fail, and the seed is the only local recovery there is.

**Why this file can change on the flag day when `<state>/metadata/` cannot** (Q2): the
seed reaches Macha's replica through one call, as a record, so the replica never sees its
AAD. And the run is already migrating this cache, so it adds no disk event.

**Gate.** Macha's SM:2019, SM:3699 and SM:3931's seed parts, converted. New: another
domain's or space's seed does not open; off means neither read nor written; a corrupt
seed is absent; a moved `opening` rewrites; migrate forward, crash at each step, reverse
byte-identical, unreadable listed. Real gate: the Macha session's state directory, with
its cache.

### 2.3 `ordered` registration

**Dannan has it, at the tag Dagda pins.** `Registration.ordered` is in 0.4.1's
`net/routes.h`; `Routes::add` takes the registration (`dannan/context.h:149`). The
scheduler keeps a per-peer FIFO, one job per peer, and the same quarter for control
(`dannan/net/scheduler.cpp:108-114`), matching Macha (`net.cpp:3851-3859`). The default
limits are Macha's: 1 worker, 64 jobs, 256 MiB (`dannan/net/scheduler.h:36-38`;
`net.hpp:239-241`). Dannan's own cases prove the pool:
`Scheduler.OrderedWorkKeepsEachPeersOrderAndRunsPeersSideBySide`,
`Scheduler.ControlClassOrderedWorkGoesFirstAndKeepsAQuarterOfTheQueue`,
`Scheduler.ACancelledQueuedRequestNeverRuns`,
`Scheduler.StoppingFinishesRunningWorkAndDropsTheQueue`. So no request to Dannan.

`dagda/store/messages.cpp` registers no metadata type, ordered or not.

**Plan.** `dagda/heads/messages.{h,cpp}` with its own `register_types(routes)`, called
by `Node::open` beside the store's (`node.cpp:110`) under the same catch, unless
`NodeOptions::heads` is off (2.1). Session S5. Numbers are
512 plus Macha's (`cluster/net.hpp:28-125`), as the store's are; none clashes with
`dagda/store/messages.h`. Everything is on the CONTROL lane, as Macha's `lane_for` puts
it (`net.cpp:2027-2037`). `Frames::any`, because Macha calls these at control, read-ahead
and speculative.

| Type | Macha | Dagda | Ordered | Note |
|---|---|---|---|---|
| `put_metadata_history_entry` | 28 | 540 | **yes** | H26 |
| `put_metadata_commit` | 30 | 542 | **yes** | H26 |
| `accept_metadata_commit` | 31 | 543 | **yes** | H26 |
| `get_metadata_history_entry` | 26 | 538 | no | |
| `has_metadata_history_entry` | 27 | 539 | no | |
| `get_metadata_heads` | 29 | 541 | no | |
| `get_metadata_history_record` | 41 | 553 | no | head repair |
| `get_committed_metadata` | 10 | 522 | no | bootstrap survey, M |
| `metadata_notice` | 9 | 521 | no | `notification = true`, sent by `Channels::broadcast` |
| `metadata_reply`, `metadata_identity_reply`, `metadata_history_entry_reply`, `metadata_heads_reply` | 105, 108, 111, 112 | 617, 620, 623, 624 | no | `bool_reply` 615 is the store's, shared |
| `get_metadata`, `get_metadata_identity` | 6, 21 | 518, 533 | no | served at `metadata_server.cpp:110`, `:122`; no caller in `src/metadata` or the FS, only tests. Ask at the merge (section 4) |

**Three things this has to get right.**
1. **One registration, and the first cutover must set `ordered` too.** The first cutover
   re-registers Macha's own metadata types from 512 (evaluation, phase 2), and two
   registrations of a number are refused (`node.cpp:111-112`). So with `heads` off Macha's
   registration stands until 8c, and at 8c it goes and Dagda's takes the same numbers.
   **Macha's registration must set `ordered` on these three**, or H26 is lost a cutover
   early; it is one field, and nothing else forces it. Say so to the Macha session.
2. **Payloads byte for byte at those numbers**, so a mixed roll at 8c works (requirements
   step 8c's mixed-build gate). A space other than Macha's adds an optional trailing
   space id, as `principal` trails the object messages (`store/messages.cpp:95`, `:104`);
   absent means the node's default space. Macha's decoders call `reader.finish()`, so no
   trailing bytes ever go to Macha's space.
3. **An ordered handler must not wait on an ordered call.** Replay fetches tree nodes
   through `get_control_object`, which is not ordered, so the fetch is safe; a case pins
   that a commit whose replay needs a node from the same peer completes.

**Gate.** Beside `store_messages_test.cpp`'s recording route table: the three are
ordered and no other is; lanes and frames as above; a second registration is refused.
In a two-node cluster: Macha's RPC:2361 and RPC:2540 observed through Dagda's types.
RPC:2215 and RPC:2614 are retired against the Dannan cases by name, if the maintainer's mapping
says so.

### 2.4 Head-backed lease records, with renew and release

**Why this is not on Macha's path.** Macha's torrent requests stay inside its blob, as
a library the FS codec calls, and the coordinator's takeover stays Macha's in the first
port (requirements 5.4). So `LeaseRecords` over a head is Lir's and AthenaSIP's first.
The Macha session lists it in M1 because `LocalLeaseRecords` loses function compared with
a head; it does, but no Macha path reads Dagda's lease records. It is session L1, before
the tag.

**Plan: `heads::HeadLeaseRecords : lease::LeaseRecords`.**
- `scope` is the space (decided at step 5). A lease space is a Dagda-owned head space
  whose state is a `LeaseTable` (`dagda/lease/lease_table.h`) with its snapshot and delta
  sections and `LeaseTable::merge(base?, a, b)` as the merge.
- **In the blob, not the tree.** `LeaseRecords::read` is declared to wait on nothing
  (`keeper.h:100-102`), and the table is bounded at 65,536 records; a tree lookup can
  fetch a node.
- `join` is `mutate_local`, completing on the caller's executor with the record re-read,
  which is what `Acquisition::lost` already decides by. The write level is the space's.
- `Node::_lease_records` (`node.h:392`) becomes this. `LocalLeaseRecords` stays for its
  tests. `Context::Leases::acquire_waits` (`context.h:201`) widens to the head write's,
  and the "held in this node's memory alone" comment goes.
- **Release** needs no new bytes: a claim by nobody at epoch + 1 (`holder` all zero). The
  lease order gives the higher epoch the win (`lease.h`), so it survives a merge; an
  absent record could not, being the bottom, and a tombstone (`removed_unix_ms`, max
  wins, `claimed_collection.h:180`) would keep a later acquire out for the window. The
  keeper treats a zero holder as vacant; the next acquire is epoch + 2.
- **Renew** is Q6. A lease holds while its holder is a member, so there is nothing to
  extend. An earlier `since` wins within an epoch, so refreshing `since` at the same
  epoch loses to the record it refreshes; epoch + 1 would fence the holder's own actions
  in flight. Recommended: `renew` writes nothing, re-reads, and returns held or stands
  down, on the evidence shape of `standing`.

**Gate.** All new: there is NO CASE in Macha for renew or release. Across two nodes:
acquired on A, read on B after replication; A gone, B takes at epoch + 1 after the
absence and rank waits; A back stands down; a restart forgets nothing; both sides of a
partition acquire and the merge keeps the lease order's winner, the loser reporting
`lost`; release then acquire from either side; `EpochFence::observe` from merged records.
Macha's `test_torrent_requests_belong_to_the_cluster_and_are_driven_by_their_owner`
(`test_hydration_catalogue.cpp`) remains Macha's own.

### 2.5 The reference-count diff seam

**Dagda keeps the counters**: `ReferenceCounts::{reset, change, views}`, moved from root
to root "by the owner's diff" (`dagda/ledger/reference_counts.h:27-37`, `:56-61`).
**The diff has no home.** At Macha it is the FS census (`filesystem/filesystem.cpp:2497-2570`):
- `census_count`: same root, nothing to do; else follow; else walk.
- `census_follow`: `diff_namespace_tree_nodes` for branches and leaves, then
  `diff_namespace_trees` for the entries that differ, each entry's extents (holes aside)
  and spine nodes in and out, totals adjusted, `counts.change`.
- `census_walk`: a whole walk, `reset`, when there is no root, the counts refuse, or a
  tree it stands at cannot be read.

**Plan: `heads::ReferenceCensus`**, session S2, generic:
- `count(space, root)` is `census_count` moved. The tree diff is Dagda's already (H18,
  H9): `diff_tree_nodes` and `diff_tree` over keys.
- What a value references is the owner's: `TreeValues::references(value) -> {data,
  children}`. Macha's returns non-hole extents and `namespace_entry_spine_nodes`, and
  `Totals` keep their meaning (entries, references).
- Single owner, called under the provider's `inventory` and `release`, as Macha's
  `maintenance_build_mutex_` serialises it (`filesystem.cpp:2621`, `:2642`).
- One `ReferenceCounts` per counting space. Macha's stays at `<state>/ledger/referenced`,
  which is where the node opens it (`node.cpp:209`) and where `dagda-migrate` knows it;
  others take `ledger/referenced-<space id hex>` and are opened with the space. Views of
  several spaces reach the collector through step 6's union of providers, on the L list.
- Macha's FS keeps its census until 8c: `Node::references()` already gives it the
  counters, so nothing here is needed at the second cutover.

**Gate.** `test_the_inventory_and_the_media_index_follow_the_tree` (NS:198);
`test_inventory_control_set_holds_the_namespace_tree`,
`test_release_over_a_tree_matches_the_pass_build` and
`test_an_unreadable_tree_node_leaves_the_release_incomplete`
(`test_horizon_builder.cpp:189`, `:246`, `:306`);
`test_a_diff_is_the_paths_that_differ_and_reads_only_their_leaves` (NT:1448),
`test_every_tree_node_is_reachable_for_the_collector` (NT:1182). New, over the neutral
owner: follow equals walk after any sequence; a refused `change` walks; an unreadable
tree walks; a crash mid-change leaves dirty state and walks; a follow reads only what
differs (counted by the node store); a shared child (a spine two entries share) goes
only with its last reference.

### 2.6 Cost baselines for commit and verify

**What the sites are** (`md.cpp:3720` deep copy, `:3757-3758` and `:4225-4242` re-encode
and rehash per delta, `:2306-2320` and `:4197`, `mgr:719` and `mgr:1019` a full decode
for parents or a clock).

**An observation that narrows the question.** In tree form the namespace is not in the
payload: `encode_snapshot_v14` writes the root and no entries, and
`test_the_record_stops_growing_with_the_library` (NT:435) pins it. So re-encoding per
delta costs O(record), not O(keys). Byte-exact replay and "no growth with keys" do not
conflict. What grows the record is the clock (authors), `node_status`, conflicts,
tombstone batches (44 bytes each, retained for the grace) and torrent requests (up to
65,536). This reading comes from the code and was not measured.

**The baselines**, written with S3 and S4, in `heads_cost_test.cpp`, **asserting
counts, never time** (injected time, no sleeps). A `heads::CostCounters` is passed in:
tree nodes read and written, record bytes encoded and hashed, full decodes, parent states
copied, history frames written, syncs.

| Baseline | Fixed | Varied | Assert |
|---|---|---|---|
| commit keys | one-leaf change, one author | 1 k, 100 k keys | Nodes written and read at most depth-proportional; record bytes equal at both sizes; no full decode for parents; no parent copy. Fails if any grows with keys. |
| verify keys | the same delta, parent materialised | 1 k, 100 k keys | As above on the receiver: `store_commit`'s path. |
| replay keys | a chain of L deltas, L at most 256 | 1 k, 100 k keys | Bytes encoded proportional to L and independent of keys; one materialisation shared, as SM:2420. |
| merge keys | k differing keys | 1 k, 100 k keys | Reads follow k, as NT:1489; the owner's merge called on differing keys only. |
| accept repeat | a held head | | No materialisation, as SM:2547. |
| record axis | one-leaf change | batches, conflicts, authors | **Reports only**: `BENCH` lines in Macha's form (`test_baselines.cpp:35-39`), so Q4 has a number. |

A million-key run stays with M10's scale pass. Lir's 1,000 commits a second is L6.

**Gate.** S3 closes verify, replay and accept; S4 closes commit; S6 closes merge.

### 2.7 The maintenance stages, and where they join

**Macha's order at `b080c38`:** local-rebalance, retention-compact, **head-repair**,
**history-truncate**, compact-packs, scrub, then the verify
(`service/maintenance.cpp:1139-1199`, and on). `ACTIVE.md`'s `:1137` and `:1146` have
moved: `:1176` and `:1185` here, `:1144` and `:1153` at `00861dd`.

- **head-repair** (`:1175-1181`): `repair_unreconstructable_heads(speculative)`.
  **Always**, `busy` or not; a no-op unless a head is flagged; every peer tried for a full
  body, `reanchor_history` (`mgr:516-569`); failure is a debug line.
- **history-truncate** (`:1183-1191`): only when `!busy`. Takes the mutation lock, so a
  commit waits for it (`metadata_view.hpp:182-186`); expires set-aside heads, then
  `compact_history_if_safe(256, 64 MiB)` (`mgr:963-977`).

**They join Dagda's `Node::local_stages_locked` between retention and packs.** That means
splitting `node.cpp:538-545`, one `if (!busy)` over both, in two:

```
538  if (!busy) { retention compaction }
     head repair            // any activity; its own try
     if (!busy) { history truncation }   // its own try
542  if (!busy) { DATA packs }
```

- Each in **its own try**, as Macha's are, so a failed head repair skips no compaction;
  today one try covers all the stages (`node.cpp:523`, `:588`).
- `forced` makes `busy` false as now, so `run_maintenance_once` truncates.
- `stop` goes in: head repair checks it between heads and peers. `_maintenance_mutex` is
  held across this and the stop joins the pass (the fix awaited from Macha, `ACTIVE.md`
  "Waiting on Macha's fix"). Each peer has a deadline (`store.control_call`), so a pass
  costs at most flagged heads times peers times that; the flagged list is bounded per
  pass so a bad head set cannot hold the local stages.
- `LocalStages` gains `heads_repair_ran`, `heads_repaired`, `history_truncate_ran`,
  `history_truncated`, and `dagda.maintenance.*` gains the figures.
- Every opened space is visited. With one space that is trivial; L6 deals with 768.

**When.** Session S5, with `heads` on. Until 8c Macha's own loop keeps these stages while
the local stages go to the node (second-cutover plan, S.1), so no seam is needed for the
flag day. Waits as Macha declares them: `state_device | network | locks`, and
`state_device | locks` (`metadata_view.hpp:179-186`).

**A third stage, beyond the two asked about.** `metadata-repair`
(`maintenance.cpp:510-594`) is heads work too and is what actually converges replicas:
`repair_step()` when the metadata is dirty, on events and a backoff, with the virgin
founder rule (`:513-532`: exactly one deterministic founder), `note_replica_validation`,
and non-destructive maintenance continuing when it fails unless recovery is required or
the generation is at most 1. It runs **first** in the pass, ahead of GC. Without it a
node converges only by the replicator's offers. So `Node::maintenance_loop` calls it
before `gc_pass_locked`. The founder clause is settled at the merge.

**Gate.** New: called in Macha's order; repair runs while busy, truncation does not; a
throw in either leaves the rest running; `stop` respected; forced runs them. From Macha:
`test_unreconstructable_accepted_head_is_repaired_live_from_a_peer` (RPC:6379),
`test_history_is_not_truncated_while_two_heads_are_held` (RPC:6309),
`test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges` (RPC:6224),
`test_failed_convergence_retries_once_per_burst_of_events` (`test_rpc_cluster.cpp`),
`test_convergence_demand_coalesces_burst_and_keeps_same_generation_event`
(`test_invariants.cpp`). `test_maintenance_trace.cpp` stays Macha's.

## 3. Sessions

**How "second cutover" is read here.** The maintainer: two cutovers, transport first, then the disk
migration as one flag day (`ACTIVE.md` Decisions). The second-cutover plan deploys steps S,
8a and 8b together on that day, with Macha's replica and manager still running, and 8c and
8d later with no object rewrite.

So the flag day needs of step 4 only **S1**: the seed, because `dagda-migrate` defines
that disk, and the off switch. Everything else Macha needs is for 8c: S2 to S7, in the
order each depends on the last. Lugh's and Lir's come after.

All but S1 wait on Macha's merge. S1 touches no tree form and no genesis, treating the
record as opaque bytes, so it could run before the merge (Q9). Each session writes its
tests first, has its own `feature/heads-*` branch and leaves a handoff in `COMPLETED.md`
with gate counts. The gates below name what pins each row. The whole list is the
second-cutover plan's 8c.4 and `2026-11-05-test-mapping.md`'s 95 GAP-HEADS cases, at
`00861dd` names: at the merge each is assigned to a session here, or retired by name, so
a count that drops is a finding.

### Macha's path

| Session | What | Rows | Gate |
|---|---|---|---|
| **S1. The seed, for the flag day** | `CacheSeed` and the migration's seed pass (2.2); `NodeOptions::heads`, off (2.1); `docs/migration.md` updated | H25 (file) | 2.2's cases, and the off switch's from 2.1. `test_metadata_recovery_cache_seed_is_not_accepted_authority` still passes in Macha through the facade. `dagda-migrate` over the real v18 state directory, `metadata/` left byte for byte and the cache seed migrated and back, when the Macha session cuts it. |
| **S2. Tree over keys** | `heads/tree`: build, update, diff, node diff, lookup and scan, value codec, child-tree hook, domain strings as a parameter; the CONTROL node store (read, commit, replay modes); `ReferenceCensus` (2.5) | H18, H19, part of H9 | The NT cases on keys and structure, converted over the neutral values: NT:78, :136, :159, :218, :261, :641, :704, :874, :1003, :1050, :1137, :1182, :1448, :1489. 2.5's. Macha's vectors for branch nodes and boundaries (section 4). |
| **S3. Replica on disk** | Record and hash; history log, journal, checkpoint; certificates and `heads.meta`; head set, ancestor rules, no rollback; authors and dots; set-aside times; truncation and compaction; materialisation cache; seed recovery; owner's codec interface; Macha's file set and sealing per Q2 | H1, H2, H3 (format), H5, H6, H10 (store), H12 (authors), H16, H17, H25 (recovery), H30 | SM:488, :564, :619, :737, :931, :1353, :1519, :1678, :1697, :1807, :1867, :1923, :1982, :2019, :2286, :2312, :2346, :2420, :2547, :2636, :2758, :4564, as they stand after the merge. New cases: H30 and the 64-parent bound. Baselines: verify, replay, accept. |
| **S4. One node commits** | `Space`, `mutate_local`, retries by dot, policy refused, claims barrier, retirement through the provider, genesis constant, `Context::Heads` (`open`, `read`, `mutate_local`, `heads`), policy type carrying `replicas: N` and salt, refused with a reason until L2; plane from real replay | H4 (local), H12 (retry), H14, H19, H20 (interface), H21, H22, H23, H27 (constant) | NS:113, :336, :419, :592, :774, NT:733, :792, :1203, SM:4748, RPC:4145's single-node part, SM:2617. New cases: H12's retry, H21's no I/O. Baseline: commit. |
| **S5. Exchange** | Message types with `ordered` (2.3); server handlers; history push and pull with its window; head survey and import; merge's sought copies (`Replicated` at the store's `metadata_write_copies`); replicator and owed claims; stalled peers; notice and generation advert; the three maintenance stages (2.7); founding; the scope check on commit and accept; M6's re-ask on the write-copies advert; `dagda.heads.*` and options for the second-cutover plan's 8c.2 and 8c.3 | H4, H7, H15, H16, H24, H26, H27, H28, H29 | 2.3's and 2.7's cases. RPC:2361, :2540, :2911, :2932, :2993, :3047 (`test_two_node_mutual_bootstrap_metadata_write_floor`), :3145, :3489, :3670, :3968, :5049, :5079, :5591, :5689, :6224, :6309, :6379. New: H29's, modelled on `store_scope_test.cpp`. |
| **S6. Reconciler** | Fold under one lock; `merge(base?, a, b)`; merge by what differs; set-aside on `MergeUnavailable`; claims under a fresh dot; own head; conflicts as owner's blob, with GC roots through the provider | H3, H8, H9, H10, H11 (seam), H22 | RPC:3739, :3792, :4521; MRG:495 and :162 in the neutral owner's terms; NS:487; NT:1517, :1640; SM:2140, :3138, :3224 (`test_metadata_delta_child_of_merge_commit_reconstructs`). New: a stated determinism case, and Q8's. Baseline: merge. |
| **S7. Conformance on Macha's disk** | Vectors from Macha's merged code; a space opening Macha's `<state>/metadata/` as it stands (Q2); dump output on the fixture | H2, H13, H18 as bytes | Every vector: SM14, 16, 17, 19, DLT5 to 11, tree nodes, hashes (section 4). A real state directory: every head and certificate opens under Dagda, unmoved, and a commit extends it. `macha-metadata-dump` output identical on the fixture, from a Macha build at the port. The 8c mixed-build case stays at the port, Macha's. |

That closes M1 for Macha.

### After it, before the tag: Lugh, Lir and the contract

None of these is on Macha's path, and none may hold S1 to S7. `Context` v1 freezes
(step 7, tag `0.1.0`) after them, because they change the contract. The S4 policy type is
what stops them from breaking it.

| Item | What | For | Note |
|---|---|---|---|
| **L1** | Head-backed leases, renew, release (2.4) | Lir, AthenaSIP | Macha's coordinator moves onto it only later (5.4). |
| **L2** | `replicas: N` holders by rendezvous, with **Lir's placement salt in `open()`** (a Dannan `placement::Scheme`; stack doc decision 20) and the **replica set returned as evidence** from the Heads noun; forwarding reads from a non-holder; CONTROL placement to match (requirements open question 14) | Lir; AthenaSIP; KV | H20's proof: three replicas on five nodes. ACTIVE step 4 and requirements 8c list this proof in their gates. It proves nothing for Macha's every-node space, so recommended: it gates the tag, not 8c. |
| **L3** | The Replicated level's **per-space k** | Lir (QoS 1/2 at k of N), Lugh | Macha's merge uses the store's `metadata_write_copies`, already an option (`distributed_store.h:230`). |
| **L4** | Generic `conflicts(space)`, `resolve`, a merge's `conflict` return made durable | First non-FS owner | H11. Macha's stay in its blob. |
| **L5** | **Node-local spaces** (one node, never gossiped, under the node-local domain) | Lugh, Lir's `seq` | The same `Space`, with no exchange. |
| **L6** | **Scale to Lir's load**: 768 spaces, about 1,000 commits a second (stack doc). Known problems: Dannan's 64-advert cap does not fit a per-space generation advert (finding F5); one ordered worker per node at one sync per commit; the local stages' per-space visit needs a dirty list | Lir | Measured against the stack doc's figures. May need a request to Dannan. |
| **L7** | **Retained values**, on Dannan's notify channels plus a head | Lir | Requires L5 only if Lir's design says so. |
| **L8** | The domain catalogue as a head space; blind replicas on the wire and in placement; maintenance on a blind pool; several providers per domain unioned; the wrap advert's certificate | Lugh (`lugh/secrets`) | Step 6's remainder. A blind node cannot run an owner's `apply_delta`, so what it holds of a head is an open design question for the maintainer. |
| **L9** | Lugh's no-rollback case for node-local spaces | Lugh | H30's case, repeated on L5. |

## 4. What is needed from the Macha session at its merge

Say which commit merged, then re-check these against it. Dagda then re-runs its name
check over section 1 and updates every row marked M.

1. **Genesis.** The constant's value, how it is defined, and its generation. Everything
   that keys on it: `accepted_head_is_ancestor_locked`'s clause (`md.cpp:3276-3283`),
   `migrate_legacy_head_locked` (`:3303-3351`), `accept_pristine_genesis_authority_`,
   `discover_or_form` (`mgr:1446-1515`), the `generation <= 1` tests (`mgr:1523`, `:1548`,
   `:1600`, `:1708`, `:2026`), and the founder rule (`maintenance.cpp:513-532`). How does
   a founder's `root_uid`, `root_gid` and `root_mode` (`mgr:1482-1484`) reach the first
   commit if genesis is a constant, and what if founders are configured differently?
2. **Snapshot forms.** Which of SM14, 16, 17, 19 are written, the magics (`md.cpp:22-41`),
   `encode_snapshot_v14`'s field order (`:984-1071`) and which slots stay reserved: voters,
   inline garbage, participants, branch floor, baseline.
3. **Delta forms.** DLT5 to 11's selection (`:1378-1405`), which slots stay, and
   `encode_snapshot_for_delta` (`:488-514`).
4. **Tree forms.** `MNL1`, `MNB1`, `MNX1`, `MNY1`; `NamespaceTreeLimits`; the domain
   strings, including `.../extent/v1` (`tree.cpp:188`), which requirements 4.3 omits;
   the provenance bit.
5. **Legacy authority.** That every `required == 0` branch is gone (`:2141-2144`,
   `:2182-2187`, `:3357-3369`, `:4284-4287`, `:4385`), and `cas`, `cas_delta`, `seed`,
   `install_committed_delta`, `remember_committed`, `remember_current_committed`.
6. **The manager.** `maybe_reconfigure` (`mgr:1350-1381`): does `data_policy_change`
   survive, and its inline branch? `read_group`'s mixed branch (`:1214-1226`). The
   `namespace_root` refusal (`:1745-1746`). Any change to `publish_commit`.
7. **`<state>/metadata/`.** The final file set. Does the old layout path
   (`current.meta`, `committed.meta`, `.v10`, `md.cpp:2408-2456`) and
   `mutation-sequence.meta` (`:2589-2604`) go? The magics and AADs of what stays.
8. **The seed.** Whether `persistent_cache.cpp:325-390` or `valid_seed`'s decode
   (`md.cpp:2348-2357`) changed.
9. **Messages.** Which of `cas_metadata`, `seed_metadata`, `checkpoint_metadata`,
   `commit_metadata`, `cas_metadata_delta` go, with numbers kept. Do `get_metadata` and
   `get_metadata_identity` have a real caller, or are they retired?
10. **Maintenance.** After taking develop: the lines and order of `metadata-repair`,
    `head-repair`, `history-truncate`, and any new stage. Macha's stop fix, whose shape
    Dagda takes.
11. **Tests.** Names retired, renamed or newly added, especially: the 45 `SM` cases on
    legacy forms; `test_metadata_seed_sibling_replays_after_restart` and
    `test_protocol20_state_rejects_legacy_authority_mutators` (legacy API);
    `test_a_manager_without_a_store_pages_a_map_backed_view` (F8); the genesis cases;
    and the real name to put in H16 and 8c's gate instead of the missing one (F1).
12. **Fixtures**, under `build/macha-fixtures/step4-*/`, README naming the commit and
    config, as M9's placement vectors were done:
    - bytes and hash of a record in each of SM14, 16, 17, 19 and DLT5 to 11, including
      a chain with its parent and successor;
    - tree nodes with their ids, and the boundary known answers per domain string;
    - the genesis constant;
    - `macha-metadata-dump` output on the fixture state, and `--stats`;
    - a real state directory, `metadata/` and the cache's `current.meta` included.
    Data only: Dagda holds no torrent code, so the torrent sections in SM16, 17, 19 and
    DLT9 are bytes to it.
13. **For the two cutovers.** That the first cutover's registration of the metadata types
    sets `ordered` on the three mutations, at 512 plus Macha's numbers (2.3). That from the
    flag day Macha's replica takes its seed from `heads::CacheSeed` (2.2).

## 5. Questions for the maintainer, each with a recommendation

- **Q1. Must the flag day wait for heads?** The second-cutover plan's section 0 item 6 lists
  M1 among what must be true first, and the evaluation's phase 1 tags Dagda 0.1.0 before
  phase 2; its question 1 recommends the opposite, not holding the flag day. *Recommended:
  not held.* Only S1 is needed, with `NodeOptions::heads` off and a Dagda tag or pin that
  does not claim `Context` v1 is frozen. Rule on it, since it decides what Macha pins.
- **Q2. `<state>/metadata/` under v19.** It is sealed under domain v1's key with magic AADs;
  `dagda-migrate` leaves it "until heads define it" (`docs/migration.md:57`). The maintainer ruled one
  flag day, and one layout with the domain in every AAD and the ledgers under the node-local
  key; these pull apart here. *Recommended, as the second-cutover plan's question 1:* at 8c
  Dagda reads Macha's files as they stand, by name, key and AAD. New spaces take
  `<state>/heads/<space id hex>/` with the same files, AAD magic plus domain and space. A
  later release moves Macha's across with `dagda-migrate`, peak extra one file (history is
  bounded near 64 MiB). Until then those files do not meet the one-layout rule, and that is
  the cost. It also keeps `macha-metadata-dump` and `macha-metadata-repair` working
  unchanged at 8c.
- **Q3. Macha's FS codec in Dagda's tests.** `ACTIVE.md` step 4 says it lives there. The
  rulings since say Dagda holds no torrent code "in the library or its tests", and that
  Macha's tests pin the torrent bytes (M7, Decisions), yet SM16, 17, 19 and DLT9 contain
  the torrent section. *Recommended:* a neutral owner in Dagda; Macha's records as opaque
  vectors for what is generic (hash, chain, certificate, tree branches); the FS codec's
  bytes proved by Macha's own tests at 8c.
- **Q4. O(state) per delta.** Waits on your ruling in Macha. *Recommended:* keep the hash
  and byte-exact re-encode; assert no growth with keys now; read the record axis from the
  `BENCH` lines before ruling. True O(delta) means hashing over a delta and root, so a
  new commit identity and every head hash changing; not worth it unless the record axis
  says so. The row "Allocations sized from the wire" cites `metadata.cpp:1951`, which at
  both `5a06d3c` and `b080c38` is `metadata_delta`'s node-status loop, not an allocation;
  I could not tell which `reserve` it meant. Bounding every reserve by the bytes remaining
  costs Dagda nothing and needs no format change, so S3 does so.
- **Q5. Policy disagreement.** Macha's shape stays until you rule; S5 copies it. A policy
  commit at `mgr:1376` also carries no dot, so its clock equals its parent's and, once
  history is truncated, only a merge can collapse it (`md.cpp:3296`). *Recommended:* the
  old section 7 proposal, set at `open()` and changed only by an explicit commit, fixed
  in Macha first.
- **Q6. What `renew` means.** See 2.4. *Recommended:* it writes nothing and reports standing.
- **Q7. The seed on the flag day.** *Recommended:* migrate it as 2.2 says: domain key, domain
  and space in the AAD. It meets the one-layout rule at no extra disk event, keeps the tag
  derivable, and a blind node, having no key, keeps no seed. The alternative is to leave it
  byte for byte with `<state>/metadata/` and move both in Q2's later release: less to build,
  and the literal rule unmet for one more file.
- **Q8. `base` and determinism.** History truncates per node, so two reconcilers may differ
  in holding the ancestor. A merge that uses `base` then can yield sibling merge commits,
  against H8's "every reconciler computes the same commit". Macha is unaffected: it passes
  none, and the catalogue's use is outside the commit. *Recommended:* state it in the
  contract; pin with a case that siblings converge rather than loop.
- **Q9. May S1 start before Macha's merge?** `ACTIVE.md` says step 4 waits, and the flag day
  waits on fixtures cut after the merge anyway. S1 depends on no tree form or genesis.
  *Recommended: yes*, if the record's envelope (`encode_metadata_record`) is confirmed
  unchanged by the merge.

## 6. Findings (reported, not fixed)

- **F1 (requirements).** `test_metadata_history_checkpoint_concurrent_proposers_converge`,
  named in H16 and 8c's gate, is not in `tests/` at `b080c38` or `00861dd` (the
  second-cutover plan found the same). It survives in Macha's `TODO/ACTIVE.md:111` at
  `b080c38` and in archives. It appears to belong to a checkpoint round Macha has since
  removed; I inferred that from its archived siblings' names and did not trace it.
- **F2 (requirements).** Proof cells on other subjects: H1 (SM:578, :619), H12 (SM:2364),
  H15 (RPC:6364), H25's unnamed cell. Section 1 gives the corrected names.
- **F3 (Macha, by reading; not run).** `mutate_delta`'s no-op (`mgr:1864-1867`) looks
  unreachable. `reserve_mutation_dot` always returns a sequence above what the head shows
  (`md.cpp:2662-2664`) and `:1794` writes it into the clock, which is in the payload, so
  `payload == current.payload` cannot hold. So a mutation that changes nothing still
  commits. `resolve_conflict` on an unknown id (`mgr:1960-1963`) returns false having made
  a commit. Send to the Macha session; "Macha's behaviour is the requirement" until it
  answers, and H14's clause should say which.
- **F4 (requirements).** 4.3's boundary domains omit `.../extent/v1` (`tree.cpp:188`).
- **F5 (requirements against Dannan).** H24 and `ACTIVE.md` have "a Dannan advert per
  space", but adverts are capped at 64 per node (`ACTIVE.md`, "Dannan, as Dagda finds it").
  Lir's 768 spaces do not fit. Macha's one does.
- **F6 (`ACTIVE.md`).** Step 4's "Macha's FS codec and merge live in the tests" conflicts
  with later rulings (Q3). Its citations `:1137` and `:1146` are stale. `metadata-repair`
  is missing from what "comes with heads". `ACTIVE.md:490`'s `node.cpp:355` for
  `higher_class_active` is now `:381`.
- **F7 (stack document).** "Wire and formats" and "Settled earlier" still say layout v18,
  against the maintainer's 2026-11-04 night ruling.
- **F8 (Macha, cosmetic).** `test_a_manager_without_a_store_pages_a_map_backed_view`
  constructs a manager with a store, which is now required. `mgr:1157-1159`'s comment
  (transition "at max(old_floor, new_floor)") does not match `:1366-1367`, which sets 1.
- **F9 (second-cutover plan).** Section 0 item 6 and question 1 disagree on whether the
  flag day waits for heads (Q1). Its B11 is replaced here by `CacheSeed` (2.2).
