# Dagda - Completed Work

The record. Items move here from `ACTIVE.md` as they land, with what
shipped and the gate counts.

## 2026-11-06: heads S2, the tree over keys (`feature/heads-s2`)

`dagda/heads/`: `tree.{h,cpp}` (build, update, apply, walk, lookup, scan, prefix, paged `entries`, diff, node diff, collect, stats,
the value codec and child-tree hook; magics, domain strings and limits are parameters), `control_node_store.{h,cpp}` (read, commit,
replay, local), `reference_census.h`. Moved from Macha at `9a08846`, and it makes every node Macha made in the order Macha made it,
the unreachable first passes of a packed level included. Bytes: all 25 of `tree-vectors-0.93.4` (20 checked in, 5 too large and
read from Macha's fixtures under a named skip) and all 19 tree nodes in `state-scratch-0.93.4`'s sealed store. Macha's 21 S2 cases
are named in the plan's 5.1; C7, C34. Macha's forms live in the test, none in the library. Not run under GCC or on arm64.
Gate: `dagda_tests` 520 passing, 3 disabled, three whole runs on the Mac.

## 2026-11-06: additions inert on a Macha node, and activity in Macha's meaning

For Tom's unconditional Yes. `AdmissionOptions::device_reserve` off leaves DATA uncapped, which Macha's facade sets. Claim parts
are one journal frame (65,536 ids, `retain_ids_per_frame`), so they cost no extra sync over Macha. The node keeps its own
`ActivityClocks`: Macha's clocks count object bytes, Dannan's every inbound request, and the arbiter, repair and the local
stages read these (`StoreDistributedStore.ActivityIsObjectWorkInMachasMeaning`). The store has Macha's `foreground_activity`,
`take_*_bytes` and `idle_for`. Gate: `dagda_tests` 455 passing, 3 disabled.

## 2026-11-06: Macha's store forms and live hosting (second-cutover B3, B10)

`held_here`, `cache_local`, `hydration_available`, `hydrate`, `retain_data_here` and `reachability_epoch` on the
store, with cases. `StorageServer::set_hosts_extents`, so `auto` can follow the dial-back while running. Left of B10 for
the facade: `get_shared` is `read().object`, `DurabilityBatch` is the ticket wait, and the activity readings are
Dannan's clocks (`take_*` semantics not checked).

## 2026-11-06: resumed, Macha 0.93.4's shapes (`9a08846`)

`contract/stopping.h`; `ObjectCorrupt` thrown only for a record read in full that does not open, and `get` and
`scrub_step` remove only on that, with any other failure counted in `storage.read_failures` and the object kept
(a truncated pack record stays a counted failure and reports held, Macha's known gap); the owner's inventory walk
takes the pause; a storage event when the online disk count rises. Dagda already had the rethrow, the multiset, the
`not_held` pause, degraded, and the pull list's reset, which is the same shape as Macha's. Census and follow are heads.

## 2026-11-05: paused, and the mapping's test gaps merged

18 of the mapping's 27 gaps closed (`dagda_tests` 448 passing, 3 disabled, three whole runs on the Mac; not yet on
Linux or under GCC, and the interposed sync count's Linux half has never compiled). The other 9 are retired or stay
Macha's by decision. Code changed by them: `wait` over many tickets makes one barrier call per place; a repair list
arriving after an empty one is walked by the step that first sees it (`StoreRepair.TheStepThatFirstSeesANewListWalksIt`);
`NodeOptions::startup_progress`. The rebase left intermediate commits that may not build; the head does.

**Paused (Tom, relayed by the Macha session, 2026-11-05: "Pause until it lands"):** no port work until Macha's old-code
removal lands. The Macha session will send the commit and the forms that remain. On resuming: re-read the baseline,
conform the read-failure shape to 0.92.6, run this tree on macnessa under GCC. Left on macnessa: `/home/tom/dagda-linux`.

## 2026-11-05: the node opens in two parts (second-cutover B1)

`Node(context, options, Node::OpenLater{})` checks the contract, declares the planes and registers the message
types before the Dannan node starts, waiting on no device; `open_store()` opens the rest after it starts, on the
owner's thread, as Macha's recovery does (`service.cpp:582-586`). A node can answer peers and Status meanwhile.
The one-part constructor is unchanged.

## 2026-11-05: sanitizers and scale (M10, part)

On the Intel Mac, Dannan 0.4.1 and Dagda both instrumented. ASan and UBSan: 422 of 422, no report, with
`detect_container_overflow=0`, since the check stops the run at a `std::string` in the test logger (uninstrumented
gtest in the same process, taken to be a false alarm, not proven). TSan: 422 of 422, twice, with
`report_thread_leaks=0 exitcode=0 abort_on_error=0`, because the crash cases' forked children exit on purpose,
and TSan turns that into exit 66. One report remains, twice each run: a race in Boost's macOS reactor
(`kqueue_reactor.ipp:494`, against `allocate_descriptor_state`), no Dagda frame, code Linux does not use. Not
explained: one abort, `mutex lock failed: Invalid argument`, in the first TSan run's output only, which three
later runs did not repeat. It may have been a forked child's; left open.

Scale cases, disabled by default (`Scale.*`), Intel Mac, internal disk:
- A trie of 1,000,000 ids: built in 44.6 s, 59.9 MB of nodes, opened at once, 10,000 lookups 15 us each, walked whole in 0.87 s.
- 100,000 objects of 4 KiB across packs: written deferred in 18.2 s, reopened in 7.1 s, `has` under 1 us, `get` 114 us,
  a pack compaction slice 394 ms.
Not at Macha's scale in every way: no 1.03M loose objects, no USB disk, no cluster. `dagda_tests` 422 passing, 3 disabled.

On macnessa (Linux aarch64, NVMe, `8361c8b`, lowest priority beside the live node, load about 1): 422 of 422, 74 s.
Scale: trie built in 38.7 s, lookups 12 us, walked in 0.75 s; 100,000 objects written in 7.4 s, reopened in 0.48 s, `get`
30 us, a compaction slice 239 ms. Faster than the Mac throughout, so these are no ceiling for the USB disks, which
are not measured.

## 2026-11-05: read errors and no storage, on Tom's ruling

A read that fails on the device (`ObjectReadError`: open, stat or read failing, or a short read) keeps the
object, in `StoragePool::get` and `scrub_step`; only bytes that read and do not open are removed. A node with
every backend offline is ready but degraded, so it does not crash-loop on a startup timeout. Both ruled for
Macha too, and Dagda conforms to Macha's shape when it lands.

## 2026-11-05: the test mapping's gaps (`feature/test-gaps`)

18 of the mapping's 27 GAP cases closed, each now RETIRE against a named case; the mapping's "Closing the gaps" has the notes. Left, by decision: the placement cases, the predicates, the maintenance loop case. Gate: `dagda_tests` 444 passing, 24 of them new and none removed, so 420 before, one disabled as before; built and run on macOS with clang only, not yet under GCC on Linux.

Changed in Dagda's code:

- `DistributedStore::wait` over many tickets, both forms: one barrier call per node, epoch, domain and backend instance, at the highest generation, and one probe per dead place (Macha's `durability_barrier`). A wait on one ticket is the same path.
- Repair: a new push or pull list is not complete. Before, a list arriving after an empty one was passed over and the pass rested on it until an event (found by `StoreRepair.ACorruptRemoteCopyIsRestoredByItsScrubThenTheDiff`; Macha's push does not have this).
- `NodeOptions::startup_progress`, and `NodeOptions::trace` documented as Macha's stage hook.

New in the fixtures: `dagda/test/fixtures/sync_count.*`, the interposed count of calls for stable storage (a library of its own on macOS, the executable's own definitions on Linux, which has not been run); `ClusterNodeOptions::time`, `control` and `startup_progress`.

## 2026-11-05: the first dry run over real state

The Macha session ran `dagda-migrate --dry-run` (Dagda `db2ba92`) on macnessa over a scratch copy of the
node's state stopped at 0.92.4, on NVMe, with a throwaway custody: exit 0, no refusal, 12 s, 3.15 GB read,
nothing written, no existing file changed, three empty lock files new. Loose objects 3,809 and pack records
314,390 all open under v18, 0 unreadable, 0 damaged spans, 0 torn tails; 9 ledgers would re-seal; 3 markers.
Output at `../macha/build/macha-fixtures/migrate-dryrun/`.

What it proves: real v18 records and ledgers open under the tool's rules, and it refuses nothing. What it does
not: no real run and no there and back on this state, the ledgers re-sealed only in classification, DATA only a
sample (one pack, 48 loose objects), and the time says nothing about 4 TB on the USB disk.

The 1,883,019 unsealed records are all touches, none tombstones, all in the control store: read from the copy's
headers, 41 packs, 2.7 GB, 2026-11-01 to -05, 311,000 ids, one touched 28,104 times. A touch is 125 bytes, so
about 235 MB, 9% of those packs. The tool passes over them correctly. Whether the rate fell with 0.92.5 is
not measured.

## 2026-11-05: the first `dagda-migrate` over a state Macha wrote

A smoke test, not a gate: a private copy of `../macha/build/macha-fixtures/state-scratch/a` (two
real processes, under a minute old, no library, no backend) under the fixture's cluster key and a
custody made for it. Dry run, run and `--reverse` all exit 0; `metadata/` and everything else not
named is untouched, the case the tool used to refuse; two markers change and come back byte for
byte. It proves little else: 0 objects, 6 of 6 ledgers empty, no backend. One finding: a dry run
leaves a `.macha.lock` in `metadata-objects/`, taking the store's lock, so "writes nothing" means
no data. `docs/migration.md` should say so.

## 2026-11-05: Macha 0.92.4's shapes

A delete marks its object and unlinks holding no stripe, in the GC sweep, `erase_all`, a peer's
`delete_object` and the instant delete; a claim finding the mark treats the object as not
present. The survey's walks stop through a pause that throws `MaintenanceStopping`, and
`Repair::pass` takes the stop. Two things differ and went to Macha first: the survey's per-peer
`catch (std::exception)` would swallow the stop, so it rethrows; and marks are a multiset. Also
fixed: `StoreLayoutV19`'s corruption set a fixed byte, a 1 in 256 flake, seen once today.

## 2026-11-05: the first Linux run (M10, part)

On macnessa (Tom's choice: Debian 13, aarch64, g++ 14.2, kernel 6.18), under
`/home/tom/dagda-linux` at lowest priority with 2 compile jobs, against Dannan 0.4.1, Boost
1.87 and GoogleTest 1.17 in a private prefix: `dagda_tests` 418 passing, 1 disabled, one run,
145 s, on the NVMe root. GCC found one compile error and warnings Clang had not; one warning, in the
standard library's `vector`, remains. The Linux code ran for the first time: `DurabilityDomain`'s `syncfs`
(`durability_domain.cpp:157`), with the durability cases counting its barriers, and the
Linux-only case in `store_durability_domain_test.cpp:218`. Macha was running on the node and the node's load reached 10 by the end, its other
work included. Not sanitizers, not repeated, not on the USB DATA disks.

## 2026-11-04 night: M5, M6 and M7 (`feature/port-respec`)

M5: `Node::reconfigure` takes Macha's live reload, backends and cache, locking a
new root before anything changes and letting a removed one's lock go. Replication
takes a restart, as `service_config.cpp:23-25` refuses it live, and
`test_replication_policy_change_on_restart` is moved. The audit also listed write
copies, intervals and grace; Macha does not reload those live, so neither does this.

M6: no change. Macha's `last-survey.bin` is its path table, and its topology event
has no advert in it; Dagda's trigger already matches (evidence in `ACTIVE.md`).

M7: `LeaseClaimedCollection.TheSectionsBytesArePinned` pins the template's snapshot
and delta bytes on the neutral record. The pin comes from this code, not from
Macha; Macha's own tests pin the torrent bytes and are the gate.

Gate: `dagda_tests` 417 passing, 1 disabled, three whole runs.

## 2026-11-04 night: M8 and M9, the claims guard and placement (`feature/port-respec`)

M8: `ClaimLocks::in_batches` holds a batch's stripes 4096 ids at a time, in stripe
order, for the server's and the local claim; a later part that is not present
leaves the earlier claims standing, which is safe and retried idempotently. The
control slot is now taken before the guard, never waited for under it. Macha's
`test_retain_data_batches_a_large_publication_within_bounded_time` is moved.

M9: `StorePlacementVectors` compares `data_order`, rendezvous and logical capacity
with Macha's answers from `capacity_placement_nodes` at `5776392`: 1,352 orders,
1,352 rendezvous, 26 capacities, all equal. The reference is Linux aarch64 and this
ran on an Intel Mac; Apple Silicon's 53-bit `long double` has not been run. Gate:
`dagda_tests` 414 passing, 1 disabled.

## 2026-11-04 night: M4, Macha's write forms (`feature/port-respec`)

`put_deferred_here` added, its ticket in place of Macha's batch. The local barrier's
deadline waits on Tom's ruling in Macha. Corrected the next day: the put forms were
first made persisted by default, which matches Macha only at `write_copies` 1 and
was wrong (the test mapping found it; `distributed_store.cpp:226`). They seek
`write_copies`, and the facade passes Macha's default of 1. Case:
`StoreDistributedStore.MachasFormsSeekWriteCopies`. Gate at the time: `dagda_tests` 411.

## 2026-11-04 night: matching Macha's fixed shapes (`feature/port-respec`)

Under Tom's ruling that the libraries match Macha's fixed behaviour (Macha
0.92.3, `feature/dagda-found-defects`): the accounting wait takes the stop through
the token, with no callback notifying outside the mutex; `HeldLedger::seed`
installs what was queued, then writes the durable marker, then sets the flag, so a
crash between them cannot lose those changes. Neither has a new test: neither
window can be hit deterministically without a fault seam in the code, and the
existing stop and seed cases still pass.

## 2026-11-04 night: M3, maintenance parity (`feature/port-respec`)

`dagda::Node` runs Macha's local stages in its order after repair: rebalance,
`compact_if_needed(4096)`, DATA pack compaction, the scrub and the ledger verify.
Gates, credit and rates are Macha's (`maintenance.cpp:417-462`, `:1099-1226`): all
but the verify are skipped while foreground or loader work is present; the verify
is 4 directories a second, 0.25 busy, then a day's rest. The scrub's date is
Macha's file, `maintenance/scrub.next`, so a migrated node keeps it. Status has
`dagda.maintenance.*`. Head repair and history truncation wait for step 4.

Found on the way and matched: repair's credit had no CPU pacing, which Macha's
has (`store/cpu_pacing.h`, `background_cpu_scale`).

`StoreRepair.RepairIsPacedNotStoppedWhileHigherClassesStayBusy` began failing 2
runs in 8 with the stages on and 0 in 8 with them off, because a whole node pass
can now outlast the share's rest. The case makes a second repair pass straight
after each node pass, and its assertions are unchanged.

Gate: `dagda_tests` 410 passing, 1 disabled (was 399; 11 added), three whole runs.
Nothing is measured at Macha's scale, which is M10.

## 2026-11-04 night: M2, `dagda-migrate` for a real Macha node (`feature/port-respec`)

From the Macha session's audit (`ACTIVE.md`, "The port respec"). Gate:
`dagda_tests` 399 passing, 1 disabled (was 389; 10 added).

- Ledgers are found by name, Macha's and the node's: `metadata/` and all else
  under the state are left byte for byte. The fixture's reference counts moved
  to `ledger/referenced`, their real path.
- Every refusal comes before the first write: the node id, the key (a probe
  that stops at the first record that opens), retention the store would
  refuse, and every ledger. `MigrateState.ALedgerUnderNeitherKeyIsRefusedAndTheStateStaysV18`
  used to pin the old order, with the objects moved first, and now pins this.
- The tool reads `<node-state>/node-id`, never creates it, and refuses a backend
  naming another id. `docs/migration.md` says so.
- The v19 tag is derived from the v18 tag (`migrate/retag.h`) through only
  Dannan's AES-GCM, with a hash-key known answer from the GCM specification. A
  fresh record is opened once instead of about four times; a dry run derives no
  tag. Pack syncs are batched, and a line reports progress and rate.
- `migrate_state_back` and `--reverse`: there and back leaves every object and
  marker byte for byte, and crashes are finished from either direction.

Not proven: no run on a real Macha state, none by Macha itself over a reversed
disk, nothing measured at scale, and a dry run still reads every byte.

## 2026-11-04 night: two cleanups and the stale text (`feature/tidy`)

Nothing waits on Macha here. `MaintenanceOptions::collector` is now
`ledger::CollectorBudgets` (the retry window and the four budgets);
`ledger::CollectorOptions` derives from it and adds the grace, so the
node's dead second grace setting is gone and the wait is only
`NodeOptions::garbage_grace`, the domain's. `store::WritePrincipal`
defaults its comparison on Dannan 0.4.0's `Principal::operator==`. The
README names Dannan 0.4.1 and the current status; `ACTIVE.md`'s header
says where things stand. Not adopted: `Nodes::certificate` and
`Identity::ca_certificate` for the wrap advert, which is one gossiped
value and cannot drop its certificate per peer; decided with the domain
catalogue (`ACTIVE.md`). Gate: `dagda_tests` 389 passing, 1 disabled, unchanged.

## 2026-11-04 night: Dannan 0.4.1, the node declares its planes (`feature/declare-planes`)

Dannan 0.4.1 (`9174cca`, pushed) adds `status::Readiness::declare`; the
pin (`find_package(dannan 0.4 CONFIG)`) takes it unchanged. `dagda::Node`
declares its readiness planes (`data_storage`, `control_storage`,
`cache`, `retention`) at open, before the Dannan node starts, and refuses
with a reason if it has started; the `readiness_planes()` export is gone,
and an owner names none of Dagda's planes in its `NodeConfig`.
`Node.UndeclaredReadinessPlanesAreRefusedWithTheRemedy` became
`Node.DeclaresItsOwnReadinessPlanes` (no planes in the config, the node
still reports each and goes ready); the cluster fixture's `planes` option
is gone. Gate: `dagda_tests` 389 passing, 1 disabled, unchanged.

## 2026-11-04 night: the claim guard and sealed sightings (`feature/guard-and-seal`)

Step 7b's two findings, fixed on Tom's word, one commit each. Gate:
`dagda_tests` 389 passing, 1 disabled (was 388; 1 added); the touched
suites rerun 3 times.

- The claim guard (`ClaimLocks`) is now held across the claim check and
  the delete in the GC sweep (`StoragePool::gc_step`), a node's own erase
  (`DistributedStore::erase_all`) and a peer's `delete_object` request
  (`StorageServer`), as the instant delete and the claim path already
  were. No claim can land between a check and its delete. No new test:
  the window is not reproducible deterministically; the existing
  late-claim sweep case still passes.
- `retention/unreferenced-{data,control}.bin` are one AES-256-GCM frame
  under the node-local key (AAD "dagda/sightings/1"), the table's bytes
  unchanged inside; `UnreferencedSince` requires the key; a file that does
  not open is ignored as before (it only delays deletion). The migration
  seals a v18 node's plain files (`MigrateState.ThePlainSightingsFiles
  AreSealed`), so a migrated node keeps its deletion waits.
  `LedgerUnreferencedSince.TheFileIsLayoutV18s` became
  `TheFileIsTheTableSealedUnderTheNodeLocalKey` (the table's bytes are
  still pinned; the file is checked to be sealed and to refuse another key).

## 2026-11-04 night: step 7b, maintenance in the node, the per-domain wait, the instant delete (`feature/maintenance`)

Gate: `dagda_tests` 388 passing, 1 disabled (was 376 and 1; 12 added,
none dropped); the new suites rerun 5 times.

- Maintenance in `dagda::Node`: one supervised thread
  (`dagda.maintenance`) on the node's clock; repair every `interval` (1 s,
  Macha's), GC every interval until a pass settles, then every
  `gc_interval` (300 s, Macha's `no_progress_backoff`); GC skipped while
  foreground or loader work is present. A collector per registered
  `ReachabilityProvider`; with none, nothing is removed and
  `dagda.gc.state` says so. Unregistering is final: the provider is never
  read again. `dagda.gc.*` figures. Stop stops the loop first. Tests turn
  the thread off and call `run_maintenance_once()`; `store_repair_test`'s
  hand loops converted to it.
- The per-domain wait: `NodeOptions::garbage_grace` (30 days) for the
  node's domain; the catalogue's descriptor carries it at step 4.
  (`MaintenanceOptions::collector.garbage_grace` is now ignored: a second
  setting to remove.)
- The instant delete: `context.objects().remove(ids, observed clock,
  scope, ...)`, async and blocking; messages `remove_objects` (769) and
  its reply (770). Every present node is asked (a node can hold a claim
  without a copy); each verifies scope, then under a new per-id claim
  guard (`ClaimLocks`, 64 stripes) releases only the claims the observed
  clock dominates (`RetentionStore::release_observed`, the same rule and
  journal frame as `release_unreferenced`) and removes its copy only if no
  claim is left and the latest inventory does not refer to it (an
  incomplete inventory keeps it). `RemoveEvidence` per node and id:
  `removed`, `absent`, `kept_by_claim` (naming every remaining claim),
  `kept_referenced`, `refused` (why and scope generation), `unreachable`.
- The partition case (`StoreRemove.AClaimTheCallersClockHasNotSeenKeeps
  ThatNodesCopyAndIsNamed`): a delete under {a:1} removes a's copy and
  keeps b's, naming (b,7); b's claims are exactly {b:7} after.
- Tests: `store_remove_test` (7), `node_maintenance_test` (3), one each in
  `LedgerRetention` and `StoreMessages`; `Context` and `node_test`
  extended for the delete and the maintenance stage.
- Findings, not fixed: the GC sweep's final check-and-delete
  (`storage_pool.cpp:1134`), `erase_all` (`distributed_store.cpp:2294`)
  and the `delete_object` handler (`storage_server.cpp:264`) check claims
  without the new claim guard, so a claim can land between check and
  delete (Macha has the same window); the sightings files
  `retention/unreferenced-{data,control}.bin` are written unsealed
  (`unreferenced_since.h:44`), unlike every other ledger since 6b.

## 2026-11-04 night: the repin to Dannan 0.4.0 and a race (`feature/dannan-0.4`)

Dannan 0.4.0 (`d657a9b`; needs yaml-cpp to build) installed into
`build/deps/prefix`; the pin, the bootstrap and the README moved to 0.4.
Three commits:

- A race, found by step 7a: `LocalStore::put_sealed` returned before its
  record reached the held ledger, so once the ledger was seeded `has()`
  could say no to an object just stored (`put` already flushed). It now
  flushes. `StoreLayoutV19.ACopyThatDoesNotOpenIsKeptUntilTheKeyIsChecked`
  failed 33 assertions over 30 runs before, none over 30 after.
- `StorageLock` is Dannan's `identity::StateLock` under the store's name
  (`.macha.lock`), its own implementation gone (rule 3).
- `domain/wrap`: `identity::x25519_public` replaces the base-point call;
  the membership test's fake `Nodes` gains `certificate()`.
- Not yet taken: `Nodes::certificate(node)` and
  `Identity::ca_certificate()` to shrink the wrap advert for peers met by
  certificate (the DER stays as the fallback for a peer never talked to);
  `Principal::operator==` where Dagda compares by hand. Small; with the
  next domain work.
- Gate: `dagda_tests` 376 passing, 1 disabled, unchanged.

## 2026-11-04 night: step 7a, the node, everything but heads (`feature/node`)

Gate: `dagda_tests` 376 passing, 1 disabled (was 367 and 1; 9 added, none
dropped); the node, context and cluster cases rerun 3 times.

- `dagda/node.{h,cpp}`: `dagda::Node(dannan::Context&, NodeOptions)`, the
  assembly. Opens in order: message types; the state lock, the v18 backend
  check and the state layout; locks on the backends, control and cache
  roots; the node-local and wrap keys; the pool, admission, the control
  store, the cache, the claims and ledger, the reference counts, the
  distributed store, availability, repair, the storage server. Stops
  admission first, then the reverse, locks last. Refuses with a reason (and
  marks readiness failed): another Dannan Context version, types already
  registered or the Dannan node started, a held lock, a v18 state or
  backend (naming `dagda-migrate --state <path>`, nothing changed), no
  custody, an undeclared readiness plane, a blind domain or a missing key.
  `register_reachability` and `publish_inventory` (C3), `publish_status`
  (C4: `dagda.storage.*`, `control.*`, `cache.*`, `references.*`,
  `repair.*`, `repair.gate.*`, `prompt.*`, `admission.*`, the context
  version and domain), readiness planes `data_storage`, `control_storage`,
  `cache`, `retention`; the wrap key advertised at open; the lease keeper
  over `RosterMembership`.
- `dagda/context.{h,cpp}`: `dagda::Context`, `context_version = 1`,
  `check_version`/`require_version`. Domains (`list`, `describe`: the
  node's one domain), Objects (`write`, `read`, `has`), Claims (`retain`
  with a `WriteScope`, `retained`; no release call: claims go only by the
  collector against the owner's horizon, as in Macha), Leases (`acquire`,
  `read`, `standing`), Tickets (`status`, `wait`, `wait_complete`); every
  completion on the caller's executor; the blocking forms wait on them.
  Heads, Retained and node-local spaces join at step 4; v1 is frozen then.
- `dagda/lease/local_records`: `LocalLeaseRecords`, in memory per node
  until heads exist, lost on restart (the header says so).
- `store/distributed_store`: an async `retain` returning `RetainOutcome`.
  `store/storage_pool`: `backend_refusal_v18`; an empty backend is adopted
  with the node's lock file in it. `store/state_layout`: Macha's "0.18
  requires a fresh namespace" now says what Dagda means.
- `test/fixtures/cluster.h`: every cluster node is a `dannan::Node` with a
  `dagda::Node` on it, so every cluster test runs through the node.
  README's "Link it" shows `Node` and `Context`.
- Tests: `node_test` (7: order on fresh and reopened state, v18 refused
  with digests unchanged, no custody, the lock against a second node and
  against the migration, types registered before start, Status and
  readiness, undeclared planes), `context_test` (2: the version check;
  every noun completes on the caller's strand on two nodes).
- Findings: `StorageLock` (`store/local_store.cpp:87`) duplicates
  Dannan's `identity::StateLock` (rule 3); `migrate_state` answers "already
  v19" before taking a lock (harmless: it writes nothing);
  `StoreLayoutV19.ACopyThatDoesNotOpenIsKeptUntilTheKeyIsChecked` failed 5
  of 10 runs on the agent's machine before 7a, passed 20 of 20 on review:
  a race to pin.
- For Tom: readiness planes are fixed at Dannan's node construction, so
  the owner lists `dagda::readiness_planes()` in `NodeConfig::planes`
  (recommended: ask Dannan for `Readiness::declare` before start). The
  collector and the maintenance loop stay the owner's until a second owner
  needs them.

## 2026-11-04 night: step 6b, one layout and the migration (`feature/one-layout`)

Tom: "We don't need backward compatibility": one layout, v19, for every
domain; Macha's v18 disks migrated in place by a tool that works and needs
no extra storage; manual only. Gate: `dagda_tests` 367 passing, 1
disabled (was 347 and 1; 22 added, 2 moved, none dropped); the migration,
layout, pool, sealing and domain cases rerun 5 times, 79 of 79.

- The store is v19 only: `SealingScheme{domain}`, AAD "dagda/seal/19" ||
  domain || id; v19 markers and state layout only. A v18 marker or state
  layout is refused with a reason naming `dagda-migrate --state <path>`,
  nothing on the disk changed; nothing in `store/` migrates.
- Every node's ledgers (held tries and journals, retention claims,
  reference counts, the control store's) are sealed under the node-local
  key (`require_node_local`), mandatory; no fallback to the domain key.
- `dagda/migrate/` (`legacy_v18`: the v18 readers, read only; `migrate`:
  `migrate_state`, `MigrationReport`, `MigrationRefused`) and
  `tools/dagda_migrate.cpp` (`dagda-migrate --state DIR --node-state DIR
  --cluster-key FILE (--passphrase-env VAR | --credential NAME) [--backend
  DIR]... [--control DIR] [--cache DIR] [--dry-run]`; exit 0 v19, 1
  refused or failed, 2 usage). `docs/migration.md`.
- Proven: under the same key and nonce v18 and v19 give the same
  ciphertext and different tags (known answers computed outside Dagda), so
  a loose object changes 16 bytes (its tag) and a pack record 48 (tag and
  header checksum), in place. Ledgers are re-sealed to a sibling and
  renamed over. Peak extra space: one ledger file.
- Order: objects, then ledgers, then markers, `storage-layout` last. A
  crash leaves each record v18 or v19 (told by its AAD), each ledger under
  one key (told by probe) and the markers v18, so the store still refuses
  and a rerun finishes (tested by `_exit` mid-run). A v19 state is left
  alone; `--dry-run` writes nothing. Refused before any write: missing or
  foreign markers, a held lock, a key under which no probe record opens.
- The key probe at activate: a member on a "blind"-checked backend opens up
  to 8 records before trusting its key, and upgrades the marker's check
  when one opens; `get` and scrub discard a copy that does not open only
  where the key is checked.
- Moved, assertions kept: `StoreLocalFormat.OpensALooseObjectMachaWrote`
  and `OpensAPackMachaWrote` to `MigrateLoose`/`MigratePack`;
  `StoreSealing.OpensWhatMachaSealed`, `SealsMachasBytesUnderTheSameNonce`
  and the v18 tag of `TheEmptyObjectSealsToATagAlone` to
  `MigrateLegacyV18` (the store keeps v19 versions). Converted to v19:
  the sealing, state layout, pool marker and layout cases (renamed where
  they named v18). Added: the refusal test, 3 layout cases, 6
  `MigrateLegacyV18`, 12 `migrate_migrate_test` (a whole v18 state from
  the known answers, migrated, read back object by object and claim by
  claim).
- Findings, not fixed: nothing in the store takes `StorageLock` at open
  (`store/local_store.h:47`), so the tool's "node is running" refusal holds
  only if the node holds the locks; recommended at step 7's node.
  `store/state_layout.cpp`'s "0.18 requires a fresh namespace" wording is
  Macha's; reword at step 7.
- Still the gate: the Macha session's real v18 state directory, dry run
  first. An empty blind-adopted disk stays online with its key unchecked.

## 2026-11-04: step 6, domains, the parts that need no head (`feature/domains`)

Branched from `develop` at "Record Tom's step 6 decisions". One commit.
Gate: `dagda_tests` 347 passing, 1 disabled (was 320 and 1; 27 added,
none lost); the cluster and crypto cases rerun 8 times, 152 of 152.

- `domain/domain_id.{h,cpp}`: `DomainId` (16 bytes, `parse`,
  `to_string`); `domain_v1`, fixed, the first 16 bytes of
  SHA-256("dagda/domain/v1") (named in memory and on the wire only, no
  v18 byte carries it); `node_local_domain(node)`.
- `domain/domain.{h,cpp}`: `Role {member, blind}`, `Domain {id, name,
  role}`; v1 is named "macha", a node-local domain "node-local".
- `domain/storage_key`: `StorageKey::derive_node_local(Identity&)` from
  `derive_local("dagda/node-local/1")`; absent without custody.
- `domain/wrap.{h,cpp}`: `WrapKey` (X25519, derived under "dagda/wrap/1",
  private half clamped in secure memory; the public half is
  `x25519_shared(private, base point)`); `WrappedKey` and `wrap`/`unwrap`
  as decided (HKDF of the shared secret under "dagda/domain-wrap/1",
  AES-256-GCM, AAD "dagda/wrap/1" || domain || member || wrapper), the
  unwrapped key landing in secure memory; the codec; `WrapAdvert` under
  advert key `dagda.wrap`: public half, signature over "dagda/wrap/1" ||
  node id || public half, and the node's certificate DER (about 600 of the
  advert's 1 KiB, until Dannan exposes peer certificates);
  `advertise_wrap_key`, `verify_wrap_advert(peer, CA, now, revocations)`.
- `store/sealing`: `SealingScheme {layout 18|19, domain}`; v18's AAD is
  the id alone (Macha's), v19's "dagda/seal/19" || domain || id;
  `blind_digest` = SHA-256 over nonce, tag and ciphertext.
- `store/local_store`, `store/storage_pool`: `sealing`, `role` and
  `ledger_key` options; `put_sealed`/`get_sealed`/`held_digest`; a blind
  store or pool takes no key and refuses `put`/`get`; sealing moved out of
  the loose and pack writers so a record sealed elsewhere is stored as it
  is. The v19 marker "dagda-backend-v19 <node> <token> <domain> <check>",
  check = HMAC-SHA256(domain key, ...)[:16], so a member without the key is
  refused at open; a blind pool adopts with check "blind". v18 markers and
  bytes as before; a disk of the other layout or domain is refused with a
  reason. `store/state_layout`: "dagda-state-layout-v19 <domain>".
- `store/messages`: `WritePrincipal {principal, prefix}` appended to
  `put_object`, `put_object_deferred` and `retain_objects` after protocol
  23's fields; absent, it is the sending node's own write under the
  domain's name.
- `store/distributed_store`, `store/storage_server`: `WriteOptions::
  principal` and `prefix`; `WriteScope` on `retain_data`/`retain_control`;
  every storing replica, the local one included, verifies before storing
  and a refusal is `ReplicaState::refused` with "403 scope_refused: <why>
  (scope generation N)" in the evidence; the server answers 403 before
  admission; `put_control_object` is checked under the sender's node
  principal.
- `test/fixtures/cluster.h`: every node has custody (a light Argon2id
  passphrase), a certificate from the cluster's CA over its custody key,
  `use_cluster_key` kept, and advertises its wrap key at start.
- New tests: `domain_domain_id_test`, `domain_wrap_test` (known answers:
  pure-Python X25519 checked against RFC 7748, GCM against the spec's test
  case 16, HKDF against RFC 5869, computed under `build/kat/`, none in the
  suite), `domain_cluster_keys_test`, `store_layout_v19_test` (the AAD,
  the marker's check, the binding: a v19 object moved to another domain or
  id does not open), `store_scope_test` (no source: node principals write,
  a directory principal is refused; a source refusing one prefix: refused
  on every replica with evidence, another prefix lands); cases added to
  `store_sealing_test` and `store_messages_test`.
- Blind nodes verify the record is whole and that its ciphertext digest is
  the one a member bound to the id; they cannot verify the tag, the domain
  binding or that the bytes open to the id's plaintext. The distributed
  blind path (sealed bytes on the wire, blind placement) waits on the
  catalogue (step 4). A blind pool has no maintenance yet: rebalance and
  scrub use `get` and would throw.
- Dannan: everything needed existed but three, asked of the Dannan
  session: an X25519 public-from-private helper; a peer's certificate (or
  public key) and the CA through the Context; `operator==` on
  `contract::Principal`.
- Finding, not fixed: `StoragePool::get` removes a copy that does not
  open, so a member opening a blind-adopted v19 backend under a wrong key
  would discard every object it reads (the marker's check protects
  member-made backends only; the same as a wrong cluster key on v18).
  Recommended to Tom: a key probe at activate.

## 2026-11-04: the decided fixes (`feature/decided-fixes`)

Tom's answers to the seven open items (Decisions, "2026-11-04 evening"),
the five that needed code, one commit each:

- `store/control_store.h`: `ControlStoreOptions::reserve_free` defaults to
  256 MiB (was Macha's 0); the defaults test says so.
- `ledger/retention.cpp` `load()`: tries that hold claims under no
  `retention/ledger` marker are refused with a reason, nothing on disk
  changed; `LedgerRetention.ClaimsWithNoMarkerAreRefusedNotRebuiltEmpty`
  (restore the marker and the claims are there).
- `lease/claimed_collection.h`: `expired` treats a stamp from a clock ahead
  as fresh (`now > removed` before the unsigned difference);
  `decode_delta` refuses a record with no key, as the snapshot decoder
  does. The two disabled cases are enabled and the "poisons the snapshot"
  case now shows the record never reaches the head.
- The device reserve needed no code: `IoReserveOptions{slots 8,
  control_reserve 2}`, one per node, is the configurable form; the
  operator's path to it is step 7's node options.
- Gate: `dagda_tests` 320 passing, 1 disabled (was 317 and 3: two enabled,
  one added).

## 2026-11-04: the repin to Dannan 0.3.0 (`feature/dannan-0.3`)

Branched from `develop` after steps 1 to 3 and 5 merged there (`5aa4d23`).
Dannan 0.3.0 (`277adab`) installed into `build/deps/prefix`; the pin and the
bootstrap in `CONTRIBUTING.md` moved to 0.3. The library compiled against
it unchanged; only the membership test's fake `Nodes` needed the new pure
virtual. Then each stand-in went, one commit each:

- `lease/membership`: `RosterMembership` no longer watches the roster to
  time a node's absence; it asks `Nodes::absent_since`, which is from when
  the roster last heard of the node (about `dead_after` earlier than the
  watch's "left") and survives a restart. A node the roster does not know
  is still absent from the first look. Its test is rewritten for it.
- `store/local_store.cpp`: Dannan's `contract::Unlocked`.
- `store/repair`: Dannan's `sched::DutyShare` (`may_start`, `started`,
  `should_yield`, `finished`); `RepairOptions` weights are `uint32_t` as
  Dannan's are, and its `time` option is gone.
- `store/distributed_store`, `store/availability`: the executor and the
  clock come from `context.time()`; the `executor` constructor argument and
  the `time` options are gone. The survey re-asks peers on
  `NodeEvents::membership` instead of `topology`, so an advert no longer
  triggers one. `call_horizon` stays as the bound on a copy in flight past
  the write; Dannan now cancels the call at it, which is what the option
  always meant.
- Not replaced: `TimedLock` (`store/storage_pool.h`) is not in 0.3.0.
- Gate: `dagda_tests` 317 passing, 3 disabled, unchanged.

## 2026-11-04: ingest removed (`feature/leases`)

Tom: "Dagda doesn't need ingest, that will be handled by macha. Remove
all code from dagda that handles ingest."

- The library had no ingest engine. What handled ingest was in the
  tests: Macha's `TorrentRequest` and `TorrentOwner` (its torrent
  coordinator through the lease template), `test/fixtures/torrent_request.{h,cpp}`,
  and their 16 cases, `test/lease_torrent_request_test.cpp`. All three
  files are deleted.
- Those cases were also the only tests of `ClaimedCollection<Traits>`,
  which stays (leases are not ingest). New:
  `test/lease_claimed_collection_test.cpp`, 11 cases over a neutral task
  record: the group-by-group join, the record and collection merge laws
  with and without a base (concurrent duplicates included, since this
  rule's loser stamp does not depend on the keeper), erasure against a
  base, the duplicate rule, snapshot and delta round trips, the decode
  bounds, the tombstone window, and the two findings (Waiting on Tom 5
  and 6) carried over unchanged, disabled as before.
- Not carried over, being Macha's and its gate at the port: Macha's
  record bytes against known answers, its merged-set and DLT9 bytes, its
  bounds by offset, the three-concurrent-adds finding (its keeper-
  dependent loser stamp), the owner-driven cases (requests driven by
  their owner, ranked takers, the lowest-member sweep).
- `ContractIncludeDirection.TheLeaseLibraryNamesNoTorrent` is now
  `TheLibraryNamesNoIngest`: no file under `dagda/` outside `test/`
  names a torrent or ingest.
- Kept, as general machinery Macha's ingest drives but does not own: the
  `loader` work class (Dannan's `WorkClass`/`FrameType`), deferred writes
  and their tickets, the DATA credit arbiter, the device reserve,
  repair's pacing behind loader work. Three comments that used ingest or
  import as the example now say bulk or deferred writes
  (`store/storage_pool.cpp`, `store/data_work.cpp`,
  `store/availability.cpp`).
- Gate: `dagda_tests` 317 passing, 3 disabled (was 322 and 3: 14 enabled
  and 2 disabled torrent cases out, 9 enabled and 2 disabled template
  cases in). Each removed test is named in the list above; the drop is
  Tom's instruction, not a regression.

## 2026-11-04: step 5, leases and the claimed collection (`feature/leases`)

Branched from `feature/distributed-store` at `ce7617c`; step 4 (heads) is
not done and waits on Macha's tree-native merge, so this step builds the
data types and the lease logic a head space will carry, and no head.

- New in `dagda/lease/` (namespace `dagda::lease`), from Macha's
  `torrent/torrent_request.cpp:36-52`, `:163-244` and
  `torrent/torrent_coordinator.cpp:837-1100` (develop `5a06d3c`),
  generalised; nothing there names a torrent (a test enforces it):
  - `lease.h`: `Lease {holder, epoch, since_unix_ms}`, `beats` (higher
    epoch, then earlier claim, then lower node id), `merge` (also over
    `optional`), its codec (Macha's claim bytes: holder, epoch, since; an
    optional one behind a 0/1 flag). Requirement E2.
  - `claimed_collection.h`: `ClaimedCollection<Traits>`, header-only. The
    owner's traits give the record (totally ordered), its key, its field
    groups (`field_group(clock, take)`, each with its own clock: Tom,
    2026-11-04; helpers `written(ms, node)` and `dominating(flag, ms,
    node)` for a dominating cancel), the record's lease and tombstone, the
    duplicate rule (`duplicate_key`, `keeps`, `lose`), the record and key
    codec, `max_records` and `retention`. A record merges from the greater
    record, each group taken whole from the greater clock (equal clocks:
    the greater record), the lease by the lease order, the tombstone max.
    `merge(base?, left, right)` is the three-way merge over the key set
    (a null base is an empty one: Tom, 2026-11-04), then the duplicate
    rule. `encode`/`decode` are the snapshot section (u32 count, records),
    `diff`/`encode(Delta)`/`decode_delta`/`apply` the delta section (u32
    and records written, u32 and keys erased), with Macha's decode bounds;
    `expired(now)` the tombstones past the window; `join(record)` an
    owner's write. Requirements E1, E3 (the window), E4, E5 (the hook), E6.
  - `lease_table.h`: `LeaseEntry {name, lease, removed}` and `LeaseTable`,
    the template's second instance: named leases as a lease space will
    carry them. Dagda's own format (format byte 1, name, optional lease,
    tombstone), pinned.
  - `membership.h`: the injected `Membership` (`self`, `absent_since`,
    `active`) and `RosterMembership` over Dannan 0.2.0's `Nodes::active()`
    and `Nodes::watch()` on an injected `TimeSource`: absent from the
    watch's `left`, or for a node never seen from the first look; seen
    again, a member at once.
  - `keeper.h`: `LeaseKeeper` (`decide`, `vacant`, `standing`, `next`,
    `acquire`) with `LeaseOptions` (absence 10 min, rank step 30 s:
    Macha's). A lease holds while its holder is a member; once absent for
    the window it is taken at epoch + 1 ({self, epoch + 1, wall now});
    a taker the owner ranks r waits r steps; a holder whose record shows
    another holder, or a higher epoch not its own, stands down.
    `acquire(records, scope, name, deadline, executor, done)` decides on
    the record held, joins its claim when it may take, and reports the
    record as it then stands (`held`, `taken`, `held_elsewhere`,
    `waiting`, `lost`, `failed`, with why), posted to the caller's
    executor. Requirements E7, E8 (`next`).
  - `fence.h`: the epoch fence (new; requirement E9): `fence(record,
    epoch)` and `EpochFence` (`admit`, `observe`, `highest`) per lease
    name, refusing an action under a lower epoch, with evidence (admitted,
    the highest known, why).
- **The step-4 seam.** (1) `LeaseRecords` (`keeper.h`): `read(scope,
  name)` and `join(scope, name, lease, deadline, executor, done)`; step 4
  implements it over a head space holding a `LeaseTable` (`scope` names
  the space), and until then the tests' `MemoryLeaseRecords` does, with
  `reconcile` standing in for the heads' merge. (2) A head's codec and
  merge call the collection's: snapshot section, delta section, and
  `merge(base?, a, b)` with the common ancestor when the reconciler holds
  it. For Macha the collection stays inside its head blob at the SM16,
  SM17 and SM19 position and in DLT9 (requirements 5.4). (3) A receiver
  raises its fence from merged records with `EpochFence::observe`.
- `TorrentRequest` is the first instantiation, in the tests
  (`dagda/test/fixtures/torrent_request.{h,cpp}`): Macha's record with
  `TorrentClaim` as `Lease` (same members in the same order, so the
  record's order and bytes are Macha's), its three groups, its codec (the
  claim written by the lease's codec), its duplicate rule, 65,536 records,
  8,192-byte text, 7 days. `TorrentOwner` is the records-and-leases part of
  Macha's coordinator over the lease keeper: adds, intent, claims,
  takeover after absence, standing down, pins, completion removal, and the
  lowest member's sweep.
- Bytes, pinned by known answers computed outside both codebases (Python
  `struct` and `hashlib`, from the layout of `torrent_request.cpp:89-120`,
  `metadata.cpp:856-876` and `:1499-1507`; the merged records written out
  by hand from the rules): Macha's own round-trip record in full; the
  lease alone; a merged set of five (a cancel against a later pause, a
  takeover, a tombstone against a newer pause, two adds of one torrent) as
  SM16/17/19 carry it (1,536 bytes, SHA-256 pinned); the DLT9 section from
  one side to that set (1,256 bytes) and a sweep's; a lease entry. No
  difference from Macha found. The fixture comparison (the Macha session's
  SM16/17/19 and DLT9 bytes) waits for that fixture, cut after Macha's
  tree-native merge.
- Tests moved from Macha (develop `5a06d3c`), converted to GoogleTest:
  `test_storage_metadata.cpp` `test_torrent_requests_join_without_conflicts`
  (every assertion), `test_torrent_requests_ride_the_snapshot_and_its_deltas`
  (the section's round trip, the delta's upsert, update and erasure, a
  quiet change, the reconciliation join); `test_hydration_catalogue.cpp`
  `test_torrent_requests_belong_to_the_cluster_and_are_driven_by_their_owner`
  (through `TorrentOwner` on stepped time: adds waiting for a capable node,
  the duplicate add refused naming the holder, pause, remove-after,
  clear, add paused, resume, unknown ids; claim at epoch 1 and the job
  under the request's id, pause and resume applied, clear refused while
  running, cancel recorded, clear; removal after completion at a delay of
  0; the absent owner's claim kept for ten minutes and taken at epoch 2
  with `phase_epoch` 2; pins; the newer claim let go, from a member here,
  as on stepped time Macha's `gone` is past the window). No other Macha
  case touches torrent requests' records (`test_rpc_cluster.cpp`
  `test_torrent_listing_is_served_from_memory_never_a_survey` and
  `test_hydration_catalogue.cpp` `test_the_torrent_plugin_restores_its_jobs_and_runs_them_through_the_engine`
  are Macha's API and engine).
- Waiting for step 4: of the snapshot case, the magics (`DHTMETB5`,
  `DHTMETB6`, `DHTMETB4`, `DHTMDLT9`, and not DLT9 for a quiet delta), the
  whole snapshot's round trip and its return to the pre-request encoding,
  and `merge_metadata_heads`' `conflicts_created == 0`; of the coordinator
  case, the records living in a head (`mutate_delta` is a join into this
  node's copy). Not moved, Macha's own and its gate: the HTTP bodies and
  status fields, the engine's job states and the migration of a local job
  into a request, ingest sources, search.
- New: the lease order and its join; the lease and entry bytes; the table
  as a claimed collection; the record merge a join over 40 versions (all
  triples); the collection merge commutative, idempotent and associative
  over 200 rounds of three branches, with a base and without; takeover
  not a millisecond before the window, a holder never seen, rank waits;
  a holder that returns standing down and fenced; `acquire`'s outcomes and
  the executor rule; eight concurrent takers leaving one standing; one
  keeper from eight threads; the fence (lower refused, equal admitted,
  observe, concurrent senders); membership from the roster; the 7-day
  sweep by the lowest member.
- Findings, none fixed (none blocks this step; answers kept identical to
  Macha's):
  - In Macha too: the sweep computes `now - removed > grace` on unsigned
    milliseconds (`torrent_coordinator.cpp:1066-1068`), so a tombstone
    stamped by a node whose clock is ahead wraps and is erased at once,
    and a branch that missed the removal can bring the request back.
    Proven by `DISABLED_ATombstoneFromAClockAheadIsKeptItsWindow`.
  - In Macha too: DLT9's decoder accepts a request with no id
    (`metadata.cpp:1735-1740`) that the snapshot decoder refuses (`:873`),
    so a head that applies such a delta encodes a snapshot it cannot read
    back. Proven by `AnUnkeyedRecordFromADeltaPoisonsTheSnapshot`; the
    refusal it should make is `DISABLED_ADeltaCannotInstallARecordTheSnapshotRefuses`.
  - In Macha too: with three concurrent adds of one torrent the collection
    merge is not associative: the third is cancelled by whichever beat it
    first, so its cancel stamp and error text depend on merge order, and
    replicas differ until they merge each other's result
    (`ThreeConcurrentAddsSettleOnlyAtTheNextMerge`). The law holds for two
    and over heads closed under the rule; the template's header says a
    new owner's `lose` should not depend on the keeper.
- Gate: warnings as errors; `dagda_tests` 322 and 3 disabled (284 and 1
  before; the two new disabled cases are the findings above). The 38 new
  cases ran 200 times each (four processes of 50): 7,600 of 7,600 passing.
- Dannan: nothing after 0.2.0 is used. Request: a node's absence on the
  steady clock from `Nodes` (a `RosterChange`'s time or
  `Nodes::absent_since`), marked `TODO(Dannan: absence on the roster)` in
  `lease/membership.h`; `RosterMembership` rebuilds it from `watch()`.

## 2026-11-04: step 3b, repair by diff and admission (`feature/distributed-store`)

- Moved and adapted (namespace `dagda::store` unless said): `data_work`
  (`DataWorkContext`, `DataResourceArbiter`, `DataResourceStats`: Macha
  `cluster/data_work.hpp`), `ledger/availability` (`dagda::ledger`:
  `AvailabilitySurvey`, `PeerDiff`, `PeerLacks`, `survey_by_diff`,
  `keep_unavailable`: Macha `ledger/availability.*`), `availability`
  (`AvailabilityService`: Macha `service/availability_service.cpp`
  without its path table), `repair` (`Repair`: the repair stage of
  Macha's pass, `service/maintenance.cpp:309-313`, `:674-852`), and in
  `DistributedStore` repair by diff (`repair_step`, `RepairLists`,
  `RepairResult`, `RepairDiagnostics`: `distributed_store.cpp:2324-3087`),
  `ensure_local`, `replicate_all`, `erase_all`, `valid_on`; in
  `StorageServer`, `have_valid_objects`, `delete_object`, the trie
  questions and each route's admission. Requirements L7 to L11, L23's
  restorer, S20, S30, section 7 item 4.
- Repair by diff, as built: the survey publishes, from the inventory the
  collector built from the owner's `ReachabilityProvider`, what this node
  lacks (the inventory joined against its held union), each extent-hosting
  peer's lacks and what no reachable node holds, by `trie_diff` rounds
  against the one snapshot the peer's root question pinned (four pins,
  two minutes; 503 `holdings_seeding`, 410 `snapshot_gone`, 413
  `prefix_too_large`). A step pushes only from lacks every hosting peer
  answered for this inventory: an object every owner holds settles with no
  call, a peer the diff did not ask is neither keeper nor destination,
  sends go out together (8 objects, two extents) at speculative priority
  and are paid for by credit, and the local copy of an object this node
  does not own goes once every owner holds it and nothing here claims it.
  It pulls from what this node lacks, cache first, passing what nobody
  holds, retrying what nobody could settle (1 s doubling to 10 min, all due
  when the reachable set changes). No cursor walk, `repair_position`
  file or `have_*` probe (Tom, 2026-11-04). `Repair::pass` runs the
  survey, the claim walks with a restorer that spends network credit, and
  one step, paced by a duty share (95:5, 25 ms turns) against this node's
  foreground and loader work and a peer's foreground traffic in its
  telemetry, never stopped; quiet after a pass with nothing to do until an
  event or new lists.
- Rebalancing across nodes: Macha has none beyond repair; its push and
  local drop are it (`ANodeThatStopsHostingDrainsThroughRepair`). The
  pool's local rebalance moved in 1d.
- Repair reports into tickets: `WriteEvidence::complete` (no member
  pending, those owed to repair included) and `ReplicaEvidence::repaired`
  (acknowledged after the write returned); `wait_complete` completes when
  the ticket is complete or at its deadline, posted to the caller's
  executor. Repair's pushes and pulls, prompt replication and a durable
  member's catch-up acknowledge the member in every kept ticket naming
  the object; a ticket that settled at its level reports again when its
  last pending copy lands. `wait` and `settled` keep 3a's meaning.
- The reserve, as built: `IoReserve` counts device operations in flight
  on the node (default 8), of which DATA work may hold at most
  `slots - control_reserve` (default 6) and control work any free one;
  while control work waits, DATA admission is closed. DATA work holds DATA
  credit and a DATA slot across one device operation, never across a
  peer's answer; CONTROL objects, CONTROL presence and claims hold a
  control slot and never DATA credit (law 1). The bound: a control
  operation never waits for a slot behind DATA and shares the device with
  at most the DATA share's operations. Proven by
  `ALoaderSaturatingDataLeavesControlWithinABound` (8 loader threads on a
  4-slot, 2-reserve node, DATA held to its 2 slots and waiting; 20 CONTROL
  writes with their claims and 20 DATA claims, each under 2 s, with no
  control wait at all) and by `WithoutTheReserveControlQueuesBehindData`
  (the same saturation with no reserve holds a commit back).
- Admission of each served request (Macha's per-route leases): reads and
  writes of DATA wait for credit and a slot up to `admission_wait`; a
  full-read probe, a validated presence or a delete tries once and is
  refused 503 `storage_busy`; a DATA read in the control frame is refused
  400. The store's own device work (the write path's local copy, reads,
  opportunistic copies, prompt replication, repair) is admitted the same
  way.
- Messages (Dagda's, 512 plus protocol 23's): `delete_object` 525,
  `have_valid_objects` 559, `trie_diff` 561, replies 635 and 637.
  `replicate_all` is a function over `put_object`, not a message. Lanes
  as protocol 23 (CONTROL); a validated presence and a delete never in the
  control frame (they take DATA credit), a trie question in any.
  Payloads byte for byte, pinned by known answers computed outside both
  codebases (hashlib, struct): the validated presence request and reply
  (any byte but 0 and 1 reads as "cannot say", as Macha's reader does),
  the delete, a root trie question. No difference from Macha found.
- Changed from Macha, none on the wire's payloads: repair is diff only;
  credit waits always have a deadline (a zero no-progress window is
  refused); the arbiter logs outside its lock; repair's lists are held by
  `shared_ptr` (below); the server pins the pool's union, not the ledger
  service's.
- Tests moved from Macha (develop `5a06d3c`), converted to GoogleTest on
  3a's cluster, every assertion kept where it applies; where Macha
  asserted a probe, the case asserts none is made, and where it walked the
  store, the lists a survey gives are handed in:
  `test_rpc_cluster.cpp` `test_repair_push_probes_by_window_and_sends_together`
  (Dannan's server runs at most its DATA workers less the foreground
  reserve at once, 6 of the 8 sent together),
  `test_a_long_push_pass_does_not_hold_back_the_pull`,
  `test_repair_push_sends_nothing_to_a_peer_that_cannot_say`,
  `test_repair_pushes_past_a_peer_with_no_room`,
  `test_repair_pushes_from_what_peers_hold_without_asking`,
  `test_repair_pushes_only_what_the_list_names`,
  `test_repair_probes_without_credit_and_transfers_only_with_it`,
  `test_repair_pass_keeps_its_place_across_live_sets_and_restarts` (not
  its restart half: `repair_position`),
  `test_repair_bandwidth_estimate_ignores_small_transfers`,
  `test_repair_decides_already_held_without_reading_the_extent`,
  `test_repair_keeps_a_pull_that_was_in_flight_when_its_turn_ended`,
  `test_repair_is_paced_not_stopped_while_higher_classes_stay_busy`,
  `test_retained_missing_copy_repairs_without_namespace_reachability`,
  `test_held_retention_claims_cost_repair_no_credit`,
  `test_storage_data_credit_reserves_viewer_headroom_and_control` (the
  CONTROL request is `have_control_objects`: ping is Dannan's),
  `test_data_credit_wait_gives_up_when_nothing_is_moving`,
  `test_data_credit_wait_survives_genuine_contention`, repair's last step
  of `test_store_heals_a_bad_local_copy_from_a_peer`, the DATA credit half
  of `test_store_control_objects_take_no_data_credit_and_commit_alone`;
  `test_storage_v18.cpp` `test_repair_passes_unavailable_and_counts_unsourceable_objects`,
  `test_a_busy_peer_still_says_what_it_does_not_hold`,
  `test_min_write_floor_publishes_then_repair_converges_to_r2` (VALIDATION
  "Placement and the write floor"),
  `test_nodes_replicating_to_each_other_do_not_hold_their_slots_across_the_peer`,
  `test_repair_keeps_a_copy_it_has_nowhere_to_send`,
  `test_node_that_stops_hosting_drains_through_repair`;
  `test_storage_metadata.cpp` `test_repair_step_is_bounded_and_yields`;
  `test_io_pressure.cpp`'s seven arbiter cases; `test_availability.cpp`
  `test_a_survey_a_peer_could_not_finish_keeps_what_was_unavailable`,
  `test_the_survey_by_diff_finds_what_no_node_holds`,
  `test_what_this_node_lacks_is_the_referenced_set_less_every_disk`, and
  the survey's assertions of `test_what_only_a_host_out_of_reach_holds_is_unavailable`,
  `test_two_nodes_survey_what_neither_holds`,
  `test_a_disk_coming_back_is_surveyed_without_a_namespace_change` (their
  path tables and files API are Macha's).
- New: repair converging after a partition heals, by diff, with no probe
  and no walk; a ticket reporting the copy repair made; the reserve's
  bound under a saturating loader, and its unit cases (control admitted
  while DATA saturates, none without a reserve, a waiting control request
  served before DATA, stop); the survey through a test provider (and
  nothing before an inventory); a trie question naming a pinned snapshot
  (410, 413); a credit deadline on stepped time; the payload known
  answers and registrations.
- Not moved: `test_a_delete_is_reclaimed_while_a_node_is_away` (a whole
  node's pass over the FS), `test_replication_policy_change_on_restart`
  (settings reload, with Context v1), the heavy
  `test_retain_data_batches_a_large_publication_within_bounded_time`,
  the path-table cases of `test_availability.cpp` (Macha's).
- Findings:
  - This session's own, proven and fixed: repair compared its lists by
    raw address, and a list freed and the next made at its address was
    taken for the old one, so a new survey's lacks were ignored (18 of 60
    runs of the link-estimate case before; 0 of 120 after). Repair and
    `Repair`'s quiet state now hold the lists by `shared_ptr`.
  - In Macha, shown by reading, not by a test, not fixed (Macha is not
    touched): `repair_step` keys its lists' identity on `data()`
    (`distributed_store.cpp:2444-2494`), and maintenance refills
    `push_list` and `pull_list` in place (`maintenance.cpp:740-773`), so a
    changed list in the same buffer is not seen as a change: a pass a list
    changed under can be reported complete and repair go quiet until the
    next event. Dagda's lists are immutable snapshots held by pointer.
- Gate: warnings as errors; `dagda_tests` 284 and 1 disabled (236
  before). The 48 new cases and the store's 17 (whose paths admission
  now touches) ran 200 times each (four processes of 50): 12,998 of
  13,000 passing. The two failures were one race in the pacing case's
  test (credit banked while waiting for a peer's telemetry brought the
  copy back before any pass was turned away); it now waits to see the
  share refuse a pass first, and the revised case then passed 200 of 200.
  Before the list-identity fix above, the link-estimate case failed 18 of
  60 runs; after it, 0 of 120.
- Dannan: nothing after 0.2.0 is used. Request: a duty-cycle share by
  weight in `sched/` (Macha's `WeightedLoaderService`), file-local in
  `store/repair.cpp` with `TODO(Dannan: ...)` until then.

## 2026-11-04: step 3a, the write and read paths on Dannan (`feature/distributed-store`)

- Moved and adapted into `dagda/store/` (namespace `dagda::store`):
  `distributed_store` (`DistributedStore`: Macha
  `cluster/distributed_store.cpp:114-486` the write path, `:799-1134`
  claims and CONTROL claims, `:1201-1342` prompt replication and
  `put_on`, `:1344-1808` the read path, `:1810-2218` presence batches and
  CONTROL offers, `:3088-3184` opportunistic local copies;
  `StorageUnavailable`), `storage_server` (`StorageServer`: Macha
  `cluster/storage_server.cpp`), `replica_selector`, `control_objects`
  (`ControlObjectFetch`), and new `messages` (Dagda's numbers, the
  registrations, protocol 23's payloads). Placement adverts beside the
  scheme in `placement.h`: `dagda/extents` (hosts extents: the only
  candidates), `dagda/capacity` (the pool's limit), `dagda/used`.
  Requirements S22 to S30.
- Runs on Dannan 0.2.0: `Routes::call` and `call_and_wait` with a
  deadline on every call, `Nodes::placement` under the store's scheme
  filtered by `dagda/extents`, `Nodes::roster`, `active`, `watch` and
  `latency`, adverts, `ActivityClocks`, `NodeEvents` ("storage"),
  `SupervisedThreads`, a scoped logger. Handlers are Dannan's
  `RequestHandler`s; refusals are Dannan's error replies (status, code,
  text), "done" is Dannan's `ok`.
- Messages: Dagda's own from 512, each 512 plus Macha's protocol-23
  number (`have_object` 515, `get_object` 516, `put_object` 517,
  `get_control_object` 531, `put_control_object` 532,
  `put_object_deferred` 534, `object_durability_barrier` 535,
  `retain_objects` 544, `have_objects` 552, `have_control_objects` 557;
  replies 615, 616, 619, 630, 633), plus `durability_covered` 768 (new:
  a ticket's status without waiting). Lanes as protocol 23: objects on
  DATA in any class but control, the rest on CONTROL. Payloads byte for
  byte, pinned by known answers computed outside both codebases
  (hashlib and struct): an id request, an object, a bool reply,
  presence batches and replies, a ticket, a barrier with and without a
  probe, a probe's answer, a claim (`store_messages_test.cpp`).
- The durability levels, as built: every write takes a level and a
  deadline. Ephemeral queues the bytes (bounded, 64 MiB) and returns at
  once, every member named pending "queued"; a worker then writes it as
  replicated. Persisted and replicated are Macha's write path with a
  floor of 1 and of `max(1, min(write_copies, nodes))`: local first,
  spill past a peer stalled beyond `write_stall` or one that fails,
  return on what landed when nothing more can be had promptly, prompt
  replication for a second copy. Durable writes local first and to every
  live member of the replication set at once, spills a refusal to the
  next fallback, never spills a stall, and returns when every live member
  has its copy or at the deadline. The replication set is the first
  `replication` capacity owners over every hosting node the roster knows,
  so a member Dannan sees as dead is in it, not waited on, and named
  pending "dead". Acknowledged means the node's strict put returned (its
  domain's generation durable); a deferred write's copies are pending
  "awaiting barrier" until `wait()` runs their barriers. Evidence names
  each member by node id, then any other node reached, with its state and
  why, `sought`, `met`, `settled` (nothing awaited), `lost`; and a ticket.
  A call still in flight when the write returns runs on to
  `call_horizon` (120 s) and lands in the ticket; a durable ticket pushes
  the object to a dead member when the roster says it is back; prompt
  replication's copies land in the ticket too. `status(ticket)` reads it,
  `wait(ticket, deadline)` completes when it settles or at the deadline
  and may be called again. 4,096 tickets are kept, the oldest settled
  forgotten first. Every asynchronous completion is posted to the
  caller's executor.
- Reads: local, then the cache (promoted when this node owns it), then
  the owners and the other ranked nodes in the replica selector's order,
  every reader of one object sharing one fetch and one buffer (the
  reply's retained frame); `read` names the source and the serving node,
  `not_found` only when every candidate said 404. `has(id)` is the pool.
- CONTROL: `offer_control` (asks `have_control_objects`, sends what is
  missing, pipelined at a quarter of a connection's replies),
  `retain_control` (local first then nearest by Dannan's
  `order_commit_replicas` and latency, `metadata_write_copies` claims),
  `ensure_control_local` (any active peer, speculative, checked by id),
  none of it on the DATA device. `retain_data` (batched presence, one
  claim per node) moved with them.
- 0.91.5: `put` and `put_deferred` (the id-returning forms) throw
  `StorageUnavailable` when nothing holds the write.
- Changed from Macha, none on the wire's payloads: every read shares its
  fetch (Macha kept deadline reads private; here every call has a
  deadline, and a reader that gives up abandons the transfer only when
  nobody else waits); an unmeasured peer is estimated by Dannan's round
  trip, not 100 ms; decoders refuse a count larger than the payload
  before reserving for it (section 7); a peer's room is read from its
  adverts; there is no DATA credit or retained-memory admission (3b);
  frame promotion of a joining reader uses `CallHandle::promote`.
- Tests moved from Macha (develop `5a06d3c`), converted to GoogleTest
  on in-process Dannan clusters (`dagda/test/fixtures/cluster.h`: real
  nodes on loopback, each with pool, control store, cache, claims,
  server and store, a hook in front of the server to hold, refuse or
  count), every assertion kept where it applies:
  `test_rpc_cluster.cpp` `test_store_put_is_loader_activity_not_viewer_activity`,
  `test_store_put_publishes_at_the_floor_and_copies_promptly` (the full
  owner by its capacity advert against the extent size),
  `test_store_put_replaces_a_replica_that_cannot_be_launched` (a peer
  that fails the call at once), `test_store_claims_ask_nothing_more_about_objects_nobody_holds`
  (the claimant hosts no extents, so the peer is the one candidate, as
  Macha's bench made it by capacity), `test_store_put_to_a_silent_replica_returns_on_the_local_copy`,
  `test_store_concurrent_readers_share_one_fetch_and_buffer` (memory
  read from Dannan's ledger), `test_store_heals_a_bad_local_copy_from_a_peer`
  (but repair's last step), `test_store_runtime_cache_keeps_a_playback_fetch`,
  the store's half of `test_store_control_objects_take_no_data_credit_and_commit_alone`;
  `test_storage_v18.cpp` `test_a_full_preferred_node_spills_data_but_not_control`
  (its catalogue as CONTROL objects and claims),
  `test_min_write_two_uses_fallback_when_preferred_replica_is_full`,
  `test_a_control_graph_larger_than_the_connection_budget_still_publishes`,
  `test_a_control_offer_sends_only_what_the_peer_lacks`, the store's half
  of `test_catalogue_control_objects_recover_on_metadata_replica`,
  `test_edge_node_never_owns_and_its_writes_land_on_owners`,
  `test_durability_barrier_across_peer_restarts` (a copy once on disk
  stays acknowledged, so the re-derivation is shown on a peer restarted
  before its barrier). VALIDATION "Placement and the write floor" (but
  repair's convergence) and "Class separation" through these.
- New: each level (ephemeral returns at once and its ticket reports the
  copies; persisted on the first copy; replicated at write_copies;
  durable on every live member; durable with a dead member, named pending
  and caught up when it returns; durable with a silent live member,
  waited to the 400 ms deadline, pending, then reported by the ticket);
  `StorageUnavailable`; the executor rule (writes, ephemeral, a settled
  ticket's wait, a local read and an unknown ticket, all posted); a read
  naming its server; the replica selector (4); the payload known answers
  and registrations (9).
- Not moved: `test_commit_replicas_ordered_local_then_nearest` (Dannan's
  `order_commit_replicas` now); `test_min_write_floor_publishes_then_repair_converges_to_r2`,
  the repair, survey and arbiter cases (3b);
  `test_nodes_replicating_to_each_other_do_not_hold_their_slots_across_the_peer`
  (the arbiter's slots, 3b); the transport cases of `test_rpc_cluster`
  (Dannan's).
- Findings: none in Macha proven. Two of this session's own, fixed: a
  call not yet placed (`idle_for()` is max) was taken for stalled and
  spilled at once; a fetch woke its readers before queueing its local
  copy, so `wait_local_copies_settled()` could miss it (5 failures in
  7,600 runs before; Macha queues first).
- Gate: warnings as errors; `dagda_tests` 236 and 1 disabled (198
  before). The 38 new cases ran 200 times each (four processes of 50):
  all 7,600 passing after the second fix above.
- Dannan: nothing after 0.2.0 is used. Requests (generic, marked
  `TODO(Dannan: ...)` in `distributed_store.h`): a call's deadline that
  also cancels the call (today the caller is told at the deadline but the
  call runs on, holding a reply slot); the node's executor and its
  `TimeSource` through the Context (the store takes both beside it).

## 2026-11-04: step 2, the local ledger and GC (`feature/ledger`)

- The state layout marker, decided as Dagda's: `dagda/store/state_layout`
  (`open_state_layout`; Macha `cluster/node_identity.cpp:15-40`):
  `<state>/storage-layout` holding `macha-state-layout-v18\n`, a non-empty
  unmarked state refused ("fresh namespace"), `.macha.lock` allowed. One
  change: a missing state directory is created (Macha's root had made it).
- Moved and adapted into `dagda/contract/` (namespace `dagda::contract`):
  `claim_store.h` (`ClaimStore`, `RetentionClass`, `RetentionDot`,
  `RetentionClock`), `object_ledger.h` (`ObjectLedger`), `horizon.{h,cpp}`
  (`ReferencedSets`, `InventoryHorizon`, `ReleaseHorizon`, `ReleaseBuild`),
  `gates.{h,cpp}` (`PassFacts`, the three gates), and new
  `reachability.h` (`ReachabilityProvider`, `Reachability`, `Garbage`,
  `GarbageBatch`, `ControlEpoch`, `GarbageCollection`). Into
  `dagda/ledger/`: `retention` (`RetentionStore` and its pure
  `retention_format`), `retention_ledger`, `reference_counts`,
  `node_horizon_builder` (`build_inventory`, `build_release`,
  `NodeHorizonBuilder` over a provider), `claim_walk` (Macha's
  `service/claim_walk`), and `collector` (`Collector`: Macha's
  `service/maintenance.cpp:620-660` and `:866-1071`, with
  `CatalogueManager::control_gc_step`, `catalogue.cpp:2355-2433`, moved in).
  Requirements L12 to L24, C3. Dannan's `sched/walk.h` (Budget, Cursor,
  Page) and `contract/published.h` are used; Dagda has no copy.
- The provider (requirements 5.3): `generation()`, `known_generation()`,
  `inventory()` and `release(head)` answer a `Reachability` (counted views,
  DATA and CONTROL ids, retirements and their batches, completeness, the
  stamp); `release_head()` is the owner's sole accepted head;
  `control_epoch()` and `within_control_epoch(sequence, removal)` are the
  catalogue's root epoch and its lock as an answer; `collect_garbage()`
  receives tombstone collection's decisions. Garbage is the provider's
  type (`Garbage {id, retired_at_ns, token}`, Macha's `GarbageRef` with its
  retirement id as the token; `GarbageBatch` Macha's tombstone batch). The
  ledger and the contract name no catalogue, filesystem, namespace or
  metadata type; a test enforces it.
- Kept as Macha has it: the gates' reasons and trace words ("catalogue
  inventory incomplete", `catalogue_complete=`), the trace kinds and the
  observation names (`catalogue.control_gc.removed` among them), so
  Macha's decision-trace fixtures read the same; budgets 64/64/64/32; the
  orphan grace `max(garbage_grace, no_progress_backoff)`; sightings saved
  on completion or every 30 s.
- Legacy readers dropped (Tom, 2026-11-04): the monolithic checkpoint
  `retention/claims.meta` (MRTS0001, `decode_monolithic_checkpoint`) and the
  sharded one `claims.current` naming `checkpoints/gen-*/xx.meta`
  (MRTS0002, `decode_checkpoint_shard`, `read_clocks`, `read_small_text`,
  `load_checkpoint_files`, `migrate_locked`'s move). Their names stay
  reserved: a state holding either without the tries' marker
  (`retention/ledger`) is refused with `std::runtime_error`, never opened
  empty, and the first checkpoint removes them as Macha's does. The WAL's
  64 MiB read limit (Macha's `max_legacy_frame`) is kept: it reads
  frames an older writer wrote, and is not a reader of an old format.
- Byte compatibility, pinned by known answers computed outside both
  codebases (hashlib; AES blocks by `openssl enc -aes-256-ecb`, GHASH by
  hand, checked first against 1b's known frame): the WAL's five
  operations' plaintext; a five-frame `claims.log` Macha would write,
  replayed here; its checkpoint, each trie's published `root` byte for
  byte and its node file's size; a checkpoint Macha saved, opened here;
  the claims record value; `retention/ledger` = `1\n`; the reference
  counts' `state` (MREF0001) both ways and a count's u64.
- Changed from Macha, none on disk: an invalid dot handed to
  `retain_batch` is `std::invalid_argument` (Macha `std::runtime_error`;
  from the journal it is a `DecodeError`); retirement ages are taken from
  the wall clock's milliseconds (Macha's `wall_ns`); the claim walk counts
  into an injected `Observations` and records no per-object timing; an
  inventory the provider cannot answer throws out of `pass()` having
  removed nothing.
- Tests moved from Macha (develop `5a06d3c`), converted to GoogleTest,
  every assertion kept: `test_ledger_gates.cpp` (all 7);
  `test_claim_walk.cpp` (all 8); `test_horizon_builder.cpp` (all 6, as
  provider answers: what they proved of the FS's walk is Macha's);
  `test_storage_metadata.cpp` (`test_retention_claims_are_causal_durable_and_observed_remove`,
  `test_retention_prune_cursor_cannot_starve_later_tombstones`,
  `test_retention_prune_keeps_a_row_claimed_while_it_checks_presence`,
  `test_retention_claims_checkpoint_into_the_ledger_and_restart`,
  `test_retention_checkpoints_after_enough_changes`,
  `test_retention_start_checkpoints_a_long_journal`,
  `test_retention_prune_survives_a_restart`,
  `test_retention_claim_is_physical_gc_barrier`; and
  `test_retention_shard_checkpoint_moves_into_the_ledger`, whose reader is
  dropped, as the refusal and the removal at the first checkpoint);
  `test_object_ledger.cpp` `test_reference_counts_follow_and_survive_a_restart`;
  `test_baselines.cpp` (`test_baseline_claim_walk_per_object`,
  `test_baseline_claim_walk_on_the_ledger_per_object`);
  `test_hydration_catalogue.cpp`, the CONTROL GC block of
  `test_catalogue_on_one_node_predicts_caches_and_protects_its_control_objects`;
  `test_storage_v18.cpp`, the state half of `test_unversioned_state_is_refused`.
  `test_absent_nodes.cpp`'s GC cases moved in 1d.
- New: a claim from an unseen branch survives a partition (store and
  sweeps); an inventory never used destructively in the pass that built
  it; an incomplete release never published; an inventory behind or
  incomplete sweeps nothing; tombstone collection's decisions; a CONTROL
  removal never races a new epoch; the CONTROL sweep in a pass; the
  include-direction check (`contract_include_direction_test`, with a
  planted self-check).
- Not moved: `test_catalogue_maintenance.cpp` (every case drives the
  catalogue; the ledger's side, an inventory complete only at the head and
  data released when nothing refers to it, is the provider's answer);
  `test_maintenance_trace.cpp` (a whole node's pass; the trace words are
  kept so it reads the same at the port); `test_namespace_tree.cpp`
  `test_every_tree_node_is_reachable_for_the_collector` (the FS's walk).
- Left for step 3: the availability survey, repair by diff, the claim
  walk's restorer with network credit, the pass's scheduling (quiet
  windows, events, repair share) and the DATA sweep's `should_yield` from
  the store's idle times. Several providers per domain, unioned: one
  provider for now; it waits on domains (step 6).
- Findings, none fixed (none blocks this step): Macha's retention store,
  finding no `retention/ledger` marker, rebuilds both tries empty before
  replaying the journal, so a lost marker loses every checkpointed claim
  (kept, as Macha's; worth a refusal when the tries hold records). The
  CONTROL step compares an age on the wall clock with a span on the
  steady one (kept).
- Gate: warnings as errors; `dagda_tests` 198 and 1 disabled (145
  before). The 51 new cases other than the two baselines ran 200 times
  each (four processes of 50 repeats), all 10,200 passing.
- Dannan: nothing after 0.2.0 is used; no new request.

## 2026-11-04: step 1d, the pool, the CONTROL store and the cache (`feature/local-store`)

- Moved and adapted into `dagda/store/` (namespace `dagda::store`):
  `storage_pool` (`StoragePool`, `StorageBackendOptions`,
  `StoragePoolOptions`, `StaleDurabilityTicket`), `io_pressure`
  (`DiskServiceMonitor`, `DiskServiceTimer`), `persistent_cache`
  (`PersistentBlockCache`, `CacheOptions`), `control_store`
  (`ControlStoreOptions`, `open_control_store`: Macha
  `cluster/local_state.cpp:134-142`), `placement` (the store's
  `data_scheme()` over Dannan's `placement::by_capacity`); into
  `dagda/ledger/`: `unreferenced_since` (requirement L22 is the ledger's
  and step 2's sweeps use it); into `dagda/contract/`: `id_lookup.h`
  (`IdLookup`). `ledger_cache_share` (`local_state.cpp:29-50`) is in
  `local_store.h`. Requirements S10 to S12, S14, S15, S18, S19, S21, L4,
  L21, L22.
- Runs on Dannan: the pool takes the node id and the process's durability
  epoch, the domain's `StorageKey` and `LocalStore::Services`
  (supervised threads, observations, a scoped logger, injected time and
  wall clock); the I/O monitor logs through its own scoped logger and its
  timer reads the injected time. Options are plain structs with Macha's
  defaults (`storage.packing` 1 MiB and 64 MiB, a 500 ms window, the DATA
  share of a 64 MiB ledger cache; `metadata_store` 4 GiB at
  `<state>/metadata-objects`; no cache unless a path and blocks are
  given). No Macha `Config`.
- Placement: for one copy, Macha's `capacity_placement_nodes` and
  Dannan's `by_capacity` compute the same thing (SHA-256 of the id's
  first four bytes, the salt and the backend's token; -ln(u)/capacity,
  zero capacity weighing 1, ties to the lower id), so the store names
  Macha's salts (`macha/data-fallback/v7`, `macha/data-extra-domain/v7`)
  and the capacity advert `dagda/capacity`, and a pool ranks its backends
  exactly as Macha's did. Pinned by known answers computed outside both
  codebases at 60 digits (`RanksBackendsAsMachaDoes`).
- Byte compatibility: the backend marker and its mirror
  (`macha-backend-v18 <node> <token>\n`, `backend-identities/<sha256 of
  the path>.id`), the ledger directory `ledger/data-<token>` and a marker
  Macha wrote are pinned (`TheBackendMarkerIsLayoutV18s`); the sightings
  file byte for byte (`TheFileIsLayoutV18s`). The backend-identities
  directory is created 0700 by Dannan's durable file (1a's map).
- Changed from Macha, none on disk: the pool's tickets carry the epoch and
  a ticket from another one throws `StaleDurabilityTicket` (Macha checked
  it in the server); a duplicate backend is `std::invalid_argument`,
  checked before anything changes (Macha could apply half); `degraded()`
  says a plane with backends but none online is not ready (section 7);
  the control store has a `reserve_free` option (section 7), zero
  (Macha's) by default; the pool's `TimedLock` is a plain `Lock`.
- The cache's metadata seed: left for step 4. What was left:
  `remember_metadata(MetadataRecord)` and `metadata()` (Macha
  `persistent_cache.cpp:325-390`): the file `<cache>/metadata/current.meta`
  (`DHTCMET1`, nonce, tag, u32-length ciphertext, AAD the magic, under the
  storage key), written by a temporary and rename without a sync, the
  last written record's hash kept to skip a rewrite and forgotten on every
  reconfiguration, read back only while the cache is on and
  `prefer_metadata` set. The seam is `PersistentBlockCache::with_seed(use)`:
  it calls `use` with the seed's directory and the cache's `opening` (new
  on every reconfiguration) under the seed lock reconfigure() takes, and
  not at all while the cache is off or `CacheOptions::seed` (Macha's
  `prefer_metadata`) is unset. The six seed assertions of
  `test_storage_pool_and_persistent_cache` move with it.
- Tests moved from Macha (develop `5a06d3c`), converted to GoogleTest,
  every assertion kept but the six seed ones above:
  `test_storage_v18.cpp` (the `StoragePool` and placement blocks of
  `test_admission_respects_reserve_and_capacity`; the backend half of
  `test_unversioned_state_is_refused`;
  `test_metadata_control_store_is_independent_of_data_quota`, store-side);
  `test_storage_metadata.cpp` `test_storage_pool_and_persistent_cache`;
  `test_invariants.cpp` (`test_rebalance_reads_nothing_with_one_backend_online`,
  `test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy`,
  `test_storage_pool_tokens_name_physical_domain_and_backend_incarnation`,
  `test_persistent_cache_is_explicitly_ephemeral`); `test_presence.cpp`
  (`test_a_backend_offline_and_readopted_leaves_presence_exact`,
  `test_the_held_identity_names_exactly_what_the_pool_holds`,
  `test_the_held_identity_is_the_same_on_any_number_of_disks`);
  `test_baselines.cpp` `test_baseline_data_store_has`;
  `test_io_pressure.cpp` (the six monitor and pool cases);
  `test_absent_nodes.cpp` (the six sightings cases). The two Linux-only
  cases run everywhere (barriers counted on the pool's domains; the
  cache's lack of syncs shown by no barrier recorded and no accounting
  file). Crash matrix row "Cache missing/corrupt" (new: Macha names no
  case).
- New: the known answers above, the first-ranked backend taking each
  object, backend identity refusals (another node's disk, the wrong disk,
  a lost marker), stale tickets and reassertion, the claim seam
  (`ClaimCheck`: a claimed copy is never swept, and a claim made between
  the slice's look and its delete stops the delete), the control store's
  place, defaults and reserve, the timer on stepped time.
- Not moved: the other seven `test_io_pressure.cpp` cases (the DATA credit
  arbiter, step 3); `test_absent_nodes.cpp`'s two roster cases (Dannan's);
  the state half of `test_unversioned_state_is_refused` (Macha's node
  identity: below); `test_a_full_preferred_node_spills_data_but_not_control`
  (two nodes, step 3); `test_retention_claim_is_physical_gc_barrier`
  (claims, step 2); `test_metadata_recovery_cache_seed_is_not_accepted_authority`
  (heads, step 4).
- Findings, each in Macha too and fixed here, none on disk:
  - A lost wakeup: the accounting scan set "complete" (and "failed") and
    notified without the mutex its waiters check under, so a put or
    remove could wait for ever on a finished scan
    (`local_store.cpp`, `mark_scan_failed` and the scan's last block).
  - A deadlock: put, remove and remove_if_older_than held the object's
    lock while waiting for the scan, and the scan takes each object's lock
    as it walks. A cache opened with a corrupt entry, read at once (the
    read removes it), hung in 13 of 20 runs. They now wait for the scan
    before taking the object's lock (`await_accounting`).
  - The held ledger marked itself seeded and then installed what was
    queued during its build, so in between `has()` and `held_view()` could
    miss a put that had returned (seen twice in the pool's presence case
    and the control store's). It now installs first, then marks itself
    seeded; the files are written in the same order.
  - Left as Macha has it: compaction's stop callback notifies without the
    mutex, so a stop requested between a waiter's check and its sleep can
    be missed until the scan ends (locking there could deadlock when the
    stop is already requested at construction).
- Section 7, owed: a reserve of device I/O for control work is the DATA
  credit arbiter's (step 3), which this monitor feeds; the monitor moved
  as Macha has it, which by itself reserves nothing.
- Gate: warnings as errors; `dagda_tests` 145 and 1 disabled (108 before).
  The 37 new cases other than the has() baseline, with the local store
  and held ledger suites the fixes touch (other than the small-values
  timing), 69 cases, ran 200 times each (four processes of 50 repeats):
  all 13,800 passing, after the three fixes above; before them the
  cache's corrupt-entry case hung in 13 of 20 runs and the whole binary
  hung twice in the cache's suite.
- Dannan: the pool's lock wants `TimedLock` (marked
  `TODO(Dannan: TimedLock)` in `storage_pool.h`). No Dannan symbol after
  0.2.0 is used.
- For Tom: the control store's `reserve_free` default (Macha's 0 or a
  figure that keeps CONTROL writable past DATA's reserve); and where the
  state layout marker (`storage-layout`, `macha-state-layout-v18`, Macha
  `cluster/node_identity.cpp:15-40`) lives, Dagda's state directory or
  Dannan's node identity, which step 1's "Refusing unversioned storage"
  needs.

## 2026-11-04: step 1c, the local store (`feature/local-store`)

- Moved and adapted into `dagda/store/` (namespace `dagda::store`):
  `local_store` (`LocalStore`, `LocalStoreOptions`, `LocalStoreMode`,
  `LocalStoreDiagnostics`, `StorageLock`), `local_store_files` (the device
  seam), `local_format` (layout v18's loose header, pack record, accounting
  slot and names, pure), `object_lock` (`ObjectLock`, `ObjectLocks`: Dagda's
  own, as decided), `write_behind`; and `dagda/contract/object_store.h`
  (`dagda::contract::ObjectStore`, `HeldView`, `HeldIdentity`). Loose files
  above the threshold, packs at or below it, the scan at start, torn-tail
  truncation, damaged spans skipped and counted, copy-on-write compaction,
  tombstones and touch records, A/B accounting, `has()` from the index or
  the held ledger, the ledger kept current, durability through the
  `DurabilityDomain` (mutations reported, generations awaited), startup
  progress ticked by the scan (requirements S1-S9, S16-S18, L3).
- Runs on Dannan: `SupervisedThreads`, `Observations`, `StartupProgress`,
  a `ScopedLogger`, an injected `TimeSource` (the default domain's window)
  and `WallClock` (pack record times, loose modification times, ages), the
  domain's `StorageKey`. Options are a plain struct with Macha's defaults
  (packs at or below 1 MiB, 64 MiB targets); `loose_only(limit)` is Macha's
  second constructor. No Macha `Config`.
- Byte compatibility: every format is Macha's, pinned by known answers
  computed outside both codebases (hashlib over 1a's pinned sealing, under
  1a's known-answer key): a whole loose file; put, touch and tombstone
  record headers; an accounting slot. Files Macha wrote (a loose object; a
  pack with a put, a touch, then a tombstone) open here, and the store
  writes Macha's touch and tombstone records exactly and Macha's put and
  loose layouts with fresh nonces (`store_local_format_test.cpp`).
- Changed from Macha, none on disk: the held ledger is made before the scan
  thread starts (Macha starts the scan, which reads the ledger pointer, then
  assigns it: a race); option errors are `std::invalid_argument`; a packed
  object that fails to open throws 1a's `ObjectIntegrityError` ("object
  integrity failure", Macha "packed object integrity failure"); `own_syncs`
  in the diagnostics counts the store's own fsyncs; descriptors are
  close-on-exec.
- Tests moved from Macha (develop `5a06d3c`), converted to GoogleTest,
  every assertion kept: `test_storage_v18.cpp`
  (`test_packs_store_compact_and_survive_restart`,
  `test_pack_recovery_settles_each_kind_of_damage`,
  `test_held_device_io_blocks_only_its_own_object`,
  `test_presence_is_known_without_reading_objects`, the `LocalStore` block of
  `test_admission_respects_reserve_and_capacity`); `test_storage_metadata.cpp`
  `test_local_store`; the four `test_object_ledger.cpp` cases 1b left;
  `test_presence.cpp` (`test_has_answers_a_put_in_progress_without_waiting_for_it`,
  `test_a_put_that_fails_midway_is_never_present`,
  `test_has_does_not_consult_the_device_after_warm_up`,
  `test_presence_is_exact_across_a_restart`); `test_invariants.cpp`
  (`test_authoritative_deferred_generation_batches_stable_storage_barriers`,
  `test_accounting_dirty_marker_is_process_session_scoped`,
  `test_clean_accounting_checkpoint_is_trusted_with_packs`,
  `test_unclean_accounting_recovery_establishes_durable_baseline`,
  `test_restart_durable_reaffirmation_requires_no_new_barrier`,
  `test_authoritative_delete_is_lazy_durability`,
  `test_local_store_barrier_remembers_complete_physical_cut`,
  `test_durability_domain_group_commits_independent_publications`,
  `test_immediate_reaffirmation_flushes_provisional_generation`). Macha
  counted fsync and syncfs by interposing them, Linux only, so eight of
  these were skipped off Linux; here they run everywhere, a syncfs being
  one of the domain's barriers and Macha's fsyncs the store's
  `own_syncs`. Batch windows run on stepped time. Crash matrix rows for the
  local store: "Pack tail torn", "DATA accounting dirty", "GC unlink lost"
  (moved), and "Pack compaction after replacement install but before old
  deletion" (new: Macha has no case).
- New: S1's hash mismatch, S3's option checks, the limit half of S9, ages
  on the wall clock, `list()`, the write-behind window, and the small-values
  case: 10,000 objects of 100 to 1,000 bytes (5.5 MB) all in one pack,
  written with `put_deferred` and one barrier in 1.0 to 1.5 s, reopened in
  about 30 ms, every object read back in about 250 ms (this Mac, Release).
- Not moved: the `StoragePool` and placement blocks of
  `test_admission_respects_reserve_and_capacity`, the pool cases of
  `test_presence.cpp`, `test_baseline_data_store_has` and the rebalance case
  of `test_invariants.cpp` (1d, with the pool); `load_or_create_node_id`
  (the node id is Dannan's).
- Findings:
  - Dannan's secure memory is not inherited by a fork (`VM_INHERIT_NONE`,
    `MADV_DONTFORK`), so a forked child holding the parent's `StorageKey`
    faults on it, even to free it. Macha's fork cases kept the key in
    ordinary memory; here the child derives its own.
  - `test_verification_resumes_after_a_restart` is racy in Macha too: the
    ledger writes `seeded` just before it answers, so a check begun
    between is a no-op. Seen once in about forty runs under load; the case
    now also waits for the store's own answer.
  - A small put costs about 100 to 150 us here: a `statfs` for the reserve
    and an open, append and close of the pack per record, each kept as
    Macha has them. Keeping the active pack open and checking free space
    less often are the obvious savings; not made, to keep 1c Macha's.
- Gate: warnings as errors; `dagda_tests` 108 and 1 disabled (67 before).
  The 40 new store cases other than the small-values timing ran 200 times
  each (four processes of 50 repeats), all passing, after the ledger
  wait above and a fix to the new compaction case (it read `used` before
  its reconciling scan had finished; 400 further runs of it clean).
- Asked of Dannan: a scoped unlock beside `Lock` (`Unlocked`, file-local in
  `local_store.cpp` until then). No Dannan symbol after 0.2.0 is used.

## 2026-11-04: step 1b, the held trie (`feature/local-store`)

- Moved and adapted into `dagda/ledger/` (namespace `dagda::ledger`):
  `sealed_journal`, `object_trie`, `held_ledger`, `trie_union`, and
  `trie_diff` whole (the local diff, `records_not_held`, and the pure
  protocol-23 encodings of trie questions and answers). Each takes the
  domain's `StorageKey` as a `shared_ptr` (snapshots and frame readers
  outlive their trie) and a Dannan `ScopedLogger`; files, AADs, magics,
  hashes and frames are Macha's (requirements 4.1, L1-L3, L5, L6).
- Known answers, computed outside both codebases (hashlib, `openssl enc
  -aes-256-ecb`, GHASH by hand, checked against GCM test case 16) and
  pinned: a trie journal frame (`MACHLJ01`) both ways; the root hash and
  the published `root` file for 3 and 300 fixed ids; a node file
  (`MACHLN01`) and root Macha would write, opened here; the four wire
  encodings.
- Tests moved from Macha (develop `5a06d3c`), converted to GoogleTest,
  every assertion kept: 19 of `test_object_ledger.cpp` (journal 4, trie
  10 with the heavy one disabled, diff 3, union 1, held ledger 1) and
  `test_trie_questions_and_answers_survive_the_wire` of
  `test_availability.cpp`.
- Not moved: the four `test_object_ledger` cases that drive a
  `LocalStore` (1c) and `test_reference_counts_follow_and_survive_a_restart`
  (`ledger/reference_counts`, the namespace's counts: step 2 or the heads
  step). Left for step 3: the diff's network rounds (snapshot pinning,
  503/410, the remote source) in `availability_service`.
- Gate: warnings as errors; `dagda_tests` 67 and 1 disabled (the heavy
  measurement: 222,000 records open in 273 ms against a 30 s bound). The
  two concurrency tests (`ASnapshotIsFrozenThroughWritesAndRewrites`,
  `AFlushDoesNotWaitForTheSeed`) ran 200 times each, all passing.

## 2026-11-04: step 1a, foundations and sealing (`feature/local-store`)

- `docs/macha-foundations-map.md`: every Macha foundation the local store
  uses, mapped to Dannan 0.2.0 or marked Dagda's own. **No store format
  depends on anything Dannan encodes differently**: the codec, SHA-256
  and `Hash256` ordering, HKDF and AES-256-GCM are byte-identical, so
  Dagda has no codec of its own. Dannan's `ClusterKey::derive(
  "macha/storage/v1")` is Macha's storage key exactly.
- Moved and adapted: `store/object_id`, `domain/storage_key`
  (`StorageKey::derive_v1`, held in Dannan's `Secret`), `store/sealing`
  (id as AAD, size and id checked on open), `store/presence_index`,
  `store/durability_domain` (Dannan's supervised threads, observations
  and an injected time source; a stop-aware shutdown wait),
  `store/durability_ticket` (protocol-23 bytes).
- Tests moved from Macha (develop `5a06d3c`), converted to GoogleTest:
  `test_presence_index_matches_its_model_over_every_short_history`,
  `test_presence_index_warmup_race_cases_by_name`,
  `test_a_durability_barrier_is_timed_per_filesystem`, the crypto
  assertions of `test_codec_and_crypto`. Known-answer tests pin the key
  derivation and sealing to Macha's bytes (computed outside both
  codebases with `openssl` and checked against GCM test case 16).
- Gate: warnings as errors; `dagda_tests` 37. The durability tests ran
  25,600 times (8 in parallel) with no hang.
- Open from this session:
  - One stall of over 180 s was seen once by the session's first
    300-loop run, never reproduced (5,700 runs by the session, 25,600 by
    the review). The libc++ in use (20.1) has the corrected stop-aware
    `condition_variable_any` wait, and no remaining path to a hang was
    found. Watch for it.
  - `await_durable` waits without a deadline, as Macha's does; the
    stack's rule is that no call waits without one. It gains a deadline
    with the durability levels at step 3.
  - Only the per-file fsync path runs on this Mac; the Linux `syncfs`
    path compiles but has not run.
  - Asked of Dannan: `set_thread_name`, an exported `resident_bytes`,
    `elapsed_us` / `ObservedDuration`. `ObjectLock` (a per-object lock
    across a writer's I/O) is store-only and is Dagda's, in 1c;
    `WriteBehind` moves into Dagda with 1c (Macha's FS will link Dagda).

## 2026-11-04: step 0, the skeleton (`feature/skeleton`)

- First commit on `main` (`8249997`): conventions, the stack document,
  the plan and the requirements; `develop` branched from it.
- The shared layout: top-level `CMakeLists.txt` (`project(dagda VERSION
  0.1.0)`, `DAGDA_BUILD_TESTING`, `DAGDA_WARNINGS_AS_ERRORS`,
  `DAGDA_BUILD_FUZZING`), the `dagda` shared library (`dagda::dagda`) in
  `dagda/`, generated `dagda/version.h`, `dagda_tests` in `dagda/test/`.
- Dannan pinned at 0.2.0 by `find_package(dannan 0.2 CONFIG)`, linking
  `dannan::dannan`; built from the tag into `build/deps/prefix`
  (`CONTRIBUTING.md` has the lines). Dannan's header-only fixtures are
  used from the install, `DANNAN_TEST_TEMP_ROOT` set to `build/test-tmp`.
- Gate: builds with warnings as errors; `dagda_tests` 3 (library matches
  its headers; built against the Dannan it links, 0.2; Dannan's
  `TempDir` writes inside the build tree).
- Next: step 1, the local store. Its code can move now; its gate waits on
  the Macha session's step-1 fixture, which follows Macha's tree-native
  branch merge.

## 2026-11-03, night: the project starts

- `git init`; basemind set up (`basemind.toml`, rules in the gitignored
  `CLAUDE.local.md`) and the index built.
- Dannan's conventions taken whole: `.clang-format`, `.gitignore`,
  `LICENSE`, `docs/principles-and-laws.md` (Macha's master, with Dannan's
  source note), `CLAUDE.md` and `CONTRIBUTING.md` in Dannan's form.
- `TODO/2026-11-03-stack.md`, the delineation of Dannan, Dagda and Lugh,
  drafted with the Dannan and Lugh sessions' reviews, under Tom's four
  rules for the stack. No code; no commits. Gate: none yet.
