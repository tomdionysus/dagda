# Macha store map: retention, claims, reference counts and the ledgers under them

Section of the call-by-call map from Macha's storage internals to Dagda. Macha is
read at develop f0597a4 (`build/macha-f0597a4/`, written `M:` below); Dagda at this
repository's develop, 59b20e9 (written `D:`). Every file:line was read; anything not
read is marked unverified. Written 2026-11-14.

Scope: the retention store (claims by dot and class, release, prune, compaction), the
claim guard, the retention ledger (ObjectLedger), reference counts, the held ledger, the
object trie, trie diff and union, unreferenced-since sightings, the horizons and their
builder, the claim walk, and the claim calls Macha's owner code makes through the
distributed store (the publication barrier). The parts of Macha's maintenance pass that
drive these are mapped against Dagda's Collector, since that is where the calls now land.

Known, deliberate differences (Tom's rulings) are stated where they touch a row, not
argued: layout v19; every ledger a node keeps of its own sealed under the node-local key
(ObjectTrie, ReferenceCounts, RetentionStore take a `StorageKey`; sightings sealed with
aad `dagda/sightings/1`); Dannan's logging and time injected; Macha's pre-ledger claim
checkpoints (MRTS0001/0002) not read.

How Macha reaches each thing after the port: `Node::claims()` (D:dagda/node.h:364),
`Node::ledger()` (:365), `Node::references()` (:366), `Node::store()` (:367),
`Node::data().claim_locks()` (D:dagda/store/storage_pool.h:279),
`Node::collector()` (node.h:410), `Node::data_sightings()` / `control_sightings()`
(:412-413), and the Context nouns `Context::Claims` (D:dagda/context.h:236-256).
Everything Macha's callers use is reachable; the only Context noun for claims is
`Claims::retain` / `Claims::retained`, the rest is through Node's part accessors.

## 1. The table

Verdicts: SAME (callers see the same behaviour and cost), DIFFERS (exactly how, in the
row), MISSING (no Dagda call does it).

### 1.1 The retention store (ClaimStore)

| # | Macha (name, file:line) | What callers rely on | Dagda through Node/Context | Verdict | Tests (M / D) |
|---|---|---|---|---|---|
| 1 | `RetentionStore::retain`, `retain_batch` (M:src/storage/retention.cpp:462-491; contract M:src/contract/claim_store.hpp:196-201) | Sorted-unique ids; one sealed, fsynced claims.log frame per 65,536 ids (:28, :475-490) applied to the class trie under the store mutex; observed-remove: an add at or below the origin's remove context is ignored (:279-284); checkpoint if 8,192 objects changed or log >= 4 MiB (:33-34, :348-351). Invalid dot (no origin or sequence 0) throws `std::runtime_error` (:470-471). Thread-safe; waits on the state device. | `Node::claims().retain/retain_batch` (D:dagda/ledger/retention.cpp:328-343): same frame, same constants (:33-39), same apply (:151-168). | DIFFERS: an invalid dot throws `std::invalid_argument` (D:retention.cpp:332; contract D:dagda/contract/claim_store.h:55-56), not `std::runtime_error`; a `catch (const std::runtime_error&)` no longer catches it. Everything else the same. | M: test_retention_claims_are_causal_durable_and_observed_remove (tests/test_storage_metadata.cpp:134), test_retention_checkpoints_after_enough_changes (:301). D: LedgerRetention.ClaimsAreCausalDurableAndObservedRemove (dagda/test/ledger_retention_test.cpp:85), CheckpointsAfterEnoughChanges (:302), JournalsMachasFrames (:608) |
| 2 | `retained(class, id)` (M:retention.cpp:493-497) | True when the record's add count is non-zero; one trie lookup under the store mutex (may read a node from disk). | `Node::claims().retained` (D:retention.cpp:345-349); `Context::Claims::retained` (D:dagda/context.cpp:213). | SAME | M: as row 1. D: as row 1 |
| 3 | `claims(class, id)` diagnostic (M:retention.cpp:499-506) | The object's adds and removes by origin; empty for none. | `Node::claims().claims` (D:retention.cpp:351-357). | SAME | M: test_retention_start_checkpoints_a_long_journal (:321). D: LedgerRetention.AStartCheckpointsALongJournal (:320) |
| 4 | `next_retained(class, cursor, complete)` (M:retention.cpp:523-542) | Next claimed id after the cursor, pages of 256 under the mutex; at the end `complete` set and cursor reset. | `Node::claims().next_retained` (D:retention.cpp:372-389). | SAME | M: test_retention_ledger_pages_every_live_claim_once (tests/test_claim_walk.cpp:59). D: LedgerClaimWalk.TheRetentionLedgerPagesEveryLiveClaimOnce (dagda/test/ledger_claim_walk_test.cpp:79) |
| 5 | `retained_ids`, `claim_objects` (M:retention.cpp:508-521, 593-595) | Linear in the class's records, under the mutex the whole walk. No Macha caller outside the store and tests. | `Node::claims().retained_ids/claim_objects` (D:retention.cpp:359-370, 437). | SAME | M: test_retention_claims_checkpoint_into_the_ledger_and_restart (:261). D: LedgerRetention.ClaimsCheckpointIntoTheLedgerAndRestart (:265) |
| 6 | `release_unreferenced(class, live, observed, budget)` (M:retention.cpp:544-591) | Release against a complete horizon's clock: zero entries dropped from the clock; scans at most max(budget x 128, 8,192) records from the kept cursor, wrapping once; up to `budget` candidates; one release frame; only adds the clock dominates go, remove contexts raised (:292-322). Single owner. | `Node::claims().release_unreferenced` (D:retention.cpp:391-417), same window and rule (:170-198, :407). | SAME | M: test_retention_claims_are_causal_durable_and_observed_remove. D: LedgerRetention.ClaimsAreCausalDurableAndObservedRemove, AClaimFromAnUnseenBranchSurvivesAPartition (:140) |
| 7 | `prune_unclaimed(class, exists, budget)` (M:retention.cpp:605-646) | Three steps so `exists` runs without the mutex; only rows still unclaimed are erased; one prune frame; cursor kept so a small budget cannot starve later rows. | `Node::claims().prune_unclaimed` (D:retention.cpp:446-474). | SAME | M: test_retention_prune_cursor_cannot_starve_later_tombstones (:194), test_retention_prune_keeps_a_row_claimed_while_it_checks_presence (:230), test_retention_prune_survives_a_restart (:367). D: LedgerRetention.ThePruneCursorCannotStarveLaterTombstones (:209), PruneKeepsARowClaimedWhileItChecksPresence (:241), APruneSurvivesARestart (:353) |
| 8 | `compact_if_needed(threshold)` (M:retention.cpp:597-603) | Checkpoints both tries and empties claims.log once it holds `threshold` frames or 64 MiB; removes the reserved pre-ledger files. | `Node::claims().compact_if_needed` (D:retention.cpp:439-444); the pass's stage is `Node::retention_compact` (D:dagda/node.cpp:708-718), skipped when busy, threshold 4096, as M:src/service/maintenance.cpp:1128-1131. | SAME | M: test_retention_claims_are_causal_durable_and_observed_remove. D: NodeLocalMaintenance.TheClaimsAreCompactedPast4096Records (dagda/test/node_local_maintenance_test.cpp:106) |
| 9 | Open: `RetentionStore(state, key, cache_bytes)` and `load` (M:retention.cpp:254-260, 448-460; built at M:src/cluster/local_state.cpp:155-157, stage "retention") | Opens ledger-data/ledger-control tries (3/4 and 1/4 of the claims share), replays claims.log, checkpoints if due. With no `ledger` marker it moves an old checkpoint in, or, with none, rebuilds the tries empty (M:retention.cpp:383-400, 456-457). Logs "retention claims moved into the ledger data= control=" (:398). | Node opens it (D:node.cpp:269-272) under the node-local key with its own logger (`dagda.claims`); `load` (D:retention.cpp:302-326). | DIFFERS: (a) sealed under the node-local key (known); (b) an unmoved MRTS checkpoint, or tries that hold claims under no marker, are refused with `std::runtime_error` (D:retention.cpp:305-307, 320-321) where Macha moved or silently emptied them (known, Tom 2026-11-04); (c) the open is traced as "open claims" (D:node.cpp:269), Macha's stage hook saw "retention" (M:local_state.cpp:155). The readiness plane is "retention" on both (M:src/cluster/cluster.cpp:42; D:node.cpp:38, 277). | M: test_retention_shard_checkpoint_moves_into_the_ledger (:399). D: AnUnmovedLegacyCheckpointIsRefusedAndAMovedOneRemoved (:419), ClaimsWithNoMarkerAreRefusedNotRebuiltEmpty (:479), OpensACheckpointMachaSaved (:636), ReplaysAClaimsLogMachaWroteAndCheckpointsMachasBytes (:582) |

### 1.2 The claim guard

| # | Macha | What callers rely on | Dagda | Verdict | Tests |
|---|---|---|---|---|---|
| 10 | `ClaimStore::guard(ids)` (M:claim_store.hpp:167-172, ClaimLocks M:106-148) | 64 stripes by first id byte, taken in ascending order, held across the presence check and `retain_batch` (and its fsync). One guard over the whole id set (M:src/cluster/distributed_store.cpp:801, 1053; M:src/cluster/storage_server.cpp:381). | `Node::data().claim_locks().lock(ids)` and `in_batches(ids, step)` (D:dagda/store/object_lock.h:147-171); used for every claim path (D:dagda/store/distributed_store.cpp:616, 2189-2195; D:dagda/store/storage_server.cpp:343-349). | DIFFERS: moved from the ClaimStore to the StoragePool (one per node on both sides). Claim paths go through `in_batches`, which sorts by stripe then id and takes the guard 65,536 ids at a time, so a set above 65,536 ids is never under one guard and a refusal in a later part leaves the earlier parts' claims standing (object_lock.h:142-146). At or below 65,536 ids it is Macha's. | M: test_a_delete_marks_its_object_and_holds_no_stripe (test_storage_metadata.cpp:4958). D: StoreClaimLocks.ABatchHoldsOnlyPartOfTheGuardAtATime (dagda/test/store_distributed_store_test.cpp:1079) |
| 11 | `ClaimStore::deleting(id)` (M:claim_store.hpp:175-177) | Under the stripe: a delete has marked the id, so a claim treats it as absent. | `claim_locks().deleting(id)` (D:object_lock.h:107-111). | SAME (moved) | M: test_a_delete_marks_its_object_and_holds_no_stripe; test_a_delete_mark_lasts_until_the_last_delete_of_the_id_ends (tests/test_claim_walk.cpp:42). D: StoreClaimLocks.ADeleteMarksItsObjectAndHoldsNoStripe (:1151), AClaimFindsAMarkedObjectNotPresent (:1186) |
| 12 | `ClaimStore::deletion(class, id)` and `ObjectLedger::claim_deletion` (M:claim_store.hpp:182-190; M:src/contract/object_ledger.hpp:65-67; M:src/ledger/retention_ledger.hpp:41-43) | A delete's leave: under the stripe, `retained(class, id)` false then the id is marked until the ClaimDeletion goes; the stripe is released before return. Called by Macha's DATA sweep (M:maintenance.cpp:1043-1045 into M:src/storage/storage_pool.cpp:1079-1086) and CONTROL sweep (M:src/catalogue/catalogue.cpp:2721). ClaimDeletion is move-constructible and move-assignable (M:claim_store.hpp:77-92). | `claim_locks().deletion(id, check)` (D:object_lock.h:125-136), the caller passing the claim check. Dagda's ObjectLedger has no `claim_deletion` (D:dagda/contract/object_ledger.h:63-81). Both sweeps now call it inside Dagda: DATA (D:dagda/store/storage_pool.cpp:1167), CONTROL (D:dagda/ledger/collector.cpp:339, on the same pool locks the CONTROL claim path takes, D:distributed_store.cpp:2189-2195). | DIFFERS: relocated, class passed as a check; ClaimDeletion is not move-assignable (D:object_lock.h:70). No surviving Macha caller: both callers moved into the Collector. Correction to macha/build/store-api-map.md:167, which says Dagda has no claim lock for the CONTROL sweep: it has (collector.cpp:339). | M: test_a_delete_marks_its_object_and_holds_no_stripe. D: StoreClaimLocks.ALocalClaimOnAControlObjectAboutToGoIsRefused (:1284), APeersClaimOnAControlObjectAboutToGoIsRefused (:1255), AControlClaimInProgressHoldsTheSweepsCheckBack (:1308) |

### 1.3 The object ledger (RetentionLedger)

| # | Macha | What callers rely on | Dagda | Verdict | Tests |
|---|---|---|---|---|---|
| 13 | `RetentionLedger(claims, data, control)` (M:retention_ledger.hpp:21-23; built M:src/service/node_services.cpp:46) | The node's one ledger over its ClaimStore and two object stores. | `Node::ledger()` (D:node.h:365), built at D:node.cpp:272 with the node's time source. | SAME (Macha takes Node's instead of building one) | (construction only) |
| 14 | `claimed(class, cursor, budget)` (M:src/ledger/retention_ledger.cpp:6-30) | One budget operation and one `next_retained` per claim; stops on cancel/deadline/yield first, then budget; at the end the cursor is the start. Deadline read on the real steady clock (M:src/contract/walk.hpp:80). | `Node::ledger().claimed` (D:dagda/ledger/retention_ledger.cpp:17-41), deadline read on the injected time source (:23). | SAME (time injected, known) | M: test_retention_ledger_pages_every_live_claim_once, test_retention_ledger_stops_for_cancellation_first (test_claim_walk.cpp:116). D: LedgerClaimWalk.TheRetentionLedgerPagesEveryLiveClaimOnce, TheRetentionLedgerStopsForCancellationFirst (:132) |
| 15 | `held(class, id)`, `held_view(class)` (M:retention_ledger.cpp:32-34; .hpp:27-29) | The class store's `has` / `held_view`. | D:retention_ledger.cpp:43; D:retention_ledger.h:30. | SAME | M: test_retention_ledger_holds_by_class (:132). D: TheRetentionLedgerHoldsByClass (:146) |
| 16 | `inventory()`, `release()`, `publish(InventoryHandle)`, `publish(ReleaseBuild)` (M:retention_ledger.hpp:30-37, .cpp:36-56) | Pointer copies of the published horizons; an incomplete release build is refused and the previous kept. | D:retention_ledger.h:31-34, .cpp:45, 55-59. | SAME (a null handle's throw is the Published type's on both sides; unverified) | M: test_retention_ledger_publishes_horizons_and_refuses_an_incomplete_release (:152). D: TheRetentionLedgerPublishesHorizonsAndRefusesAnIncompleteRelease (:163) |
| 17 | `release_unreferenced(class, ReleaseHorizon, budget)`, `prune_unclaimed(class, budget)`, `compact_if_needed` (M:retention_ledger.cpp:40-49; .hpp:47-49) | Forward to the store with the horizon's lookup and clock; prune's `exists` is `held`. | D:retention_ledger.cpp:47-53; .h:38. | SAME | M: test_retention_ledger_forwards_claims_releases_prunes_and_compacts (:194). D: TheRetentionLedgerForwardsClaimsReleasesPrunesAndCompacts (:199) |

### 1.4 Claims made by Macha's owner code (the publication barrier and the peers' share)

Macha's callers: `NodeServices::retain_metadata_publication` (M:node_services.cpp:346-531) for a
mutation (called at M:src/metadata/metadata_manager.cpp:1820-1825) and a merge
(:1242-1259), and `retain_on_peers` (M:node_services.cpp:533-554) from the replication loop
(metadata_manager.cpp:129-141). With Dagda's head spaces the same sequence is Dagda's own
(D:dagda/heads/space.cpp:487-510, D:dagda/heads/spaces.cpp:29-43); with `NodeOptions::heads`
off (Macha's first port stage) Macha keeps calling it.

| # | Macha | What callers rely on | Dagda | Verdict | Tests |
|---|---|---|---|---|---|
| 18 | `DistributedStore::retain_data_here(ids, dot)` (M:distributed_store.cpp:1045-1060) | Asks no peer. Under one guard over all ids: an id held here and not marked is claimed (one `retain_batch`), the rest returned sorted. On the writer's commit path. | `Node::store().retain_data_here` (D:distributed_store.cpp:611-629). No Context noun: `Context::Claims::retain` always asks peers for DATA. | DIFFERS: through `in_batches` (row 10), so above 65,536 ids the guard and the frames are per part; each part's `retain_batch` is one frame, so a call may write one frame more than Macha's (the parts count ids not held too). Only reachable through Node's part accessor. | M: unverified (no test found by name). D: StoreDistributedStore.RetainDataHereClaimsWhatIsHeldAndReturnsTheRest (:1416) |
| 19 | `DistributedStore::retain_data(ids, dot)` (M:distributed_store.cpp:835-1000) | Up to `write_copies` claims per object on nodes that hold it, the presence asked in batches (:861); short objects re-placed (:898-915); one claim RPC per selected node, all in flight at once with `std::async` (:924-954); per-object fallback (:956-990); returns the objects no node present holds. Metrics `claim.data_barrier_us` (histogram), `claim.data_barrier.ids`, `claim.data_barrier.under_claimed` (:881-887); debug line "DATA retention barrier ..." at 250 ms (:889-895). Synchronous on the caller's thread. | `Context::Claims::retain(data, ids, dot, writer, deadline[, executor, done])` (D:context.cpp:177-211) or `Node::store().retain_data(ids, dot, writer)` (D:distributed_store.cpp:2206-2288). | DIFFERS: (a) the per-node claims run one after another (D:distributed_store.cpp:2256-2260: "One claim per selected node"), so the barrier costs the sum of the nodes' round trips and remote fsyncs where Macha's cost their maximum; (b) the three `claim.data_barrier*` metrics are gone (no hit for `claim.data_barrier` in dagda/); the debug line is kept (:2283-2286); (c) every claiming node checks `writer` as a write (D:distributed_store.cpp:2178-2181; D:storage_server.cpp:333) and claims nothing for a writer it refuses (the default, this node's principal, is allowed); (d) the local claim takes a control slot first (D:distributed_store.cpp:2187-2188) and fails when none is free, which happens only with a device reserve (`AdmissionOptions::device_reserve`, default true, D:node.h:73-75; with it off `admit_control` always grants, D:dagda/store/data_work.cpp:382-383); (e) the Context form runs on the store's workers and answers at the deadline at the latest with "may yet land" (D:context.cpp:186-202; D:distributed_store.cpp:2421-2442). | M: test_retain_data_batches_a_large_publication_within_bounded_time (named in D's test comment; file unverified). D: StoreDistributedStore.RetainDataBatchesALargePublicationWithinBoundedTime (:1122), DistributedStore.ClaimsAskNothingMoreAboutObjectsNobodyHolds (:206), StoreScope.ClaimsAreCheckedAsWritesAre (dagda/test/store_scope_test.cpp:179), StoreDistributedStore.AClaimIsNeverRefusedWithTheDeviceReserveOff (:1343). Nothing pins (a) |
| 20 | `DistributedStore::retain_control(ids, dot, scope)` (M:distributed_store.cpp:1062-1130) | Makes each object local (or leaves it unclaimed), then local first then nearest: put the graph and claim, until `sought` (here 1, everywhere metadata_write_copies, every_node all) claims held; true only when the local claim held. Debug line "CONTROL retention claim ..." at 250 ms. | `Context::Claims::retain(control, ..., scope)` (`held_here`), or `Node::store().retain_control` (D:distributed_store.cpp:2364-2419). | DIFFERS: (c), (d) and (e) of row 19; the local graph put also takes a control slot (:2389-2390). Order, `sought` and the result are Macha's. | M: unverified. D: StoreClaimLocks.ALocalClaimOnAControlObjectAboutToGoIsRefused (:1284), StoreDistributedStore.AClaimIsNeverRefusedWithTheDeviceReserveOff (:1343), HeadsCommit.ACommitWhoseControlClaimDoesNotHoldIsRefused (dagda/test/heads_space_test.cpp:527) |
| 21 | Peer side of a claim: `retain_objects` handler (M:storage_server.cpp:373-391) | Under the guard: every id present and unmarked or 409 "not_present"; then one `retain_batch`; no admission. | D:storage_server.cpp:331-351. | DIFFERS: scope check first (403 "scope_refused", :333); a control slot or 503 "storage_busy" (:341-342); `in_batches` (row 10), so a refusal after the first 65,536 ids leaves earlier parts claimed. | M: unverified. D: StoreClaimLocks.APeersClaimOnAControlObjectAboutToGoIsRefused (:1255), StoreMessages.AClaimIsClassDotThenIds (dagda/test/store_messages_test.cpp:139) |
| 22 | Claims event: `events.notify(NodeEvent::claims)` after the barrier (M:node_services.cpp:529) | Wakes maintenance (claims are one of its four events). | The owner notifies `context.events().kind("claims")`, counted in `Node::maintenance_events()` (D:node.cpp:121, 887-890); the heads barrier notifies it itself (D:spaces.cpp:47). | SAME (the owner's, as in Macha) | D: LocalStagesBed.OnlyMachasFourEventsEndRebalancesRest (dagda/test/node_seams_test.cpp:815) |
| 23 | `delete_object` handler's claim check (M:storage_server.cpp:393-412) | Claim check (409 "claimed") before DATA admission (503); the mark held across admission and remove. | D:storage_server.cpp:281-294. | DIFFERS: admission first, so a claimed object on a busy node answers 503 not 409; the mark is held across the remove only. The only caller, `erase_all`, ignores the reply on both sides. | none either side |
| 24 | `erase_all` local claim check (M:distributed_store.cpp:2297-2320) | Local copy removed only through a ClaimDeletion. | D:distributed_store.cpp:2649-2668. | SAME | none either side |
| 25 | Repair's drop-local claim check (M:distributed_store.cpp:2913-2921) | `retained(data)` checked, then `remove` with no mark between. | D:distributed_store.cpp:3041-3047. | SAME (the unguarded window is in both; see gap 9) | D: StoreRepair.ARetainedMissingCopyIsRestoredWithoutReachability (dagda/test/store_repair_test.cpp:752) does not pin the window |

### 1.5 Reference counts

| # | Macha | What callers rely on | Dagda | Verdict | Tests |
|---|---|---|---|---|---|
| 26 | Open: `ReferenceCounts(dir, key, cache_bytes)` (M:src/ledger/reference_counts.cpp:77-108; built M:local_state.cpp:158-160 at `<state>/ledger/referenced`, a quarter of the ledger cache) | A missing, damaged or dirty `state` leaves no root, so the next census walks. | Node opens it (D:node.cpp:274-276), same dir and share, under the node-local key, traced "open reference-counts". | DIFFERS: key (known) and stage name only | M: test_reference_counts_follow_and_survive_a_restart (tests/test_object_ledger.cpp:1281). D: LedgerReferenceCounts.FollowAndSurviveARestart (dagda/test/ledger_reference_counts_test.cpp:48), TheStateIsLayoutV18s (:95) |
| 27 | `root()`, `totals()` (M:reference_counts.hpp:30-35; used M:src/filesystem/filesystem.cpp:2485-2486, 2519, 2544-2545) | The root the counts stand at and their totals. | `Node::references().root/totals` (D:dagda/ledger/reference_counts.h:55-57, .cpp:106-109). | SAME (totals now read under a mutex, so any thread may read them) | D: TotalsAreReadWholeWhileTheCensusSetsThem (:124) |
| 28 | `reset(root, extents, nodes, totals)` (M:reference_counts.cpp:124-132; used filesystem.cpp:2477) | Dirty state, both tries replaced, clean state: two durable state writes and two trie checkpoints. | D:reference_counts.cpp:124-134. | SAME | as row 26 |
| 29 | `change(root, extents_in, extents_out, nodes_in, nodes_out, totals)` (M:reference_counts.cpp:134-150; used filesystem.cpp:2509) | Checked whole before anything is written; false and nothing changed when a count would go below zero; else dirty, two journal frames (one per trie, each fsynced), clean. | D:reference_counts.cpp:136-152. | DIFFERS: the four vectors are taken by value (D:reference_counts.h:63-64) where Macha took const references; Macha's call passes lvalues, so each census step copies all four. Behaviour the same. | as row 26 |
| 30 | `views()` (M:reference_counts.cpp:152-154; used filesystem.cpp:2540, 2609) | Frozen snapshots of both tries. | D:reference_counts.cpp:154, Dagda's `ObjectTrie::Snapshot`. | SAME | as row 26 |

### 1.6 The object trie, held ledger, trie diff and union

| # | Macha | What callers rely on | Dagda | Verdict | Tests |
|---|---|---|---|---|---|
| 31 | `ObjectTrie(dir, key, options)` (M:src/ledger/object_trie.cpp:445-517) | Reads `root`, removes other generations' node files, replays journal.log, rewrites a format-1 trie, checkpoints a due journal. | `ObjectTrie(dir, StorageKey, options, logger)` (D:dagda/ledger/object_trie.cpp:415-457), the frame decode moved to `decode_journal_frame` (:565). | DIFFERS: key and logger only (known); adds `rekey`, `probe`, `read_only` for the migration (D:object_trie.h:189-219) | M: test_a_reopened_trie_is_the_one_that_was_closed (test_object_ledger.cpp:625), test_a_format_1_trie_opens_and_is_rewritten (:797). D: AReopenedTrieIsTheOneThatWasClosed (dagda/test/ledger_object_trie_test.cpp:217), AFormat1TrieOpensAndIsRewritten (:389), OpensATrieMachaSaved (:688) |
| 32 | `get`, `apply`, `write`, `install`, `replace_all`, `checkpoint`, `next`, `size`, `root_hash`, `snapshot`, `stats` (M:object_trie.cpp:525-...; .hpp:165-200) | Journal frame per apply/write (fsync), checkpoint at 1 MiB journal, rewrite when superseded > live and > 16 MiB; leaf_max 128, value_max 16 KiB; node cache LRU of `cache_bytes`. | D:object_trie.cpp:463-527, 720-842; same Options (D:object_trie.h:88-96). | SAME (read side by side: write/install/apply/replace_all/checkpoint are line for line) | M: test_the_trie_holds_exactly_what_was_written (:217), test_a_crash_at_any_step_of_a_checkpoint_loses_nothing (:656), test_a_trie_rewrites_its_node_file_and_stays_bounded (:712), test_a_trie_larger_than_its_cache_reads_what_it_needs (:745), test_a_large_trie_opens_within_the_bound (:992, heavy). D: HoldsExactlyWhatWasWritten (:89), ACrashAtAnyStepOfACheckpointLosesNothing (:249), RewritesItsNodeFileAndStaysBounded (:305), ATrieLargerThanItsCacheReadsWhatItNeeds (:336), DISABLED_ALargeTrieOpensWithinTheBound (:573, opt-in as Macha's heavy test) |
| 33 | `ObjectTrie::Snapshot` (`get`, `next`, `size`, `root_hash`, `ids`, `children`, `records`) (M:object_trie.hpp:129-156); held by Macha's filesystem (M:src/filesystem/filesystem.hpp:56-71) | Frozen through later writes and rewrites; thread-safe. | D:object_trie.h:142-164. | SAME (Macha's structs hold Dagda's type) | M: test_a_snapshot_is_frozen_through_writes_and_rewrites (:921). D: ASnapshotIsFrozenThroughWritesAndRewrites (:504) |
| 34 | `HeldLedger` (M:src/ledger/held_ledger.cpp:17-161) | Queue under the store's lock, flush journals outside the lookup lock, seed then marker then flag. Used inside LocalStore only. | D:dagda/ledger/held_ledger.cpp:26-155. | SAME (line for line) | M: test_a_flush_does_not_wait_for_the_seed (test_object_ledger.cpp:1222). D: LedgerHeldLedger.AFlushDoesNotWaitForTheSeed (dagda/test/ledger_held_ledger_test.cpp:41) and three more |
| 35 | `diff_tries`, `records_not_held`, trie question/answer codecs (M:src/ledger/trie_diff.hpp:67-108) | Cost follows the difference; wire limits 64 prefixes (children) and 512 (records). Macha callers are its wire handlers (src/cluster/dannan_bridge.cpp:342, 364), which become Dagda's server (D:storage_server.cpp:130, 370). | D:dagda/ledger/trie_diff.h:68-106. | SAME (Dagda's `encode_trie_question` documents a throw for too many prefixes, :97; Macha's behaviour there unverified) | M: test_a_diff_finds_exactly_what_differs (:433), test_a_diff_costs_what_differs_not_the_size (:484), test_records_not_held_are_what_no_held_trie_holds (:510). D: LedgerTrieDiff.FindsExactlyWhatDiffers (dagda/test/ledger_trie_diff_test.cpp:73), CostsWhatDiffersNotTheSize (:122), RecordsNotHeldAreWhatNoHeldTrieHolds (:148), WireFormsAreMachas (:247) |
| 36 | `TrieUnion`, `UnionSource` (M:src/ledger/trie_union.hpp:26-74) | Several disks' held tries read as one; hint reuse. | D:dagda/ledger/trie_union.h:34-67. | SAME | M: test_a_union_of_tries_reads_as_one_trie_of_their_records (:551). D: LedgerTrieUnion.ReadsAsOneTrieOfTheirRecords (dagda/test/ledger_trie_union_test.cpp:39) |

### 1.7 Sightings (unreferenced since)

| # | Macha | What callers rely on | Dagda | Verdict | Tests |
|---|---|---|---|---|---|
| 37 | `UnreferencedSince(path)` (M:src/storage/unreferenced_since.cpp:45-57; built M:node_services.cpp:47-48 at `<state>/retention/unreferenced-{data,control}.bin`) | Loads the clear table; an unreadable file is ignored with a warn, which only delays deletion. | Node opens both (D:node.cpp:361-364), reached by `Node::data_sightings()/control_sightings()`; file sealed (D:dagda/ledger/unreferenced_since.cpp:58-86). | DIFFERS: the file is one AES-GCM frame under the node-local key, aad `dagda/sightings/1` (known); a v18 clear file not migrated does not open and is ignored, restarting every wait (the migration seals it, D:dagda/migrate/migrate.cpp:195-238). | M: test_sightings_survive_a_restart (tests/test_absent_nodes.cpp:80). D: LedgerUnreferencedSince.SightingsSurviveARestart (dagda/test/ledger_unreferenced_since_test.cpp:105), TheFileIsTheTableSealedUnderTheNodeLocalKey (:179) |
| 38 | `matured`, `forget`, `pass_complete`, `size` (M:unreferenced_since.cpp:59-91, 115-118) | First sighting on this node's wall clock; a clock set back restarts the wait; a complete sweep drops unrenewed sightings. | D:unreferenced_since.cpp:88-117, 138-141. | SAME | M: test_an_object_matures_a_grace_after_its_first_sighting (:29), test_a_clock_set_back_restarts_the_wait (:53), test_a_completed_sweep_drops_sightings_of_objects_no_longer_held (:64). D: AnObjectMaturesAGraceAfterItsFirstSighting (:52), AClockSetBackRestartsTheWait (:78), ACompletedSweepDropsSightingsOfObjectsNoLongerHeld (:88) |
| 39 | `save()` (M:unreferenced_since.cpp:93-113); cadence `Maintenance::save_sightings` (M:maintenance.cpp:136-146: on a complete sweep, else at most every 30 s) | Writes the whole table by durable replace when dirty; a failed write is logged and retried. | D:unreferenced_since.cpp:119-136; cadence `Collector::save_sightings` (D:collector.cpp:61-69), same rule. | DIFFERS: one AES-GCM seal of the table per save (cost in section 2); the seal runs outside the try (:127), so a throw from it leaves `_dirty` cleared and escapes `save()`, where Macha's encode could not throw. | as rows 37-38 |

### 1.8 Horizons, their builder, and the pass that drives the claims

| # | Macha | What callers rely on | Dagda | Verdict | Tests |
|---|---|---|---|---|---|
| 40 | `ReferencedSets` (`referenced`, `lookup`, `next`, `all`, `not_held`, `size`) (M:src/contract/horizon.cpp:22-126) | Counted view plus sorted others; `lookup` a span when there is no view, else a function over both. | D:dagda/contract/horizon.cpp:27-104. | SAME | M: test_inventory_is_both_live_sets_and_the_catalogue_control_set (tests/test_horizon_builder.cpp:158). D: LedgerHorizonBuilder.TheInventoryIsBothLiveSetsAndTheControlSet (dagda/test/ledger_horizon_builder_test.cpp:67) |
| 41 | `InventoryHorizon`, `ReleaseHorizon`, `ReleaseBuild` (M:horizon.hpp:67-130, .cpp:128-154) | Garbage split stale/collectable at build; release stamped with head and clock. | D:dagda/contract/horizon.h:102-148, .cpp:106-127. | DIFFERS (names only): `catalogue_complete()` is `complete()`, `GarbageRef` is `Garbage{id, retired_at_ns, token}`, `TombstoneBatch` is `GarbageBatch`, `tombstone_batches()` is `batches()`. Both keep the batches in the order given (Macha's comment says sorted, M:horizon.hpp:91; its code does not sort, .cpp:137-138). | as row 40 |
| 42 | `HorizonBuilder::namespace_objects` + `inventory` (M:src/ledger/node_horizon_builder.cpp:11-28, 91-100) and the pass's rebuild rule (M:maintenance.cpp:611-653) | Rebuild when none, incomplete or behind `objects->metadata_generation` (after the cached census), held back on a repair-only pass until metadata is ready; DATA = conflict extents + catalogue live + counted extents view; CONTROL = catalogue control + batches + counted nodes view. | The owner's `ReachabilityProvider::inventory(pause)` and `generation()` (D:dagda/contract/reachability.h:86-95), `NodeHorizonBuilder::inventory` (D:dagda/ledger/node_horizon_builder.cpp:34-39, batches added at :18), `Collector::inventory_step(pause, may_rebuild)` (D:collector.cpp:113-143). | DIFFERS: the merge Macha did in `build_inventory` is now Macha's provider's to write; the staleness test reads `provider.generation()` before the build where Macha read the built objects' generation; the follow-up wake (`gc_quiescent_until`, M:maintenance.cpp:652) is the owner's. Trace words and `maintenance.inventory.*` metrics kept (D:collector.cpp:129-141). | M: test_inventory_control_set_holds_the_namespace_tree (:192). D: TheInventoryControlSetHoldsTheOwnersNodes (:94), LedgerCollector.AnInventoryIsNeverUsedDestructivelyInThePassThatBuiltIt (dagda/test/ledger_collector_test.cpp:174) |
| 43 | `HorizonBuilder::release(head)` / `build_release` (M:node_horizon_builder.cpp:30-85, 102-115) | The release at the sole accepted head: extents (counted view when the census is at that tree, else a walk), conflict roots, every catalogue root's objects (an unreadable root marks it incomplete, logged debug :59-60), tree nodes (unreadable: incomplete, warn :72-73), batches, the head's mutation clock. | `provider.release(head)` and `NodeHorizonBuilder::release` (D:node_horizon_builder.cpp:41-53); the release step (D:collector.cpp:155-175). | DIFFERS: completeness per part and its log lines are the provider's; a provider that throws gives an empty incomplete build and one debug line "retention release horizon unavailable" (:47-51). The step itself (head check, trace "release-horizon", `maintenance.release_horizon.build_us`, refuse incomplete) is Macha's (M:maintenance.cpp:920-934). | M: test_an_unreadable_catalogue_root_leaves_the_release_incomplete (:288), test_an_unreadable_tree_node_leaves_the_release_incomplete (:309), test_an_empty_head_releases_against_nothing (:328). D: AnUnreadablePartLeavesTheReleaseIncomplete (:149), AHeadThatCannotBeReadLeavesTheReleaseIncomplete (:168), AnEmptyHeadReleasesAgainstNothing (:180), LedgerCollector.AnIncompleteReleaseIsNeverPublished (:240) |
| 44 | The CONTROL gate's release, sweep, prune (M:maintenance.cpp:946-972) and `CatalogueManager::control_gc_step` (M:catalogue.cpp:2654-2737) | Release 64, sweep 32, prune 64; metrics `retention.released.control`, `catalogue.control_gc.removed`, `retention.pruned.control`; trace "release.control", "control-gc", "prune.control"; epoch fence under the catalogue's mutex; claim checked at once and again under a ClaimDeletion. | `Collector::control_step` (D:collector.cpp:177-205) and `control_gc_step` (:295-352), budgets D:collector.h:44-47. | DIFFERS: the epoch is the provider's (`control_epoch`, `within_control_epoch`, D:reachability.h:104-112) instead of the catalogue's mutex; the delete leave is the pool's claim locks (row 12). Budgets, metrics, trace words and the debug line are Macha's. | M: none found by name. D: LedgerCollector.TheControlSweepReleasesRemovesAndPrunes (:421), AControlObjectStagedInTheCurrentEpochIsKept (:357), AControlRemovalNeverRacesANewEpoch (:381) |
| 45 | The DATA gate's release, sweep, prune (M:maintenance.cpp:991-1092) | Release 64 then log "retention released DATA claims node= count="; protected = unmatured retirements; `gc_step` with budget 64 and orphan grace max(grace, no_progress_backoff); prune 64; metrics `retention.released.data`, `maintenance.gc.*`, `retention.pruned.data`; trace "release.data", "gc", "prune.data". | `Collector::data_step` (D:collector.cpp:207-273); `StoragePool::gc_step` (D:storage_pool.cpp:1129-1184). | DIFFERS: `gc_step` reports `deferred` at a sweep's completion for the whole pass (D:storage_pool.cpp:1143-1147) where Macha's reported only the last slice's (M:storage_pool.cpp:1053-1057, 1068-1070); an owner that schedules on it (Macha's `gc_quiescent_until`, M:maintenance.cpp:1081-1090) wakes after the orphan grace in cases Macha waited for an event. The `claim_deletion` argument is gone (row 12). Macha's `Log::trace` "maintenance: garbage collection ..." lines (:1079-1090) are not the collector's; they stay with the owner's pass. | M: test_retention_claim_is_physical_gc_barrier (test_storage_metadata.cpp:505). D: LedgerCollector.ARetentionClaimIsAPhysicalGcBarrier (:128), AClaimFromAnUnseenBranchSurvivesTheSweeps (:277), StoreStoragePool.AClaimedCopyIsUntouchable (dagda/test/store_storage_pool_test.cpp:624) |
| 46 | `ClaimWalk::step` (M:src/service/claim_walk.cpp:8-43) | 16 claims a step from a kept cursor; restorer asked for each one not held; resumes before a claim that met no credit. Metrics: `maintenance.claim_walk.examine_us` (histogram of each `held` check), `.examined`, `.missing`. | `ledger::ClaimWalk::step` (D:dagda/ledger/claim_walk.cpp:14-41), run by `store::Repair`. | DIFFERS: no `maintenance.claim_walk.examine_us`; the two counters are bumped through `Observations::add(name)` per claim (string-keyed) where Macha held counter references (M:claim_walk.cpp:9-11). | M: test_claim_walk_matches_the_walk_it_replaced_exhaustively (:372), _at_the_bound (:392), test_claim_walk_follows_a_changing_ledger (:409). D: LedgerClaimWalk.MatchesTheWalkItReplacedExhaustively (:354), MatchesTheWalkItReplacedAtTheBound (:373), FollowsAChangingLedger (:389) |

Counts: 46 rows.

- SAME (26): 2, 3, 4, 5, 6, 7, 8, 11, 13, 14, 15, 16, 17, 22, 24, 25, 27, 28, 30, 32, 33, 34, 35, 36, 38, 40.
- DIFFERS (20): 1, 9, 10, 12, 18, 19, 20, 21, 23, 26, 29, 31, 37, 39, 41, 42, 43, 44, 45, 46.
- MISSING (0). Nothing Macha's callers use is without a Dagda call; what is missing is
  inside rows (the parallel claims and three metrics of row 19, one histogram of row 46).

## 2. Performance-relevant differences

1. **DATA claims to peers are serial.** Macha sends one claim per selected node, all at
   once (M:distributed_store.cpp:924-954, `std::async` per node, then `get()` in turn);
   Dagda sends them one after another (D:distributed_store.cpp:2256-2260). Each claim is
   a control RPC plus a `retain_batch` fsync on the peer (D:storage_server.cpp:343-349).
   With k selected nodes the barrier is the sum of k round trips and k remote fsyncs where
   Macha's was the largest one. Where it lands: the commit path when a commit brings in an
   object not held here (M:node_services.cpp:510-511; D:dagda/heads/space.cpp:495) and the
   peers' share of every commit (M:node_services.cpp:543; D:spaces.cpp:41), which in Macha
   runs on the replication thread and so delays the owed claims queue, not the caller.
   With write_copies 2 most objects select two nodes, one of them often this node, whose
   claim is local (no RPC), so the loss is the remote nodes' count minus one.
2. **Syncs per claim are Macha's.** One fsynced claims.log frame per 65,536 ids
   (D:retention.cpp:336-342), checkpoint at 8,192 changed objects or a 4 MiB log
   (:37-38), compaction at 4,096 frames or 64 MiB (:33, 441). The journal code is line
   for line Macha's: fsync per append (D:dagda/ledger/sealed_journal.cpp:49-55, 182-198;
   M:src/storage/sealed_journal.cpp:47-54, 179-196). `retain_data_here` and the local
   claim path go through `in_batches` (row 10), which can write one frame more than
   Macha's for a call above 65,536 ids; no difference below it.
3. **Sealing cost per frame is Macha's.** Both seal with AES-256-GCM through a fresh
   `EVP_CIPHER_CTX` per call (D: dannan/identity/crypto.cpp:104-113; M:src/crypto.cpp:
   120-128); the node-local key is read as a span over secure memory with no copy or
   derivation per frame (D:dagda/domain/storage_key.h:58; dannan/identity/
   secure_memory.h:113). New: each sightings save seals the whole table once
   (D:unreferenced_since.cpp:127): 40 bytes an object, at most twice a minute, next to a
   durable replace that already writes and fsyncs those bytes. Negligible.
4. **Cache sizes are Macha's.** Default ledger cache 64 MiB on both (M:src/config.hpp:598;
   D:dagda/store/local_store.h:122), split the same way: claims 3/8 (24 MiB, 18 to DATA,
   6 to CONTROL), reference counts 1/4 (16 MiB, 12/4), CONTROL held 1/16, the rest the
   DATA backends' shared equally (M:local_state.cpp:33-50, M:storage_pool.cpp:249-250;
   D:local_store.h:121-138, D:storage_pool.cpp:349). Trie options identical (64 MiB
   default cache, 1 MiB journal, 16 MiB rewrite floor, D:object_trie.h:88-96).
5. **Compaction is Macha's.** Only in the local stages, skipped when busy, threshold
   4096 (D:node.cpp:708-718; M:maintenance.cpp:1128-1131).
6. **Locks across I/O.** Same as Macha: the retention store's mutex is held across the
   claims.log fsync, a lookup's node read and a checkpoint; the claim stripes across the
   presence check and the claim's fsync; prune runs `exists` without the mutex. New: the
   local claim, the peer handler and the local CONTROL graph put take a control slot
   before the stripes (D:distributed_store.cpp:2187, 2389; D:storage_server.cpp:341),
   which waits up to the admission wait or fails when a device reserve is on. Macha's
   facade turns the reserve off (D:node.h:73-75), and then the slot is free
   (D:data_work.cpp:382-383).
7. **The Context claim hops threads.** `Context::Claims::retain` runs the claim as a job
   on the store's workers and posts the answer (D:distributed_store.cpp:2421-2442); when
   the workers are busy the claim queues behind them. Macha's barrier ran on the caller's
   thread. `Node::store().retain_data/retain_control` keep Macha's synchronous form.
8. **Small copies.** `ReferenceCounts::change` copies four id vectors per census step
   (D:reference_counts.h:63-64). The claim walk looks its two counters up by name per
   claim (D:claim_walk.cpp:23, 26).
9. **Shared inefficiency, unchanged.** `RetentionLedger::claimed` calls `next_retained`
   per item, and each call reads a page of up to 256 records under the store mutex to
   return one id (M:retention.cpp:523-542; D:retention.cpp:372-389). The claim walk pays
   it 16 times a step on both sides.

## 3. Gaps for Dagda to fix

1. **Parallel DATA claims.** Macha needs `retain_data` to claim on every selected node at
   once; Dagda claims one node after another (D:distributed_store.cpp:2256-2260). Test:
   `StoreDistributedStore.RetainDataClaimsOnEveryNodeAtOnce` (three hosting peers each
   delaying `retain_objects` by D through `NodeOptions::front`; the call returns within
   1.5 D; fails today at about 3 D). Severity: loses performance.
2. **The DATA barrier's metrics.** Macha records `claim.data_barrier_us`,
   `claim.data_barrier.ids`, `claim.data_barrier.under_claimed`
   (M:distributed_store.cpp:881-887); Dagda records none. Test:
   `StoreDistributedStore.ADataClaimBarrierIsMeasured`. Severity: cosmetic (observability).
3. **The claim walk's examine histogram.** Macha records
   `maintenance.claim_walk.examine_us` per claim (M:claim_walk.cpp:9, 22); Dagda does not.
   Test: `LedgerClaimWalk.EachPresenceCheckIsTimed`. Severity: cosmetic.
4. **A here-only DATA claim on the Context.** Macha's publication barrier (kept while
   `NodeOptions::heads` is off) claims what it holds asking no peer
   (M:node_services.cpp:505); Dagda has it only as `Node::store().retain_data_here`, not on
   `Context::Claims`, whose DATA retain always asks peers (D:distributed_store.cpp:2426-
   2427). Test: `ContextClaims.ADataClaimHereAsksNoPeer`. Severity: cosmetic while Node's
   part accessors are allowed; blocks the port if Macha is held to the Context nouns.
5. **All or nothing above 65,536 ids.** Macha's local claim and peer handler claim nothing
   when any id is absent (M:distributed_store.cpp:801-815; M:storage_server.cpp:381-390);
   Dagda's `in_batches` leaves earlier parts claimed and reports failure
   (D:object_lock.h:142-161; D:distributed_store.cpp:2189-2195; D:storage_server.cpp:
   343-349). Test: `StoreDistributedStore.ALocalClaimThatFindsOneObjectAbsentClaimsNone`
   (65,537 ids, the absent one sorted into the second part). Severity: cosmetic (extra
   claims are safe, and released by the horizon).
6. **The startup stage name.** Macha's stage hook sees "retention" for the claims and
   reference counts (M:local_state.cpp:155); Dagda traces "open claims" and "open
   reference-counts" (D:node.cpp:269, 274). A Macha fixture that holds the "retention"
   stage never fires. Test: `NodeSeams.TheClaimsOpenUnderMachasRetentionStage`, or the
   facade maps the names. Severity: cosmetic.
7. **A claimed object's delete answer.** Macha answers `delete_object` for a claimed
   object 409 "claimed" whatever the node's load (M:storage_server.cpp:400-407); Dagda
   admits first and answers 503 when busy (D:storage_server.cpp:283-288). Test:
   `StoreClaimLocks.ADeleteOfAClaimedObjectIsClaimedEvenWhenBusy`. Severity: cosmetic
   (the only caller ignores the reply).
8. **Sightings save on a failed seal.** `UnreferencedSince::save` clears `_dirty` and then
   seals outside the try (D:unreferenced_since.cpp:120-127), so a throwing seal escapes
   into the collector's step and the table is not retried until the next change. Test:
   `LedgerUnreferencedSince.AFailedSealIsRetriedAtTheNextSave`. Severity: cosmetic.
9. **Repair's drop-local window (shared with Macha).** Both check `retained` and then
   remove with no mark between (M:distributed_store.cpp:2913-2921;
   D:distributed_store.cpp:3041-3044), so a claim landing in between finds its copy gone.
   Not a regression; the copies counted as keepers remain, and the claim walk restores
   the local one. Test: `StoreRepair.ADropLocalNeverRemovesACopyClaimedDuringItsCheck`.
   Severity: cosmetic for the port; a correctness defect in both.
10. **Invalid dot exception type.** Macha throws `std::runtime_error` for a dot with no
    origin or sequence (M:retention.cpp:470-471); Dagda `std::invalid_argument`
    (D:retention.cpp:332, contract-documented). Test:
    `LedgerRetention.AnInvalidDotIsRefusedAsMachaRefusedIt`, or record it as a decided
    difference. Severity: cosmetic (Macha's dots come from `reserve_mutation_dot`).

## 4. Not mapped

- Macha's AvailabilityService and its path table (src/service/availability_service.*)
  beyond its ledger calls (`held_view`, `inventory`): another section's.
- What a commit introduces (the object collection in M:node_services.cpp:372-500 against
  D:dagda/heads/space.cpp before :470) and the owed-claims queue bound
  (M:metadata_manager.cpp:93-109): the heads section's. Only the barrier's calls into the
  store are mapped here.
- `CatalogueManager::retention_objects` and the catalogue's half of the inventory: owner
  code that becomes Macha's ReachabilityProvider; its correctness against Macha's
  `build_inventory` / `build_release` is not checked here.
- Status figures: Dagda's `dagda.gc.*`, `dagda.maintenance.*` and the reference totals
  (D:node.h:481-512) against Macha's status_api: not compared.
- dagda-migrate's handling of claims tries, claims.log, the reference counts and the
  sightings: only `migrate_sightings` was read (D:migrate.cpp:195-238).
- Macha tests not paired by name: the stage-2 held-ledger tests on LocalStore
  (test_object_ledger.cpp:1064-1260), any Macha test of `retain_data_here` and
  `retain_control` (none found by name), the CONTROL sweep's Macha tests.
- Unverified: `Published<T>::publish` on a null handle on either side; Macha's
  `encode_trie_question` on too many prefixes; Macha's `no_progress_backoff` default
  against `CollectorBudgets::retry_window` (D:dagda/ledger/collector.h:41).
