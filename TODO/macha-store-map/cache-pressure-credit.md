# Macha store map: the persistent block cache, io pressure, DATA credit and admission

Macha side: develop f0597a4, read from `build/macha-f0597a4/` (the truth). Macha
paths below are relative to that tree (`src/...`); `ds.cpp` is
`src/cluster/distributed_store.cpp`, `ds.hpp` its header. Dagda side: this
repository at develop 59b20e9, paths relative to the repository root;
`dds.cpp`/`dds.h` are `dagda/store/distributed_store.{cpp,h}`. Every claim was
read in code on both sides; anything not read is marked unverified.

How Macha reaches each Dagda part: `Node::cache()` (`dagda/node.h:363`),
`Node::credit()` (`node.h:372`, null when `NodeOptions::admission` is unset),
`Node::io()` (`node.h:374`), `Node::activity()` (`node.h:377`), `Node::data()`
(`node.h:361`, for `service_monitor()`), `Node::store()` (`node.h:367`, for the
store's Macha forms: `cache_local`, `hydrate`, `locally_available`, the activity
forms, `read`/`get` with no deadline), `Node::reconfigure` (`node.h:471`), and
`Context::objects().read` (`dagda/context.h:195-196`, which refuses a deadline that
is not positive). Nothing below needs an alias of a Macha type.

## 1. The table

Verdicts: SAME, DIFFERS (how), MISSING. Tests: Macha `file:suite:name`, Dagda
`Suite.Case`.

### The persistent block cache

| # | Macha (name, file:line) | What is relied on | Dagda through Node/Context (file:line) | Verdict | Tests (Macha / Dagda) |
|---|---|---|---|---|---|
| 1 | `PersistentBlockCache(CacheConfig, key)`, `src/storage/persistent_cache.cpp:60-85`; built at recovery stage "cache", `src/cluster/local_state.cpp:150-153` | No path or `max_blocks` 0 is no cache (`config.hpp:49-53`: path empty, max_blocks 0, prefer_metadata true); an ephemeral loose-only `LocalStore` with an unlimited limit (`LocalStoreOptions{limit}` leaves pack_threshold 0, `local_store.hpp:44-53`); the LRU is rebuilt from one directory walk (`:119-131`); a cache that cannot open is logged `warn "persistent cache disabled: ..."` and disabled, never an error (`:78-84`); sealed under the cluster storage key | `Node` step "open cache", `dagda/node.cpp:263-267`; ctor `dagda/store/persistent_cache.cpp:22-48`; `CacheOptions` `persistent_cache.h:29-38` (same defaults). `LocalStoreOptions::loose_only(max)` (`local_store.h:107-113`), ephemeral mode, same warn text (`persistent_cache.cpp:46`) through the `dagda.cache` scoped logger. Sealed v19 under the domain key (`node.cpp:265-266`); dagda-migrate rewrites a Macha cache in place under `derive_v1(cluster)` (`dagda/migrate/migrate.cpp:868, 882, 907, 947-950`), so a warm cache survives the cutover | SAME | `test_invariants.cpp:3147 invariants:test_persistent_cache_is_explicitly_ephemeral`, `test_storage_metadata.cpp:3861 storage_metadata:test_storage_pool_and_persistent_cache` / `StorePersistentCache.TheCacheIsExplicitlyEphemeral`, `StorePersistentCache.TheCacheMakesNoCallForStableStorage`, `StorePersistentCache.AMissingOrCorruptCacheIsDiscardedAndRebuilt`, `StoreLayoutV19.TheControlStoreAndTheCacheSealUnderTheirDomain`, `MigrateCompareDamage.AnObjectLostFromTheCache` |
| 2 | `reconfigure(CacheConfig)`, `persistent_cache.cpp:87-112`; called by `LocalState::reconfigure` after the pool's reconfigure and refresh, `local_state.cpp:189-193` | A new path reopens, fewer blocks trims LRU-first outside the state lock, no path or no blocks disables at once; writer then metadata lock, then state lock | `Node::reconfigure(NodeReload{backends, cache})`, `node.cpp:590-621`: pool reconfigure, refresh, then `PersistentBlockCache::reconfigure` (`persistent_cache.cpp:50-73`), same order and trim; also locks a new cache root and bumps `_opening` | SAME | (none found in Macha) / `NodeReconfigure.BackendsAndCacheFollowALiveReload` |
| 3 | `enabled()`, `persistent_cache.cpp:114-117` | store open and max_blocks set | `persistent_cache.cpp:75-78` via `Node::cache()` | SAME | / `StoreDistributedStore.CacheLocalAndHydrateFillTheCache` (through `hydration_available`) |
| 4 | `put(id, data)`, `persistent_cache.cpp:167-232` | Writer lock serialises puts so `max_blocks` is never exceeded; a present object is only marked used; one slot made before the put by evicting the LRU head (counted in `evictions`); no durability barrier; a reconfigure racing the physical put is tolerated; an exception is `debug "persistent cache write: ..."` and false | `persistent_cache.cpp:121-181`, line for line | SAME | `test_storage_metadata.cpp:3861` / `StorePersistentCache.TheCacheEvictsTheLeastRecentlyUsedAndReopens` |
| 5 | `get(id)`, `persistent_cache.cpp:234-279` | No cache-wide lock across the device read (state lock only to copy the store pointer, again to mark used); hit and miss counted; a read that throws is a miss, `debug "persistent cache read: ..."`, and the entry removed from the same store instance only | `persistent_cache.cpp:183-227`, line for line | SAME | `test_storage_metadata.cpp:3861` / `StorePersistentCache.AMissingOrCorruptCacheIsDiscardedAndRebuilt` |
| 6 | `has(id)`, `persistent_cache.cpp:281-288` | index lookup on the store snapshot | `persistent_cache.cpp:229-236` | SAME | / `StoreDistributedStore.CacheLocalAndHydrateFillTheCache` |
| 7 | `remove(id)`, `persistent_cache.cpp:290-313` | under the writer lock; LRU entry dropped | `persistent_cache.cpp:238-260` | SAME | / (through `erase_all`, row 22) |
| 8 | `blocks()`, `stats()`, `persistent_cache.cpp:315-323`; `Stats` `persistent_cache.hpp:61-67` | hits, misses, evictions cumulative since start; entries instantaneous | `persistent_cache.cpp:262-267`, `persistent_cache.h:86-92`; published `dagda.cache.{enabled,blocks,capacity_bytes,used_bytes,hits,misses,evictions}`, capacity = max_blocks x extent_size as Macha's, `node.cpp:1159-1166` | SAME | / (no Dagda test reads the Status names; unverified by test) |
| 9 | `remember_metadata`, `metadata()`, `persistent_cache.cpp:329-390` (`<cache>/metadata/current.meta`, magic DHTCMET1) | the recovery seed beside the cache | `PersistentBlockCache::with_seed`, `persistent_cache.cpp:269-279`: the cache hands the seed's place to the heads; the record itself is the heads' | DIFFERS: moved to the heads (not mapped here, see "Not mapped") | `test_storage_metadata.cpp:2031` / `HeadsSeed.*` |
| 10 | Cache advert: `StorageServer::refresh`, `src/cluster/storage_server.cpp:62-65`, `advertise_cache(max_blocks x extent, blocks x extent, {hits, misses, evictions})`, read per node by Status, `src/api/status_api.cpp:300-314` | every peer's Status shows each node's cache fill and activity | `StorageServer::advertise`, `dagda/store/storage_server.cpp:94-102`, advertises capacity, used and extents only; the cache figures reach only this node's own Status surface (row 8) | MISSING (peer-visible cache advert) | / none |

### The cache inside the store, and foreground reads

| # | Macha (name, file:line) | What is relied on | Dagda through Node/Context (file:line) | Verdict | Tests (Macha / Dagda) |
|---|---|---|---|---|---|
| 11 | `get_shared(id, stripe, frame, deadline, cancelled)`, `ds.cpp:1720-1811` (and `get`, `:1704-1718`); called by the FUSE read handle, `src/filesystem/filesystem.cpp:131-132` | Foreground demand noted before any device access (`:1729-1730`); local, then cache, then remote; each local stage takes DATA credit of `extent_size` (not the object's size) at the read's class with the caller's deadline and cancel, released right after the device read (`:1744-1755`, `:1771-1782`), and retained memory of `extent_size` for the reader's class with the same deadline (`:1749-1753`, `:1776-1780`), held inside the returned `ObjectBuffer` until the last reader lets it go (`:1763-1767`, `:1792-1796`; the FUSE handle releases its previous extent first for this reason, `filesystem.cpp:126-128`); interactive bytes noted per stage (`:1757-1758`, `:1784-1785`, `:1803-1804`); log lines `debug "playback object read source=..."` at 250 ms and `trace "DIAG object-get ..."` | `DistributedStore::read_impl`, `dds.cpp:1727-1784`, via `Node::store().read/get` (no deadline allowed) or `Context::objects().read` (positive deadline only, `context.h:194-196`). Same order, same credit charge and class, same notes, same log text (`dds.cpp:1736-1746`); the deadline is one time across the stages (`:1747-1752`); a DATA device slot is taken with the credit when the reserve is on (`data_work.cpp:364-380`). No retained memory is taken for a local or cache read: `buffer_of` builds a buffer with no hold (`dds.cpp:94-98`, `:1763`, `:1777`); only a remote reply's frame is held (`dds.h:172-177`) | DIFFERS: no retained-memory lease on local and cache reads; slot taken when the reserve is on; Context refuses a read without a deadline | `test_rpc_cluster.cpp:890 rpc_cluster:test_store_runtime_cache_keeps_a_playback_fetch` / `DistributedStore.ARuntimeCacheKeepsAPlaybackFetch`, `StoreRepair.AReadsDeadlineIsOneTimeAcrossItsStages`, `StoreRepair.AStoreReadWithItsOwnDeadlineGivesUpAtIt`, `StoreRepair.AReadPastItsDeadlineGoesNoFurther` |
| 12 | Cache hit promotes when this node should own it: `enqueue_local_copy(id, cached, true)`, `ds.cpp:1786-1787` | a playback cache hit converges the owner's copy without a fetch | `dds.cpp:1775` | SAME | / `DistributedStore.ARuntimeCacheKeepsAPlaybackFetch` |
| 13 | Opportunistic persist after an interactive remote fetch: `get_remote(..., opportunistic_persist = interactive)` `ds.cpp:1802`, queued by the leader or a waiter once (`:1513-1514`, `:1573-1576`, `:1602-1617`) | a foreground or read-ahead fetch lands in the cache (and the pool if owner) without the reader waiting | `read_impl` passes `interactive` (`dds.cpp:1780`); `finish_fetch` queues once, before any reader is told (`dds.cpp:1981-1990`) | SAME | `test_rpc_cluster.cpp:890` / `DistributedStore.ARuntimeCacheKeepsAPlaybackFetch`, `DistributedStore.ConcurrentReadersShareOneFetchAndBuffer` |
| 14 | `enqueue_local_copy(id, data, promote)`, `ds.cpp:3105-3139` | Nothing to do without a cache unless promoting; a speculative retained-memory `try_acquire` of the object's size first, else dropped with `debug "opportunistic persistence skipped object=... reason=retained_memory ..."` (`:3111-3119`); a 256 MiB queue bound, else `reason=queue_full` (`:3123-3129`); never waits | `dds.cpp:2007-2022`: same cache/promote rule, same bound (configurable, `DistributedStoreOptions::local_copy_bytes` 256 MiB, `dds.h:285`), same `queue_full` line. No retained-memory admission and no `reason=retained_memory` drop | DIFFERS: queued copies are not charged to retained memory | / (no test of either drop) |
| 15 | `local_writer_loop`, `ds.cpp:3148-3201` | One writer; speculative DATA credit of the object's size with the destructor's cancel flag (`:3174-3177`), else `reason=data_credit`; cache put, `reason=cache_put_failed`; promote into the pool only when there is no cache or the cache put failed, then `advertise_storage` at once (`:3194-3198`) | `dds.cpp:2029-2060`: `admit_data(speculative, size, no_caller_deadline)` (credit, then a slot when the reserve is on); same skip lines; same promote rule (`:2056`); the cache's state read at write time, not at enqueue; no storage advert after a promotion (the server's refresh advertises, `storage_server.cpp:78-87`); a stopping node stops the arbiter first (`node.cpp:404-407`) instead of a cancel flag | DIFFERS: no immediate advert after promotion; cancellation by arbiter stop | / `DistributedStore.HealsABadLocalCopyFromAPeer` (waits for copies settled) |
| 16 | `wait_local_copies_settled`, `ds.cpp:3141-3146` | queue empty and nothing being written | `dds.cpp:2024-2027` via `Node::store()` | SAME | / `DistributedStore.HealsABadLocalCopyFromAPeer`, `DistributedStore.AReadNamesTheNodeThatServedIt` |
| 17 | `locally_available(id)`, `ds.cpp:2231-2233`; hydration's skip test, `src/filesystem/hydration.cpp:695`, `:800` | pool or cache holds it, no device read | `dds.cpp:587` | SAME | / `StoreDistributedStore.CacheLocalAndHydrateFillTheCache` |
| 18 | `cache_local(id, data)`, `ds.cpp:2235-2248`; write-through from FUSE writes, `filesystem.cpp:389-390`, `:428-429`, `:1092-1093` | false without a cache; speculative credit of the object's size, waited under the no-progress window; `debug "cache write-through skipped ..."` on a throw | `dds.cpp:589-598`, same; a slot too when the reserve is on | SAME | `test_hydration_catalogue.cpp:388` / `StoreDistributedStore.CacheLocalAndHydrateFillTheCache` |
| 19 | `hydration_available()`, `ds.cpp:2250-2252`; `hydration.cpp:689`, `:793` | the cache is enabled | `dds.cpp:600` | SAME | / `StoreDistributedStore.CacheLocalAndHydrateFillTheCache` |
| 20 | `hydrate(id, stripe = 0, frame = speculative)`, `ds.hpp:464`, `ds.cpp:2254-2265`; called with the hint's stripe and frame (read_ahead or speculative, `hydration.cpp:247, 362, 390, 458`), `hydration.cpp:621-622`, `:711` | true at once if pool or cache holds it; remote fetch as non-foreground with no opportunistic persist and no deadline; credit of the object's size at the hint's class, then cache put | `hydrate(id, ReadOptions)`, `dds.h:504`, `dds.cpp:602-609`: same steps; `ReadOptions::frame` defaults to foreground (`dds.h:187`) and `get_remote` treats a foreground frame as foreground (`dds.cpp:1792`) | DIFFERS: a call that names no frame runs as foreground; with the frame and stripe passed, as Macha's callers do, it is the same | `test_hydration_catalogue.cpp:388 hydration_catalogue:test_cache_hydrator_fetches_to_persistent_cache` / `StoreDistributedStore.CacheLocalAndHydrateFillTheCache` |
| 21 | `ensure_local` cache step, `ds.cpp:2267-2290` (`:2277-2282`) | valid local under extent credit; else the cache copy put under credit; else fetch | `dds.cpp:2670-2689` (`:2684`); also notifies storage and the tickets | SAME | / (repair tests) |
| 22 | `erase_all` cache removal, `ds.cpp:2312` | the cache copy goes with the pool's | `dds.cpp:2659`; the server's `delete_object` too, `storage_server.cpp:291` | SAME | / `StoreRemove.*` (cache by `remove_here`, `dds.cpp:2482`) |
| 23 | Repair pull promotes a cached copy before any network work, `ds.cpp:3039-3053` | the cache read takes no credit; the pool put takes speculative credit; a refusal stops the step | `dds.cpp:3144-3158` | SAME | / (repair section) |

### Activity

| # | Macha (name, file:line) | What is relied on | Dagda through Node/Context (file:line) | Verdict | Tests (Macha / Dagda) |
|---|---|---|---|---|---|
| 24 | `foreground_activity`, `interactive_activity`, `loader_activity`, `ds.hpp:466-474`; called per read and write by the FS, `filesystem.cpp:186-192`, `:888-895` | bytes by frame and presence by class on the node's one clock | `dds.h:489-491` on `Node::activity()`'s clock (`dannan/status/activity.cpp:16-26`) | SAME | `test_rpc_cluster.cpp:82` / `DistributedStore.APutIsLoaderActivityNotViewerActivity`, `StoreDistributedStore.ActivityIsObjectWorkInMachasMeaning` |
| 25 | `take_foreground_bytes`, `take_interactive_bytes`, `take_loader_bytes`, `ds.hpp:513-521`; maintenance, `src/service/maintenance.cpp:400-402` (working tree; unverified at f0597a4, the file differs) | exchange to zero | `dds.h:492-494` | SAME | / `StoreDistributedStore.ActivityIsObjectWorkInMachasMeaning` |
| 26 | `idle_for(WorkClass)`, `ds.hpp:526-528`, `activity_clocks.cpp:40-45` | 24 h when never present | `dds.h:495`, `activity.cpp:35-39` (also fixes Macha's epoch-zero ambiguity with a +1) | SAME | / |
| 27 | `ActivityClocks::note(WorkClass)` for API requests, `src/service/service.cpp:81-82` (an API request is viewer work and a viewer present) | that presence reaches the DATA arbiter's law 3 check, which reads the same `NodeResources::activity` (`node_resources.cpp:21-23`) | The arbiter reads the node's own clock (`node.cpp:245-249`), built by the node (`node.cpp:212`) with no option to supply one; Macha must note into `Node::activity()` (`node.h:377`) | DIFFERS: two clocks unless the owner writes into the node's | `test_service_root.cpp:529` / `Node.TheLoaderYieldsToARecentForegroundUserUnderPressure` (store-driven presence only) |
| 28 | Arbiter's presence predicate: `viewer_recently_active(maintenance.foreground_quiet)`, `node_resources.cpp:21-23`, `activity_clocks.cpp:47-49`, default 2000 ms (`config.hpp:57`) | a viewer between extents still counts | `recently_active(foreground, repair.foreground_quiet)`, `node.cpp:245-249`, default 2000 ms (`dagda/store/repair.h:47`) | SAME | `test_io_pressure.cpp:314` / `StoreIoPressure.AForegroundUserBetweenTwoExtentsIsStillPresent`, `Node.TheLoaderYieldsToARecentForegroundUserUnderPressure` |

### Device pressure

| # | Macha (name, file:line) | What is relied on | Dagda through Node/Context (file:line) | Verdict | Tests (Macha / Dagda) |
|---|---|---|---|---|---|
| 29 | `DiskServiceMonitor::note(elapsed, bytes)`, `src/storage/io_pressure.hpp:75-118` | expected = overhead + bytes x per_mib; 1/16 EWMA of the ratio and of latency; worst by CAS; onset above slowdown or a single sample above outlier, release below release; each transition once; lock-free | `dagda/store/io_pressure.cpp:14-48`, the same arithmetic and hysteresis | SAME | `test_io_pressure.cpp:24, 84, 113` / `StoreIoPressure.AnOrdinaryLargeWriteIsNotASlowDevice`, `.ADeviceFarSlowerThanItShouldBeIsNoticed`, `.TheSingleSampleTripScalesWithTheOperation` |
| 30 | `pressured`, `pressure_onsets`, `sample`, `forget_worst`, `thresholds`, `io_pressure.hpp:122-147` | relaxed loads | `io_pressure.h:103-109`, `io_pressure.cpp:50-54` | SAME | (as row 29) |
| 31 | `Thresholds` defaults, `io_pressure.hpp:36-47`; config `dht.io_pressure_*` 25, 120, 300, 150, 1000, min_background 1, `config.hpp:663-670` | the shipped figures | `DiskServiceThresholds`, `io_pressure.h:24-41` (25, 120, 300, 150, 1000); `AdmissionOptions::device_pressure`, `min_background_under_pressure` 1, `node.h:69-72` | SAME | `test_io_pressure.cpp:18` / `NodeSeams.TheDevicePressureFiguresReachTheMonitor` |
| 32 | Configure at recovery when `io_pressure_slowdown_percent` is non-zero, with `info "data io pressure gate enabled expected_ms=..+../MiB slowdown_percent=.. release_percent=.. outlier_percent=.. min_background=.."`, `local_state.cpp:95-109`; observe only then, `local_services.cpp:25-27` | zero turns the gate off | `node.cpp:221` (pool thresholds), `:226-232` (same line), `:252` (observe) under the same zero rule | SAME | / `NodeSeams.TheDevicePressureFiguresReachTheMonitor`, `StoreIoPressure.WithNoDeviceAdmissionIsExactlyWhatItWas` |
| 33 | `DiskServiceTimer(monitor, bytes)`, `io_pressure.hpp:178-200`, on `steady_clock`; six pool sites: put `storage_pool.cpp:416`, put_deferred `:450`, get `:590-594` (size noted after), rebalance get `:937-939`, rebalance put `:957`, scrub get `:1012-1014` | one sample is one device operation; a read is judged at the size it returned | `io_pressure.h:133-151`, on the pool's injected `TimeSource` (`_services.time`); the same six sites, `dagda/store/storage_pool.cpp:527, 558, 730-734, 1035-1037, 1052, 1103-1105` | DIFFERS: time from the node's TimeSource, not steady_clock (the same in production; a stepped source stops the gate) | `test_io_pressure.cpp:145, 193` / `StoreIoPressure.AReadIsMeasuredWithTheBytesItReturned`, `.MaintenanceReadsArePartOfTheServiceTimeTheySpend`, `.TheTimerMeasuresOnTheInjectedTime` |
| 34 | Transition lines `info "DATA device pressure onset\|released slowdown_percent=.. last_percent=.. last_us=.. last_bytes=.."`, `io_pressure.hpp:150-159`, global `Log` | an operator's search finds them | `io_pressure.cpp:56-63`, same text, through the pool's logger child "pressure" (`storage_pool.cpp:153`) | SAME (text); the scope prefix is Dannan's (rendered prefix unverified) | `test_io_pressure.cpp:42` / `StoreIoPressure.OnsetAndReleaseAreEachLoggedOnce` |
| 35 | Torrent I/O feeds the monitor when staging shares a DATA device: `monitor->note(elapsed, bytes)`, `src/torrent/torrent_manager.cpp:1325-1329` | the arbiter sees the loader's own disk cost | `Node::data().service_monitor()` (`storage_pool.h:180`, non-const) | SAME | (Macha torrent tests) / none |

### DATA credit and admission

| # | Macha (name, file:line) | What is relied on | Dagda through Node/Context (file:line) | Verdict | Tests (Macha / Dagda) |
|---|---|---|---|---|---|
| 36 | `DataResourceArbiter(capacity, viewer_reserve, background_concurrency, no_progress, viewer_recently_active)`, `src/cluster/data_work.hpp:197-223`, built by `NodeResources`, `node_resources.cpp:16-23` | 128 MiB in flight, 32 MiB viewer reserve (`config.hpp:657-658`); background concurrency 0 = half the hardware threads, at least 1 (`config.hpp:89-92`, `node_resources.cpp:17-19`); no-progress window 120 s, zero waits unbounded (`config.hpp:671-673`); throws unless 0 < reserve < capacity | `AdmissionOptions`, `node.h:61-77`; built at step "open admission", `node.cpp:242-254`; ctor `data_work.cpp:59-73`. 128 MiB, 32 MiB as Macha's. `background_concurrency` defaults to 4 with no zero rule (`node.h:64`); a zero no-progress window throws (`data_work.cpp:72`; the facade passes 100 years, `TODO/2026-11-05-second-cutover.md:262`) | DIFFERS: background_concurrency default and zero rule; no-progress zero refused | `test_foundations.cpp:95, 118` / `StoreDataWork.TheBackgroundCeilingCountsLoaderAndSpeculativeNotForeground`, `StoreDataWork.AHundredYearWindowDoesNotOverflow` |
| 37 | `observe_device(monitor, min_background)`, `data_work.hpp:202-206`; attached `local_services.cpp:25-27`, detached `:33` | min_background at least 1 | `data_work.cpp:75-79`; `node.cpp:252`; not detached, the arbiter is stopped first at `Node::stop` (`node.cpp:404-407`) and destroyed before the pool (`node.cpp:427-429`) | SAME | / `StoreIoPressure.BackgroundWorkAlwaysDrainsOnAPressuredDevice` |
| 38 | `acquire(context, bytes)`, `data_work.hpp:261-346` | control throws; charge max(1, bytes); over the class budget fails at once; waits counted once per waiter, pressure refusal counted once; caller's deadline is absolute and not re-armed; with none, no-progress windows re-armed by any release, then `warn "DATA credit wait abandoned after N ms with no release anywhere: class=.. bytes=.. used=../.. lower_active=../.. waiting_viewers=.. waiting_loaders=.."`; stop, cancel or expiry refuse even with room (`:327-331`); a cancel is seen only on a wakeup | `data_work.cpp:130-166` over Dannan's `ClassWaits::wait` (`dannan/sched/class_waits.h:65-118`): the same rules and the same warn line word for word (`data_work.cpp:157-162`), logged after the lock is let go; a cancel is polled every 25 ms (`data_work.cpp:23, 146`; `class_waits.h:113`); release notifies outside the lock (`data_work.cpp:183-194`) | DIFFERS: a cancelled waiter leaves within 25 ms instead of at the next release or deadline (better); `frame_type_name` source for the `class=` word unverified on Macha's side | `test_rpc_cluster.cpp:1276, 1307` / `StoreDataWork.ACreditWaitGivesUpWhenNothingIsMoving`, `.ACreditWaitSurvivesGenuineContention`, `.AWaitWithNoDeadlineIsReArmedByAnyRelease`, `.ACallersOwnDeadlineIsNotReArmedByARelease`, `.StoppingRefusesEveryWaiter` |
| 39 | `try_acquire`, `data_work.hpp:348-379` | never waits; pressure refusal counted | `data_work.cpp:168-181` | SAME | / (server busy tests, row 48) |
| 40 | `available()`, `data_work.hpp:225-259` | viewer takes any capacity and is never refused for pressure; a waiting viewer closes lower classes; under pressure and past min_background, speculative refused, loader refused when a viewer is present (waiting, holding credit, or recently active); background ceiling; lower classes within capacity less reserve; speculative behind a waiting loader | `data_work.cpp:83-106`, the same rules in the same order | SAME | `test_io_pressure.cpp:229, 254, 279, 343, 372, 392` / `StoreIoPressure.AForegroundRequestIsNeverRefusedBecauseTheDiskIsSlow`, `.TheLoaderRunsFreelyUnderPressureWithNoForegroundUser`, `.TheLoaderYieldsToAForegroundUserOnASlowDevice`, `.BackgroundWorkAlwaysDrainsOnAPressuredDevice`, `.ControlCannotEnterTheDataArbiterAtAll`, `.WithNoDeviceAdmissionIsExactlyWhatItWas`, `StoreDataWork.TheForegroundReserveHoldsAndALoaderIsServedBeforeSpeculation` |
| 41 | `Lease`, `data_work.hpp:96-135`, `:393-399` | move-only, released on destruction | `data_work.h:109-127`, `data_work.cpp:37-57` | SAME | / |
| 42 | `stop()`, `data_work.hpp:401-405`; `NodeResources::stop`, `node_resources.cpp:26-29` | refuses new leases, wakes all | `data_work.cpp:196-202`; `Node::stop`, `node.cpp:406` | SAME | / `StoreDataWork.StoppingRefusesEveryWaiter` |
| 43 | `stats()`, `data_work.hpp:407-431`; Status `diagnostics.data_resources.*`, `status_api.cpp:1027-1054` | the figures by Macha's names (`viewer_reserve_bytes`, `viewer_admissions`, `viewer_waits`, ...) | `data_work.cpp:213-239` via `Node::credit()->stats()`; Status `dagda.admission.*`, `node.cpp:1250-1273`. `viewer_*` are `foreground_*`; adds `no_progress_failures` | DIFFERS: field names; one more figure | / (no test reads the Status names) |
| 44 | `DataWorkContext`, `data_work.hpp:25-55` | control throws; progress present only with a budget; the arbiter itself ignores quantum and progress | `data_work.h:42-59`, `data_work.cpp:29-33` (origin "dagda.data"; Dannan `TimePoint` deadline) | SAME | `test_contract.cpp:134` / `StoreDataWork.TheContextIsTheDataSpecialisation` |
| 45 | Torrent `loader_admission(arbiter)`, `src/torrent/torrent_disk_io.cpp:988-1002`, `torrent_manager.cpp:1271` | loader credit in 1 s deadline slices until aborted | `Node::credit()->acquire(DataWorkContext(loader, bytes, credit->now() + 1s), bytes)` (`node.h:372`, `data_work.h:150, 160`) | SAME | `test_torrent_disk_io.cpp:902` / none on Dagda's side |
| 46 | Local stages of a read take credit, `ds.cpp:1744-1755`, `:1771-1782` (row 11); a local fetch inside `get_from`, `ds.cpp:1357-1369` | extent credit and extent memory at the read's class | `dds.cpp:1754-1758`, `:1767-1771`; no memory (row 11) | DIFFERS: no retained memory (as row 11) | as row 11 |
| 47 | Local writes take credit at the write's class and no lease is held across a peer: `put_impl` launch `ds.cpp:297-300`, `put_here` `:1010-1011`, `put_deferred_here` `:1027-1028`, `put_on` `:1323-1330` | law 3 bounded by credit; a peer admits its own copy | `dds.cpp:808-816` (the write's own deadline, else the window), `put_here` `:1166-1168`, `put_on` `:1177-1185` | SAME (credit); the write path itself is the write section's | `test_rpc_cluster.cpp:237` / `StoreRepair.NodesReplicatingToEachOtherDoNotHoldTheirSlotsAcrossThePeer`, `StoreRepair.AWriteSeeksItsSecondCopyAfterASlowLocalAdmission` |
| 48 | Peer requests: `have_object`, `have_objects`, `have_valid_objects` try once (`storage_server.cpp:89-93`, `:119-123`, `:161-165`, "503 storage_busy"); `get_object`, `put_object(_deferred)` wait (`:202-205`, `:244-247`, "503 stopping"); control objects take no DATA credit (`:171-195`, `:220-233`) | a busy node says so at once for probes and waits for transfers | `dagda/store/storage_server.cpp:115-117` (try once or wait), `:175-176`, `:195-199`, `:217-221` (try), `:228-229`, `:257-258` (wait), same codes and words (`:33-36`); control objects take a control slot when the reserve is on (`:189-190`, `:244-245`, `:276-277`) | SAME | `test_rpc_cluster.cpp:1330` / `StoreRepair.ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit`, `.AServedRequestWaitsForCreditPastAnyLimit`, `.AServedRequestAfterAStallAnswersStopping`, `.TheServerRefusesInMachasCodes`, `.ABusyPeerStillSaysWhatItDoesNotHold` |
| 49 | Prompt replication reads the local object with no credit, `ds.cpp:1303` | a prompt copy never waits on DATA admission here | `dds.cpp:1681-1689` under `DistributedStoreOptions::prompt_read_admission`, default true (`dds.h:279-283`) | DIFFERS: default takes speculative credit; the facade must turn it off | / `StoreRepair.APromptCopyReadsWithoutCreditWhenTheNodeSaysSo`, `StoreRepair.APromptCopyWaitsForCreditByDefault` |
| 50 | (none: Macha has no device I/O reserve) | DATA operations bounded by credit bytes and the background ceiling only | `IoReserve`, `data_work.h:200-294`, `data_work.cpp:243-385`, on by default (`AdmissionOptions::device_reserve` true, 8 slots, 2 for control, `node.h:75-76`, `data_work.h:202-209`); every `admit_data` takes a slot after its credit | DIFFERS: new; the facade must turn it off (`node.h:73-75`) | / `StoreDataWork.ControlWorkIsAdmittedWhileDataSaturatesTheDevice`, `.WithoutTheReserveControlQueuesBehindData`, `.AWaitingControlRequestIsServedBeforeData`, `.ASlotWaitWithNoDeadlineEndsAtTheWindow`, `StoreDistributedStore.AClaimIsNeverRefusedWithTheDeviceReserveOff` |

Counts: 50 rows; SAME 36, DIFFERS 13 (rows 9, 11, 14, 15, 20, 27, 33, 36, 38, 43, 46, 49, 50), MISSING 1 (row 10).

## 2. Performance-relevant differences

- Cache hit path. Identical: the state lock only to copy the store pointer
  (`persistent_cache.cpp:184-188` / Macha `:235-239`), the device read and open
  outside every cache lock, the state lock again for the LRU splice
  (`:92-100` / Macha `:133-142`). The store under it is Dagda's `LocalStore`; its
  read cost against Macha's was not compared here (local store section): unverified.
- Sealing cost. The cache seals v19, the domain in the associated data
  (`node.cpp:265`); the cipher is the same AES-256-GCM (`StoreSealing.TheCipherIsStandardAes256Gcm`).
  A few bytes more of associated data per object; no measurement made.
- Read admission. With the reserve off (Macha's facade), a local or cache read takes
  one arbiter lock as Macha's does (`data_work.cpp:364-380`). With it on, each stage also
  takes an `IoReserve` slot (a second mutex), and a foreground read has no precedence
  over a loader for slots: `IoReserve::data` admits whoever finds one free
  (`data_work.cpp:267`), and only a waiting control request closes it. Six DATA slots by
  default (`data_work.h:204-208`): a loader holding six device operations queues a
  playback read behind them. Macha has no such cap.
- No retained memory on local and cache reads (`dds.cpp:94-98`) and on queued local
  copies (`dds.cpp:2007-2022`): Macha bounds both by its retained-memory ledger and makes a
  foreground read wait for memory under its deadline (`ds.cpp:1749-1753`, `:3111-3119`).
  Dagda's reads never wait for memory (faster under memory pressure) and never count
  against it (the bound is lost).
- Cancellation. Dagda's credit and slot waits with a cancel flag wake every 25 ms
  (`data_work.cpp:23`); Macha's notices a cancel only at a release or the deadline.
  Prompter cancellation; a negligible wakeup cost per cancellable waiter.
- Notification. Dagda's release notifies after unlocking (`data_work.cpp:183-194`);
  Macha's notifies under the lock (`data_work.hpp:381-391`). Fewer contended wakeups.
  Dagda waits on `std::condition_variable_any` (`data_work.h:182`), Macha on
  `std::condition_variable` (`data_work.hpp:163`).
- Defaults that differ, each a performance change if the facade forgets it:
  `background_concurrency` 4 (`node.h:64`) against half the hardware threads (2 on a
  4-core machine, where 4 would double background CPU); `prompt_read_admission` true
  (`dds.h:283`); `device_reserve` true (`node.h:75`); `no_progress` cannot be zero
  (`data_work.cpp:72`); `ReadOptions::frame` foreground (`dds.h:187`) where Macha's
  `hydrate` defaults to speculative (`ds.hpp:464`).
- Promotion advert. Macha advertises storage right after the local writer promotes
  (`ds.cpp:3197`); Dagda waits for the server's refresh (`storage_server.cpp:104-113`).
  Placement peers see the new used figure up to one refresh interval later.
- Timer clock. The pool's timers read the node's `TimeSource` (`storage_pool.cpp:527`
  ff.), a virtual call per read of the time; in production the steady clock.

## 3. Gaps for Dagda to fix

1. Local and cache reads hold no retained memory. Macha takes `extent_size` of the
   reader's class from the node's retained-memory ledger before the local and the cache
   read, waits for it under the read's deadline, and keeps it in the returned buffer until
   the last reader lets go (`ds.cpp:1749-1767`, `:1776-1796`; the FUSE handle releases its
   previous extent first for this, `filesystem.cpp:126-128`). Dagda's `buffer_of`
   (`dds.cpp:94-98`) holds nothing. Test: `StoreDistributedStore.ALocalReadHoldsItsClassMemoryUntilTheLastReaderLetsGo`.
   Severity: loses performance (the node's memory bound on readers is lost: many readers
   can pass the ledger's capacity). May be the memory section's to own.
2. Opportunistic local copies are not charged to retained memory. Macha
   `try_acquire`s speculative memory for each queued copy and drops it, logged
   `reason=retained_memory`, when there is none (`ds.cpp:3111-3119`); Dagda queues up to
   256 MiB uncharged (`dds.cpp:2007-2022`). Test:
   `StoreDistributedStore.AnOpportunisticCopyIsDroppedWhenSpeculativeMemoryIsShort`.
   Severity: loses performance (memory bound).
3. The cache is not advertised to peers. Macha's server advertises the cache's capacity,
   use, hits, misses and evictions every refresh (`storage_server.cpp:62-65`) and Status
   shows them per node (`status_api.cpp:300-314`); Dagda's server advertises capacity,
   used and extents only (`dagda/store/storage_server.cpp:94-102`). Test:
   `StoreStorageServer.TheCacheIsAdvertisedWithItsActivity`. Severity: cosmetic (Status).
4. The arbiter's presence clock cannot be the owner's. Macha's API requests note viewer
   presence on the clock its arbiter reads (`service.cpp:81-82`,
   `node_resources.cpp:21-23`); Dagda's node builds its own (`node.cpp:212`) and takes none
   in `NodeOptions`. The port must note into `Node::activity()`, or the loader stops
   yielding to an API-driven viewer under pressure. Either document that as the one clock
   or accept one in `NodeOptions`. Test:
   `Node.AnOwnersActivityClockIsWhatTheArbiterReads` (fails today: no field). Severity:
   loses performance if missed.
5. The device reserve gives foreground no precedence. `IoReserve::data` admits any class
   to a free DATA slot (`data_work.cpp:267`); a loader holding the DATA share queues a
   foreground read behind it. Off for Macha, on by default for everyone else. Test:
   `StoreDataWork.AWaitingForegroundRequestTakesTheNextDataSlotBeforeALoader`. Severity:
   loses performance (law 2) wherever the reserve is on.
6. `background_concurrency` has no zero rule. Macha's 0 is half the hardware threads, at
   least 1 (`node_resources.cpp:17-19`); Dagda's default is a fixed 4 and 0 means no cap
   (`node.h:64`, `data_work.cpp:100`). Test:
   `Node.ZeroBackgroundConcurrencyIsHalfTheHardwareThreads`. Severity: loses performance
   if the facade passes Macha's 0 through (no ceiling at all).
7. `hydrate` with no frame runs as foreground. `ReadOptions::frame` defaults to
   foreground (`dds.h:187`), so `hydrate(id)` takes the foreground reserve and is never
   refused for pressure; Macha's default is speculative (`ds.hpp:464`). Macha's callers pass
   the frame, so its port is unaffected. Test:
   `StoreDistributedStore.AHydrateNamingNoFrameIsSpeculative`. Severity: cosmetic for
   Macha; loses performance for any caller that omits it.
8. A promotion is not advertised at once. Macha advertises storage after the local writer
   promotes into the pool (`ds.cpp:3194-3198`); Dagda does not (`dds.cpp:2056`). Test:
   `StoreDistributedStore.APromotedCopyIsAdvertisedAtOnce`. Severity: cosmetic (placement's
   used figure lags one refresh).

## 4. Not mapped

- The cache's metadata seed (`remember_metadata`/`metadata`, `persistent_cache.cpp:329-390`)
  past its attachment point `with_seed`: the heads section's.
- The retained-memory ledger itself (Macha `RetainedMemoryLedger`, Dannan's
  `dannan/sched/retained_memory.h`), FUSE's memory admission (`src/fuse/fuse_frontend.cpp:998-1007`,
  which takes no DATA credit) and playback's segment holds (`src/playback/playback.cpp:3035`):
  memory, FUSE and playback, not the DATA arbiter.
- The FS write path's no-progress budget and memory (`filesystem.cpp:241-289`) and
  Dagda's `WriteOptions::no_progress_budget` (`dds.h:162`): the write section's.
- Repair's own pacing (duty share, CPU pacing, byte credit, `RepairGate`) beyond the DATA
  credit sites in rows 23 and 47: the repair section's.
- `maintenance.cpp` callers of the activity takes (row 25): the f0597a4 file differs from
  the working tree; its lines at f0597a4 were not read.
- Macha's `frame_type_name` at f0597a4 (the `class=` word of the abandoned-wait line): not
  located in the f0597a4 tree; Dannan's names not compared.
- Dagda `LocalStore` read and seal cost on the cache's hit path against Macha's: not
  measured; the local store section's.
- The working-tree `src/cluster/data_work.hpp` and `src/config.hpp` differ from f0597a4;
  only f0597a4 was mapped.
