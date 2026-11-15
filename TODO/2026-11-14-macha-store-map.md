# Macha's store onto dagda::Node and Context: the call-by-call map (2026-11-14)

## The bar (the maintainer, 2026-11-14, to the Dagda session)

"Port through dagda::Node and Context is correct, but you must make ABSOLUTELY SURE that Macha can perform exactly as
well as it did before with Dagda as its storage backend. It is not enough for the concepts and primitives to be
'sorta' like Macha."

"There must be no 'shims' in Macha to make Dagda work for it. Both: parts of Macha may need to be rewritten to use
native Dagda effectively, and that's the NO LOSS of performance/reliability/etc - AND - Dagda MUST be capable of
handling DHT storage AT LEAST AS WELL as Macha."

So two requirements, both binding: Macha uses Dagda natively, with no adapter layer, rewritten where it must be and
losing nothing; and Dagda, measured on its own terms against Macha's store on the same workloads, is at least as good
a DHT store in every respect.

## The route (the maintainer, 2026-11-14)

Most of Macha's store calls (a write with a known id, put_here, deferred writes and many-ticket waits, hydrate,
cache_local, ensure_local, the activity clocks, reads with no deadline on the caller's thread, the collector's steps,
the retirement helpers, claimed collections) were reachable only through `Node`'s parts (`node.h:359`, "for an owner's
maintenance and for tests"); `Context` offered a content-addressed write and read with a required deadline, `has`,
`remove` and one ticket's wait, run on the store's four worker threads. **Ruled: `Context` grows to serve all of it,
with Macha's semantics** (a blocking form on the caller's own thread, no worker hop; a read with no deadline under the
admission window; many-ticket waits; known-id writes), and Macha calls `Context` only: one contract for Macha, Lugh and
Lir, contract version 2. Every row below whose Dagda side is a `Node` part is a gap until `Context` serves it.

## Method

Against Macha's develop `f0597a4` (0.94.2's code), extracted read-only at `build/macha-f0597a4/`. Written with the Macha
session, split by subsystem (agreed 2026-11-14): Macha writes the callers' side (filesystem and FUSE, catalogue and
hints, metadata, the namespace control store, ingest, torrent, media, HTTP and status, the composition root); Dagda
writes the store's side, below. Each reviews the other's against code, not against a summary. The Macha session's
`build/store-api-map.md` is a starting point only: its verdicts are unverified (four were wrong).

Every row states what Macha's callers rely on (inputs, results, errors, durability, ordering, threads, deadlines, the
log lines and figures operators and tests read), the Dagda call through `dagda::Node` or `Context` that serves it, and
a verdict: SAME, DIFFERS (exactly how) or MISSING, with file:line on both sides and the cases that pin it. Every gap is
fixed in Dagda, test-first (the maintainer: "fix gaps as found"), and the map records the fix. Nothing is inferred; what could not
be verified says so.

The map is the first proof. The others (the maintainer, 2026-11-14): Macha's own suites as the gate on Dagda; the side-by-side
harness on the Pis (Macha's store and Dagda on identical workloads), then the port on live-a and live-b against
Macha's live baselines; the in-place migration on copies of real state; reliability under crash, restart and
partition. Key custody: Macha's nodes get Dannan key custody at the cutover.

## The store's side (Dagda's), by subsystem

| Subsystem | Section | State |
|---|---|---|
| Subsystem | Section | Rows (SAME / DIFFERS / MISSING) | Gaps |
|---|---|---|---|
| DistributedStore: put, get, deferred writes and tickets, the barrier, write copies, durability, reads and the server's handlers | `macha-store-map/distributed-store.md` | 41 (17 / 23 / 1) | 15 |
| StoragePool and LocalStore: durability domain, presence index, sealed journal, layout, device reserve, backends, scrub | `macha-store-map/pool-and-local-store.md` | 58 (43 / 13 / 2) | 9 |
| Retention, claims and reference counts; the ledgers (held ledger, object trie, trie diff and union, sightings, horizon) | `macha-store-map/retention-and-ledgers.md` | 46 (26 / 20 / 0) | 10 |
| Availability and the survey, repair, maintenance's store stages and pacing (node loop) | `macha-store-map/availability-repair-maintenance.md` | 43 (23 / 15 / 5) | 13 |
| GC and the deletion path (claim_deletion, the collector, tombstones) | `macha-store-map/gc-and-deletion.md` | 29 (15 / 12 / 2) | 9 |
| Cache, io pressure, data credit and admission, the device reserve | `macha-store-map/cache-pressure-credit.md` | 50 (36 / 13 / 1) | 8 |
| **All six (first pass, by six readers, 2026-11-14)** | | **267 (160 / 96 / 11)** | **64** |

Each section's claims are the readers'; the sharpest are checked against code before a fix, and the ones checked so far
held: the ticket cap (`distributed_store.h:276`, `.cpp:711-719`), Context's reach (`node.h:359`), the GC loop's
dropped `!metadata_dirty` (`node.cpp:552` against Macha `maintenance.cpp:575`), serial DATA claims (`distributed_store.cpp:2256-2260`
against Macha's `std::async` per node, `:924-954`), and the backend lock creating a missing mount's directory (Dannan's
`StateLock`, `node_identity.cpp:25`).

## The callers' side (Macha's)

Written by the Macha session: `../macha/TODO/store-map-callers/` at Macha `d53ddfe` (README.md the index; six sections:
filesystem, fuse-and-hydration, catalogue-and-media, metadata-and-control, ingest-and-torrent, http-status-and-root;
222 rows, verdicts against `Context`, GAP where only a `Node` part serves). Against Macha `f0597a4` and this branch at
about `0d9a4ff`. To be reviewed here against code, row by row.

Verified by Macha, and checked here:
- The state lock: Macha's Service flocks `<state>/.macha.lock` and the node's storage lock flocks the same root, so in
  one process the node is refused "in use". Macha's side: its root rewrite drops its own lock (one lock, one owner).
  One state directory is ruled (Macha's `TODO/2026-11-13-store-cutover.md:14-16`: "one state directory (Macha's
  `state_path`, so `--node-state` is `--state`)"); `NodeOptions::state`'s comment and `docs/migration.md` now say so.
  Gap: no case yet opens a node whose Dagda state is Dannan's state directory; one is written with the Context work.
- Read and write requirement 5 (data safety): `WriteOptions::cancelled` and `ReadOptions::cancelled` are raw
  `std::atomic_bool*` (`distributed_store.h:158`, `:197`), read by store workers after an async caller may have
  returned (`distributed_store.cpp:971-974`, `:1826`, `:1912`). Every async form `Context` offers takes a cancellation
  the store co-owns.
- API traffic is viewer activity (Macha's law 2, `docs/principles-and-laws.md:92-99`, ruled 2026-10-07): Macha's HTTP
  server notes every served request and draining body into the store's own clock (`service.cpp:81-82` at f0597a4),
  which repair pacing and DATA admission read through `idle_for`. Dagda's store clock takes only object work, and
  `StoreDistributedStore.ActivityIsObjectWorkInMachasMeaning` (`store_distributed_store_test.cpp:1556-1576`) pins that.
  Its half about Dannan's inbound counting stands (a peer's RPC is not a viewer); its claim that Macha's activity is
  only object bytes does not. Fix with `Context`'s Activity noun: an owner's note of presence by class reaches the
  clock the store, repair and the arbiter read (cache section row 27); the case is amended, not deleted.

Dagda's review of the callers' side (2026-11-14, every row against code; `build/callers-map-review.md`): 280 claims
confirmed, 27 wrong, 14 not reached (rows with no store call). No verdict was wrong; the errors are in claims and
citations around them. Checked here in code before recording:
- Requirement 5 reaches the blocking forms too: `await` (`context.cpp:36-43`) throws `deadline + completion_margin`
  after it began and returns, while the store's job still holds the caller's raw `cancelled` (`distributed_store.cpp:1153`).
  The co-owned cancellation is for every `Context` form, blocking or not.
- The barrier's re-put set (filesystem row 17): Macha takes as definitive "object absent", `peer-unknown` and any
  `local-barrier-failed`, re-deriving a local failure from disk first (Macha `distributed_store.cpp:606-646`,
  `:734-737` at f0597a4); Dagda refuses an unknown node in English ("unknown node", `distributed_store.cpp:1462`) and
  leaves a local failure other than a stale ticket transient (`:1556-1559`), so the wait runs to its deadline. A typed
  loss in `ReplicaEvidence` must carry each of these. Whether a local failure re-derives from disk is the Macha-side-first
  item above (Macha's call), and Dagda matches what Macha settles. **Settled (the maintainer, relayed by the Macha session,
  2026-11-14: "healing is better"):** a failed local barrier re-derives and heals, as Macha's does, never "transient
  until the deadline". A re-derived "durable" must never be false, so `StoragePool::reassert_durable`
  (`storage_pool.cpp:784-810`, which trusts `has()`) gains two guards, each with its case:
  1. A sync failure latched in the domain is kept (`durability_domain.cpp:74`, `:89`, `:102`): the reassertion throws,
     the object is absent and re-put. Holds today.
  2. After a power cut presence is not proof: pack replay accepts a record whose header decodes without reading its
     payload, so a header on disk with its payload lost is "present". Re-derive with an authenticated read before the
     barrier (after a reboot the page cache is empty, so the read is the disk's). Case: a torn payload behind an intact
     header re-derives as absent.
  3. A sync failure, then a reopen or restart without a reboot: the failed pages stay cached and clean, so no read in
     that boot proves anything, and the new incarnation's domain has no latch. Persist the failure per backend (the
     incarnation and its last good durable generation) so it survives reopen and restart; a ticket of that
     incarnation past that generation is never re-derived, it is definitive and re-put. Case: a latched domain's
     failure is not reasserted after a reopen.
Corrected: `Context` does have CONTROL work, in `Claims::retain(RetentionClass::control)` (`context.cpp:177-211`, through
`retain_control`, `distributed_store.cpp:2421-2476`: fetch to local, local put, offer, claim); missing are the bare forms
(a fetch with no claim, a local get and has, a strict local put, a claimless offer, `offer_control` at `:2400-2419`).
"Four workers in one FIFO" holds for Objects, Claims and barrier jobs, not Heads (the spaces thread), Leases, or an
ephemeral write (inline). No extra SHA-256 on the content-addressed path (both hash twice); a third comes only from the
missing known-id write. Foreground remote bytes are noted already (`distributed_store.cpp:1929`).

The readers' remaining findings, now checked (the review's table for each): `Context` has no CONTROL calls
(fetch to local, local get and has, a strict local put, an offer writing no claim); typed outcomes (a read cancelled,
timed out or unavailable; a typed per-ticket loss in barrier evidence; a refused commit typed); the hosting advert
withdrawn by Macha on a failed dial-back is put back by the storage server's refresh; `Context` work runs on four
workers in one FIFO whatever its class, barriers always at loader, `offer_control_on` holding the whole missing graph in
memory; a DATA admission for Macha's own disk I/O and the device service monitor through `Context`.

## Gaps and their fixes

The order: data safety first, then what blocks the port, then what loses performance, then the cosmetic. Each is fixed
test-first; its case and commit go here when it lands.

**Fixed on `feature/macha-store-gaps`** (gate: node-c, Dannan 0.7.0, GCC Release with warnings as errors at
`5a5834d`: 0 warnings; `dagda_tests` with Macha's fixtures 1078 ran, 1075 passed, 3 skipped, 0 failed):
- Safety 1, a missing backend directory: the node locks a backend's root only once its directory is there and the pool
  takes it as the disk comes online (`StoragePoolOptions::lock_root`), only for roots still configured
  (`2949bd6`, `e6e1d8f`, `ebf611b`). `Node.AMissingBackendPathStaysOfflineAndIsNotCreated`,
  `.ABackendThatAppearsLaterComesOnlineHeldByThisNode`, `.ABackendPathThatCannotBeCreatedLeavesTheNodeDegradedNotRefused`.
- Safety 2 and 6, garbage while metadata is dirty and the sweep's quiet window: `MaintenanceGates` in the node loop
  (`404dd9b`). `MaintenanceGates.*` (five cases).
- Safety 3, a cancelled put: `WriteEvidence::cancelled`; the put forms answer as Macha's (`f15b2e7`).
  `DistributedStore.ACancelledPutFailsEvenWhenACopyLanded`.
- Safety 4, the ticket table: a ticket still waiting, or a deferred one no caller has asked after, is never forgotten
  under the bound; a ceiling (`max_waiting_tickets`) with a warning bounds what nobody consumes (`593e129`, `c67b70b`,
  `5a5834d`). `StoreDurabilityBarrier.APublicationLargerThanTheTicketTableStillBarriersEveryExtent`,
  `.TicketsStillWaitingAreBoundedByTheirCeiling`.
- From the Macha session's review: a cache that cannot be had is turned off with Macha's warning, not a node refused
  (`57993eb`), `Node.ACacheThatCannotBeHadIsDisabledAndTheNodeRuns`; the network estimate learns from writes, fetches
  and CONTROL offers and pulls as Macha's does (`a7e2e88`), `DistributedStore.WritesAndReadsTeachTheNetworkEstimate`.

**Fixed after that gate, on the same branch.** Gated at `3ce0e10` (node-c, Dannan 0.7.0, GCC Release with
warnings as errors: 0 warnings; `dagda_tests` with Macha's fixtures 1088 ran, 1085 passed, 3 skipped, 0 failed), which
holds every fix below through `1ee2740` (compaction, healing, requirement 5, `ReadEnd`, `BarrierLoss`, activity, the
Context's object, CONTROL and filesystem forms). Everything after it gated at `caec517` (same Pi and build: 0
warnings; 1099 ran, 1096 passed, 3 skipped, 0 failed); the run at `780b8a9` before it failed only
`ContractIncludeDirection.TheLibraryNamesNoIngest`, a comment of the Devices noun naming what the library may not name,
reworded.

Also landed and in that gate, the performance and port items: DATA claims on every holder at once (`1bfbb0d`,
`DistributedStore.ClaimsGoToEveryHolderAtOnce`); repair pushes to the peers that answered (`0b8c789`, the maintainer's ruling;
`StoreRepair.APushSendsNothingToAPeerTheSurveyCouldNotSettle` amended); a barrier at its writes' frame (`6a957c7`,
`StoreDurabilityBarrier.ABarrierTravelsAtTheFrameItsWriteWasMadeAt`); a wait answering once its barriers have (`5b5a27a`,
`.ATransientBarrierFailureAnswersAtOnceNotAtTheDeadline`); a short deferred write met at what landed (`7cbbce9`,
`.AWriteThatEndedShortIsMetAtWhatLanded`); opportunistic copies in speculative memory (`b8edf6d`,
`DistributedStore.AnOpportunisticCopyIsDroppedWhenSpeculativeMemoryIsShort`); the pool recovering beside the control
chain, with Macha's stage names on `NodeOptions::startup_stage` (`39fffc4`,
`Node.TheDataPoolAndTheControlChainRecoverSideBySide`); a reload beside a maintenance pass (`427bfb9`,
`NodeReconfigure.AReloadDoesNotWaitForAMaintenancePassInProgress`); room before sealing (`e20f9ee`,
`StoreLocalStore.APutRefusedForRoomSealsNothing`); the claim barrier's figures (`780b8a9`,
`DistributedStore.ADataClaimBarrierIsMeasured`); and the Macha session's compaction case (`2f7eba6`,
`StoreLocalStore.AnObjectPutAgainAfterItsRemoveSurvivesCompactionOfTheRemove`). The Macha session reviewed the
compaction fix in code: it passes.

From Dannan (asked 2026-11-14, answered 2026-11-15): a waiting `Budget::acquire(class, owner, bytes, deadline,
cancelled)` and an owned `reserve`, in Dannan 0.8.0, for reads to hold their class's memory (cache gap 1, distributed
store gap 6): built here once 0.8.0 is merged (`feature/budget-acquire` in Dannan; gate on the 0.8.0 tag, build early
on develop once merged). `acquire` refuses a default deadline, so a read with no deadline of its own waits under the
arbiter's no-progress window; a cancel is seen only when the ledger is woken (`Budget::wake()` after the flag is set);
`Budget::stats()` gives the ledger with its owners' bytes. Built on `feature/read-memory` (reads hold their class's
memory, `3e53986` to `cc59e99`), gated early on Dannan develop `26ba1be` in a private prefix: 1106 ran, 1103 passed,
0 failed; that branch's copy of this map has the detail. It merges here once 0.8.0 is tagged. Promotion of a call in flight to loader (distributed store gap 12) is a
new wire message, ruled for Dannan 0.9.0 with the wire batch: until then a **stated difference** (the restriction is
protocol 23's, which has only promote_read_ahead and promote_foreground, not a law). Macha pins Dannan 0.8.0 for its
cutover, so Dagda is built and gated on 0.8.0 before Macha ships on both.
- Safety: pack compaction kept what replay needs (the maintainer: Dagda's to fix; a defect in Macha's store too). A compacted
  pack's live puts are rewritten with their current age; a remove is carried while a put of its object survives in
  another pack; the newest touch of an object put elsewhere is carried; such records count as live (`7f14515`,
  `d49103d`, `3ce0e10`). `StoreLocalStore.ARemovedObjectStaysRemovedAcrossCompactionAndRestart`,
  `.AReaffirmedObjectKeepsItsAgeAcrossCompactionAndRestart`, `.AReaffirmationOutlivesTheCompactionOfItsOwnPack`.
  The first gate on `d49103d` proved both halves (the removed object stayed removed; the ages agreed to the
  millisecond, not an hour apart); the age cases then compared nanoseconds a fresh conversion does not keep.
- Safety: a failed local barrier heals from disk (the maintainer, "healing is better"), with Macha's guards: a copy is read back,
  not trusted on presence, and a failed sync is kept under the state directory so a ticket issued past it is never
  re-derived after a reopen or a restart (`a445585`). `StoreStoragePool.ATornPayloadBehindAnIntactHeaderIsNotReasserted`,
  `.ALatchedFailureIsNotReassertedAfterAReopenOrARestart`; `StoreDurabilityBarrier.ALocalBarrierThatFailsIsWarnedOf`
  now pins the copy refused as absent. A blind pool's read-back checks only that the whole record is there.
- Requirement 5: a read's or write's cancellation and progress counter are co-owned by the store
  (`std::shared_ptr`), for every form (`84be44b`). `DistributedStore.ACallerMayCancelAndGoWhileItsFetchIsInFlight`.
- Requirement 2 in part: a read says how it ended, `ReadEnd` served, not_found, cancelled, timed_out or unavailable
  (`5c35435`). `DistributedStore.AReadSaysHowItEnded`.
- A barrier's definitive loss is typed, `BarrierLoss` absent_on_peer, absent_here, unknown_node (`1a96aaf`).
- Activity: `Context::activity()` notes presence on the clock the store, repair, the local stages and admission read,
  so Macha's API requests are a viewer present (`0729dfc`); `StoreDistributedStore.ActivityIsObjectWorkInMachasMeaning`
  amended to pin it.
- The route, in part (contract version 2): `Context::Objects` known-id writes, caller-thread reads and writes with no
  deadline of their own, `Tickets::wait` over many tickets (`98705a4`); `Context::Control` has, get, put, ensure_local
  and a claimless offer (`c02be9d`); the filesystem forms put_here, put_deferred_here, locally_available, cache_local,
  hydration_available, hydrate, ensure_local, reachability_epoch, and `Claims::retain_data_here` (`1ee2740`);
  `Context::Devices` DATA admission for the owner's disk I/O and the device monitor (`3b57a6b`). `Context.VersionTwo*`.

Gated at `e1ec1d1` (2026-11-15, same Pi and build: 0 warnings; 1104 ran, 1101 passed, 3 skipped, 0 failed): Macha's
names for the local stages (`9069770`, `NodeLocalMaintenance.TheStageHookHearsEveryLocalStageInMachasNames`); a
foreground copy to a peer as foreground activity (`1599b7c`, `DistributedStore.AForegroundReplicateAllIsForegroundActivity`);
a delete of a claimed object answered claimed however busy (`04ee76f`,
`DistributedStore.ADeleteOfAClaimedObjectIsClaimedEvenWhenBusy`); the disks online and the cache advertised on every
refresh (`7ae0c5c`, `e1ec1d1`; `.ARefreshAdvertisesTheBackendsOnline`, `.TheCacheIsAdvertisedWithItsActivity`). After
it, not yet gated: a reload naming a disk twice refused before any lock (`7519170`,
`NodeReconfigure.AReloadRefusedForADuplicateBackendChangesNothing`).

Reviewer findings checked against code (2026-11-15): the local CONTROL claim's failure escaping before any peer is
tried is **SAME** as Macha's (both `retain_control` loops, Macha `distributed_store.cpp:1112-1122` at f0597a4, Dagda
`:2554-2562`, put the graph and claim locally first with no catch); Dagda alone takes a control slot for the local put
and skips it when the reserve is full. `cache_local` and `hydrate` are **SAME** (Macha `:2235-2265`). Claims and
reference counts opening after the pool: resolved by the side-by-side open (`39fffc4`).

**Still Macha-side first:** prompt replication's wait with no timeout, the repair-share wake's rounding.

**Ruled (the maintainer, 2026-11-15): the side-by-side harness builds Macha's store from the extract.** Dagda builds Macha's
store code from the read-only extract of f0597a4 in its own `build/macha-f0597a4/`, copied to a Pi and built there,
and drives both stores with one workload driver of Dagda's; `../macha` is never touched, and the Macha session reviews
the driver.

**Ruled (the maintainer, 2026-11-15): Node for the owner.** The composition root that opens and owns the node keeps `Node`'s owner
surface (open, stop, reload, the maintenance steps), and the retirement codec and `lease::ClaimedCollection` stay
library helpers; the Context serves every call Macha's callers make. GC gap 8 is closed by this ruling. The question as
asked: The collector's steps, the local stages and
the retirement codecs are the owner's maintenance surface, on `Node` and in `dagda::heads`; Macha's interim loop
(`MaintenanceOptions::automatic = false` until 8c) drives them. Recommended: the composition root, which opens and
owns the node, keeps `Node`'s owner surface (open, stop, reconfigure, the maintenance steps), and the library helpers
(retirement batch codec, `lease::ClaimedCollection`) are library, not shims; `Context` serves every call Macha's
callers make. The alternative is a `Context::Maintenance` noun mirroring the steps.

Data safety and correctness:
1. A missing backend directory is created by the backend lock and can be adopted as a new, empty backend, so writes
   meant for an unmounted disk land on the root filesystem; one that cannot be created refuses the whole node rather
   than starting degraded (pool section, gap 2).
2. The node loop collects garbage while metadata is dirty: `garbage_due = !busy`, Macha `!busy && !metadata_dirty`
   (GC section, gap 4).
3. A cancelled put that already landed a copy returns success; Macha fails it (DS section, gap 5).
4. Deferred-write tickets capped at 4,096, the oldest unsettled forgotten past it: a 16 GiB publication at 4 MiB
   extents loses its barrier (DS section, gap 1).
5. The node loop builds repair's inventory only in a GC pass, so new objects go unrepaired during an ingest
   (availability section, gap 1).
6. No GC quiet window after an event, a generation move or a rebuilt inventory (GC section, gap 1).
7. Repair's drop-local checks a claim and removes with nothing held between, in Macha and Dagda both (retention
   section, gap 9).

Blocks the port: `Context` serving every form Macha's store calls need, with Macha's semantics (the route above; DS
gaps 7 to 10, retention gap 4, GC gap 8); the pool and the control chain opened side by side, and Macha's startup
stage names (pool gap 1); a write ended short of its floor met at what landed (DS gap 4, when write copies exceed one).

Loses performance: serial DATA claims (retention gap 1); the barrier's early answer and its frame (DS gaps 2, 3); read
buffers and queued copies without retained memory (DS gap 6, cache gaps 1, 2); the node loop's fixed 1 s wake, stage
order, GC schedule and survey failure (availability gaps 2, 3, 4, 10; GC gaps 2, 3); the survey's discarded trie roots
(availability gap 7); a refused put sealed first (pool gap 3); a reload waiting on maintenance (pool gap 4); extra
copies on writes and a speculative fetch not raised (DS gaps 11, 12); the defaults Macha's options must set
(cache section: background concurrency, prompt read admission, the device reserve); the arbiter's activity clock (cache
gap 4).

Cosmetic: the remaining log lines, metrics, adverts, stage-hook names and exception types, each listed in its section.

Repair (availability gap 13), **ruled (the maintainer, 2026-11-14): push to the peers that answered.** Repair stays diff-only
but repairs toward every peer whose survey settled rather than waiting for all; a peer that cannot answer is asked
nothing (`StoreRepair.APushSendsNothingToAPeerTheSurveyCouldNotSettle` stands for that peer).

Reads, for Macha's FUSE on libfuse's low-level API (the maintainer's decision of 2026-11-14, relayed by the Macha session: after
the port Macha's FUSE reads become asynchronous and abandonable; Macha's heap investigation found today's
`FuseFrontend::dispatch` returns ETIMEDOUT while a worker still writes the caller's buffer). Every read `Context` offers
Macha must state and keep, each with its case:
1. No write into caller memory after the call has returned, ever: results Dagda owns and hands over (shared extent
   bytes, as Macha's `get_shared` does), or a caller buffer whose cancellation waits for the copy to stop.
2. Prompt cancellation: a deadline and a cancel token, honoured at fetch boundaries, a remote fetch in flight
   included, with the outcome told (cancelled, timed out, unavailable).
3. An async form completing on the caller's executor, beside the blocking form.
4. The read class per call (foreground, loader, speculative), as the frame type is today.
