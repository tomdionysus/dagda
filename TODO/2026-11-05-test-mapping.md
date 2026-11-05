# Macha to Dagda: the test mapping

Status: written 2026-11-05 for the port. Nothing here has been done: no case is retired yet.

**Scope.** Every case in every test file under `../macha/tests` at `00861dd` (0.92.3, `develop`): the store, ledger, retention, availability, io_pressure, presence and storage_v18 cases, and every other case that stands on the store. Heads and metadata cases are included as GAP-HEADS. Transport cases with no store under them are KEEP here; Dannan's mapping deals with them.

**The rule.** Tom, 2026-11-04 night (`ACTIVE.md`, Decisions): a Macha case whose subject becomes Dagda's may be retired against a named `dagda_tests` case, mapped case by case; everything Macha-visible stays. Macha's tests keep pinning the torrent bytes.

**Fates.**
- RETIRE: the subject becomes Dagda's, and the named cases prove the same thing.
- REWRITE: Macha-visible; stays in Macha, rewritten over `dannan::Node` and `dagda::Node` fixtures.
- KEEP: Macha's own and untouched by the store swap.
- GAP: should retire, but no Dagda case proves it yet.
- GAP-HEADS: subject is the heads machinery, not built (step 4); names the row H1 to H30 of `2026-11-03-macha-store-requirements.md` where one fits.

**Method and its limits.**
- Cases were enumerated from `git show 00861dd:tests/...` by the three registering macros in `tests/test_framework.hpp`: `MACHA_TEST`, `MACHA_FAST_TEST` and `MACHA_HEAVY_TEST`. There are no other forms. `MACHA_FAST_TEST` is why counts from the other two looked low: `test_storage_metadata.cpp` has 61 of it, 6 `MACHA_TEST` and 1 `MACHA_HEAVY_TEST`.
- Dagda cases are `dagda/test/*.cpp` as of `9c190a9`, 418 of them. Every name under RETIRE was checked to exist.
- Fates come from reading the code. Nothing was built or run on either side.
- Reading depth differs, and this matters when relying on a KEEP or REWRITE:
  - RETIRE and GAP: both bodies read and the assertions compared.
  - `test_filesystem_fuse.cpp`, `test_hydration_catalogue.cpp`, `test_torrent_disk_io.cpp`, `test_media_playback.cpp`, `test_fuse_subsystem.cpp`, and the catalogue and provider cases of `test_invariants.cpp`: store-suspect bodies were read; the rest were settled from the opening lines and a fixture scan.
  - The seven files listed as not touching the store: settled by a scan for store-bearing fixtures only.
- The work was split among parallel readers, one per group of files. Each RETIRE was judged by the reader that compared its bodies.
- Review of those readers, which is a check and not a second full read:
  - A random sample of 10 RETIREs with no doubt attached, drawn from outside `test_object_ledger.cpp` and `test_presence.cpp`, was compared assertion by assertion, and all 10 matched.
  - Of the 12 RETIREs that carried a doubt, the 3 under "Moved on review" were read at both ends and changed fate. The other 9 stand on their reader's comparison, with the doubt listed under Uncertain.
  - All KEEP rows were scanned for store types in their bodies and in-file helpers.
- A citation in a Dagda comment was never taken as proof. 18 cases that a Dagda test or fixture file cites are not RETIRE, because what cites them proves only part.

**Totals, 786 cases.**

| Fate | Cases |
|---|---|
| RETIRE | 143 |
| REWRITE | 167 |
| KEEP | 354 |
| GAP | 27 |
| GAP-HEADS | 95 |

432 cases are not KEEP. That is above the Macha session's estimate of 238 to 282, mainly because this count includes the 95 GAP-HEADS and every case whose fixture holds the store, whatever its subject. Against the estimate's samples: `test_storage_metadata.cpp` 52 of 68 (estimated 54 of 67), `test_invariants.cpp` 34 of 40 (26), `test_rpc_cluster.cpp` 73 of 114 (46).

**Per file.**

| File | Cases | RETIRE | REWRITE | KEEP | GAP | GAP-HEADS |
|---|---|---|---|---|---|---|
| test_absent_nodes.cpp | 8 | 6 | 0 | 2 | 0 | 0 |
| test_availability.cpp | 13 | 4 | 3 | 6 | 0 | 0 |
| test_baselines.cpp | 4 | 3 | 0 | 1 | 0 | 0 |
| test_book_conversion.cpp | 3 | 0 | 0 | 3 | 0 | 0 |
| test_catalogue_maintenance.cpp | 15 | 0 | 13 | 2 | 0 | 0 |
| test_claim_walk.cpp | 8 | 8 | 0 | 0 | 0 | 0 |
| test_contract.cpp | 17 | 0 | 2 | 14 | 1 | 0 |
| test_epub_metadata.cpp | 24 | 0 | 0 | 24 | 0 | 0 |
| test_filesystem_fuse.cpp | 17 | 0 | 13 | 3 | 1 | 0 |
| test_foundations.cpp | 47 | 0 | 0 | 41 | 6 | 0 |
| test_fuse_subsystem.cpp | 7 | 0 | 7 | 0 | 0 | 0 |
| test_horizon_builder.cpp | 6 | 1 | 5 | 0 | 0 | 0 |
| test_http_compression.cpp | 3 | 0 | 0 | 3 | 0 | 0 |
| test_http_server.cpp | 16 | 0 | 0 | 16 | 0 | 0 |
| test_hydration_catalogue.cpp | 44 | 0 | 26 | 16 | 1 | 1 |
| test_invariants.cpp | 40 | 13 | 13 | 6 | 6 | 2 |
| test_io_pressure.cpp | 13 | 13 | 0 | 0 | 0 | 0 |
| test_ledger_gates.cpp | 7 | 7 | 0 | 0 | 0 | 0 |
| test_lifecycle_record.cpp | 2 | 0 | 2 | 0 | 0 | 0 |
| test_maintenance_trace.cpp | 10 | 0 | 7 | 3 | 0 | 0 |
| test_media_playback.cpp | 16 | 0 | 7 | 9 | 0 | 0 |
| test_metadata_merge.cpp | 19 | 0 | 0 | 19 | 0 | 0 |
| test_mobi_metadata.cpp | 29 | 0 | 0 | 29 | 0 | 0 |
| test_models.cpp | 3 | 0 | 0 | 2 | 1 | 0 |
| test_namespace_entries.cpp | 8 | 0 | 0 | 0 | 0 | 8 |
| test_namespace_migration.cpp | 15 | 0 | 9 | 3 | 0 | 3 |
| test_namespace_tree.cpp | 33 | 0 | 0 | 14 | 0 | 19 |
| test_node_services.cpp | 6 | 0 | 5 | 0 | 0 | 1 |
| test_object_ledger.cpp | 25 | 25 | 0 | 0 | 0 | 0 |
| test_observation.cpp | 17 | 0 | 1 | 16 | 0 | 0 |
| test_predicates.cpp | 5 | 0 | 0 | 0 | 5 | 0 |
| test_presence.cpp | 9 | 9 | 0 | 0 | 0 | 0 |
| test_root_parts.cpp | 5 | 0 | 0 | 1 | 4 | 0 |
| test_rpc_cluster.cpp | 114 | 25 | 23 | 41 | 0 | 25 |
| test_runtime_dependencies.cpp | 14 | 0 | 1 | 13 | 0 | 0 |
| test_service_root.cpp | 12 | 0 | 12 | 0 | 0 | 0 |
| test_session.cpp | 10 | 0 | 5 | 5 | 0 | 0 |
| test_storage_metadata.cpp | 68 | 11 | 5 | 16 | 0 | 36 |
| test_storage_v18.cpp | 21 | 17 | 2 | 0 | 2 | 0 |
| test_subsystem_supervisor.cpp | 4 | 0 | 1 | 3 | 0 | 0 |
| test_torrent_disk_io.cpp | 21 | 0 | 1 | 20 | 0 | 0 |
| test_transcode_timeline.cpp | 4 | 0 | 0 | 4 | 0 | 0 |
| test_users.cpp | 12 | 0 | 4 | 8 | 0 | 0 |
| test_web_api.cpp | 8 | 0 | 0 | 8 | 0 | 0 |
| test_work_class.cpp | 4 | 1 | 0 | 3 | 0 | 0 |
| Total | 786 | 143 | 167 | 354 | 27 | 95 |

**Files that do not touch the store, no table below.** 87 cases, all KEEP, with no store-bearing fixture in the file: `test_book_conversion.cpp` (3), `test_epub_metadata.cpp` (24), `test_http_compression.cpp` (3), `test_http_server.cpp` (16), `test_mobi_metadata.cpp` (29), `test_transcode_timeline.cpp` (4), `test_web_api.cpp` (8).

## The tables

### test_absent_nodes.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_an_object_matures_a_grace_after_its_first_sighting | RETIRE | LedgerUnreferencedSince.AnObjectMaturesAGraceAfterItsFirstSighting |
| test_an_object_referenced_again_starts_its_wait_afresh | RETIRE | LedgerUnreferencedSince.AnObjectReferencedAgainStartsItsWaitAfresh |
| test_a_clock_set_back_restarts_the_wait | RETIRE | LedgerUnreferencedSince.AClockSetBackRestartsTheWait |
| test_a_completed_sweep_drops_sightings_of_objects_no_longer_held | RETIRE | LedgerUnreferencedSince.ACompletedSweepDropsSightingsOfObjectsNoLongerHeld |
| test_sightings_survive_a_restart | RETIRE | LedgerUnreferencedSince.SightingsSurviveARestart |
| test_the_sightings_codec_round_trips_and_refuses_damage | RETIRE | LedgerUnreferencedSince.TheSightingsCodecRoundTripsAndRefusesDamage |
| test_a_node_unheard_of_for_the_horizon_is_forgotten | KEEP | transport, no store: Membership roster alone; forget_unseen is Dannan membership (Dannan mapping) |
| test_forgetting_counts_from_when_the_node_was_last_heard_of | KEEP | transport, no store: Membership roster alone; forget_unseen is Dannan membership (Dannan mapping) |

### test_availability.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_a_trees_children_are_exactly_its_extent_references | KEEP | namespace tree node format and children over an in-memory node store; FS format, no store |
| test_the_path_table_follows_a_change_to_the_tree | KEEP | path table over namespace tree diff; Macha's, no store (held and survey are plain inputs) |
| test_trie_questions_and_answers_survive_the_wire | RETIRE | LedgerTrieDiff.QuestionsAndAnswersSurviveTheWire |
| test_a_survey_a_peer_could_not_finish_keeps_what_was_unavailable | RETIRE | LedgerAvailability.ASurveyAPeerCouldNotFinishKeepsWhatWasUnavailable |
| test_the_survey_by_diff_finds_what_no_node_holds | RETIRE | LedgerAvailability.TheSurveyByDiffFindsWhatNoNodeHolds |
| test_what_this_node_lacks_is_the_referenced_set_less_every_disk | RETIRE | LedgerAvailability.WhatThisNodeLacksIsTheReferencedSetLessEveryDisk |
| test_a_files_code_follows_its_extent_counts | KEEP | file availability code over path facts; pure Macha API vocabulary, no store |
| test_items_take_their_best_file_and_sets_count_their_members | KEEP | catalogue item availability over a plain snapshot, no store |
| test_the_item_table_is_rebuilt_only_when_a_snapshot_changes | KEEP | catalogue item table cache, no store |
| test_a_file_the_survey_cannot_decide_takes_its_last_known_count | KEEP | path table carry-over by file content and its on-disk codec; Macha's, plain inputs, no store |
| test_what_only_a_host_out_of_reach_holds_is_unavailable | REWRITE | Pins a file's code moving with no commit when a host goes and returns (Service). Survey half is StoreRepair.WhatOnlyAHostOutOfReachHoldsIsUnavailable; the path table following it is Macha's. |
| test_two_nodes_survey_what_neither_holds | REWRITE | Pins path rollups, last-survey.bin, files and catalogue HTTP answers on two Services. Survey half only is StoreRepair.TwoNodesSurveyWhatNeitherHolds. |
| test_a_disk_coming_back_is_surveyed_without_a_namespace_change | REWRITE | Pins a file's extents_local following a disk with the head unmoved (Service). Survey half is StoreRepair.ADiskComingBackIsSurveyedWithNothingReferencedChanging; path table wiring is Macha's. |

### test_baselines.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_baseline_data_store_has | RETIRE | StoreStoragePool.DataStoreHasBaseline |
| test_baseline_claim_walk_per_object | RETIRE | LedgerClaimWalk.BaselineClaimWalkPerObject |
| test_baseline_claim_walk_on_the_ledger_per_object | RETIRE | LedgerClaimWalk.BaselineClaimWalkOnTheLedgerPerObject |
| test_baseline_observation_probe_costs | KEEP | Observation probes and HTTP route label cost; no store |

### test_catalogue_maintenance.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_without_metadata_nothing_installs_and_the_read_is_incomplete | REWRITE | Catalogue install and maintenance head against no metadata; stands on BareNode and DistributedStore |
| test_the_read_is_complete_only_when_the_view_is_at_the_head | REWRITE | Catalogue inventory complete only at the head; provider's answer, Macha's gate; BareNode and DistributedStore |
| test_a_head_behind_the_known_generation_reads_the_record_first | REWRITE | Catalogue head taken from the record when behind the node's known generation; BareNode and DistributedStore |
| test_a_commit_installs_what_it_wrote_once | REWRITE | Catalogue install count and read-your-write; BareNode and DistributedStore |
| test_an_unchanged_root_is_never_installed_again | REWRITE | Catalogue install follows its root, not the head; BareNode and DistributedStore |
| test_another_writers_commit_is_installed_on_the_next_head | REWRITE | Catalogue view and revision across writers; BareNode and DistributedStore |
| test_a_root_that_cannot_be_read_keeps_the_view_serving | REWRITE | Catalogue keeps serving and reports unavailable on an unreadable root; BareNode and DistributedStore |
| test_an_install_racing_a_commit_takes_what_it_wrote | REWRITE | Catalogue load count under a racing install, root read from the control store; BareNode and DistributedStore |
| test_resident_bytes_count_what_the_catalogue_holds | KEEP | catalogue memory accounting on a plain snapshot, no store |
| test_a_view_holds_each_entry_once_in_its_shard | KEEP | catalogue view sharding on a plain snapshot, no store |
| test_what_a_commit_installs_is_what_a_load_of_its_root_reads | REWRITE | Catalogue commit equals a load of its root; BareNode and DistributedStore |
| test_a_load_reuses_every_shard_the_new_root_still_names | REWRITE | Catalogue load shares unchanged shards; BareNode and DistributedStore |
| test_a_write_shares_every_shard_it_did_not_touch | REWRITE | Catalogue write shares untouched shards; BareNode and DistributedStore |
| test_data_is_released_only_when_nothing_refers_to_it | REWRITE | Catalogue garbage for artwork and media indexes across shards; provider's answer; BareNode and DistributedStore |
| test_artwork_is_staged_in_the_class_of_whoever_asked | REWRITE | Catalogue chooses the admission class for artwork, read from data stats; BareNode and DistributedStore |

### test_claim_walk.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_retention_ledger_pages_every_live_claim_once | RETIRE | LedgerClaimWalk.TheRetentionLedgerPagesEveryLiveClaimOnce |
| test_retention_ledger_stops_for_cancellation_first | RETIRE | LedgerClaimWalk.TheRetentionLedgerStopsForCancellationFirst |
| test_retention_ledger_holds_by_class | RETIRE | LedgerClaimWalk.TheRetentionLedgerHoldsByClass |
| test_retention_ledger_publishes_horizons_and_refuses_an_incomplete_release | RETIRE | LedgerClaimWalk.TheRetentionLedgerPublishesHorizonsAndRefusesAnIncompleteRelease |
| test_retention_ledger_forwards_claims_releases_prunes_and_compacts | RETIRE | LedgerClaimWalk.TheRetentionLedgerForwardsClaimsReleasesPrunesAndCompacts |
| test_claim_walk_matches_the_walk_it_replaced_exhaustively | RETIRE | LedgerClaimWalk.MatchesTheWalkItReplacedExhaustively |
| test_claim_walk_matches_the_walk_it_replaced_at_the_bound | RETIRE | LedgerClaimWalk.MatchesTheWalkItReplacedAtTheBound |
| test_claim_walk_follows_a_changing_ledger | RETIRE | LedgerClaimWalk.FollowsAChangingLedger |

### test_contract.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_waits_compose_and_include_over_every_mask | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_only_control_is_refused_and_only_device_or_network_waits | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_work_may_allow_itself_less_never_more | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_wait_guard_records_or_throws | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_work_context_deadline_and_cancellation | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_data_work_context_is_the_data_specialisation | GAP | StoreIoPressure.ControlCannotEnterTheDataArbiterAtAll covers the control refusal, not quantum_bytes nor frame_type through the WorkContext base |
| test_budget_bounds_are_spent_exactly | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_a_yield_source_is_destroyed_through_its_interface | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_budget_stop_precedence_over_every_combination | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_cursor_and_page | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_published_is_empty_until_the_first_publish | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_published_refuses_a_missing_snapshot | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_a_held_handle_keeps_its_snapshot_across_republish | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_publish_destroys_the_previous_snapshot_outside_its_lock | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_published_readers_see_whole_snapshots_in_order | KEEP | contract vocabulary, Dannan's sched and contract; no store |
| test_the_wait_guard_on_snapshot_views | REWRITE | TestService; pins MetadataManager and catalogue snapshot_view wait declarations |
| test_the_wait_guard_on_a_cold_catalogue | REWRITE | BareNode with DistributedStore; pins the cold catalogue wait declaration |

### test_filesystem_fuse.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_fuse_signal_exit_is_a_clean_service_shutdown | KEEP | pure FUSE loop result classifier, no store |
| test_a_node_clears_only_its_own_abandoned_write_files | KEEP | FileSystem static over plain tmp files, no store under it |
| test_fuse_journal_frame_scanner_exhaustive_tail_model | KEEP | FUSE journal frame scanner over bytes, FS format, no store |
| test_publication_writers_stage_and_commit_generations | REWRITE | FS WriteHandle generations, overlay and commit guard; FilesystemNode stands on TestNode |
| test_publication_writer_failure_stays_invisible | REWRITE | failed writer leaves FS size 0, Macha-visible; FilesystemNode on TestNode |
| test_local_snapshot_view_is_local_before_cluster_forms | REWRITE | FileSystem local_snapshot_view before the floor forms; TestNode, MetadataManager |
| test_fuse_frontend_local_semantics | REWRITE | FUSE overlay, inode, fsync semantics; FilesystemNode on TestNode |
| test_fuse_publication_scheduling | REWRITE | FUSE publication scheduling; FilesystemNode on TestNode |
| test_fuse_admission_backpressure | REWRITE | FUSE spool admission and backpressure; FilesystemNode on TestNode |
| test_fuse_publication_failures_back_off_and_park | REWRITE | FUSE publication back-off and parking; FilesystemNode on TestNode |
| test_fuse_publication_backlog_wider_than_ledger_completes | REWRITE | FUSE writer cap against retained memory (not the store ledger); FilesystemNode on TestNode |
| test_fuse_namespace_journal_recovery | REWRITE | FUSE namespace journal recovery, FS format and semantics; FilesystemNode on TestNode |
| test_fuse_data_journal_recovery | REWRITE | FUSE data journal recovery; FilesystemNode on TestNode |
| test_management_api_reports_and_acts_on_fuse_state | REWRITE | management API over FUSE state, Macha-visible; TestService |
| test_removing_empty_directories_in_a_burst_keeps_the_node_up | REWRITE | rmdir burst through the FUSE frontend, namespace visible; TestService |
| test_disconnected_maintenance_sleeps_until_peer_event | GAP | No Dagda case or code: maintenance_loop polls each interval (node.cpp:657-659), no peer-event wake. NodeMaintenance.TheNodesThreadRunsThePassesOnItsOwn covers running, not parking. (see Uncertain) |
| test_coalesced_delete_burst_wakes_at_exact_garbage_grace | REWRITE | FS unlink burst, snapshot.garbage and convergence counts meet in Service's pass. NodeMaintenance.TheDomainsGrace... covers the grace, not the exact wake. (see Uncertain) |

### test_foundations.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_spool_retirement_rate_is_aggregate | KEEP | FUSE spool estimator (fuse_frontend.hpp), no store |
| test_weighted_loader_service_is_work_conserving_and_non_starving | KEEP | FUSE loader share (fuse_frontend.hpp), no store; Dagda's Repair::Share is its own file-local copy |
| test_data_resource_arbiter_background_effort_ceiling | GAP | StoreDataWork.ACreditWaitGivesUpWhenNothingIsMoving covers a loader at the ceiling, not speculative counted, foreground exempt, nor background_limit/active/peak stats |
| test_data_resource_arbiter_reserves_viewer_headroom | GAP | StoreRepair.ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit covers headroom, not oversized refusal, loader served before speculative on release, or class stats |
| test_miniupnpc_igd_status_compatibility | KEEP | NAT compatibility, no store |
| test_codec_and_crypto | KEEP | codec, telemetry and primitives, no store; crypto tail mirrored by StoreSealing.MachaCodecAndCryptoSealOpenAndRefuseAFlippedBit, rest not Dagda's |
| test_membership_identity_reset_tombstone | KEEP | membership roster, Dannan's mapping; no store |
| test_membership_identity_reset_is_durable_without_metadata | KEEP | membership roster, Dannan's mapping; no store |
| test_membership_persists_gc_fence_and_requires_direct_reachability | KEEP | membership roster and all_known_reachable, Dannan's; no store |
| test_telemetry_identity_reset_freshness_boundary | KEEP | telemetry store is node telemetry, not the object store |
| test_membership_ip_identity_reset_without_node_id | KEEP | membership roster, Dannan's mapping; no store |
| test_durable_replace_file_matrix | KEEP | durable file primitive, Dannan's; no store |
| test_torrent_uri_safety_matrix | KEEP | torrent, ingest and catalogue state names, no store |
| test_thread_cpu_reporter_debug_escalation | KEEP | diagnostics, no store |
| test_config | KEEP | config defaults and logging, no store; cpu scale also in NodeLocalMaintenance.TheCpuScaleIsMachas |
| test_placement | GAP | StorePlacementVectors.DataOrderIsMachasOnEveryVector pins determinism and domain diversity by known answer, not stability when a node is added (see Uncertain) |
| test_capacity_placement | GAP | StorePlacementVectors.DataOrderIsMachasOnEveryVector pins orders and logical capacity on fixed sets, not the small node's exact shard boundary nor R=1 monotonic stealing (see Uncertain) |
| test_retained_memory_ledger_preserves_priority_headroom | KEEP | RetainedMemoryLedger, Dannan's sched; no store |
| test_retained_memory_ledger_never_starves_rpc_reassembly | KEEP | RetainedMemoryLedger, Dannan's sched; no store |
| test_retained_memory_reassembly_reserve_is_bounded_not_absolute | KEEP | RetainedMemoryLedger, Dannan's sched; no store |
| test_retained_memory_ledger_sheds_borrowed_cache_for_viewer | KEEP | RetainedMemoryLedger, Dannan's sched; no store |
| test_retained_memory_ledger_restores_durable_overcommit | KEEP | RetainedMemoryLedger, Dannan's sched; no store |
| test_every_subsystem_thread_is_run_supervised | KEEP | scans Macha's own src for run_supervised; no store |
| test_supervised_restart_delay_doubles_to_its_ceiling | KEEP | supervised threads, Dannan's; no store |
| test_a_supervised_loop_that_throws_runs_again | KEEP | supervised threads, Dannan's; no store |
| test_a_supervised_loop_in_backoff_stops_when_asked | KEEP | supervised threads, Dannan's; no store |
| test_a_supervised_task_that_throws_ends_on_record | KEEP | supervised threads, Dannan's; no store |
| test_an_escalating_thread_hands_its_fault_to_the_sink | KEEP | supervised threads, Dannan's; no store |
| test_normalize_config_defaults_plugin_path_to_the_build_default | KEEP | config, no store |
| test_publication_open_writer_bound_fits_the_loader_reserve | KEEP | config normalisation of FUSE publication, no store |
| test_torrent_binds_the_advertised_address_not_libtorrent_enumeration | KEEP | torrent config, no store |
| test_retry_budget_is_reachable_once_backoff_reaches_its_ceiling | KEEP | Macha's retry_policy.hpp, pure, no store |
| test_normalize_config_preserves_an_explicit_plugin_path | KEEP | config, no store |
| test_fuse_mountpoint_preflight_counts_stray_entries | KEEP | FUSE mountpoint, no store |
| test_fuse_mountpoint_preflight_refuses_unrelated_filesystem | KEEP | FUSE mountpoint, no store |
| test_node_info_flags_travel_on_the_wire_and_in_the_roster | KEEP | node records and roster, Dannan's; no store |
| test_gc_fence_excludes_pairs_that_can_never_dial_each_other | KEEP | Membership::all_known_reachable, Dannan's; no store |
| test_configuration_tristates_and_hosting_rules | KEEP | config parsing and warnings, no store |
| test_transcode_rates_keep_a_median_per_class_and_survive_a_restart | KEEP | playback rate book, no store |
| test_coverage_child_profile_name | KEEP | test tooling, no store |
| test_message_routes_answer_only_while_bound | KEEP | transport, no store |
| test_rpc_refusals_say_whether_asking_again_may_help | KEEP | transport, no store |
| test_node_events_count_each_kind | KEEP | Macha's NodeEvents counter, pure, no store |
| test_write_behind_acts_once_per_window | GAP | StoreWriteBehind.AWindowFillsThenStartsAgain covers filling and restarting, not that an overfill leaves no carry (window_filled(0) false after a large write) |
| test_write_behind_leaves_what_was_written_intact | GAP | StoreWriteBehind.WritebackLeavesTheFileAsWritten checks only file size, never reads the bytes back; the content comparison is missing |
| test_a_connectivity_probe_holds_no_reader_of_the_status | KEEP | public connectivity, no store |
| test_a_roster_being_written_holds_no_reader_of_membership | KEEP | membership roster, Dannan's mapping; no store |

### test_fuse_subsystem.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_fuse_subsystem_mounts_and_publishes_its_frontend | REWRITE | FUSE subsystem mount with a test driver; TestService |
| test_fuse_subsystem_clean_stop_is_not_a_fault | REWRITE | FUSE subsystem supervision; TestService |
| test_fuse_subsystem_rebuilds_the_mount_after_losing_it | REWRITE | FUSE subsystem rebuild after mount loss; TestService |
| test_fuse_subsystem_construction_fault_leaves_the_rest_of_the_node_serving | REWRITE | subsystem crash isolation, node keeps serving; TestService |
| test_fuse_subsystem_without_a_mount_path_is_unavailable | REWRITE | FUSE subsystem unavailable state; TestService |
| test_fuse_subsystem_without_a_mount_driver_is_unavailable | REWRITE | FUSE subsystem unavailable state; TestService |
| test_fuse_plugin_loads_over_the_real_dlopen_path | REWRITE | FUSE plugin dlopen path; TestService |

### test_horizon_builder.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_inventory_is_both_live_sets_and_the_catalogue_control_set | RETIRE | LedgerHorizonBuilder.TheInventoryIsBothLiveSetsAndTheControlSet |
| test_inventory_control_set_holds_the_namespace_tree | REWRITE | Namespace walk names every tree node incl. extent spines and throws on an unreadable node: Macha provider. Inventory half is in Dagda TheInventoryControlSetHoldsTheOwnersNodes |
| test_release_over_a_tree_matches_the_pass_build | REWRITE | Owner walk: extents not holes, conflict alternatives, both catalogue roots, tree nodes. Dagda TheReleaseAtAHeadIsWhatItRefersTo is handed those sets, proves dedupe and stamp only |
| test_an_unreadable_catalogue_root_leaves_the_release_incomplete | REWRITE | Provider must turn a throwing catalogue root into complete=false, keeping the rest. Dagda AnUnreadablePartLeavesTheReleaseIncomplete only passes a given complete=false through |
| test_an_unreadable_tree_node_leaves_the_release_incomplete | REWRITE | Provider keeps walked extents 1,2 and goes incomplete on an unreadable node. Dagda AHeadThatCannotBeReadLeavesTheReleaseIncomplete is an unknown head with an empty horizon: different |
| test_an_empty_head_releases_against_nothing | REWRITE | Walks an empty MetadataSnapshot through the FS and catalogue sources: provider half is Macha. Builder half is in Dagda LedgerHorizonBuilder.AnEmptyHeadReleasesAgainstNothing |

### test_hydration_catalogue.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_hydration_scheduler_orders_runs_and_follows_playback | KEEP | pure hydration scheduler and tracker, Macha's, no store |
| test_catalogue_three_way_merge | KEEP | catalogue merge over snapshots, no store |
| test_replica_selector | GAP | ReplicaSelector.* cover foreground rotation, promotion, penalty. Missing: speculative rotation, speculative in-flight load on equal peers, failures count kept after success. |
| test_cache_hydrator_fetches_to_persistent_cache | REWRITE | Macha's CacheHydrator over DistributedStore and block cache; BareNode. Hydration is not in Dagda |
| test_media_probe_and_metadata_providers | KEEP | media probe and metadata providers over fake HTTP, no store |
| test_musicbrainz_release_tracks | KEEP | provider parsing over fake HTTP, no store |
| test_catalogue_scanner_matches_binds_and_reconciles_a_library | REWRITE | catalogue scanner end to end; CatalogueNode on TestNode |
| test_catalogue_scanner_admits_only_what_is_ready_to_catalogue | REWRITE | catalogue scanner admission; CatalogueNode on TestNode |
| test_catalogue_non_coordinator_idle_does_not_spin | REWRITE | catalogue scanner thread idles on a non-coordinator; two Services |
| test_catalogue_hint_queue_follows_machadfs_rename_and_delete | KEEP | hint queue over a temp dir, no store |
| test_catalogue_hint_queue_persistence_coalescing_and_priority | KEEP | hint queue persistence over a temp dir, no store |
| test_an_ingest_and_its_torrent_read_one_catalogue_state | KEEP | ingest and torrent state mapping, pure, no store |
| test_a_torrent_jobs_publication_travels_the_wire_and_reaches_the_api | KEEP | torrent job JSON codecs and API, pure, no store |
| test_torrent_requests_belong_to_the_cluster_and_are_driven_by_their_owner | REWRITE | TorrentCoordinator and acquisition API, port gate; TestNode. LeaseClaimedCollection is the generalised part only |
| test_a_torrent_job_is_saved_only_when_its_record_changes | KEEP | torrent job save rule, pure, no store |
| test_the_torrent_plugin_restores_its_jobs_and_runs_them_through_the_engine | REWRITE | torrent plugin job restore and engine run; TestNode |
| test_ingest_plans_destinations_and_imports_files | REWRITE | IngestManager planning and import; CatalogueNode on TestNode |
| test_a_pause_or_cancel_accepted_during_the_final_rename_stands | REWRITE | ingest job control; CatalogueNode on TestNode |
| test_a_pause_or_cancel_accepted_while_the_worker_fails_stands | REWRITE | ingest job control; CatalogueNode on TestNode |
| test_ingest_files_are_paused_resumed_and_cancelled_one_by_one | REWRITE | ingest file control; CatalogueNode on TestNode |
| test_ingest_job_control_acts_on_workers_held_mid_copy | REWRITE | ingest job control; CatalogueNode on TestNode |
| test_ingest_rebases_when_its_partial_is_rolled_back | REWRITE | ingest rebase on a rolled back partial; CatalogueNode on TestNode |
| test_ingest_blocks_while_storage_is_unavailable | REWRITE | ingest job state storage_unavailable, Macha-visible; CatalogueNode, local_state backends |
| test_ingest_copies_when_published_extents_are_missing | REWRITE | ingest falls back to copying when the manifest lies, read through FS; TestService |
| test_catalogue_warm_read_defers_remote_refresh | REWRITE | catalogue and FUSE warm reads stay memory-only; BareNode, MetadataManager, DistributedStore |
| test_metadata_decoded_cache_ttl_recovers_missed_notice | GAP-HEADS:H7,H21,H24 | decoded view reused until the TTL, then a real read finds a head no notice announced; MetadataManager only (see Uncertain) |
| test_catalogue_effective_music_artwork_resolution | KEEP | catalogue artwork resolution over values, no store |
| test_media_indexes_round_trip_and_older_shards_still_read | KEEP | catalogue media index codec, no store |
| test_catalogue_on_one_node_predicts_caches_and_protects_its_control_objects | REWRITE | prediction, media index, catalogue CONTROL fencing; CatalogueNode. Its citing LedgerCollector case proves another Macha case, not this |
| test_a_catalogue_write_touches_its_own_shards_and_leaves_a_whole_catalogue | REWRITE | catalogue shard writes and reload; CatalogueNode on TestNode |
| test_a_page_resumes_after_its_cursor_and_never_repeats | KEEP | catalogue paging over values, no store |
| test_catalogue_lists_page_in_key_order | REWRITE | catalogue list paging; CatalogueNode on TestNode |
| test_catalogue_api_edits_searches_and_signs_artwork | REWRITE | catalogue API edits, search, signed artwork; CatalogueNode on TestNode |
| test_catalogue_sync_search_and_artwork_gc | REWRITE | catalogue sync, search, artwork GC, clear metadata via API; three Services on TestCluster |
| test_catalogue_uses_final_state_after_coalesced_metadata_burst | REWRITE | catalogue final state and GC after a burst; Services on TestCluster |
| test_a_failed_hint_outlives_the_job_that_raised_it | KEEP | hint queue over a temp dir, no store |
| test_staging_is_available_only_while_its_directory_exists | KEEP | ingest staging directory check, no store |
| test_an_ingest_waits_out_a_missing_staging_directory | REWRITE | ingest staging wait; CatalogueNode on TestNode |
| test_a_discarded_payload_leaves_at_once_and_is_deleted_later | KEEP | ingest staging trash over a temp dir, no store |
| test_books_are_catalogued_from_their_files_and_served_as_content | REWRITE | books catalogued and served through FS; CatalogueNode on TestNode |
| test_open_library_enriches_a_book_and_the_probe_alone_catalogues_it | KEEP | OpenLibrary provider over fake HTTP, no store |
| test_mobi_books_in_title_author_folders_are_read_and_old_matches_replaced | REWRITE | mobi layout scan; CatalogueNode on TestNode |
| test_books_are_converted_through_the_catalogue_api | REWRITE | book conversion routes; CatalogueNode on TestNode |
| test_ingest_files_a_book_under_its_first_author | REWRITE | ingest book planning; CatalogueNode on TestNode |

### test_invariants.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_convergence_demand_coalesces_burst_and_keeps_same_generation_event | KEEP | Self-contained ConvergenceDemand primitive in src/service, no store under it (see Uncertain) |
| test_unmatched_files_are_identified_by_hand_or_by_reference | REWRITE | ManageApi unmatched files and catalogue on CatalogueBench (TestNode) |
| test_a_titles_files_are_unmatched_and_deleted_one_call_each | REWRITE | ManageApi title file operations on CatalogueBench (TestNode) |
| test_provider_search_and_artwork_choice | REWRITE | ManageApi provider search and artwork on CatalogueBench (TestNode) |
| test_provider_release_tracks | REWRITE | ManageApi release tracks on CatalogueBench (TestNode) |
| test_identity_association_reset | REWRITE | Catalogue identity reset via ManageApi on CatalogueBench (TestNode), BareNode membership |
| test_service_answers_health_and_status_through_startup | REWRITE | Service health and status routes through startup; stands on TestCluster and Service |
| test_a_recovering_node_answers_peers_and_is_shown_recovering | REWRITE | Status API phase and readiness while recovering, on BareNode; storage stage names feed the gate |
| test_status_is_light_and_diagnostics_have_their_own_route | REWRITE | Status and diagnostics routes on BareNode via TestCluster |
| test_status_renders_each_node_from_what_it_knows | REWRITE | Status API rendering on BareNode, MetadataManager and local_state sources |
| test_a_node_advertises_its_api_endpoint_and_playback_budgets | KEEP | Pure config functions for API endpoint and playback budgets, no store |
| test_status_follows_a_peer_through_telemetry_and_departure | REWRITE | Status API over telemetry gossip and departure between two BareNodes |
| test_metadata_availability_logs_only_transitions | GAP-HEADS | MetadataManager availability: write_available and stable through pending reconciliation, one transition log. Heads not built; no H row names it (see Uncertain) |
| test_open_fuse_inodes_keep_their_identity | REWRITE | FUSE inode identity over FS on CatalogueBench (TestNode) |
| test_catalogue_keeps_what_the_namespace_holds | REWRITE | Catalogue rollback, scan fences, GC liveness on CatalogueBench; also uses DistributedStore::DurabilityBatch, which Dagda lacks (see Uncertain) |
| test_http_connections_are_bounded_and_reused | KEEP | HttpServer lifecycle, no store |
| test_replica_repair_does_not_count_corrupt_remote_as_healthy | GAP | A sender's repair healing a corrupt remote copy unscrubbed. DistributedStore.HealsABadLocalCopyFromAPeer covers scrub then pull on the corrupt node, not this (see Uncertain) |
| test_rebalance_reads_nothing_with_one_backend_online | RETIRE | StoreStoragePool.RebalanceReadsNothingWithOneBackendOnline |
| test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy | RETIRE | StoreStoragePool.RebalanceNeverDeletesTheLastValidCopyForACorruptPreferredOne |
| test_rpc_pre_auth_admission_is_bounded | KEEP | transport, no store |
| test_a_durability_barrier_is_timed_per_filesystem | RETIRE | StoreDurabilityDomain.ABarrierIsTimedPerFilesystem |
| test_authoritative_deferred_generation_batches_stable_storage_barriers | RETIRE | StoreLocalStoreDurability.DeferredGenerationsBatchStableStorageBarriers |
| test_durability_batch_retains_exact_nondominated_frontier | GAP | Dagda has no DurabilityBatch or DurabilityRequirement type and no test of a nondominated frontier over per-object replica generations; nearest is none (see Uncertain) |
| test_accounting_dirty_marker_is_process_session_scoped | RETIRE | StoreLocalStoreDurability.TheAccountingDirtyMarkIsOncePerSession |
| test_clean_accounting_checkpoint_is_trusted_with_packs | RETIRE | StoreLocalStoreDurability.ACleanAccountingCheckpointIsTrustedWithPacks |
| test_unclean_accounting_recovery_establishes_durable_baseline | RETIRE | StoreLocalStoreDurability.UncleanAccountingRecoveryEstablishesADurableBaseline |
| test_restart_durable_reaffirmation_requires_no_new_barrier | RETIRE | StoreLocalStoreDurability.ARestartedReaffirmationNeedsNoNewBarrier |
| test_authoritative_delete_is_lazy_durability | RETIRE | StoreLocalStoreDurability.AnAuthoritativeDeleteIsLazilyDurable |
| test_local_store_barrier_remembers_complete_physical_cut | RETIRE | StoreLocalStoreDurability.ABarrierRemembersTheWholeCut |
| test_durability_domain_group_commits_independent_publications | RETIRE | StoreLocalStoreDurability.ADomainGroupCommitsIndependentPublications, StoreDurabilityDomain.OneBarrierServesEveryWaiterInAWindow |
| test_storage_pool_tokens_name_physical_domain_and_backend_incarnation | RETIRE | StoreStoragePool.TicketsNameThePhysicalDomainAndTheBackendIncarnation |
| test_immediate_reaffirmation_flushes_provisional_generation | RETIRE | StoreLocalStoreDurability.AnImmediateReaffirmationFlushesTheProvisionalGeneration |
| test_claims_owed_to_slow_peers_stay_within_their_bound | GAP-HEADS:H15,H22 | Replicator backlog of commit claim ids bounded at 32 MB with oldest dropped and newest head offered; needs commits and the replicator |
| test_publication_generation_barrier_precedes_metadata_commit | REWRITE | FS writer commit with WriteDurability::publication_generation: one barrier at commit, none during write, on TestNode |
| test_persistent_cache_is_explicitly_ephemeral | GAP | Macha counts fsync and syncfs calls, zero. StorePersistentCache.TheCacheIsExplicitlyEphemeral covers stats, eviction, no barrier recorded and no accounting file; a raw fsync in the cache would pass (see Uncertain) |
| test_control_plane_hint_admission_is_storage_durable | KEEP | CatalogueHintQueue fsyncs on its own files, no store |
| test_deferred_object_barrier_rejects_stale_process_epoch | GAP | Direct refusal of a stale barrier and put reply epoch equal to the server's. DistributedStore.ADeferredWritesBarrierAcrossPeerRestarts covers refusal only indirectly, through its re-probe |
| test_rpc_durability_barrier_group_commits_independent_publications | GAP | One physical barrier for three barrier calls over the wire. StoreLocalStoreDurability.ADomainGroupCommitsIndependentPublications covers it in process, not through storage_server |
| test_rpc_durability_barrier_reuses_already_covered_generation | GAP | Barrier calls over the wire: covered generation costs none, newer costs one. StoreLocalStoreDurability.ABarrierRemembersTheWholeCut covers it in process, not through storage_server |
| test_authenticated_receiver_enforces_transport_lane | KEEP | transport, no store |

### test_io_pressure.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_an_ordinary_large_write_is_not_a_slow_device | RETIRE | StoreIoPressure.AnOrdinaryLargeWriteIsNotASlowDevice |
| test_pressure_onset_and_release_are_each_logged_once | RETIRE | StoreIoPressure.OnsetAndReleaseAreEachLoggedOnce |
| test_a_device_far_slower_than_it_should_be_is_noticed | RETIRE | StoreIoPressure.ADeviceFarSlowerThanItShouldBeIsNoticed |
| test_the_single_sample_trip_scales_with_the_operation | RETIRE | StoreIoPressure.TheSingleSampleTripScalesWithTheOperation |
| test_a_read_is_measured_with_the_bytes_it_returned | RETIRE | StoreIoPressure.AReadIsMeasuredWithTheBytesItReturned |
| test_maintenance_reads_are_part_of_the_service_time_they_spend | RETIRE | StoreIoPressure.MaintenanceReadsArePartOfTheServiceTimeTheySpend |
| test_a_viewer_is_never_refused_because_the_disk_is_slow | RETIRE | StoreIoPressure.AForegroundRequestIsNeverRefusedBecauseTheDiskIsSlow |
| test_the_loader_runs_freely_under_pressure_with_no_viewer | RETIRE | StoreIoPressure.TheLoaderRunsFreelyUnderPressureWithNoForegroundUser |
| test_the_loader_yields_to_a_viewer_on_a_slow_device | RETIRE | StoreIoPressure.TheLoaderYieldsToAForegroundUserOnASlowDevice |
| test_a_viewer_between_two_extents_is_still_a_viewer | RETIRE | StoreIoPressure.AForegroundUserBetweenTwoExtentsIsStillPresent |
| test_background_work_always_drains_on_a_pressured_device | RETIRE | StoreIoPressure.BackgroundWorkAlwaysDrainsOnAPressuredDevice |
| test_control_cannot_enter_the_data_arbiter_at_all | RETIRE | StoreIoPressure.ControlCannotEnterTheDataArbiterAtAll |
| test_with_no_device_admission_is_exactly_what_it_was | RETIRE | StoreIoPressure.WithNoDeviceAdmissionIsExactlyWhatItWas |

### test_ledger_gates.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_referenced_sets_are_sorted_unique_and_per_class | RETIRE | ContractGates.ReferencedSetsAreSortedUniqueAndPerClass |
| test_inventory_splits_tombstones_by_whether_data_still_refers_to_them | RETIRE | ContractGates.TheInventorySplitsRetirementsByWhetherDataStillRefersToThem |
| test_release_horizon_carries_its_head_and_clock | RETIRE | ContractGates.TheReleaseHorizonCarriesItsHeadAndClock |
| test_tombstone_gate_is_the_pass_condition_over_every_input | RETIRE | ContractGates.TheTombstoneGateIsThePassConditionOverEveryInput |
| test_control_gate_is_the_pass_condition_over_every_input | RETIRE | ContractGates.TheControlGateIsThePassConditionOverEveryInput |
| test_data_gate_is_the_pass_condition_and_reason_over_every_input | RETIRE | ContractGates.TheDataGateIsThePassConditionAndReasonOverEveryInput |
| test_each_shut_gate_names_the_first_condition_that_failed | RETIRE | ContractGates.EachShutGateNamesTheFirstConditionThatFailed |

### test_lifecycle_record.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_lifecycle_of_gbni_1 | REWRITE | Service on a real node config; pins the recorded lifecycle order, fixture to regenerate |
| test_lifecycle_of_fi_1 | REWRITE | Service on a real node config; pins the recorded lifecycle order, fixture to regenerate |

### test_maintenance_trace.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_manual_clock_moves_and_wakes_only_when_told | KEEP | Macha's maintenance clock, no store (see Uncertain) |
| test_system_clock_reads_and_waits_in_real_time | KEEP | Macha's maintenance clock, no store (see Uncertain) |
| test_normalise_names_ids_by_first_appearance | KEEP | the trace fixture's own id normaliser, no store |
| test_trace_quiet_node | REWRITE | Whole-node pass decisions on a Service against a trace fixture; ACTIVE.md keeps this file as Macha's gate (see Uncertain) |
| test_trace_tombstones_maturing | REWRITE | FS delete, tombstone grace and erasure as traced on a Service; Macha's gate per ACTIVE.md |
| test_trace_revived_tombstone | REWRITE | FS rewrite reviving a tombstone as traced on a Service; Macha's gate per ACTIVE.md |
| test_trace_backend_offline_and_back | REWRITE | Gates and held state through a backend loss on a Service against a trace fixture; Macha's gate per ACTIVE.md |
| test_trace_claimed_objects_lost | REWRITE | Claim walk and pull order on a Service against a trace fixture; Macha's gate per ACTIVE.md (see Uncertain) |
| test_trace_peer_unreachable_and_back | REWRITE | Destructive gates through a peer leaving and returning on two Services against a trace fixture; Macha's gate per ACTIVE.md |
| test_trace_incomplete_catalogue_and_release_horizon | REWRITE | Catalogue root lost shuts the gates and the release horizon, on a Service; Macha's gate per ACTIVE.md |

### test_media_playback.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_segment_store_serves_its_plan_as_a_closed_playlist | KEEP | media segment store over a temp dir, not the object store |
| test_segment_store_holds_requests_until_published_superseded_or_ended | KEEP | media segment store over a temp dir, not the object store |
| test_production_rate_excludes_time_parked_on_demand | KEEP | media segment store rate, no store |
| test_segment_hold_arbiter_admits_within_limits_and_refuses_beyond_them | KEEP | media hold limits, pure |
| test_vod_plans_follow_the_keyframe_index | KEEP | VOD planning, pure |
| test_media_vocabulary_names_containers_codecs_and_subtitles | KEEP | media vocabulary, pure |
| test_media_timestamp_repair | KEEP | timestamp repair, pure |
| test_older_video_profiles_are_stale_and_regenerate | KEEP | media profile staleness, pure |
| test_http_server_serves_streams_concurrently | KEEP | HTTP server with stub handler, no store |
| test_playback_performs_the_instruction_it_is_given | REWRITE | playback API behaviour; PlaybackNode on TestNode |
| test_playback_admission_bounds_slots_sessions_and_accounts | REWRITE | playback admission; PlaybackNode on TestNode |
| test_playback_probes_once_and_replays_idempotent_creates | REWRITE | playback probe and idempotent creates; PlaybackNode on TestNode |
| test_media_information_schedules_scans_and_publishes_profiles | REWRITE | media information scans and profile publication; PlaybackNode on TestNode |
| test_playback_holds_admitted_fragment_requests_and_refuses_the_rest_at_once | REWRITE | playback fragment holds; PlaybackNode on TestNode |
| test_playback_idle_clocks_release_what_is_not_used | REWRITE | playback idle clocks; PlaybackNode on TestNode |
| test_an_async_start_answers_at_once_and_reports_its_progress | REWRITE | playback async start API; PlaybackNode on TestNode |

### test_metadata_merge.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_a_file_one_head_never_saw_is_kept | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_a_file_one_head_removed_stays_removed | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_an_edit_survives_a_concurrent_removal | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_the_newer_of_two_values_wins_without_a_conflict | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_concurrent_edits_install_the_later_and_keep_both | KEEP | FS conflict records and supersession (H11 marks FS); pure merge, no store fixture. |
| test_the_same_content_written_twice_is_no_conflict | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_a_rename_meets_a_concurrent_edit_of_the_same_file | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_a_rename_onto_an_existing_file_replaces_it | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_deciding_for_the_other_file_is_kept | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_two_renames_of_one_file_keep_one_name | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_the_same_rename_on_both_heads_is_one_file_and_no_conflict | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_a_removed_directory_returns_for_a_file_added_under_it | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_a_directory_with_entries_wins_a_clash_with_a_file | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_entries_from_before_provenance_merge_by_the_legacy_clock | KEEP | legacy_clock semantics (H13 marks FS); pure merge, no store fixture. |
| test_a_returning_node_from_before_provenance_loses_nothing | KEEP | legacy_clock semantics (H13 marks FS); pure merge, no store fixture. |
| test_a_decision_is_kept_against_a_head_that_has_not_seen_it | KEEP | FS conflict decision (H11 marks FS); pure merge, no store fixture. |
| test_cluster_values_take_the_greater_and_both_heads_conflicts_stand | KEEP | FS merge semantics: pure merge_metadata_heads on snapshots, no store fixture. |
| test_three_heads_merge_to_the_same_namespace_in_any_order | KEEP | Asserts FS outcomes of the owner merge. H8 cites it, but Dagda still owes its own fold and determinism case on a neutral owner. |
| test_the_catalogue_root_follows_its_dot | KEEP | Catalogue root conflict in the FS merge; pure merge, no store fixture. |

### test_models.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_metadata_delta_state_model | KEEP | pure model of the FS snapshot and delta codec, the owner codec; no store (see Uncertain) |
| test_hydration_scheduler_state_model | KEEP | filesystem hydration scheduler, pure, no store |
| test_capacity_placement_invariants_model | GAP | StorePlacementVectors.DataOrderIsMachasOnEveryVector pins orders on fixed sets, not the R=1 monotonic rule when a node is added, nor distinct domains at every R on six nodes (see Uncertain) |

### test_namespace_entries.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_pages_visit_what_the_callback_walk_visits_on_every_shape | GAP-HEADS:H18,H21 | Budgeted resumable walk over the tree over keys; no tree in Dagda. Memory node store, no store fixture. |
| test_a_resumed_page_reads_only_its_own_path | GAP-HEADS:H18,H21 | A resumed page reads one root-to-leaf path; tree over keys not built. |
| test_a_cursor_between_paths_resumes_after_it | GAP-HEADS:H21 | Cursor between keys resumes after it, map and tree forms; paged view not built. |
| test_cancellation_stops_before_an_entry | GAP-HEADS:H21 | Cancelled budget stops the paged walk before an entry; paged view not built. |
| test_a_missing_node_or_store_throws | GAP-HEADS:H18,H21 | Missing tree node or store throws, never a short page; tree over keys not built. |
| test_the_metadata_view_pages_a_live_namespace | GAP-HEADS:H21 | MetadataView.entries on a TestService equals the callback walk; H21 cites it. Macha needs a rewrite on TestService once the view is Dagda. |
| test_a_node_that_is_not_a_tree_node_throws | GAP-HEADS:H18 | Bytes at a node address that are no tree node are refused; tree over keys not built. |
| test_a_manager_without_a_store_pages_a_map_backed_view | GAP-HEADS:H21 | MetadataManager with no node store pages from the map; stands on TestNode; manager not in Dagda yet. |

### test_namespace_migration.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_every_write_records_where_it_came_from | REWRITE | FS provenance dots through FileSystem on a Service, map and tree forms. |
| test_the_inventory_and_the_media_index_follow_the_tree | REWRITE | FS inventory, media index and FUSE listing on a Service; Macha-visible. |
| test_tombstones_live_in_batches_in_a_tree_backed_head | GAP-HEADS:H23 | Batch CONTROL object named by the head, grace drop, head stays small. Service fixture; FS delete is only the trigger. (see Uncertain) |
| test_operations_waiting_on_a_commit_share_the_next_one | REWRITE | FileSystem::apply_namespace_batch group commit on a Service; FS op fails alone with ENOENT. |
| test_a_migrated_node_serves_and_writes_its_library | REWRITE | macha-namespace-migrate re-root then FS reads and writes on a Service. (see Uncertain) |
| test_every_node_computes_the_same_record_from_the_same_head | REWRITE | plan_namespace_migration is Macha tool code over FS record; stands on Service and LocalStore. |
| test_a_migration_refuses_what_it_cannot_re_root_safely | REWRITE | Migration tool refusals (migrated, witnesses, tampered); Service, LocalStore and MetadataReplica fixtures. |
| test_the_pre_migration_state_is_kept_not_deleted | REWRITE | Migration tool quarantines ancestry files on a Service state directory. |
| test_reconciling_two_tree_backed_branches_keeps_the_namespace | GAP-HEADS:H9 | Materialise, merge, re-root and tree merge reach one root; tree-backed merge scaffolding not in Dagda. |
| test_a_lagging_node_adopts_the_leaders_record_only_if_it_agrees | KEEP | Migration adoption rule over FS entries and catalogue root; memory node stores, no store fixture. |
| test_a_namespace_change_is_visible_as_a_change | KEEP | namespace_differs and signature are owner FS code; memory node stores, no store fixture. |
| test_a_tree_backed_delta_reconstructs_its_record | KEEP | SM14 codec premise over owner delta; memory node store. Dagda H2 owes its own replay-reproduces-record case. |
| test_a_catalogue_change_on_a_tree_leaves_the_tree_alone | REWRITE | Catalogue upsert on a Service leaves the FS root alone and claims the catalogue; cross-component. |
| test_the_control_collector_keeps_an_unclaimed_namespace | REWRITE | Collector, catalogue moves and FS reads on two Services; library survives. LedgerCollector cases cover only the provider seam. |
| test_a_merge_claims_what_it_introduces | GAP-HEADS:H22,H8 | Merge commit is a pure clock join and claims tree nodes and conflict extents under a fresh dot; heads not built. |

### test_namespace_tree.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_the_tree_round_trips_every_namespace_it_is_given | GAP-HEADS:H18 | Build and read round trip including the empty map; tree over keys not built. |
| test_the_tree_carries_each_entrys_provenance | KEEP | FS leaf entry codec (H18 marks it FS); memory node store, no store fixture. |
| test_the_root_is_a_function_of_the_entry_set_and_nothing_else | GAP-HEADS:H18 | History independence; H18 cites it; not built. |
| test_changing_one_file_rewrites_a_path_to_the_root_and_not_the_library | GAP-HEADS:H18 | Write cost follows depth; H18 cites it; not built. |
| test_appending_an_extent_does_not_rewrite_the_extent_list | KEEP | FS extent spine (H18 marks it FS); memory node store. |
| test_a_lookup_reads_a_path_to_the_root_rather_than_the_namespace | GAP-HEADS:H18 | Point lookup including absent keys; not built. |
| test_a_leaf_stays_small_however_many_extents_a_file_has | KEEP | FS extent spine externalises long lists; memory node store. (see Uncertain) |
| test_a_corrupt_node_is_refused_rather_than_trusted | GAP-HEADS:H18 | Bit flips in nodes never crash or hang (asserts survival only); H18 cites it; not built. |
| test_a_stat_only_lookup_fetches_no_extent_nodes | KEEP | FS extent spine: stat reads no extent node; memory node store. |
| test_an_sm14_record_points_at_the_namespace_instead_of_carrying_it | KEEP | SM13 and SM14 owner codec bytes; memory node store. |
| test_the_record_stops_growing_with_the_library | KEEP | SM14 owner codec payload size; memory node store. |
| test_a_snapshot_never_carries_its_namespace_in_two_places | KEEP | Owner codec and detach or attach refusals; memory node store. |
| test_a_stat_against_a_decoded_record_never_materialises_the_namespace | KEEP | Decoded SM14 plus FS stat-only lookup; memory node store. |
| test_a_damaged_record_is_refused_rather_than_trusted | KEEP | decode_snapshot on damaged SM14 is owner codec; memory node store. |
| test_a_snapshot_handed_to_a_reader_names_its_namespace_once | KEEP | require_coherent_namespace on MetadataSnapshot, owner code; memory node store. |
| test_a_reader_sees_the_same_namespace_in_either_form | KEEP | Owner read primitives agree on map and tree forms; memory node store. |
| test_a_detached_namespace_without_a_store_refuses_rather_than_reporting_empty | KEEP | Owner read primitives refuse with no node store; no store fixture. |
| test_an_incremental_update_produces_the_tree_a_rebuild_would | GAP-HEADS:H18 | Update root equals full build; H18 cites it; not built. |
| test_a_write_rewrites_a_path_rather_than_the_library | GAP-HEADS:H18 | Update writes exactly tree-depth nodes; not built. |
| test_a_delta_applied_to_the_tree_lands_where_the_map_lands | GAP-HEADS:H14,H18 | Delta on the tree equals map result, bad append base refused; H14 cites it; not built. Append onto the spine half is FS. |
| test_history_replay_applies_a_delta_to_the_tree_not_the_map | GAP-HEADS:H19 | Replay rebuilds the tree locally and refuses without a store; H19 cites it; not built. |
| test_a_stat_only_read_costs_no_extents_in_either_form | KEEP | FS extent spine and owner primitives; memory node store. |
| test_a_prefix_scan_descends_rather_than_walking_the_library | GAP-HEADS:H18 | Prefix descent reads a fraction of the tree; not built. |
| test_a_listing_reads_the_directory_not_its_descendants | KEEP | Directory listing is FS semantics over path keys; memory node store. |
| test_a_scan_seeks_and_stops_where_it_is_told | GAP-HEADS:H18 | Seek, stop and backward-seek refusal over keys; not built. |
| test_an_update_reaches_the_root_a_full_build_reaches | GAP-HEADS:H18 | Update and diff match full build across limits and shapes; H18 cites it; not built. |
| test_an_update_reads_the_path_it_changes_not_the_tree | GAP-HEADS:H18 | Update and diff read costs follow the change; H18 cites it; not built. |
| test_every_tree_node_is_reachable_for_the_collector | GAP-HEADS:H18,H19 | Collected set is exactly what the build wrote; not built. |
| test_a_commit_claims_the_nodes_it_introduced_and_prunes_the_rest | GAP-HEADS:H19,H22 | Parallel walk claims only new nodes, unreadable node fails; H19 cites it; not built. |
| test_a_diff_is_the_paths_that_differ_and_reads_only_their_leaves | GAP-HEADS:H9,H18 | Tree diff over keys at leaf cost; Dagda supplies this per H9; not built. |
| test_distant_changes_read_no_leaf_between_them | GAP-HEADS:H18 | Update windows read no leaf between changes; not built. |
| test_a_tree_merge_is_the_merge_of_the_materialised_heads | GAP-HEADS:H9 | Tree merge equals materialised merge, delta replays byte for byte; H9 cites it; not built. FS outcomes also asserted. |
| test_baseline_reconcile_materialised_against_tree | GAP-HEADS:H9 | Benchmark of tree against materialised reconcile at library size; not built. |

### test_node_services.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_services_never_started_stop_nothing_and_answer_until_destroyed | REWRITE | Pins NodeServices lifecycle and ingest and torrent routes; stands on BareNode (ServicesBench) |
| test_a_reload_reaches_every_service_with_live_limits | REWRITE | Pins reload reaching scanner, hydration, staging, jobs, streaming; stands on BareNode (ServicesBench) |
| test_media_routes_answer_from_this_nodes_files_and_engine | REWRITE | Pins catalogue, playback and manage HTTP routes over FS files; stands on BareNode (ServicesBench) |
| test_a_node_without_streaming_says_it_cannot_profile | REWRITE | Pins the catalogue profile route's refusal over an FS file; stands on BareNode (ServicesBench) |
| test_observation_gauges_follow_activity_repair_and_the_mount | REWRITE | Pins Macha's gauge names over activity, repair diagnostics and the FUSE mount; stands on BareNode (ServicesBench) |
| test_a_commit_is_published_only_once_its_objects_are_claimed | GAP-HEADS:H22 | No commit claims barrier in Dagda: claim under the commit's dot, refuse DATA held by no node present and log why. Which extents count (a hole is not an object) stays Macha's in the rewrite. |

### test_object_ledger.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_a_sealed_journal_replays_what_was_appended_in_order | RETIRE | LedgerSealedJournal.ReplaysWhatWasAppendedInOrder |
| test_a_torn_journal_keeps_what_came_before_the_tear | RETIRE | LedgerSealedJournal.ATornJournalKeepsWhatCameBeforeTheTear |
| test_a_damaged_or_refused_frame_ends_the_replay_there | RETIRE | LedgerSealedJournal.ADamagedOrRefusedFrameEndsTheReplayThere |
| test_a_reset_journal_is_empty_and_keeps_appending | RETIRE | LedgerSealedJournal.AResetJournalIsEmptyAndKeepsAppending |
| test_the_trie_holds_exactly_what_was_written | RETIRE | LedgerObjectTrie.HoldsExactlyWhatWasWritten |
| test_the_trie_hash_depends_only_on_its_records | RETIRE | LedgerObjectTrie.TheHashDependsOnlyOnItsRecords |
| test_a_subtree_reads_the_same_in_every_trie_holding_it | RETIRE | LedgerObjectTrie.ASubtreeReadsTheSameInEveryTrieHoldingIt |
| test_a_diff_finds_exactly_what_differs | RETIRE | LedgerTrieDiff.FindsExactlyWhatDiffers |
| test_a_diff_costs_what_differs_not_the_size | RETIRE | LedgerTrieDiff.CostsWhatDiffersNotTheSize |
| test_records_not_held_are_what_no_held_trie_holds | RETIRE | LedgerTrieDiff.RecordsNotHeldAreWhatNoHeldTrieHolds |
| test_a_union_of_tries_reads_as_one_trie_of_their_records | RETIRE | LedgerTrieUnion.ReadsAsOneTrieOfTheirRecords |
| test_a_reopened_trie_is_the_one_that_was_closed | RETIRE | LedgerObjectTrie.AReopenedTrieIsTheOneThatWasClosed |
| test_a_crash_at_any_step_of_a_checkpoint_loses_nothing | RETIRE | LedgerObjectTrie.ACrashAtAnyStepOfACheckpointLosesNothing |
| test_a_trie_rewrites_its_node_file_and_stays_bounded | RETIRE | LedgerObjectTrie.RewritesItsNodeFileAndStaysBounded |
| test_a_trie_larger_than_its_cache_reads_what_it_needs | RETIRE | LedgerObjectTrie.ATrieLargerThanItsCacheReadsWhatItNeeds |
| test_a_trie_with_a_torn_journal_keeps_what_was_whole | RETIRE | LedgerObjectTrie.ATornJournalKeepsWhatWasWhole |
| test_a_format_1_trie_opens_and_is_rewritten | RETIRE | LedgerObjectTrie.AFormat1TrieOpensAndIsRewritten |
| test_a_snapshot_is_frozen_through_writes_and_rewrites | RETIRE | LedgerObjectTrie.ASnapshotIsFrozenThroughWritesAndRewrites |
| test_a_large_trie_opens_within_the_bound | RETIRE | LedgerObjectTrie.DISABLED_ALargeTrieOpensWithinTheBound |
| test_a_store_with_its_ledger_starts_knowing_what_it_holds | RETIRE | StoreLocalStoreLedger.AStoreWithItsLedgerStartsKnowingWhatItHolds |
| test_verification_corrects_the_ledger_from_the_disk | RETIRE | StoreLocalStoreLedger.VerificationCorrectsTheLedgerFromTheDisk |
| test_the_ledger_lists_what_the_packs_hold | RETIRE | StoreLocalStoreLedger.TheLedgerListsWhatThePacksHold |
| test_a_flush_does_not_wait_for_the_seed | RETIRE | LedgerHeldLedger.AFlushDoesNotWaitForTheSeed |
| test_verification_resumes_after_a_restart | RETIRE | StoreLocalStoreLedger.VerificationResumesAfterARestart |
| test_reference_counts_follow_and_survive_a_restart | RETIRE | LedgerReferenceCounts.FollowAndSurviveARestart |

### test_observation.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_observation_buckets_tile_the_value_range | KEEP | Macha diagnostics, no store. |
| test_observation_bucket_contains_every_small_value | KEEP | Macha diagnostics, no store. |
| test_observation_quantiles | KEEP | Macha diagnostics, no store. |
| test_observation_snapshot_windows | KEEP | Macha diagnostics, no store. |
| test_observation_histogram_records_from_many_threads | KEEP | Macha diagnostics, no store. |
| test_observation_registry_is_bounded | KEEP | Macha diagnostics, no store. |
| test_observation_events_are_bounded_oldest_first | KEEP | Macha diagnostics, no store. |
| test_observation_registry_windows | KEEP | Macha diagnostics, no store. |
| test_observation_process_registry_and_durations | KEEP | Macha diagnostics, no store. |
| test_observation_route_labels | KEEP | Macha diagnostics, no store. |
| test_observation_window_rendering | KEEP | Macha diagnostics, no store. |
| test_observation_event_rendering | KEEP | Macha diagnostics, no store. |
| test_observation_log_rotates_at_its_bound | KEEP | Macha diagnostics, no store. |
| test_observation_log_survives_an_unwritable_path | KEEP | Macha diagnostics, no store. |
| test_observation_recorder_writes_windows_and_events | KEEP | Macha diagnostics, no store. |
| test_observation_recorder_thread_and_final_window | KEEP | Macha diagnostics, no store. |
| test_a_service_writes_its_lifecycle_to_the_observation_file | REWRITE | Macha diagnostics file lifecycle events; stands on TestService. |

### test_predicates.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_each_predicate_is_its_row_over_every_combination_of_facts | GAP | Dagda has no predicates: no satisfies or ObjectFacts in Dagda source or tests. Nearest: none (see Uncertain) |
| test_each_query_pages_to_the_predicate_over_the_whole_set | GAP | Dagda has no predicates: no query() paging over inventory or claims, no Placement. Nearest: none (see Uncertain) |
| test_queries_without_horizons_examine_nothing | GAP | Dagda has no predicates: no query() to examine nothing without horizons. Nearest: none (see Uncertain) |
| test_garbage_is_not_queryable_at_stage_0 | GAP | Dagda has no predicates: no query() refusing garbage. Nearest: none (see Uncertain) |
| test_a_cancelled_budget_stops_a_query_before_it_examines | GAP | Dagda has no predicates: no query(). LedgerClaimWalk.TheRetentionLedgerStopsForCancellationFirst covers claimed(), not query() (see Uncertain) |

### test_presence.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_presence_index_matches_its_model_over_every_short_history | RETIRE | StorePresenceIndex.MatchesItsModelOverEveryShortHistory |
| test_presence_index_warmup_race_cases_by_name | RETIRE | StorePresenceIndex.WarmupRaceCasesByName |
| test_has_answers_a_put_in_progress_without_waiting_for_it | RETIRE | StoreLocalStorePresence.HasAnswersAPutInProgressWithoutWaitingForIt |
| test_a_put_that_fails_midway_is_never_present | RETIRE | StoreLocalStorePresence.APutThatFailsMidwayIsNeverPresent |
| test_has_does_not_consult_the_device_after_warm_up | RETIRE | StoreLocalStorePresence.HasDoesNotConsultTheDeviceAfterWarmUp |
| test_presence_is_exact_across_a_restart | RETIRE | StoreLocalStorePresence.PresenceIsExactAcrossARestart |
| test_a_backend_offline_and_readopted_leaves_presence_exact | RETIRE | StoreStoragePool.ABackendOfflineAndReadoptedLeavesPresenceExact |
| test_the_held_identity_names_exactly_what_the_pool_holds | RETIRE | StoreStoragePool.TheHeldIdentityNamesExactlyWhatThePoolHolds |
| test_the_held_identity_is_the_same_on_any_number_of_disks | RETIRE | StoreStoragePool.TheHeldIdentityIsTheSameOnAnyNumberOfDisks |

### test_root_parts.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_control_fetch_stores_only_the_object_it_asked_for | GAP | No Dagda case drives ControlObjectFetch with wrong answers: other reply type, other id, bad hash, truncated; nor the speculative frame and peers_asked log |
| test_control_fetch_passes_over_a_failing_peer | GAP | DistributedStore.ControlObjectsRecoverOntoAReplicaFromAnother covers a successful pull, not passing a dead peer, the observer, or asking nobody on a second pull |
| test_control_fetch_fails_when_the_store_cannot_take_the_object | GAP | No Dagda case: a good answer the control store refuses is a failed pull, observed once, not valid locally, and logged |
| test_node_resources_yield_the_loader_to_a_recent_viewer_under_pressure | GAP | StoreIoPressure.TheLoaderYieldsToAForegroundUserOnASlowDevice uses an injected signal; dagda::Node's foreground_quiet boundary and admit-nothing after stop are unproved (see Uncertain) |
| test_recovery_progress_keeps_the_first_failure_and_wakes_its_waiters | KEEP | RecoveryProgress is Dannan's mechanism (requirement S31), pure; no store |

### test_rpc_cluster.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_peer_sockets_use_delay_based_congestion_control | KEEP | transport, no store; raw sockets |
| test_rpc_reassembly_has_count_byte_and_message_bounds | KEEP | transport, no store; MessageAssembler bounds |
| test_rpc_reassembly_is_process_memory_charged_until_consumed | KEEP | transport, no store; assembler memory charge |
| test_async_rpc_move_ownership | KEEP | transport, no store; AsyncRpc ownership |
| test_best_effort_telemetry_notifications_reach_both_route_directions | KEEP | transport, no store; TestCluster here supplies only cluster keys |
| test_rpc_v15_frame_priority_and_variable_length | KEEP | transport, no store; TestCluster here supplies only cluster keys |
| test_store_put_is_loader_activity_not_viewer_activity | RETIRE | DistributedStore.APutIsLoaderActivityNotViewerActivity |
| test_store_put_publishes_at_the_floor_and_copies_promptly | RETIRE | DistributedStore.APutPublishesAtTheFloorAndCopiesPromptly |
| test_store_put_replaces_a_replica_that_cannot_be_launched | RETIRE | DistributedStore.APutReplacesAReplicaThatFailsAtOnce (see Uncertain) |
| test_store_claims_ask_nothing_more_about_objects_nobody_holds | RETIRE | DistributedStore.ClaimsAskNothingMoreAboutObjectsNobodyHolds |
| test_store_put_to_a_silent_replica_returns_on_the_local_copy | RETIRE | DistributedStore.APutToASilentReplicaReturnsOnTheLocalCopy |
| test_store_concurrent_readers_share_one_fetch_and_buffer | RETIRE | DistributedStore.ConcurrentReadersShareOneFetchAndBuffer |
| test_repair_push_probes_by_window_and_sends_together | RETIRE | StoreRepair.APushSendsItsBatchTogetherAndAsksNothing (see Uncertain) |
| test_a_long_push_pass_does_not_hold_back_the_pull | RETIRE | StoreRepair.ALongPushPassDoesNotHoldBackThePull |
| test_repair_push_sends_nothing_to_a_peer_that_cannot_say | RETIRE | StoreRepair.APushSendsNothingToAPeerTheSurveyCouldNotSettle (see Uncertain) |
| test_repair_pushes_past_a_peer_with_no_room | RETIRE | StoreRepair.APushPassesAPeerWithNoRoom |
| test_repair_pushes_from_what_peers_hold_without_asking | RETIRE | StoreRepair.APushWorksFromWhatPeersHoldWithoutAsking (see Uncertain) |
| test_repair_pushes_only_what_the_list_names | RETIRE | StoreRepair.APushSendsOnlyWhatTheListNames |
| test_repair_probes_without_credit_and_transfers_only_with_it | RETIRE | StoreRepair.ATransferWaitsForItsCredit (see Uncertain) |
| test_repair_pass_keeps_its_place_across_live_sets_and_restarts | RETIRE | StoreRepair.APushKeepsItsPlaceAcrossANewList (see Uncertain) |
| test_repair_bandwidth_estimate_ignores_small_transfers | RETIRE | StoreRepair.TheLinkEstimateIgnoresSmallTransfers |
| test_repair_decides_already_held_without_reading_the_extent | RETIRE | StoreRepair.APullDecidesHeldByIndex |
| test_repair_keeps_a_pull_that_was_in_flight_when_its_turn_ended | RETIRE | StoreRepair.APullInFlightWhenItsTurnEndsIsKept |
| test_store_heals_a_bad_local_copy_from_a_peer | RETIRE | DistributedStore.HealsABadLocalCopyFromAPeer |
| test_store_runtime_cache_keeps_a_playback_fetch | RETIRE | DistributedStore.ARuntimeCacheKeepsAPlaybackFetch |
| test_store_control_objects_take_no_data_credit_and_commit_alone | REWRITE | Pins ControlNamespaceNodeStore::for_commit, Macha's. Credit half proved by DistributedStore.ALocalControlObjectIsFoundWithoutAskingAnyone (see Uncertain) |
| test_startup_gate_fires_on_silence_not_on_slow_progress | KEEP | StartupStallGate pure logic, Macha's startup; no store |
| test_torrent_listing_is_served_from_memory_never_a_survey | REWRITE | TorrentCoordinator listing on TestNode: Macha-visible, no survey on read |
| test_repair_is_paced_not_stopped_while_higher_classes_stay_busy | RETIRE | StoreRepair.RepairIsPacedNotStoppedWhileHigherClassesStayBusy |
| test_rpc_v15_persistence_and_multiplexing | KEEP | transport, no store; TestCluster here supplies only cluster keys |
| test_rpc_concurrent_cold_data_calls_share_one_dial | KEEP | transport, no store; TestCluster here supplies only cluster keys |
| test_rpc_full_extent_reply_uses_owned_transport_handoff | KEEP | transport, no store; TestCluster here supplies only cluster keys |
| test_rpc_v15_bidirectional_and_deduplication | KEEP | transport, no store; TestCluster here supplies only cluster keys; local TestNode is RPC only |
| test_mutual_bootstrap_prunes_cross_dial | REWRITE | Transport (cross-dial pruning) on BareNode, a store-bearing fixture |
| test_rpc_v7_handshake_is_rejected | KEEP | transport, no store; TestCluster here supplies only cluster keys |
| test_rpc_slow_control_does_not_abort_data | KEEP | transport, no store; TestCluster here supplies only cluster keys |
| test_rpc_request_payload_is_charged_until_handler_completion | KEEP | transport, no store; TestCluster here supplies only cluster keys |
| test_rpc_held_executors_do_not_delay_other_classes | KEEP | transport, no store; TestCluster here supplies only cluster keys |
| test_rpc_call_fails_after_no_progress_deadline | KEEP | transport, no store: RpcClient no-progress deadline against a stub RpcServer; TestCluster supplies keys only |
| test_rpc_metadata_mutations_use_bounded_isolated_executor | GAP-HEADS:H26 | Bounded metadata executor and per-class byte limits, stub handler; Dagda registers no metadata message types yet (see Uncertain) |
| test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers | GAP-HEADS:H26 | H26's named proof: per-peer order of commit RPCs, parallel across peers, stub handler; heads types not yet registered ordered (see Uncertain) |
| test_rpc_a_dial_in_flight_is_waited_for_no_longer_than_a_dial | KEEP | transport, no store: raw listener and RpcClient only |
| test_rpc_job_traffic_travels_as_its_own_class | KEEP | transport, no store: torrent and ingest job message classes against a stub RpcServer |
| test_rpc_metadata_control_mutations_go_first | GAP-HEADS:H26 | H26's named proof: a commit's accept overtakes queued history imports, imports leave a control slot; stub handler (see Uncertain) |
| test_rpc_metadata_executor_cancellation_disconnect_and_shutdown | GAP-HEADS:H26 | Metadata executor boundaries: cancel a queued job, abort keeps a running one, stop drops the queue; stub handler (see Uncertain) |
| test_service_shutdown_cancels_pending_outbound_rpc_before_join | REWRITE | Transport on a store-bearing Service: Service stop closes outbound routes first and a later call is refused |
| test_data_credit_wait_gives_up_when_nothing_is_moving | RETIRE | StoreDataWork.ACreditWaitGivesUpWhenNothingIsMoving |
| test_data_credit_wait_survives_genuine_contention | RETIRE | StoreDataWork.ACreditWaitSurvivesGenuineContention |
| test_storage_data_credit_reserves_viewer_headroom_and_control | RETIRE | StoreRepair.ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit |
| test_joiner_cannot_form_genesis | GAP-HEADS:H27 | A pristine bootstrap joiner advertises no accepted head and refuses a read while it waits for its peer; no genesis yet |
| test_bootstrap_joiner_requires_complete_checkpoint_survey | GAP-HEADS:H7,H27 | A failed head survey stops a replication-1 joiner forming an empty generation-2 namespace; no head survey in Dagda yet |
| test_nodes_seeking_different_copy_counts_share_one_namespace | GAP-HEADS:H28 | H28's named proof: nodes advertising different write floors share one namespace; no floor advert or head space yet |
| test_two_node_mutual_bootstrap_metadata_write_floor | GAP-HEADS:H8,H27 | Competing genesis proposals converge on one history, then a write reaches both nodes; no genesis or merge yet |
| test_service_metadata_repair_coalesces_real_generation_burst | GAP-HEADS:H7,H24 | A burst of 33 commits announced to a held convergence run coalesces into one further run; heads not built |
| test_failed_convergence_retries_once_per_burst_of_events | GAP-HEADS:H7 | A failed convergence run retries once per burst, after the settle and not the backoff; heads not built |
| test_an_ingest_interrupted_by_its_node_stopping_resumes | REWRITE | Ingest job interrupted, not failed, across a Service restart; read back through the FS |
| test_an_ingest_job_read_elsewhere_carries_its_owners_catalogue_items | REWRITE | Acquisition API job detail from a non-owner node carries catalogue items, null when the owner is away |
| test_service_same_generation_sibling_notice_triggers_reconciliation | GAP-HEADS:H2,H5,H8,H24 | Same-generation notice wakes reconciliation, a duplicate certificate is silent, the next write is a delta over the merge |
| test_a_head_from_an_unknown_history_is_merged | GAP-HEADS:H7,H8 | H7's named proof: a foreign head with no common ancestor merges and neither side's entries are lost |
| test_a_head_that_cannot_be_merged_is_set_aside | GAP-HEADS:H10 | H10's named proof: an unmergeable head is set aside, reads, writes and release follow the own head, both heads held |
| test_concurrent_reads_during_divergence_produce_one_reconciliation | GAP-HEADS:H8 | H8's named proof: eight readers of one divergence mint exactly one merge commit |
| test_service_startup_gate_spares_progress_and_ends_a_stall | REWRITE | Service startup gate wiring: progress holds it, silence fires the stall handler with its readiness diagnostic (see Uncertain) |
| test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs | GAP-HEADS:H7,H24 | A returning replica takes a 64-commit burst in at most 4 runs, history transfer in flight bounded at 8; heads not built |
| test_an_ingest_on_a_node_with_no_namespace_yet_resumes_when_it_has_one | REWRITE | Ingest job blocks with metadata_unavailable, is retried and completes once a Service namespace forms |
| test_a_file_touch_commits_while_a_holder_is_away | GAP-HEADS:H22 | H22's named proof: a metadata-only commit makes a fresh claim on holders present with one away; no commit claiming yet |
| test_commit_replicas_ordered_local_then_nearest | GAP-HEADS:H4 | H4's named proof: pure order_commit_replicas, local then nearest then unmeasured; no such function or case in Dagda |
| test_a_delete_is_reclaimed_while_a_node_is_away | REWRITE | FS unlink through the head to claim release and reclaim, absent node on return. StoreRemove proves store half, not this path |
| test_retained_missing_copy_repairs_without_namespace_reachability | RETIRE | StoreRepair.ARetainedMissingCopyIsRestoredWithoutReachability |
| test_held_retention_claims_cost_repair_no_credit | RETIRE | StoreRepair.HeldClaimsCostRepairNoCredit |
| test_disjoint_metadata_pairs_branch_and_reconcile | GAP-HEADS:H2,H8 | H8's named proof: two partitioned pairs commit, then a merge descends from both with one delta history entry |
| test_replication_policy_change_on_restart | REWRITE | Pins policy committed in the head snapshot and FS read-back; NodeReconfigure.AReplicationPolicyChangesOnRestartAndRepairConverges proves repair only (see Uncertain) |
| test_replacement_node_recovers_namespace_and_replication | REWRITE | Wiped node with a new id recovers the FS namespace from the survivor, repair refills it, FS read and write follow |
| test_a_durable_trio_founds_commits_and_reads_through_every_node | REWRITE | File written through one Service node reads whole and at an offset through others; FS visibility over commits |
| test_metadata_commits_with_whichever_nodes_remain | GAP-HEADS:H4,H15 | Commits continue and reach the remaining node as members are lost, down to a lone survivor; no head commits yet |
| test_unlink_retires_an_object_from_the_maintenance_inventory | REWRITE | Pins FS unlink retirement and the FS's cached maintenance inventory, the provider side, on TestService |
| test_rename_of_a_file_onto_a_directory_is_eisdir | REWRITE | FS rename semantics, EISDIR, on TestService |
| test_a_joining_node_pulls_its_objects_through_maintenance | REWRITE | Service over a file written through the FS: Macha reachability reaches a joiner. Store half only: StoreRepair.AWriteAtTheFloorConvergesToTwoCopiesThroughRepair, StoreRepair.RepairConvergesAfterAPartitionHeals (see Uncertain) |
| test_a_ping_answered_while_the_health_pass_ran_late_keeps_its_route | KEEP | transport, no store: RpcServer and RpcClient over FaultyLinks; TestCluster supplies keys only |
| test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes | GAP-HEADS:H7,H14 | Head repair fan-out to a silent peer does not block a local commit; no heads built in Dagda |
| test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim | GAP-HEADS:H4,H22 | A commit's CONTROL objects are claimed here before answering and the peer's claim follows later; needs commits. DistributedStore.APutToASilentReplicaReturnsOnTheLocalCopy is a put, not a commit |
| test_a_catalogue_conflict_decided_for_the_root_in_place_is_removed | REWRITE | Catalogue conflict record and catalogue repair pass; stands on TestService |
| test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again | GAP-HEADS:H15 | Replicator waits once for a stalled peer and does not re-ask until later, repair then catches it up; H15 covers the caller not waiting, the not-asked-again rule is not named in the table |
| test_every_nodes_unmatched_files_are_listed_and_handled_anywhere | REWRITE | Unmatched files through the manage API and catalogue hints across nodes; stands on Service |
| test_ingest_job_pinned_to_a_node_runs_there | REWRITE | Ingest job pinning through the acquisition API; stands on Service |
| test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node | REWRITE | Ingest and torrent jobs through the acquisition API and torrent plugin across nodes; stands on Service |
| test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges | GAP-HEADS:H16,H6,H7 | History truncated by one node alone, returning peer takes the newer head with no merge; replica history and head set not built |
| test_history_is_not_truncated_while_two_heads_are_held | GAP-HEADS:H16,H8 | Never truncate with two heads, a read merges them, then truncation proceeds; stands on BareNode and MetadataManager |
| test_unreconstructable_accepted_head_is_repaired_live_from_a_peer | GAP-HEADS:H7,H15 | A head with a certificate but no history is flagged, excluded and reanchored from a peer's full history record; also the history record wire call |
| test_a_hung_health_probe_is_retried_inside_the_liveness_budget | REWRITE | Transport: health ping retries inside dead_after, through FaultyLinks; runs on Service nodes |
| test_inbound_incapable_node_is_reached_only_over_its_own_sessions | KEEP | transport, no store: RpcClient and RpcServer FlagNodes; object message types only select the lane |
| test_a_call_whose_dial_is_retired_by_a_simultaneous_connect_waits_for_the_peer | KEEP | transport, no store: RpcClient and RpcServer over FaultyLinks |
| test_inbound_auto_resolves_from_dial_back_and_survives_restart | REWRITE | Transport and config: inbound_capable auto resolution, its persistence, gossiped flags and founder refusal; runs on BareNode |
| test_large_replies_never_crowd_out_a_small_message | KEEP | transport, no store: pure outbound_refusal limits |
| test_peer_sockets_disable_nagle_and_keep_alive | KEEP | transport, no store: socket options on dialled sockets |
| test_peers_with_different_frame_limits_agree_on_the_smaller | KEEP | transport, no store: handshake over RelayedChannels |
| test_a_negotiated_frame_above_the_clients_limit_is_refused | KEEP | transport, no store: client handshake against a crafted ack |
| test_both_ends_of_a_session_share_its_id_and_no_other_session_does | KEEP | transport, no store: session id over RelayedChannels |
| test_a_tampered_fragment_ends_the_session | KEEP | transport, no store: fragment authentication over RelayedChannels |
| test_a_replayed_fragment_ends_the_session | KEEP | transport, no store: fragment counters over RelayedChannels |
| test_a_peer_with_another_cluster_key_fails_the_handshake | KEEP | transport, no store: handshake keys over RelayedChannels |
| test_a_hello_from_a_newer_protocol_is_refused | KEEP | transport, no store: raw hello against RpcServer |
| test_a_client_refuses_an_ack_of_another_version | KEEP | transport, no store: client handshake against a crafted ack |
| test_traffic_counts_both_directions_by_frame_class_in_one_instance | KEEP | transport, no store: traffic counters on RpcClient and RpcServer; object message type is only a label |
| test_a_promoted_call_overtakes_on_both_sides | KEEP | transport, no store: call promotion against a stub RpcServer handler |
| test_broadcast_notify_never_dials_and_never_waits_on_a_silent_peer | KEEP | transport, no store: broadcast_notify against RpcServer and SilentPeer |
| test_a_ping_is_admitted_behind_a_full_queue_of_large_messages | KEEP | transport, no store: outbound reserve against SilentPeer |
| test_a_connection_holds_no_more_than_its_pending_reply_budget | KEEP | transport, no store: pending reply budget against a stub RpcServer handler |
| test_no_progress_deadline_follows_the_lane_configured | REWRITE | Transport and config: per-lane no-progress deadlines; the calling node is a BareNode |
| test_unauthenticated_connections_are_bounded_on_every_platform | KEEP | transport, no store: unauthenticated connection bound on RpcServer |
| test_round_trip_is_smoothed_from_pings_not_from_slow_replies | KEEP | transport, no store: peer latency on RpcClient |
| test_dial_backoff_is_per_peer | KEEP | transport, no store: dial backoff on RpcClient over FaultyLinks |
| test_dial_back_probes_are_rate_limited_per_asking_peer | REWRITE | Transport: dial-back probe rate limit served by the node; the hub is a BareNode |
| test_reprobe_cadence_defaults_follow_the_resolution | KEEP | config defaults, no store |
| test_a_reverse_dial_that_never_comes_fails_within_its_bound | KEEP | transport, no store: reverse dial bound on RpcClient and RpcServer |

### test_runtime_dependencies.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_glibc_allocator_arena_limit_is_hard | KEEP | process allocator, no store |
| test_ffmpeg_log_bridge | KEEP | FFmpeg logging, no store |
| test_advertised_api_endpoint_is_validated | KEEP | YAML config, no store |
| test_yaml_edge_node_needs_no_data_backends | KEEP | YAML config, no store |
| test_yaml_refuses_a_repair_weight_of_zero | KEEP | YAML config, no store |
| test_books_enabled_is_the_one_switch_for_books | KEEP | YAML config, no store |
| test_yaml_refuses_unknown_keys | KEEP | YAML config, no store |
| test_example_configuration_loads | KEEP | YAML config, no store |
| test_yaml_config | KEEP | YAML config, no store |
| test_password_checks_may_never_fill_the_control_lane | KEEP | config parsing, no store |
| test_memory_bytes_is_one_budget | KEEP | config parsing, no store |
| test_embedded_music_metadata_and_artwork | REWRITE | Service; pins music probe, catalogue artwork and artwork in local_state data |
| test_probe_failure_reports_why_it_failed | KEEP | media engine with fake inputs, no store |
| test_aac_encodes_a_standard_channel_configuration | KEEP | AAC encoder, no store |

### test_service_root.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_a_service_never_started_stops_in_order_once | REWRITE | TestCluster and Service; pins the stop order of the composition root |
| test_a_failed_startup_is_what_every_caller_is_told | REWRITE | Service with store planes; pins health, stall diagnostic and HTTP on a failed startup |
| test_a_failed_startup_with_no_reason_still_says_it_failed | REWRITE | Service with store planes; pins the failure text callers are told |
| test_a_stall_before_the_control_plane_names_it_starting | REWRITE | Service; pins the stall diagnostic naming control_plane and local_state |
| test_a_stalled_startup_with_nobody_to_tell_ends_the_process | REWRITE | Service in a forked child; pins exit status 1 on a stall at data-storage |
| test_a_stop_once_local_state_has_recovered_builds_no_services | REWRITE | Service; pins lifecycle steps after a stop during startup |
| test_a_stop_once_services_are_built_starts_none_of_them | REWRITE | Service; pins lifecycle steps after a stop during startup |
| test_a_request_is_admitted_by_session_capability_and_role | REWRITE | TestService; pins HTTP admission, sessions and roles |
| test_each_route_reaches_the_api_that_owns_it | REWRITE | TestService; pins the HTTP route table |
| test_an_api_request_is_viewer_work_and_a_viewer_present | REWRITE | Service; pins route classes marking activity |
| test_standing_conflicts_are_listed_and_resolved_over_http | REWRITE | Service and MetadataManager; pins FS conflict listing and resolution over HTTP |
| test_conflict_routes_say_when_metadata_is_unavailable | REWRITE | TestService; pins the metadata_unavailable HTTP answer |

### test_session.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_session_create_and_validate | REWRITE | Session API and account roles; stands on BareNode. |
| test_session_rejects_non_anonymous_credentials | REWRITE | Session API refusal; stands on BareNode. |
| test_session_revoke_invalidates_immediately | REWRITE | Session API revoke; stands on BareNode. |
| test_session_expiry | KEEP | SessionManager alone, no store. |
| test_session_create_enforces_max_sessions | KEEP | SessionManager alone, no store. |
| test_session_lww_tie_prefers_revoked | KEEP | SessionManager merge rule, no store. |
| test_session_survives_persist_reload | KEEP | SessionManager own file, no store. |
| test_session_cluster_propagation | REWRITE | Session gossip over transport; two BareNodes. |
| test_session_revoke_propagates_cluster_wide | REWRITE | Revocation gossip over transport; two BareNodes. |
| test_http_server_gate_uses_session_authenticator | KEEP | HttpServer with a lambda authenticator, no store. |

### test_storage_metadata.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_retention_claims_are_causal_durable_and_observed_remove | RETIRE | LedgerRetention.ClaimsAreCausalDurableAndObservedRemove |
| test_retention_prune_cursor_cannot_starve_later_tombstones | RETIRE | LedgerRetention.ThePruneCursorCannotStarveLaterTombstones |
| test_retention_prune_keeps_a_row_claimed_while_it_checks_presence | RETIRE | LedgerRetention.PruneKeepsARowClaimedWhileItChecksPresence |
| test_retention_claims_checkpoint_into_the_ledger_and_restart | RETIRE | LedgerRetention.ClaimsCheckpointIntoTheLedgerAndRestart |
| test_retention_checkpoints_after_enough_changes | RETIRE | LedgerRetention.CheckpointsAfterEnoughChanges |
| test_retention_start_checkpoints_a_long_journal | RETIRE | LedgerRetention.AStartCheckpointsALongJournal |
| test_retention_prune_survives_a_restart | RETIRE | LedgerRetention.APruneSurvivesARestart |
| test_retention_shard_checkpoint_moves_into_the_ledger | RETIRE | LedgerRetention.AnUnmovedLegacyCheckpointIsRefusedAndAMovedOneRemoved, MigrateState.RetentionTheStoreWouldRefuseIsRefusedBeforeAnyWrite (see Uncertain) |
| test_retention_claim_is_physical_gc_barrier | RETIRE | LedgerCollector.ARetentionClaimIsAPhysicalGcBarrier |
| test_metadata_record_payload_copy_is_shared | GAP-HEADS:H17 | Commit records sharing one immutable payload backing across copies; nothing in Dagda yet |
| test_metadata_seed_sibling_replays_after_restart | GAP-HEADS:H6,H25 | Same-generation sibling seed: higher hash wins, and journal replay reproduces it |
| test_metadata_hash_streaming_matches_canonical_encoding | GAP-HEADS:H1 | Streamed commit hash equal to the canonical encoding; Dagda has no commit hash yet |
| test_metadata_delta_in_place_preserves_namespace_storage | KEEP | Owner codec: in-place delta over FS entries, no store under it; Dagda's generic H17 needs its own case |
| test_metadata_branch_merge_history_roundtrip | GAP-HEADS:H3,H7,H8 | Merge commit, secondary-parent acceptance, history import and reopen on the replica; its FS merge half pins conflicts and catalogue (see Uncertain) |
| test_metadata_commit_store_acceptance_heads_roundtrip | GAP-HEADS:H3,H4,H5,H6,H16 | Store and accept, certificates, persistence figures, heads set, merge to one head, reopen, truncation to the head |
| test_accept_commit_refreshes_checkpoint_on_both_repair_and_ordinary_paths | GAP-HEADS:H5,H6 | accept_commit moves the committed head and its checkpoint durably on both paths |
| test_merge_delta_primary_may_precede_merge_generation | GAP-HEADS:H2,H3 | A merge delta whose primary parent is more than one generation below it |
| test_merge_delta_with_generation_gap_reconstructs_after_cache_eviction_and_reopen | GAP-HEADS:H2,H3,H17 | Generation-gap merge delta reconstructs from history once evicted, live and after restart |
| test_unreconstructable_accepted_head_is_kept_at_startup_and_reanchored_live | GAP-HEADS:H7 | Head that cannot replay is kept and flagged, re-anchored from a peer's full record, forgery refused |
| test_full_fallback_boundary_heals_when_parent_arrives | GAP-HEADS:H7,H16 | Unknown-previous boundary heals when the parent is imported; common ancestor found, also after reopen |
| test_compacted_direct_predecessor_cannot_resurface_as_head | GAP-HEADS:H6,H16,H30 | A truncated predecessor offered again stays an ancestor and never becomes a head |
| test_unreconstructable_accepted_head_is_rate_limited_not_hammered | GAP-HEADS:H7 | Cooldown after a failed reconstruction: first failure surfaces, later calls neither retry nor throw |
| test_history_transfer_follows_only_materialization_dependencies | GAP-HEADS:H2,H7 | History transfer dependency rule: a full body needs nothing, a delta needs only its primary parent |
| test_pristine_joiner_adopts_compacted_cluster_head | GAP-HEADS:H6,H7,H16,H27 | Pristine joiner adopts a truncated head; genesis is an ancestor; the opposite genesis race leaves the head |
| test_a_head_with_no_clock_is_nobodys_ancestor | GAP-HEADS:H6 | A clockless head stays a head beside another |
| test_a_head_from_before_truncation_is_recognised_by_its_clock | GAP-HEADS:H6,H16 | Clock domination drops a pre-truncation head; uncovered clock stays; no truncation with two heads |
| test_returning_replica_adopts_a_head_whose_ancestry_a_peer_truncated | GAP-HEADS:H6,H16,H30 | Returning replica adopts a truncated peer's head by clock; a foreign clock is a second head |
| test_a_checkpoint_write_holds_no_reader_of_the_replica | GAP-HEADS:H10,H21 | Reader answers while a set-aside's checkpoint write is held at fsync; set-aside not durable across restart |
| test_a_journal_compaction_holds_no_reader_of_the_replica | GAP-HEADS:H16,H21 | Reader answers while journal compaction's checkpoint write is held at fsync |
| test_a_head_set_aside_for_the_horizon_is_dropped | GAP-HEADS:H10 | Set-aside rules, persisted first time, expiry at the horizon, durable drop |
| test_the_set_aside_time_of_a_head_no_longer_held_is_forgotten | GAP-HEADS:H10 | set-aside.meta forgets a head no longer held; expiry drops nothing |
| test_metadata_recovery_cache_seed_is_not_accepted_authority | GAP-HEADS:H25 | Cache seed may materialise but gains no accepted-head authority; recovery marker holds across restart |
| test_metadata_journal_mid_frame_corruption_truncates_not_reseeds | GAP-HEADS | Replica journal damage: prefix kept, tail quarantined, usable. LedgerSealedJournal.ADamagedOrRefusedFrameEndsTheReplayThere covers truncation, not quarantine or the replica |
| test_metadata_conflict_alternatives_are_gc_roots | KEEP | FS conflict records as extent and catalogue roots, pure over a snapshot; no store under it (H11 is FS today) |
| test_metadata_legacy_prepare_journal_replays_deterministically | GAP-HEADS:H5 | Replica cas and PREPARE journal replay, lower hash wins, committed unmoved; no heads built in Dagda (see Uncertain) |
| test_protocol20_checkpoint_without_acceptance_never_self_promotes | GAP-HEADS:H5,H6 | heads.meta lost: checkpoint is recovery material, no accepted heads, recovery_required set; not built |
| test_an_author_sequence_never_regresses | GAP-HEADS:H12 | reserve_mutation_dot never regresses and continues across restart; author.meta not built |
| test_a_broken_author_chain_starts_a_new_author | GAP-HEADS:H12 | A head lacking an accepted dot takes a new author id, kept across restart; not built |
| test_a_lost_author_record_starts_a_new_author | GAP-HEADS:H12 | Lost author record with a head showing the node starts a new author; clock_covers; not built |
| test_protocol20_delta_preserves_governance_snapshot_encoding | REWRITE | Pins DLT5 and exact snapshot bytes of Macha's codec through store_commit and reopen; MetadataReplica fixture becomes Dagda heads with owner codec |
| test_metadata_delta_chain_reuses_bounded_materialized_head | GAP-HEADS:H17,H2 | Shared off-lock materialisation, bounded cache, no authority from cache, evicted record rebuilt; not built |
| test_metadata_history_payloads_are_disk_backed_and_byte_bounded | GAP-HEADS:H17 | History is disk backed, no resident payload, cache byte limit honoured; not built |
| test_cold_accepted_head_validation_caches_only_owned_results | GAP-HEADS:H17,H6 | Startup validation of an accepted head caches at most 2 entries, not one per delta; not built |
| test_metadata_materialization_cache_never_validates_corrupt_delta | GAP-HEADS:H1,H2,H17 | import_history refuses a delta rebuilding another hash despite a cached parent; not built |
| test_metadata_dlt7_presence_flags_round_trip | KEEP | Macha's DLT5-7 delta format bytes, owner codec, no store |
| test_metadata_dlt8_append_extents_delta | KEEP | Macha's DLT8 extent delta format, owner codec, no store |
| test_metadata_merge_over_append_ordered_tombstones_is_a_delta | KEEP | FS merge and DLT7 bytes, owner codec, no store |
| test_metadata_superseded_conflicts_leave_the_snapshot | KEEP | FS conflict records and FS merge rules (H11 FS side), no store |
| test_metadata_merge_delta_preserves_standing_conflicts | REWRITE | FS conflict set and DLT7 bytes, with the replica's exact reconstruction; MetadataReplica fixture becomes Dagda heads with owner codec |
| test_metadata_delta_child_of_merge_commit_reconstructs | REWRITE | FS conflict set and DLT7 bytes, with cold replay over a merge anchor; MetadataReplica fixture becomes Dagda heads with owner codec |
| test_metadata_delta_tombstone_edits_are_linear | KEEP | Macha's inline garbage delta apply and its complexity, owner codec, no store |
| test_an_entrys_provenance_survives_snapshot_and_delta | KEEP | FsEntry provenance bytes in snapshot and delta, owner codec, no store |
| test_a_written_entry_is_stamped_against_what_was_there | KEEP | FS provenance stamping rules, namespace semantics, no store |
| test_the_legacy_clock_survives_snapshot_and_delta | KEEP | legacy_clock is blob side (H13 FS), owner codec, no store |
| test_protocol20_state_rejects_legacy_authority_mutators | GAP-HEADS:H5,H25 | seed, remember_committed and cas cannot move an accepted head; not built |
| test_local_store | RETIRE | StoreLocalStore.ALooseObjectIsSealedAccountedAndReaffirmed |
| test_storage_pool_and_persistent_cache | GAP-HEADS:H25 | Seed record round trip across reopen and disable/enable missing: with_seed offers only a place. Rest: StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance, cache LRU case (see Uncertain) |
| test_metadata_codec_and_replica | REWRITE | Pins SM5, SM7, DLT1 and current snapshot bytes and macOS FUSE names, over MetadataReplica journal, checkpoint and cache-seed recovery; fixture becomes Dagda heads (see Uncertain) |
| test_metadata_identity_rpc | GAP-HEADS:H24 | get_metadata_identity reply is the replica's generation and hash on BareNode; Dagda has no such message yet |
| test_repair_step_is_bounded_and_yields | RETIRE | StoreRepair.AStepIsBoundedAndYields,StoreRepair.APushKeepsItsPlaceAcrossANewList |
| test_genesis_root_configuration | REWRITE | Configured root uid, gid and mode read through FileSystem getattr; TestService fixture |
| test_metadata_delta_rejects_unrepresentable_garbage_reordering | KEEP | Macha's inline garbage order in DLT5-7, owner codec, no store |
| test_local_metadata_store_falls_back_from_invalid_delta | GAP-HEADS:H14,H2 | A wrong exact delta is refused and the commit retried as a full body; no mutate_local yet. TestService and getattr read rewrite with it |
| test_decoded_extent_vectors_carry_no_allocator_slack | KEEP | FS snapshot decoder and forged SM13 bytes, owner codec, no store |
| test_torrent_requests_join_without_conflicts | KEEP | TorrentRequest's own merge answers and duplicate_torrent, step 8b port gate over the traits, no store |
| test_tombstone_batches_ride_the_head_and_its_deltas | KEEP | SM14, SM19, DLTB magic and FS tombstone batches, owner codec, no store |
| test_torrent_requests_ride_the_snapshot_and_its_deltas | KEEP | Pins torrent bytes SM15, SM16, DLT9 and FS merge join, step 8b port gate, no store |
| test_a_delta_refuses_a_torrent_request_with_no_id | KEEP | Pins DLT9 and refusal in Macha's decoder, no store. LeaseClaimedCollection cases prove the template with test traits, never DLT9 |

### test_storage_v18.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_packs_store_compact_and_survive_restart | RETIRE | StoreLocalStore.PacksStoreCompactAndSurviveRestart |
| test_admission_respects_reserve_and_capacity | RETIRE | StoreLocalStore.AdmissionRespectsTheFreeSpaceReserve, StoreStoragePool.ARefusedPutFallsThroughToABackendWithRoom, StoreStoragePool.OneCopysLogicalCapacityIsTheSumOfTheNodes |
| test_pack_recovery_settles_each_kind_of_damage | RETIRE | StoreLocalStore.PackRecoverySettlesEachKindOfDamage |
| test_held_device_io_blocks_only_its_own_object | RETIRE | StoreLocalStore.HeldDeviceIoBlocksOnlyItsOwnObject |
| test_presence_is_known_without_reading_objects | RETIRE | StoreLocalStore.PresenceIsKnownWithoutReadingObjects |
| test_metadata_control_store_is_independent_of_data_quota | RETIRE | StoreControlStore.AControlWriteIsNotBlockedByDataExhaustion, DistributedStore.AFullPreferredNodeSpillsDataButNotControl |
| test_unversioned_state_is_refused | GAP | Refusal covered by StoreStateLayout.ANonEmptyUnversionedStateIsRefused, pool by StoreStoragePool.ANonEmptyUnversionedBackendIsRefused; no Node up over such a backend, control ready (see Uncertain) |
| test_a_full_preferred_node_spills_data_but_not_control | REWRITE | Catalogue artwork spill, root delivery, reader follows head and reads artwork; store half is DistributedStore.AFullPreferredNodeSpillsDataButNotControl |
| test_repair_passes_unavailable_and_counts_unsourceable_objects | RETIRE | StoreRepair.RepairPassesUnavailableAndCountsUnsourceable |
| test_a_busy_peer_still_says_what_it_does_not_hold | RETIRE | StoreRepair.ABusyPeerStillSaysWhatItDoesNotHold |
| test_durability_barrier_across_peer_restarts | RETIRE | DistributedStore.ADeferredWritesBarrierAcrossPeerRestarts (see Uncertain) |
| test_min_write_two_uses_fallback_when_preferred_replica_is_full | GAP | Macha put() seeks write_copies copies; Dagda put() defaults to persisted, floor 1 (distributed_store.cpp:710). DistributedStore.WriteCopiesTwoUsesAFallbackWhenAPreferredOwnerIsFull proves write() at replicated only (see Uncertain) |
| test_min_write_floor_publishes_then_repair_converges_to_r2 | RETIRE | StoreRepair.AWriteAtTheFloorConvergesToTwoCopiesThroughRepair |
| test_nodes_replicating_to_each_other_do_not_hold_their_slots_across_the_peer | RETIRE | StoreRepair.NodesReplicatingToEachOtherDoNotHoldTheirSlotsAcrossThePeer |
| test_retain_data_batches_a_large_publication_within_bounded_time | RETIRE | StoreDistributedStore.RetainDataBatchesALargePublicationWithinBoundedTime |
| test_a_control_graph_larger_than_the_connection_budget_still_publishes | RETIRE | DistributedStore.AControlGraphLargerThanTheConnectionBudgetStillPublishes |
| test_a_control_offer_sends_only_what_the_peer_lacks | RETIRE | DistributedStore.AControlOfferSendsOnlyWhatThePeerLacks |
| test_catalogue_control_objects_recover_on_metadata_replica | REWRITE | Catalogue repair restores manifest and shards and a fresh manager reads the item; store half is DistributedStore.ControlObjectsRecoverOntoAReplicaFromAnother |
| test_edge_node_never_owns_and_its_writes_land_on_owners | RETIRE | DistributedStore.AnEdgeNodeNeverOwnsAndItsWritesLandOnOwners (see Uncertain) |
| test_repair_keeps_a_copy_it_has_nowhere_to_send | RETIRE | StoreRepair.RepairKeepsACopyItHasNowhereToSend |
| test_node_that_stops_hosting_drains_through_repair | RETIRE | StoreRepair.ANodeThatStopsHostingDrainsThroughRepair |

### test_subsystem_supervisor.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_plugin_discovery_settles_each_kind_of_file | KEEP | plugin supervisor with empty context, no store |
| test_builtin_lifecycle_settles_each_outcome | KEEP | plugin supervisor with scripted builtins, no store |
| test_stop_cancels_a_blocked_factory | KEEP | plugin supervisor, no store |
| test_service_stops_plugins_before_the_store | REWRITE | TestService; pins that plugins stop before Service withdraws store admission |

### test_torrent_disk_io.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_a_real_swarm_downloads_through_the_backend_and_leaves_nothing_behind | KEEP | real libtorrent swarm over Macha's backend; a data arbiter only supplies admission, no store under it |
| test_extents_publish_once_their_pieces_verify_and_journal_the_manifest | KEEP | torrent backend over hooks and extent journal, no store |
| test_publication_progress_is_reported_until_every_extent_is_published | KEEP | torrent backend over hooks, no store |
| test_a_removed_torrent_keeps_its_files_alive_until_publication_lets_go | KEEP | torrent backend over hooks, no store |
| test_lost_piece_alerts_are_recovered_from_the_held_bitfield | KEEP | torrent backend over hooks, no store |
| test_a_restarted_backend_does_not_republish_journalled_extents | KEEP | torrent extent journal, no store |
| test_pieces_spanning_files_round_trip_through_the_payload_layout | KEEP | torrent payload bytes and layout, no store |
| test_a_hash_issued_before_its_writes_complete_sees_them | KEEP | torrent backend job order, no store |
| test_every_read_write_and_hash_is_admitted_and_measured | KEEP | torrent backend over hooks, no store |
| test_a_blocked_admission_pushes_back_on_the_network | KEEP | torrent backend over hooks, no store |
| test_abort_releases_jobs_blocked_in_admission | KEEP | torrent backend over hooks, no store |
| test_resume_is_trusted_only_while_files_still_hold_it | KEEP | torrent backend resume check, no store |
| test_delete_removes_the_payload_and_nothing_else | KEEP | torrent backend delete, no store |
| test_a_three_hundred_file_torrent_holds_a_bounded_number_of_descriptors | KEEP | torrent backend descriptor cache, no store |
| test_loader_admission_waits_for_credit_and_yields_on_abort | REWRITE | torrent's loader_admission over the data arbiter and disk monitor, which become Dagda's; the adaptor stays Macha's |
| test_a_held_torrent_is_not_restarted_by_the_queue | KEEP | libtorrent queue behaviour; CountingSession's arbiter is incidental, type swap only |
| test_a_torrent_held_at_add_is_never_checked | KEEP | libtorrent check behaviour; CountingSession's arbiter is incidental, type swap only |
| test_resume_data_lets_a_restart_skip_the_recheck | KEEP | libtorrent resume behaviour; CountingSession's arbiter is incidental, type swap only |
| test_resume_data_saved_while_held_does_not_keep_a_released_job_held | KEEP | libtorrent resume behaviour; CountingSession's arbiter is incidental, type swap only |
| test_a_queued_check_is_told_apart_from_a_running_one | KEEP | torrent status classifier, pure |
| test_shutdown_cancels_a_publication_in_flight | KEEP | torrent backend abort over a publish hook, no store |

### test_users.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_role_policy | KEEP | Role policy and static route table with UserStore; no store. |
| test_user_table_merge_rules | KEEP | UserStore merge rules; account table, no object store. |
| test_user_credentials | KEEP | UserStore credentials; no object store. |
| test_a_change_being_written_holds_no_reader_of_the_accounts | KEEP | UserStore lock discipline over its own file; no object store. |
| test_user_and_session_encodings | KEEP | Account and session encodings; no object store. |
| test_initial_accounts | KEEP | UserStore genesis; TestCluster used only for keys. |
| test_password_validator_outcomes | KEEP | Validator over UserStore; no object store. |
| test_password_login_mints_a_bound_session | REWRITE | Login through SessionApi; stands on BareNode. |
| test_password_work_is_one_limit_for_sign_in_and_account_changes | REWRITE | Password work limit through the APIs; stands on BareNode. |
| test_users_api | REWRITE | Users API; stands on BareNode. |
| test_recovery_key_machinery_is_dormant_but_sound | KEEP | Recovery envelope crypto; TestCluster used only for keys. |
| test_accounts_converge_across_nodes | REWRITE | Account gossip and login across two BareNodes; Macha-visible, notes transport. |

### test_work_class.cpp

| case | fate | dagda case or reason |
|---|---|---|
| test_every_frame_has_one_class | KEEP | frame and class vocabulary, Dannan's sched; no store |
| test_each_class_moves_data_on_its_own_frame | KEEP | frame and class vocabulary, Dannan's sched; no store |
| test_presence_is_per_class_and_bytes_per_frame | KEEP | Macha's ActivityClocks, pure; Dagda reads Dannan's activity; no store |
| test_control_never_forms_data_work | RETIRE | StoreIoPressure.ControlCannotEnterTheDataArbiterAtAll |

## GAPS

27 cases: work Dagda owes, or a question to settle first where the entry says so. Nothing here retires until its case exists.

**Write path and durability.**

- `test_storage_v18.cpp` `test_min_write_two_uses_fallback_when_preferred_replica_is_full`: Macha put() seeks write_copies copies; Dagda put() defaults to persisted, floor 1 (distributed_store.cpp:710). DistributedStore.WriteCopiesTwoUsesAFallbackWhenAPreferredOwnerIsFull proves write() at replicated only
- `test_invariants.cpp` `test_persistent_cache_is_explicitly_ephemeral`: Macha counts fsync and syncfs calls, zero. StorePersistentCache.TheCacheIsExplicitlyEphemeral covers stats, eviction, no barrier recorded and no accounting file; a raw fsync in the cache would pass
- `test_invariants.cpp` `test_durability_batch_retains_exact_nondominated_frontier`: Dagda has no DurabilityBatch or DurabilityRequirement type and no test of a nondominated frontier over per-object replica generations; nearest is none
- `test_invariants.cpp` `test_deferred_object_barrier_rejects_stale_process_epoch`: Direct refusal of a stale barrier and put reply epoch equal to the server's. DistributedStore.ADeferredWritesBarrierAcrossPeerRestarts covers refusal only indirectly, through its re-probe
- `test_invariants.cpp` `test_rpc_durability_barrier_group_commits_independent_publications`: One physical barrier for three barrier calls over the wire. StoreLocalStoreDurability.ADomainGroupCommitsIndependentPublications covers it in process, not through storage_server
- `test_invariants.cpp` `test_rpc_durability_barrier_reuses_already_covered_generation`: Barrier calls over the wire: covered generation costs none, newer costs one. StoreLocalStoreDurability.ABarrierRemembersTheWholeCut covers it in process, not through storage_server
- `test_foundations.cpp` `test_write_behind_acts_once_per_window`: StoreWriteBehind.AWindowFillsThenStartsAgain covers filling and restarting, not that an overfill leaves no carry (window_filled(0) false after a large write)
- `test_foundations.cpp` `test_write_behind_leaves_what_was_written_intact`: StoreWriteBehind.WritebackLeavesTheFileAsWritten checks only file size, never reads the bytes back; the content comparison is missing

**Repair and maintenance.**

- `test_invariants.cpp` `test_replica_repair_does_not_count_corrupt_remote_as_healthy`: A sender's repair healing a corrupt remote copy unscrubbed. DistributedStore.HealsABadLocalCopyFromAPeer covers scrub then pull on the corrupt node, not this
- `test_filesystem_fuse.cpp` `test_disconnected_maintenance_sleeps_until_peer_event`: No Dagda case or code: maintenance_loop polls each interval (node.cpp:657-659), no peer-event wake. NodeMaintenance.TheNodesThreadRunsThePassesOnItsOwn covers running, not parking.

**Arbiter and work context.**

- `test_contract.cpp` `test_data_work_context_is_the_data_specialisation`: StoreIoPressure.ControlCannotEnterTheDataArbiterAtAll covers the control refusal, not quantum_bytes nor frame_type through the WorkContext base
- `test_foundations.cpp` `test_data_resource_arbiter_background_effort_ceiling`: StoreDataWork.ACreditWaitGivesUpWhenNothingIsMoving covers a loader at the ceiling, not speculative counted, foreground exempt, nor background_limit/active/peak stats
- `test_foundations.cpp` `test_data_resource_arbiter_reserves_viewer_headroom`: StoreRepair.ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit covers headroom, not oversized refusal, loader served before speculative on release, or class stats
- `test_root_parts.cpp` `test_node_resources_yield_the_loader_to_a_recent_viewer_under_pressure`: StoreIoPressure.TheLoaderYieldsToAForegroundUserOnASlowDevice uses an injected signal; dagda::Node's foreground_quiet boundary and admit-nothing after stop are unproved

**Control fetch.**

- `test_root_parts.cpp` `test_control_fetch_stores_only_the_object_it_asked_for`: No Dagda case drives ControlObjectFetch with wrong answers: other reply type, other id, bad hash, truncated; nor the speculative frame and peers_asked log
- `test_root_parts.cpp` `test_control_fetch_passes_over_a_failing_peer`: DistributedStore.ControlObjectsRecoverOntoAReplicaFromAnother covers a successful pull, not passing a dead peer, the observer, or asking nobody on a second pull
- `test_root_parts.cpp` `test_control_fetch_fails_when_the_store_cannot_take_the_object`: No Dagda case: a good answer the control store refuses is a failed pull, observed once, not valid locally, and logged

**Placement and selection.**

- `test_foundations.cpp` `test_placement`: StorePlacementVectors.DataOrderIsMachasOnEveryVector pins determinism and domain diversity by known answer, not stability when a node is added
- `test_foundations.cpp` `test_capacity_placement`: StorePlacementVectors.DataOrderIsMachasOnEveryVector pins orders and logical capacity on fixed sets, not the small node's exact shard boundary nor R=1 monotonic stealing
- `test_models.cpp` `test_capacity_placement_invariants_model`: StorePlacementVectors.DataOrderIsMachasOnEveryVector pins orders on fixed sets, not the R=1 monotonic rule when a node is added, nor distinct domains at every R on six nodes
- `test_hydration_catalogue.cpp` `test_replica_selector`: ReplicaSelector.* cover foreground rotation, promotion, penalty. Missing: speculative rotation, speculative in-flight load on equal peers, failures count kept after success.

**Node over an old state.**

- `test_storage_v18.cpp` `test_unversioned_state_is_refused`: Refusal covered by StoreStateLayout.ANonEmptyUnversionedStateIsRefused, pool by StoreStoragePool.ANonEmptyUnversionedBackendIsRefused; no Node up over such a backend, control ready

**Predicates (Dagda has none; settle whether they move before writing cases).**

- `test_predicates.cpp` `test_each_predicate_is_its_row_over_every_combination_of_facts`: Dagda has no predicates: no satisfies or ObjectFacts in Dagda source or tests. Nearest: none
- `test_predicates.cpp` `test_each_query_pages_to_the_predicate_over_the_whole_set`: Dagda has no predicates: no query() paging over inventory or claims, no Placement. Nearest: none
- `test_predicates.cpp` `test_queries_without_horizons_examine_nothing`: Dagda has no predicates: no query() to examine nothing without horizons. Nearest: none
- `test_predicates.cpp` `test_garbage_is_not_queryable_at_stage_0`: Dagda has no predicates: no query() refusing garbage. Nearest: none
- `test_predicates.cpp` `test_a_cancelled_budget_stops_a_query_before_it_examines`: Dagda has no predicates: no query(). LedgerClaimWalk.TheRetentionLedgerStopsForCancellationFirst covers claimed(), not query()

## Uncertain

For the Macha session to confirm. Each case keeps the fate shown in its table until this is answered.

**A ruling that may reach these.** The requirements were re-based while this was being written: "Macha's code as it is outranks the design documents" (Tom, 2026-11-05, requirements head). The RETIREs below that lean on "dropped by decision" rest on requirements lines 999 to 1002, which still record them as decided, but they should be read against that ruling.

**Moved on review.** Three fates differ from the first reading:
- `test_a_joining_node_pulls_its_objects_through_maintenance`: RETIRE to REWRITE.
- `test_min_write_two_uses_fallback_when_preferred_replica_is_full`: RETIRE to GAP.
- `test_persistent_cache_is_explicitly_ephemeral`: RETIRE to GAP.

**Questions, by case.**

| case | fate | question |
|---|---|---|
| test_rpc_cluster test_store_put_replaces_a_replica_that_cannot_be_launched | RETIRE | Macha's throw at placement has no Dagda form: routes().call always completes through its callback, so a failed reply takes the same path. |
| test_rpc_cluster test_repair_push_probes_by_window_and_sends_together | RETIRE | Probe half dropped by decision: repair is diff only, have_* probes not moved (requirements line 999). Dagda sees min(8, DATA workers less reserve) in flight, not all eight: Dannan's server limit. |
| test_rpc_cluster test_repair_push_sends_nothing_to_a_peer_that_cannot_say | RETIRE | Probe half dropped by decision: repair is diff only, have_* probes not moved (requirements line 999). Cannot-say and busy replies become an unsettled survey. |
| test_rpc_cluster test_repair_pushes_from_what_peers_hold_without_asking | RETIRE | Probe half dropped by decision: repair is diff only, have_* probes not moved (requirements line 999). Macha's unknown-is-probed half is inverted on purpose: unknown is left alone. |
| test_rpc_cluster test_repair_probes_without_credit_and_transfers_only_with_it | RETIRE | Probe half dropped by decision: repair is diff only, have_* probes not moved (requirements line 999). |
| test_rpc_cluster test_repair_pass_keeps_its_place_across_live_sets_and_restarts | RETIRE | Restart half dropped by decision: repair_position not moved (requirements line 999, COMPLETED.md line 731). No Dagda case covers a restart. |
| test_storage_metadata test_retention_shard_checkpoint_moves_into_the_ledger | RETIRE | Not the same proof: the move of shard claims and tombstones has no Dagda case because Tom dropped that reader (2026-11-04); Dagda refuses an unmoved state and keeps only the file removal. Is retiring on that ruling right, given a v18 disk must be moved by Macha before migrating? |
| test_storage_v18 test_durability_barrier_across_peer_restarts | RETIRE | Dagda restarts the peer before the first wait; Macha re-asks a settled batch. filesystem.cpp:1297 clears the batch on success, catalogue callers ask once: confirm no caller re-asks. |
| test_storage_v18 test_edge_node_never_owns_and_its_writes_land_on_owners | RETIRE | Not in Dagda: inbound_capable (test_rpc_cluster.cpp:6733, Dannan's) and hosts_extents auto from no backends (test_foundations.cpp:1731, Macha config). Both tested elsewhere; confirm they stay. |
| test_filesystem_fuse test_coalesced_delete_burst_wakes_at_exact_garbage_grace | REWRITE | If the pass moves to dagda::Node the exact-grace wake is a GAP: its loop polls (node.cpp:657-659). |
| test_invariants test_catalogue_keeps_what_the_namespace_holds | REWRITE | Artwork block needs a replacement for DurabilityBatch; see test_durability_batch_retains_exact_nondominated_frontier. |
| test_maintenance_trace test_trace_quiet_node | REWRITE | Needs dagda::Node to expose the collector's trace and a manual clock; it passes no Trace today (node.cpp:413). |
| test_maintenance_trace test_trace_claimed_objects_lost | REWRITE | Fixture wants claim-walk lines (Dagda's ClaimWalk emits no trace) and no repair until an unrelated write; Dagda's thread passes on an interval. Does this step still hold? |
| test_namespace_migration test_a_migrated_node_serves_and_writes_its_library | REWRITE | Does the map-to-tree migration survive the v19 no-compat ruling, or does dagda-migrate replace it? If gone, all migration cases here need a ruling. |
| test_rpc_cluster test_store_control_objects_take_no_data_credit_and_commit_alone | REWRITE | Could the Macha rewrite keep only the for_commit half, since the credit half is proved in Dagda? |
| test_rpc_cluster test_service_startup_gate_spares_progress_and_ends_a_stall | REWRITE | Stall is injected at Service's data-storage stage: does that hook survive over dagda::Node open? |
| test_rpc_cluster test_replication_policy_change_on_restart | REWRITE | Once repair is cited as retired here, should the snapshot policy half become a head-space case under H20? |
| test_rpc_cluster test_a_joining_node_pulls_its_objects_through_maintenance | REWRITE | Neither side isolates pull from push; Macha asserts only that the joiner holds the ids. Moved from RETIRE on review: no Dagda test cites it and neither has a Service-level provider. |
| test_storage_metadata test_metadata_codec_and_replica | REWRITE | Split at the port: replica half (journal tail, quarantine, compaction; H5, H16, H25) is owed by heads; codec half stays Macha's. Agreed? |
| test_invariants test_convergence_demand_coalesces_burst_and_keeps_same_generation_event | KEEP | Goes with the metadata convergence pass: if heads reconciliation moves to Dagda step 4, does this primitive move too? |
| test_maintenance_trace test_manual_clock_moves_and_wakes_only_when_told | KEEP | Does MaintenanceClock survive the port, with the pass on Dannan's time source? If not this case goes with it. |
| test_maintenance_trace test_system_clock_reads_and_waits_in_real_time | KEEP | Same question as the manual clock case. |
| test_models test_metadata_delta_state_model | KEEP | Clock regress and policy-field refusal here are generic (H2, H14): does Dagda step 4 owe its own? |
| test_namespace_tree test_a_leaf_stays_small_however_many_extents_a_file_has | KEEP | H18 cites this as proof though the same row marks the spine FS; confirm it stays Macha. |
| test_filesystem_fuse test_disconnected_maintenance_sleeps_until_peer_event | GAP | Does Service's pass stay in Macha? If so this is REWRITE on Service; floor forming is H28 either way. |
| test_foundations test_placement | GAP | Dannan placement_test covers this function's properties; may it retire against dannan_tests? |
| test_foundations test_capacity_placement | GAP | Dannan Placement.ANewcomerOnlyTakesShardsItWins covers monotonic stealing; retire there for that part? |
| test_invariants test_replica_repair_does_not_count_corrupt_remote_as_healthy | GAP | Repair is diff only since 2026-11-04, so a corrupt remote reads as held until its own scrub. Does this case become scrub then diff, or is healing without scrub still required? |
| test_invariants test_durability_batch_retains_exact_nondominated_frontier | GAP | Does the frontier stay on Macha's side of the contract, over tickets from Dagda, which would make this KEEP or REWRITE? |
| test_invariants test_persistent_cache_is_explicitly_ephemeral | GAP | Moved from RETIRE on review. No raw fsync in the source today; the owed case is an interposed count. |
| test_models test_capacity_placement_invariants_model | GAP | Dannan Placement tests prove these properties; may it retire against dannan_tests? |
| test_predicates test_each_predicate_is_its_row_over_every_combination_of_facts | GAP | Macha query() has no production caller (predicates.hpp:11). Port predicates into Dagda, or does Tom retire them as dead code? |
| test_predicates test_each_query_pages_to_the_predicate_over_the_whole_set | GAP | Macha query() has no production caller (predicates.hpp:11). Port predicates into Dagda, or does Tom retire them as dead code? |
| test_predicates test_queries_without_horizons_examine_nothing | GAP | Macha query() has no production caller (predicates.hpp:11). Port predicates into Dagda, or does Tom retire them as dead code? |
| test_predicates test_garbage_is_not_queryable_at_stage_0 | GAP | Macha query() has no production caller (predicates.hpp:11). Port predicates into Dagda, or does Tom retire them as dead code? |
| test_predicates test_a_cancelled_budget_stops_a_query_before_it_examines | GAP | Macha query() has no production caller (predicates.hpp:11). Port predicates into Dagda, or does Tom retire them as dead code? |
| test_root_parts test_node_resources_yield_the_loader_to_a_recent_viewer_under_pressure | GAP | Does NodeResources survive in Macha, or is dagda::Node the only wiring? |
| test_storage_v18 test_unversioned_state_is_refused | GAP | Macha CHECKs data_storage_ready true with 0 backends; dagda node.cpp:172 leaves the plane not ready (section 7). status_api startup line changes: accepted? |
| test_storage_v18 test_min_write_two_uses_fallback_when_preferred_replica_is_full | GAP | Moved from RETIRE on review. Should put() honour a configured write_copies above 1, or does the port pass replicated? Macha default and both deployed configs are 1. |
| test_hydration_catalogue test_metadata_decoded_cache_ttl_recovers_missed_notice | GAP-HEADS:H7,H21,H24 | No H row names the decoded view TTL itself; should it get one? |
| test_invariants test_metadata_availability_logs_only_transitions | GAP-HEADS | Is the log text and cluster_status Macha-visible (then REWRITE over Dagda heads evidence) or the head layer's own? |
| test_namespace_migration test_tombstones_live_in_batches_in_a_tree_backed_head | GAP-HEADS:H23 | H23 is FS plus Generalise: does Macha keep a rewrite of the FS-delete half once Dagda owns batch retirement? |
| test_rpc_cluster test_rpc_metadata_mutations_use_bounded_isolated_executor | GAP-HEADS:H26 | Mechanism is transport with no store under it: if Dannan's mapping proves the executor, should this be KEEP? |
| test_rpc_cluster test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers | GAP-HEADS:H26 | Mechanism is Dannan's ordered execution: does Macha keep it until heads land, or does Dannan's mapping take it? |
| test_rpc_cluster test_rpc_metadata_control_mutations_go_first | GAP-HEADS:H26 | Mechanism is Dannan's executor: does Macha keep it until heads land, or does Dannan's mapping take it? |
| test_rpc_cluster test_rpc_metadata_executor_cancellation_disconnect_and_shutdown | GAP-HEADS:H26 | Mechanism is transport with no store under it: if Dannan's mapping proves it, should this be KEEP? |
| test_storage_metadata test_metadata_branch_merge_history_roundtrip | GAP-HEADS:H3,H7,H8 | Split on port: do the FS merge assertions (conflicts, /same, catalogue root) stay as a Macha REWRITE, or does test_metadata_merge already hold them? |
| test_storage_metadata test_metadata_legacy_prepare_journal_replays_deterministically | GAP-HEADS:H5 | Does a legacy PREPARE journal survive into v19 heads, or is it read only by the migration, or dropped under "no backward compatibility"? |
| test_storage_metadata test_storage_pool_and_persistent_cache | GAP-HEADS:H25 | On H25, retire against StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance and StorePersistentCache.TheCacheEvictsTheLeastRecentlyUsedAndReopens plus the seed case. |

**Requirements rows these cases show need attention, for Dagda to settle at step 4.**
- H15 does not name the "not asked again" rule pinned by `test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again`.
- H8: `test_three_heads_merge` is KEEP because it asserts FS outcomes, so Dagda owes its own fold and determinism case on a neutral owner.
- H2: `test_a_tree_backed_delta_reconstructs_its_record` asserts only its premise, so Dagda owes the replay check.
- H17: `test_metadata_delta_in_place_preserves_namespace_storage` is KEEP as owner codec, but the row lists in-place deltas as generic, so Dagda owes its own case.
- H18: `test_a_corrupt_node_is_refused_rather_than_trusted` asserts only that nothing crashes or hangs; Dagda's port of the row should assert the refusal.
- L25 gives `test_predicates.cpp` as the proof of budgets and cursors, which hides the predicates themselves.
- L27 cites `test_torrent_requests_ride_the_snapshot_and_its_deltas`, whose body has no removal or clock-ahead assertion.
- L28 names `LeaseClaimedCollection.AnUnkeyedRecordFromADeltaNeverReachesTheSnapshot`, which swallows the `DecodeError`; `LeaseClaimedCollection.ADeltaCannotInstallARecordTheSnapshotRefuses` is the one that proves refusal. Either way `test_a_delta_refuses_a_torrent_request_with_no_id` is KEEP: it pins Macha's own DLT9 decoder.

## The Macha session's answers, 2026-11-05

The mapping is confirmed from Macha's side, with these. They outrank the tables above where they differ.

- Retirements stand: the `have_*` probes, `repair_position` and the legacy checkpoint reader (Tom's decisions, not design documents); `test_store_put_replaces_a_replica_that_cannot_be_launched`; `test_retention_shard_checkpoint_moves_into_the_ledger`; `test_durability_barrier_across_peer_restarts`, confirming at the port that no caller re-asks a settled batch; `test_edge_node_never_owns...` (the inbound half Dannan's, the `hosts_extents` half Macha's).
- May retire against a library case: `test_placement`, `test_capacity_placement`, `test_capacity_placement_invariants_model` against Dannan's `Placement.*` and the vectors; the four H26 executor cases against Dannan's `Scheduler.*`; `test_min_write_two_uses_fallback...` against `StoreDistributedStore.MachasFormsSeekWriteCopies` and a fallback case.
- Gaps, Dagda's: `test_catalogue_keeps_what_the_namespace_holds` and `test_durability_batch_retains_exact_nondominated_frontier` (a wait over many tickets coalesces by node, epoch, domain and instance into one barrier call per peer, or the FUSE group commit becomes N round trips); `test_service_startup_gate_spares_progress_and_ends_a_stall` (`Node::open` needs Macha's `StartupStageHook`); `test_persistent_cache_is_explicitly_ephemeral` (an interposed count); `test_replica_repair_does_not_count_corrupt_remote_as_healthy` becomes scrub then diff; `test_node_resources_yield_the_loader...` at the second cutover.
- Stay Macha's: `test_store_control_objects_take_no_data_credit_and_commit_alone`'s `for_commit` half; `test_a_leaf_stays_small...`; the availability transition log text; the tombstone batch's FS-delete half; the branch merge history roundtrip's FS assertions; the migration cases, settled at the merge.
- Step 4 owes: `test_replication_policy_change_on_restart`'s policy half as an H20 case; `test_metadata_codec_and_replica` split; `test_metadata_delta_state_model` on a neutral owner; an H row for the decoded view's TTL. The legacy PREPARE journal is read by the migration only.
- For Tom: `test_unversioned_state_is_refused` and zero storage (a node whose only disk is offline at start must not crash-loop on a startup timeout, so Macha may need "ready, degraded"); the predicates (the Macha session recommends retiring them in Macha as dead code).
- Depends on Tom's answer about the maintenance loop (`ACTIVE.md`, "Waiting on Tom"): `test_coalesced_delete_burst_wakes_at_exact_garbage_grace`, `test_trace_quiet_node`, `test_trace_claimed_objects_lost`, `test_disconnected_maintenance_sleeps_until_peer_event`, `test_manual_clock_*`, `test_system_clock_*`, `test_convergence_demand_coalesces_*`, which the Macha session rewrites over its own pass if that loop stays.

