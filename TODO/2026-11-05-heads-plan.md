# Dagda step 4: the heads plan

Status: re-based 2026-11-06 on Macha `9a08846` (0.93.4, live on both nodes by
`build/macha-fixtures/goldens-0.93.4/README.md`; Macha's own `TODO/ACTIVE.md` at that commit still says
0.92.5, finding N7). Nothing here is built. Macha is frozen at `9a08846` until the port. The goal this
plan serves: Dagda can answer **Yes, unconditionally**, to whether Macha moves onto Dagda with no change
in data, functionality, reliability and performance. A Yes is a named case or a named retirement for
everything below; where there is neither, section 6 says what to write.

**What this rests on, and what it could not check.**
- Citations are `file:line` under `../macha/src/` at `9a08846` unless another root is named, read from a
  `git archive 9a08846` under `build/scratch/`. Macha's tree was not built, edited or switched. Basemind
  was not used; lookups were by script over the export.
- Every test name below was checked by script to exist at `9a08846` at the line given. **No Macha test was
  run.** Python did run over the scratch fixture, under the fixture's own test key, in section 9.
- Bodies read in full at `9a08846`: `test_a_new_namespace_is_a_tree_from_its_first_commit`,
  `test_genesis_root_configuration`, `test_accepting_a_held_head_again_rebuilds_nothing`,
  `test_a_tree_backed_delta_reconstructs_its_record`,
  `test_a_manager_without_a_store_pages_a_map_backed_view`, `test_metadata_identity_rpc`,
  `test_local_metadata_store_falls_back_from_invalid_delta`,
  `test_metadata_availability_logs_only_transitions`,
  `test_a_commit_is_published_only_once_its_objects_are_claimed`,
  `test_nodes_seeking_different_copy_counts_share_one_namespace`,
  `test_metadata_commits_with_whichever_nodes_remain`,
  `test_metadata_recovery_cache_seed_is_not_accepted_authority`, `test_a_head_with_no_clock_is_nobodys_ancestor`,
  `test_compacted_direct_predecessor_cannot_resurface_as_head`, `test_the_metadata_view_pages_a_live_namespace`,
  and the first parts of `test_replication_policy_change_on_restart` and `test_metadata_delta_state_model`.
  **The other gate cases were checked for name, size and assertion count only**, with the commit messages
  of `076115f`, `18f0c4b`, `ecce886`, `766617e` saying what moved. Each session reads its own cases'
  bodies before it claims a row; a proof cell here says what the case is for, not that I compared it.
- Not verified: the live nodes' `<state>/metadata/` (no ssh). "Replay of a copy of live-a's real state
  reproduces every frame, root 2fdd09bd" is Macha's note (`TODO/ACTIVE.md:27-28`), and "the live heads hold
  23 torrent requests" is its torrent plan's (`:14`). I saw neither state.
- Not verified: `control_no_progress_deadline`'s value on `repair_unreconstructable_heads`' call
  (`mgr:532`, bounded by `cluster/cluster.cpp:667-668`, `:640-645`; the value was not traced). Dagda gives
  that call a deadline either way.

## 1. H1 to H30

Paths: `md` = `metadata/metadata`, `mgr` = `metadata/metadata_manager.cpp`, `tree` =
`metadata/namespace_tree`. Test files: `SM` = `test_storage_metadata.cpp`, `RPC` = `test_rpc_cluster.cpp`,
`MRG` = `test_metadata_merge.cpp`, `NT` = `test_namespace_tree.cpp`, `NS` = `test_namespace_service.cpp`,
`NE` = `test_namespace_entries.cpp`. `tests/test_namespace_migration.cpp` is gone: 10 of its 15 cases moved
to `NS` with 2 added, and the 5 that went are in section 5.3.

"PARTIAL" means the cases named touch the row but none pins it whole, and section 6 names what is owed.
"Settled" says what the merge did to the row, in place of the merge marks the `b080c38` draft carried.

| # | Requirement, restated | Code at `9a08846` | Macha cases that gate it | Sessions |
|---|---|---|---|---|
| H1 | A commit is immutable and content-identified: SHA-256 of u64be generation, previous, u32be length, payload; streamed; recomputed on validation. | `md.hpp:244-248`; `md.cpp:1734-1741`, `:448-453`, `:1920-1922` | `test_metadata_hash_streaming_matches_canonical_encoding` (SM:571). Fixture: recomputed by script over its 12 full frames (section 9). | S3 |
| H2 | A body is a full record or a delta on its primary parent, a delta only when strictly smaller; a delta needs only parent generation below child's. Settled: only DLT5 to 11 and SM14, 16, 17, 19 read or written. | `md.hpp:288-291`; `mgr:1832-1836`, `:1241-1246`; `md.cpp:423-427`, `:976-999`, `:408-420` | `test_metadata_dlt7_presence_flags_round_trip` (SM:2789), `test_metadata_dlt8_append_extents_delta` (SM:2879), `test_merge_delta_primary_may_precede_merge_generation` (SM:933), `test_a_tree_backed_delta_reconstructs_its_record` (NS:670). PARTIAL: "smaller wins" has no case (C2); the fixture shows it (12 full, 1 DLT5). | S3, S4 |
| H3 | Multi-parent: at most 64 merge parents in every codec; lower hash primary; generation max(parents)+1. Settled: Macha's merge always names exactly one merge parent (`mgr:1216`), so 64 is a codec bound no live path reaches. | `md.hpp:188`; `md.cpp:764`, `:907`, `:1866`, `:1888`, `:3560-3561`; `mgr:1213-1216`, `:1229-1230` | `test_metadata_branch_merge_history_roundtrip` (SM:626), `test_metadata_commit_store_acceptance_heads_roundtrip` (SM:739). PARTIAL: the 64 bound (C3). | S3, S6 |
| H4 | Acceptance: this node stores the exact commit and persists a certificate; a mutation publishes to self alone; a merge, founding or policy commit seeks `metadata_write_copies`, local first then nearest; peers get parent history before accept; the local replica is mandatory. | `mgr:171-179`, `:825-914`; `cluster/placement.cpp:6-17`; `md.hpp:256-264`; `docs/metadata.md:18-26` | `test_commit_replicas_ordered_local_then_nearest` (RPC:4317), `test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim` (RPC:5660), `test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again` (RPC:5758) | S4, S5 |
| H5 | Certificate rules: replicas at least required; repeats take min(required) and the union; sealed `DHTMACC1` in `heads.meta`; no vote. Settled: `required == 0` only for genesis on a founder (`md.cpp:2841-2843`, `:2817-2824`); a certificate adding nothing costs a lookup. | `md.cpp:1751-1809`, `:2678-2752`, `:2836-2853`, `:3738-3747`, `:3751-3867` | `test_metadata_commit_store_acceptance_heads_roundtrip` (SM:739), `test_accepting_a_held_head_again_rebuilds_nothing` (SM:2492), `test_protocol20_checkpoint_without_acceptance_never_self_promotes` (SM:2189) | S3 |
| H6 | Heads are a set; an ancestor by history or clock is dropped; an empty clock is nobody's ancestor; preferred head is highest generation, then higher hash. Settled: the genesis clause stays (`md.cpp:2785-2789`); `migrate_legacy_head_locked` is gone. | `md.hpp:550`; `md.cpp:2754-2807`, `:2885-2887`, `:3801-3828` | `test_a_head_with_no_clock_is_nobodys_ancestor` (SM:1683), `test_a_head_from_before_truncation_is_recognised_by_its_clock` (SM:1702), `test_compacted_direct_predecessor_cannot_resurface_as_head` (SM:1355), `test_returning_replica_adopts_a_head_whose_ancestry_a_peer_truncated` (SM:1754). PARTIAL: preferred head lost its case with `test_metadata_seed_sibling_replays_after_restart` (C4). | S3 |
| H7 | Heads surveyed on every node, history imported from owners and witnesses, certificates accepted, 30 s cooldown for those that cannot be; rootless healing. Settled: `discover_or_form` runs only at generation at most 1 (`mgr:1477`, `:1976`). | `mgr:381-508`, `:916-937`, `:1056-1125` | `test_a_head_from_an_unknown_history_is_merged` (RPC:3737), `test_pristine_joiner_adopts_compacted_cluster_head` (SM:1544), `test_history_transfer_follows_only_materialization_dependencies` (SM:1524), `test_full_fallback_boundary_heals_when_parent_arrives` (SM:1273) | S5 |
| H8 | Divergent heads folded pairwise in hash order under one lock; owner's merge; commit with merge parents; no ancestor needed; every reconciler computes the same commit. Settled: the mixed branch is gone; a head with no tree root is `MergeUnavailable` (`mgr:1196-1197`). | `mgr:1127-1315`; `docs/metadata.md:60-71` | `test_concurrent_reads_during_divergence_produce_one_reconciliation` (RPC:3859), `test_disjoint_metadata_pairs_branch_and_reconcile` (RPC:4590), `test_three_heads_merge_to_the_same_namespace_in_any_order` (MRG:495, FS outcomes, stays Macha's; C8 is Dagda's) | S6 |
| H9 | Tree reconciliation merges only what differs and expresses the result as a delta on the primary parent, when smaller. | `tree.cpp:1934`, `:2000`; `mgr:1198-1201`, `:1220-1225`, `:1241-1246` | `test_a_tree_merge_is_the_merge_of_the_materialised_heads` (NT:1505), `test_reconciling_two_tree_backed_branches_keeps_the_namespace` (NS:565), `test_distant_changes_read_no_leaf_between_them` (NT:1477), `test_baseline_reconcile_materialised_against_tree` (NT:1628) | S2, S6 |
| H10 | A merge that cannot be made sets aside the other head (not this node's own); until membership changes; times persisted in `set-aside.meta`; dropped after `garbage_grace`. | `mgr:1180-1183`, `:1308-1313`, `:963-966`, `:991-1004`; `md.hpp:551-559`; `md.cpp:2908-2998` | `test_a_head_that_cannot_be_merged_is_set_aside` (RPC:3806), `test_a_head_set_aside_for_the_horizon_is_dropped` (SM:1928), `test_the_set_aside_time_of_a_head_no_longer_held_is_forgotten` (SM:1987) | S3, S6 |
| H11 | Conflicts are durable records, superseded by a later change, resolvable by a commit, GC roots while they stand. FS blob; generic `conflicts(space)` waits (L4). | `md.hpp:106-120`; `md.cpp:295-350`, `:691`, `:4151`; `mgr:1799-1811`, `:1905-1950` | `test_metadata_conflict_alternatives_are_gc_roots` (SM:2145), `test_metadata_superseded_conflicts_leave_the_snapshot` (SM:2977), `test_metadata_merge_delta_preserves_standing_conflicts` (SM:3079), `test_concurrent_edits_install_the_later_and_keep_both` (MRG:162), `test_a_decision_is_kept_against_a_head_that_has_not_seen_it` (MRG:421), `test_standing_conflicts_are_listed_and_resolved_over_http` (`test_service_root.cpp:547`) | S6 (seam) |
| H12 | Dots: a clock of author to sequence; author and sequences persisted in `author.meta`; a new author whenever the chain cannot be shown; a retry keeps its dot and returns at once if covered; an external identity adds a second clock entry. | `md.hpp:31-41`, `:163`, `:595-608`, `:692-698`; `md.cpp:2126-2246`; `mgr:1640-1642`, `:1678-1695`, `:1746-1749`, `:1853` | `test_an_author_sequence_never_regresses` (SM:2231), `test_a_broken_author_chain_starts_a_new_author` (SM:2257), `test_a_lost_author_record_starts_a_new_author` (SM:2291). PARTIAL: the retry and the identity have no named case (C5, C6). | S3, S4 |
| H13 | `legacy_clock` judges entries without provenance; only the FS merge reads it. Settled: it stays, a live feature; set once by the first mutation of a lineage (`mgr:1723-1727`). | `md.hpp:168`; `metadata_merge.cpp:38`, `:110-116` | `test_a_returning_node_from_before_provenance_loses_nothing` (MRG:369), `test_entries_from_before_provenance_merge_by_the_legacy_clock` (MRG:337), `test_the_legacy_clock_survives_snapshot_and_delta` (SM:3471) | Owner's; carried in the blob, Macha's cases stay |
| H14 | `mutate_delta`: pick the head (own head among several), reconfigure, decode, idempotency check, reserve a durable dot, refuse a policy change, stamp, bump clock, apply to tree, encode, claim, accept locally, owe the peers. Settled: the only write path; `mutate()` is gone. Reproduces R1. | `mgr:1636-1903`, `:1006-1036` | `test_a_tree_backed_delta_reconstructs_its_record` (NS:670), `test_a_delta_applied_to_the_tree_lands_where_the_map_lands` (NT:721), `test_every_write_records_where_it_came_from` (NS:113), `test_operations_waiting_on_a_commit_share_the_next_one` (NS:497), `test_local_metadata_store_falls_back_from_invalid_delta` (SM:4340) | S4 |
| H15 | The replicator offers the newest head to every active node, history first; claims owed in order, bounded at 32 MiB dropping the oldest; the caller does not wait; a stalled peer is not asked again for `dead_after`. | `mgr:93-169`, `:724-761`; `metadata_manager.hpp:212` | `test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim` (RPC:5660), `test_a_file_touch_commits_while_a_holder_is_away` (RPC:4214), `test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` (RPC:4037), `test_metadata_commits_with_whichever_nodes_remain` (RPC:5148), `test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again` (RPC:5758), `test_claims_owed_to_slow_peers_stay_within_their_bound` (`test_invariants.cpp:3030`) | S5 |
| H16 | Each node truncates alone: one accepted head and past 256 records or 64 MiB, that head becomes the root; never with two heads. Journal compaction at 128 records or 8 MiB. Settled: nothing writes the journal, so compaction fires only over frames older code left (finding D1); `migrate_legacy_head_locked` is gone. | `md.hpp:750-751`; `md.cpp:4083-4116`, `:4049-4081`; `mgr:957-971` | `test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges` (RPC:6293), `test_history_is_not_truncated_while_two_heads_are_held` (RPC:6378), `test_metadata_history_payloads_are_disk_backed_and_byte_bounded` (SM:2581), `test_a_journal_compaction_holds_no_reader_of_the_replica` (SM:1872, fills the journal by hand). The requirements' `test_metadata_history_checkpoint_concurrent_proposers_converge` still does not exist: finding F1. | S3; stages S5 |
| H17 | Bounded memory: shared payload bytes, streamed hash, deltas in place, a materialisation cache (128 MiB, 64 entries) pinning current and committed. | `md.hpp:198-242`, `:617-620`, `:677`; `md.cpp:1734-1741`, `:3080-3127`, `:3130-3226` | `test_metadata_record_payload_copy_is_shared` (SM:541), `test_metadata_delta_chain_reuses_bounded_materialized_head` (SM:2365), `test_metadata_materialization_cache_never_validates_corrupt_delta` (SM:2699), `test_cold_accepted_head_validation_caches_only_owned_results` (SM:2637); VALIDATION "Memory" | S3 |
| H18 | The Merkle tree over keys: boundaries by hash of domain, level and key, target 32, max 128; a branch holds (first key, id, count); node id is the object id; an empty map is one empty leaf. Generic branches, owner's leaves. Settled, final forms: `MNL1`, `MNB1`, `MNX1`, `MNY1`; four domain strings, `macha/namespace-tree/{entry,branch,extent,extent-branch}/v1`; provenance bit `0x80` in the leaf's type byte. A leaf record has no length prefix (`tree.cpp:244-272`), so Dagda cannot walk a leaf without the owner's codec. | `tree.hpp:38-51`; `tree.cpp:18-21`, `:27-43`, `:78-83`, `:188`, `:365`, `:409-429`; `metadata/namespace_control_store.cpp:15-36` | `test_the_root_is_a_function_of_the_entry_set_and_nothing_else` (NT:136), `test_changing_one_file_rewrites_a_path_to_the_root_and_not_the_library` (NT:159), `test_a_corrupt_node_is_refused_rather_than_trusted` (NT:261, asserts survival only; C7), `test_an_incremental_update_produces_the_tree_a_rebuild_would` (NT:629), `test_an_update_reaches_the_root_a_full_build_reaches` (NT:1038), `test_an_update_reads_the_path_it_changes_not_the_tree` (NT:1125). `test_a_leaf_stays_small_however_many_extents_a_file_has` (NT:236) stays Macha's. | S2 |
| H19 | Tree nodes are CONTROL objects; a commit names a root only once this node holds every node it wrote; replay writes every node locally and fetches what it reads. | `metadata/namespace_control_store.hpp:17-57`; `.cpp:15-44`; `cluster/local_state.cpp:165-174`; `docs/durability.md:69` | `test_history_replay_applies_a_delta_to_the_tree_not_the_map` (NT:780), `test_a_commit_claims_the_nodes_it_introduced_and_prunes_the_rest` (NT:1191), `test_the_control_collector_keeps_an_unclaimed_namespace` (NS:740) | S2, S3, S4 |
| H20 | Replica policy per space. For Macha: one every-node space, the policy type at `open()`. `replicas: N` waits (L2). | none: Macha has one every-node space | NO CASE. For Macha: C9, C10 and the policy half of `test_replication_policy_change_on_restart` (RPC:4756, C11). L2's proof is C12. | Interface S4; every-node S5; L2 |
| H21 | Reads. `current()` declares no wait and reads memory; `local()` may materialise, `converged()` may use the network. The requirements' "reads are memory reads" holds for `current()` only. | `contract/metadata_view.hpp:79-93`; `mgr:364-379`, `:1544-1593`, `:1595-1603`; `docs/metadata.md:7` | `test_the_metadata_view_pages_a_live_namespace` (NE:187), `test_a_checkpoint_write_holds_no_reader_of_the_replica` (SM:1812). PARTIAL: none says "no I/O" (C13). | S4 |
| H22 | A commit claims what it introduces: DATA here, or on a node present, or refused; new CONTROL here; a merge under a fresh dot not in its clock. | `service/node_services.cpp:349-537`, `:539-553`; `mgr:1841-1848`, `:1254-1271`; `docs/durability.md:49-51` | `test_a_file_touch_commits_while_a_holder_is_away` (RPC:4214), `test_a_merge_claims_what_it_introduces` (NS:852), `test_a_commit_is_published_only_once_its_objects_are_claimed` (`test_node_services.cpp:362`) | S4, S6 |
| H23 | Tombstone batches: one immutable CONTROL object per retiring mutation, named by the head, dropped whole after grace; a stale one drops its batch at once and the rest is written again. Settled: inline tombstones gone, the `garbage` slot stays in SM14 unfilled. | `md.hpp:87-104`; `mgr:1774-1795`; `service/maintenance.cpp:174-257`; `docs/metadata.md:119-129` | `test_tombstones_live_in_batches_in_a_tree_backed_head` (NS:336), `test_a_stale_tombstone_in_a_batch_is_erased_once` (NS:421), `test_tombstone_batches_ride_the_head_and_its_deltas` (SM:4524), `test_trace_tombstones_maturing` (`test_maintenance_trace.cpp:485`) | S4, S5 |
| H24 | Generation advertised on an interval and on acceptance; heads announced when the set changes; a generation not above what is set aside is not news. | `metadata_server.cpp:54-77`, `:84-102`; `cluster/cluster.cpp:43`, `:732-744`, `:864`, `:878` | `test_service_metadata_repair_coalesces_real_generation_burst` (RPC:3212), `test_service_same_generation_sibling_notice_triggers_reconciliation` (RPC:3556), `test_metadata_identity_rpc` (SM:4183) | S5 |
| H25 | The cache seed: the last committed record kept in the cache, used to recover a head set, never authority. Settled: `valid_seed` decodes only tree forms and genesis (`md.cpp:976-999`); envelope unchanged. | `storage/persistent_cache.cpp:325-390`; `cluster/local_state.cpp:169`, `:175`; `metadata_server.cpp:95-96`; `md.cpp:1964-1973`, `:2000-2005`, `:2032-2037`, `:2040-2074`; `docs/metadata.md:47` | `test_metadata_recovery_cache_seed_is_not_accepted_authority` (SM:2024), `test_storage_pool_and_persistent_cache` (SM:3616, the seed at `:3829-3844`), `test_metadata_codec_and_replica` (SM:3848, recovery at `:4098-4133`) | S3; S7 for the file on disk |
| H26 | Per-peer ordered execution of history import, commit store and accept, on a bounded executor with a quarter kept for control. | `cluster/net.cpp:3813-3815`, `:3854-3869`; `cluster/net.hpp:239-241`; `docs/metadata.md:100-104` | Retired against Dannan (section 5): `test_rpc_metadata_mutations_use_bounded_isolated_executor` (RPC:2282), `test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers` (RPC:2428), `test_rpc_metadata_control_mutations_go_first` (RPC:2607), `test_rpc_metadata_executor_cancellation_disconnect_and_shutdown` (RPC:2681). Dagda's half: C14. | S5 |
| H27 | Genesis. Settled: a fixed constant record, generation 1, payload `DHTMGEN1`, hash `87ad4c58cd1ccdd1326698d186eeb0ce54e5bf0cb8b1829b688a54584b6d7f06` (computed by script, matching the fixture's first frame); new clusters only; holds no tree; `decode_snapshot` returns a `/` entry for it because a node at genesis serves that root locally. A founder holds it under `required == 0`; a joiner never. A namespace is founded as a tree, generation 2, SM14, floor 1, root from `filesystem.root_*`, times 1. One deterministic founder, lowest active id. | `md.cpp:33`, `:988-997`, `:1910-1919`, `:2785-2789`, `:2812-2825`, `:2841-2843`; `mgr:1403-1469`, `:1477`; `service/maintenance.cpp:461-480`; `docs/metadata.md:89`, `:164-166` | `test_a_new_namespace_is_a_tree_from_its_first_commit` (SM:2562), `test_genesis_root_configuration` (SM:4290), `test_joiner_cannot_form_genesis` (RPC:2978), `test_bootstrap_joiner_requires_complete_checkpoint_survey` (RPC:2999), `test_a_durable_trio_founds_commits_and_reads_through_every_node` (RPC:5118). PARTIAL: no known answer for the constant, none for the founder rule (C15, C16). | S3 (constant), S5 |
| H28 | Nodes advertise `metadata_write_copies`; a mismatch marks membership changed and does not fail closed. Settled: `maybe_reconfigure` changes `data_replication` alone, the floor stays 1 (`mgr:1322`, `:1448`), a merge takes the max (`metadata_merge.cpp:95-96`). Reproduces R2. | `cluster/cluster.cpp:44-45`, `:871`; `types.hpp:61-64`; `mgr:1317-1338`; `service/service_config.cpp:23-25` | `test_nodes_seeking_different_copy_counts_share_one_namespace` (RPC:3060). PARTIAL: the policy commit (C11, C17). | S5 |
| H29 | The scope check at the write floor: every replica that counts checks the writer's scope before storing. No source installed: node principals allowed. | none in Macha | NO CASE (C18, C19; Dagda's `store_scope_test.cpp` is the pattern). Macha gains nothing and loses nothing: C18 is what proves that. | S5 |
| H30 | No rollback: an ancestor of the held set changes nothing; an unrelated commit is a divergence, never a replacement. | `md.cpp:2777-2807`, `:3801-3828` | NO CASE as such (C20). Touching it: SM:1355, RPC:3737, SM:1754. | S3 |
| H31 | New (the Macha session's answer: "an H row for the decoded view's TTL"). A decoded view is reused until `metadata_cache` passes, a remote epoch moves or a higher generation is known; then a real read finds what no notice announced. | `mgr:297-298`, `:337-346`, `:364-379` | `test_metadata_decoded_cache_ttl_recovers_missed_notice` (`test_hydration_catalogue.cpp:4177`) | S5 |

**Rows with NO CASE, whole or in part:** H20, H29 and H30 whole; parts of H2, H3, H6, H12, H21, H27, H28; the
metadata plane; lease renew and release. Section 6 names each.

### What the merge settled, against the questions the draft asked

1. **Genesis.** As H27. Founders with different `filesystem.root_*` build different generation 2 commits and
   merge them like any two heads; the case is `test_two_node_mutual_bootstrap_metadata_write_floor` (RPC:3114)
   in the same-config form only, so C16 adds the other.
2. **Snapshot forms.** Written by content (`md.cpp:773`): batches, SM19; else legacy clock, SM17; else torrent
   requests, SM16; else SM14. Fields `md.cpp:774-836`. Every SM14 slot stays hash-covered: `metadata_voters`,
   `garbage`, `metadata_participants`, `metadata_branch_floor`, `retention_baseline_complete`. No live path
   fills voters, garbage, participants or the floor (`mgr:1707-1717` refuses a change; `metadata_merge.cpp:97-103`
   joins participants).
3. **Delta forms.** Selection `md.cpp:1013-1032`. `encode_snapshot_for_delta` is `encode_snapshot_v14` for any
   version at least 5 (`:423-427`).
4. **Tree forms.** As H18. `NamespaceTreeLimits`: 32, 128, 32, 128, 256, 1024, inline 8 (`tree.hpp:41-50`). Any
   change moves every root.
5. **Legacy authority.** `cas`, `cas_delta`, `seed`, `install_committed_delta`, `remember_committed` and
   `remember_current_committed` gone (`18f0c4b`). `required == 0` as H5.
6. **The manager.** `maybe_reconfigure` as H28. `read_group`'s mixed branch gone. `publish_commit` as H4.
   `mutate_delta` throws `logic_error` on a head with no root (`mgr:1699-1700`).
7. **`<state>/metadata/`.** `checkpoint.meta`, `journal.log`, `history.log`, `heads.meta`, `author.meta`,
   `author-chain-broken`, `set-aside.meta`, `recovery.required` (`md.cpp:1953-1958`), and quarantines by
   `.corrupt.<stamp>`. Not read: `current.meta`, `committed.meta`, `.v10`, `mutation-sequence.meta` (`ecce886`; a
   script finds none in `src/metadata`). All are `magic, nonce 12, tag 16, u32 length, ciphertext`, AAD the
   magic, under `identity.keys.storage` (`cluster/local_state.cpp:169`), except `set-aside.meta`, which is
   **plain and has no magic** (`md.cpp:2933-2941`, finding D2), and the two text markers. A journal or history
   frame is `u32 length, nonce, tag, u32 length, ciphertext`, AAD `DHTMJNL1` or `DHTMHST1`.
8. **The seed.** As H25.
9. **Messages.** `cas_metadata` 7, `seed_metadata` 8, `checkpoint_metadata` 11, `commit_metadata` 17 and
   `cas_metadata_delta` 18 keep their numbers (`cluster/net.hpp:35-46`) with no route (`metadata_server.cpp:109-197`).
   `get_metadata` 6 and `get_metadata_identity` 21 are bound and have no production caller, only tests (`test_rpc_cluster.cpp:334`,
   `test_storage_metadata.cpp:4204`). `put_metadata_commit` and `put_metadata_history_entry` run the same handler, `import_history`
   (`:167-188`).
10. **Maintenance.** Section 2.7.
11. **Tests.** Section 5.
12. **Fixtures.** Section 9.
13. **For the cutovers.** Sections 2.2, 2.3, 3.

## 2. Plans

### 2.0 The seams every plan below assumes

`dagda::heads`, in `dagda/heads/`, with a `Space` per opened head space. The owner supplies, per
`open(space, domain, codec, merge, policy)`:

- **`Codec`**: `genesis()`, `decode_full`, `encode_full`, `encode_delta`, `apply_delta(parent state, delta bytes,
  tree)`, and `head_fields(state)`: the clock, merge parents and floor Dagda reads. For Macha these are a
  function of both halves, because head and FS fields interleave in SM14 and up. State is an opaque shared
  handle, so a delta can share what it did not touch.
- **`Merge`**: `merge(base?, a, b) -> state | conflict`. Macha ignores `base`.
- **`TreeValues`**: the leaf value codec, what a value references, the optional child tree (the extent spine).
  The domain strings and limits are parameters of the space; Macha's are section 1, item 4 and H18.
- **`ReachabilityProvider`**: exists (`dagda/contract/reachability.h`).
- **A mutation** carries an optional external identity, as `MetadataMutationIdentity`
  (`contract/metadata_view.hpp:48`, `filesystem/filesystem.cpp:1825`, `fuse/fuse_frontend.cpp:3150`). It is how the
  FUSE journal recognises a mutation accepted before a crash, and it is what writes a second clock entry
  (`mgr:1746-1749`).
- **A policy commit** is the owner's call, carrying no dot (R2). Macha's `read_record_uncached`, `mutate_delta` and
  `repair_once` all run it, so a read can commit.

**The test owner is a neutral record** in `dagda/test/fixtures/`, as the lease template did (Q1).

### 2.1 The `metadata` readiness plane

**Macha.** Stage "metadata" comes after control, cache and retention (`cluster/local_state.cpp:164-179`). The plane
is marked once `MetadataReplica` has opened and replayed locally with no peer asked, and a failure fails startup
with its reason (`:182-186`). Status reports it as `startup.metadata` (`api/status_api.cpp:829`). NO CASE names the
plane: a search of `tests/` at `9a08846` for `metadata_ready` and `"metadata"]` finds nothing.

**Dannan 0.4.1** has what is needed: `Readiness::declare` until the node starts, idempotent, and `ready` throws for
an undeclared plane (`dannan/status/readiness.h:35-42`).

**Plan, session S4.**
1. `plane_metadata` joins `readiness_planes()` (`dagda/node.cpp:56`), so it is declared at `:97` with the rest.
2. Stage "open heads" goes after `open distributed-store` (`node.cpp:213`) and before `open storage-server`
   (`:227`). Macha's replay needs the control store and a `ControlObjectFetch` (`local_state.cpp:146`, `:172`); in
   Dagda that fetch is the distributed store's `ensure_control_local`.
3. Replay runs the owner's codec, so the plane cannot be marked honestly unless the space is known then.
   `NodeOptions::spaces` names the spaces whose replay readiness waits on; `Node::open` opens them in that stage and
   marks the plane after them. An empty list marks it at once. `Heads::open` opens further spaces at any time and they
   do not gate readiness.
4. A space that cannot open (primary state bad and no valid seed, `md.cpp:2000-2004`, `:2033-2035`) is `NodeRefused`
   with the reason, through `Node::Node`'s catch, which calls `readiness().fail`.
5. **Through the first stage Macha's own `MetadataReplica` runs**, so `NodeOptions::heads` is an off switch: off, the
   node declares no `metadata` plane, registers no head type and runs no head stage, and Macha's facade declares and
   marks the plane itself, which `declare` allows for any hosted system. That keeps `startup.metadata` honest. Macha
   sets it off at the first stage and on at the later one. On is the library's default.

**Gate.** C21 to C24.

### 2.2 The cache seed

**Macha, exactly.** `<cache>/metadata/current.meta`: magic `DHTCMET1`, nonce 12, tag 16, then the ciphertext as a Writer
blob (`storage/persistent_cache.cpp:325-350`). Sealed under the domain's storage key (`local_state.cpp:151`), AAD the
magic. The plaintext is `encode_metadata_record` (`md.cpp:1742-1749`). Written atomically by `remember_metadata`,
deduplicated by hash, off when `prefer_metadata` is false (`:340`). Written at open from the committed record
(`local_state.cpp:175`) and on each acceptance that moves the committed head (`metadata_server.cpp:95-96`). Read once
at open and used **only** when the primary state fails or a recovery marker stands (`md.cpp:1977-2007`, `:2032-2037`);
then it is never authority: no certificate, the recovery marker set, the author chain broken (`:2040-2074`). A write
failure is a debug line; a read failure is an absent seed.

**Dagda has** the attachment point: `with_seed`, `SeedPlace{directory, opening}` and `CacheOptions::seed`
(`dagda/store/persistent_cache.h:34`, `:61`, `:99`). `dagda-migrate` walks only `objects/` and `packs/` under a cache
root, so through the first stage `current.meta` is left as it was.

**The maintainer's ruling moves this.** `<state>/metadata/` and the cache seed migrate on disk at step 8c, by `dagda-migrate`
(`ACTIVE.md` Decisions, 2026-11-05). So the draft's S1 is gone: **there is no seed-only flag day**, and the draft's
replacement of the second-cutover plan's B11 is withdrawn.
- **First stage:** B11 stands: Macha's facade keeps `current.meta` itself under `with_seed`, in Macha's bytes with
  Macha's AAD, because Macha's replica reads it.
- **`heads::CacheSeed`, session S3:** `remember(space, record bytes)`, `load(space)`, over `with_seed`, under the seed
  lock, by `durable_replace_file`, rewritten when `opening` moves (Macha resets its dedupe on reconfigure the same way,
  `persistent_cache.cpp:87`). v19's file and AAD as section 4.
- **The migration pass, session S7:** section 4.

**Why migrate rather than discard**, which would be simpler since the next open rewrites it: the first open after a
migration is the likeliest moment for primary state to fail, and the seed is the only local recovery there is.

**Gate.** SM:2024, SM:3616's seed lines, SM:3848's recovery lines, converted, in S3. C25 and C26. Section 4's in S7.

### 2.3 `ordered` registration

**Dannan has it, at the tag Dagda pins.** `Registration.ordered` is in 0.4.1's `net/routes.h`. The scheduler keeps a
per-peer FIFO, one job per peer, and the same quarter for control, matching Macha (`cluster/net.cpp:3854-3869`). The
default limits are Macha's: 1 worker, 64 jobs, 256 MiB (`cluster/net.hpp:239-241`). Dannan's cases, names checked in
`../dannan/dannan/test/net_scheduler_test.cpp`: `OrderedWorkKeepsEachPeersOrderAndRunsPeersSideBySide`,
`ControlClassOrderedWorkGoesFirstAndKeepsAQuarterOfTheQueue`, `AQueueOverItsByteBoundRefusesAndCounts`,
`ACancelledQueuedRequestNeverRuns`, `StoppingFinishesRunningWorkAndDropsTheQueue`,
`AControlMutationOvertakesItsOwnPeersBackgroundWork`, `HeldOrderedWorkDelaysNoOtherClass`. So no request to Dannan.

**Plan, session S5.** `dagda/heads/messages.{h,cpp}` with its own `register_types(routes)`, called by `Node::open`
beside the store's (`node.cpp:110`) under the same catch, unless `NodeOptions::heads` is off. Numbers are 512 plus
Macha's (`cluster/net.hpp:34-123`, all re-read). Everything is on the CONTROL lane, as `lane_for` puts it
(`cluster/net.cpp:2027-2037`). `Frames::any`, because Macha calls these at control, read-ahead and speculative.

| Type | Macha | Dagda | Ordered | Note |
|---|---|---|---|---|
| `put_metadata_history_entry` | 28 | 540 | **yes** | H26 |
| `put_metadata_commit` | 30 | 542 | **yes** | H26, same handler |
| `accept_metadata_commit` | 31 | 543 | **yes** | H26 |
| `get_metadata_history_entry` | 26 | 538 | no | |
| `has_metadata_history_entry` | 27 | 539 | no | |
| `get_metadata_heads` | 29 | 541 | no | |
| `get_metadata_history_record` | 41 | 553 | no | head repair |
| `get_committed_metadata` | 10 | 522 | no | bootstrap survey, `mgr:1361` |
| `metadata_notice` | 9 | 521 | no | `notification = true`, broadcast (`cluster/cluster.cpp:744`) |
| `metadata_reply`, `metadata_identity_reply`, `metadata_history_entry_reply`, `metadata_heads_reply` | 105, 108, 111, 112 | 617, 620, 623, 624 | no | `bool_reply` 615 is the store's, shared |
| `get_metadata`, `get_metadata_identity` | 6, 21 | 518, 533 | no | Served, no production caller. Registered, because SM:4183 is a mapped case and a Macha-visible wire |

The five retired numbers (7, 8, 11, 17, 18) are not registered, as Macha binds none; their slots stay free.

**Three things this has to get right.**
1. **One registration, and the first cutover must set `ordered` too.** The first cutover re-registers Macha's own
   metadata types from 512, and two registrations of a number are refused (`node.cpp:111-113`). So with `heads` off
   Macha's registration stands through the first stage, and at the later stage it goes and Dagda's takes the same
   numbers. **Macha's registration must set `ordered` on these three**, or H26 is lost a stage early; it is one field.
   Say so to the Macha and Dannan sessions.
2. **Payloads byte for byte at those numbers**, so a mixed roll at the later stage works. A space other than Macha's
   adds an optional trailing space id; Macha's decoders call `reader.finish()`, so no trailing bytes ever go to Macha's
   space.
3. **An ordered handler must not wait on an ordered call.** Replay fetches tree nodes through `get_control_object`,
   which is not ordered, so the fetch is safe; C27 pins it.

**Gate.** C14, C27. RPC:2428 and RPC:2607's behaviour observed through Dagda's types in C14.

### 2.4 Head-backed lease records, with renew and release: waits, L1

**Macha does not need this.** No Macha path reads Dagda's lease records through a head: its torrent requests stay inside
its blob, as a library the FS codec calls, and the coordinator's takeover stays Macha's in the first port (requirements
5.4). `LocalLeaseRecords` serves the lease template at 8b. By the maintainer's ruling it waits, and its design stays here so it is
not lost. It stops waiting only if torrent's move out of the record gives the torrent project its own space and that
project's coordinator takes a head-backed lease (section 8).

**Plan: `heads::HeadLeaseRecords : lease::LeaseRecords`.**
- `scope` is the space (decided at step 5). A lease space is a Dagda-owned head space whose state is a `LeaseTable`
  (`dagda/lease/lease_table.h`) with its snapshot and delta sections and `LeaseTable::merge(base?, a, b)` as the merge.
- **In the blob, not the tree.** `LeaseRecords::read` is declared to wait on nothing (`keeper.h:100-102`), and the table
  is bounded at 65,536 records; a tree lookup can fetch a node.
- `join` is the space's mutation, completing on the caller's executor with the record re-read. `Node::_lease_records`
  (`node.h:392`) becomes this; `LocalLeaseRecords` stays for its tests. `Context::Leases::acquire_waits`
  (`context.h:201`) widens.
- **Release** needs no new bytes: a claim by nobody at epoch + 1. The lease order gives the higher epoch the win, so it
  survives a merge; an absent record could not, being the bottom, and a tombstone would keep a later acquire out for the
  window. The next acquire is epoch + 2.
- **Renew** is Q4. Recommended: it writes nothing, re-reads, and returns held or stands down.

**Gate.** C28 to C33, all new: there is NO CASE in Macha for renew or release.
`test_torrent_requests_belong_to_the_cluster_and_are_driven_by_their_owner` (`test_hydration_catalogue.cpp:2382`) remains
Macha's own.

### 2.5 The reference-count diff seam

**Dagda keeps the counters**: `ReferenceCounts::{reset, change, views}` (`dagda/ledger/reference_counts.h:27-37`,
`:56-61`). **The diff has no home.** At Macha it is the FS census (`filesystem/filesystem.cpp:2498-2576`):
- `census_count` (`:2555`): same root, nothing to do; else follow; else walk.
- `census_follow` (`:2523`): `diff_namespace_tree_nodes` for branches and leaves, then `diff_namespace_trees` for the
  entries that differ, each entry's extents (holes aside) and spine nodes in and out, totals adjusted, `counts.change`.
- `census_walk` (`:2498`): a whole walk, `reset`, when there is no root, the counts refuse, or a tree it stands at cannot
  be read.

**Plan: `heads::ReferenceCensus`**, session S2, generic:
- `count(space, root)` is `census_count` moved. The tree diff is Dagda's already (H18, H9).
- What a value references is the owner's: `TreeValues::references(value) -> {data, children}`. Macha's returns non-hole
  extents and `namespace_entry_spine_nodes` (`tree.cpp:1853`), and `Totals` keep their meaning.
- Single owner, called under the provider's `inventory` and `release`, as Macha's `maintenance_build_mutex_` serialises it
  (`filesystem.cpp:2629`, `:2650`).
- Macha's stays at `<state>/ledger/referenced`, which is where the node opens it and where `dagda-migrate` knows it.
- Macha's FS keeps its census through the first stage: `Node::references()` already gives it the counters.

**Gate.** `test_the_inventory_and_the_media_index_follow_the_tree` (NS:198); `test_inventory_control_set_holds_the_namespace_tree`,
`test_release_over_a_tree_matches_the_pass_build` and `test_an_unreadable_tree_node_leaves_the_release_incomplete`
(`test_horizon_builder.cpp:189`, `:246`, `:306`); NT:1436, NT:1170. C34.

### 2.6 Cost baselines for commit and verify

**The sites** (`md.cpp:3680`, `:3590`, `:3175` deep copy of a parent; `:3212`, `:3592`, `:3687` re-encode and rehash per delta;
`:1930`, `:3652`, `:4099`, `mgr:713`, `mgr:1013` a full decode for parents or a clock).

**In tree form the namespace is not in the payload**: `encode_snapshot_v14` writes the root and no entries, and
`test_the_record_stops_growing_with_the_library` (NT:432) pins it. So re-encoding per delta costs O(record), not O(keys), and
byte-exact replay and "no growth with keys" do not conflict. What grows the record is the clock, `node_status`, conflicts,
batches (44 bytes each, retained for the grace) and torrent requests. The fixture's record is 178 bytes; a live one was not
seen.

**The baselines**, written with S3 and S4, in `heads_cost_test.cpp`, **asserting counts, never time**. A
`heads::CostCounters` is passed in: tree nodes read and written, record bytes encoded and hashed, full decodes, parent states
copied, history frames written, syncs.

| Baseline | Fixed | Varied | Assert |
|---|---|---|---|
| commit keys | one-leaf change, one author | 1 k, 100 k keys | Nodes written and read at most depth-proportional; record bytes equal at both sizes; no full decode for parents; no parent copy. Fails if any grows with keys. |
| verify keys | the same delta, parent materialised | 1 k, 100 k keys | As above on the receiver: `store_commit`'s path. |
| replay keys | a chain of L deltas, L at most 256 | 1 k, 100 k keys | Bytes encoded proportional to L and independent of keys; one materialisation shared, as SM:2365. |
| merge keys | k differing keys | 1 k, 100 k keys | Reads follow k, as NT:1477; the owner's merge called on differing keys only. |
| accept repeat | a held head | | No materialisation, as SM:2492. |
| record axis | one-leaf change | batches, conflicts, authors | **Reports only**: `BENCH` lines in Macha's form (`test_baselines.cpp`). |

"No change in performance" needs numbers from the same operations on Macha at `9a08846`: the Pi baselines are Macha's
(its `TODO/ACTIVE.md:37`, `:71`), and they are the comparison at 8c. A million-key run stays with P1's scale pass.

**Gate.** S3 closes verify, replay and accept; S4 closes commit; S6 closes merge.

### 2.7 The maintenance stages, and where they join

**Macha's order at `9a08846`** (`enter_stage`, `service/maintenance.cpp`): `metadata-repair` `:489`, `catalogue-repair` `:549`,
`availability` `:596`, `inventory` `:618`, `retention-repair` `:725`, `network-repair` `:786`, `control-gc` `:954`,
`garbage-collect` `:1020`, `local-rebalance` `:1100`, `retention-compact` `:1127`, **`head-repair` `:1135`**,
**`history-truncate` `:1144`**, `compact-packs` `:1156`, `scrub` `:1161`, `ledger-verify` `:1212`. `ACTIVE.md`'s `:1137` and
`:1146` are stale.

- **head-repair** (`:1134-1140`): `repair_unreconstructable_heads(speculative)`. **Always**, `busy` or not; a no-op unless a
  head is flagged; every peer tried for a full body, `reanchor_history` (`mgr:510-563`); failure is a debug line.
- **history-truncate** (`:1142-1150`): only when `!busy`. Takes the mutation lock, so a commit waits for it; expires set-aside
  heads, then `compact_history_if_safe(256, 64 MiB)` (`mgr:957-971`).
- **metadata-repair** (`:459-542`), what actually converges replicas: `repair_step()` when the metadata is dirty, on events
  and a backoff, with the virgin founder rule (`:461-480`), `note_replica_validation`, and non-destructive maintenance
  continuing when it fails unless recovery is required or the generation is at most 1. It runs **first** in the pass. Without
  it a node converges only by the replicator's offers. Its demand is `ConvergenceDemand`
  (`service/convergence_demand.hpp:29`), which moves with it.

**They join Dagda's `Node::local_stages_locked` between retention and packs.** That means splitting `node.cpp:538-545`, one
`if (!busy)` over both, in two:

```
538  if (!busy) { retention compaction }
     head repair            // any activity; its own try
     if (!busy) { history truncation }   // its own try
542  if (!busy) { DATA packs }
```

- Each in **its own try**, as Macha's are; today one try covers all the stages (`node.cpp:523`, `:588`).
- `forced` makes `busy` false as now. `stop` goes in: head repair checks it between heads and peers. Each peer has a deadline,
  so a pass costs at most flagged heads times peers times that; the flagged list is bounded per pass.
- `LocalStages` gains `heads_repair_ran`, `heads_repaired`, `history_truncate_ran`, `history_truncated`, and
  `dagda.maintenance.*` the figures.
- `Node::maintenance_loop` calls metadata-repair before `gc_pass_locked` (`node.cpp:628-651`), and parks on events as Macha's
  loop does. **Dagda's loop polls today** (`ACTIVE.md`, known gaps); the later stage moves Macha onto it, so that gap closes
  before the later stage, not after.

**When.** Session S5, with `heads` on. Through the first stage Macha's pass keeps these stages. At the later stage they move
with heads and the loop, together (the maintainer, 2026-11-05). Waits as Macha declares them: `state_device | network | locks`, and
`state_device | locks` (`contract/metadata_view.hpp:179-186`).

**Gate.** C35. From Macha: RPC:6448, RPC:6378, RPC:6293, `test_failed_convergence_retries_once_per_burst_of_events` (RPC:3328),
`test_convergence_demand_coalesces_burst_and_keeps_same_generation_event` (`test_invariants.cpp:113`, moved to S5 by the loop
ruling). `test_maintenance_trace.cpp` stays Macha's and is the later stage's gate on Dagda's loop.

## 3. The order, for Macha alone

**The maintainer's rulings, which this follows.**
- Heads is first on Dagda's plan and **waits on nothing**. Macha is frozen, its code is the specification, Dannan 0.4.1 has
  `ordered`. S2 can start today. A fixture still to be cut holds a session's close, not its start (section 9).
- Dagda's goal is Macha. L1 to L9 wait unless Macha needs them; section 3.3 marks them, and none is needed.
- The cutover is in stages. **First stage:** store, reachability provider, lease template, with Macha's loop running. **Later
  stage:** heads, the metadata migration and Dagda's loop, together. A stage moves on when its tests pass.
- `<state>/metadata/` and the seed migrate on disk at 8c, by `dagda-migrate`. S1 as drafted is gone.

**Building and deploying are two orders.** Heads is built first; it deploys at the later stage. The first stage (`ACTIVE.md`'s
P7) needs no heads session: Macha's replica, manager and loop keep running over their v18-sealed `metadata/`, with objects,
including tree nodes, already v19 beneath them. What the first stage needs of this plan is three lines: the off switch once S4
exists, B11 standing (2.2), and `ordered` set on Macha's own registration (2.3).

**Nothing in S2 to S7 touches the live path until the later stage.** So heads being first does not hold the first stage, and the
first stage does not hold heads. `ACTIVE.md`'s table puts heads at P8 behind P7; that row order has to change to match, which
this file cannot do (finding F10).

### 3.1 Sessions, in order

Each writes its tests first, has its own `feature/heads-*` branch and leaves a handoff in `COMPLETED.md` with gate counts.

| Session | What | Rows | Gate | Stage |
|---|---|---|---|---|
| **S2. Tree over keys** | `heads/tree`: build, update, diff, node diff, lookup, scan, paged walk, value codec, child-tree hook, domain strings and limits as parameters; the CONTROL node store's read, commit and replay modes; `ReferenceCensus` (2.5) | H18, H19, part of H9 | Section 5's 21 S2 cases over the neutral values; C7, C34; Macha's branch nodes and boundaries as vectors | Later |
| **S3. Replica on disk** | Genesis as the owner's constant; record and hash; history log, journal reader, checkpoint; certificates and `heads.meta`; head set, ancestor rules, no rollback; authors and dots; set-aside times; truncation and compaction; materialisation cache; seed recovery and `CacheSeed`; lazy frame reading; owner's codec interface; v19's file set from the start (section 4) | H1, H2, H3 (format), H5, H6, H10 (store), H12 (authors), H16, H17, H25, H27 (constant), H30 | Section 5's 32 S3 cases; C3, C4, C15, C20, C25, C26; the neutral `test_metadata_delta_state_model` and `test_metadata_codec_and_replica`'s replica half; verify, replay and accept baselines | Later |
| **S4. One node commits** | `Space`, the mutation path with identity and retries by dot, policy refused in a mutation, claims barrier, retirement through the provider, `Context::Heads` (`open`, `read`, mutate, `heads`), the policy type, refused with a reason for `replicas: N`; the plane from real replay and the off switch (2.1) | H4 (local), H12 (retry), H14, H19, H20 (interface), H21, H22, H23 | Section 5's 4 S4 cases; C2, C5, C6, C9, C13, C21 to C24, C36 (R1); commit baseline | Later |
| **S5. Exchange** | Message types with `ordered` (2.3); server handlers; history push and pull with its window of 8; head survey and import; merge's sought copies; replicator and owed claims; stalled peers; notice and generation advert; the three maintenance stages and `ConvergenceDemand` (2.7); founding; policy commit; the scope check; head status and `dagda.heads.*`; options for second-cutover 8c.2, 8c.3; M6's re-ask on the write-copies advert | H4, H7, H15, H16, H20 (every node), H24, H26, H27, H28, H29, H31 | Section 5's 17 S5 cases and 4 retirements; C10, C11, C14, C16, C17, C18, C19, C27, C35, C37 (R2), C38 | Later |
| **S6. Reconciler** | Fold under one lock; `merge(base?, a, b)`; merge by what differs; set-aside on `MergeUnavailable`; claims under a fresh dot; own head; conflicts as owner's blob, with GC roots through the provider | H3, H8, H9, H10, H11 (seam), H22 | Section 5's 11 S6 cases; C2's merge path, C8, C16's merge, C39, C40; merge baseline | Later |
| **S7. Conformance and the disk** | Vectors from Macha's code; the migration's metadata and seed pass (section 4), forward, resume and reverse; a space opening the migrated scratch fixture; a commit extends it; docs/migration.md | H2, H18 as bytes, H25 (file) | Section 4's cases, C41 to C46; every vector in section 9 | Later |

**Then the later stage at the port**, which is Macha's work over these: 8c, 8d of the second-cutover plan, against `9a08846`. Macha's
owner codec moves to `src/filesystem/`, Macha's `MetadataManager` becomes a facade, its dump tool `tools/metadata_history_dump.cpp`
and `tools/metadata_inspect.cpp` read v19. **The replay of real history and identical dump output are proved here, in Macha's port
branch, because only Macha's codec can apply Macha's deltas and Dagda holds none of it** (section 4.3). The cases Macha keeps (section
5, and every KEEP and REWRITE in the mapping) run on that branch; that run, plus the Pi baselines, is the Yes.

### 3.2 What changed from the draft's order

- S1 is gone: `CacheSeed` to S3, the migration pass to S7, the off switch to S4.
- Genesis moves from S4 to S3, because a replica opens at genesis (`md.cpp:2024`).
- Nothing changes in S2 to S6's dependency order: each needs the last.

### 3.3 L1 to L9: what Macha needs

| Item | What | Macha needs it? |
|---|---|---|
| L1 | Head-backed leases, renew, release (2.4) | **No.** Only if torrent gets its own space (section 8) |
| L2 | `replicas: N`, placement salt, replica set as evidence, forwarding reads; C12 | **No.** Every-node space; C9, C10 stand for Macha. Recommended: gates the tag, not 8c |
| L3 | The Replicated level's per-space k | **No.** Macha's merge uses `metadata_write_copies` |
| L4 | Generic `conflicts(space)`, `resolve` | **No.** Macha's stay in its blob (H11) |
| L5 | Node-local spaces | **No** |
| L6 | Scale to Lir's load | **No.** Macha's one space fits the advert cap (F5) |
| L7 | Retained values | **No** |
| L8 | Domain catalogue as a head space; blind replicas; several providers | **No.** Macha is one domain, and the provider union is step 6's |
| L9 | Lugh's no-rollback on node-local spaces | **No.** H30's case is Macha's (C20) |

The S4 policy type stops them from breaking the contract later. `Context` v1 freezes after them, tag `0.1.0`, so Macha's pin is a
commit or a pre-1 tag that does not claim v1 is frozen.

## 4. The metadata migration

**Where it stands.** At stage one `dagda-migrate` moves objects and ledgers and writes the v19 layout marker, leaving `metadata/`
byte for byte (`docs/migration.md:57-58`). Run again it says "state is layout v19 already: nothing to do" (`dagda/migrate/migrate.cpp:580-581`,
`:549-550`; finding D11), and `--reverse` names Macha 0.92.x. So the later stage needs a second pass that the v19 marker does not stop.

### 4.1 What it must do, given v19

v19 is the domain in every AAD and the node's own ledgers under the node-local key. `<state>/metadata/` is two kinds of thing, and the
split is my reading of the maintainer's rule, so it is Q2:

| File | What it holds | Key under v19 | How it moves | Crash |
|---|---|---|---|---|
| `history.log` | Domain content: records and deltas | Domain's | Each frame opened once under v18's AAD, tag derived by `TagDeriver` (`dagda/migrate/retag.h:40`), written to a sibling, renamed. Nonce and ciphertext unchanged | Old file or new, by rename |
| `checkpoint.meta` | Domain content: the committed record | Domain's | Same, one record, durable replace | Told by which AAD opens |
| `journal.log` | Domain content; empty unless written before `ecce886` | Domain's | Same, frame by frame. Empty stays empty | As history |
| `<cache>/metadata/current.meta` | Domain content: the seed | Domain's | Same; new file `<cache>/metadata/<space id hex>.meta`, old removed; magic `DHTCMET1` stays | Both present: redo and remove |
| `heads.meta` | This node's standing: certificates | Node-local | Opened under the domain key, re-sealed whole | Told by which key opens |
| `author.meta` | This node's author id, reserved, accepted, past ids | Node-local | Same. **Plaintext byte for byte**, or the chain breaks (`md.cpp:2218-2219`) | Same |
| `set-aside.meta` | Hashes and times, **plain today** (D2) | Node-local | Sealed for the first time, magic `DHTMSAS1` (new). Unreadable: dropped, "lost times only restart the wait" (`md.cpp:2926-2929`) | Same |
| `recovery.required`, `author-chain-broken` | Reason text; authority is by existence | None | Left byte for byte | Nothing to do |
| `*.corrupt.*` and journal or history tails | Quarantine | None | Left, listed | Nothing to do |

**AAD, one rule:** `"dagda/heads/19" || domain id || space id || magic`, the same for every sealed file, and `"dagda/seed/19" || domain id ||
space id || magic` for the seed. Space id: `SHA-256("dagda/space/1" || domain id || name)`, Macha's name `macha/namespace` (proposed).
The magic stays in each file.

**Path.** The maintainer's "everything meets the one layout" means one path for every space: `<state>/heads/<space id hex>/`. The directory renames
last, which is the atomic mark of completion, so no new marker is needed: `heads/<space>/` present is done; `metadata/` present is not, and
each file inside says how far it got. Q2 asks.

**Rules, as the rest of the tool.** Node stopped, the store's lock held. Every refusal before the first write: state marker not v19
(objects and tree nodes come first, so stage one must be done); no custody; the node id; the key proved on the first sealed file; a
`heads.meta`, `author.meta` or `checkpoint.meta` opening under neither key: refused by name, with "open it once under 0.93.4, which recovers
from the seed or from peers, and run again". One history frame or the tail opening under neither is not a refusal: it is listed and
copied as bytes, and the store's reader then does what Macha's does, skipping it and quarantining a tail (`md.cpp:2627-2639`, `:2651-2670`).
A frame is never decoded by the tool: it opens the seal and moves the tag, so a frame in a form 0.93.4 cannot read goes through unharmed.

**Space.** Peak extra: one `history.log`. The soft bound is 64 MiB plus a frame, **but truncation never runs with two heads held** (`md.cpp:4086-4089`),
so it can be larger; the dry run reports the size and refuses if the filesystem lacks it. "MUST NOT need twice the storage" holds: this is one
small file, not the data.

**Evidence and flags.** `--dry-run` writes nothing and reports frames, bytes, each file's state. The report gains history frames migrated, already,
unreadable; files re-sealed; seed `migrated | already | unreadable | none`. `--reverse` mirrors it, gating nothing (the maintainer): frames and sealed
records back to v18's AAD, ledgers back under the domain key, `set-aside.meta` plain, directory renamed, seed renamed. Not byte for byte for a
re-sealed file, a fresh nonce, as ledgers are today.

**On Macha's dump tool.** `tools/metadata_history_dump.cpp` opens `history.log` under the magic as AAD, so it cannot read a migrated file. It moves
with 8c.1; a run of the old tool against a migrated state failing is expected, not a defect.

### 4.2 What must be true for replay of real history

Every item is a "must" on Macha's codec, on Dagda's replica or on the tool; the case that proves each is in 4.3.

1. **Every frame's seal opens** under v19's AAD, with the nonce and ciphertext Macha wrote.
2. **Reading stays lazy and forgiving.** `load_history` validates frames and does not decode a full body (`md.cpp:2588-2598`); a delta that fails to
   decode, or has no parent, is skipped with a warning (`:2599-2639`), and a head needing it is flagged and repaired from a peer. Dagda must do the same,
   since real history may hold frames written before 0.93.
3. **Every head materialises by Macha's rule** (`md.cpp:3130-3226`): the nearest cached or full anchor, deltas applied in order, each re-encoded, each
   checked against its hash and merge parents. A failure leaves the head flagged, not fatal (`:2699-2713`).
4. **Re-encoding is byte exact.** The hash covers the payload, so the owner's encoder must reproduce every slot and form: the unfilled `voters`, `garbage`,
   `participants`, `branch_floor`, `baseline`, SM selection by content, the clock and legacy clock, batches, the torrent section as Macha writes it.
5. **The tree is byte exact.** The record names a root, so build and update must reach Macha's ids: the four domain strings, the limits, `is_boundary`, the
   packed fallback (`tree.cpp:118-124`), leaf and spine forms, inline at 8 extents.
6. **Replay writes nodes locally and fetches what it reads** (`namespace_control_store.cpp:19-25`, `:38-44`; `local_state.cpp:165-174`), asking no peer to
   take anything. Stage one's CONTROL store and fetch must serve it under v19, unchanged.
7. **An anchor's tree may have been collected.** A chain replays from its anchor's root, and CONTROL GC follows the current head. A node it cannot fetch makes
   the head unreconstructable, and repair takes a full body. That is Macha's behaviour and Dagda keeps it. **I could not verify how often it happens on a real
   state**; S7's run should count it.
8. **Nothing in replay reads the clock, the config or a dot.** `apply_metadata_delta_in_place` is pure over its inputs (I read its call sites, not its whole
   body: S3 confirms). Config enters only at the extent size check and policy commit, after replay.
9. **Real history does not begin at the new genesis.** `766617e` changed genesis's hash, new clusters only, so a live chain's first records name the old one or
   have been truncated to a root with `previous_known = 0`. Nothing may assume generation 1's hash on an existing state. A joiner still holds the new constant
   and adopts the cluster's head, as SM:1544.
10. **Certificates and authors come through intact**: the same `required` and replicas, so a head stays accepted; the same author id, `reserved` and `accepted`, so the
    next mutation continues (`md.cpp:2221-2224`) and no author rotates. `note_author_accepted` is memory only until the next reserve (`:2233-2237`), so a crash may
    have left `accepted` behind; that stays as it is.
11. **A non-empty `journal.log` replays**, if a state has one. Macha's `load_journal` does (`md.cpp:2248-2429`). On a state that has run since `ecce886` a head change has
    emptied it, but I could not check the live nodes. Dagda keeps the reader (Q3).
12. **The result is the same state.** Same head hash, same root, same `macha-metadata-dump --all --stats --tree` output apart from what the tool cannot help.

### 4.3 Who proves it

- **In `dagda_tests`, S7:** the tool, over synthetic states and the scratch fixture (C41 to C44). The neutral owner's chain replays through the migrated files
  (C45). Macha's records as opaque vectors: seal opens, hash recomputes, frame layout, certificates, head set, genesis, branch nodes (C46). Dagda cannot apply Macha's
  deltas or re-encode Macha's record: it holds no FS or torrent codec.
- **At the port, on Macha's branch, the owner's codec linked:** items 3, 4, 5, 12 on the scratch fixture and on a copy of real state. Macha's whole metadata suite.
- **Whether a real copy is run is the maintainer's:** Macha's `TODO/ACTIVE.md:96-97` records "No drop that, I'll decide later" on the real migration on a copy and the live-b rehearsal,
  and says this overrides what Dagda reported; Dagda's `ACTIVE.md` Decisions says the real run on a second copy is to go ahead (finding F11). The plan's gate says a real copy
  and waits on his word. Without it the Yes rests on 0.93.4's scratch state alone, and I would say so rather than call it unconditional.

## 5. The 95 GAP-HEADS cases

From `2026-11-05-test-mapping.md`, its later sections applied. Names and lines at `9a08846`. "Keeps" means a rewrite stays Macha's beside Dagda's case. **Count: 95 in, 95 accounted.**

| Fate | Cases |
|---|---|
| S2 | 21 |
| S3 | 32 |
| S4 | 4 |
| S5 | 17 |
| S6 | 11 |
| S7 | 0 (its proofs are new: C41 to C46) |
| Retired against Dannan | 4 |
| Retired by Macha at `18f0c4b` | 3 |
| Stays Macha's | 3 |
| Total | 95 |

### 5.1 One line a case

| Case | At `9a08846` | Fate | Note |
|---|---|---|---|
| `test_metadata_decoded_cache_ttl_recovers_missed_notice` | `test_hydration_catalogue.cpp:4177` | S5 | H31 |
| `test_metadata_availability_logs_only_transitions` | `test_invariants.cpp:1874` | Stays Macha's | The Macha session's answer: log text. Dagda's evidence half is C38 |
| `test_claims_owed_to_slow_peers_stay_within_their_bound` | `test_invariants.cpp:3030` | S5 | H15, 32 MiB |
| `test_pages_visit_what_the_callback_walk_visits_on_every_shape` | NE:88 | S2 | | `HeadsTree.PagesVisitWhatTheCallbackWalkVisitsOnEveryShape` |
| `test_a_resumed_page_reads_only_its_own_path` | NE:108 | S2 | | `HeadsTree.AResumedPageReadsOnlyItsOwnPath` |
| `test_a_cursor_between_paths_resumes_after_it` | NE:129 | S2 | `HeadsTree.ACursorBetweenKeysResumesAfterIt`. Tree form; map form stays with Macha's working form |
| `test_cancellation_stops_before_an_entry` | NE:148 | S2 | | `HeadsTree.CancellationStopsBeforeAnEntry` |
| `test_a_missing_node_or_store_throws` | NE:159 | S2 | | `HeadsTree.AMissingNodeOrStoreThrows` |
| `test_the_metadata_view_pages_a_live_namespace` | NE:187 | Stays Macha's | Rewrites over the Heads noun; S4 supplies paging |
| `test_a_node_that_is_not_a_tree_node_throws` | NE:225 | S2 | | `HeadsTree.ANodeThatIsNotATreeNodeThrows` |
| `test_a_manager_without_a_store_pages_a_map_backed_view` | NE:242 | Stays Macha's | The map is Macha's working form, Dagda has none. Name is stale (F8) |
| `test_tombstones_live_in_batches_in_a_tree_backed_head` | NS:336 | S4 | Keeps the FS-delete half |
| `test_reconciling_two_tree_backed_branches_keeps_the_namespace` | NS:565 | S6 | |
| `test_a_merge_claims_what_it_introduces` | NS:852 | S6 | |
| `test_the_tree_round_trips_every_namespace_it_is_given` | NT:78 | S2 | | `HeadsTree.TheTreeRoundTripsEveryKeySetItIsGiven` |
| `test_the_root_is_a_function_of_the_entry_set_and_nothing_else` | NT:136 | S2 | | `HeadsTree.TheRootIsAFunctionOfTheEntrySetAndNothingElse` |
| `test_changing_one_file_rewrites_a_path_to_the_root_and_not_the_library` | NT:159 | S2 | | `HeadsTree.ChangingOneValueRewritesAPathToTheRootAndNotTheTree` |
| `test_a_lookup_reads_a_path_to_the_root_rather_than_the_namespace` | NT:218 | S2 | | `HeadsTree.ALookupReadsAPathToTheRootRatherThanTheTree` |
| `test_a_corrupt_node_is_refused_rather_than_trusted` | NT:261 | S2 | `HeadsTree.ACorruptNodeIsRefused`. Dagda asserts the refusal, C7 |
| `test_an_incremental_update_produces_the_tree_a_rebuild_would` | NT:629 | S2 | | `HeadsTree.AnIncrementalUpdateProducesTheTreeARebuildWould` |
| `test_a_write_rewrites_a_path_rather_than_the_library` | NT:692 | S2 | | `HeadsTree.AWriteRewritesAPathRatherThanTheTree` |
| `test_a_delta_applied_to_the_tree_lands_where_the_map_lands` | NT:721 | S2 | `HeadsTree.ADeltaAppliedToTheTreeLandsWhereAMapLands`. Keeps the spine-append half |
| `test_history_replay_applies_a_delta_to_the_tree_not_the_map` | NT:780 | S3 | Needs replay's applier |
| `test_a_prefix_scan_descends_rather_than_walking_the_library` | NT:862 | S2 | | `HeadsTree.APrefixScanDescendsRatherThanWalkingTheTree` |
| `test_a_scan_seeks_and_stops_where_it_is_told` | NT:991 | S2 | | `HeadsTree.AScanSeeksAndStopsWhereItIsTold` |
| `test_an_update_reaches_the_root_a_full_build_reaches` | NT:1038 | S2 | | `HeadsTree.AnUpdateReachesTheRootAFullBuildReaches` |
| `test_an_update_reads_the_path_it_changes_not_the_tree` | NT:1125 | S2 | | `HeadsTree.AnUpdateReadsThePathItChangesNotTheTree` |
| `test_every_tree_node_is_reachable_for_the_collector` | NT:1170 | S2 | | `HeadsTree.EveryTreeNodeIsReachableForTheCollector` |
| `test_a_commit_claims_the_nodes_it_introduced_and_prunes_the_rest` | NT:1191 | S4 | |
| `test_a_diff_is_the_paths_that_differ_and_reads_only_their_leaves` | NT:1436 | S2 | | `HeadsTree.ADiffIsTheKeysThatDifferAndReadsOnlyTheirLeaves` |
| `test_distant_changes_read_no_leaf_between_them` | NT:1477 | S2 | | `HeadsTree.DistantChangesReadNoLeafBetweenThem` |
| `test_a_tree_merge_is_the_merge_of_the_materialised_heads` | NT:1505 | S6 | Keeps the FS outcomes |
| `test_baseline_reconcile_materialised_against_tree` | NT:1628 | S6 | Merge baseline |
| `test_a_commit_is_published_only_once_its_objects_are_claimed` | `test_node_services.cpp:362` | S4 | Keeps "a hole is not an object" |
| `test_rpc_metadata_mutations_use_bounded_isolated_executor` | RPC:2282 | Retired against Dannan | `Scheduler.AQueueOverItsByteBoundRefusesAndCounts`, `Scheduler.HeldWorkersOfOneClassNeverDelayAnother`; S5's C14. 38 assertions: compare them at S5 before the retirement is recorded |
| `test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers` | RPC:2428 | Retired against Dannan | `Scheduler.OrderedWorkKeepsEachPeersOrderAndRunsPeersSideBySide`; C14 |
| `test_rpc_metadata_control_mutations_go_first` | RPC:2607 | Retired against Dannan | `Scheduler.ControlClassOrderedWorkGoesFirstAndKeepsAQuarterOfTheQueue`, `Scheduler.AControlMutationOvertakesItsOwnPeersBackgroundWork`; C14 |
| `test_rpc_metadata_executor_cancellation_disconnect_and_shutdown` | RPC:2681 | Retired against Dannan | `Scheduler.ACancelledQueuedRequestNeverRuns`, `Scheduler.StoppingFinishesRunningWorkAndDropsTheQueue`. Its disconnect half: compare at S5 |
| `test_joiner_cannot_form_genesis` | RPC:2978 | S5 | |
| `test_bootstrap_joiner_requires_complete_checkpoint_survey` | RPC:2999 | S5 | |
| `test_nodes_seeking_different_copy_counts_share_one_namespace` | RPC:3060 | S5 | |
| `test_two_node_mutual_bootstrap_metadata_write_floor` | RPC:3114 | S6 | Needs merge. Carried as a known failure in Macha's `TODO/ACTIVE.md:146` |
| `test_service_metadata_repair_coalesces_real_generation_burst` | RPC:3212 | S5 | |
| `test_failed_convergence_retries_once_per_burst_of_events` | RPC:3328 | S5 | |
| `test_service_same_generation_sibling_notice_triggers_reconciliation` | RPC:3556 | S6 | |
| `test_a_head_from_an_unknown_history_is_merged` | RPC:3737 | S6 | |
| `test_a_head_that_cannot_be_merged_is_set_aside` | RPC:3806 | S6 | |
| `test_concurrent_reads_during_divergence_produce_one_reconciliation` | RPC:3859 | S6 | |
| `test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` | RPC:4037 | S5 | |
| `test_a_file_touch_commits_while_a_holder_is_away` | RPC:4214 | S5 | |
| `test_commit_replicas_ordered_local_then_nearest` | RPC:4317 | S5 | |
| `test_disjoint_metadata_pairs_branch_and_reconcile` | RPC:4590 | S6 | |
| `test_metadata_commits_with_whichever_nodes_remain` | RPC:5148 | S5 | |
| `test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes` | RPC:5588 | S5 | |
| `test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim` | RPC:5660 | S5 | |
| `test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again` | RPC:5758 | S5 | |
| `test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges` | RPC:6293 | S5 | |
| `test_history_is_not_truncated_while_two_heads_are_held` | RPC:6378 | S6 | A read merges the two |
| `test_unreconstructable_accepted_head_is_repaired_live_from_a_peer` | RPC:6448 | S5 | |
| `test_metadata_record_payload_copy_is_shared` | SM:541 | S3 | |
| `test_metadata_seed_sibling_replays_after_restart` | gone | Retired by Macha | See 5.2 |
| `test_metadata_hash_streaming_matches_canonical_encoding` | SM:571 | S3 | |
| `test_metadata_branch_merge_history_roundtrip` | SM:626 | S3 | Keeps the FS assertions |
| `test_metadata_commit_store_acceptance_heads_roundtrip` | SM:739 | S3 | |
| `test_accept_commit_refreshes_checkpoint_on_both_repair_and_ordinary_paths` | SM:877 | S3 | |
| `test_merge_delta_primary_may_precede_merge_generation` | SM:933 | S3 | |
| `test_merge_delta_with_generation_gap_reconstructs_after_cache_eviction_and_reopen` | SM:1007 | S3 | |
| `test_unreconstructable_accepted_head_is_kept_at_startup_and_reanchored_live` | SM:1112 | S3 | |
| `test_full_fallback_boundary_heals_when_parent_arrives` | SM:1273 | S3 | |
| `test_compacted_direct_predecessor_cannot_resurface_as_head` | SM:1355 | S3 | |
| `test_unreconstructable_accepted_head_is_rate_limited_not_hammered` | SM:1428 | S3 | |
| `test_history_transfer_follows_only_materialization_dependencies` | SM:1524 | S3 | |
| `test_pristine_joiner_adopts_compacted_cluster_head` | SM:1544 | S3 | |
| `test_a_head_with_no_clock_is_nobodys_ancestor` | SM:1683 | S3 | |
| `test_a_head_from_before_truncation_is_recognised_by_its_clock` | SM:1702 | S3 | |
| `test_returning_replica_adopts_a_head_whose_ancestry_a_peer_truncated` | SM:1754 | S3 | |
| `test_a_checkpoint_write_holds_no_reader_of_the_replica` | SM:1812 | S3 | |
| `test_a_journal_compaction_holds_no_reader_of_the_replica` | SM:1872 | S3 | Fills the journal by hand; follows Q3 |
| `test_a_head_set_aside_for_the_horizon_is_dropped` | SM:1928 | S3 | |
| `test_the_set_aside_time_of_a_head_no_longer_held_is_forgotten` | SM:1987 | S3 | |
| `test_metadata_recovery_cache_seed_is_not_accepted_authority` | SM:2024 | S3 | |
| `test_metadata_journal_mid_frame_corruption_truncates_not_reseeds` | SM:2069 | S3 | Follows Q3 |
| `test_metadata_legacy_prepare_journal_replays_deterministically` | gone | Retired by Macha | See 5.2 |
| `test_protocol20_checkpoint_without_acceptance_never_self_promotes` | SM:2189 | S3 | |
| `test_an_author_sequence_never_regresses` | SM:2231 | S3 | |
| `test_a_broken_author_chain_starts_a_new_author` | SM:2257 | S3 | |
| `test_a_lost_author_record_starts_a_new_author` | SM:2291 | S3 | |
| `test_metadata_delta_chain_reuses_bounded_materialized_head` | SM:2365 | S3 | |
| `test_metadata_history_payloads_are_disk_backed_and_byte_bounded` | SM:2581 | S3 | |
| `test_cold_accepted_head_validation_caches_only_owned_results` | SM:2637 | S3 | |
| `test_metadata_materialization_cache_never_validates_corrupt_delta` | SM:2699 | S3 | |
| `test_protocol20_state_rejects_legacy_authority_mutators` | gone | Retired by Macha | See 5.2 |
| `test_storage_pool_and_persistent_cache` | SM:3616 | S3 | Seed lines `:3829-3844`. Rest retires against `StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance`, `StorePersistentCache.TheCacheEvictsTheLeastRecentlyUsedAndReopens` |
| `test_metadata_identity_rpc` | SM:4183 | S5 | |
| `test_local_metadata_store_falls_back_from_invalid_delta` | SM:4340 | S4 | |

### 5.2 The three removed: a count that drops is a finding

All three went at `18f0c4b` ("Retired, their subject being the removed API"). Each proved something besides the API, so:

- **`test_metadata_seed_sibling_replays_after_restart`.** Became nothing in Macha. Its API, `seed()`, has no production caller left. What it also pinned, that the higher hash wins between same-generation siblings, **now has no named case at `9a08846`**; the rule stands at `md.cpp:2885-2887`. Dagda's C4 takes it.
- **`test_metadata_legacy_prepare_journal_replays_deterministically`.** Became the journal reader's own tests, which write frames by hand (`append_journal_frame`, used at `test_storage_metadata.cpp:4041`, and SM:2069). What it pinned, PREPARE and CAS replay with the lower hash winning, is gone with `cas`. `load_journal`'s PREPARE and COMMIT branches remain, covered only as those tests reach them. Q3.
- **`test_protocol20_state_rejects_legacy_authority_mutators`.** Became nothing: the mutators do not exist, so there is nothing to reject. The property that nothing but a certificate gives authority is held by SM:2024 and SM:2189. Dagda never had the mutators and has no case to write.

Also retired by Macha, blocks inside surviving cases, not counted above: in `test_metadata_codec_and_replica` (SM:3848) the DLT1 replay, a proposal pre-empting a prepared one, and the SM7 and SM5 reader checks; in SM:4524 the inline head refusing batches; in `test_a_snapshot_never_carries_its_namespace_in_two_places` the SM13 encoder refusal; in `test_an_sm14_record_points_at_the_namespace_instead_of_carrying_it` the SM13 size comparison (`18f0c4b`, `766617e`).

### 5.3 Beyond the 95

Cases the mapping fates differently, that heads owes or that moved:

| Case | At `9a08846` | What |
|---|---|---|
| `test_metadata_codec_and_replica` | SM:3848 | Split, the Macha session's answer: replica half (quarantine, seed, compaction) S3; codec half stays Macha's |
| `test_metadata_delta_state_model` | `test_models.cpp:43` | S3 owes it on the neutral owner; Macha's stays |
| `test_replication_policy_change_on_restart` | RPC:4756 | Policy half is C11 |
| `test_convergence_demand_coalesces_burst_and_keeps_same_generation_event` | `test_invariants.cpp:113` | KEEP in the mapping; the loop ruling moves the primitive to S5, and the case with it |
| `test_a_new_namespace_is_a_tree_from_its_first_commit` | SM:2562 | New since `00861dd`. Neutral form S5; stays Macha's too |
| `test_accepting_a_held_head_again_rebuilds_nothing` | SM:2492 | New. S3 |
| `test_a_namespace_tree_is_a_function_of_its_entries` | NS:609 | New. S2 |
| `test_a_stale_tombstone_in_a_batch_is_erased_once` | NS:421 | New. S4 and S5, through the provider; the FS half stays Macha's |

**Counts at `9a08846`.** The mapping's 786 at `00861dd` is now 792, by the script: 19 cases added, 13 gone. Gone: the 3 above; the 5 of `test_predicates.cpp` (the maintainer: retired; the mapping's GAP rows close); 5 of `test_namespace_migration.cpp`, `test_a_migrated_node_serves_and_writes_its_library`, `test_every_node_computes_the_same_record_from_the_same_head`, `test_a_migration_refuses_what_it_cannot_re_root_safely`, `test_the_pre_migration_state_is_kept_not_deleted`, `test_a_lagging_node_adopts_the_leaders_record_only_if_it_agrees`, which went with `macha-namespace-migrate` at `076115f` and need no Dagda case. Added and not heads', for the mapping's next pass: `test_a_delete_mark_lasts_until_the_last_delete_of_the_id_ends`, `test_a_delete_marks_its_object_and_holds_no_stripe` (already mapped), `test_a_failed_file_refuses_pause_and_resume_and_can_be_cancelled`, `test_a_failed_read_keeps_the_object_and_a_bad_record_is_removed`, `test_a_files_actions_work_on_a_failed_job`, `test_a_node_whose_only_disk_is_offline_starts_and_says_so`, `test_a_node_with_its_disk_online_is_not_degraded`, `test_a_pull_list_new_while_the_push_runs_is_not_passed_over`, `test_a_stop_during_the_diff_is_not_a_failed_peer`, `test_a_stop_ends_the_inventory_walk`, `test_a_store_tells_a_bad_record_from_a_failed_read`, `test_an_error_of_one_file_fails_that_file_and_the_job_carries_on`, `test_ingest_files_a_series_extras_under_the_series`, `test_the_repair_step_that_first_sees_a_new_pull_list_walks_it`, `test_what_is_not_held_pauses_over_the_ids_beside_the_view`.

## 6. The cases Dagda must write

Written first, in `dagda_tests`, over the neutral owner unless it says otherwise. Each exists because Macha has no case for that clause, or Macha's stays Macha's.

| # | Row | Case | What it asserts | Session |
|---|---|---|---|---|
| C2 | H2 | `HeadsCommit.ABodyIsADeltaOnlyWhenStrictlySmallerThanTheRecord`, `HeadsReconcile.AMergeBodyIsADeltaOnlyWhenStrictlySmaller` | Delta smaller: delta frame. Equal or larger: full frame. Both paths (`mgr:1834`, `:1244`), for a commit and a merge | S4, S6 |
| C3 | H3 | `HeadsRecord.SixtyFourMergeParentsPassAndSixtyFiveAreRefusedEverywhere` | Encode, decode, frame and import each accept 64 and refuse 65 | S3 |
| C4 | H6 | `HeadsReplica.ThePreferredHeadIsTheHighestGenerationThenTheHigherHash` | Across generations and between siblings, live and after reopen; a set-aside head is never preferred | S3 |
| C5 | H12 | `HeadsCommit.ARetryKeepsItsDotAndAnAcceptedAttemptIsNotMadeTwice` | First publish throws after acceptance; second attempt uses the same dot, finds it covered, returns the head, generation unchanged | S4 |
| C6 | H12 | `HeadsCommit.AnExternalIdentityIsCarriedAndACoveredOneReturnsAtOnce` | Identity writes its own clock entry beside the dot; a repeat returns with no commit; zero sequence refused (`mgr:1643-1644`) | S4 |
| C7 | H18 | `HeadsTree.ACorruptNodeIsRefused` | Every bit flip: a named refusal or a different id, never a wrong answer. Macha's asserts only survival | S2 |
| C8 | H8 | `HeadsReconcile.TheFoldIsTheSameInAnyArrivalOrder` | Three heads, every order: the same final hash on the neutral owner, the same on two nodes | S6 |
| C9 | H20 | `HeadsSpace.AReplicasPolicyIsRefusedWithAReasonUntilHoldersExist` | `replicas: N` at `open()` refused, naming L2. Every-node opens | S4 |
| C10 | H20 | `HeadsSpace.AnEveryNodeSpaceIsOfferedToEveryActiveNode` | Commit on one of three reaches all; a node away gets it on return; the one who was asked first is not preferred | S5 |
| C11 | H20, H28 | `HeadsPolicy.AChangedPolicyOnRestartIsCommittedAndWritesFollowIt` | Neutral form of RPC:4756's policy half: changed at restart, one commit, carried by later commits | S5 |
| C12 | H20 | `HeadsPlacement.ThreeReplicasOnFiveNodes` | L2. Waits, not Macha's | L2 |
| C13 | H21 | `HeadsSpace.TheCurrentViewReadsNothing` | With counters on node store, files and network: `current()` zero, under `WaitGuard` with `Waits::none`; `local()` at an unchanged head revision zero; a head change is the first read to do any | S4 |
| C14 | H26 | `HeadsMessages.TheThreeMutationsAreOrderedAndNoOther`, `HeadsMessages.LanesFramesAndADuplicateRegistration`, `HeadsExchange.ACommitOvertakesImportsAndOnePeersOrderHolds` | Beside `store_messages_test.cpp`'s recording route table; two nodes for the last | S5 |
| C15 | H27 | `HeadsGenesis.TheConstantIsMachasOnMachasSpace` | Macha's payload through the owner's `genesis()` hashes to `87ad4c58...`, generation 1, previous zero. On the neutral owner, a constant per space | S3 |
| C16 | H27 | `HeadsFounding.TheLowestActiveNodeFoundsAndFollowersWait`, `HeadsFounding.FoundersConfiguredDifferentlyMerge` | `maintenance.cpp:467-472`; different founding state on two founders gives two generation 2 heads and one merge | S5, S6 |
| C17 | H28 | `HeadsPolicy.DisagreeingNodesKeepCommitting` | Macha's loop, kept: each commit on seeing the other's value. Names the behaviour; asserts it, with the maintainer's "left" (Macha `TODO/ACTIVE.md:79-82`) | S5 |
| C18 | H29 | `HeadsMessages.WithNoScopeSourceNodePrincipalsCommitAndAccept` | The one that matters to Macha: no source, a commit and accept from a node principal pass and nothing is refused | S5 |
| C19 | H29 | `HeadsMessages.ARefusedScopeStoresNothingAndSaysWhy` | With a source: on put and accept, nothing durable, evidence names why. Pattern: `store_scope_test.cpp` | S5 |
| C20 | H30 | `HeadsReplica.AnAncestorChangesNothing`, `HeadsReplica.AnUnrelatedCommitIsADivergenceNeverAReplacement` | Set unchanged, no checkpoint write for the first; both held, preferred by C4's rule, for the second, whichever generation is higher; both after reopen | S3 |
| C21 | Plane | `NodeHeads.TheMetadataPlaneIsNotReadyBeforeReplayReturns` | Seam held; plane unready; then ready. `node_test.cpp:240` and `:274` gain `metadata` (changed, not removed) | S4 |
| C22 | Plane | `NodeHeads.WithNoSpacesThePlaneIsReadyAtOnce` | | S4 |
| C23 | Plane | `NodeHeads.ASpaceThatCannotOpenFailsReadinessAndNamesIt` | | S4 |
| C24 | Plane | `NodeHeads.OffTheNodeDeclaresNothingAndAnOwnersPlaneWorks` | No plane, no types, no stage; the owner declares and marks its own; `startup.metadata` proved by Macha's facade at the port | S4 |
| C25 | H25 | `HeadsSeed.AnotherDomainsOrSpacesSeedDoesNotOpen`, `HeadsSeed.OffMeansNeitherReadNorWritten`, `HeadsSeed.ACorruptSeedIsAbsent` | | S3 |
| C26 | H25 | `HeadsSeed.AMovedPlaceIsWrittenAgain` | `opening` changes, the record is rewritten | S3 |
| C27 | H26 | `HeadsExchange.ACommitWhoseReplayNeedsANodeFromTheSamePeerCompletes` | An ordered handler's fetch does not wait on ordered work | S5 |
| C28 | Lease | `HeadLeases.AcquiredOnOneIsReadOnAnotherAfterReplication` | L1 | L1 |
| C29 | Lease | `HeadLeases.AGoneHolderIsTakenAtTheNextEpochAfterTheWaits` | Absence and rank; the old holder back stands down; a restart forgets nothing | L1 |
| C30 | Lease | `HeadLeases.BothSidesOfAPartitionAcquireAndTheMergeKeepsTheLeaseOrdersWinner` | The loser reports `lost`; `EpochFence::observe` from merged records | L1 |
| C31 | Lease | `HeadLeases.ReleaseThenAcquireFromEitherSide` | Zero holder at epoch + 1, next acquire epoch + 2, survives a merge | L1 |
| C32 | Lease | `HeadLeases.RenewWritesNothingAndReportsStanding` | Per Q4. Held, or stands down, on `standing`'s evidence; no commit, generation unchanged | L1 |
| C33 | Lease | `HeadLeases.ReleaseByANonHolderChangesNothing` | | L1 |
| C34 | Census | `HeadsCensus.FollowEqualsWalkAfterAnySequence`, `.ARefusedChangeWalks`, `.AnUnreadableTreeWalks`, `.ACrashMidChangeWalks`, `.AFollowReadsOnlyWhatDiffers`, `.ASharedChildGoesWithItsLastReference` | 2.5, counted by the node store | S2 |
| C35 | Stages | `NodeHeads.StagesRunInMachasOrder`, `.RepairRunsWhileBusyAndTruncationDoesNot`, `.AThrowInEitherLeavesTheRestRunning`, `.StopIsRespected`, `.ForcedRunsThem`, `.MetadataRepairRunsFirstAndAVirginFollowerWaits` | 2.7 | S5 |
| C36 | R1 | `HeadsCommit.AnUnchangedMutationCommitsItsClock` | Section 7, R1 | S4 |
| C37 | R2 | `HeadsPolicy.APolicyCommitCarriesNoDotAndOnlyAMergeCollapsesIt` | Section 7, R2 | S5 |
| C38 | Status | `HeadsStatus.StaysWritableThroughPendingReconciliationAndReportsStable` | Dagda's half of `test_metadata_availability_logs_only_transitions` | S5 |
| C39 | R3 | `HeadsReconcile.AMergeReservesADotItsClockDoesNotCarry` | Section 7, R3; two reconcilers still mint one hash | S6 |
| C40 | Base | `HeadsReconcile.SiblingMergesConvergeRatherThanLoop` | Q5 | S6 |
| C41 | Migration | `MigrateHeads.ForwardKeepsNonceAndCiphertextAndEveryFrameOpens` | Over Macha's scratch fixture and a synthetic state with every file kind in 4.1 | S7 |
| C42 | Migration | `MigrateHeads.ACrashAtEachStepIsFinishedByASecondRun` | Each file boundary and before the rename | S7 |
| C43 | Migration | `MigrateHeads.ReverseRestoresWhatTheStoreReads` | Sealed records under v18 AAD and domain key; Macha's bytes for frames; not a gate | S7 |
| C44 | Migration | `MigrateHeads.RefusesBeforeTheFirstWrite`, `.AnUnreadableFrameIsListedAndCarried`, `.ARunUnderV19StillDoesThePass`, `.TheDryRunWritesNothing`, `.ASeedThatOpensUnderNeitherIsListedAndLeft` | 4.1 | S7 |
| C45 | Replay | `HeadsReplica.AChainThroughAMigratedStateIsTheSameHead` | Neutral owner: write under v18's form with a test sealer, migrate, reopen, same head, a commit extends it, the author continues | S7 |
| C46 | Vectors | `HeadsConformance.MachasFramesCertificatesAndBranchesAsOpaqueBytes` | Fixture bytes: seal, frame layout, recomputed hash, `heads.meta`, genesis, boundaries for the four strings, `MNB1` and `MNY1` re-encoded equal. Torrent sections are bytes, never parsed | S7 |

## 7. Macha's behaviour that looks odd: requirements, kept

**The maintainer: written down, to change later in Macha first** (`ACTIVE.md`, Waiting on the maintainer; Macha `TODO/ACTIVE.md:87-88`). Heads reproduces these exactly. None is a defect to fix here; a change starts in Macha.

- **R1. An unchanged mutation commits its clock.** The dot is written into the clock (`mgr:1744-1745`) before the comparison (`:1814-1817`), and the clock is in the payload, so `payload == current.payload` cannot hold and the branch is unreachable. A mutation that changes nothing is a new generation. `resolve_conflict` on an unknown id (`:1910-1913`) returns false having made a commit. `ACTIVE.md` cites `:1919`, now stale. Case C36. A case on Macha's own side does not exist at `9a08846`; Macha's rewrites keep whatever they imply.
- **R2. A policy commit carries no dot.** `maybe_reconfigure` (`mgr:1317-1338`) writes no clock entry, so its clock equals its parent's, and after truncation only a merge can collapse it (`md.cpp:2802` needs `behind != ahead`). It also clears `merge_parents`, publishes to the active set at `FrameType::control`, and runs on reads. Disagreeing nodes loop (C17). `ACTIVE.md` cites `:1376`, now stale. Case C37.

Others of the same kind, found reading, with the same standing: reproduce, do not fix, and report to the Macha session.

- **R3.** A merge reserves a dot its clock does not carry (`mgr:1255-1256`), to claim under; the author's sequence therefore has gaps. `docs/metadata.md:71` says "a merge mints no dots", true of the record alone.
- **R4.** Two opposite tie-breaks: the preferred head takes the higher hash (`md.cpp:2886`), the own head the lower (`mgr:1029-1034`, a `max_element` whose order is reversed on hash).
- **R5.** `required` in a certificate is how many held it (`mgr:866`), and repeats take the min (`md.cpp:3839`), so a repeat can lower it. Never a condition.
- **R6.** A read path can write: `usable_heads` clears set-aside on a membership change (`mgr:992-994`); `read_record_uncached` may commit a policy (`:1483`) and clear recovery (`:1484-1485`).
- **R7.** `metadata_write_copies` governs a merge, a founding and a policy commit, never a mutation (`mgr:171-179`).
- **R8.** A set-aside is forgotten at restart and retried; only its time persists (`md.hpp:551-559`).
- **R9.** `note_author_accepted` is memory until the next reserve (`md.cpp:2233-2237`).
- **R10.** A corrupt `author.meta` throws from `reserve_mutation_dot` (`md.cpp:2160-2162`); no recovery path, so writes stop. D3.
- **R11.** Message `get_metadata_identity` replies with the committed identity (`metadata_server.cpp:125`). SM:4183 compares with `current_identity()`, which is equal unless a journal is mid-replay.

## 8. Torrent

**The maintainer has ruled: torrent moves after all of the port work and is shelved until he asks.** It stays in Macha through the port. The 23 requests stay, the SM19 torrent section and DLT9 stay live, "`macha-metadata-dump` identical" holds as first written, and Macha has one space. Dagda holds no torrent code, in the library or its tests: Macha's bytes for it are proved by Macha's own tests, or as opaque vectors.

SM16, 17, 19 and DLT9, 10, 11 pass through the owner's codec as Macha writes them. Dagda sees payload bytes and a hash. C46 treats those sections as bytes and parses none. `test_torrent_requests_ride_the_snapshot_and_its_deltas` (SM:4624), `test_a_delta_refuses_a_torrent_request_with_no_id` (SM:4732) and the coordinator's cases stay Macha's. Several gate cases mention torrent only to set `config.torrent.enabled = false`; Dagda's versions have no such line.

The `-torrent` fixture is owed by the Macha session at S7: it cuts it on a node when told S7 has started, as the laptop has no torrent plugin. `journal-frames-0.93.4` will not exist: 0.93.4 only writes `journal.log` empty (`metadata.cpp:4039`, `:4070`), so the journal reader (Q3) has no fixture from live code.

## 9. Fixtures

**What was checked.** A script opened the scratch fixture under its own test key and recomputed tags and hashes. All 13 frames on each node, `heads.meta`, `checkpoint.meta` and `author.meta` authenticate under the magic as AAD; all 12 full frames re-hash. **Forms present:** generation 1 `DHTMGEN1`, generation 2 SM14 (122 bytes), 10 frames of SM17 (178 bytes), generation 8 DLT5 (78 bytes). That is the README's gap filled: it does not state forms. Nodes `a` and `b` hold the same history; `b` has no `author.meta`. `storage-layout` is `macha-state-layout-v18`.

### What S2 needs

**From `state-scratch-0.93.4/`:** `metadata-objects/` under domain v1's sealing; `macha-metadata-dump-*.txt` says 3 nodes, 2 leaves, 1 branch, depth 2, 7 entries, 1 extent, root `ade47a9a0e53...`. Gives one real `MNB1` to re-encode and one real root to reach only by an owner's codec. Its `MNL1` leaves cannot be parsed without that codec.

**Cut 2026-11-06, 25 cases, and S2 passes them all; 20 of the cases are checked in at `dagda/test/data/tree-vectors-0.93.4/`:** `HeadsTreeVectors.*`. What it was cut to, **`tree-vectors-0.93.4/`, by name:** boundary known answers for each of the four domain strings, at several levels; the empty leaf's id; an `MNB1` at depth 3; a file past 8 extents so `MNX1` and `MNY1` exist, which the fixture lacks; a boundary-run case hitting the packed fallback (`tree.cpp:118-124`); before and after roots of one update, with node ids; a README naming the commit.

### What S3 needs

**From `state-scratch-0.93.4/`:** the frame layout; a real genesis, SM14, SM17 and DLT5 chain; one certificate; `author.meta`; one absent `author.meta`; a checkpoint; an empty journal. Only C15 and C46's opaque checks use these, as Dagda has no owner's codec for them. **From `goldens-0.93.4/`:** S3 and S5 take the 38 `diagnostics.metadata.*` and `diagnostics.convergence.*` keys and 16 `cluster.metadata_*` and `startup.metadata` keys present in `status-diagnostics.json` and `status.json`, so `dagda.heads.*` feeds each (second-cutover 8c.2). Prefer `mask-list-with-cac5998.txt`. `coverage-status.txt` does not mark any of those keys unreached. The journal lines are the Macha log texts the facade keeps.

**More to cut, by name** (artefacts under the freeze change no tracked file; `gen/state_run.py` is the generator):
- `state-scratch-0.93.4-branch/`: a partition, a merge commit with a DLT delta (`merge_parents = 1`), a standing conflict, a generation-gap delta. Needs DLT6 or DLT7, DLT8 and DLT10.
- `state-scratch-0.93.4-delete/`: a delete past the first, so SM19 and DLT11 exist with a tombstone batch object.
- `state-scratch-0.93.4-truncated/`: past 256 records or with a low threshold, so a `previous_known = 0` root.
- `state-scratch-0.93.4-torrent/`: SM16 and DLT9 as bytes; a torrent request needs the plugin, so if it cannot be made say so and those forms are proved by Macha's tests alone.
- `state-scratch-0.93.4-markers/`: `set-aside.meta`, `recovery.required`, `author-chain-broken`, a `.corrupt.` file, a two-head `heads.meta` with `required` above 1.
- `journal-frames-0.93.4/`: one frame of each `JOURNAL_*` kind, by the test helper, or Q3 says none is needed.
- **The cache and its seed.** `cache.path` is outside `state_path` (`gen/state_run.py:27`, `:52-53`) and was not copied, so **the fixture holds no `current.meta`**. Rerun with the cache copied, under every state above.
- `macha-metadata-dump --all --stats --tree` beside each, as the 0.93.4 set has.

**A real state at 0.93.4,** with its seed, for the port's gate. The copy on live-a is at 0.92.4 (`migrate-dryrun/README.md`) and the migration's dry run there predates this pass. The maintainer's call (F11).

## 10. Questions for the maintainer, each with a recommendation

- **Q1. Macha's FS codec in Dagda's tests.** `ACTIVE.md` step 4 says it lives there; the rulings since say Dagda holds no torrent code "in the library or its tests", and SM16, 17, 19 and DLT9 contain the torrent section. *Recommended:* a neutral owner in Dagda; Macha's records as opaque vectors; the FS codec's bytes and real replay proved on Macha's port branch. Section 4.3 rests on this.
- **Q2. `<state>/metadata/` under v19: which key and which path.** You ruled it migrates at 8c, the domain in every AAD, ledgers under the node-local key. "Ledger" did not name these files. *Recommended:* section 4.1: content under the domain's key, as objects are, so a node without the domain's key cannot read it; `heads.meta`, `author.meta` and `set-aside.meta`, this node's own standing, under the node-local key; everything moves to `<state>/heads/<space id hex>/`. *The alternative:* keep `metadata/` as the name, and all under the domain's key: less to build, and the literal rule unmet for those three.
- **Q3. The journal reader.** Macha's `load_journal` has no production writer (its `TODO/ACTIVE.md:26-27`, "a question for the operator"); `compact()`'s thresholds, `pending_history_` and `cur_` differing from `committed_` stand only on it. *Recommended:* Dagda keeps the reader in S3, because the frozen code has it, its two cases gate it and a Yes must not rest on the live nodes being clean; it gets no writer. Retire it in Macha first, then here, if you want it gone. Answering "none" removes SM:1872 and SM:2069's replica half with named retirements.
- **Q4. What `renew` means.** See 2.4. *Recommended:* it writes nothing and reports standing. Not needed for Macha.
- **Q5. `base` and determinism.** History truncates per node, so two reconcilers may differ in holding the ancestor. A merge that uses `base` then can yield sibling merge commits, against H8. Macha is unaffected: it passes none, and the catalogue's use is outside the commit (`mgr:1952-1961`). *Recommended:* state it in the contract; pin with C40.
- **Q6. A real state for the gate.** F11. *Recommended:* a copy at 0.93.4, migrated and replayed on Macha's port branch, before the later stage. If you decide against, the Yes says what it rests on.

Standing and not reopened: O(state) per delta, the policy disagreement loop, wire-sized allocations and the rest of Macha's design defects, "left" (Macha `TODO/ACTIVE.md:79-82`); Dagda follows. Bounding each `reserve` by what remains costs nothing and needs no format change, so S3 does it; the cited `metadata.cpp:1951` could not be matched at `b080c38` and I did not look for it again.

## 11. Findings (reported, not fixed)

**Macha's code, by reading; none run.**
- **D1.** Nothing writes `journal.log` (a script finds only readers; `ecce886` says so). So `compact()`'s 128-record and 8 MiB thresholds (`md.cpp:4050-4064`) count only frames an older version left, `repair_once` calling it (`mgr:2023`) is a no-op on a new state, and `pending_history_` is set only from a journal. Macha knows (`TODO/ACTIVE.md:26-27`). `docs/metadata.md` does not mention the journal.
- **D2.** `set-aside.meta` is plain, with no magic or seal (`md.cpp:2933-2941`), and it is read without one. The only unsealed metadata file with content. `docs/metadata.md:38` does not say. Harmless by content; it forces a choice in section 4.1.
- **D3.** A corrupt `author.meta` throws out of `reserve_mutation_dot` (`md.cpp:2160-2162`) and every mutation with it, while every other sealed file there recovers or quarantines. R10.
- **D4.** R1's unreachable branch and `resolve_conflict` on an unknown id.
- **D5.** `load_history`'s and `load_journal`'s handling compares an exception's text (`md.cpp:2302`, `:2627`). Dagda decides by type.
- **D6.** `MetadataManager::maybe_reconfigure` runs in `repair_once` under the mutation lock and publishes to all active with `FrameType::control` (`mgr:1333`, `:1986`), so a stalled peer can hold that lock through `commit_call`'s stall window. Read only; not traced whether `write_stall` bounds it in practice.

**Macha's notes against its code.**
- **N1.** `mgr:1151-1153`: "maybe_reconfigure() commits the transition at max(old_floor, new_floor)"; since `ecce886` it changes `data_replication` alone.
- **N2.** `mgr:1449-1450`: "background convergence advances the horizon to generation 2"; nothing writes `metadata_branch_floor` (a script finds only the guard and the copies).
- **N3.** `service/maintenance.cpp:158-159` and `md.hpp:78-79`: a zero retirement time is "stamped by" maintenance; `766617e` removed the stamp.
- **N4.** `docs/metadata.md:71` "a merge mints no dots" against `mgr:1250-1256` (R3).
- **N5.** `docs/metadata.md:16`: "any connected pair holds each commit on two nodes"; `:7` and the code say copies are sought for a merge only, and a mutation reaches the pair through the replicator.
- **N6.** `test_a_manager_without_a_store_pages_a_map_backed_view` constructs a manager with a store (F8).
- **N7.** `TODO/ACTIVE.md:3`, `:14`, `:17`: "live 0.92.5", "not merged, not deployed"; the commit is that merge, and the fixture READMEs say 0.93.4 runs. It was not updated at the merge, which its own head warns about.
- **N8.** `TODO/2026-11-06-ingest-plugins-torrent-removal-plan.md:78` puts the section's removal "inside 8c" as a phase; a proposal, the maintainer has not ruled. Its line "the libraries should be told now not to invest further in torrent parity" (`:82-83`) is unsent by its own account.
- **N9.** Macha's `TODO/ACTIVE.md:56-61` says Dagda is planned on `cac5998`; superseded by this re-base.

**Dagda.**
- **D11.** `dagda-migrate` under a v19 marker reports "nothing to do" (`dagda/migrate/migrate.cpp:580-581`, `docs/migration.md:113-114`), so it cannot do a later pass as it stands; and `docs/migration.md:57-58` and `:69` say `metadata/` waits "until heads define it" and name 0.92.x. S7.
- **D12.** `Node::maintenance_loop` polls where Macha parks (`node.cpp:628-659`). Known, in `ACTIVE.md`; it now gates the later stage since Dagda's loop takes over there.

**Documents this file cannot change, for whoever edits them next.**
- **F1 (requirements).** `test_metadata_history_checkpoint_concurrent_proposers_converge`, named in H16 and 8c's gate, is not in `tests/` at `9a08846`. Nearest by name: RPC:4590, RPC:3859.
- **F2 (requirements).** Proof cells on other subjects: H1, H12, H15, H25's. Section 1 gives the names.
- **F4 (requirements).** 4.3's boundary domains omit `.../extent/v1` (`tree.cpp:188`).
- **F5 (requirements against Dannan).** H24 has "a Dannan advert per space", capped at 64 per node. Lir's 768 do not fit. Macha's one does.
- **F6 (`ACTIVE.md`).** Step 4's "Macha's FS codec and merge live in the tests" against later rulings (Q1). `:1137`, `:1146`, `metadata_manager.cpp:1919`, `:1376` are stale; `metadata-repair` is missing from what "comes with heads".
- **F7 (stack document).** "Wire and formats" and "Settled earlier" still say layout v18.
- **F8.** N6, and `test_metadata_identity_rpc` (R11).
- **F9 (second-cutover plan).** Section 8c is at `00861dd` names, with `tests/test_namespace_migration.cpp`; its "likely to go" list is now confirmed in section 5. Its B11 stands (2.2), and its question 1 is answered by the stages.
- **F10 (`ACTIVE.md`).** P5 and P8 have heads behind P7 and "against `808fa0f`"; the maintainer's ruling puts heads first and the baseline at `9a08846`. "No tree-native merge under the freeze" is obsolete. S1 no longer exists as P5 describes.
- **F11 (`ACTIVE.md` against Macha's `TODO/ACTIVE.md`).** Whether the real migration on a copy and the live-b rehearsal go ahead: Dagda's says yes, Macha's says the maintainer said "No drop that, I'll decide later". Unresolved here. Q6.
- **F12 (test mapping).** At `00861dd`; 792 cases now. Its `test_namespace_migration.cpp` rows point at `NS`. Section 5.3 lists the 15 new names not heads'.
