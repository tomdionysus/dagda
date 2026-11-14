# Macha store map: availability, repair and the maintenance pass's store stages

Section of the call-by-call map from Macha's storage internals to Dagda. Written 2026-11-14
from code read at Macha develop f0597a4 (extracted read-only at `build/macha-f0597a4/`) and
Dagda develop 59b20e9. Nothing here was built or run.

Path prefixes: `M:` is `build/macha-f0597a4/` (Macha at f0597a4); `D:` is this repository.
Dannan paths are `../dannan/`.

## How Macha reaches Dagda here, and the two paths

Macha reaches this subsystem only through `dagda::Node` (D:dagda/node.h:323-629): its parts
`store()`, `availability()`, `repair()`, `activity()`, `ledger()` (node.h:361-382), the
collector (`collector()`, node.h:410), the local stages one by one (node.h:450-463) and
`maintenance_events()` (node.h:452). There are two drivers, and both are in the plan
(TODO/ACTIVE.md:166-167, Tom 2026-11-05):

- **Steps** (stages 8a/8b, until heads move at 8c): `MaintenanceOptions::automatic = false`
  (node.h:101) and Macha's own pass (M:src/service/maintenance.cpp:257-1394) calls Dagda's
  steps: `Repair::earn_credit/paced_by/may_start/note_gate/survey/begin_turn/claim_walks/
  lists/transfer/settle/turn_wait` (D:dagda/store/repair.h:194-246), `Collector::
  inventory_step/tombstone_step/release_step/control_step/data_step`
  (D:dagda/ledger/collector.h:197-216), `begin_local_stages` and the local stages
  (node.h:450-463). The reference port of that pass is the test bed `MachaLoop`
  (D:dagda/test/maintenance_steps_test.cpp:395-637).
- **Node loop** (after 8c): `automatic = true`, Dagda's thread runs `Node::maintenance_loop`
  (D:dagda/node.cpp:943-1001): metadata repair, then a GC pass when `next_gc` is due, then
  `Repair::pass()` (D:dagda/store/repair.cpp:213-258) and the local stages, then a wait.

The verdict column gives the **node loop** verdict, the end state. Where the steps path
differs from it, the cell says "Steps: SAME" (or how it differs): on the steps path the
behaviour is Macha's own pass code calling Dagda's step, as the `MachaLoop` bed does.

## 1. Table

| # | Macha (name, file:line) | What is relied on | Dagda through Node/Context (file:line) | Verdict | Tests (Macha / Dagda) |
|---|---|---|---|---|---|
| 1 | `Maintenance::start` (M:src/service/maintenance.cpp:109-114), `run_supervised_loop` (M:src/supervised.hpp:21-27) | One thread, restarted after a fault with 1 s to 60 s backoff, fault listed under its name in Status `threads` | Thread started at open, last (D:dagda/node.cpp:382-383) under `SupervisedThreads::loop` (../dannan/dannan/supervise/supervised.h:49-52, same backoff) | DIFFERS: the thread's name in the supervised-thread list is `dagda.maintenance`, Macha's `service-maintenance`. Steps: SAME (Macha's thread) | M: test_subsystem_supervisor.cpp (unverified which case) / D: NodeLocalMaintenance.TheNodesThreadRunsThem, NodeMaintenance.TheNodesThreadRunsThePassesOnItsOwn |
| 2 | `Maintenance::request_stop` (maintenance.cpp:116-124), called by `NodeServices::request_stop` (M:src/service/node_services.cpp:275-279) | Signal the pass to stop without joining, so every subsystem winds down at once before any is joined | none: `Node::stop()` stops and joins the thread and then closes the stores (node.cpp:388-390, 1003-1008) | MISSING. Steps: SAME (Macha's thread) | none / none |
| 3 | `Maintenance::stop` (maintenance.cpp:126-134) and the waits it ends (`wait_for_events`, maintenance.cpp:1418-1434) | Stop requested, the wait returns, the thread is joined | `stop_maintenance` (node.cpp:1003-1008): `request_stop`, `notify_all`; the wait is `wait_for(lock, stop, ...)` (node.cpp:998-999) | SAME | M: test_disconnected_maintenance_sleeps_until_peer_event (test_filesystem_fuse.cpp:3830) / D: NodeMaintenance.AStopEndsTheOwnersInventoryWalk |
| 4 | `absorb_events`, `wait_for_events` (maintenance.cpp:1396-1434), the pre-wait recheck (:1373-1374) | The pass wakes at once on a storage, claims, metadata or topology event; an event that arrives during a pass is not lost | Loop wakes early only on `_maintenance_kicked`, set by the head spaces (node.cpp:303, 639-646); otherwise it polls every `interval` (node.cpp:991-999). `maintenance_events()` gives Macha's four counts (node.cpp:887-890) | DIFFERS: a storage, claims or topology event waits up to one interval (1 s). Steps: SAME | M: test_disconnected_maintenance_sleeps_until_peer_event / D: none for the loop |
| 5 | The park deadline (maintenance.cpp:1276-1369) | Wake at the earliest of: repair credit for the next extent (:1348-1357), the share's next turn (:1358-1362), `interval` after a turn with work left (:1363-1364), local credit (:1366), the survey's `due()` (:1368-1369), the busy class's quiet window ending (:1323-1332), scrub due or scrub credit (:1306-1321), the GC quiet window (:1295-1305), metadata retry (:1283-1293) | Loop: `due = min(next_gc, next_repair, metadata_due)`, `next_repair = now + interval` always (node.cpp:988, 993-997); no credit, share, survey, busy or scrub deadline | DIFFERS: fixed 1 s cadence; see section 2. Steps: SAME (`Repair::turn_wait` repair.cpp:130, `RepairCredit::rate` repair.h:94-98, `AvailabilityService::due` availability.h:109, MachaLoop::park maintenance_steps_test.cpp:592-626) | M: test_maintenance_trace.cpp fixtures / D: StepsBed.ABusyPassWakesWhenTheQuietWindowEnds, StepsBed.RepairResumesAfterYieldingToAViewerThatArrivedMidStep |
| 6 | `busy`, `viewer_busy`, `loader_busy` (maintenance.cpp:400-411) | Busy when viewer or loader bytes moved since the last pass (`take_*_bytes`), or the class was present within `foreground_quiet` | `Node::higher_class_active` (node.cpp:526-530), `Repair::paced_by` (repair.cpp:119-126): presence within `foreground_quiet` only. `take_*_bytes` exist (D:dagda/store/distributed_store.h:492-494) but nothing in the loop takes them | DIFFERS: the bytes-since-last-pass signal is dropped; it matters only after a pass that slept longer than `foreground_quiet`. Steps: SAME (Macha's pass can OR the bytes in and pass `LocalTurnOptions::busy`, node.h:165) | M: test_repair_is_paced_not_stopped_while_higher_classes_stay_busy (test_rpc_cluster.cpp:1066) / D: NodeLocalMaintenance.BusyHoldsTheGatedStages |
| 7 | `peer_viewers_active` (M:src/cluster/cluster.cpp:413-425) with window `max(3 x telemetry_interval, 3 s)` (maintenance.cpp:414-415) | A peer's foreground or read-ahead traffic in fresh telemetry paces repair | `Repair::peer_foreground_active` (repair.cpp:70-80); read-ahead is foreground work (../dannan/dannan/test/sched_work_test.cpp:65-71); window `RepairOptions::peer_window` 3 s (repair.h:51) | SAME, provided the facade sets `peer_window` to Macha's `max(3 x telemetry_interval, 3 s)` | M: test_repair_is_paced_not_stopped_while_higher_classes_stay_busy / D: StoreRepair.RepairIsPacedNotStoppedWhileHigherClassesStayBusy, MaintenanceStepsPeers.ATurnKeepsThePeerReadingItBeganWithWhenAsked |
| 8 | `background_cpu_scale` and its inputs (maintenance.cpp:247-255, 388-398, 428-429) | Background budget shrinks continuously once the rest of the process passes `cpu_target`; the maintenance thread's own CPU is excluded | `background_cpu_scale`, `CpuPacer` (D:dagda/store/cpu_pacing.h:29-55), `system_cpu_sample` (cpu_pacing.cpp:16-22); one pacer in Repair (repair.h:266) and one for the local stages (node.h:612) | SAME | M: none named / D: NodeLocalMaintenance.TheCpuScaleIsMachas, NodeLocalMaintenance.CpuElsewhereShrinksTheCredit |
| 9 | Repair credit (maintenance.cpp:419-437) | `bandwidth` = estimate, else `initial_bandwidth`, capped by `max_bandwidth`; repair earns at the idle fraction times CPU scale whatever the busyness; banked to `max(extent, 5 s of link)`; an interval earned when no time passed (:385-387) | `Repair::earn_credit` (repair.cpp:102-115) | SAME. The loop's `pass()` earns nothing when no time passed (repair.cpp:216), harmless since a loop pass is at least an interval after the last | M: test_held_retention_claims_cost_repair_no_credit (test_rpc_cluster.cpp:3012) / D: StepsBed.CreditEarnsAnIntervalOnlyWhenAsked, StoreRepair.ATransferWaitsForItsCredit |
| 10 | Local and scrub credit (maintenance.cpp:417, 431, 436-448) | Rebalance/scrub earn at the busy or idle fraction times CPU scale, same burst cap; scrub earns only inside a due campaign and banks nothing outside | `begin_local_locked` (node.cpp:653-680) | SAME | M: none named / D: NodeLocalMaintenance.BusyHoldsTheGatedStages, NodeLocalMaintenance.TheScrubRunsWhenDueAndItsDateSurvivesARestart |
| 11 | Repair's duty share `WeightedLoaderService(95, 5, 25 ms)` (maintenance.cpp:294-295; M:src/fuse/fuse_frontend.hpp:73-185): `can_start` (:562), `started` (:709), `should_yield` (:795), `finished` in `RepairTurn`'s destructor (:713-720), `wait_for` (:1359) | Paced never stopped: a turn of 25 ms earns a rest of 19 x the turn; a throw still closes the turn | `dannan::sched::DutyShare` (../dannan/dannan/sched/duty_share.cpp:38-101) through `Repair::may_start` (repair.cpp:128), `begin_turn` (:151-154), `Turn::~Turn` (:51), `transfer`'s yield (:184), `turn_wait` (:130) | SAME (rest is `active x 95 / 5` in integer ms on both sides, capped at a day) | M: test_repair_is_paced_not_stopped_while_higher_classes_stay_busy / D: StoreRepair.RepairIsPacedNotStoppedWhileHigherClassesStayBusy, StepsBed.ATurnTheShareHoldsIsNotWalked, StepsBed.AFailedTurnStillClosesTheShare |
| 12 | `higher_class_active` with `peer_viewers` captured at the pass's start (maintenance.cpp:704-708) | The turn's yield and close read the peer reading the pass began with | `PeerReading::as_begun` (repair.h:125-128, 225); `pass()` begins its turn with the default `each_time` (repair.cpp:241) | DIFFERS: the loop re-reads peer telemetry at every yield check and at the close. Steps: SAME | M: test_repair_is_paced_not_stopped_while_higher_classes_stay_busy / D: MaintenanceStepsPeers.ATurnKeepsThePeerReadingItBeganWithWhenAsked |
| 13 | `gate.repair` and `note_repair_gate` (maintenance.cpp:562-574, 833-837) | `ran` / `share` / `quiescent` with share taking precedence; a credit-limited turn is counted `ran` then `credit`; the `credit` note carries no paced bits | `Repair::pass` (repair.cpp:229-250), `note_gate` (:132-149) | DIFFERS: quiescent is decided before the share (repair.cpp:230-240), a credit-limited turn is counted `credit` only (:245-249), and the credit note carries the pass's paced bits. Status `repair.gate.ran` reads lower than Macha's `pass_gates.ran`. Steps: SAME (MachaLoop notes both, maintenance_steps_test.cpp:492, 513) | M: test_node_services.cpp test_observation_gauges_follow_activity_repair_and_the_mount / D: StepsBed.SettleAsksInMachasOrder |
| 14 | Repair quiescence (maintenance.cpp:355-357, 850-853) | After a turn with nothing to do, repair waits until one of Macha's four events | `Repair::pass` (repair.cpp:227-236, 251-256): quiet while `_context.events().total()` and the three list handles are unchanged | DIFFERS: any Dannan node event (adverts included) or a new list ends it; more turns that find nothing. Steps: SAME (owner keeps it, MachaLoop `_network_quiet_until`) | M: test_trace_quiet_node (test_maintenance_trace.cpp:486) / D: MaintenanceSteps.TheQuietNodeFixture |
| 15 | Inventory for repair (maintenance.cpp:611-653) | Rebuilt whenever repair is due (or GC is) and the generation moved, busy or not; a repair-only pass holds a stale one back until metadata is ready | Loop: only `gc_pass_locked` builds it (node.cpp:966-973, 532-588), and `Collector::pass` builds nothing unless GC is due (D:dagda/ledger/collector.cpp:277-278), which is never while busy (node.cpp:550-553), and after a settled GC only every `gc_interval` = 300 s (node.h:103, node.cpp:969). `Repair::pass` never rebuilds it | MISSING in the loop: repair's lists stay on the old inventory while foreground or loader work continues and for up to 300 s after a settled GC. Steps: SAME (`Collector::inventory_step(pause, may_rebuild)`, collector.cpp:113-143) | M: test_trace_* fixtures, test_unlink_retires_an_object_from_the_maintenance_inventory (test_rpc_cluster.cpp:3650) / D: StepsBed.ARepairOnlyPassHoldsAStaleInventoryBack (steps only); none for the loop |
| 16 | The survey lambda (maintenance.cpp:589-606), run after the inventory (:666) or alone (:1094) | Every pass; a stop goes through; any other error is logged "availability survey deferred: ..." and the pass, repair included, goes on | `Repair::pass` calls `survey` first (repair.cpp:217-219); an error escapes `pass()` and the loop logs "repair pass: ..." (node.cpp:980-982) | DIFFERS: in the loop a survey error ends that pass's repair turn. Steps: SAME (`Repair::survey`, repair.cpp:117) | M: test_a_survey_a_peer_could_not_finish_keeps_what_was_unavailable (test_availability.cpp:326) / D: StepsBed.AFailedSurveyLeavesThePassRunning, StepsBed.AStopInsideTheSurveyComesThroughTheStep, StoreRepair.AStopEndsTheSurveysWalk |
| 17 | `AvailabilityService::refresh` triggers (M:src/service/availability_service.cpp:372-433, 562-576) | Ask peers on first survey, topology change, peer set change, a peer's storage shrinking, or a retry; pace inventory/holdings changes and growth-while-missing by 20 x the last survey's cost; a gain alone asks nobody; nothing while seeding or before the first inventory; failed peer retried 1 s doubling to 5 min | `AvailabilityService::refresh` (D:dagda/store/availability.cpp:114-168, 231-243), `AvailabilityOptions` (availability.h:30-50) | SAME (topology is Dannan's membership event count, availability.cpp:116, 152). The cost the share multiplies excludes Macha's path table: see row 20 | M: test_a_failed_diff_is_asked_again_on_a_timer (test_availability.cpp:837), test_a_disk_coming_back_is_surveyed_without_a_namespace_change (:1194), test_two_nodes_survey_what_neither_holds (:917) / D: NodeSeams.TheSurveyRunsOnTheInjectedClock, StoreRepair.ADiskComingBackIsSurveyedWithNothingReferencedChanging, StoreRepair.TwoNodesSurveyWhatNeitherHolds |
| 18 | `RemoteTrie` and `survey_by_diff` (availability_service.cpp:30-108, 451-494; M:src/ledger/availability.hpp:56-65) | Each peer's held trie diffed in rounds against the snapshot its root question pinned; presence-probe deadline 2 min (M:src/cluster/call_deadline.hpp:13); `keep_unavailable` when a peer failed | `RemoteTrie` (availability.cpp:32-98: `call` 2 min, `call_no_progress` 30 s, availability.h:34-35), `ledger::survey_by_diff`, `keep_unavailable` (D:dagda/ledger/availability.h:83-89), used at availability.cpp:180-209 | SAME | M: test_what_only_a_host_out_of_reach_holds_is_unavailable (test_availability.cpp:757) / D: LedgerAvailability.TheSurveyByDiffFindsWhatNoNodeHolds, LedgerAvailability.ASurveyAPeerCouldNotFinishKeepsWhatWasUnavailable, StoreRepair.WhatOnlyAHostOutOfReachHoldsIsUnavailable, StoreRepair.ATrieQuestionToASilentPeerEndsAtTheNoProgressWindow |
| 19 | `answer_trie` (availability_service.cpp:328-370) | Root pins the held trie (at most 4, unused 2 min dropped); 503 holdings_seeding, 410 snapshot_gone, 413 prefix_too_large | `StorageServer::answer_trie` (D:dagda/store/storage_server.cpp:130-164; pins storage_server.h:163-171) | SAME | M: test_availability.cpp (unverified which case) / D: StoreRepair.ATrieQuestionNamesAPinnedSnapshot, StoreRepair.ATrieDiffPinsAtMostFourSnapshots, StoreRepair.ATrieDiffPinIsDroppedAfterTwoMinutesUnused, StoreRepair.ARootQuestionWhileHoldingsSeedIsRefused, StoreRepair.TheServerRefusesInMachasCodes |
| 20 | `AvailabilitySnapshot` (M:src/service/availability_service.hpp:44-53) and the path table (availability_service.cpp:509-536, 551-560; constructor read-back :307-317) | `paths`, `by_hash` read by the files API (M:src/api/files_api.cpp:80) and node services (M:src/service/node_services.cpp:98, 127); `generation` is the head's; the table "followed" incrementally when each answering peer's trie root is unchanged (:521-527); persisted and read back so a restarted node answers before its first survey; the survey's cost includes the table | `AvailabilitySnapshot {generation, surveyed_unix_ms, survey}` (availability.h:55-60); `generation` is the inventory's (availability.cpp:177); the path table is left to Macha (availability.h:62-63); each `PeerDiff::root` is discarded (availability.cpp:205 keeps only `lacks`) | DIFFERS: no `paths`/`by_hash` (Macha keeps them, by design), no persisted read-back, and no per-peer roots, so Macha's table cannot take the "followed" path and must walk the namespace (`fill_path_table`) on every survey that changes anything; its walk is outside the survey's paced cost | M: test_availability.cpp:808-1046 (path table cases) / D: none |
| 21 | Survey figures and line (availability_service.cpp:441, 465-468, 539-550) | Observations `availability.join_us`, `availability.diff_us`, `availability.survey_us`; debug line ending `table=... method=...` | Debug line without `table=` (availability.cpp:223-229); no observations (no `availability.*_us` anywhere under D:dagda/) | DIFFERS: three observations missing | none / none |
| 22 | Claim walks and `CreditedRestorer` (maintenance.cpp:677-703, 721-727; M:src/service/claim_walk.cpp:8-43) | 16 claims a step a class, DATA then CONTROL; presence costs nothing, a missing claim costs an extent of credit; `ensure_local` / `ensure_control_local`; trace "claim-walk" lines; observations examined, missing, `maintenance.claim_walk.examine_us` | `Repair::claim_walks` and `Restorer` (repair.cpp:22-47, 156-170); `ClaimWalk::step` (D:dagda/ledger/claim_walk.cpp:14-41, bound 16 at claim_walk.h:47) | DIFFERS: `maintenance.claim_walk.examine_us` histogram is not recorded (claim_walk.cpp:22-26 counts only); behaviour SAME | M: test_retained_missing_copy_repairs_without_namespace_reachability (test_rpc_cluster.cpp:2926), test_held_retention_claims_cost_repair_no_credit (:3012) / D: StoreRepair.ARetainedMissingCopyIsRestoredWithoutReachability, StoreRepair.HeldClaimsCostRepairNoCredit, MaintenanceSteps.TheClaimedObjectsLostFixture |
| 23 | Repair's lists (maintenance.cpp:729-806) into `repair_step` (M:src/cluster/distributed_store.cpp:2430-2510, 2612-2706) | Pull: the survey's `missing_here`, or every referenced object before the first survey (:739-743). Push: from the peers' lacks when every peer answered for this inventory (:755-771), otherwise a walk of the store with batched validated-presence probes of the peers the survey did not settle (distributed_store.cpp:2612-2706) | `Repair::lists` (repair.cpp:172-179) into `DistributedStore::repair_step` (D:dagda/store/distributed_store.cpp:2755-3204): pull from `lists.pull` only, nothing when it is null (:2795-2796); push only when the lacks are complete and of this inventory (:2786), else nothing pushed to anyone; no store walk, no probe ("repair is diff only, Tom, 2026-11-04", distributed_store.h:297-299) | DIFFERS, by Tom's ruling: no push at all while any hosting peer cannot answer the survey; no pull before the first survey | M: test_repair_step_is_bounded_and_yields (test_storage_metadata.cpp:4466) / D: StoreRepair.APushSendsNothingToAPeerTheSurveyCouldNotSettle (pins the Dagda behaviour), StoreRepair.APushWorksFromWhatPeersHoldWithoutAsking |
| 24 | `repair_step` bounds (maintenance.cpp:729-734; distributed_store.cpp:2540-2552) | Byte budget = credit, 16 operations, scan budget clamp(4 x ops, 16, 64), skip budget, 8 sends in flight, 2 extents a batch, per-object retry 1 s doubling to 10 min, all due at once on a reachability change, `unavailable` passed without asking | distributed_store.cpp:2760-2837, 2866-3002, 3097-3184; `RepairOptions::operation_budget` 16 (repair.h:63) | SAME (summary; the step's internals are the store section's) | M: test_repair_step_is_bounded_and_yields, test_repair_passes_unavailable_and_counts_unsourceable_objects (test_storage_v18.cpp:915) / D: StoreRepair.AStepIsBoundedAndYields, StoreRepair.RepairPassesUnavailableAndCountsUnsourceable |
| 25 | What a turn leaves owing (maintenance.cpp:829-856) | credit-limited, yielded, work left, waiting for credit, quiescent (drops credit), in that order | `Repair::settle` (repair.cpp:204-211); `RepairSettled` (repair.h:112-123) | SAME | M: test_trace_* fixtures / D: StepsBed.SettleAsksInMachasOrder, StepsBed.RepairResumesAfterYieldingToAViewerThatArrivedMidStep |
| 26 | Repair's observations and slow-stage line (maintenance.cpp:807-828) | `maintenance.repair.step_us/bytes/push_examined/pull_examined.{loaded,idle}`, `maintenance.repair.retained_repairs`; DIAG line at 50 ms | `Repair::transfer` (repair.cpp:186-198); `log_slow_stage` (D:dagda/store/slow_stage.h:27-31) | SAME | M: none named / D: StepsBed.APassRecordsMachasObservations |
| 27 | Repair's trace lines (maintenance.cpp:843, 853) | "maintenance: repair yielded to foreground I/O", "maintenance: repair quiescent; waiting for an event" | none (no such text under D:dagda/) | MISSING (log lines only). Steps: SAME (Macha's) | none / none |
| 28 | `estimated_network_bps` (M:src/cluster/distributed_store.cpp:98-109) | EWMA 0.8/0.2 of transfers of at least half an extent | `note_network` (D:dagda/store/distributed_store.cpp:2591-2600), `estimated_network_bps` (distributed_store.h:542) | SAME | M: none named / D: StoreRepair.TheLinkEstimateIgnoresSmallTransfers |
| 29 | Order of a pass's store stages (maintenance.cpp:335-1226) | gate.repair; inventory; availability; retention-repair (claim walks); network-repair; tombstones; release horizon; control-gc; garbage-collect; then local-rebalance, retention-compact, head-repair, history-truncate, compact-packs, scrub, ledger-verify | Loop (node.cpp:957-989): metadata repair; GC pass (inventory, tombstones, release, control, data) when `next_gc`; then `Repair::pass` (credit, survey, gate, walks, transfer); then the local stages in Macha's order (node.cpp:861-879) | DIFFERS: the GC pass runs before repair, and repair works from the inventory as the last GC pass left it. Steps: SAME | M: test_trace_* fixtures / D: StepsBed.EveryStepRunsInMachasOrder (steps); none for the loop |
| 30 | GC's schedule inside the pass (maintenance.cpp:353, 355-362, 575-577, 652, 1079-1091, 1243-1274, 1295-1305) | GC due when not busy and past `gc_quiescent_until`; any event or new local generation sets that to now + `foreground_quiet`; a rebuilt inventory waits `foreground_quiet`; a complete sweep waits for an event, a deferred one for the orphan grace; one exact wake for the earliest retirement | Loop: `next_gc = now + (settled ? gc_interval : interval)` (node.cpp:966-973), `gc_interval` 300 s (node.h:103); no event quiet window, no retirement wake | DIFFERS: GC rests 300 s after settling whatever happens, and does not hold off `foreground_quiet` after an event. Steps: SAME (MachaLoop:473-480, 503, 534-535, 576-590) | M: test_trace_tombstones_maturing (test_maintenance_trace.cpp:498) / D: MaintenanceSteps.TheTombstonesMaturingFixture (steps); NodeMaintenance.TheNodesThreadRunsThePassesOnItsOwn (loop, schedule not pinned) |
| 31 | local-rebalance (maintenance.cpp:1100-1123) | Not busy, not resting, credit at least an extent; 64 objects, yields to foreground; rest after a complete step that moved nothing until Macha's next event | `local_rebalance_locked` (node.cpp:682-706); rest compared against `_context.events().total()` in the loop (node.cpp:864) | DIFFERS: in the loop the rest ends on any Dannan node event. Steps: SAME (`maintenance_events().total()`, node.cpp:895) | M: test_rebalance_reads_nothing_with_one_backend_online (test_invariants.cpp:2359) / D: NodeLocalMaintenance.RebalanceMovesCopiesOntoTheirPreferredBackend |
| 32 | retention-compact (maintenance.cpp:1128-1131) | Not busy: `compact_if_needed(4096)` | `retention_compact_locked` (node.cpp:708-718) | SAME | M: none named / D: NodeLocalMaintenance.TheClaimsAreCompactedPast4096Records |
| 33 | head-repair and history-truncate between them (maintenance.cpp:1133-1152) | Head repair any activity; truncation only when idle; failures logged | `head_repair_locked`, `history_truncate_locked` (node.cpp:722-764), in that place (node.cpp:868-869) | SAME (placement; their content is the heads section's) | M: test_unreconstructable_accepted_head_is_repaired_live_from_a_peer (test_rpc_cluster.cpp:4770) / D: none in this section |
| 34 | compact-packs (maintenance.cpp:1157-1160) | Not busy: one victim pack per backend, stop honoured | `compact_packs_locked` (node.cpp:788-797) | SAME | M: none named / D: NodeLocalMaintenance.DeadPackBytesAreReclaimed |
| 35 | scrub (maintenance.cpp:31-80, 1162-1197) | Not busy, campaign due, scrub credit at least an extent; 64 objects; next campaign `scrub_interval` after completion, kept at `<state>/maintenance/scrub.next`, persistence failure only logged | `local_scrub_locked` (node.cpp:799-824), `persist_scrub_due` (:623-633), read at open (:366-376) | SAME (next date from the turn's start, not the step's end: milliseconds) | M: none named / D: NodeLocalMaintenance.TheScrubRunsWhenDueAndItsDateSurvivesARestart, NodeLocalMaintenance.AScheduleMachaLeftIsHonoured |
| 36 | ledger-verify (maintenance.cpp:1199-1226) | 4 directories a second, 0.25 busy, credit capped 256; a full pass then 24 h rest; observations `ledger.verify.*`; info line | `ledger_verify_locked` (node.cpp:826-859), `ledger_verify_credit` (node.h:198) | SAME | M: none named / D: NodeLocalMaintenance.TheVerifysRateIsMachas, NodeLocalMaintenance.TheVerifyFindsWhatWentAndThenRests |
| 37 | `enter_stage` and `MaintenancePort` (maintenance.cpp:336-340, 1376-1391; M:src/service/maintenance.hpp:49-63), read by `Service` (M:src/service/service.hpp:231-244) | Status shows the pass's wakeups, its stage, `wait_ms`, `gc_quiet_ms`, `busy`, `gc_due` | `MaintenanceOptions::stage` hook (node.h:132-135) is told only retention-compact, head-repair, history-truncate, compact-packs (node.cpp:712, 728, 747, 791) and the heads' stages; no wakeups, wait, quiet or flags figures (publish_status, node.cpp:1069-1230) | MISSING in the loop: pass-begin, inventory, availability, retention-repair, network-repair, control-gc, garbage-collect, local-rebalance, scrub, ledger-verify, wait, and the sleep diagnostic. Steps: SAME (Macha's port) | none / none |
| 38 | Stop inside a pass (maintenance.cpp:597-600, 617-620, 1159, 1228-1229) | A stop ends the survey's and the inventory's walks and pack compaction; claim walks and the repair step run to their bound | Survey `pause` (repair.cpp:217-219), inventory `pause` (node.cpp:556-558), local stages check the stop between and inside stages (node.cpp:66-68, 522-524); claim walks and transfer run to their bound (repair.h:227-242) | SAME (Dagda's local stages stop sooner) | M: test_trace_* / D: StepsBed.AStopBetweenAnyTwoStepsEndsTheLoopAsMachasDoes, StepsBed.AStopInsideABoundedStepWaitsForItsBound, NodeMaintenance.AStopEndsTheOwnersInventoryWalk |
| 39 | Repair's Status block (M:src/api/status_api.cpp:1311-1368) | unsourceable, local_unreadable, sample, push/pull examined, bytes, passes, push_phase_complete, pass_gates, last_credit_bytes, pace, paced_by | `store().repair_diagnostics()` (D:dagda/store/distributed_store.cpp:2737-2753) plus `repair().gates()` (repair.cpp:88-100), published as `dagda.repair.*` (node.cpp:1172-1194); paced bits equal Macha's values (repair.h:79) | SAME by composition (the gate counts differ as row 13 says) | M: test_observation_gauges_follow_activity_repair_and_the_mount (test_node_services.cpp:308) / D: none pins the figures |
| 40 | Node gauges (M:src/service/node_services.cpp:195-207) | viewer_idle_ms, loader_idle_ms, repair_* gauges, repair_estimated_network_bps | `dagda.activity.*_idle_ms`, `dagda.repair.*` (node.cpp:1186-1197) | SAME | M: test_observation_gauges_follow_activity_repair_and_the_mount / D: none |
| 41 | Metadata repair holding the pass (maintenance.cpp:498-532) | A failed convergence with recovery required or generation at most 1 ends the pass; otherwise repair goes on | `hold_pass` (D:dagda/heads/exchange.cpp:1351-1365) skips GC and repair (node.cpp:957, 964, 974) | SAME | M: test_failed_convergence_retries_once_per_burst_of_events (test_rpc_cluster.cpp:1775) / D: heads section |
| 42 | `ThreadCpuReporter("macha-maint", 5 s)` (maintenance.cpp:259, 1276) | The maintenance thread's CPU reported | none in D:dagda/ | MISSING (diagnostic). Steps: SAME (Macha's thread) | M: test_thread_cpu_reporter_debug_escalation (test_foundations.cpp:359) / none |
| 43 | Slow-stage DIAG lines for local-rebalance and scrub (maintenance.cpp:1110-1112, 1171-1173) | `DIAG maintenance-stage stage=... elapsed_ms=...` at 50 ms | node.cpp:690-691, 806 | SAME | none / D: node_seams_test.cpp:271-274 (unverified case name) |

Counts, node loop (after 8c): **SAME 23, DIFFERS 15, MISSING 5** (43 rows).
Counts, steps path (8a/8b): **SAME 39, DIFFERS 4** (rows 20, 21, 22, 23), **MISSING 0**.

## 2. Performance-relevant differences

1. **Repair's inventory in the node loop (row 15).** Macha rebuilds the inventory whenever
   repair is due and the generation moved (maintenance.cpp:611-653), so the survey and repair
   see a new file within one pass. Dagda's loop rebuilds it only in a GC pass
   (node.cpp:966-973), which does nothing while foreground or loader work is present
   (collector.cpp:277, node.cpp:550-553) and, once settled, runs every 300 s (node.h:103).
   During a sustained ingest the survey's `missing_here` and the peers' lacks
   (availability.cpp:170-174, 188) never name the new objects, so repair neither pulls nor
   pushes them until the loader stops; prompt replication still makes their second copy.
2. **Turn cadence under load (row 5).** Macha parks until the share's next turn
   (maintenance.cpp:1358-1362) or one interval after a turn with work left (:1363-1364).
   With a 25 ms turn the rest is 475 ms, so Macha takes a turn about every 0.5 s. Dagda's
   loop wakes every `interval` (1 s, node.cpp:988, 997) and `may_start` is asked then, so it
   takes a turn about every 1 s: about half Macha's repair rate under contention while
   turns are short (under 53 ms, where 19 x the turn is under the interval). For longer
   turns the loss is at most one interval of rounding per turn.
3. **Wake latency (rows 4, 5).** Macha wakes at once on storage, claims, metadata or topology
   events and at the exact credit, survey `due()` and quiet-window deadlines. Dagda's loop
   wakes early only on metadata (node.cpp:303, 639-646) and otherwise within one interval.
4. **Stage order (row 29).** Dagda's loop runs the GC pass (inventory build, tombstones,
   release, CONTROL sweep of 32, DATA sweep of 64) before repair in the same locked pass
   (node.cpp:966-989); Macha runs repair right after the inventory and survey. When GC is
   due, repair's start is delayed by the GC steps' time.
5. **Survey cost and the path table (rows 17, 20).** Macha's survey cost (`surveyed_cost_`,
   availability_service.cpp:537-538) includes the path table, so its duty share (20 x cost)
   bounds both. Dagda's cost covers the survey alone (availability.cpp:222); Macha's path
   table, rebuilt on top, is unpaced, and without the peers' roots it cannot use
   `update_path_table` (availability_service.cpp:521-527) and walks the namespace each time.
6. **Diff-only repair (row 23).** While any active hosting peer cannot finish a diff (a
   seeding disk answers 503, a slow peer times out at 30 s no progress), Dagda pushes
   nothing to any peer (distributed_store.cpp:2786-2793); Macha walks its store and probes
   (Macha distributed_store.cpp:2612-2706). Before a node's first complete survey Dagda
   pulls nothing (distributed_store.cpp:2795-2796); Macha pulls over every referenced
   object (maintenance.cpp:739-743). Fewer round trips in Dagda, slower convergence in
   those states. Tom's ruling (distributed_store.h:297-299).
7. **Extra quiet-ending wakes (rows 14, 31).** Dagda's loop ends repair's and rebalance's
   rest on any Dannan node event (repair.cpp:229-236, node.cpp:864); each costs a claim-walk
   step (two classes, 16 claims each) and a repair step that scans to its budget, and a
   rebalance step of up to 64 objects.
8. **Peer telemetry per yield check (row 12).** `pass()` re-reads every peer's telemetry
   (repair.cpp:70-80, 82-86) at each operation boundary and at the close; Macha reads it once
   a pass.
9. **Locks.** Dagda's loop holds `_maintenance_mutex` across metadata repair, GC, the whole
   repair pass (survey round trips included) and the local stages (node.cpp:953-990);
   `register_reachability`, `publish_inventory`, `reconfigure` and the step API wait for it
   (node.cpp:454, 508, 591, 893-921). Macha's pass holds no such lock across its stages;
   its readers of the survey are lock-free on both sides (Published handles).

## 3. Gaps for Dagda to fix

1. **The node loop does not rebuild the inventory for repair.** Macha needs repair's
   inventory current whenever repair is due, busy or not (maintenance.cpp:611-653). Dagda's
   loop builds it only in a GC pass, never while busy and every 300 s once settled
   (node.cpp:966-973, collector.cpp:277). Test: `NodeMaintenance.RepairFollowsANewGenerationWhileTheLoaderIsBusy`
   (loader noted every step, a new generation referencing an object only a peer holds: the
   node pulls it within a few intervals). Severity: **blocks the port** (at 8c).
2. **The node loop parks on a fixed interval.** Macha parks for the share's next turn,
   credit, the survey's `due()`, the quiet window and scrub (maintenance.cpp:1276-1369) and
   wakes on its four events (:1396-1434). Dagda's loop polls every interval and wakes only
   on metadata (node.cpp:991-999). Test: `NodeMaintenance.UnderLoadRepairTakesATurnEachRestNotEachInterval`
   (stepped time, foreground busy, 25 ms turns: at least 3 turns in 2 s of repair time).
   Severity: **loses performance**.
3. **The node loop runs GC before repair.** Macha: inventory, survey, repair, then the
   destructive stages (maintenance.cpp:611-1092). Dagda: GC pass, then repair
   (node.cpp:966-989). Test: `NodeMaintenance.AThreadPassRunsItsStagesInMachasOrder`
   (stage hook sequence). Severity: **loses performance**.
4. **The node loop's GC schedule is not Macha's.** Macha holds GC `foreground_quiet` after
   any event or rebuilt inventory, rests until an event after a complete sweep, and wakes
   once for the earliest retirement (maintenance.cpp:353-362, 652, 1079-1091, 1243-1274).
   Dagda rests `gc_interval` (300 s) after a settled pass and has no event window
   (node.cpp:966-973). Test: `NodeMaintenance.AnEventHoldsTheSweepForTheQuietWindowThenItRuns`.
   Severity: **loses performance** (reclamation latency); the collector section should
   confirm whether the missing quiet window matters for safety (unverified here).
5. **No signal-only stop.** Macha's `NodeServices::request_stop` signals maintenance
   without joining (node_services.cpp:275-279). Dagda has only `Node::stop()`
   (node.cpp:388-390). Test: `NodeMaintenance.ARequestedStopReturnsAtOnceAndTheJoinFollows`.
   Severity: **loses performance** (shutdown latency).
6. **The node loop tells too few stages and keeps no sleep diagnostic.** Macha's Status
   reads stage, wakeups, wait_ms, gc_quiet_ms, busy, gc_due (service.hpp:231-244). Dagda's
   loop names four stages (node.cpp:712, 728, 747, 791) and publishes none of the rest.
   Test: `NodeMaintenance.TheThreadNamesEveryStageItEnters`. Severity: **cosmetic**.
7. **The survey's snapshot drops each peer's root.** Macha's path table needs the answering
   peers' roots to follow the tree incrementally (availability_service.cpp:486, 494,
   521-527); Dagda discards `PeerDiff::root` (availability.cpp:205). Test:
   `StoreRepair.TheSurveyNamesTheRootItDiffedOfEachPeer`. Severity: **loses performance**
   (Macha walks its namespace on every survey that changes anything).
8. **Survey and claim-walk observations missing.** `availability.join_us`,
   `availability.diff_us`, `availability.survey_us` (availability_service.cpp:441, 466, 539)
   and `maintenance.claim_walk.examine_us` (claim_walk.cpp:9) are not recorded. Test:
   `StepsBed.ASurveyRecordsMachasObservations`. Severity: **cosmetic**.
9. **`Repair::pass` counts gates differently.** A credit-limited turn is not counted `ran`,
   and quiescent is decided before the share (repair.cpp:230-249); Macha counts `ran` then
   `credit` and lets the share win (maintenance.cpp:564-574, 836). Test:
   `StoreRepair.ACreditLimitedPassCountsRanThenCredit`. Severity: **cosmetic** (Status).
10. **A survey failure ends the loop's repair turn.** Macha logs and carries on
    (maintenance.cpp:603-605); Dagda's `pass()` lets it escape (repair.cpp:217-219,
    node.cpp:980-982). Test: `NodeMaintenance.AFailedSurveyStillLetsRepairRun`. Severity:
    **loses performance**.
11. **`pass()` reads peer telemetry each time, and its rests end on any node event.**
    Macha reads peer viewers once a pass and rests until its four events
    (maintenance.cpp:414-416, 704-708, 344-357). Dagda: repair.cpp:229-241, node.cpp:864.
    Test: `StoreRepair.APassKeepsThePeerReadingItBeganWith` and
    `NodeMaintenance.AnAdvertDoesNotEndRepairsRest`. Severity: **cosmetic** (small extra work).
12. **Macha's log and diagnostic lines missing.** "maintenance: repair yielded to
    foreground I/O", "maintenance: repair quiescent; waiting for an event"
    (maintenance.cpp:843, 853), the thread CPU reporter (:259), and the supervised thread
    name. Test: `NodeMaintenance.RepairSaysWhenItGoesQuiet`. Severity: **cosmetic**.
13. **Diff-only repair (design question for Tom, not reopened here).** Macha pushes while a
    peer cannot answer, by walking and probing, and pulls before its first survey
    (maintenance.cpp:739-743; Macha distributed_store.cpp:2612-2706). Dagda does neither
    (distributed_store.cpp:2786-2796), by Tom's 2026-11-04 ruling, and pins it
    (StoreRepair.APushSendsNothingToAPeerTheSurveyCouldNotSettle). A test for Macha's
    behaviour, `StoreRepair.APushReachesThePeersThatAnsweredWhileAnotherCannot`, would fail
    today and contradicts that pinned case. Recommendation: push to the peers that did
    answer (their lacks are known) while one is unsettled, still asking nobody; this keeps
    "diff only" and restores Macha's progress. Severity: **loses performance** (durability
    latency while a peer is seeding or slow).

## 4. Not mapped

- Metadata convergence and catalogue repair (maintenance.cpp:456-558; `ConvergenceDemand`,
  retry triggers :1436-1450), `cluster_stable` (:859-869) and the media-information prune
  (:1402-1403): heads and catalogue sections.
- The collector's gates and steps (tombstones, release, CONTROL and DATA sweeps,
  maintenance.cpp:871-1092): collector section. Only their place and schedule in the pass
  are mapped here (rows 29, 30).
- The inside of `repair_step` beyond its bounds and lists (push batching, the presence
  window, retries, unsourceable sampling, `spans_change`), and Macha's `repair_position`
  file (only used by Macha's store walk, `!listed`, Macha distributed_store.cpp:2559):
  store section.
- Prompt replication and its figures (Macha distributed_store.hpp:231-239, status_api.cpp:1369-1375).
- Macha's own path table functions (`fill_path_table`, `update_path_table`,
  availability_service.hpp:55-76): Macha's, by design; only what they need from Dagda
  (row 20, gap 7) is mapped.
- The storage server's advert refresh (Macha storage_server.cpp:48-78, Dagda
  storage_server.h:44) as an input to the survey's shrank/grew test: defaults match
  (10 s, unverified for Macha's configured value); whether Dagda notifies a storage event
  when a disk comes back, as Macha's does (storage_server.cpp:58-60), is unverified.
- Macha test names given as "unverified which case" in rows 1 and 19, and the Dagda case
  in row 43, were not opened.
