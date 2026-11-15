# Macha's DistributedStore on Dagda: put, get, deferred writes, barriers, reads, handlers

Section of the call-by-call map from Macha's storage internals to Dagda (2026-11-14).
Macha side: develop f0597a4, extracted read-only at `build/macha-f0597a4/` (paths below are
relative to it, `ds` = `src/cluster/distributed_store.cpp`, `dsh` = `src/cluster/distributed_store.hpp`,
`ss` = `src/cluster/storage_server.cpp`). Dagda side: this repository at develop 59b20e9
(`DS` = `dagda/store/distributed_store.cpp`, `DSH` = `dagda/store/distributed_store.h`,
`SS` = `dagda/store/storage_server.cpp`, `CTX` = `dagda/context.cpp`, `CTXH` = `dagda/context.h`).
Every claim was read in code; anything not read is marked unverified.

How Macha can reach Dagda. `dagda::Context` (CTXH:182-224, 379-396) offers only
`Objects::write` (content-addressed: the id is computed from the bytes, CTX:120), `Objects::read`,
`Objects::has`, `Objects::remove` and `Tickets::status/wait/wait_complete` for one ticket; every call
there takes a positive deadline (CTX:29-31) and runs on the store's worker pool (CTX:116-141 call
`DistributedStore::write/read` async forms, which `submit` to `_workers`, DS:1101-1120, 1713-1719).
Everything else in this section is reachable only through `dagda::Node::store()` (node.h:367), the
"parts, for an owner's maintenance and for tests" accessor, which returns the whole
`store::DistributedStore`. The table names the narrowest route that exists; "Node::store()" means
the call is not in Context.

Production figures that matter below (Macha's own fixture `tests/fixtures/node-configs/fi-1.yaml:107-111`):
replicas 2, write_copies 1, write_stall 2500 ms, extent_size 4M.

## 1. Operations

| # | Macha (name, file:line) | What callers rely on | Dagda through Node / Context | Verdict | Tests (Macha / Dagda) |
|---|---|---|---|---|---|
| 1 | `put(span[, FrameType], cancelled)` -> ObjectId, dsh:395-396, ds:139-157 | id = SHA-256 of bytes; returns once write_copies copies landed or nothing more can be had promptly (ds:463-469); throws `StorageUnavailable("object replication quorum unavailable")` when nothing landed, `runtime_error("object replication cancelled")` when cancelled (ds:147-151) even if a copy landed (cancel path `finish(false, need)`, ds:378-384); default frame loader (ds:140); runs on the caller's thread; trace line `DIAG object-put` (ds:153-155). Callers: torrent publish (`src/torrent/torrent_manager.cpp:1280`, returns nullopt on throw), filesystem non-publication put (`src/filesystem/filesystem.cpp:383`). | `Node::store().put(span, WriteOptions)` DSH:445, DS:1122-1134; or `Context::Objects::write(span, options, deadline)` CTXH:193, CTX:124-129. | DIFFERS: (a) cancelled with a copy landed returns the id (throws only when `!evidence.holders()`, DS:1126-1129); (b) through Context: deadline mandatory, bytes copied once more into `Bytes` (CTX:127), runs on a store worker, returns evidence not an exception. Floor, local-first, spill and the trace line are the same (row 5). | Macha `test_store_put_is_loader_activity_not_viewer_activity` (`tests/test_rpc_cluster.cpp:82`), `test_store_put_publishes_at_the_floor_and_copies_promptly` (:100) / Dagda `DistributedStore.APutIsLoaderActivityNotViewerActivity`, `DistributedStore.APutPublishesAtTheFloorAndCopiesPromptly`, `StoreDistributedStore.MachasFormsSeekWriteCopies`. No test on either side pins the cancel-with-a-copy case. |
| 2 | `put(id, span[, FrameType], cancelled)` -> bool, dsh:397-398, ds:159-167 | throws "object hash mismatch" / "exceeds configured extent size" (ds:200-203); returns `success > 0` except cancelled -> false (ds:383); callers turn false into EINTR/ESTALE/`FsStorageUnavailable` (`filesystem.cpp:1081-1091`), `CatalogueUnavailable` (`src/catalogue/catalogue.cpp:1560-1561`). One SHA-256 (the check). | `Node::store().put(id, span, WriteOptions)` DSH:446, DS:1136-1139. Context: none with a caller-supplied id (Objects::write rehashes, CTX:120, then `check_write` hashes again, DS:685). | DIFFERS: returns `holders() > 0` (DS:1138), so true when cancelled after a copy landed; through Context one extra SHA-256 of the extent and one extra copy. | Macha `test_store_put_replaces_a_replica_that_cannot_be_launched` (:158) / Dagda `DistributedStore.APutReplacesAReplicaThatFailsAtOnce`. |
| 3 | `put_deferred(span, DurabilityBatch&[, frame], cancelled, const DataWorkContext*)` -> ObjectId, dsh:411-417, ds:169-184 | bytes written but not synced; on success appends `{id, required, replicas}` to the caller's batch (ds:248-276) where `required = need` (floor) on a normal return but `= success` when the write ended short (ds:475) or the budget ended it (ds:375); the `DataWorkContext` carries the no-progress budget and shared progress counter (ds:350-377). Throws as row 1. Callers: publication extents (`filesystem.cpp:380-381, 427`). | `Node::store().write(id, span, {deferred=true, no_progress_budget, progress, frame, cancelled})` DSH:438, or `Context::Objects::write(..., options{deferred})` CTXH:193; the evidence's `ticket` replaces the batch entry. `Node::store().put_deferred(span)` (DSH:447) returns only the id and drops the ticket, so it cannot be barriered. | DIFFERS: (a) the ticket's `sought` is always the floor (DS:969) and `met` needs `acknowledged >= sought` (DS:265), so a write that ended short is never met, where Macha's barrier accepts what landed; (b) every deferred write is one TicketRecord, unsettled until waited (DS:250, 976-980), in a table bounded at `max_tickets` 4096 (DSH:276) that forgets the oldest unsettled ticket first when no settled one exists (DS:711-726); Macha's batch has no bound; (c) cancel as row 1. Budget re-arm and the "made no progress within budget" line are the same (DS:919-932). | Macha `test_store_put_to_a_silent_replica_returns_on_the_local_copy` (:237) / Dagda `DistributedStore.APutToASilentReplicaReturnsOnTheLocalCopy`, `StoreRepair.AWriteWithABudgetEndsWhenNothingMovesForIt`, `StoreRepair.AWritesBudgetIsReArmedByItsPipelinesProgress`. Nothing pins the 4096 bound or `required = success`. |
| 4 | `put_deferred(id, span, batch[, frame], cancelled, work)` -> bool, dsh:418-421, ds:186-195 | As row 3 with a known id; the commit's re-put of an unsatisfiable id goes through it (`filesystem.cpp:1215-1216`). | `Node::store().write(id, span, {deferred=true})` DSH:438. Context: none with a known id. | DIFFERS: as row 3. | As row 3. |
| 5 | The write path `put_impl`, ds:197-476 (rows 1-4 share it) | local store first (ds:214-218), then placement order; floor = `max(1, min(write_copies, nodes))` (ds:222); spill past a peer idle `>= write_stall` (ds:390-399, "object write stalled; spilling"); fallback launched per failure (ds:444-451); stop when no further copy can be had promptly (ds:463-469); queue prompt replication when `success < target` (ds:277-278); slow-write line "object write quorum ... result=" at >= 500 ms (ds:281-289); remote puts take no local DATA lease (ds:293-296), the local one does (ds:299-304); remote call deadline `call_deadline::object_transfer` 30 min (ds:331); storage event when the local copy is new (ds:306-307). Polls every 1 ms (ds:471-472). | `DistributedStore::run_write` DS:732-1061. | DIFFERS: local first, floor, spill, fallback, stop rule and prompt queue are the same (DS:749-751, 777, 900-913, 939-947, 956-957, 1029). Differences: `sought`/`required` (row 3); cancel (row 1); every write also builds a ticket and the replication set over the whole roster (DS:745, 966-1024); the local copy is checked against the writer's scope first (DS:807); each remote put carries a `WritePrincipal` (DS:784-785) and is checked by the peer (row 33); every remote call is also watched by `call_no_progress` 300 s (DS:852, DSH:250), which Macha's has not; the quorum line's `result=` is `success >= floor && success > 0` (DS:1049) where Macha's is the return value; a new debug line "object write landed nowhere" (DS:1059). Waits on a condition variable at most 25 ms (DS:961) instead of 1 ms polling. | Dagda `DistributedStore.WriteCopiesTwoUsesAFallbackWhenAPreferredOwnerIsFull`, `StoreDistributedStore.APutSeeksWriteCopiesThroughAFallback`, `DistributedStore.AnEdgeNodeNeverOwnsAndItsWritesLandOnOwners`, `DistributedStore.NothingCanTakeTheWrite`, `StoreRepair.AWriteSeeksItsSecondCopyAfterASlowLocalAdmission`, `StoreRepair.AWriteAfterAWholeWindowStallHereLandsOnAPeer`, `StoreRepair.AWriteWithNoDeadlineWaitsForAPeerInsideTheWindowPastAnyLimit`. |
| 6 | `put_here(id, span, frame)` -> bool, dsh:404, ds:1006-1021 | durable here and returns; queues prompt replication; falls back to `put` when the node hosts no extents, admission fails or the local put fails. Caller: catalogue artwork (`catalogue.cpp:2172`, frame speculative by default, `catalogue.hpp:513-515`). | `Node::store().put_here(id, span, frame)` DSH:450, DS:1156-1175. Context: none. | DIFFERS (cosmetic): adds a scope check of this node under the domain's name and returns false on refusal without the fallback put (DS:1161-1164). Otherwise the same. | Dagda `StoreDistributedStore.PutHereIsLocalFirstAndAPutWhereNoExtentsAreHeld`. |
| 7 | `put_deferred_here(id, span, batch, frame)` -> bool, dsh:406, ds:1023-1043 | written here, batch requirement `required = 1` on the local replica only (ds:1035-1038), prompt replication queued; falls back to full `put_deferred` (floor write_copies) when not hosting, not admitted or refused. Caller: artwork staging (`catalogue.cpp:2186`). | `Node::store().put_deferred_here(id, span, frame)` DSH:455, DS:1146-1154 (a `persisted` deferred write, so floor 1). Context: none. | DIFFERS: the fallback keeps floor 1 rather than write_copies (same at production write_copies 1); a ticket replaces the batch entry (row 8). | No test on either side pins the fallback floor. |
| 8 | `DurabilityBatch::add`, ds:61-92 | keeps only the non-dominated frontier: a requirement whose replica keys match and whose generations are all `>=` dominates, whatever its object id, so a file's sequential deferred puts collapse to one requirement per replica set (at write_copies 1, one). | None: each deferred write is a TicketRecord (DS:702-730); `DistributedStore::wait(vector<Ticket>)` (DSH:472-473, DS:1228-1274) coalesces per place only when waited (DS:1366-1390). | DIFFERS: memory and table entries are O(writes) instead of O(replica sets), and bounded at 4096 (row 3). | Macha `test_durability_batch_retains_exact_nondominated_frontier` (`tests/test_invariants.cpp:2607`) / Dagda none for the frontier; `StoreDurabilityBarrier.AWaitOverManyTicketsAsksEachPeerForOneBarrier` pins the per-place coalescing. |
| 9 | `durability_barrier(batch, FrameType = loader, vector<ObjectId>* unsatisfiable)` -> bool, dsh:426-427, ds:495-772 | one barrier per (node, epoch, domain, backend instance) at the highest generation (ds:504-518); remote barriers launched before the local one (ds:534-562), at the caller's frame (ds:551-553; publication passes its work frame, `filesystem.cpp:1201`; artwork passes speculative, `catalogue.hpp:520-521`, `media_catalogue.cpp:5459`), deadline object_transfer; dead tokens re-derived by one probe per peer, batch re-stamped in place (ds:614-728, log "object durability re-derived after incarnation change reasserted= absent= peers="); returns false at once on any transient failure (ds:766-767); with `unsatisfiable`, definitive losses are listed for re-put (ds:734-738, 768); debug "object durability quorum unavailable ... transient=" (ds:761-764); blocks the caller's thread. | `Node::store().wait(vector<Ticket>, deadline)` DSH:473, DS:1258-1274 (barriers in `run_barriers`, DS:1366-1515, submitted to the workers, DS:1255); `Context::Tickets::wait(Ticket, deadline)` CTXH:388 for one ticket only. | DIFFERS: (a) barrier calls always at `FrameType::loader` (DS:1420, 1459); (b) a wait completes when the ticket settles (DS:283-292), and a transient barrier failure leaves the replica awaiting a barrier (DS:1394-1397), so the wait runs to its deadline instead of answering false at once; a write that left a call in flight (a spilled peer) also holds the wait to its deadline (DS:972-975, 250); (c) no unsatisfiable list: a definitive loss is a `refused` replica, "absent on the peer after its restart" (DS:1478) or "absent here after the backend changed" (DS:1506), for the caller to find; (d) met is judged against the floor (row 3); (e) the deadline bounds each barrier call (DS:1420) where Macha's uses 30 min; (f) the re-derived line is logged per peer with `peers=1` (DS:1450-1451); the unmet line only in the blocking store forms (DS:1265, 1333), never through Context's waits (CTX:425-445); (g) Context has no many-ticket wait, so a batch through Context is one barrier per ticket per place. Coalescing per place and the probe are the same. | Macha `test_durability_domain_group_commits_independent_publications` (:2931), `test_deferred_object_barrier_rejects_stale_process_epoch` (:3206), `test_durability_barrier_across_peer_restarts` (`tests/test_storage_v18.cpp:1082`) / Dagda `StoreDurabilityBarrier.AStaleProcessEpochIsRefused`, `.IndependentPublicationsGroupCommitOverTheWire`, `.AWaitOverManyTicketsAsksEachPeerForOneBarrier`, `.EachPeerIsAskedOnceWhicheverObjectsItHolds`, `.ARestartedPeerIsProbedOnceWithEveryIdThatNamedIt`, `.ALocalBarrierThatFailsIsWarnedOf`, `.AWaitOverManyTicketsPostsItsEvidenceInOrder`, `DistributedStore.ADeferredWritesBarrierAcrossPeerRestarts`. |
| 10 | `get(id, stripe, bool foreground, deadline, cancelled)` / `get(id, stripe, FrameType, deadline, cancelled)` -> optional<Bytes>, dsh:444-449, ds:1704-1718 | copy of the shared buffer; `foreground=false` is speculative (ds:1707); no deadline (`{}`) is legal and common (`filesystem.cpp:490-491, 642-643`, `catalogue.cpp:1553`). | `Node::store().get(id, ReadOptions)` DSH:483, DS:1721-1725; `Context::Objects::read` CTXH:196 (deadline mandatory). | DIFFERS: as row 11 (no retained-memory lease); through Context a deadline must be named. | As row 11. |
| 11 | `get_shared(id, stripe, frame, deadline, cancelled)` -> ObjectData, dsh:450-451, ds:1720-1813: local and cache stages | foreground demand noted before any access (ds:1729-1730); local stage: DATA credit for extent_size under the caller's deadline and cancel, and a `RetainedMemoryLedger` lease of extent_size for the work class held by the returned buffer (ds:1744-1767); cache stage the same (ds:1771-1797), promoting into the pool when this node should own it (ds:1786-1787); abort check before the remote stage (ds:1800-1801); read_ahead remote bytes noted (ds:1803-1804); lines "playback object read source=" at >= 250 ms foreground (ds:1732-1743) and `DIAG object-get` (ds:1760-1762, 1789-1791, 1807-1811). FUSE relies on the memory lease: "Release the previous extent before reserving the next, so a two-buffer handoff cannot hold the viewer headroom" (`filesystem.cpp:126-132`). | `DistributedStore::read_impl` DS:1727-1784 through `Node::store().read(id, ReadOptions)` DSH:481 or `Context::Objects::read` CTX:131-141 (returns `ReadResult{object, source, served_by, answer}`). | DIFFERS: no retained-memory lease on the local, cache or local-copy buffers (DS:94-98 `buffer_of` holds none; the remote buffer holds Dannan's frame `MemoryHold`, DS:90); order, admission, activity, promotion, abort check and lines are the same. | Macha `test_store_runtime_cache_keeps_a_playback_fetch` (:890) / Dagda `DistributedStore.ARuntimeCacheKeepsAPlaybackFetch`, `StoreRepair.AReadsDeadlineIsOneTimeAcrossItsStages`, `StoreRepair.AReadPastItsDeadlineGoesNoFurther`, `StoreRepair.AReadWithNoDeadlineHasItsWholeRemoteLimitAfterALocalWait`, `StoreRepair.AStoreReadWithItsOwnDeadlineGivesUpAtIt`. |
| 12 | `get_remote` shared fetch (no deadline), ds:1520-1702 | one fetch per id; later readers join, raise its frame and promote the in-flight call for any higher-priority frame (ds:1540-1543, 1555-1556) and the replica selector's class to foreground (ds:1544-1551); owners first, then the other ranked nodes (ds:1680-1693); the transfer is cancelled only when no reader waits (ds:1405-1409); opportunistic local copy queued once (ds:1602-1617); "remote object scheduling" on a throw (ds:1694-1700). | `DistributedStore::get_remote` DS:1786-1884, `fetch_next` DS:1886-1948, `fetched` DS:1950-1979, `finish_fetch` DS:1981-2005. | DIFFERS (minor): the in-flight call is promoted only when the joining reader is foreground or read_ahead (DS:1832); a loader joining a speculative fetch raises `fetch->frame` (used for the next candidate) but not the call in flight. Selector order adds Dannan's latency (DS:1912). Otherwise the same. | Macha `test_store_concurrent_readers_share_one_fetch_and_buffer` (:277) / Dagda `DistributedStore.ConcurrentReadersShareOneFetchAndBuffer`, `DistributedStore.AReadNamesTheNodeThatServedIt`. |
| 13 | `get_remote` with a deadline: private transfer, ds:1463-1518 | a reader with a wall deadline does not share; it asks candidates one at a time and aborts its own call at its deadline (ds:1459-1462, 1470-1493). | Same function as row 12: a deadline reader joins or leads the shared fetch and leaves at its deadline (DS:1865-1873); the call is cancelled only when no reader is left (DS:1873, 1881). | DIFFERS: deadline readers are deduplicated with every other reader; each peer call is bounded by `object_transfer_call` and `call_no_progress`, not the reader's deadline. | Dagda `StoreRepair.AStoreReadWithItsOwnDeadlineGivesUpAtIt`. Macha none found. |
| 14 | `get_from(target, ...)`, ds:1351-1439, and `FetchAnswer` (dsh:59) | per peer: `get_object` at `call_deadline::object_transfer`; 404 is `not_found`, anything else `try_later`; the reply is checked against the id and its own hash and slid in place to keep the allocation (ds:35-58); `note_network` for half-extent transfers. | `fetch_next`/`fetched` DS:1886-1979, `take_object_reply` DS:77-92; `ReadResult::answer` (DSH:202). | DIFFERS: each call also has the 300 s `call_no_progress` watch (DS:1945) that Macha's data calls lack (DSH:247-249). Integrity check, 404 rule and in-place slide the same. | Macha `test_store_heals_a_bad_local_copy_from_a_peer` (:853) / Dagda `DistributedStore.HealsABadLocalCopyFromAPeer`. |
| 15 | `held_here(id)`, ds:995-997 | the pool holds it, asks no peer. Caller `catalogue.hpp:523-524`, `media_catalogue.cpp:4573`. | `Context::Objects::has` CTX:143, or `Node::store().held_here` DSH:501. | SAME | Dagda `StoreDistributedStore.PutHereIsLocalFirstAndAPutWhereNoExtentsAreHeld`. |
| 16 | `holds_extents()`, ds:999-1004 | this node's membership record has the hosts-extents bit. | `Node::store().holds_extents()` DSH:516, DS:583 (this node's `extents` advert). | SAME | Dagda `StoreDistributedStore.HostingExtentsChangesWhileRunning`. |
| 17 | `locally_available(id)`, ds:2231-2233 | pool or cache. Caller `src/filesystem/hydration.cpp:695, 800`. | `Node::store().locally_available` DSH:487, DS:587. | SAME | Dagda `StoreDistributedStore.CacheLocalAndHydrateFillTheCache`. |
| 18 | `cache_local(id, span)`, ds:2235-2248 | write-through to the cache under speculative DATA credit; false when no cache, no credit or a throw ("cache write-through skipped"). Callers `filesystem.cpp:390, 429, 1093`. | `Node::store().cache_local` DSH:502, DS:589-598. | SAME (the device reserve slot is off for Macha, node.h:73-75). | Dagda `StoreDistributedStore.CacheLocalAndHydrateFillTheCache`. |
| 19 | `hydration_available()`, ds:2250-2252 | the cache is enabled. | `Node::store().hydration_available` DSH:503, DS:600. | SAME | as row 18. |
| 20 | `hydrate(id, stripe = 0, frame = speculative)`, dsh:464, ds:2254-2265 | true once local or cached, else fetched (shared, no persist) into the cache. Callers pass stripe and frame (`hydration.cpp:621-622, 711`). | `Node::store().hydrate(id, ReadOptions)` DSH:504, DS:602-609. | DIFFERS (cosmetic): the default frame is foreground (`ReadOptions`, DSH:187) instead of speculative; Macha's callers always pass one. | Macha `test_cache_hydrator_fetches_to_persistent_cache` (`tests/test_hydration_catalogue.cpp:388`) / Dagda `StoreDistributedStore.CacheLocalAndHydrateFillTheCache`. |
| 21 | `ensure_local(id, foreground)`, ds:2267-2290 | valid here, else from cache, else fetched, each under DATA credit. Caller `src/service/maintenance.cpp:694`. | `Node::store().ensure_local` DSH:535, DS:2670-2689. | DIFFERS (cosmetic): also notifies the storage event and tells tickets (DS:2680-2681). | Dagda `DistributedStore.HealsABadLocalCopyFromAPeer`. |
| 22 | `has_on(target, id)`, ds:2074-2090 | local index lookup, or `have_object` (a validated read on the peer) as a bounded control call at speculative, false on any failure. | `Node::store().has_on` DSH:523, DS:2072-2082. | SAME. The bound is `control_call` 30 s (DSH:253), stated equal to Macha's `max(dead_after, connect_timeout)` (ds:778); Macha's config defaults for those two were not read (unverified). | Covered through prompt replication tests (row 26). |
| 23 | `replicate_all(id, span, foreground)`, ds:2092-2104 | put on every hosting active node; count of successes; foreground notes foreground bytes (ds:1345-1346). | `Node::store().replicate_all` DSH:529, DS:2636-2647. | DIFFERS (minor): Dagda's `put_on` does not note foreground activity (DS:1177-1199). | none found on either side. |
| 24 | `erase_all(id)`, ds:2297-2321 | delete everywhere unclaimed, cache too; peers by `delete_object` at speculative within 2 min. | `Node::store().erase_all` DSH:532, DS:2649-2668. | SAME | none found. |
| 25 | `should_own(id)`, ds:133-137 | this node is among the first `replication` ranked. | `Node::store().should_own` DSH:517, DS:577-581. | SAME | Dagda `DistributedStore.AnEdgeNodeNeverOwnsAndItsWritesLandOnOwners`. |
| 26 | Prompt replication: `queue_prompt_replication`, `prompt_replication_loop`, ds:1198-1321 | one worker; first owner lacking the object and with room; retries 30/60/120/240 s then drop; at most 4096 retries; the local read takes no admission (ds:1303). | DS:1591-1707. | SAME given `DistributedStoreOptions::prompt_read_admission = false` for Macha (DSH:279-283; default true takes a speculative admission, DS:1684-1687). Retries run on the node's clock; a landed copy also reaches tickets (DS:1694). | Macha `test_store_put_publishes_at_the_floor_and_copies_promptly` / Dagda `StoreDistributedStore.PromptReplicationRetriesAt30_60_120And240SecondsThenGivesUp`, `StoreDistributedStore.PromptReplicationKeepsAtMost4096Retries`. |
| 27 | Opportunistic local copies: `enqueue_local_copy`, `local_writer_loop`, ds:3105-3201 | a fetched object is copied to the cache (or promoted) by one writer, never holding a reader; dropped when a speculative retained-memory lease is not free (ds:3111-3119, "reason=retained_memory") or the 256 MiB queue is full; after a promotion the node re-advertises storage (ds:3197). | DS:2007-2068. | DIFFERS: no retained-memory gate (DS:2007-2022); no advert after a promotion (the server's refresh advertises every 10 s, storage_server.h:44). Queue bound and drop lines otherwise the same. | Dagda `DistributedStore.ARuntimeCacheKeepsAPlaybackFetch`. |
| 28 | `wait_local_copies_settled()`, ds:3141-3146 | waits until every queued copy is written or dropped. | `Node::store().wait_local_copies_settled` DSH:513, DS:2024-2027. | SAME | as row 27. |
| 29 | `prompt_replication_stats()`, ds:1187-1196 | queued, copies, failures. | `Node::store().prompt_replication_stats` DSH:580, DS:1576-1587 (adds skipped_no_room, dropped). | SAME | as row 26. |
| 30 | Activity: `foreground_activity`, `interactive_activity`, `loader_activity`, `take_*_bytes`, `idle_for`, dsh:466-528 | the clocks maintenance gates on (`maintenance.cpp:400-419`, `filesystem.cpp:889-895`). | `Node::store()` DSH:489-495 over the store's clocks. | SAME | Dagda `StoreDistributedStore.ActivityIsObjectWorkInMachasMeaning`. |
| 31 | `estimated_network_bps()` / `note_network`, ds:98-109 | EWMA 0.8/0.2 of transfers of at least half an extent. | DSH:542, DS:2591-2600. | SAME | none found. |
| 32 | `reachability_epoch()`, ds:2227-2229 | `(topology events << 8) + online backends`. | DSH:510, DS:631-633: `(NodeEvents::membership << 8) + online backends`. | DIFFERS (unverified size): the event counted is Dannan's membership count, not Macha's topology event; whether it also moves on adverts (every storage refresh) was not read. | Dagda `StoreDistributedStore.TheReachabilityEpochMovesWithMembershipAndDisks`. |
| 33 | `StorageUnavailable`, dsh:27-30 | thrown by the id forms when nothing landed. | `dagda::store::StorageUnavailable` DSH:56-59, thrown by `Node::store().put(span)` (DS:1128). Context returns evidence (`lost`) instead. | SAME (through Node::store()). | Dagda `DistributedStore.NothingCanTakeTheWrite`. |
| 34 | Handler `get_object`, ss:196-219 | DATA credit waited under the arbiter's window, 503 "stopping" when refused; 404 only with every backend online, else 503 storage_unavailable; served bytes noted as activity. | SS:226-239. | SAME (admission released before encoding the reply, SS:233; Macha holds it to the handler's end). | Dagda `StoreRepair.AServedRequestWaitsForCreditPastAnyLimit`. |
| 35 | Handler `put_object` / `put_object_deferred`, ss:234-273 | 413 past extent size; DATA credit waited; storage event if new; deferred replies the durability token; 507 storage_full; advertises storage. | SS:251-272. | DIFFERS: checks the writer's scope through `Identity::verify` before admission (SS:256, 403 scope_refused); the request may carry a trailing `WritePrincipal` (messages.cpp:91-107), which Dagda's own puts always send (DS:785). | Dagda `StoreScope` suite (not read for this section). |
| 36 | Handler `object_durability_barrier`, ss:285-349 | epoch check, probe answer with fresh tokens, 409 epoch_changed, 500 barrier_failed, batchable barrier. | SS:295-325. | SAME | Dagda `StoreDurabilityBarrier.AStaleProcessEpochIsRefused`, `.ARestartedPeerIsProbedOnceWithEveryIdThatNamedIt`. |
| 37 | Handler `have_object`, ss:82-99 | try-once DATA admission, 503 storage_busy; validated read. | SS:171-181. | DIFFERS: a control-frame request is refused 400 (SS:174). Macha's own `has_on` sends speculative (ds:2082-2083), as Dagda's does (DS:2075). | none found. |
| 38 | Handler `delete_object`, ss:393-412 | claim check (marks the delete) first, 409 claimed, then try-once admission, remove from pool and cache. | SS:281-294. | DIFFERS: admission is tried before the claim check (SS:283-288), so a busy node answers 503 without marking. | Dagda `StoreClaimLocks.ADeleteMarksItsObjectAndHoldsNoStripe`. |
| 39 | Purpose deadlines, `src/cluster/call_deadline.hpp:10-13` | object_transfer 30 min for put, get and barrier; control 2 min for delete; data calls have no no-progress bound. | `object_transfer_call` 30 min (DSH:257) for put and get (DS:851, 1195, 1929); `delete_call` 2 min (DSH:261); barrier calls bounded by the caller's wait deadline (DS:1420); `call_no_progress` 300 s on puts and gets (DSH:250). | DIFFERS: barrier bound and the added no-progress watch, as rows 9 and 14. | Dagda `StoreRepair.AWriteWithNoDeadlineWaitsForAPeerInsideTheWindowPastAnyLimit`. |
| 40 | `ObjectBuffer::retained_memory` (dsh:49-52): the read buffer's memory lease | every buffer a read returns holds a `RetainedMemoryLedger` lease (local, cache: ds:1749-1753, 1776-1780, 1763-1767, 1792-1796; remote: the frame's, ds:56) until the last reader drops it; acquiring it waits under the read's deadline and cancel, so memory pressure backs reads off. | Dagda's `ObjectBuffer` holds only Dannan's frame `MemoryHold` (DSH:174-177); local and cache buffers hold none (DS:94-98). Dannan has the ledger (`dannan::sched::RetainedMemoryLedger`, docs/macha-foundations-map.md:147) but the store does not use it; the journal map records `reason=retained_memory` as "a gate Dagda has not" (TODO/2026-11-10-dagda-capabilities.md:441). | MISSING | Macha `test_store_concurrent_readers_share_one_fetch_and_buffer` (asserts the held bytes, `tests/test_rpc_cluster.cpp:313`) / Dagda none. |
| 41 | `FetchAnswer` (dsh:59) | ok / not_found / try_later, internal to repair and reads. | `FetchAnswer` DSH:183, returned in `ReadResult::answer`. | SAME | Dagda `DistributedStore.AReadNamesTheNodeThatServedIt`. |

Counts: SAME 17 (rows 15, 16, 17, 18, 19, 22, 24, 25, 26, 28, 29, 30, 31, 33, 34, 36, 41); DIFFERS 23 (rows 1-14, 20, 21, 23, 27, 32, 35, 37, 38, 39); MISSING 1 (row 40).

## 2. Performance-relevant differences

1. **The Context routes run every read and write on four workers.** `Context::Objects::read/write`
   (CTX:116-141) call the asynchronous store forms, which `submit` the whole `read_impl` or
   `run_write` to `_workers` (DS:1108-1112, 1714-1718), `workers = 4` (DSH:272); the blocking Context
   forms are the asynchronous ones awaited (CTX:33-43). A read waiting on a remote fetch, a write
   waiting on a peer (up to `call_no_progress` 300 s without a caller deadline), a barrier job
   (DS:1225, 1255) with its local syncfs (DS:1493) and a catch-up (DS:1552) all hold a worker. Macha
   runs `get_shared` and `put_impl` on the caller's own thread (FUSE readers, the extent task pool,
   `filesystem.cpp:423-434`), unbounded by the store. Through `Node::store()` the blocking forms run on
   the caller's thread as Macha's do.
2. **A Context deadline starts when a worker picks the job up**, not at the call (`until` set in
   `read_impl`, DS:1750; `deadline` in `run_write`, DS:740); the blocking form gives up at
   deadline + 5 s with `runtime_error` (CTX:40-41) while the job may still run.
3. **Barrier waits run to the deadline on a transient failure or an in-flight call** (row 9 (b)):
   Macha's barrier answers false as soon as its calls return (ds:766-767) and the commit fails fast
   (`filesystem.cpp:1221-1226`); Dagda's wait completes only when the ticket settles or the timer
   fires (DS:283-292, 1296, 1310-1320).
4. **Barriers at loader priority always** (DS:1420, 1459): artwork barriers that Macha sends at
   speculative (`catalogue.hpp:520-521`) compete with publication at loader.
5. **Tickets cost per write.** Each write allocates a TicketRecord, takes `_shared->mutex`, computes
   the replication set over the roster in addition to `ranked()` (DS:745-746) and builds evidence
   vectors and strings (DS:966-1024, 1053-1057). Past `max_tickets`, each new write scans the table
   from the oldest, locking each ticket and building its evidence until it finds a settled one
   (DS:711-726): with 4096 unsettled deferred tickets that is 4096 evidence builds under the table
   lock per write. `note_acknowledged` and `on_roster` walk the whole table (DS:1517-1557). A wait
   over N tickets makes N strands and N steady timers (DS:1295-1296). Macha's batch frontier keeps
   one requirement per replica set (ds:84-91).
6. **Copies.** The blocking `Context::Objects::write` copies the caller's span into `Bytes`
   (CTX:127): one extra full extent copy per write. Each remote put passes the encoded payload by value
   to `Routes::call` (DS:850-851; `dannan/context.h:216` takes `net::Bytes payload`), one copy per
   peer; Macha passes a span (`src/cluster/cluster.hpp:241`); whether Macha's transport copies it
   later was not read (unverified).
7. **Hashing.** A write through Context with a known id hashes twice (CTX:120 and DS:685); Macha's
   `put(id, ...)` once (ds:200). `put(span)` hashes twice on both sides (ds:146 and 200; DS:1124 and
   685).
8. **Scope checks.** Every local copy (DS:807, 1161, 1180) and every served put (SS:256) calls
   `Identity::verify`; Macha does neither. Its cost was not read (unverified).
9. **No memory backpressure on reads or local copies** (rows 27, 40): Macha's reads wait for
   retained-memory headroom per work class and its local-copy queue drops without it; Dagda's do
   neither, so on a memory-tight node (the Pis) foreground reads are not protected by a viewer
   reserve.
10. **In-flight promotion** only for foreground and read_ahead joiners (DS:1832), not for a loader
   joining a speculative fetch (ds:1540-1543).
11. **Better than Macha**: the write loop waits on a condition variable (DS:961) instead of 1 ms
   polling (ds:471-472); `get_object` releases its admission before encoding (SS:233); data calls have
   a 300 s no-progress bound (DS:852, 1945); deadline readers share fetches (row 13).

## 3. Gaps for Dagda to fix

1. **Deferred tickets are bounded at 4096 and forgotten oldest-first while still unsettled.** Macha
   needs an unbounded publication batch: at production extent_size 4M (`fi-1.yaml:111`) one 16 GiB
   file is 4096 deferred extents before its commit barrier (`filesystem.cpp:380-381, 1198-1201`).
   Dagda forgets the oldest unsettled ticket (DS:711-726) and a later wait reports it unknown
   (DS:1283-1289), and each write past the bound scans the table under its lock. Test:
   `StoreDurabilityBarrier.APublicationLargerThanTheTicketTableStillBarriersEveryExtent`. Severity:
   blocks the port.
2. **A wait answers on "settled", not when the barriers have answered.** Macha's barrier returns false
   at once on a transient failure and true when the replicas that landed are durable, ignoring a peer
   that was spilled past. Dagda holds the wait to its deadline when a barrier failed transiently
   (DS:1394-1397) or a spilled call is still in flight (DS:972-975). Test:
   `StoreDurabilityBarrier.ATransientBarrierFailureAnswersAtOnceNotAtTheDeadline` (and
   `.ASpilledPeerStillInFlightDoesNotHoldTheBarrier`). Severity: loses performance.
3. **Barriers ignore the caller's frame.** Macha passes the work's frame (publication) or speculative
   (artwork); Dagda sends loader (DS:1420, 1459). Test: `StoreDurabilityBarrier.ABarrierTravelsAtTheFrameItWasAskedAt`.
   Severity: loses performance.
4. **What a write that ended short must reach.** Macha's batch requires what landed when a put ended
   short or its budget ended it (ds:375, 475); Dagda's ticket keeps the floor (DS:969), so the wait is
   never met. Test: `StoreDurabilityBarrier.AWriteThatEndedShortIsMetAtWhatLanded`. Severity: blocks
   the port for write_copies > 1; none at production write_copies 1.
5. **Cancellation after a copy landed.** Macha's put fails (false, or throws "object replication
   cancelled") whenever the caller cancelled (ds:378-384, 147-149); Dagda reports success when any copy
   landed (DS:1126-1129, 1138). Test: `DistributedStore.ACancelledPutFailsEvenWhenACopyLanded`.
   Severity: blocks the port (a caller-visible result differs: EINTR paths in `filesystem.cpp:384-387,
   1087-1089`, the torrent abort in `torrent_manager.cpp:1279-1284`).
6. **No retained-memory gate on read buffers and local copies.** Macha's buffers hold a
   `RetainedMemoryLedger` lease that a read waits for (ds:1749-1753, 1776-1780) and its local-copy
   queue drops without one (ds:3111-3119); Dagda has neither (DS:94-98, 2007-2022). Test:
   `DistributedStore.AReadWaitsForObjectMemoryAndHoldsItUntilTheLastReaderLetsGo`. Severity: loses
   performance.
7. **Context caps object work at the store's four workers.** Macha's reads and writes run on its
   callers' threads; Context's run on `workers` (DS:1108-1112, 1714-1718), with barriers and catch-ups
   in the same queue. Test: `Context.ConcurrentReadsAreNotCappedByTheStoresWorkers`. Severity: blocks
   the port if Macha's reads and writes go through Context (none through `Node::store()`).
8. **A Context deadline starts at the worker, not at the call** (DS:740, 1750). Test:
   `Context.AReadQueuedBehindBusyWorkersStillAnswersByItsDeadline`. Severity: loses performance.
9. **Context cannot express "no deadline".** Macha's FUSE reads, its publication puts and the
   catalogue's reads pass none (`filesystem.cpp:490-491, 642-643`; `catalogue.cpp:1553`) and wait under
   the arbiter's window; Context refuses a non-positive deadline (CTX:29-31) and a deadline turns the
   admission wait into that deadline (DS:1752, 812-816). Test:
   `Context.AReadWithNoDeadlineWaitsUnderTheArbitersWindow`. Severity: blocks the port if through
   Context.
10. **Context lacks Macha's store forms**: a write with a known id, `put_here`, `put_deferred_here`,
   the many-ticket wait, `get` returning a plain buffer with no deadline, `hydrate`, `cache_local`,
   `locally_available`, `ensure_local`, `replicate_all`, `erase_all`, the activity clocks,
   `estimated_network_bps`, `wait_local_copies_settled`, `prompt_replication_stats`,
   `reachability_epoch`, `holds_extents`, `should_own`, `has_on`. Each exists only on `Node::store()`,
   documented as for maintenance and tests (node.h:359-360). Test:
   `ContractGates.MachasStoreFormsAreReachableThroughTheContract`. Severity: blocks the port until the maintainer
   rules which route Macha uses.
11. **Extra copies on the write path**: the blocking Context write copies its span (CTX:127) and each
   remote put copies the payload into `Routes::call` (DS:850-851). Test:
   `DistributedStore.AWriteCopiesItsBytesNoMoreThanOnceForThePeers` (counting allocations). Severity:
   loses performance.
12. **In-flight promotion for a loader joining a speculative fetch** (DS:1832 vs ds:1540-1556). Test:
   `DistributedStore.ALoaderJoiningASpeculativeFetchRaisesTheCallInFlight`. Severity: loses
   performance.
13. **A promoted local copy is not advertised at once** (ds:3197 vs DS:2056). Test:
   `DistributedStore.APromotedLocalCopyIsAdvertisedAtOnce`. Severity: cosmetic.
14. **Log lines**: the unmet-barrier line is never written through Context's waits (DS:1265, 1333 vs
   CTX:425-445); the re-derived line is per peer with `peers=1` (DS:1450-1451) where Macha's is one line
   with the peer count (ds:725-727); the quorum line's `result=` differs (DS:1049 vs ds:288). Test:
   `StoreDurabilityBarrier.AnUnmetBarrierThroughTheContractIsLoggedAsMachaLogsIt`. Severity: cosmetic.
   **Done 2026-11-15** (`6adc5ed`; tests `b54784a`, with `.ARoundThatProbesTwoPeersSaysSoOnOneLine` and
   `.ASlowWriteThatEndedShortIsLoggedAsLanded`): the barrier round writes both lines before its waits answer.
15. **`replicate_all` at foreground does not note foreground bytes** (ds:1345-1346 vs DS:1177-1199).
   Test: `DistributedStore.AForegroundReplicateAllIsForegroundActivity`. Severity: cosmetic.

## 4. Not mapped

- Claims: `retain_data`, `retain_data_here`, `retain_control`, `retain_on`, `ClaimScope`,
  `select_present_batched`, `batched_have_objects`, `bounded_control_call` beyond its use by `has_on`,
  and the handlers `retain_objects`, `have_objects`, `have_control_objects`. Another section's.
- CONTROL objects: `offer_control`, `offer_control_on`, `ensure_control_local`, handlers
  `get_control_object`, `put_control_object`.
- Repair and scrub: `repair_step`, `repair_once`, `scrub_once`, `repair_diagnostics`,
  `note_repair_gate`, `validated_presence`, handler `have_valid_objects`, Dagda's `valid_on`,
  `trie_diff`, `remove_objects`, `durability_covered` (served only, SS:326-330; the store never asks it).
- The storage server's `refresh`/`advertise` and its refresher thread.
- The pool beneath both stores (`StoragePool::put/put_deferred/get/durability_barrier/reassert_durable`),
  the arbiter (`DataResourceArbiter`) and the device reserve: read only as far as `admit_data`
  (data_work.cpp:364-380).
- `ReplicaSelector::order` (Dagda passes a latency function, DS:1912) and placement
  (`capacity_placement_nodes`, ds:123, vs `nodes().placement`, DS:568): not compared line by line.
- Transport: Dannan's server worker count and whether Macha's `call_async` copies its span; the cost
  of `Identity::verify`; Macha's `dead_after`/`connect_timeout` defaults behind `bounded_control_call`.
- Wire compatibility between a Macha node and a Dagda node (message type numbers; a Macha server
  would refuse Dagda's trailing `WritePrincipal`, `reader.finish()` at ss:241): not checked, as the
  cutover is whole-cluster.
- Whether Dannan's `NodeEvents::membership` count moves on adverts (row 32).
- Hedged reads: neither side has any (no match for "hedg" in Macha's `src/` or in Dagda).
- Macha's own test bodies were read only for the four cited in rows 1, 3, 12 and 40; the other Macha
  test names are from their declarations.
