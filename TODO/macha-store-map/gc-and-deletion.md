# Macha store map: garbage collection and the deletion path

Mapped 2026-11-14 against Macha develop `f0597a4` (extracted read-only at
`build/macha-f0597a4/`, written `M:` below) and Dagda develop `59b20e9` (written `D:`).
Every line reference was read at those commits unless marked "(wt)": a Macha test line
taken from Macha's working-tree index and not re-read at `f0597a4`.

How Macha reaches each Dagda part. Tom's ruling (ACTIVE.md:166-167): Macha's pass runs
a Macha node until 8c, calling Dagda's stages as steps, with `MaintenanceOptions::automatic`
off; from 8c Dagda's own loop runs it. So every row has two Dagda answers:

- **steps** (until 8c): `Node::collector()` (D:dagda/node.h:409-410, node.cpp:487-495) hands
  out `ledger::Collector` whose five steps (D:dagda/ledger/collector.h:197-216) Macha's loop
  calls in its own order, with its own `PassDue` (collector.h:72-85), plus `Node::ledger()`,
  `Node::data_sightings()`, `Node::retention_compact()` (node.h:365, 411-413, 454).
- **loop** (from 8c): `Node::maintenance_loop` (D:node.cpp:943-1001) and
  `Node::gc_pass_locked` (D:node.cpp:532-588).

Where the two differ the verdict says which.

What moves out of Macha and what stays. Macha's collector stages (M:src/service/
maintenance.cpp:871-1092), `CatalogueManager::control_gc_step` (M:src/catalogue/
catalogue.cpp:2654-2737) and `StoragePool::gc_step` (M:src/storage/storage_pool.cpp:1037-1105)
move into Dagda. What Macha's state refers to, its retirements and their commits stay
Macha's and are its `contract::ReachabilityProvider` (D:dagda/contract/reachability.h:77-117):
`inventory`, `release_head`/`release`, `control_epoch`/`within_control_epoch`,
`collect_garbage`. That provider is Macha code by design (Tom, 2026-11-04 night), not a shim.

## 1. Operations

| # | Macha (name, file:line) | What is relied on | Dagda through Node/Context (file:line) | Verdict | Tests (Macha / Dagda) |
|---|---|---|---|---|---|
| 1 | `tombstone_gate`, `control_gate`, `data_gate` (M:src/contract/gates.cpp:32-94) | Order of conditions: due, not rebuilt this pass, release view (control/data), catalogue complete, inventory present, generation >= known; reason strings ("foreground busy", "complete; waiting for an event", "quiet window", "inventory generation N behind known K"); `conditions` flag text for the trace | D:dagda/contract/gates.cpp:29-83, called from collector.cpp:149, 182, 216 | SAME (text and order identical; `inventory_complete` is Macha's `catalogue_complete`, the flag still printed `catalogue_complete=`) | M: tests/test_ledger_gates.cpp:132 (wt) `test_tombstone_gate_is_the_pass_condition_over_every_input` and its control/data siblings / D: `ContractGates.TheTombstoneGateIsThePassConditionOverEveryInput` (contract_gates_test.cpp:148), `ContractGates.TheInventorySplitsRetirementsByWhetherDataStillRefersToThem` (:74) |
| 2 | Inventory stage (M:maintenance.cpp:611-664): rebuild when absent, incomplete or generation moved; held back on a repair-only pass until metadata is ready; publish to the ledger, read back; `maintenance.inventory.build_us`, `maintenance.inventory.live_objects`; trace `inventory generation= live= control_live= garbage= stale_garbage= catalogue_complete=`; `DIAG maintenance-inventory` at trace level | Never consumed destructively in the pass that built it (`rebuilt_inventory` into every gate); the stop check inside the walk (`MaintenanceStopping`) | steps: `Collector::inventory_step(pause, may_rebuild)` (D:collector.cpp:113-143); generation from `provider.generation()` (:118) | SAME (the line, trace, figures and hold-back rule; `held_back` replaces Macha's second condition at M:628) | M: test_maintenance_trace fixtures (all pin the `inventory` line) / D: `LedgerCollector.AnInventoryIsNeverUsedDestructivelyInThePassThatBuiltIt` (ledger_collector_test.cpp:174), `StepsBed.ARepairOnlyPassHoldsAStaleInventoryBack` (maintenance_steps_test.cpp:1192), `StepsBed.AStopInsideTheInventorysWalkComesThroughTheStep` (:929), `NodeMaintenance.AStopEndsTheOwnersInventoryWalk` (node_maintenance_test.cpp:203) |
| 3 | Release horizon (M:maintenance.cpp:913-934): built when the sole accepted head moved; incomplete build refused, previous kept; `maintenance.release_horizon.build_us`; trace `release-horizon complete|incomplete data_live= control_live=` | Claims released only against the owner's sole accepted head and its clock | steps: `Collector::release_step()` (D:collector.cpp:155-175); `provider.release_head()` / `provider.release(head)`; refusal at D:dagda/ledger/retention_ledger.cpp:55-59 (Macha M:src/ledger/retention_ledger.cpp:51-56) | SAME (Macha reads `release_head()` twice, M:914 and M:920; Dagda once, D:161: no effect) | M: tests/test_maintenance_trace.cpp:717 (wt) `test_trace_incomplete_catalogue_and_release_horizon` / D: `LedgerCollector.AnIncompleteReleaseIsNeverPublished` (:240), `StepsBed.AnUnreadableReleaseIsIncompleteAndThePassGoesOn` (:1165), `MaintenanceSteps.TheIncompleteCatalogueFixture` (:842) |
| 4 | `Maintenance::collect_garbage` (M:maintenance.cpp:148-170) | A retirement matures at `retired_at_ns > 0 && now >= retired && now - retired >= grace` on this node's wall clock; a matured one drops its cache copy at once and stops protecting the object (bytes go only by the sweep's age check); `maintenance.tombstones.collected` counts matured only; erase = stale + matured; trace `tombstones erase=[sorted ids]` | steps: `Collector::tombstone_step` (D:collector.cpp:147-153) -> `collect_tombstones` (:71-97); cache from `Node::cache()` (node.cpp:482) | SAME (rule :25-27, cache :86, figure :92, trace :93) | M: tests/test_maintenance_trace.cpp:498 `test_trace_tombstones_maturing`, :527 `test_trace_revived_tombstone` / D: `LedgerCollector.TombstoneCollectionHandsItsDecisionsToTheOwner` (:307), `MaintenanceSteps.TheTombstonesMaturingFixture` (:711), `MaintenanceSteps.TheRevivedTombstoneFixture` (:740) |
| 5 | Unstamped retirements (M:maintenance.cpp:156-160: "ineligible until maintain_garbage_metadata() stamps it", which it never does, M:172-213) | Macha leaves a zero `retired_at_ns` alone: it protects its object forever and is never traced | D:collector.cpp:80 puts every zero retirement in `GarbageCollection::stamp`; :95 traces `tombstones stamp=[...]`; the provider is asked to stamp (reachability.h:66-74) | DIFFERS: Dagda asks the owner to stamp and adds a trace line Macha never writes. A Macha provider that ignores `stamp` keeps Macha's behaviour, but the extra trace line remains wherever a zero retirement exists | M: none / D: `LedgerCollector.TombstoneCollectionHandsItsDecisionsToTheOwner` (:307) |
| 6 | `maintain_garbage_metadata` (M:maintenance.cpp:172-213), `maintain_tombstone_batches` (M:215-245) | Erase rewrites a batch holding an erased id (rest rewritten at its own time; a later retirement survives); matured batches dropped by id, only those the converged head still names (M:225-232); both inside `metadata_.mutate_delta` | `ReachabilityProvider::collect_garbage(GarbageCollection{erase, stamp, matured_batches})` (D:reachability.h:116), called on every pass the gate opens with an inventory (collector.cpp:151; Macha M:904-905 likewise); the rewrite as `heads::rewrite_retirements` (D:dagda/heads/retirement.h:59-64) | DIFFERS: the commit is the owner's provider code. Dagda hands `matured_batches` from the inventory (collector.cpp:90-91) without Macha's filter against the converged head (M:225-232); Macha's provider must keep that filter. The heads helper is a free function in `dagda::heads`, not on Node or Context | M: tests/test_namespace_service.cpp:421 (wt) `test_a_stale_tombstone_in_a_batch_is_erased_once` / D: `HeadsRetirement.RewritingDropsABatchAndKeepsTheRestOfIt` (heads_retirement_test.cpp:123), `NodeHeads.AStaleRetirementInABatchIsErasedOnce` (heads_node_test.cpp:625), `StepsBed.AFailedTombstoneCommitEndsThePass` (:1152) |
| 7 | `ObjectLedger::release_unreferenced` -> `RetentionStore::release_unreferenced` (M:maintenance.cpp:950-956, 992-1005; M:src/storage/retention.cpp:544-591, apply at :292-322) | Budget 64 per class; a stretch of max(64*128, 8192) claims scanned under the claims mutex from a persisted cursor; only claims not live in the release horizon and whose add the clock dominates; one journal frame; `retention.released.{control,data}`; trace `release.control N` / `release.data N`; debug `retention released DATA claims node=xxxxxx count=N`; once `retention release skipped: horizon incomplete` | steps: control_step D:collector.cpp:190-194, data_step :229-237; D:dagda/ledger/retention.cpp:391-417, apply :170-198 | SAME (budget `CollectorBudgets::release_budget` 64, collector.h:44) | M: tests/test_claim_walk.cpp:194 (wt) `test_retention_ledger_forwards_claims_releases_prunes_and_compacts`, tests/test_catalogue_maintenance.cpp:542 (wt) `test_data_is_released_only_when_nothing_refers_to_it` / D: `LedgerClaimWalk.TheRetentionLedgerForwardsClaimsReleasesPrunesAndCompacts` (ledger_claim_walk_test.cpp:199), `LedgerCollector.AClaimFromAnUnseenBranchSurvivesTheSweeps` (:277) |
| 8 | `CatalogueManager::control_gc_step` (M:catalogue.cpp:2654-2737) | Budget 32 from a persisted cursor; live -> forget sighting and epoch; claimed -> forget; sighting must mature for the grace; an object touched since the root epoch began stays and is re-stamped with the epoch; an unreferenced object survives the epoch it was first seen in; removal under the catalogue mutex so it never races a new root; claim deletion then `remove_if_older_than(grace)`; `pass_complete()` on exhaustion; `compact_packs()` on the CONTROL store when a full pass removed something; `catalogue.control_gc.removed`; trace `control-gc N`; debug `catalogue control GC removed=N` | steps: `Collector::control_step` (D:collector.cpp:177-205) -> `Collector::control_gc_step` (:295-352); epoch from `provider.control_epoch()` (:302), removal inside `provider.within_control_epoch(sequence, ...)` (:327, :333) | DIFFERS, three ways. (a) The epoch and its lock are the provider's (reachability.h:54-64, 104-112): Macha's provider must expose `control_gc_root_epoch_`, its sequence and `mutex_`. (b) The epoch's age is read on the pass's time source (D:collector.cpp:304, `CollectorServices::time` = `pass_time()`, node.cpp:482) and compared against file ages on the store's wall clock (D:dagda/store/local_store.cpp:1326-1339); Macha reads both on real clocks (M:catalogue.cpp:2673-2675). Same in production; differs under a stepped `MaintenanceOptions::time`. (c) The epoch map and cursor live in the Collector (collector.h:248-250), which `register_reachability`/`unregister_reachability` destroy (node.cpp:455-456, 463-464); Macha's live for the process in `CatalogueManager` | M: tests/test_hydration_catalogue.cpp:4955-5002 (inside `test_catalogue_sync_search_and_artwork_gc`), tests/test_namespace_service.cpp:738 (wt) / D: `LedgerCollector.AControlObjectStagedInTheCurrentEpochIsKept` (:357), `LedgerCollector.AControlRemovalNeverRacesANewEpoch` (:381), `LedgerCollector.TheControlSweepReleasesRemovesAndPrunes` (:421) |
| 9 | `ledger_.claim_deletion(RetentionClass::control, id)` (M:catalogue.cpp:2721), the caller this map was asked about | Under the id's stripe: refuse if a CONTROL claim stands, else mark the id; the stripe released before the unlink; a claim arriving meanwhile finds the mark and treats the object as not present (M:src/cluster/storage_server.cpp:381-389) | D:collector.cpp:339 `_data.claim_locks().deletion(id, [&]{ return _ledger.retained(control, id); })` | SAME. Dagda's guard sits on the DATA pool (storage_pool.h:279) but is the node's one guard for both classes: every claim path checks it for CONTROL too (D:dagda/store/storage_server.cpp:343-349; D:dagda/store/distributed_store.cpp:2189-2195), as Macha's one `ClaimStore` guard does | M: tests/test_storage_metadata.cpp:4958 `test_a_delete_marks_its_object_and_holds_no_stripe` / D: `StoreClaimLocks.APeersClaimOnAControlObjectAboutToGoIsRefused` (store_distributed_store_test.cpp:1255), `StoreClaimLocks.ALocalClaimOnAControlObjectAboutToGoIsRefused` (:1284), `StoreClaimLocks.AControlClaimInProgressHoldsTheSweepsCheckBack` (:1308) |
| 10 | `ClaimLocks` / `ClaimDeletion` / `ClaimStore::deletion` (M:src/contract/claim_store.hpp:42-190) and `ObjectLedger::claim_deletion` (M:src/contract/object_ledger.hpp:60-67; M:src/ledger/retention_ledger.hpp:41-43) | 64 stripes by first byte; batches take stripes in ascending order; a multiset of marks so two deletes of one id each hold one; mark released when the leave goes | D:dagda/store/object_lock.h:62-189, reached as `Node::data().claim_locks()` (storage_pool.h:279) | DIFFERS in placement only: Dagda's `contract::ObjectLedger` has no `claim_deletion` (Macha's two callers, M:catalogue.cpp:2721 and M:maintenance.cpp:1044, both move into Dagda, rows 9 and 11); `ClaimDeletion` has no move-assignment (object_lock.h:70), which Macha's callers did not use | M: tests/test_claim_walk.cpp:42 `test_a_delete_mark_lasts_until_the_last_delete_of_the_id_ends`, tests/test_storage_metadata.cpp:4958 / D: `StoreClaimLocks.ADeleteMarksItsObjectAndHoldsNoStripe` (:1151, the two-deletes case at :1174-1182), `StoreClaimLocks.AClaimFindsAMarkedObjectNotPresent` (:1186) |
| 11 | `StoragePool::gc_step` (M:storage_pool.cpp:1037-1105) | Live, protected or claimed -> forget sighting; sighting not matured -> `deferred`; claim deletion just before the delete; `remove_if_older_than(orphan_grace)` (a reaffirming put refreshes age); bytes counted; `pass_complete()` on the sweep's end; yields between objects; a throw is logged `garbage collection skipped local object ...` and the sweep goes on | D:dagda/store/storage_pool.cpp:1129-1184, from data_step (collector.cpp:249-251) | DIFFERS: on the slice that completes a pass Dagda reports `deferred` for the whole pass (D:storage_pool.cpp:1143-1147, `_gc_pass_deferred`), Macha only for that slice (M:storage_pool.cpp:1053-1057). Through Macha's loop (M:maintenance.cpp:1081-1090) a pass whose deferral fell in an earlier slice now rests `orphan_grace` instead of "waiting for an event". The claim deletion is always taken inside (D:1167); Macha takes it only when given (M:1079-1086), which its pass always does | M: tests/test_storage_metadata.cpp:505 `test_retention_claim_is_physical_gc_barrier`, the slice checks at tests/test_storage_metadata.cpp:3958-3989 / D: `LedgerCollector.ARetentionClaimIsAPhysicalGcBarrier` (:128), `StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance` (store_storage_pool_test.cpp:288), `StoreStoragePool.TheSliceThatCompletesASweepReportsEveryDeferralInItsPass` (:679) |
| 12 | DATA sweep stage (M:maintenance.cpp:974-1078) | Gate (row 1); skip reason logged at debug only when it changes (`garbage collection sweep node=xxxxxx enabled|skipped: reason`); protected = retirements not matured, sorted unique; `orphan_grace = max(garbage_grace, no_progress_backoff)`; budget 64; yields while viewer or loader work is within `foreground_quiet`; `maintenance.gc.{step_us,examined,reclaimed_bytes,per_object_ns}`; slow-stage line `garbage-collect`; debug `garbage collection sweep node= objects= bytes= deferred= yielded= complete= live= protected=`; `garbage collection reclaimed N local bytes`; trace `gc reclaimed_bytes=N`; then prune (row 14) | steps: `Collector::data_step` (D:collector.cpp:207-273); `orphan_grace` from `CollectorBudgets::retry_window` 300000 ms (collector.h:41) and the domain's wait (node.cpp:479); yield is the caller's `PassDue::should_yield` | SAME (lines :218-222, :226, :248-268; order of the two debug lines after prune differs from Macha's, :268 before :269 vs M:1072 before M:1076, same text) | M: test_maintenance_trace fixtures / D: `StepsBed.APassRecordsMachasObservations` (:1055), `StepsBed.TheSweepUsesTheNodesOwnSightings` (:1037), `LedgerCollector.AnInventoryBehindOrIncompleteSweepsNothing` (:214), `NodeMaintenance.TheDomainsGraceDecidesWhenAnUnreferencedObjectGoes` (node_maintenance_test.cpp:109) |
| 13 | Scheduling after the sweep (M:maintenance.cpp:1079-1091): yielded -> next interval; complete and deferred -> rest `orphan_grace`; complete, nothing deferred -> `gc_quiescent_until = max` ("waiting for an event"), and `gc_waiting_for_event` into the next pass's gate reasons (M:877) | A settled collector costs nothing until an event | steps: the caller's, from `DataStep::swept` and `DataStep::orphan_grace` (collector.h:137-143). loop: D:node.cpp:586-587, 966-969: settled (complete, nothing deferred, not yielded, not rebuilt) rests `gc_interval` (300 s, node.h:103), anything else runs again after `interval` (1 s); `gc_waiting_for_event` is never set (node.cpp:551-555) | DIFFERS (loop): Dagda's loop never sleeps on an event, runs a slice every second for as long as anything is deferred, and starts a full new sweep every 300 s once settled (section 2, P1, P2). Steps: SAME | M: tests/test_maintenance_trace.cpp:486 (wt) `test_trace_quiet_node` / D: steps: `MaintenanceSteps.TheQuietNodeFixture` (:699), test loop at maintenance_steps_test.cpp:534-535; loop: `NodeMaintenance.TheNodesThreadRunsThePassesOnItsOwn` (:166) pins only that passes run |
| 14 | `ObjectLedger::prune_unclaimed` -> `RetentionStore::prune_unclaimed` (M:maintenance.cpp:966-969, 1072-1075; M:retention.cpp:605-646) | Budget 64; picks unclaimed records under the lock, asks `held` without it, journals and erases only what is still unclaimed; cursor reset when empty; `retention.pruned.{control,data}`; trace `prune.control N` / `prune.data N` | D:collector.cpp:200-202, 269-271; D:retention.cpp:446-474; `held` D:retention_ledger.cpp:43, 51-53 | SAME | M: tests/test_storage_metadata.cpp:194 (wt) `test_retention_prune_cursor_cannot_starve_later_tombstones`, :367 (wt) `test_retention_prune_survives_a_restart` / D: `LedgerRetention.ThePruneCursorCannotStarveLaterTombstones` (ledger_retention_test.cpp:209), `LedgerRetention.APruneSurvivesARestart` (:353), `LedgerRetention.PruneKeepsARowClaimedWhileItChecksPresence` (:241) |
| 15 | `UnreferencedSince` (M:src/storage/unreferenced_since.cpp:45-118), files `<state>/retention/unreferenced-{data,control}.bin` (M:src/service/node_services.cpp:47-48) | First sighting on this node's clock; a clock set back restarts the wait; `forget` when referenced or gone; `pass_complete` drops what a full sweep did not renew; loaded sightings count as renewed once; save only when dirty; a damaged file is ignored (only delays deletion) | D:dagda/ledger/unreferenced_since.cpp:74-141, same paths (node.cpp:361-364), reached as `Node::data_sightings()` / `control_sightings()` (node.h:411-413) | DIFFERS on disk only: one AES-256-GCM frame under the node-local key (D:unreferenced_since.cpp:58-72, 127; aad `dagda/sightings/1`, .h:37); `dagda-migrate` seals Macha's plain files (D:dagda/migrate/migrate.cpp:566-567, 583-584). Logic line for line the same (:88-117 vs M:59-91) | M: tests/test_absent_nodes.cpp:29, 43, 53, 64, 80, 109 / D: `LedgerUnreferencedSince.*` (ledger_unreferenced_since_test.cpp:52-179), `MigrateState.ThePlainSightingsFilesAreSealed` (migrate_migrate_test.cpp:284) |
| 16 | `Maintenance::save_sightings` (M:maintenance.cpp:139-146) | Written when a sweep completes, else at most every 30 s; CONTROL always passes `complete=false` (M:962) | D:collector.cpp:64-69, called :197 (false) and :252 (`swept.complete`) | SAME (Dagda's 30 s is on the pass's time source, node.cpp:482) | M: none found / D: `StepsBed.TheSweepUsesTheNodesOwnSightings` (:1037) |
| 17 | Earliest-retirement wake (M:maintenance.cpp:1243-1274) | After a GC-due pass that was not busy, one exact wake at the earliest immature retirement's deadline, so a protected object does not leave the sweep looking quiescent forever | steps: the caller's, from `Node::ledger().inventory()->garbage()` (node.h:365). loop: none (D:node.cpp:943-1001) | MISSING in Dagda's loop (steps: Macha keeps its own code until 8c) | M: tests/test_filesystem_fuse.cpp:3919 `test_coalesced_delete_burst_wakes_at_exact_garbage_grace` / D: only the test's own copy of Macha's loop, maintenance_steps_test.cpp:575-590; nothing on `Node::maintenance_loop` |
| 18 | GC quiet windows (M:maintenance.cpp:348-354 local metadata generation moved; :355-362 any of the four events; :652 an inventory rebuilt) | "Object arrival and namespace publication are separate durable operations. Start an exact quiet window so a sweep cannot race the retention claim which makes newly-arrived bytes reachable" (M:358-360); a rebuilt inventory is consumed only after `foreground_quiet` | steps: the caller's, from `Node::maintenance_events()` (node.h:452) and `InventoryStep::rebuilt`. loop: `gc_due = !busy` only (D:node.cpp:550-553); a rebuilt inventory is used on the next pass after `interval` (1 s) < `foreground_quiet` (2 s, D:dagda/store/repair.h:47) | MISSING in Dagda's loop. The backstops remain (orphan grace >= 300 s, the claim check and mark), so this is defence in depth, but Macha's rule is gone | M: test_maintenance_trace fixtures / D: steps: `StepsBed.ABusyPassWakesWhenTheQuietWindowEnds` (:1282) on the test loop; loop: none |
| 19 | `garbage_due = !busy && !metadata_dirty` (M:maintenance.cpp:575, dirty from `metadata_convergence.pending()` at :343) | Tombstones are not collected while metadata convergence is pending | steps: the caller's `PassDue::garbage_due`. loop: `due.garbage_due = !busy` (D:node.cpp:552) | DIFFERS (loop): Dagda's loop commits erasures while convergence is pending. Steps: SAME | M: test_maintenance_trace fixtures / D: none |
| 20 | Claims compaction (M:maintenance.cpp:1125-1131) | Only when not busy, past 4096 frames or the byte bound; snapshot before truncate | `Node::retention_compact(turn)` (D:node.h:454; node.cpp:708-718) -> D:retention.cpp:439-444 (M:retention.cpp:597-603) | SAME | M: tests/test_claim_walk.cpp:194 (wt) / D: `NodeSeams.AStopInsideTheClaimsCompactionWaitsForItsEndThenComesThrough` (node_seams_test.cpp:608), `LedgerClaimWalk.TheRetentionLedgerForwardsClaimsReleasesPrunesAndCompacts` (:199) |
| 21 | `LocalStore::remove_if_older_than` (M:src/storage/local_store.cpp:1574-1596) | Atomic age check and remove under the object lock; packed records by touch time, loose files by mtime; a deletion is lazily durable (ledger flush) | D:dagda/store/local_store.cpp:1197-1217 | SAME (age read on the store's `NodeOptions::wall`, D:1204, 1211) | M: tests/test_invariants.cpp:2844 `test_authoritative_delete_is_lazy_durability` / D: `StoreLocalStoreDurability.AnAuthoritativeDeleteIsLazilyDurable` (store_local_store_durability_test.cpp:288) |
| 22 | `delete_object` handler (M:storage_server.cpp:393-410) | Claim deletion first (409 `claimed`), then DATA admission (503 `storage_busy`), remove, cache remove | D:storage_server.cpp:281-294 | DIFFERS: admission first (D:283-284), then the claim check (D:287-288); the mark ends before the cache remove (D:290-291). No Macha code sends this message (row 23) | M: none found / D: `StoreRepair.ADeleteAskedOfASilentPeerEndsAtTheNoProgressWindow` (store_repair_test.cpp:1768) |
| 23 | `DistributedStore::erase_all` (M:src/cluster/distributed_store.cpp:2297-2321) | none: no caller in Macha's src or tests (a whole-repository search finds only the definition and M:distributed_store.hpp:464) | D:dagda/store/distributed_store.cpp:2649-2668 (not on Context) | SAME (unused by Macha; deleted objects leave every node only by each node's own collector, on both sides) | M: none / D: `StoreRepair.ADeleteAskedOfASilentPeerEndsAtTheNoProgressWindow` (:1768) |
| 24 | The claim path's half of the guard (M:storage_server.cpp:373-391; M:distributed_store.cpp:801-808, 1053-1055) | Every stripe of the whole batch (up to 1,000,000 ids, M:storage_server.cpp:363) held across the presence checks and the journal write; all-or-nothing: one absent id refuses the batch before anything is claimed | `Context::Claims::retain` (D:dagda/context.h:244-247) -> D:distributed_store.cpp:611-626, 2187-2195; server D:storage_server.cpp:341-350; `ClaimLocks::in_batches` (object_lock.h:138-161) | DIFFERS: sub-batches of 65,536 ids, each under its own stripes; a refusal in a later sub-batch leaves the earlier ones claimed (object_lock.h:145-146 says why that is safe) | M: tests/test_storage_metadata.cpp:4958 / D: `StoreClaimLocks.ABatchHoldsOnlyPartOfTheGuardAtATime` (:1079) |
| 25 | Record tombstone sweep (M:src/torrent/torrent_coordinator.cpp:1175-1187) | Erase strictly past the grace; a stamp from a clock ahead is fresh, not expired; one sweeper (lowest node id) | `lease::ClaimedCollection::expired` (D:dagda/lease/claimed_collection.h:158-170) | SAME rule (the sweeper choice stays Macha's). The template is in `dagda::lease`, not on Node or Context | M: none found / D: `LeaseClaimedCollection.ATombstoneExpiresStrictlyPastItsRetention` (lease_claimed_collection_test.cpp:486), `LeaseClaimedCollection.ATombstoneFromAClockAheadIsKeptItsWindow` (:501) |
| 26 | `retire_objects`, `encode/decode_tombstone_batch`, `tombstones_of` (M:src/metadata/metadata.cpp:565-649) | One pending batch per mutation, stamped at commit; batch layout magic, i64 retired_at, u32 count, sorted unique ids; latest retirement per object, batch id as token, unreadable batches counted | `heads::encode_retirement_batch`, `decode_retirement_batch`, `write_retirement_batch`, `retirements_of` (D:dagda/heads/retirement.h:39-54) | DIFFERS: free functions in `dagda::heads`, not on Node or Context; used only once Macha's metadata is on heads (8c). Layout "Macha's tombstone batches at 9a08846" (retirement.h:23) | M: tests/test_storage_metadata.cpp:4776 (wt), tests/test_namespace_service.cpp:336 (wt) / D: `HeadsRetirement.*` (heads_retirement_test.cpp:39-123), `NodeHeads.RetirementsLiveInBatchesInATreeBackedHead` (heads_node_test.cpp:545), `HeadsTreeState.ARealRetirementBatchOpensAndIsMadeAgain` (heads_tree_vectors_test.cpp:506) |
| 27 | Observations and log lines of the stages (rows 2-14) | Names `maintenance.*`, `retention.*`, `catalogue.control_gc.removed`; Macha's debug and trace words | `CollectorServices::observations` = `context.observations()`, logger `dagda.gc` (D:node.cpp:482) | SAME names and words (whether Dannan's scoped logger renders the `dagda.gc` scope where Macha's global `Log` printed none is unverified) | D: `StepsBed.APassRecordsMachasObservations` (:1055), `NodeSeams.ASlowStageIsLoggedInMachasWords` (:267) |
| 28 | What operators read: Macha has no GC block in its status JSON; `metadata.tombstones` (M:src/api/status_api.cpp:969) is the owner's count | none beyond the observations | `dagda.gc.{provider,grace_ms,passes,skipped,failures,inventories,released_data,released_control,removed_control,examined,reclaimed_bytes,pruned,state,data_gate}` (D:node.cpp:1226-1241), `dagda.maintenance.retention_compactions` (:1214) | DIFFERS (additive). `dagda.gc.*` "count the node's own passes, not an owner's steps" (node.h:407-408): until 8c they stay at zero on a Macha node | D: `NodeMaintenance.WithNoProviderTheCollectorRemovesNothingAndSaysSo` (:82) |
| 29 | The decision trace (`maintenance_trace_`, M:maintenance.cpp:307-314) | Gate verdicts and actions in Macha's kinds and words, one hook | `MaintenanceOptions::trace` (D:node.h:127-131) into the Collector (node.cpp:483) | SAME but for row 5's `tombstones stamp=` | D: `NodeSeams.AnOwnersPassDrivesTheNodesCollectorToItsTrace` (node_seams_test.cpp:205), every `MaintenanceSteps.*Fixture` |

Counts: 29 rows; SAME 15 (1, 2, 3, 4, 7, 9, 12, 14, 16, 20, 21, 23, 25, 27, 29), DIFFERS 12
(5, 6, 8, 10, 11, 13, 15, 19, 22, 24, 26, 28), MISSING 2 (17, 18).

Not a Macha operation, recorded for completeness: the instant delete,
`Context::Objects::remove` (D:context.h:201-218) -> `store::remove_here` (D:dagda/store/remove.cpp:59-97).
Macha has no counterpart and no caller needs one: a Macha file's bytes leave by retirement, release
against the head and the sweep after the grace, on Dagda as on Macha.

## 2. Performance-relevant differences

- **P1. Dagda's loop sweeps for the whole grace once anything is deferred.** `gc_pass_locked`
  is "settled" only when the sweep completed with nothing deferred (D:node.cpp:586-587);
  otherwise the next GC pass is one `interval` later (D:node.cpp:969). A sighting inside its
  grace (30 days by default) is a deferral, so after any delete the loop runs a 64-object
  DATA slice and a 32-object CONTROL slice every second, with `generation()`,
  `known_generation()` twice, `release_head()`, `control_epoch()` and `collect_garbage()`
  asked of the provider each time, for 30 days. Macha's pass rests `orphan_grace` after a
  complete deferred sweep (M:maintenance.cpp:1082-1085). Loop only (8c).
- **P2. Dagda's loop never rests on an event.** Settled, it rests `gc_interval` (300 s) and
  then starts again; a slice completes a pass only at the end of the pool, so every pass
  until then is not settled and runs at `interval`. A settled pool of N objects is swept
  every 300 s + N/64 s, forever: about 4.3 hours of continuous sweeping per million objects
  between 5-minute rests. Macha sleeps until one of its four events (M:maintenance.cpp:1086-1089).
  Loop only (8c).
- **P3. Pass-wide deferral (row 11).** Through Macha's own loop, a sweep whose deferral fell
  in an earlier slice now rests `orphan_grace` and sweeps again then, where Macha waited for
  an event: at most one extra full sweep per grace, and sighting-deferred garbage in a quiet
  cluster is freed at its grace instead of at the next event. Space is held shorter.
- **P4. Claim batches hold less of the guard (row 24).** A sweep's or delete's mark waits
  behind at most a 65,536-id sub-batch's journal write (object_lock.h:138-161), not a whole
  publication's (Macha's holds every stripe for up to 1,000,000 ids, M:storage_server.cpp:363,
  381-390). Lower delete latency under large publications; no extra journal syncs (one frame
  per 65,536 ids on both sides).
- **P5. CONTROL removal goes through the provider.** Each unreferenced CONTROL object costs
  one or two `within_control_epoch` calls (a virtual call and a `std::function`,
  D:collector.cpp:327, 333) where Macha took its own mutex (M:catalogue.cpp:2700, 2708). The
  lock held across the CONTROL unlink is the provider's, as Macha's was the catalogue's.
  Negligible.
- **P6. Re-registering the provider restarts the CONTROL sweep's state** (row 8c): the
  epoch map and the CONTROL cursor go with the Collector (collector.h:248-250; node.cpp:455-456),
  so every unreferenced CONTROL object waits one more epoch and the sweep starts from the
  front. Macha keeps both for the process. Only matters if Macha re-registers (unverified).
- **P7. Sightings are sealed.** Each save is an AES-256-GCM seal of 40 bytes per entry
  (D:unreferenced_since.cpp:58-66, 127), at most twice a minute; each load one open.
  Negligible.
- **P8. `delete_object` admits before it checks the claim** (row 22): a claimed object costs
  an admission slot. No Macha caller sends it.
- **Unchanged:** release scans max(budget*128, 8192) claims under the claims mutex per call
  (both, retention.cpp); prune runs `held` outside the lock (both); the inventory is rebuilt
  only when the generation moves (Macha's `maintenance_objects_cached`,
  M:src/ledger/node_horizon_builder.cpp:92-93; Dagda's `provider.generation()`, collector.cpp:118);
  budgets 64/64/64/32 (collector.h:44-47 = M:maintenance.cpp:952, 960, 966, 994, 1032, 1072);
  orphan grace and the sightings grace identical, so no object is held longer or shorter in
  step mode except by P3.

## 3. Gaps for Dagda to fix

1. **Dagda's loop has no GC quiet window after an event, a generation move or a rebuilt
   inventory** (row 18). Macha needs a sweep never to run within `foreground_quiet` of object
   arrival, publication or a new inventory (M:maintenance.cpp:348-362, 652). Dagda's loop
   decides `gc_due = !busy` only (D:node.cpp:550-553) and uses a rebuilt inventory one
   `interval` later. Test: `NodeMaintenance.AGcPassWaitsTheQuietWindowAfterAStorageEvent`.
   Severity: blocks the port at 8c.
2. **Dagda's loop does not rest when deferred or settled** (row 13, P1, P2). Macha needs the
   collector idle between events and resting `orphan_grace` after a deferred sweep
   (M:maintenance.cpp:1079-1091). Dagda reruns every `interval` while deferred or unfinished
   and every `gc_interval` when settled (D:node.cpp:586-587, 966-969), and never sets
   `gc_waiting_for_event`. Tests: `NodeMaintenance.ASettledCollectorSleepsUntilAnEvent`,
   `NodeMaintenance.ADeferredSweepRestsItsOrphanGrace`. Severity: loses performance (8c).
3. **Dagda's loop has no earliest-retirement wake** (row 17). Macha needs one exact wake at
   the earliest immature retirement's deadline (M:maintenance.cpp:1243-1274); once gap 2 is
   fixed, without it a matured retirement waits for the next event. Dagda: none in
   D:node.cpp:943-1001. Test: `NodeMaintenance.AnImmatureRetirementWakesTheCollectorAtItsDeadline`.
   Severity: loses performance (8c; space held longer).
4. **Dagda's loop collects tombstones while metadata convergence is pending** (row 19).
   Macha needs `garbage_due = !busy && !metadata_dirty` (M:maintenance.cpp:575). Dagda sets
   `garbage_due = !busy` (D:node.cpp:552). Test:
   `NodeMaintenance.TombstonesWaitWhileHeadConvergenceIsPending`. Severity: blocks the port
   at 8c (a gate input Macha relies on is gone).
5. **`tombstones stamp=[...]` and the `stamp` decision Macha never makes** (row 5). Macha's
   trace has no such line (M:maintenance.cpp:156-160, 881-906); Dagda writes it for any zero
   retirement (D:collector.cpp:80, 95). Test:
   `MaintenanceSteps.AnUnstampedRetirementTracesAsMachasDoes` (a fixture holding one zero
   retirement, expecting Macha's lines only). Severity: cosmetic, unless a Macha fixture or
   legacy head carries a zero retirement, then it breaks trace parity.
6. **The CONTROL epoch's age and the object's age are read on different clocks** (row 8b).
   Macha reads both in real time (M:catalogue.cpp:2673-2675, 2699). Dagda reads the epoch on
   `MaintenanceOptions::time` (D:collector.cpp:304) and the object on the store's wall
   (D:local_store.cpp:1326-1339). Test:
   `LedgerCollector.AControlObjectTouchedSinceTheEpochIsKeptUnderASteppedClock`. Severity:
   cosmetic (tests with a stepped pass clock; production reads real time on both).
7. **Re-registering the provider loses the CONTROL sweep's epochs and cursor** (row 8c, P6).
   Macha needs them for the process. Dagda resets the Collector (D:node.cpp:455-456, 463-464).
   Test: `NodeSeams.RegisteringTheProviderAgainKeepsTheControlSweepsEpochs`. Severity: loses
   performance (one extra epoch per CONTROL orphan), only if Macha re-registers.
8. **Retirement and record-tombstone helpers are outside Node and Context** (rows 6, 25, 26).
   Tom's rule is that Macha calls Dagda only through `dagda::Node` and `dagda::Context`; the
   retirement batch codec and rewrite (D:heads/retirement.h:39-64) and
   `lease::ClaimedCollection` (D:lease/claimed_collection.h) are free functions and a
   template. Either the rule admits these as library helpers or Macha keeps its own copies
   (rule 3 of the stack, no duplication, says the former). Test (fails to compile today):
   `ContextHeads.ARetirementIsWrittenAndReadThroughTheContext`. Severity: blocks the port at 8c
   if the rule is read strictly; a question for Tom.
9. **An instant delete on a node with no inventory published deletes unclaimed copies**
   (not a Macha path). `remove_here` keeps a copy when the inventory is incomplete or refers to
   it, but a null inventory lets it go (D:remove.cpp:75, 83), though "one that could not be
   read whole may refer to anything" (:73-74). Test:
   `StoreRemove.ANodeWithNoInventoryPublishedKeepsItsCopy`. Severity: cosmetic for Macha's port
   (Macha does not call `Objects::remove`); a safety question for Lugh and Lir. **Ruled (Tom, 2026-11-16, after
   Lugh and Lir answered): the node keeps the copy.** Not built yet.

## 4. Not mapped

- Macha's provider side, which stays Macha's: `CatalogueManager::maintenance_head` and
  `maintenance_objects` (M:catalogue.cpp, the head's tail read at :2560-2572, the objects at
  :2574-2652, including metadata conflict alternatives as
  GC roots at :2579-2581, `test_metadata_conflict_alternatives_are_gc_roots`) and Macha's
  `NodeHorizonBuilder` (M:src/ledger/node_horizon_builder.cpp). Their answers feed rows 2, 3
  and 8; whether Macha's provider reproduces them exactly is the provider's map, not this one.
- Other deletions that are not GC: repair's surplus-copy drops (`repair.local_drops`,
  D:node.cpp:1176), the scrub's removal of a record that fails to open (M:storage_pool.cpp:
  1018-1027), rebalance's removal of a moved duplicate (M:storage_pool.cpp:973-985), cache
  eviction, DATA and CONTROL pack compaction (`compact_packs`), heads history truncation.
  These belong to the local-stages and repair sections.
- The filesystem's unlink-to-retirement path and its cached maintenance inventory
  (`test_unlink_retires_an_object_from_the_maintenance_inventory`,
  `test_a_delete_is_reclaimed_while_a_node_is_away`), REWRITE rows in
  TODO/2026-11-05-test-mapping.md:727, 735; Macha-side.
- `dagda-migrate`'s sealing of the sightings files was checked only by the file list and its
  test name (migrate.cpp:566-567, 583-584), not read through.
- Macha test lines marked "(wt)" were not re-read at `f0597a4`; no Macha test was found for
  the `delete_object` handler, `save_sightings`, or the torrent tombstone sweep.
- The Macha session's `build/store-api-map.md` was not consulted.
