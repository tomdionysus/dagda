# Dagda's capability report, for the port (2026-11-10, brought to 2026-11-13)

For the Macha session, which reviews it by reading Dagda's code and builds the port (the maintainer's rulings, relayed 2026-11-06). Written
from the tag `0.1.0`, brought to `0.1.2`: `0.1.1` named your purpose deadlines on the store's calls; `0.1.2` holds a manifest's partial for its pass and writes the store's journal lines in Macha's words. The migration rehearsal ran on `0.1.0`'s tool. Macha's baseline: `98b816d` (0.93.5), with 0.93.6 `a931fe5` for the yielded wake; 0.93.4 `9a08846` is
the last tag. Every claim names a case (`Suite.Case`, `file:line` under `dagda/test/`) or a commit; lines are at `0.1.0`.

## The answer

**I cannot say Yes.** Whether Macha moves onto Dagda with no loss of data, function, reliability or performance can only be shown
by Macha running on it, and by the in-place migrations of live-a and live-b after the port. That is your test on the nodes.
What I can say: every requirement row below has code, and all but the ones named under "Gaps in the trace" have a case that
failed before its code or pins Macha's bytes; I know of no Dagda work left that I can do without the port. That is not a claim
that nothing is left. Since S7 alone, reading and the opt-in stress case found three real defects in code earlier called done
(the pack append gap, the sweep's per-slice deferral, a status race), gate runs found races in three tests, and your 0.93.6 found
a wake the test's copy of your loop lacked. Expect the port to find more; as the maintainer ruled, the port pauses and I fix them.

## Head and gates

- **Tag `0.1.2`**, annotated, on `main`, the tag the port freezes Dagda on. Since `0.1.0`: the store's peer calls name the maintainer's
  purpose deadlines (`0.1.1`, below, "What the port must do"); a manifest pass holds its partial, so a resume beside it is
  refused (the rehearsal's gap); every store journal line in Macha's text and level (`TODO/2026-11-12-journal-mapping.md`).
  Build the cutover on it. `0.1.0` is on the remote; `0.1.1` and `0.1.2` are local until the maintainer says push.
- **macOS at `0.1.2`, plain: 1041 passing, 6 disabled** (`0.1.0`'s 1031, seven deadline cases, one manifest case, two journal
  cases; journal assertions added to seven more), one whole run, Macha's fixtures present, nothing skipped.
- **TSan, macOS, at `0.1.2` on Dannan 0.5.3 built with TSan:** 1024 passed, 17 failed, and no report in Dagda's code. The two
  reports are Boost's kqueue reactor race, as in every macOS run. The 17 are forked children expecting exit 0 and getting
  TSan's 66: 15 migration crash cases inherited the parent's report count across the fork, and pass alone in a fresh process;
  2 durability cases' children `_exit` on purpose (a simulated crash) with a finished thread unjoined, which TSan counts as a
  leak. No `mutex lock failed`. The arm64 runs below are at `0.1.0`.
- Dannan pinned at **0.5.3** (`CMakeLists.txt:28`, `find_package(dannan 0.5.3 CONFIG REQUIRED)`; tag on main `899faea`), the tag the
  port freezes on: 0.5.2's library, its installed config asking for Boost 1.83, so the Pis build with Debian's Boost. Built into
  `build/deps/prefix-0.5.3`.
- **macOS (Apple clang, arm64), plain: 1031 passing, 6 disabled**, one whole run at the final code (the maintainer, 2026-11-06: "Test once and
  be done with it."), Macha's fixtures present, nothing skipped. The six disabled, each opt-in by `--gtest_also_run_disabled_tests`,
  none a gate:
  - `DeleteStress.DISABLED_AStormOfDeletesOnABusyCluster` (`delete_stress_test.cpp:1801`) and
    `DeleteStress.DISABLED_HeldTriesThroughCheckpointsAndRewrites` (`delete_stress_test.cpp:1810`): the concurrent-delete stress case, real clocks, minutes
    each. The maintainer, 2026-11-09: "No more stress tests."
  - `LedgerObjectTrie.DISABLED_ALargeTrieOpensWithinTheBound` (`ledger_object_trie_test.cpp:573`): Macha's stage-1 measurement,
    200,000 records by default, real clock.
  - `Scale.DISABLED_ATrieOfAMillionIds` (`scale_test.cpp:56`), `Scale.DISABLED_AHundredThousandObjectsAcrossPacks` (`:106`),
    `Scale.DISABLED_ManifestsOfMachasCounts` (`scale_test.cpp:169`): live-a's scale, real clock, BENCH lines. Last run 2026-11-05 on the Mac and
    live-a (`COMPLETED.md`, "sanitizers and scale"); not rerun at this tree.
- **arm64 (Pi 5, GCC 14.2, Debian's Boost 1.83), plain, on live-b at `ddba991`:** 958 passed, 3 failed, 68 skipped, 7 disabled.
  This was the first GCC link and run since S2. The 3 failures were root's: root ignores the directory modes the permission-fault
  cases rely on, and they now skip as root, as Dannan's do (`48370f3`). The 68 skipped needed Macha's fixtures, which were not on the
  Pi. GCC gave 46 warnings, none an error. All are fixed (`6d37738`, `e01e11a`, `9fafb90`); the fixes are covered by the Mac run
  above, and GCC confirms them at `0.1.0` (below).
- **arm64 at `0.1.0`, on build-host** (192.0.2.1, a Pi 5 like live-b; GCC 14.2, Debian's Boost 1.83 and GoogleTest 1.16;
  Dannan 0.5.3; one job at a time, nice 19, idle I/O; Macha's fixtures without `state-live-a`), 2026-11-12:
  - Plain: build with **0 warnings**; **1028 passed, 0 failed, 3 skipped** (the root cases), 6 disabled. Every fixture case ran.
  - ASan with UBSan (`-fno-sanitize-recover=undefined`): **1028 passed, 0 failed, 3 skipped**, 6 disabled, no sanitizer report.
    The build gave 14 warnings, all `-Wmaybe-uninitialized` inside libstdc++'s `std::regex` through gtest, in one test file
    (`contract_include_direction_test.cpp`), only with sanitizers on; a GCC false positive in system headers, left alone.
- **live-b** went down from about 01:30 to 18:34 on 2026-11-11, during an ASan build beside the fixture run. Its journal was
  volatile, so the cause is unknown; it is persistent now. build-host logged one "Undervoltage detected!" during the plain build
  (normalised in 2 s, `throttled=0x50000`), so under-voltage under C++ build load is the likely cause on these Pis. No more builds
  on live-b (the maintainer, 2026-11-12).
- **TSan, macOS, at `ff77156` on Dannan 0.5.2:** 1028 passed. There were 5 reports. 2 are Boost's kqueue reactor race, with no
  Dagda frame, as on 2026-11-05. 3 were test-only: a `dannan::test::Gate` was destroyed before the Cluster whose threads used it.
  Those are fixed (`c5a86a2`). It also showed a raced wait in the prompt replication cases, fixed in `e8bf934`. No
  `mutex lock failed` abort. Not rerun after the fixes. TSan on arm64 is not possible with GCC 14 (Dannan's report, 7).
- **Not rerun on the final tree:** Linux x86. The last Linux run was live-a at 422 cases (2026-11-05).

## What changed since the last thing you read

Each a difference from Macha, or a defect, found since S7 (`2ba4666`):

- **The pack append gap (`793a9ec`).** Compaction no longer runs between a pack append and its indexing: it waits, under the pack
  lock it already holds, until every appended record is indexed (`LocalStore::_unindexed_appends`, `dagda/store/local_store.h`).
  Cases `StoreLocalStore.ACompactionWaitsForAPutInItsAppendGap` (`store_local_store_test.cpp:393`) and
  `StoreLocalStore.ACompactionWaitsForARemoveInItsAppendGap` (`store_local_store_test.cpp:440`), which failed first.
- **The sweep's pass-wide deferral (`96a534c`).** `StoragePool::gc_step`'s pass-completing slice reports every deferral of the pass
  (`_gc_pass_deferred`). Case `StoreStoragePool.TheSliceThatCompletesASweepReportsEveryDeferralInItsPass`
  (`store_storage_pool_test.cpp:679`), which failed first.
- **Macha's publication budget (`6635618`).** `WriteOptions::no_progress_budget` and `progress` (`distributed_store.h:157-165`): a
  window over the whole write, re-armed by a copy landing, a fallback starting, any pending call that moved within it, or the
  caller's counter; when it runs out the pending calls are cancelled and the write ends with what landed, the rest owed to prompt
  replication. Off by default. Cases `StoreRepair.AWriteWithABudgetEndsWhenNothingMovesForIt` (`store_repair_test.cpp:1425`, failed
  first) and `StoreRepair.AWritesBudgetIsReArmedByItsPipelinesProgress` (`store_repair_test.cpp:1464`). `0a4b0eb` fixed a race in the first case's
  stepping (the local copy landing re-arms the window; the case now steps until the write ends, at most 93 s, under the call's 300 s).
- **Macha's unmoved metadata refuses a heads node (`e74071c`).** With `NodeOptions::heads` on, `<state>/metadata` or
  `<cache>/metadata/current.meta` refuses the node, naming `dagda-migrate --heads --state <state>`, and nothing changes
  (`node.cpp:165-174`; paths `heads::machas_metadata_directory` and `heads::machas_metadata_seed`, `heads/sealed_file.h:48-51`,
  shared with the migration). With `heads` off it opens. Case `Node.MachasUnmovedMetadataRefusesAHeadsNodeNamingTheMigration`
  (`node_test.cpp:135`), which failed first.
- **Repins** to Dannan 0.5.1 (`620f172`), 0.5.2 (`fd15440`) and 0.5.3 (`0a2192b`): no Dagda code changed for any of them.
- **Your 0.93.6 yielded wake, in the test's copy of your loop (`84ad783`).** Dagda's own loop cannot sleep that way: every pass sets
  the next one an interval on, and no wait is longer than one interval (`node.cpp:912-969`). The copy in `maintenance_steps_test.cpp`
  had the defect. It now sets `repair_continue` for `RepairSettled::yielded` as 0.93.6 does, and parks on `repair_busy` as the pass
  found it when it began (your `maintenance.cpp:1362`), not re-read. Case
  `StepsBed.RepairResumesAfterYieldingToAViewerThatArrivedMidStep` (`maintenance_steps_test.cpp:1301`) failed first: the copy never
  pushed. `repair.h:112-117` says a yielded step owes another turn after an interval.
- **Your busy wake, in the same copy (`cb06c8d`), as you asked.** A pass that began busy wakes when the foreground quiet window ends,
  or at once if it already has (your `maintenance.cpp:1327-1336`). The copy computes this from the activity clocks' `idle_for`,
  against repair's `foreground_quiet`. Case `StepsBed.ABusyPassWakesWhenTheQuietWindowEnds` (`maintenance_steps_test.cpp:1282`)
  failed first. Both changes are test code only.
- **Test races, test code only.** In 17 cases a test Gate was destroyed before the Cluster that used it, and futures could wait
  before their gate opened, so a failed assertion hung the run (`c5a86a2`). The prompt replication and silent-peer cases also raced
  their own waits (`e8bf934`).
- **Root and GCC (`48370f3`, `6d37738`, `e01e11a`, `9fafb90`).** Three permission-fault cases skip as root. A record's bytes are now a
  range only: the conversion operator is gone, because GCC saw two ways to make the span. A remote read with no deadline now uses
  `TimePoint::max()` as its deadline, with each wait capped at one look at the clock; the old optional deadline gave a false
  `-Wmaybe-uninitialized`. No behaviour changed.
- **The status race (`4002e9c`):** `ReferenceCounts::totals()` read under its own lock; case
  `LedgerReferenceCounts.TotalsAreReadWholeWhileTheCensusSetsThem` (`ledger_reference_counts_test.cpp:124`).

## Where Dagda is safer than Macha at `98b816d`

1. **Pack compaction against a packed put or remove.** Macha's `local_store.cpp:821-867`, `:1694`, `:1700-1714`, `:1842` let a
   compaction take the gap between an append and its indexing: it counts the new record dead, may take the active pack as victim
   and unlink it, so a put is lost while the held ledger says held, and a remove's object comes back at the next start. Found by
   the stress case (run 101) and read in both trees. Fixed in Dagda (`793a9ec`, cases above); Macha's fix is after the freeze, by
   The maintainer's decision (2026-11-08).
2. **The sweep's deferral.** Macha's `gc_step` (`storage_pool.cpp:1030-1083`) reports `deferred` for its own slice only, and its
   loop (`maintenance.cpp:1079-1087`) reads it from the slice that completes the pass, so a pass whose unripe objects fell in
   earlier slices ends "complete; waiting for an event" with them on disk. Garbage late, never lost. Fixed in Dagda (`96a534c`);
   Macha's loop is unchanged and now gets the right answer.

## Macha's behaviours kept on purpose

Each is Macha's at `98b816d`, recorded in `TODO/ACTIVE.md`, and not Dagda's to change before the port:

- **The transient 409** when a DATA and a CONTROL id coincide: a delete marks the id alone, so a claim of the other class is refused
  until it ends. One `RetentionStore` and one `ClaimLocks` per node, keyed by id (`COMPLETED.md:2090-2093`).
- **The quiet-window sweep starvation:** a node whose storage, metadata, topology or claims events come faster than the window
  never sweeps until they stop (`COMPLETED.md:2088-2089`; `COMPLETED.md:263-266`).
- **The pool's `get` stopping at the first holding backend's empty answer** (`storage_pool.cpp:591-610` in Macha), still open
  (`COMPLETED.md:2084`, `:2087-2088`). With the append gap closed, the case that made it visible no longer arises.
- **The local writer's DATA admission carries no cancellation:** a store stopped while a write waits for credit is held until a
  release or the no-progress window (120 s on the live nodes), as Macha's flag without a notify is (`COMPLETED.md:2100-2102`).
- **W1:** a peer may sweep an old unreferenced object just as another node re-references it, before it hears of the commit; it
  costs one replica until repair. The fix is the maintainer's, recommended after the port (`COMPLETED.md:2074-2075`).
- **A full control store** behaves and recovers as Macha's (`COMPLETED.md:2071-2073`); Macha's silent status and unbounded retry stay
  Macha's defects for after the freeze.

## Lost wakeups (your found-issues 14)

Every notify in Dagda's tree was checked on 2026-11-08 (`COMPLETED.md:2094-2102`): none has the lost-wakeup shape. Where Dagda differs,
it is already closed: the local writer's and the durability worker's waits take the stop token (`distributed_store.cpp` local
writer, `durability_domain.cpp`), so `request_stop` wakes them through `condition_variable_any`'s own lock; prompt replication waits
timed while a retry is queued; the scan's completion and failure are set under the mutex their waiters read; the exchange's
replicator, `time_advanced` and maintenance wakes take the waiter's mutex before notifying. The same as Macha: the local writer's
DATA admission (above). Where a caller's flag exists, `ClassWaits` polls it every 25 ms. You have read and agreed this.

## Capabilities and evidence, by requirement

The rows are `TODO/2026-11-03-macha-store-requirements.md` section 3. Macha's own cases map onto these in
`TODO/2026-11-05-test-mapping.md` (161 RETIRE, each against a named case). One or more cases per row; the first is the strongest.

**Domain (3.1).**
- D1 key derivation: `DomainStorageKey.DerivesMachasStorageKeyFromTheClusterKeyFile` (`domain_storage_key_test.cpp:39`), `...TrailingLineEndsAreNotPartOfTheKey` (`:71`).
- D2 sealing: `StoreSealing.SealObjectIsAesGcmWithTheDomainAndIdAsAssociatedData` (`store_sealing_test.cpp:104`), `...OpenChecksTheSizeAndTheId` (`:134`); v18's in `MigrateLegacyV18.OpensWhatMachaSealed` (`migrate_legacy_v18_test.cpp:67`).
- D3 roles: `StoreLayoutV19.AV19DiskIsRefusedWithoutItsKeyUnlessOpenedBlind` (`store_layout_v19_test.cpp:195`), `...ASealedRecordMovesToABlindNodeAndBackUnopened` (`:262`). Not on Macha's path.
- D4 node-local: `DomainId.ANodeLocalDomainIsTheNodesOwn` (`domain_domain_id_test.cpp:35`), `StoreLayoutV19.AStoreWithALedgerNeedsTheNodeLocalKey` (`store_layout_v19_test.cpp:400`). Every ledger is under it (v19).

**Store (3.2).**
- S1 ids: `StoreObjectId.IsTheSha256OfThePlaintext` (`store_object_id_test.cpp:21`), `StoreLocalStore.APutRefusesBytesThatAreNotTheId` (`store_local_store_test.cpp:788`).
- S2 loose form: `StoreLocalFormat.WritesTheLooseFileMachaWrites` (`store_local_format_test.cpp:234`), `StoreWriteBehind.*` (`store_write_behind_test.cpp:27-70`).
- S3 packs: `StoreLocalStore.PacksStoreCompactAndSurviveRestart` (`store_local_store_test.cpp:133`), `...RefusesOptionsThatDoNotHoldTogether` (`:805`), `StoreLocalFormat.EncodesPackRecordsAsMachaDoes` (`store_local_format_test.cpp:134`).
- S4 index rebuilt at open: `StoreLocalStore.PacksStoreCompactAndSurviveRestart` (`store_local_store_test.cpp:133`), `StoreLocalStorePresence.PresenceIsExactAcrossARestart` (`store_local_store_presence_test.cpp:134`).
- S5 pack recovery: `StoreLocalStore.PackRecoverySettlesEachKindOfDamage` (`store_local_store_test.cpp:215`).
- S6 compaction: `StoreLocalStore.ACompactionWhoseOldPackSurvivedIsReclaimedLater` (`store_local_store_test.cpp:311`), `...ACompactionWaitsForAPutInItsAppendGap` (`:393`), `NodeLocalMaintenance.DeadPackBytesAreReclaimed` (`node_local_maintenance_test.cpp:126`).
- S7 tombstones and touches: `StoreLocalStore.AgesFollowTheWallClockAndATouch` (`store_local_store_test.cpp:820`), `MigratePack.OnlyTheTagAndChecksumMoveAndAPackMachaWroteOpens` (`migrate_migrate_test.cpp:175`).
- S8 accounting and lock: `StoreLocalStoreDurability.UncleanAccountingRecoveryEstablishesADurableBaseline` (`store_local_store_durability_test.cpp:212`), `...ACleanAccountingCheckpointIsTrustedWithPacks` (`:142`), `Node.TheStorageLockRefusesASecondNodeAndTheMigration` (`node_test.cpp:181`).
- S9 admission: `StoreLocalStore.AdmissionRespectsTheLimit` (`store_local_store_test.cpp:196`), `...AdmissionRespectsTheFreeSpaceReserve` (`:182`).
- S10 pool: `StoreStoragePool.ARefusedPutFallsThroughToABackendWithRoom` (`store_storage_pool_test.cpp:81`), `...RanksBackendsAsMachaDoes` (`:122`), `NodeLocalMaintenance.RebalanceMovesCopiesOntoTheirPreferredBackend` (`node_local_maintenance_test.cpp:81`), `StoreStoragePool.RebalanceNeverDeletesTheLastValidCopyForACorruptPreferredOne` (`store_storage_pool_test.cpp:481`).
- S11 classes: `StoreControlStore.AControlWriteIsNotBlockedByDataExhaustion` (`store_control_store_test.cpp:35`), `StoreIoPressure.ControlCannotEnterTheDataArbiterAtAll` (`store_io_pressure_test.cpp:383`).
- S12 backend identity: `StoreStoragePool.ABackendThatIsNotThisNodesDiskIsRefused` (`store_storage_pool_test.cpp:224`), `...ANonEmptyUnversionedBackendIsRefused` (`:264`), `...ABackendOfflineAndReadoptedLeavesPresenceExact` (`:721`).
- S13 durability domain: `StoreDurabilityDomain.ImmediateDoesNotWaitForTheWindow` (`store_durability_domain_test.cpp:110`), `...BatchableWaitsOutItsWindow` (`:122`), `...OneBarrierServesEveryWaiterInAWindow` (`:148`), `...AMutationAfterACutIsNotClaimedByIt` (`:189`).
- S14 sticky failure: `StoreDurabilityDomain.AFailedBarrierIsSticky` (`store_durability_domain_test.cpp:215`).
- S15 tickets: `StoreDurabilityTicket.EncodesAsProtocol23` (`store_durability_ticket_test.cpp:54`), `StoreStoragePool.ATicketFromAnotherProcessOrIncarnationSatisfiesNothing` (`store_storage_pool_test.cpp:586`), `StoreDurabilityBarrier.AStaleProcessEpochIsRefused` (`store_durability_barrier_test.cpp:134`).
- S16 strict put: `StoreLocalStore.ALooseObjectIsSealedAccountedAndReaffirmed` (`store_local_store_test.cpp:697`), `StoreLocalStoreDurability.AnImmediateReaffirmationFlushesTheProvisionalGeneration` (`store_local_store_durability_test.cpp:391`), `DistributedStore.PersistedReturnsOnTheFirstCopyOnDisk` (`store_distributed_store_test.cpp:756`).
- S17 lazy delete: `StoreLocalStoreDurability.AnAuthoritativeDeleteIsLazilyDurable` (`store_local_store_durability_test.cpp:283`).
- S18 presence: `StoreLocalStorePresence.HasAnswersAPutInProgressWithoutWaitingForIt` (`store_local_store_presence_test.cpp:47`), `...APutThatFailsMidwayIsNeverPresent` (`:88`), `...HasDoesNotConsultTheDeviceAfterWarmUp` (`:116`).
- S19 cache: `StorePersistentCache.TheCacheIsExplicitlyEphemeral` (`store_persistent_cache_test.cpp:54`), `...TheCacheEvictsTheLeastRecentlyUsedAndReopens` (`:125`), `...AMissingOrCorruptCacheIsDiscardedAndRebuilt` (`:184`).
- S20 credit arbiter: `StoreDataWork.TheForegroundReserveHoldsAndALoaderIsServedBeforeSpeculation` (`store_data_work_test.cpp:361`), `...TheBackgroundCeilingCountsLoaderAndSpeculativeNotForeground` (`:333`), `...AWaitWithNoDeadlineFailsAfterAWholeWindowWithNoRelease` (`:142`).
- S21 I/O pressure: `StoreIoPressure.*` (14 cases, `store_io_pressure_test.cpp:64-399`).
- S22 distributed put: `DistributedStore.APutPublishesAtTheFloorAndCopiesPromptly` (`store_distributed_store_test.cpp:115`), `...APutToASilentReplicaReturnsOnTheLocalCopy` (`:233`), `...WriteCopiesTwoUsesAFallbackWhenAPreferredOwnerIsFull` (`:472`), `StoreDistributedStore.MachasFormsSeekWriteCopies` (`:1024`).
- S23 StorageUnavailable: `DistributedStore.NothingCanTakeTheWrite` (`store_distributed_store_test.cpp:910`), the case requirements said was owed.
- S24 deferred and barrier: `DistributedStore.ADeferredWritesBarrierAcrossPeerRestarts` (`store_distributed_store_test.cpp:655`), `StoreDurabilityBarrier.ARestartedPeerIsProbedOnceWithEveryIdThatNamedIt` (`store_durability_barrier_test.cpp:357`), `...AWaitOverManyTicketsAsksEachPeerForOneBarrier` (`:268`).
- S25 placement: `StorePlacementVectors.*` (Macha's vectors), `DistributedStore.AnEdgeNodeNeverOwnsAndItsWritesLandOnOwners` (`store_distributed_store_test.cpp:608`), `StoreRepair.APushPassesAPeerWithNoRoom` (`store_repair_test.cpp:302`).
- S26 prompt replication: `DistributedStore.APutPublishesAtTheFloorAndCopiesPromptly` (`store_distributed_store_test.cpp:115`), `StoreRepair.APromptCopyWaitsForCreditByDefault` (`store_repair_test.cpp:1669`), `StoreDistributedStore.PromptReplicationRetriesAt30_60_120And240SecondsThenGivesUp` (`store_distributed_store_test.cpp:1540`), `...PromptReplicationKeepsAtMost4096Retries` (`:1573`).
- S27 get: `DistributedStore.ConcurrentReadersShareOneFetchAndBuffer` (`store_distributed_store_test.cpp:268`), `...HealsABadLocalCopyFromAPeer` (`:319`), `ReplicaSelector.*` (`store_replica_selector_test.cpp:40-104`).
- S28 put_here: `StoreRepair.APromptCopyReadsWithoutCreditWhenTheNodeSaysSo` (`store_repair_test.cpp:1646`), `StoreRepair.AStoreCallWithNoDeadlineWaitsForCreditPastAnyLimit` (`:1098`), `StoreDistributedStore.PutHereIsLocalFirstAndAPutWhereNoExtentsAreHeld` (`store_distributed_store_test.cpp:1507`).
- S29 CONTROL objects: `DistributedStore.AControlOfferSendsOnlyWhatThePeerLacks` (`store_distributed_store_test.cpp:551`), `...ControlObjectsRecoverOntoAReplicaFromAnother` (`:584`), `StoreControlFetch.*` (`store_control_fetch_test.cpp:65-196`).
- S30 serving side: `StoreMessages.*` (forms), `StoreRepair.ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit` (`store_repair_test.cpp:801`), `StoreClaimLocks.APeersClaimOnAControlObjectAboutToGoIsRefused` (`store_distributed_store_test.cpp:1255`), `StoreRepair.TheServerRefusesInMachasCodes` (`store_repair_test.cpp:2226`): 413, 507, 409 claimed, and 404 against 503 with a backend offline.
- S31 recovery by plane: `Node.OpensAndStopsInOrderOnAFreshStateAndAReopenedOne` (`node_test.cpp:80`), `Node.DeclaresItsOwnReadinessPlanes` (`:311`), `Node.AStageHoldsTheOpenAndTheOwnersProgressTellsAStallFromRecovery` (`:491`).
- S32 fresh genesis: `StoreStateLayout.ANonEmptyUnversionedStateIsRefused` (`store_state_layout_test.cpp:57`), `Node.AnUnversionedStateIsRefusedAndAnUnversionedBackendIsReadyButDegraded` (`node_test.cpp:406`).
- S33 reload: `NodeReconfigure.BackendsAndCacheFollowALiveReload` (`node_local_maintenance_test.cpp:296`), `NodeReconfigure.AReplicationPolicyChangesOnRestartAndRepairConverges` (`:340`).

**Ledger (3.3).**
- L1 trie: `LedgerObjectTrie.TheHashDependsOnlyOnItsRecords` (`ledger_object_trie_test.cpp:126`), `...PublishesMachasRootForAFixedSet` (`:660`), `...OpensATrieMachaSaved` (`:688`).
- L2 persistence: `LedgerObjectTrie.ACrashAtAnyStepOfACheckpointLosesNothing` (`ledger_object_trie_test.cpp:249`), `...RewritesItsNodeFileAndStaysBounded` (`:305`), `...ASnapshotIsFrozenThroughWritesAndRewrites` (`:504`).
- L3 held ledger: `LedgerHeldLedger.AnswersOnceSeededAndCountsRemovals` (`ledger_held_ledger_test.cpp:79`), `StoreLocalStoreLedger.*`.
- L4 holdings identity: `StoreStoragePool.TheHeldIdentityNamesExactlyWhatThePoolHolds` (`store_storage_pool_test.cpp:752`), `...TheHeldIdentityIsTheSameOnAnyNumberOfDisks` (`:813`).
- L5 union: `LedgerTrieUnion.ReadsAsOneTrieOfTheirRecords` (`ledger_trie_union_test.cpp:39`).
- L6 diff: `LedgerTrieDiff.FindsExactlyWhatDiffers` (`ledger_trie_diff_test.cpp:69`), `...CostsWhatDiffersNotTheSize` (`:118`), `...RecordsNotHeldAreWhatNoHeldTrieHolds` (`:144`).
- L7 diff over the wire: `LedgerTrieDiff.WireFormsAreMachas` (`ledger_trie_diff_test.cpp:243`), `StoreRepair.ATrieQuestionNamesAPinnedSnapshot` (`store_repair_test.cpp:2064`), `...ATrieDiffPinsAtMostFourSnapshots` (`:2115`), `...ARootQuestionWhileHoldingsSeedIsRefused` (`:2191`, skipped as root). `...ATrieDiffPinIsDroppedAfterTwoMinutesUnused` (`:2155`).
- L8 survey: `LedgerAvailability.TheSurveyByDiffFindsWhatNoNodeHolds` (`ledger_availability_test.cpp:98`), `...ASurveyAPeerCouldNotFinishKeepsWhatWasUnavailable` (`:65`), `StoreRepair.WhatOnlyAHostOutOfReachHoldsIsUnavailable` (`store_repair_test.cpp:1821`).
- L9 push: `StoreRepair.APushWorksFromWhatPeersHoldWithoutAsking` (`store_repair_test.cpp:350`), `...APushSendsNothingToAPeerTheSurveyCouldNotSettle` (`:270`), `...APushKeepsItsPlaceAcrossANewList` (`:455`).
- L10 pull: `StoreRepair.APullDecidesHeldByIndex` (`store_repair_test.cpp:515`), `...RepairPassesUnavailableAndCountsUnsourceable` (`:871`).
- L11 pacing: `StoreRepair.RepairIsPacedNotStoppedWhileHigherClassesStayBusy` (`store_repair_test.cpp:620`), `...AStepIsBoundedAndYields` (`:569`).
- L12 claims: `LedgerRetention.ClaimsAreCausalDurableAndObservedRemove` (`ledger_retention_test.cpp:85`), `...AClaimFromAnUnseenBranchSurvivesAPartition` (`:140`).
- L13 release slices and prune: `LedgerRetention.ThePruneCursorCannotStarveLaterTombstones` (`ledger_retention_test.cpp:209`), `...PruneKeepsARowClaimedWhileItChecksPresence` (`:241`), `...CheckpointsAfterEnoughChanges` (`:302`).
- L14 retention ledger: `LedgerClaimWalk.TheRetentionLedgerPublishesHorizonsAndRefusesAnIncompleteRelease` (`ledger_claim_walk_test.cpp:163`).
- L15 reference counts: `LedgerReferenceCounts.FollowAndSurviveARestart` (`ledger_reference_counts_test.cpp:48`), `HeadsCensus.FollowEqualsWalkAfterAnySequence` (`heads_census_test.cpp:100`).
- L16 referenced sets: `ContractGates.ReferencedSetsAreSortedUniqueAndPerClass` (`contract_gates_test.cpp:55`).
- L17 inventory horizon: `ContractGates.TheInventorySplitsRetirementsByWhetherDataStillRefersToThem` (`contract_gates_test.cpp:74`), `LedgerCollector.AnInventoryIsNeverUsedDestructivelyInThePassThatBuiltIt` (`ledger_collector_test.cpp:174`).
- L18 release horizon: `ContractGates.TheReleaseHorizonCarriesItsHeadAndClock` (`contract_gates_test.cpp:89`), `LedgerCollector.AnIncompleteReleaseIsNeverPublished` (`ledger_collector_test.cpp:240`).
- L19 gates: `ContractGates.TheTombstoneGateIsThePassConditionOverEveryInput` (`contract_gates_test.cpp:148`), `...TheControlGate...` (`:159`), `...TheDataGate...` (`:173`).
- L20 GC sweeps: `LedgerCollector.TheControlSweepReleasesRemovesAndPrunes` (`ledger_collector_test.cpp:421`), `StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance` (`store_storage_pool_test.cpp:288`), `NodeMaintenance.TheDomainsGraceDecidesWhenAnUnreferencedObjectGoes` (`node_maintenance_test.cpp:109`).
- L21 claimed copy untouchable: `StoreStoragePool.AClaimedCopyIsUntouchable` (`store_storage_pool_test.cpp:624`), `LedgerCollector.ARetentionClaimIsAPhysicalGcBarrier` (`ledger_collector_test.cpp:128`).
- L22 sightings: `LedgerUnreferencedSince.*` (7 cases, `ledger_unreferenced_since_test.cpp:52-179`).
- L23 claim walk: `LedgerClaimWalk.MatchesTheWalkItReplacedExhaustively` (`ledger_claim_walk_test.cpp:354`), `StoreRepair.ARetainedMissingCopyIsRestoredWithoutReachability` (`store_repair_test.cpp:724`).
- L24 horizon builder: `LedgerHorizonBuilder.*` (6 cases, `ledger_horizon_builder_test.cpp:67-180`).
- L25 budgets and cursors: Dannan's `sched/`; `LedgerClaimWalk.TheRetentionLedgerPagesEveryLiveClaimOnce` (`ledger_claim_walk_test.cpp:79`), `HeadsTree.AResumedPageReadsOnlyItsOwnPath` (`heads_tree_test.cpp:758`).

**Heads (3.4).**
- H1 commit hash: `HeadsRecord.TheHashStreamsTheCanonicalEncoding` (`heads_record_test.cpp:56`).
- H2 snapshot or delta: `HeadsCommit.ABodyIsADeltaOnlyWhenStrictlySmallerThanTheRecord` (`heads_space_test.cpp:150`), `HeadsReplica.AMergeDeltasPrimaryMayPrecedeItsGeneration` (`heads_replica_test.cpp:290`).
- H3 merge parents: `HeadsRecord.SixtyFourMergeParentsPassAndSixtyFiveAreRefusedEverywhere` (`heads_record_test.cpp:102`).
- H4 acceptance: `HeadsCommit.AMutationKeepsMachasOrderAndIsAcceptedHereAlone` (`heads_space_test.cpp:87`), `HeadsExchange.CommitReplicasAreOrderedLocalThenNearest` (`heads_exchange_test.cpp:610`).
- H5 certificates: `HeadsRecord.ACertificateHoldsOrIsRefused` (`heads_record_test.cpp:194`).
- H6 accepted set: `HeadsReplica.AHeadWithNoClockIsNobodysAncestor` (`heads_replica_test.cpp:677`), `...ThePreferredHeadIsTheHighestGenerationThenTheHigherHash` (`:1778`), `...AHeadWithTheSameClockIsNotAnAncestor` (`:2774`).
- H7 exchange: `HeadsWireFixture.ASurveyOfTwoRealHeadsImportsBoth` (`heads_messages_test.cpp:601`), `HeadsExchange.AnUnreconstructableHeadIsRepairedLiveFromAPeer` (`heads_exchange_test.cpp:893`), `HeadsReplica.AnUnreconstructableHeadIsRateLimited` (`heads_replica_test.cpp:556`).
- H8 reconciliation: `HeadsReconcile.TheFoldIsTheSameInAnyArrivalOrder` (`heads_reconcile_test.cpp:866`), `HeadsWireFixture.TheFoldMintsMachasOwnMergeFromItsTwoHeads` (`heads_messages_test.cpp:682`).
- H9 tree-backed merge: `HeadsReconcile.TwoTreeBackedBranchesKeepTheSpace` (`heads_reconcile_test.cpp:199`), `HeadsCost.MergeFollowsWhatDiffersAndNotTheKeys` (`heads_cost_test.cpp:358`).
- H10 set aside: `HeadsReconcile.AHeadThatCannotBeMergedIsSetAside` (`heads_reconcile_test.cpp:467`), `HeadsReplica.AHeadSetAsideForTheHorizonIsDropped` (`heads_replica_test.cpp:907`).
- H11 conflicts: `HeadsReconcile.AConflictsValuesAreClaimedAndRootedWhileItStands` (`heads_reconcile_test.cpp:1475`). Their content is the owner's merge (FS).
- H12 dots: `HeadsCommit.ARetryKeepsItsDotAndAnAcceptedAttemptIsNotMadeTwice` (`heads_space_test.cpp:200`), `HeadsCommit.ADotIsDurableBeforeAnythingElseAndSurvivesARefusal` (`:1056`), `HeadsReplica.ABrokenAuthorChainStartsANewAuthor` (`heads_replica_test.cpp:1075`).
- H13 legacy clock: FS, Macha's merge reads it behind `heads::Merge`; carried as opaque bytes (`HeadsConformance.MachasFramesCertificatesAndBranchesAsOpaqueBytes`, `migrate_heads_test.cpp:1119`). No Dagda case by design.
- H14 mutate: `HeadsCommit.AMutationExtendsItsOwnHeadAmongSeveral` (`heads_space_test.cpp:369`), `...AMutationThatChangesPolicyIsRefused` (`:432`).
- H15 replicator: `HeadsSpace.AnEveryNodeSpaceIsOfferedToEveryActiveNode` (`heads_exchange_test.cpp:1161`), `HeadsExchange.AHistoryPushFillsItsWindowOfEightAndNoMore` (`:510`).
- H16 truncation: `HeadsReplica.TruncationFiresAtItsThresholds` (`heads_replica_test.cpp:1734`), `HeadsReconcile.HistoryIsNotTruncatedWhileTwoHeadsAreHeld` (`heads_reconcile_test.cpp:526`), `HeadsReplica.CompactionBoundsTheJournal` (`heads_replica_test.cpp:1606`).
- H17 bounded memory: `HeadsRecord.APayloadCopyIsShared` (`heads_record_test.cpp:35`), `HeadsReplica.TheCacheHoldsAtMostSixtyFourAndPinsTheHead` (`heads_replica_test.cpp:2702`), `...HistoryPayloadsAreDiskBackedAndByteBounded` (`:1255`).
- H18 tree: `HeadsTree.TheRootIsAFunctionOfTheEntrySetAndNothingElse` (`heads_tree_test.cpp:132`), `HeadsTreeVectors.*` (Macha's 25 vectors).
- H19 tree nodes as CONTROL: `HeadsControlNodeStore.ACommitHoldsEveryNodeItsRootNamesAndWritesOnlyWhatIsNew` (`heads_control_node_store_test.cpp:81`), `...ACommitThatCannotHoldANodeThrowsSoNoRootIsPublished` (`:107`).
- H20 replica policy: every-node built (`NodeHeads.TheContractOpensAnEveryNodeSpaceAndRefusesReplicas`, `heads_node_test.cpp:250`); `replicas: N` refused with a reason (`HeadsSpace.AReplicasPolicyIsRefusedWithAReasonUntilHoldersExist`, `heads_space_test.cpp:736`). Macha has one every-node space.
- H21 memory reads: `HeadsSpace.TheCurrentViewReadsNothing` (`heads_space_test.cpp:761`).
- H22 commit claims: `NodeHeads.ACommitIsPublishedOnlyOnceItsObjectsAreClaimed` (`heads_node_test.cpp:406`), `HeadsCommit.AnObjectNobodyPresentHoldsRefusesTheCommitAndSaysWhich` (`heads_space_test.cpp:509`), `HeadsReconcile.AMergeClaimsWhatItIntroduces` (`heads_reconcile_test.cpp:598`).
- H23 retirement batches: `HeadsRetirement.ABatchIsMachasLayout` (`heads_retirement_test.cpp:39`), `NodeHeads.RetirementsLiveInBatchesInATreeBackedHead` (`heads_node_test.cpp:545`).
- H24 generation advert: `HeadsExchange.ANoticeMakesAViewStaleBeforeItsLifetimeEnds` (`heads_exchange_test.cpp:1027`), `...ADecodedViewsLifetimeRecoversAMissedNotice` (`:972`).
- H25 cache seed: `HeadsSeed.*` (9 cases, `heads_seed_test.cpp:58-306`), `HeadsReplicaBytes.AMachaSeedOpensAndRecovers` (`heads_replica_bytes_test.cpp:500`).
- H26 per-peer order: `HeadsMessages.TheThreeMutationsAreOrderedAndNoOther` (`heads_messages_test.cpp:129`), `HeadsExchange.ACommitOvertakesImportsAndOnePeersOrderHolds` (`:804`).
- H27 genesis: `HeadsGenesis.TheConstantIsMachasOnMachasSpace` (`heads_record_test.cpp:234`), `HeadsFounding.*` (`heads_exchange_test.cpp:166-258`).
- H28 write-floor advert: `HeadsExchange.NodesSeekingDifferentCopyCountsShareOneSpace` (`heads_exchange_test.cpp:641`), `...AWriteCopiesAdvertChangeIsAMembershipEvent` (`:663`).
- H29 scope: `HeadsMessages.ARefusedScopeStoresNothingAndSaysWhy` (`heads_exchange_test.cpp:1474`), `StoreScope.*` (`store_scope_test.cpp:67-179`). With no source installed, node principals write: Macha's case.
- H30 no rollback: `HeadsReplica.AnAncestorChangesNothing` (`heads_replica_test.cpp:1833`), `...AnUnrelatedCommitIsADivergenceNeverAReplacement` (`:1877`).

**Leases (3.5).** Torrent stays in Macha through the port (the maintainer), so Macha calls none of these at the port; they are built for after it.
- E1 claimed collection: `LeaseClaimedCollection.RecordsJoinGroupByGroup` (`lease_claimed_collection_test.cpp:212`).
- E2 claim order: `LeaseRecord.TheOrderIsEpochThenEarlierThenLowerNode` (`lease_lease_test.cpp:39`).
- E3 tombstone window: `LeaseClaimedCollection.ATombstoneExpiresStrictlyPastItsRetention` (`lease_claimed_collection_test.cpp:486`), `LeaseTable.AReleasedNameIsSweptAfterItsWindow` (`lease_lease_test.cpp:145`).
- E4 three-way merge: `LeaseClaimedCollection.AnErasureStandsUnlessTheOtherSideChangedTheRecord` (`lease_claimed_collection_test.cpp:350`).
- E5 duplicates: `LeaseClaimedCollection.TheDuplicateRuleKeepsTheEarlierTask` (`lease_claimed_collection_test.cpp:369`).
- E6 decode bounds: `LeaseClaimedCollection.DecodeKeepsItsBounds` (`lease_claimed_collection_test.cpp:442`).
- E7 takeover: `LeaseKeeperTest.AnAbsentHoldersLeaseIsTakenAtTheNextEpoch` (`lease_keeper_test.cpp:87`), `...RankedTakersWaitRankTimesTheStep` (`:115`), `LeaseMembership.AbsenceComesFromTheRoster` (`lease_membership_test.cpp:85`).
- E8 owner re-claim: FS, the owner's phase rule; near it, `LeaseKeeperTest.AHolderThatReturnsStandsDownAndIsFenced` (`lease_keeper_test.cpp:131`).
- E9 fence: `LeaseFence.*` (`lease_fence_test.cpp:28-68`). New; unused unless asked for.
- E10 per-origin counters: not built (Lugh's; Macha does not need it).

**Context (3.6).**
- C1 executor and deadline: `Context.EachNounCompletesOnTheCallersStrandOnATwoNodeCluster` (`context_test.cpp:73`), `DistributedStore.CompletionsArePostedNeverRunInline` (`store_distributed_store_test.cpp:929`), `NodeHeads.TheContractCompletesOnTheCallersExecutorAndByTheDeadline` (`heads_node_test.cpp:274`).
- C2 nouns: the same `Context` case.
- C3 reachability provider: `StoreRepair.TheSurveyGoesThroughTheProvider` (`store_repair_test.cpp:2017`), `NodeMaintenance.WithNoProviderTheCollectorRemovesNothingAndSaysSo` (`node_maintenance_test.cpp:82`).
- C4 Status: `Node.PublishesTheStoresFiguresAndGoesReady` (`node_test.cpp:277`), `NodeSeams.MachasOtherMaintenanceFiguresArePublished` (`node_seams_test.cpp:312`). Macha's Status JSON is the facade's; not shown.
- C5 registration: `Node.RegistersDagdasMessageTypesBeforeTheDannanNodeStarts` (`node_test.cpp:235`), `StoreMessages.TheNumbersAre512PlusProtocol23s` (`store_messages_test.cpp:186`), `HeadsMessages.OffRegistersNothingSoAnOwnersOwnRegistrationStands` (`heads_messages_test.cpp:181`).

**Macha 0.92.3 and 0.92.4 (3.7).**
- S34 writer waits for the accounting scan, S35 held ledger before the scan, S36 scan flags under the mutex, L25 (3.7's) seed order: built, no case (Macha has none; the windows need a fault seam).
- L26 striped claim guard: `StoreClaimLocks.*` (`store_distributed_store_test.cpp:1079-1308`).
- S37 refilled lists: equivalent by `shared_ptr` identity, no case.
- L29 delete marks, no device wait under a stripe: `StoreClaimLocks.ADeleteMarksItsObjectAndHoldsNoStripe` (`store_distributed_store_test.cpp:1151`), `...AClaimFindsAMarkedObjectNotPresent` (`:1186`).
- S38: `StoreRepair.AStopEndsTheSurveysWalk` (`store_repair_test.cpp:2442`). L27: `LeaseClaimedCollection.ATombstoneFromAClockAheadIsKeptItsWindow` (`lease_claimed_collection_test.cpp:501`). L28: `LeaseClaimedCollection.ADeltaCannotInstallARecordTheSnapshotRefuses` (`:516`).

### Gaps in the trace

Code exists for each; what no case pins. S26, S28, S30 and L7 were here at `6ae4e7f` and are pinned since (`c5ad7db`, `ff77156`,
`1adc6d1`; the cases are in their rows above).
- **S34, S35, S36, 3.7's L25, S37:** built, no case, as in Macha.
- Not built, by design: E10 (Lugh's), `replicas: N` spaces (H20), the owner's FS rules (H13, E8, H11's content).

## What the port must do, checked against your list (`../macha/TODO/2026-11-08-port.md:83-99`)

Each item read against Dagda's code; corrections marked.

- **The 23 numbers** Dagda registers at stage one: correct. `dagda/store/messages.h:39-70`: 515, 516, 517, 525, 531, 532, 534, 535,
  544, 552, 557, 559, 561, 615, 616, 619, 630, 633, 635, 637 and Dagda's own 768, 769, 770 (no protocol-23 number). Pinned by
  `StoreMessages.TheNumbersAre512PlusProtocol23s` (`store_messages_test.cpp:186`).
- **Add: the later stage registers 15 more.** With `NodeOptions::heads` on, `heads::message::Numbers` (`dagda/heads/messages.h:37-59`)
  registers 15 types from 784 by default. Macha passes its own numbers in `NodeOptions::exchange.numbers` and removes those from its
  own registrations at the same moment; `register_types` throws on a number already taken, refusing the node.
- **`write_copies` 1 set explicitly:** correct. `DistributedStoreOptions::write_copies = 2` (`distributed_store.h:234`).
  `metadata_write_copies` (`:236`) and `ExchangeOptions::write_copies` (`heads/exchange.h:52`) also default to 2: set them from
  Macha's `dht.metadata_write_copies`.
- **ReachabilityProvider:** correct. `contract::ReachabilityProvider` (`contract/reachability.h:72`), registered by
  `Node::register_reachability(domain, provider)` (`node.h:392`); `known_generation` from `Exchange::known_generation()`
  (`heads/exchange.h:239`, the replica's or the peers' advertised, whichever is higher), reached by `Node::spaces()->exchanges()`.
- **Never `Objects::remove` on Macha's paths:** correct. It is the instant delete (`context.h:182-198`, `store/remove.h`), with no
  grace; Macha's deletes go to the collector.
- **The control store adapter:** correct. `Node::control()` is a `store::LocalStore&` (`node.h:362`); Macha's metadata code takes
  it through a type adapter.
- **`.macha.lock` held by Dagda's `Node` only:** correct. `store::storage_lock_name` is `.macha.lock` (`local_store.h:59`); the node
  takes it on the state, every backend, the control store and the cache (`node.cpp:150-183`), and the migration takes the same.
- **`startup.data_storage_degraded` from `dagda.storage.degraded`:** correct (`node.cpp:1050`; `Node.NoBackendOnlineIsReadyButDegraded`, `node_test.cpp:325`).
- **`heads.reconcile_us` and `heads.reconcile_wait_us`** to Macha's `metadata.reconcile_*`: correct; they are observations
  (`exchange.cpp:772`, `:877`), not Status figures.
- **Later stage: `NodeOptions::exchange.numbers`, `SpaceSpec::unaddressed`, advert names:** correct. `SpaceSpec::unaddressed`,
  `generation_advert`, `write_copies_advert` (`heads/space.h:305-311`); empty names default to Dagda's per-space names
  (`exchange.cpp:54-55`), so Macha sets its own.
- **Macha's merge behind `heads::Merge`:** correct (`heads/space.h:100`).
- **Custody provisioned by Macha's deploy:** correct. A node without custody is refused (`Node.ANodeWithoutCustodyIsRefused`,
  `node_test.cpp:167`); one without a certificate opens and advertises no wrap key (`NodeSeams.ANodeWithoutACertificateOpensAndAdvertisesNoWrapKey`, `node_seams_test.cpp:56`).
- **Add: `NodeOptions::heads` off at stage one** (`node.h:265-269`), while Macha's own replica runs: the node declares no
  `metadata` plane and registers no heads type. On at the later stage.
- **Add: the head space is `macha/namespace`,** the migration's default (`heads_metadata.h:65`). Macha's facade opens it under that
  name; another name needs `dagda-migrate --space` to match.
- **Add: the refusal.** A node with `heads` on refuses to open while `<state>/metadata` or `<cache>/metadata/current.meta` exists,
  naming `dagda-migrate --heads --state <state>` (`e74071c`). The later stage runs `--heads` before the first start with heads on.
- **Add: the publication budget.** Macha's publication passes `WriteOptions::no_progress_budget = 30s` (its
  `fuse.publication_no_progress_deadline`) and its shared counter as `WriteOptions::progress` (`6635618`). Other writes leave it 0.
- **Add: the store's calls name your purpose deadlines (0.1.1).** After the cutover the store's peer calls are Dagda's, so
  `call_deadline.hpp` no longer reaches them, and the bridge turns Dannan's node-wide watches off. Dagda names the maintainer's set
  (2026-11-11) itself, as `DistributedStoreOptions` (`distributed_store.h`): `object_transfer_call` 30 min on a put, a get and
  a repair push; `presence_call` 2 min on `have_objects` and `have_valid_objects`; `delete_call` 2 min on `delete_object`;
  `metadata_call` 5 min on `put_control_object` and `get_control_object`. The two you wait on synchronously (a CONTROL fetch, a
  delete) also end after `control_no_progress`, 30 s idle, as your `control_no_progress_deadline` does. Claims, `has_on`'s
  `have_object` and `have_control_objects` keep `control_call` 30 s, your `bounded_control_call`. The trie question
  (`AvailabilityOptions::call` 2 min, `call_no_progress` 30 s) and the exchange (`ExchangeOptions::call_deadline` 5 min;
  `call_no_progress` 30 s on its synchronous calls; commit calls keep `write_stall`) follow. Cases that failed first:
  `StoreRepair.APutToAPeerEndsAtTheObjectTransferDeadline`, `...AReadFromAPeerEndsAtTheObjectTransferDeadline`,
  `...APresenceBatchToASilentPeerEndsAtThePresenceDeadline`, `...ADeleteAskedOfASilentPeerEndsAtTheNoProgressWindow`,
  `...AControlFetchFromASilentPeerEndsAtTheNoProgressWindow`, `...ATrieQuestionToASilentPeerEndsAtTheNoProgressWindow`,
  `HeadsExchange.AProbeToASilentPeerEndsAtTheNoProgressWindow`. Set nothing: the defaults are your values.
- **`DistributedStoreOptions::call_no_progress` stays 300 s** on transfers: one that moves no byte for that long fails before
  its 30 minutes. Your port has no idle bound on a transfer (`data_no_progress_deadline` 0); Dagda's rule forbids a wait
  with none. Named difference: a transfer silent for 5 to 30 minutes ends in Dagda and goes on in your port.
- **Add: maintenance.** While Macha's loop runs (until 8c), `MaintenanceOptions::automatic = false` (`node.h:101`) and Macha drives
  the node's steps and local stages (`begin_local_stages` and the step calls; `StepsBed.EveryStepRunsInMachasOrder`,
  `maintenance_steps_test.cpp:893`). Pass Macha's configured values, not Dagda's defaults: live-b runs
  `idle_bandwidth_fraction: 0.5` (Dagda's default 0.10, `node.h:108`). Your loop must carry 0.93.6's yielded wake and its own busy
  wake. The test's copy now has both, so `maintenance_steps_test.cpp` is a model of the loop the port keeps, and its two cases
  (above) state what the loop owes Dagda's steps.
- **Add: build on Dannan 0.5.3 and Debian's Boost 1.83.** No private Boost prefix on the Pis.
- **Add: the device reserve and admission** are on by default (`NodeOptions::admission`, `node.h:239`); `Node.TheDeviceReserveCanBeOff`
  (`node_test.cpp:542`). Judged by the baselines; off if worse.
- **Add: `ExchangeOptions::joiner`** is Macha's `bootstrap` (`heads/exchange.h:49`).

## The migration (`dagda-migrate`)

Shown by cases:
- Forward in place, no second copy: tags and pack checksums overwritten at fixed offsets, nonce and ciphertext kept
  (`MigrateLoose.OnlyTheTagMoves`, `migrate_migrate_test.cpp:115`; `MigratePack.OnlyTheTagAndChecksumMoveAndAPackMachaWroteOpens`,
  `:175`); ledgers re-sealed file by file, peak extra space one ledger file (`docs/migration.md:269-292`); metadata peak one history
  file (`MigrateHeads.ForwardKeepsNonceAndCiphertextAndEveryFrameOpens`, `migrate_heads_test.cpp:330`, asserts
  `metadata_peak_extra_bytes`). No case measures the peak for the objects' and ledgers' pass on a full disk.
- Reverse: `MigrateBack.ThereAndBackLeavesTheObjectsAndMarkersAsTheyWere` (`migrate_migrate_test.cpp:408`),
  `MigrateHeads.ReverseRestoresWhatTheStoreReads` (`migrate_heads_test.cpp:569`), `MigrateCompare.ThereAndBackComparesEqualUnderLayoutV18` (`migrate_compare_test.cpp:901`).
- Crash after every step, both ways, and a crash in the second run: `MigrateCrashPhases.*` (15, `migrate_crash_test.cpp:373-624`),
  `MigrateHeadsCrash.*` (5, `migrate_heads_test.cpp:459-545`), `MigrateCrash.*`.
- Resume of the comparison pass: `MigrateComparePass.APassStoppedAtAnyCheckpointResumes` (`migrate_compare_test.cpp:678`);
  `--resume` from a real partial through the command line, on a fixture of 600 objects killed by SIGKILL after two durable
  checkpoints: `MigrateHeadsTool.APassKilledMidWayResumesFromItsPartial` (`migrate_heads_test.cpp:1219`).
- The comparison: `MigrateCompare.AStateMigratedWholePassesWithEveryCategorySized` (`migrate_compare_test.cpp:120`), a damage case per item
  (`MigrateCompareDamage.*`, 41; `MigrateHeadsCompareDamage.*`, 12); every real fixture moved through both stages compares PASS
  and back PASS (`MigrateHeadsReal.EveryRealStateMovesComparesAndComesBack`, `migrate_heads_test.cpp:1057`).
- Refusals before any write: `MigrateState.AWrongKeyAndAnUnrecognisedDiskAreRefusedBeforeAnyWrite` (`migrate_migrate_test.cpp:572`),
  `...ABackendOfAnotherNodeIdIsRefusedBeforeAnyWrite` (`migrate_migrate_test.cpp:370`), `MigrateHeads.RefusesBeforeTheFirstWrite` (`migrate_heads_test.cpp:621`).
- A dry run over live-a's state (scratch copy, 0.92.4, 2026-11-05): exit 0, nothing refused, 3,809 loose and 314,390 pack records
  open under v18 (`COMPLETED.md:546-556`). DATA was a sample.

**The rehearsal, 2026-11-12** (the maintainer: now, on build-host, the copy and the cluster key streamed there and deleted after):
live-a's state copy `/root/macha-state-20261105T145340Z` (0.92.4, 3.3 GB: the whole state, the cache's seed, a DATA sample of
48 loose objects and one pack), worked on a copy of itself, with `dagda-migrate` at `0.1.0` (plain arm64 build), fresh custody
holding live-a's node id. Logs in `build/scratch/rehearsal202/`. Every step exit 0 and every comparison `RESULT: PASS`:
1. Before manifest, 29 s.
2. A second manifest pass SIGKILLed with a 253,927-byte partial, resumed by `--resume` through the command line, compared with
   the before manifest (18 and 18): PASS.
3. Forward, stage one: the first run SIGKILLed after 3 s with the marker at `dagda-state-layout-going-forward`, finished by a
   second run (32 s). 3,809 loose and 314,390 pack records migrated, 1,883,019 touches and tombstones, 9 ledgers re-sealed,
   3 markers. Compared: PASS, every category sized and SAME (held 317,544 control and 1,248,384 data records, claims 316,469
   and 1,243,017, reference counts 1,227,311 and 12,658, the control store's 2,196,802 pack records).
4. `--heads`: 154 history frames and 4 files moved, the seed renamed to the space's. Compared: PASS, the metadata categories
   (author, certificates, checkpoint, heads, history 154, markers, seed) SAME.
5. `--reverse` (29 s), compared with the before manifest (18 and 18): PASS. `metadata/` and `current.meta` back.

Still not shown: the bytes of today's 0.94.0 disks (the copy is 0.92.4; the port's Dannan files were not in it, and read as
"other files" byte for byte), and time and space at 4 TB on the USB disks. A real DATA disk is a sample here.

Found by the rehearsal, a gap, not fixed: nothing stops two manifest passes writing the same `<manifest>.partial` at once (my
first script did that by killing a wrapper and not the pass). The result is a manifest that fails its own checksum and a
rename that fails, so it is caught, not silent. A pass could hold an exclusive lock on the partial.

Rollback: Macha's binary comes back only after `--reverse` has finished (your list's "Rollback" stands).

## Open and unexplained

- **The heap aborts.** Two Release-only silent SIGABRTs in the delete stress case with in-process restarts, and one more with
  restarts and the instant delete both off (one in 14 runs, `HeldLedger::held_with_prefix` from `verify_step`). ASan, UBSan and the
  malloc checkers found nothing in 14 runs; no writer found, no mechanism, no fix (`COMPLETED.md:245-251`, `COMPLETED.md:2076-2081`). The
  stacks share only reading an `ObjectTrie` and freeing its copies. They do not explain live-a's heap corruption, nor clear the
  moved code; live-a's one store frame is `ObjectTrie` under `StoragePool::held_view`. No stress run since the maintainer's word.
- **The `mutex lock failed` abort** in one 2026-11-05 macOS TSan run: not answered; not seen in the 2026-11-10 TSan run either.
- **The trie pin clock**, a difference from Macha: the server's pins age on the node's time source (`1adc6d1`); Macha's on the
  real clock. Live nodes run the real clock, so they behave the same.
- **live-b's outage** on 2026-11-11, from about 01:30 to 18:34, under Dagda's ASan build and fixture run beside Macha: cause
  unknown, because the journal was volatile. It is persistent now (2026-11-12), so another outage will leave its cause. Its twin
  build-host under-volted once under the same kind of build: the Pis' supplies are worth a look before the port loads them.
- **The pool's first-empty-answer `get`** (kept, above).
- **The stress case's end check** compares two views taken at different moments: its race, not the store's (`COMPLETED.md:270-271`).
- **The store's journal lines: mapped and restored (0.1.2).** Every store line Macha writes was mapped against Dagda's
  (`TODO/2026-11-12-journal-mapping.md`): 65 the same, 23 reworded, 39 missing, 18 not Dagda's. The maintainer: restore everything. Each now
  has Macha's text and level but those its section 8 names, each with its reason (the survey line's `table=`, Macha's path
  table's; `reason=retained_memory`, a gate Dagda has not). Your sink drops the `dagda.` scope at the cutover, so they read
  as Macha's. The golden sample holds no store warning or error, so it confirms none of the trouble lines either way.

## What I could not show without a running port

1. **Macha's merge hash for hash** beyond the one fixture: `HeadsWireFixture.TheFoldMintsMachasOwnMergeFromItsTwoHeads`
   (`heads_messages_test.cpp:682`).
2. **Replay of delta frames through Macha's codec:** Dagda replays the chain of opaque bytes; applying Macha's deltas is the port
   branch's (`COMPLETED.md:430-431`).
3. **Everything end to end:** Status bytes and the widened goldens, client contracts, Macha's own suite on Dagda.
4. **Performance against the baselines** (`../macha/TODO/2026-11-06-pi-baselines.md`): no Dagda figure through the mount or the API;
   the comparison rule is set on the port's first figures (the maintainer). Dagda's own: loopback and scale cases only.
5. **A real state imported and started:** no Dagda node has opened a migrated live-a or live-b state.
6. **The migration rehearsal** listed above.
7. **Reliability over time** on real hardware: no soak.
8. **live-a's heap corruption:** in Macha's code at 0.93.4; nothing here shows it absent from Dagda. Dagda's own suite ran clean
   under ASan and UBSan on arm64 at `0.1.0`, which is not the same thing.
