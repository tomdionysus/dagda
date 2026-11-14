# Macha store map: StoragePool, LocalStore and what sits under them

Written 2026-11-14 from code read on both sides. Nothing here was built or run.

- **M:** is Macha at develop f0597a4, extracted read-only at
  `/Users/tom/devroot/dagda/build/macha-f0597a4/` (paths below are relative to it).
  Macha tests are under `tests/` there.
- **D:** is this repository (`/Users/tom/devroot/dagda`) at develop 59b20e9.
- Macha reaches these parts only through `dagda::Node` and `dagda::Context`:
  `Node::data()` returns the DATA `store::StoragePool&` (D:dagda/node.h:361),
  `Node::control()` the CONTROL `store::LocalStore&` (D:dagda/node.h:362),
  `Node::reconfigure(NodeReload)` is the live reload (D:dagda/node.h:471),
  the local stages are `Node::local_rebalance/compact_packs/local_scrub/ledger_verify`
  (D:dagda/node.h:450-457), and `Context::Objects::has` is the pool's `has`
  through `DistributedStore::has` (D:dagda/context.cpp:143,
  D:dagda/store/distributed_store.cpp:585).
- Macha's surviving callers (code that stays Macha's) of these parts, found by
  `basemind code grep` over Macha's tree and confirmed in f0597a4:
  `src/filesystem/filesystem.cpp:1208` (`data().get`), `:2420-2421` (`data().limit/used`);
  `src/catalogue/catalogue.cpp:1392-1393,1796` (`data().has`, `control().has`),
  `:1047,1068,1128,1807,2489,2499,2518,2595,2613,2627` (`control().get`),
  `:1862,1865` (`control().put`), `:2680` (`control().next_object`), `:2699`
  (`control().older_than`), `:2724` (`control().remove_if_older_than`), `:2735`
  (`control().compact_packs`); `src/metadata/namespace_control_store.cpp:20,30,32,39,43`
  (`control_.put/has/get`); `src/torrent/torrent_manager.cpp:1325-1328`
  (`data().service_monitor().note`); `src/api/status_api.cpp:1100` (`data().diagnostics`);
  `src/service/maintenance.cpp:1031,1105,1159,1166,1215-1216` (the local stages, which move
  into the Node); `src/cluster/local_state.cpp` and `local_services.cpp` (construction and
  reload, which move into the Node). The callers inside `distributed_store.cpp`,
  `storage_server.cpp` and `control_objects.cpp` move into Dagda and are mapped by
  their own sections; they are cited here only where they define what a pool
  operation must deliver.

## 1. Operation table

Verdicts: SAME (no observable difference to a caller), DIFFERS (exactly how), MISSING
(Macha needs it, Dagda has nothing).

### 1a. StoragePool (DATA)

| # | Macha (name, file:line) | What callers rely on | Dagda through Node/Context | Verdict | Tests M / D |
|---|---|---|---|---|---|
| 1 | DATA recovery: `StoragePool` ctor M:src/storage/storage_pool.hpp:115-119, impl storage_pool.cpp:75-86; built by `LocalState::recover_data` M:src/cluster/local_state.cpp:83-131 | Opens every backend it can, an unopenable one offline, never an error. **Runs on its own thread beside the control chain** (control store, cache, retention, metadata): M:src/cluster/local_state.cpp:57-79. Stage hook names `"data-storage"`, `"control-storage"`, `"cache"`, `"retention"`, `"metadata"` (local_state.cpp:88,137,150,155,164). Lines: `node data storage ready used= capacity=[ (hosts no extents)]` (:117-119), `node data storage degraded: no disk online of N; ...` (:120-123), failure `node data storage recovery failed: ` (:127). Planes marked as each part opens. | `Node(context, options)` / `Node::open_store()` D:dagda/node.h:332-344; pool opened at D:dagda/node.cpp:214-240, then control store :256-261, cache :263-267, claims :269-277, all **sequentially on one thread** | DIFFERS: (a) the DATA pool and the control chain open one after the other (D:dagda/node.cpp:214-277), not side by side; (b) the trace hook is told `"open storage-pool"`, `"open control-store"`, `"open cache"`, `"open claims"`, ... (D:dagda/node.cpp:106-110, 214, 256, 263, 269), not Macha's stage names; (c) a failing stage refuses the whole node at once (D:dagda/node.cpp:90-102) so the other plane is never left recovering. The three log lines are word for word Macha's (D:dagda/node.cpp:96, 235-238). Duplicate backend path throws `std::invalid_argument` (D:dagda/store/storage_pool.cpp:424) where Macha throws `std::runtime_error` (M:storage_pool.cpp:304). | M: tests/test_service_root.cpp:191-208 (`test_a_failed_startup_is_what_every_caller_is_told`, `test_a_failed_startup_with_no_reason_still_says_it_failed`: one plane fails or is held while the other recovers), tests/test_invariants.cpp:1259 (`test_service_answers_health_and_status_through_startup`), :1388 (`test_a_recovering_node_answers_peers_and_is_shown_recovering`), tests/test_service_root.cpp:212 (`test_a_node_whose_only_disk_is_offline_starts_and_says_so`). D: Node.NoBackendOnlineIsReadyButDegraded (D:dagda/test/node_test.cpp:341), Node.OpensInTwoPartsWithTheDannanNodeAlreadyRunning (:371). Nothing in D pins side-by-side recovery. |
| 2 | Root locks: Macha locks only the state path (`StorageLock` in M:src/service/service.hpp:73; class M:src/storage/local_store.cpp:438-457) | A configured backend path that does not exist is "configured path is not a directory" and the backend stays offline (M:storage_pool.cpp:175-179); nothing is created there. | `Node::open` takes a `StorageLock` on every backend root, the control root and the cache root (D:dagda/node.cpp:179-190); `StorageLock` is Dannan's `StateLock`, which runs `create_directories(root)` (D:../dannan/dannan/identity/node_identity.cpp:24-28). Same in `Node::reconfigure` (D:dagda/node.cpp:600-609). | DIFFERS: a missing backend path is **created** by the lock before the pool sees it; a path whose parent cannot be written makes `create_directories` throw and the whole node is refused (`NodeRefused`, D:dagda/node.cpp:180-184) where Macha starts degraded. See row 3 for what the pool then does with the new empty directory. | M: tests/test_storage_v18.cpp:581 (`test_unversioned_state_is_refused`: node up with that backend offline). D: Node.NoBackendOnlineIsReadyButDegraded reloads to a path that does not exist (D:dagda/test/node_test.cpp:360-363) and expects it to come **online**, which pins the new behaviour, not Macha's. |
| 3 | Backend adoption and identity: `activate` M:storage_pool.cpp:149-293 | Marker `macha-backend-v18 <node> <token>` in `.macha.backend`, mirrored at `<state>/backend-identities/<sha256(path)>.id`; another node's disk, a wrong disk in a known place, an absent marker for a known backend, a non-empty unmarked directory: offline with the reason. Empty directory adopted. Per backend `LocalStore` with its ledger at `<state>/ledger/data-<token>`, cache share `ledger_cache / configured count`. Log `storage backend online <path> elapsed_ms= accounting=ready|reconciling` and event `backend_online`. | `StoragePool::activate` D:dagda/store/storage_pool.cpp:238-415 | DIFFERS (by design, Tom 2026-11-04): marker is `dagda-backend-v19 <node> <token> <domain> <check>` (D:dagda/store/storage_pool.h:74-88); a v18 marker refused naming dagda-migrate (D:storage_pool.cpp:64, node.cpp:162-164); another domain refused (:291-293); key checked by HMAC (:297-301) or by probing up to 8 records on a blind-adopted disk (:364-384). The emptiness test ignores `.macha.lock` (:314-316), so a directory row 2's lock just created is "empty" and **is adopted** if the state has no identity for that path. Ledger sealed under the node-local key, not the storage key (:352). Log line and event identical (:403-409). | M: tests/test_storage_v18.cpp:581. D: StoreStoragePool.TheBackendMarkerIsLayoutV19sAndMachasIsRefused (D:dagda/test/store_storage_pool_test.cpp:177), ABackendThatIsNotThisNodesDiskIsRefused (:224), ANonEmptyUnversionedBackendIsRefused (:264), StoreLayoutV19.* (D:dagda/test/store_layout_v19_test.cpp:98-400). |
| 4 | Backend offline: `deactivate` M:storage_pool.cpp:116-147 | Any throw from a backend's store on write takes it offline; store torn down outside locks; `departed_removals += held_removals()+1`; log `storage backend offline <path>: <reason>` once per new reason; event `backend_offline` when it was online. | D:dagda/store/storage_pool.cpp:212-236 | SAME | M: tests/test_presence.cpp:279. D: StoreStoragePool.ABackendOfflineAndReadoptedLeavesPresenceExact (store_storage_pool_test.cpp:721) |
| 5 | Live reload: `reconfigure` + `refresh` via `LocalState::reconfigure` M:local_state.cpp:189-193; pool M:storage_pool.cpp:295-362 | New backend added, changed limit/reserve reopens it (generation bump, store retired), removed one offline with `removed from configuration`; then every configured backend activated. Not serialised with maintenance. | `Node::reconfigure(NodeReload)` D:dagda/node.cpp:590-621 (pool D:storage_pool.cpp:417-481) | DIFFERS: (a) holds `_maintenance_mutex` (D:node.cpp:591), so a reload waits for a maintenance pass in progress; (b) refuses (`NodeRefused`) a v18 backend or a root held by another process before changing anything (:594-608); (c) locks, and so creates, each new root (row 2); (d) a duplicate path throws `std::invalid_argument` from the pool after the new locks were merged into `_reloadable_locks` (:609-610), which keeps them until the next reload; (e) logs `reloaded: N backends online, cache on|off` and notifies `topology` (:619-620). Pool logic itself is Macha's line for line. | M: tests/test_node_services.cpp:153 (`test_a_reload_reaches_every_service_with_live_limits`). D: StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance (reconfigure part, store_storage_pool_test.cpp:408-436), Node.NoBackendOnlineIsReadyButDegraded (node_test.cpp:360-364) |
| 6 | Periodic refresh: `StorageServer::refresh` M:src/cluster/storage_server.cpp:48-78 | Every heartbeat: `refresh()`, advertise used/limit, **advertise backends online** (`advertise_storage_backends`, :56-57, carried in telemetry as `storage_backends_online`, M:src/cluster/cluster.cpp:159, shown by Status M:src/api/status_api.cpp:316-317), storage event when the online count rises, trace `DIAG node-stage stage=storage-refresh` past 100 ms. | `StorageServer::refresh` D:dagda/store/storage_server.cpp:78-113 | DIFFERS: same refresh, event and trace; advertises capacity/used/hosts-extents (:94-102) but **not the backends-online count**. Dagda publishes `dagda.storage.backends_online` to Dannan's Status surface every `status_interval` (10 s default, D:dagda/node.h:247; node.cpp:1080). Macha must read `node.data().online_backends()` itself to keep its telemetry field. | M: tests/test_invariants.cpp:1586 (`test_status_renders_each_node_from_what_it_knows`). D: StoreRepair.ADiskComingBackIsSurveyedWithNothingReferencedChanging (store_repair_test.cpp:2137) |
| 7 | `service_monitor()` / `configure_service_monitor` M:storage_pool.hpp:126-134; enabled at M:local_state.cpp:96-109; observed at M:src/cluster/local_services.cpp:25-27; fed by torrent I/O M:src/torrent/torrent_manager.cpp:1325-1328 | Per-pool monitor fed by every put and get and by rebalance/scrub reads; zero slowdown_percent disables the gate; log `data io pressure gate enabled expected_ms=...`; arbiter observes it. | `node.data().service_monitor()` D:dagda/store/storage_pool.h:180-182; thresholds set from `AdmissionOptions::device_pressure` before open (D:node.cpp:221); arbiter wired at D:node.cpp:252; log D:node.cpp:226-232; `note(elapsed, bytes)` D:dagda/store/io_pressure.h:97 | SAME (timer reads the injected steady time, D:storage_pool.cpp:527; the node's in production) | M: tests/test_io_pressure.cpp:145, :193. D: StoreIoPressure.AReadIsMeasuredWithTheBytesItReturned (store_io_pressure_test.cpp:158), MaintenanceReadsArePartOfTheServiceTimeTheySpend (:198) |
| 8 | `put` M:storage_pool.hpp:138, impl :398-427 | Strict: true once durable on the first ranked backend that takes it; false when none would. A put of an held object reaffirms its age (never short-circuited through `has`). A throw takes that backend offline and the next is tried. One `DiskServiceTimer` sample per device write. | `node.data().put` D:storage_pool.h:190, impl D:storage_pool.cpp:509-537 | SAME (a blind pool throws `std::logic_error`, D:storage_pool.cpp:510; Macha's node is a member). Cost differs on a refused put: see section 2, item 2. | M: tests/test_storage_v18.cpp:166. D: StoreStoragePool.ARefusedPutFallsThroughToABackendWithRoom (:81), AnObjectLandsOnItsFirstRankedBackend (:145) |
| 9 | `put_deferred` M:storage_pool.hpp:139, impl :429-474 | Generation from the backend that took it, bound to its domain and backend incarnation; no token if the backend reopened meanwhile; nothing if none would. Macha's server adds the process epoch on the wire (M:storage_server.cpp:259-263). | `node.data().put_deferred` D:storage_pool.h:193, impl :539-580 | SAME (the ticket carries the epoch itself, D:storage_pool.cpp:573) | M: tests/test_invariants.cpp:2978. D: StoreStoragePool.TicketsNameThePhysicalDomainAndTheBackendIncarnation (:541) |
| 10 | `durability_barrier(token, urgency)` M:storage_pool.hpp:140-141, impl :476-509 | Returns once the generation is durable on that incarnation; a failure warns `storage durability barrier failed <path>: <error>`, takes the backend offline and rethrows; a vanished placement throws. All `std::runtime_error`. Macha's server checks the epoch first (M:storage_server.cpp:304-316). | `node.data().durability_barrier` D:storage_pool.h:206, impl :582-612 | DIFFERS (types only): an invalid ticket throws `std::invalid_argument` (a `logic_error`, D:storage_pool.cpp:583), another epoch and a vanished or changed placement throw `StaleDurabilityTicket` (a `runtime_error`, :584, 602, 611). Warning text and offline behaviour identical. | M: tests/test_invariants.cpp:3206, :2879. D: StoreStoragePool.ATicketFromAnotherProcessOrIncarnationSatisfiesNothing (:586), StoreDurabilityBarrier.AStaleProcessEpochIsRefused (store_durability_barrier_test.cpp:135) |
| 11 | `durability_covered` M:storage_pool.hpp:142, impl :511-523 | Whether the incarnation's domain has passed the generation; no device wait. | D:storage_pool.h:207, impl :614-624 | SAME (false for another epoch, which a ticket from this pool never is) | M: tests/test_invariants.cpp:3340. D: StoreDurabilityBarrier.ACoveredGenerationCostsNoBarrierOverTheWire (:206) |
| 12 | `reassert_durable` M:storage_pool.hpp:146, impl :643-673 | For a held object: flush the holding backend's last mutation, then a token at its durable generation. | D:storage_pool.h:211, impl :780-806 | SAME | M: tests/test_storage_v18.cpp:1082. D: StoreDurabilityBarrier.ARestartedPeerIsProbedOnceWithEveryIdThatNamedIt (:362) |
| 13 | `get` M:storage_pool.hpp:147, impl :571-621; caller M:src/filesystem/filesystem.cpp:1208 | Ranked order; `has` first; one timer sample with the bytes read; trace lines `DIAG backend-get ...` and every 5 s `DIAG storage-get ...`; `ObjectCorrupt`: warn `discarding corrupt local object`, remove, next backend; any other throw: counter `storage.read_failures`, warn `cannot read local object <id> on <path>; keeping it: ...`, next backend. | `node.data().get` D:storage_pool.h:216, impl :710-762 | SAME (every v19 marker a member writes carries the key check, D:storage_pool.cpp:297-301, 323-326). Only on a blind-adopted disk, before the probe proves the key, is a copy that does not open kept and warned `... kept (key unchecked)` (:745-748). | M: tests/test_invariants.cpp:2493 (`test_a_failed_read_keeps_the_object_and_a_bad_record_is_removed`). D: StoreStoragePool.AReadErrorKeepsTheObject (:881), StoreLayoutV19.ACopyThatDoesNotOpenIsKeptUntilTheKeyIsChecked (:360) |
| 14 | `has` M:storage_pool.hpp:148, impl :623-641; callers M:src/catalogue/catalogue.cpp:1392,1796 | Any online ranked backend's `has`; a throwing backend answers no; no device read once ledgers are seeded. | `node.data().has` D:storage_pool.h:217, impl :764-778; `Context::Objects::has` D:dagda/context.h:199 | SAME | M: tests/test_baselines.cpp (`test_baseline_data_store_has`). D: StoreStoragePool.DataStoreHasBaseline (:841) |
| 15 | `held_view` M:storage_pool.hpp:151, impl :691-734 | Union of online backends' held tries sorted by disk token, each union made from the one before; removals never fall; incomplete while a backend seeds. | D:storage_pool.h:221, impl :820-857 | SAME | M: tests/test_presence.cpp:310, :371. D: StoreStoragePool.TheHeldIdentityNamesExactlyWhatThePoolHolds (:752), TheHeldIdentityIsTheSameOnAnyNumberOfDisks (:813) |
| 16 | `valid` M:storage_pool.hpp:152, impl :675-689 | Some ranked online backend reads it back and it opens. | D:storage_pool.h:222, impl :808-818 | SAME | M: tests/test_invariants.cpp:2312. D: covered by StoreStoragePool.RebalanceNeverDeletesTheLastValidCopyForACorruptPreferredOne (:481) |
| 17 | `remove` M:storage_pool.hpp:153, impl :736-756 | From every online backend; errors logged at debug and swallowed. | D:storage_pool.h:224, impl :859-877 | SAME | M: tests/test_storage_v18.cpp:112. D: StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance (:288) |
| 18 | `list`, `full_list_scans` M:storage_pool.hpp:154-157, impl :758-780 | Union of every online backend's list; counts full scans. | D:storage_pool.h:225-226, impl :879-899 | SAME | D: StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance |
| 19 | `next_object(Cursor&, pass_complete)` M:storage_pool.hpp:158, impl :782-838 | Persistent traversal, packed first then loose per backend, reset when the backend set changes. | D:storage_pool.h:227, impl :901-951 | SAME | D: StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance |
| 20 | `older_than` M:storage_pool.hpp:159, impl :840-858 | Some online backend holds it and its age exceeds the given age. | D:storage_pool.h:228, impl :953-967 | SAME (age on the injected wall clock, `NodeOptions::wall`, D:node.h:250) | D: StoreLocalStore.AgesFollowTheWallClockAndATouch (:820) |
| 21 | `rebalance_step` M:storage_pool.hpp:161-162, impl :860-990; stage M:src/service/maintenance.cpp:1100-1123 | Nothing read with fewer than two backends online; at most `operation_budget` objects; a duplicate goes only once the preferred backend holds a valid copy; reads and writes timed. Stage: 64 objects, credit in bytes, yields to viewer/loader, `enter_stage("local-rebalance")`, trace lines `maintenance: local rebalance yielded...` / `...quiescent; waiting for an event`, slow-stage log. | Pool D:storage_pool.h:233, impl :969-1083; stage `Node::local_rebalance` D:node.h:453, D:node.cpp:682-706 | Pool: SAME. Stage: DIFFERS: no `enter_stage("local-rebalance")` (D:node.cpp:682-706; Macha M:maintenance.cpp:1102); budget 64, credit, trace lines and slow-stage log are Macha's. | M: tests/test_invariants.cpp:2359, :2381. D: StoreStoragePool.RebalanceReadsNothingWithOneBackendOnline (:464), RebalanceNeverDeletesTheLastValidCopyForACorruptPreferredOne (:481) |
| 22 | `scrub_step` M:storage_pool.hpp:163-164, impl :992-1035; stage M:maintenance.cpp:1162-1197 | Reads each object timed; `ObjectCorrupt` removed with warn `removing corrupt local object`; other failures kept and counted. Stage: due date at `<state>/maintenance/scrub.next`, `enter_stage("scrub")`. | Pool D:storage_pool.h:236, impl :1085-1127; stage `Node::local_scrub` D:node.cpp:799-824 | Pool: SAME for a key-checked backend (as row 13). Stage: DIFFERS: no `enter_stage("scrub")`; due-date file and trace lines are Macha's (D:node.cpp:809-815). | M: tests/test_invariants.cpp:2493. D: StoreStoragePool.AReadErrorKeepsTheObject (:881) |
| 23 | `gc_step` M:storage_pool.hpp:172-179, impl :1037-1105; caller M:maintenance.cpp:1031 | Skips live, protected, retained; sightings mature before deletion; final claim check is `claim_deletion(id)` from the claims store, which marks the object while it is deleted; orphan grace by age; `deferred` is **this slice's**. | Pool D:storage_pool.h:244-247, impl :1129-1184; driven by the node's collector (D:dagda/ledger/collector.cpp:195, 249, 295) | DIFFERS: (a) no `is_retained` + `claim_deletion` pair: one `ClaimCheck` and the pool's own `ClaimLocks::deletion` mark (D:storage_pool.cpp:1167; claims take the same guard through `StoragePool::claim_locks()`, D:storage_pool.h:279); (b) the slice that completes a pass reports `deferred` for the **whole pass** (D:storage_pool.cpp:1143-1149), deliberately. Mapped in detail by the GC section. | D: StoreStoragePool.AClaimedCopyIsUntouchable (:624), TheSliceThatCompletesASweepReportsEveryDeferralInItsPass (:679) |
| 24 | `rebalance_once` M:storage_pool.hpp:183, impl :1107-1115 | Complete pass for tests and explicit callers. | D:storage_pool.h:250, impl :1186-1193 | SAME | D: StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance |
| 25 | `compact_packs(stop)` M:storage_pool.hpp:187, impl :1138-1164; stage M:maintenance.cpp:1157-1160 | One victim pack per online backend per call; a failure warns `DATA pack compaction deferred path= error=` and keeps the backend online. | Pool D:storage_pool.h:253, impl :1214-1237; stage `Node::compact_packs` D:node.cpp:788-797 | SAME (the per-store compaction differs, row 46) | M: tests/test_storage_v18.cpp:112. D: StoreLocalStore.PacksStoreCompactAndSurviveRestart (:133) |
| 26 | `verify_step(directories)` M:storage_pool.hpp:191, impl :1117-1136; stage M:maintenance.cpp:1203-1226 | Summed over online backends; complete when all are. Stage: 4 dirs/s, 0.25 busy, cap 256, a day's rest; observations `ledger.verify.directories/recorded/lost`; info `storage held ledgers verified against the disk`; `enter_stage("ledger-verify")`. | Pool D:storage_pool.h:256, impl :1195-1212; stage `Node::ledger_verify` D:node.cpp:826-859, credit D:node.h:198 | Pool: SAME. Stage: DIFFERS: no `enter_stage("ledger-verify")`; rate, rest, observations and line are Macha's. | M: tests/test_object_ledger.cpp:1117, :1260. D: StoreLocalStoreLedger.VerificationCorrectsTheLedgerFromTheDisk (:101), VerificationResumesAfterARestart (:205) |
| 27 | `used` M:storage_pool.hpp:193, impl :1166-1179; caller M:filesystem.cpp:2421 | Sum of online backends' accounted bytes. | `node.data().used()` D:storage_pool.h:258, impl :1239-1250 | SAME | M: tests/test_storage_v18.cpp:166. D: StoreStoragePool.OneCopysLogicalCapacityIsTheSumOfTheNodes (:104) |
| 28 | `limit` M:storage_pool.hpp:194, impl :1181-1198; caller M:filesystem.cpp:2420 | Sum of configured limits of backends whose identity is known, saturating. | D:storage_pool.h:260, impl :1252-1268 | SAME | same as row 27 |
| 29 | `online_backends` M:storage_pool.hpp:195, impl :1200-1202 | Count cached at the last `refresh`, not moved by a backend going offline between refreshes. | D:storage_pool.h:261 (stored at D:storage_pool.cpp:480 only) | SAME | D: StoreStoragePool.PoolsSpreadTraverseSweepAndRebalance (:408-436) |
| 30 | `every_backend_online` M:storage_pool.hpp:198, impl :1204-1211 | Only then is a miss final (404 vs "cannot say"). | D:storage_pool.h:264, impl :1270-1276 | SAME | M: tests/test_storage_v18.cpp:1022 |
| 31 | Degraded state: Macha `data_storage_degraded` M:src/cluster/cluster.cpp:179 and the warn at M:local_state.cpp:120-123 | Configured backends and none online: ready but degraded, says so. | `StoragePool::degraded()` D:storage_pool.h:268, impl :1278-1287; Status `dagda.storage.degraded` D:node.cpp:1081; warn D:node.cpp:237-238 | SAME | M: tests/test_service_root.cpp:212. D: Node.NoBackendOnlineIsReadyButDegraded (node_test.cpp:341) |
| 32 | `diagnostics` M:storage_pool.hpp:199, impl :1213-1233; caller M:src/api/status_api.cpp:1100-1112 | Six counters summed over online backends. | `node.data().diagnostics()` D:storage_pool.h:269, impl :1289-1308; Status `dagda.data_store.*` D:node.cpp:1014-1017, 1151-1156 | SAME (adds `own_syncs`) | D: StorePersistentCache.TheCacheMakesNoCallForStableStorage (store_persistent_cache_test.cpp:93) reads `own_syncs` |
| 33 | Durability domain per filesystem: `domain_for` M:storage_pool.cpp:93-108 | Backends on one `st_dev` share a domain (one syncfs serves them); a failed domain is replaced. | D:storage_pool.cpp:169-182 | SAME | M: tests/test_invariants.cpp:2541. D: StoreDurabilityDomain.ABarrierIsTimedPerFilesystem (store_durability_domain_test.cpp:67) |
| 34 | Pool lock diagnostics: `TimedLock(mutex_, "storage.pool")` M:storage_pool.cpp:89, 299; M:src/diagnostics.hpp:59-74 | A wait or hold on the pool lock above 10 ms is reported at log level ALL. | plain `Lock` D:storage_pool.cpp:165, 420; TODO at D:storage_pool.h:323-325 | MISSING (diagnostic only) | none either side |
| 35 | Pool device-work seam: `StoragePool(..., LocalStoreFiles& files)` M:storage_pool.hpp:119, passed to every backend store M:storage_pool.cpp:251-252 | Inject the device under every backend. No Macha test uses it at pool level (only `LocalStore` tests inject, M:tests/test_storage_v18.cpp:337-487, tests/test_presence.cpp:173-215). | none: the pool's stores take the default `posix_local_store_files()` (D:storage_pool.cpp:353-356) | MISSING (test seam only) | none |

### 1b. LocalStore (CONTROL, and each DATA backend's store)

| # | Macha (name, file:line) | What callers rely on | Dagda through Node/Context | Verdict | Tests M / D |
|---|---|---|---|---|---|
| 36 | Control store open: M:local_state.cpp:137-148 (`LocalStoreOptions{limit, 0, threshold, target}`, ledger `<state>/ledger/control`, cache share 1/16, own domain) | `<state>/metadata-objects` by default (M:src/config_base.cpp:630-631), 4 GiB, packs 1 MiB / 64 MiB, **reserve_free 0**, authoritative, its own 500 ms durability domain over its root. | `Node::control()`; opened by `open_control_store` D:dagda/store/control_store.cpp:18-31 from D:node.cpp:256-261 | DIFFERS: `ControlStoreOptions::reserve_free` defaults to **256 MiB** (D:control_store.h:30-35, Tom 2026-11-04, requirements section 7), so a CONTROL put is refused with less than 256 MiB free where Macha's succeeds; the field is settable to 0. Ledger sealed under the node-local key (D:control_store.cpp:28). Root locked (row 2). | M: tests/test_storage_v18.cpp:546. D: StoreControlStore.AControlWriteIsNotBlockedByDataExhaustion (store_control_store_test.cpp:35), TheControlStoreTakesMachasPlaceAndDefaults (:70), AControlReserveStopsAWriteBeforeTheDeviceIsFull (:96) |
| 37 | `put` M:local_store.hpp:313, impl local_store.cpp:1224-1358, 1520-1524; callers M:catalogue.cpp:1862-1865, M:namespace_control_store.cpp:20,32 | Hash checked (`object hash mismatch`); waits for the accounting scan before the object lock; a held packed object is read and compared, then a touch record; a held loose object is reaffirmed by stamp or full read and its mtime set; a corrupt copy replaced; strict: returns once its generation is durable (immediate urgency); false on capacity or reserve; ledger flushed before return. | `node.control().put` D:local_store.h:234, impl D:local_store.cpp:891-1005, 1151-1155 | SAME (sealed after the checks with the index free, D:local_store.cpp:990-995; a blind store throws). Cost differs on a refused put: section 2, item 2. | M: tests/test_storage_v18.cpp:112, :166. D: StoreLocalStore.PacksStoreCompactAndSurviveRestart (:133), ALooseObjectIsSealedAccountedAndReaffirmed (:697), APutRefusesBytesThatAreNotTheId (:788), AdmissionRespectsTheLimit (:196) |
| 38 | `put_deferred` M:local_store.hpp:314, impl :1526-1534 | As `put`, returning the generation to await (0: durable). | D:local_store.h:237, impl :1157-1163 | SAME | M: tests/test_invariants.cpp:2556. D: StoreLocalStoreDurability.DeferredGenerationsBatchStableStorageBarriers (:74) |
| 39 | `durability_barrier(gen, urgency)`, `durability_barrier()` M:local_store.hpp:315-317, impl :1536-1552 | No-op for an ephemeral store or generation 0; the no-argument form waits for the last mutation, immediately. | D:local_store.h:252-254, impl :1165-1178 | SAME | M: tests/test_invariants.cpp:2879. D: StoreLocalStoreDurability.ABarrierRemembersTheWholeCut (:311) |
| 40 | `durable_generation`, `durability_domain_id` M:local_store.hpp:318-319, impl :1554-1560 | Domain's durable generation; domain id (0 without). | D:local_store.h:255-256, impl :1180-1182 | SAME | D: StoreLocalStoreDurability.* |
| 41 | `get` M:local_store.hpp:320, impl :1360-1402; callers M:catalogue.cpp (ten sites), M:namespace_control_store.cpp:39,43 | Under the object lock; packed read with a reader lease; a zero-byte loose file pruned; a record read in full that does not open throws `ObjectCorrupt`; a read that did not finish throws plainly; an unlisted file found by a read is recorded in the ledger. | `node.control().get` D:local_store.h:262, impl :1007-1034 | SAME (`DecodeError` and `ObjectIntegrityError` are `runtime_error`s, so a torn or corrupt file is `ObjectCorrupt` as in Macha; a short pack read is plain) | M: tests/test_storage_v18.cpp:620 (`test_a_store_tells_a_bad_record_from_a_failed_read`). D: no direct port (see gap 7); StoreStoragePool.AReadErrorKeepsTheObject covers the pool's half |
| 42 | `has` M:local_store.hpp:326, impl :1414-1452; callers M:catalogue.cpp:1393, M:namespace_control_store.cpp:30, M:src/ledger/retention_ledger.cpp:33 | Once seeded: packs or ledger, index mutex only, no device; before warm-up: device under the object lock; a put in progress reads absent. | `node.control().has` D:local_store.h:268, impl :1049-1082 | SAME | M: tests/test_presence.cpp:171, :211, :237, :257. D: StoreLocalStorePresence.HasAnswersAPutInProgressWithoutWaitingForIt (:47), APutThatFailsMidwayIsNeverPresent (:88), HasDoesNotConsultTheDeviceAfterWarmUp (:116), PresenceIsExactAcrossARestart (:134) |
| 43 | `valid` M:local_store.hpp:343, impl :1454-1460; caller M:src/cluster/control_objects.cpp:19 | `get` succeeds. | D:local_store.h:272, impl :1084-1094 | SAME (member) | D: StoreControlFetch.* (store_control_fetch_test.cpp:65-196) |
| 44 | `remove`, `remove_if_older_than` M:local_store.hpp:344-345, impl :1562-1596; caller M:catalogue.cpp:2724 | Packed: tombstone record; loose: unlink; neither waits for durability; ledger flushed; age by touch record or mtime. | D:local_store.h:285-287, impl :1184-1217 | SAME (age on the injected wall clock; real clock in production) | M: tests/test_invariants.cpp:2844. D: StoreLocalStoreDurability.AnAuthoritativeDeleteIsLazilyDurable (:288) |
| 45 | `list`, `next_object(Cursor&)` M:local_store.hpp:346-347, impl :1598-1657; caller M:catalogue.cpp:2680 | Packed ids in order, then loose ids as the walk finds them. | D:local_store.h:288-291, impl :1219-1272 | SAME | D: StoreLocalStore.ListsEveryObjectOnce (:851) |
| 46 | `compact_packs(stop)` M:local_store.hpp:368, impl :1738-1938; caller M:catalogue.cpp:2735 | Waits for the exact accounting (stoppable); one victim with most dead bytes; copy-on-write, fsync, rename, dir sync; index switched before unlink; waits for leased readers; false if no room, stopped, or unlink failed. | D:local_store.h:322, impl :1351-1525 | DIFFERS (fix): also waits, under the index mutex, until no appended record is still unindexed (D:local_store.cpp:1357; counted at :443-451, 486-487) so a record in its append gap cannot be counted dead; Macha has that race. Its fsyncs are counted in `own_syncs` (:1446, 294). | M: tests/test_storage_v18.cpp:112, :324. D: StoreLocalStore.ACompactionWaitsForAPutInItsAppendGap (:393), ACompactionWaitsForARemoveInItsAppendGap (:440), ACompactionWhoseOldPackSurvivedIsReclaimedLater (:311) |
| 47 | `verify_step` M:local_store.hpp:355-361, impl :1998-2069; caller M:maintenance.cpp:1216 | 65,536 directories per pass, resumable; unlisted file recorded, listed id with no file (not packed) recorded lost; warn `storage held ledger corrected path= recorded= lost=`. | `node.control().verify_step`, run by `Node::ledger_verify` D:local_store.h:300-306, impl :1549-1611 | SAME | M: tests/test_object_ledger.cpp:1117, :1173, :1260. D: StoreLocalStoreLedger.* (:101, :157, :205) |
| 48 | `older_than`, `touch`, `last_write` M:local_store.hpp:362, 365-366, impl :1678-1730; caller M:catalogue.cpp:2699 | Age by pack touch record or loose mtime; touch appends a touch record or sets mtime. | D:local_store.h:309, 315-318, impl :1291-1339 | SAME (wall clock injected, D:local_store.cpp:1347-1349) | D: StoreLocalStore.AgesFollowTheWallClockAndATouch (:820) |
| 49 | `object_path`, `stored_size`, `is_packed` M:local_store.hpp:363-367, impl :1659-1676, 1732-1736 | Used by the pool's rebalance and sweep and by tests. | D:local_store.h:312-319, impl :1274-1289, 1341-1345 | SAME | D: StoreLocalStore.PacksStoreCompactAndSurviveRestart |
| 50 | `held_view`, `held_removals`, `losses`, `presence_authoritative`, `scan_complete`, `used`, `limit` M:local_store.hpp:328-342, 370-378, impl :569-580 | Atomic reads or index-mutex reads; view complete once seeded. | D:local_store.h:275-281, 324-326, impl :237-245, 1044-1047 | SAME | M: tests/test_object_ledger.cpp:1064. D: StoreLocalStoreLedger.AStoreWithItsLedgerStartsKnowingWhatItHolds (:52) |
| 51 | `diagnostics` M:local_store.hpp:379-386 | Six counters. | D:local_store.h:327, impl :1764-1774 | SAME (adds `own_syncs`) | as row 32 |
| 52 | Open-time recovery: ctor M:local_store.cpp:459-537, `rebuild_pack_index` :956-1093, `restore_accounting` :668-691, `scan` :2129-2241, `warm_presence_index` :2071-2127, `seed_ledger` :587-611 | Packs replayed synchronously (torn tail truncated, damaged span skipped and counted); clean A/B checkpoint trusted; dirty one an estimate while a supervised scan reconciles and establishes a durable baseline; presence walk by readdir; ledger seeded once; lines `storage accounting restored|estimate`, `truncated undecodable pack tail`, `skipped unreadable pack region`, `storage held ledger seeded`, `storage presence index warmed`; startup progress ticked per entry. | D:local_store.cpp:87-173, 613-733, 297-316, 1665-1762, 1613-1663, 251-271 | SAME (threads through Dannan's `SupervisedThreads::once`, progress through `StartupProgress::note`, log lines word for word) | M: tests/test_storage_v18.cpp:220, tests/test_invariants.cpp:2680, :2750, :2809. D: StoreLocalStore.PackRecoverySettlesEachKindOfDamage (:215), StoreLocalStoreDurability.ACleanAccountingCheckpointIsTrustedWithPacks (:143), UncleanAccountingRecoveryEstablishesADurableBaseline (:215), ARestartedReaffirmationNeedsNoNewBarrier (:260) |
| 53 | Close: dtor M:local_store.cpp:619-651 | Joins the walks, awaits the last mutation, writes a clean checkpoint. | D:local_store.cpp:207-235 | SAME | M: tests/test_invariants.cpp:2642. D: StoreLocalStoreDurability.TheAccountingDirtyMarkIsOncePerSession (:116) |
| 54 | On-disk formats: loose `DHTOBJ01` header, pack `MACHPK01` records with SHA-256 header checksum, `.macha.accounting` A/B slots, names `objects/xx/yy/<hex>.obj`, `packs/pack-<20 digits>.pack` M:local_store.cpp:42-55, 107-207, 270-288, 653-656 | Byte layout. | D:dagda/store/local_format.h:29-124 | DIFFERS (by design, Tom 2026-11-04): every byte as Macha's but the AES-GCM associated data, which is `"dagda/seal/19" || domain || id` (D:dagda/store/sealing.h:29-46, sealing.cpp:14-20) instead of the id; v18 disks are migrated by dagda-migrate | D: StoreLocalFormat.* (store_local_format_test.cpp:117-234), StoreSealing.* (store_sealing_test.cpp:74-280) |

### 1c. Under them: durability domain, presence index, sealed journal, device work

| # | Macha (name, file:line) | What callers rely on | Dagda through Node/Context | Verdict | Tests M / D |
|---|---|---|---|---|---|
| 55 | `DurabilityDomain` M:src/storage/durability_domain.hpp:33-90, impl .cpp:63-293 | Generations from 1, issued only after the syscall; one worker; batchable waits a 500 ms window, immediate cuts it; one `syncfs` per cut on Linux (per-file and per-directory fsync elsewhere); a failed barrier is sticky; observation `durability.barrier_us.<dir name>`; `physical_barriers`. Thread named `macha-durable` (:220). | Internal to the pool and stores: D:dagda/store/durability_domain.h:50-135, impl .cpp:45-249; `StoragePool::physical_barriers()` D:storage_pool.h:272 (Status `dagda.storage.barriers`, D:node.cpp:1082) | SAME (window measured on the injected steady time, D:durability_domain.cpp:199-207; thread naming not checked, see section 4) | M: tests/test_invariants.cpp:2541, :2931. D: StoreDurabilityDomain.* (store_durability_domain_test.cpp:67-215), StoreLocalStoreDurability.ADomainGroupCommitsIndependentPublications (:355) |
| 56 | `PresenceIndex` M:src/storage/presence_index.hpp, .cpp:1-43 | installed/observed/forgotten/pruned/listed/warmed/answer as a model. | D:dagda/store/presence_index.h, .cpp | SAME (bodies identical after renaming) | M: tests/test_presence.cpp:95, :126. D: StorePresenceIndex.MatchesItsModelOverEveryShortHistory (:86), WarmupRaceCasesByName (:115) |
| 57 | `SealedJournal` M:src/storage/sealed_journal.hpp, .cpp:1-251 | Sealed frames appended in order; torn tail truncated on replay; read by offset; reset; fsync on `sync`. | D:dagda/ledger/sealed_journal.h, .cpp:1-337 | SAME (key held as `shared_ptr<const StorageKey>`; replay also closes its fd on a throw; adds probe/rekey/read-only scans for the migration) | M: tests/test_object_ledger.cpp:52, :73, :103, :142. D: LedgerSealedJournal.* (ledger_sealed_journal_test.cpp:57-197) |
| 58 | `LocalStoreFiles` / `PosixLocalStoreFiles` M:local_store.hpp:68-110, impl local_store.cpp:298-436 | install (temp, `start_writeback`, close, rename; no fsync), read, read_at, append, truncate, list. | D:dagda/store/local_store_files.h, .cpp:40-158 | SAME (adds `O_CLOEXEC`; `start_writeback` identical, D:dagda/store/write_behind.cpp vs M:src/write_behind.cpp) | M: tests/test_storage_v18.cpp:324. D: StoreLocalStore.HeldDeviceIoBlocksOnlyItsOwnObject (:478), StoreWriteBehind.* |

### Counts

58 rows: **SAME 43, DIFFERS 13, MISSING 2**.

- SAME: 4, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 24, 25, 27, 28, 29, 30, 31, 32, 33, 37, 38, 39, 40, 41, 42, 43, 44, 45, 47, 48, 49, 50, 51, 52, 53, 55, 56, 57, 58.
- DIFFERS: 1, 2, 3, 5, 6, 10, 21, 22, 23, 26, 36, 46, 54.
- MISSING: 34, 35.

Rows 21, 22 and 26 are DIFFERS only at the node's stage hook; the pool operations
themselves are SAME. Row 13 is SAME for every disk a member adopted; it differs only on
a disk a blind node adopted, before the member's key probe has passed.

## 2. Performance-relevant differences

1. **Startup is serial where Macha's is parallel.** Macha opens the DATA pool on its own
   thread while the control store, cache, claims, reference counts and metadata open on
   the caller's (M:src/cluster/local_state.cpp:57-79). Dagda opens the pool
   (D:dagda/node.cpp:214-222), then the control store (:256-261), the cache (:263-267),
   the claims and reference counts (:269-277) on one thread. Every `LocalStore` replays
   its packs synchronously in its constructor (D:dagda/store/local_store.cpp:118,
   613-733: one 125-byte `pread` per record) and opens its held ledger
   (:132-136), so time to ready becomes the sum of the DATA pool's open and the rest,
   not the larger of the two. On a multi-terabyte pool with ~1 MiB packed objects this is
   the pack-replay time of every backend added to the control side's.
2. **A put refused for capacity or reserve still seals.** Macha checks
   `physical_space_available_locked` and the filesystem reserve before it encrypts
   (loose: M:local_store.cpp:1099-1113 then seal at :1128; packed: :1171-1183 then seal
   inside `append_pack_record_locked` at :859). Dagda seals first
   (D:local_store.cpp:990-995: AES-GCM over the whole payload and a ciphertext buffer
   of its size) and only then asks `put_loose_locked`/`put_packed_locked` for room
   (:739-750, :801-812). `StoragePool::put` and `put_deferred` fall through the ranked
   backends (D:storage_pool.cpp:511-535, 542-578), so with k full backends ahead of the
   one with room Dagda does k+1 encryptions of the object where Macha does one; a pool
   with every backend full does k where Macha does none. Rebalance's
   `preferred_store->put` pays the same (D:storage_pool.cpp:1053).
3. **Associated data is built per seal and per open.** Macha passes the 32-byte id as a
   span (M:local_store.cpp:859, 942, 1128, 1297, 1382). Dagda builds a 61-byte
   `Bytes` (`"dagda/seal/19"`, 16-byte domain, 32-byte id) with one heap allocation for
   every seal and open (D:dagda/store/sealing.cpp:14-20, 23, 28): two more GHASH blocks
   and a malloc per object operation. Immaterial for extents; the largest relative cost
   is on small packed CONTROL objects.
4. **`complete_mutation` copies its paths.** Dagda takes `file` and `directory` by value
   (D:dagda/store/durability_domain.h:84) and discards them on Linux (.cpp:76-81); Macha
   takes them by `const&` (M:durability_domain.hpp:78-79). Each put, touch, tombstone and
   loose unlink therefore copies one or two paths, and for puts, touches and tombstones
   that happens with the store's index mutex held (D:local_store.cpp:783, 839, 919,
   1115, 1316). Two small allocations; negligible.
5. **Every pack append signals a condition variable.** The unindexed-append count
   (D:local_store.cpp:443-451, 486-487) costs an atomic increment, an atomic decrement and
   a `notify_all` on `_appends_indexed_cv` per pack record (put, touch, tombstone), with
   or without a waiter. Compaction may wait for in-flight appends to be indexed
   (:1357). Both are small; the wait closes a race Macha has.
6. **Accounting re-check under the object lock.** `put_impl`, `put_sealed`, `remove` and
   `remove_if_older_than` call `wait_for_accounting` again with the object lock held
   (D:local_store.cpp:858, 897, 1190, 1202) after `await_accounting`. In steady state this
   is two atomic loads under the index mutex; it can only wait in the window between the
   scan clearing the estimate and setting `_scan_complete` (:1729-1757), in which the scan
   takes no object lock.
7. **Reload waits for maintenance.** `Node::reconfigure` takes `_maintenance_mutex`
   (D:node.cpp:591), held across a whole maintenance pass's device work and the
   provider (D:node.h:589-591); Macha's `LocalState::reconfigure` takes only the pool's
   locks (M:local_state.cpp:189-193). A reload issued during a long GC or repair pass is
   delayed until the pass ends.
8. **One more file and descriptor per root.** Each backend, the control store and the
   cache hold a `.macha.lock` file with an open fd for the node's life
   (D:node.cpp:179-190). No effect on the object paths.
9. **Unchanged** (verified line by line): syncs and batching (no fsync on a loose install,
   `start_writeback` instead, D:local_store_files.cpp:52-54 vs M:local_store.cpp:310-312;
   one durable dirty marker per session, D:local_store.cpp:318-335 vs
   M:local_store.cpp:693-714; compaction's one fsync and two directory syncs,
   D:local_store.cpp:1445, 1454, 1503 vs M:local_store.cpp:1845, 1858, 1912; one
   `syncfs` per domain cut with a 500 ms window, D:storage_pool.h:55, node.h:234 vs
   M:src/service/main.cpp:53); locks across I/O (index mutex released across every
   device call, pack stream lock across appends, per-object lock across that object's I/O,
   D:local_store.h:171-173 vs M:local_store.hpp:173-184); copies on the put path (the
   pack payload is a span into the sealed ciphertext, D:local_store.cpp:454-459; the
   loose install writes the ciphertext in place, :770); pack compaction (one victim per
   store per call, same selection and space check, D:local_store.cpp:1393-1405 vs
   M:local_store.cpp:1780-1797); file counts and layout (D:local_format.h:29-110);
   presence (readdir only, no stat, D:local_store.cpp:1631-1641 vs
   M:local_store.cpp:2089-2102); the held ledger's flush per put/remove
   (D:local_store.cpp:1153, 1161, 1193, 1215 vs M:local_store.cpp:1522, 1530, 1570, 1594).

## 3. Gaps for Dagda to fix

1. **The DATA pool and the control chain do not recover side by side.** Macha needs the
   pool's open to run beside the control store, cache, claims and metadata, so that one
   plane can fail or stall while the other keeps recovering and is reported ready
   (M:src/cluster/local_state.cpp:57-79). M:tests/test_service_root.cpp:191-208 fails one
   plane's stage while holding the other's and expects, for example,
   `control_storage=ready, cache=recovering, data_storage=recovering` (:205-206);
   M:tests/test_invariants.cpp:1272 and :1395 hold `"data-storage"` or
   `"control-storage"` by name. Dagda
   opens them in sequence and refuses the node at the first failure
   (D:dagda/node.cpp:90-102, 214-277), and its trace hook names stages
   `"open storage-pool"` etc. rather than Macha's. Test that fails today:
   `NodeOpen.TheDataPoolAndTheControlChainRecoverSideBySide` (hold the trace at the
   pool's stage; expect the `control_storage` and `cache` planes ready meanwhile).
   Severity: **blocks the port** (Macha's gates above), and **loses performance**
   (startup time is a sum, section 2 item 1).
2. **A missing backend path is created and can be adopted.** Macha leaves a backend whose
   path does not exist offline (M:storage_pool.cpp:175-179) and starts degraded. Dagda's
   node locks every backend root first, and Dannan's `StateLock` creates the directory
   (D:node.cpp:187, 600-609; D:../dannan/dannan/identity/node_identity.cpp:24-25); the
   pool then finds a directory holding only `.macha.lock`, treats it as empty
   (D:storage_pool.cpp:314-316) and, for a path the state has no identity for, writes a
   marker and puts DATA on whatever filesystem holds the mount point. Where the parent is
   not writable the node is refused instead of starting degraded (D:node.cpp:180-184).
   D:dagda/test/node_test.cpp:360-363 currently relies on the creation. Tests that fail
   today: `Node.AMissingBackendPathStaysOfflineAndIsNotCreated` and
   `Node.ABackendPathThatCannotBeCreatedLeavesTheNodeDegradedNotRefused`. Severity:
   **blocks the port** (an unmounted disk must not take DATA onto the root filesystem).
3. **A put refused for room still encrypts the object.** Macha checks room before it
   seals (M:local_store.cpp:1099-1113, 1171-1183); Dagda seals first
   (D:local_store.cpp:990-995), once per backend tried (D:storage_pool.cpp:511-535).
   Test that fails today: `StoreLocalStore.APutRefusedForRoomSealsNothing` (needs a seal
   count in `LocalStoreDiagnostics`, or a sealing seam, to observe). Severity:
   **loses performance** (a pool at capacity spills every write through full backends).
4. **The reload waits for a maintenance pass.** Macha's reload touches only the pool's
   locks (M:local_state.cpp:189-193); Dagda's holds `_maintenance_mutex`
   (D:node.cpp:591). Test that fails today:
   `Node.AReloadDoesNotWaitForAMaintenancePassInProgress` (hold a pass in a stage hook;
   expect `reconfigure` to return). Severity: **loses performance** (reload latency).
5. **The backends-online count is not available at refresh time.** Macha advertises it on
   every storage refresh (M:src/cluster/storage_server.cpp:56-57) into telemetry and
   Status (M:src/cluster/cluster.cpp:159, M:src/api/status_api.cpp:316-317); Dagda's
   refresh advertises capacity and used only (D:dagda/store/storage_server.cpp:94-102)
   and publishes `dagda.storage.backends_online` to Status every 10 s (D:node.cpp:1080).
   Macha can poll `node.data().online_backends()`; the change is when a fresh count is
   seen. Test that fails today: `StoreStorageServer.ARefreshAdvertisesTheBackendsOnline`.
   Severity: **cosmetic**.
6. **The maintenance stage hook does not hear three local stages.** Macha calls
   `enter_stage("local-rebalance")`, `("scrub")`, `("ledger-verify")`
   (M:src/service/maintenance.cpp:1102, 1163, 1214); Dagda's stages do not
   (D:node.cpp:682-706, 799-824, 826-859) although `MaintenanceOptions::stage` promises
   every stage in Macha's names (D:node.h:132-135). No Macha test holds these names
   (basemind grep of `tests/` finds none). Test that fails today: `NodeMaintenance.TheStageHookHearsEveryLocalStageInMachasNames`.
   Severity: **cosmetic**.
7. **Macha's store-level read classification test was not ported.**
   M:tests/test_storage_v18.cpp:620 (`test_a_store_tells_a_bad_record_from_a_failed_read`,
   added after the 5a06d3c port of that file, D:dagda/test/store_local_store_test.cpp:12)
   has no Dagda twin. The behaviour matches by reading (row 41). Proposed test
   `StoreLocalStore.TellsABadRecordFromAFailedRead`: it would **pass** today; this is
   coverage, not a behaviour fix. Severity: **cosmetic**.
8. **A reload refused for a duplicate backend keeps the locks it took.**
   `Node::reconfigure` merges new locks (D:node.cpp:609) before the pool's duplicate
   check throws `std::invalid_argument` (D:storage_pool.cpp:422-424), so those roots stay
   locked until the next reload, and the caller gets `invalid_argument`, not
   `NodeRefused` with nothing changed as D:node.h:465-468 promises. Test that fails today:
   `Node.AReloadRefusedForADuplicateBackendChangesNothing`. Severity: **cosmetic**.
9. **No timed diagnostics on the pool lock.** Macha's `TimedLock(mutex_, "storage.pool")`
   (M:storage_pool.cpp:89, 299) reports holds or waits over 10 ms at level ALL; Dagda has
   a plain lock and a TODO waiting on Dannan (D:storage_pool.h:323-325). Test that fails
   today: `StoreStoragePool.ASlowPoolLockHolderIsReported`. Severity: **cosmetic**.

Not gaps, but changes Macha's facade must choose deliberately:

- CONTROL reserve: Dagda refuses a CONTROL put with less than 256 MiB free by default
  (D:control_store.h:30-35); setting `ControlStoreOptions::reserve_free = 0` reproduces
  Macha, which requirements section 7 says not to (row 36).
- `durability_barrier` throws `std::invalid_argument` for an invalid ticket where Macha
  threw `std::runtime_error` (row 10); only Dagda's own server and store call it.
- The sweep's `deferred` is per pass, not per slice (row 23), deliberately.

## 4. Not mapped

- The GC sweep's claim seam end to end (`ClaimLocks`, `RetentionStore`, the collector's
  three `gc_step` calls at D:dagda/ledger/collector.cpp:195, 249, 295) and Macha's
  catalogue sweep of the CONTROL store (M:src/catalogue/catalogue.cpp:2670-2737), which
  in Dagda would run beside the node collector's own CONTROL sweep: left to the GC section.
- `PersistentBlockCache`'s ephemeral `LocalStore` (M:src/storage/persistent_cache.cpp:74):
  left to the cache section.
- The callers inside `DistributedStore`, `StorageServer` and `ControlObjectFetch` (their
  admission, 507/404/409 replies and probes): left to those sections; only what they
  need from the pool is mapped above.
- `HeldLedger` and `ObjectTrie` internals behind `held_view`, `seed_ledger` and
  `verify_step` (cache division, checkpoints, node-local key): not compared line by line.
- `DiskServiceMonitor` and `DiskServiceTimer` internals (M:src/storage/io_pressure.hpp vs
  D:dagda/store/io_pressure.h): compared only through the ported tests, not line by line.
- Dannan's `SupervisedThreads::once/loop` against Macha's `run_supervised_once/loop`
  (restart and backoff of the scan, presence walk and durability worker), and whether
  Dannan names the durability worker's thread as Macha's `set_thread_name("macha-durable")`
  does (M:src/storage/durability_domain.cpp:220): unverified.
- The node's `local_yield` predicate against Macha's `idle_for(viewer|loader) < quiet`
  (M:maintenance.cpp:1105-1109, 1166-1170) and the rebalance rest on events: left to the
  maintenance section.
- The log scope prefix Dannan's `ScopedLogger` puts before Macha's unchanged line texts
  (`dagda.pool`, `dagda.pool.backend`, `dagda.control`): format not checked.
- Macha tests added between 5a06d3c (where Dagda's ports were taken) and f0597a4 other
  than gap 7: not enumerated.
- `src/cluster/store_node.cpp` in Macha's working tree (a facade on the port branch, not
  in f0597a4): not read.
