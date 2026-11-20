# Macha's foundations on Dannan

Step 1a, 2026-11-04. For each foundation Macha's local store uses, where it
lives once the store runs on Dannan 0.2.0. Macha is read at `develop`
(`5a06d3c`, `git show develop:src/...`); Dannan at the `0.2.0` tag
(`build/deps/dannan-0.2.0`). "Dagda's own" means store-specific and moved
here; "missing in Dannan (request)" means generic and asked of the Dannan
session, never written here.

## Byte compatibility (what the formats depend on)

Every on-disk and wire byte the store writes through a foundation was
checked by reading both implementations side by side, and the crypto ones
are pinned by known-answer tests computed outside both codebases
(`dagda/test/domain_storage_key_test.cpp`, `store_sealing_test.cpp`).

| What | Macha | Dannan 0.2.0 | Verdict |
|---|---|---|---|
| Integers | `codec.cpp:5-24`: big-endian u8/u16/u32/u64, i64 as u64 | `net/codec.h:38-49`: the same loops | **Identical** |
| Blob, string | `codec.cpp:30-39`: u32 length then bytes | `net/codec.h:52-57`: the same | **Identical** bytes. Only difference: over 2^32-1 bytes Macha throws `std::runtime_error`, Dannan `std::length_error`. No store record reaches 4 GiB (pack payloads stop at 128 MiB). |
| Fixed arrays | `codec.hpp:31-33`: raw | `net/codec.h:58-61`: raw | **Identical** |
| Reader | `codec.cpp:41-112`: `truncated input`, `blob too large` (max checked before copy), `trailing input`, defaults 256 MiB and 65536 | `net/codec.cpp:12-72`: the same checks, messages and defaults | **Identical**. Dannan deletes `Reader(Bytes&&)` (compile-time only). |
| SHA-256 / object ids | `crypto.cpp:23-66`: EVP SHA-256; `object_id = sha256` | `identity/crypto.cpp:33-64` | **Identical**. `Hash256` is the same 32-byte array with defaulted `<=>`, so id order (sets, listings, the trie) is the same. |
| HKDF-SHA256 | `crypto.cpp:86-102`: EVP_PKEY HKDF, salt set only if non-empty, info added only if non-empty, 32 bytes | `identity/crypto.cpp:76-99`: the same calls and the same conditions | **Identical**. "No salt" is OpenSSL's default (a zero salt of hash length, RFC 5869) in both. |
| Cluster master | `crypto.cpp:103-113`: SHA-256 of the key file, trailing CR/LF removed | `identity/cluster_key.cpp:23-33`: the same | **Identical** |
| Storage key | `crypto.cpp:115`: `HKDF(master, no salt, "macha/storage/v1")` | `ClusterKey::derive(label)` / `hkdf_sha256_secret` with Macha's label | **Identical** under Macha's label. Pinned: key file `dagda known-answer cluster key\r\n` gives storage key `879277d5...79bbd8`. |
| Auth key, cluster id | `crypto.cpp:114`, `:116-117`: labels `macha/auth/v1`, `macha/cluster-id/v1` | `cluster_key.cpp:34-36`: labels `dannan/auth/1`, `dannan/cluster-id/1` | **Differ** (pinned in `DannansOwnLabelsAreNotMachas`). Neither is a store byte: they are the wire handshake's, which is Dannan's. Nothing on disk in layout v18 depends on them that the store reads. |
| AES-256-GCM seal | `crypto.cpp:121-154`: random 12-byte nonce (RAND_bytes), IVLEN 12, AAD only if non-empty, ciphertext the plaintext's size, 16-byte tag | `identity/crypto.cpp:101-132`: the same sequence; a fixed-nonce overload for reproduction | **Identical**. `SealedData` is the same `{nonce 12, tag 16, ciphertext}`. The order on disk (nonce, then tag, then ciphertext) is the store's own framing (`local_store.cpp:1092-1096`, the pack header at 65/77/125) and moves with LocalStore. Pinned against GCM spec test case 16 and a Macha-shaped object (`OpensWhatMachaSealed`, `SealsMachasBytesUnderTheSameNonce`). |
| AES-256-GCM open | `crypto.cpp:155-186`: tag set before final, `AES-GCM authentication failed` | `identity/crypto.cpp:138-169`: the same | **Identical**. Dannan also offers `aes_gcm_open_secure` (plaintext in secure memory). Dannan refuses buffers over `INT32_MAX` with an exception; Macha would truncate the `int` silently. No object reaches 2 GiB. |
| Object AAD | `local_store.cpp:830`, `:910`, `:1091`, `:1257`, `:1337`: AAD = the 32 id bytes; open checks size then `object_id(plain) == id` | `dagda/store/sealing.{h,cpp}` | **Identical** (moved). |
| hex | `types.cpp:16-24`: lowercase | `identity/crypto.cpp:216-225`: lowercase | **Identical** output. `unhex` accepts the same inputs (both cases, even length). |
| Ticket on the wire | `storage_server.cpp:247-252`: epoch 16, domain, generation, instance u64 | `dagda/store/durability_ticket.{h,cpp}` on Dannan's Writer | **Identical** (moved, pinned in `EncodesAsProtocol23`). |
| Durable replace | `durable_file.cpp:38-79`: temp `<path>.tmp.<pid>.<unix_ms>`, 0600, fsync, rename, fsync directory; missing directories at the umask's mode | `identity/durable_file.cpp:96-107`: temp `<path>.tmp.<pid>.<seq>`, 0600, the same sequence; missing directories made **0700** | **Same file contents and crash guarantees.** Differs in the temporary's name and the mode of directories it creates. No v18 reader matches these temporaries by name (LocalStore's own temporaries are its own). Directories Dannan creates are owner-only where Macha's were not: deliberate on Dannan's side, worth knowing for an operator's backup tooling. |

Conclusion: **no store format depends on anything Dannan encodes
differently.** Macha's codec is Dannan's codec byte for byte, so it is not
store-specific and Dagda writes none of its own; the crypto primitives are
the same OpenSSL calls in the same order. The only store-specific crypto
is the label `macha/storage/v1` and the AAD rule, both in Dagda.

## Symbol by symbol

### `src/types.hpp`

| Macha | Where it goes |
|---|---|
| `Bytes` | `dannan::identity::Bytes` / `dannan::net::Bytes` (`std::vector<uint8_t>`) |
| `Clock` | `dannan::contract::Clock` (`contract/time_source.h`) |
| `NodeId`, `NodeIdHash`, `to_string(NodeId)` | `dannan::identity::NodeId`, `NodeIdHash`, `to_string` (`identity/node_id.h`): same 16 bytes, same order |
| `Hash256` | `dannan::identity::Hash256` (`identity/crypto.h`) |
| `ObjectId` | Dagda's own name for Dannan's `Hash256`: `dagda::store::ObjectId` (`dagda/store/object_id.h`) |
| `to_string(ObjectId)` | `dagda::store::to_string` over `dannan::encoding::hex` |
| `hex`, `unhex` | `dannan::encoding::hex`, `unhex` (`dannan/encoding/base.h`, Dannan 0.10.0) |
| `unix_ms()` | `dannan::contract::WallClock::unix_ms()` (`contract/time_source.h`), injected |
| `Endpoint`, `NodeInfo`, node flags, `IdentityAssociationReset` and its keys | Dannan's membership (`membership/node_record.h`, `membership/identity_reset.h`); not the store's |
| `SessionIdentity` | Macha's (HTTP sessions); not the store's |

### `src/crypto.hpp`

| Macha | Where it goes |
|---|---|
| `ClusterKeys`, `load_cluster_keys` | `dannan::identity::ClusterKey`, `load_cluster_key` (`identity/cluster_key.h`); `.storage` becomes `dagda::domain::StorageKey::derive_v1` (`dagda/domain/storage_key.h`), held in Dannan's `Secret` |
| `SealedData` | `dannan::identity::SealedData` |
| `Sha256Hasher`, `sha256`, `hmac_sha256`, `hkdf_sha256` | `dannan::identity::*` (`identity/crypto.h`); `hkdf_sha256_secret` for keys |
| `object_id` | `dagda::store::object_id` (Dagda's own, over Dannan's `sha256`) |
| `aes_gcm_seal`, `aes_gcm_open` | `dannan::identity::*`; the object rule (id as AAD, size and id checked) is `dagda::store::seal_object` / `open_object` |
| `random_bytes`, `random_node_id`, `x25519_*`, `constant_time_equal` | `dannan::identity::random_bytes`, `NodeId::random`, `x25519_*`, `constant_time_equal` |

### `src/codec.hpp`

| Macha | Where it goes |
|---|---|
| `Writer`, `Reader`, `DecodeError` | `dannan::net::Writer` (= `BasicWriter<Bytes>`), `Reader`, `DecodeError` (`net/codec.h`): byte-identical (above) |
| `encode_node_info`, `decode_node_info` | Dannan's membership records; not the store's |

### `src/log.hpp`

| Macha | Where it goes |
|---|---|
| `LogLevel`, `log_level_name`, `parse_log_level` | `dannan::status::*` (`status/log.h`) |
| `Logger`, `ConsoleLogger` | `dannan::status::LogSink`, `ConsoleSink` / `JsonSink` |
| `Log` (process-wide facade) | none: each component takes a `dannan::status::ScopedLogger` (from `Context::logger`). Dagda has no global logger. |

### `src/durable_file.hpp`

| Macha | Where it goes |
|---|---|
| `durable_replace_file` | `dannan::identity::durable_replace_file` (`identity/durable_file.h`); also `durable_create_file` |

### `src/contract/thread_safety.hpp`

| Macha | Where it goes |
|---|---|
| `MACHA_*` annotations | `DANNAN_*` (`contract/thread_safety.h`), the same set |
| `NoIo`, `no_io`, `NoIoRegion`, `Mutex`, `IoMutex`, `SharedMutex`, `IoSharedMutex`, `Lock`, `WriteLock`, `ReadLock` | `dannan::contract::*`, the same semantics |
| `ObjectLock` (per-object `IoMutex` held across a writer's I/O) | **Dagda's own** (decided in 1a's review: store-only): `dagda/store/object_lock.h` (`ObjectLock`, `ObjectLocks`), 1c |
| `Unlocked` (file-local in `local_store.cpp`: releases a held `Mutex` for a scope) | **missing in Dannan (request)**, generic; file-local in `dagda/store/local_store.cpp` until it lands, as in Macha (1c) |

### `src/contract/time_source.hpp`

| Macha | Where it goes |
|---|---|
| `TimeSource`, `SteadyTimeSource`, `steady_time_source` | `dannan::contract::*` (`contract/time_source.h`); Dannan drops the `now_waits`/`now_safety` constants and states them in the comment |
| tests' `stepped_time.hpp` | `dannan::test::SteppedTime`, `SteppedWall` (`test/fixtures/stepped_time.h`) |

### `src/contract/work.hpp`

| Macha | Where it goes |
|---|---|
| `Waits`, `includes`, `local_waits`, `default_waits`, `may_enter`, `ThreadSafety`, `WorkContext` | `dannan::sched::*` (`sched/work.h`), the same values. `WorkContext::expired(now)` takes the injected time, no default |
| `WorkClass` (`control, viewer, loader, speculative`), `FrameType` | `dannan::sched::WorkClass` (`control, foreground, loader, speculative`), `FrameType` (wire values unchanged). Macha's "viewer" is Dannan's "foreground" |
| `WaitGuard` (static, process-wide) | `dannan::sched::WaitGuard`, one per node, injected |

### `src/observation.hpp`

| Macha | Where it goes |
|---|---|
| `observation_bucket*`, `HistogramSnapshot`, `LatencyHistogram`, `ObservationEvent`, `ObservationSnapshot`, `Observations`, `ObservationLog`, `ObservationRecorder`, `render_*`, `observation_route_label` | `dannan::status::bucket_of` / `bucket_lower` / `bucket_upper`, `HistogramSnapshot`, `Histogram`, `ObservationEvent`, `ObservationSnapshot`, `Observations`, `ObservationLog`, `ObservationRecorder`, `render_window`, `render_event`, `route_label` (`status/observation.h`) |
| `observations()` (process registry) | none: one `Observations` per node, injected (the durability domain takes it) |
| `elapsed_us`, `ObservedDuration` | **missing in Dannan (request)**, small: the domain computes its microseconds inline for now |

### `src/diagnostics.hpp`

| Macha | Where it goes |
|---|---|
| `set_thread_name` | **missing in Dannan (request)**. Macha names the domain's worker `macha-durable`; Dagda's worker is unnamed until Dannan has it |
| `process_resident_bytes` | Dannan has it privately (`status/telemetry.cpp:100`, `resident_bytes`); **request** to export it |
| `thread_cpu_time_ns`, `ThreadCpuReporter`, `DiagnosticLock`, `TimedLock`, `elapsed_ms` | **missing in Dannan (request)**. 1d: the storage pool's `TimedLock` (`storage_pool.cpp:88`, `:300`) is a plain `Lock` with a `TODO(Dannan: TimedLock)` beside it (`dagda/store/storage_pool.h`) |

### `src/supervised.hpp`

| Macha | Where it goes |
|---|---|
| `run_supervised_loop`, `run_supervised_once`, `run_supervised_escalating`, `SupervisedThreadStatus`, `supervised_thread_statuses`, `supervised_restart_delay` | `dannan::supervise::SupervisedThreads::loop` / `once` / `escalating` / `statuses` / `restart_delay` (`supervise/supervised.h`), one per node, injected |

### `src/startup_progress.hpp`

| Macha | Where it goes |
|---|---|
| `note_startup_progress`, `startup_progress` (process counter) | `dannan::supervise::StartupProgress` (`supervise/startup.h`), one per node, injected |
| `StartupStallGate` | `dannan::supervise::StartupStallGate` |

### `src/retained_memory.hpp`

| Macha | Where it goes |
|---|---|
| `RetainedMemoryLedger`, `Lease`, `RetainedMemoryStats` | `dannan::sched::RetainedMemoryLedger` (`sched/retained_memory.h`); "viewer" reserve is "foreground" |
| `MemoryOwner` (closed enum incl. `object_payload`, `durability`, `cache`, ...) | Dannan's open `MemoryOwner`: Dannan names `rpc_frame`; **Dagda's own** owners (`object_payload`, `durability`, `cache`) are numbered from `MemoryOwner::hosted_first` when 1c/1d need them |

### `src/write_behind.hpp`

| Macha | Where it goes |
|---|---|
| `WriteBehind`, `start_writeback` | **Dagda's own** (decided): `dagda/store/write_behind.{h,cpp}`, moved with LocalStore in 1c. Macha's FS spool will link it from Dagda |

### `src/config.hpp` (the parts storage reads)

| Macha | Where it goes |
|---|---|
| `StorageBackendConfig`, `StoragePackingConfig`, `MetadataObjectStoreConfig`, `CacheConfig`, `ledger_cache`, `write_copies`, `metadata_write_copies`, `replication`, `MaintenanceConfig::garbage_grace` | **Dagda's own**, moved in 1c and 1d as plain option structs handed to the store (no YAML in Dagda; Macha's config parser keeps reading them and passes them in). 1c: `dagda::store::LocalStoreOptions` (limit, reserve, packing threshold and target with Macha's 1 MiB and 64 MiB defaults, ledger directory and cache) |
| durability batch window (`StoragePool` argument, default 500 ms) | a constructor argument of `dagda::store::DurabilityDomain` |

## The store pieces moved in 1a

| Macha | Dagda |
|---|---|
| `ObjectId`, `object_id` | `dagda/store/object_id.{h,cpp}` |
| `ClusterKeys::storage` | `dagda/domain/storage_key.{h,cpp}` (`StorageKey::derive_v1`) |
| the seal and open at `local_store.cpp:830`, `:910-911`, `:1091`, `:1257-1258`, `:1337-1338` | `dagda/store/sealing.{h,cpp}` (`seal_object`, `open_object`, `ObjectIntegrityError`) |
| `storage/presence_index.{hpp,cpp}` | `dagda/store/presence_index.{h,cpp}`, unchanged |
| `storage/durability_domain.{hpp,cpp}` | `dagda/store/durability_domain.{h,cpp}`: takes `SupervisedThreads`, `Observations` and a `TimeSource` instead of process globals; the batch window runs on the injected time (`time_advanced()` wakes the worker for a stepped source); the destructor's stop can no longer be missed (stop-token waits); constructor errors are `std::invalid_argument` |
| `StoragePool::DurabilityToken` plus the server's epoch | `dagda/store/durability_ticket.{h,cpp}` (`DurabilityTicket`, its protocol-23 form) |

## The store pieces moved in 1c

| Macha | Dagda |
|---|---|
| `storage/local_store.{hpp,cpp}`: `LocalStore`, `LocalStoreOptions`, `LocalStoreMode`, `LocalStoreDiagnostics`, `StorageLock` | `dagda/store/local_store.{h,cpp}`: takes the domain's `StorageKey`, Dannan's `SupervisedThreads`, `Observations`, `StartupProgress`, a `ScopedLogger`, a `TimeSource` (its default domain's window) and a `WallClock` (record times and ages) instead of process globals; `own_syncs` added to the diagnostics |
| the formats inside `local_store.cpp` (loose header, pack header, accounting slot, pack names) | `dagda/store/local_format.{h,cpp}`, pure and pinned by known answers (`store_local_format_test.cpp`) |
| `LocalStoreFiles`, `PosixLocalStoreFiles` | `dagda/store/local_store_files.{h,cpp}` |
| `ObjectLock` and `LocalStore::object_mutex` | `dagda/store/object_lock.h` |
| `write_behind.{hpp,cpp}` | `dagda/store/write_behind.{h,cpp}` |
| `contract/object_store.hpp` (`HeldIdentity`, `HeldView`, `ObjectStore`) | `dagda/contract/object_store.h` (`dagda::contract`) |
| `load_or_create_node_id` | not moved: the node id is Dannan's (`identity/node_identity.h`) |

## The store pieces moved in 1d

| Macha | Dagda |
|---|---|
| `storage/storage_pool.{hpp,cpp}` (`StoragePool`) | `dagda/store/storage_pool.{h,cpp}`: takes the node id and the process's durability epoch, the domain's `StorageKey` and `LocalStore::Services` instead of process globals; `put_deferred` returns 1a's `DurabilityTicket` (Macha's token plus the epoch its server added), and a ticket from another epoch throws `StaleDurabilityTicket`; `StorageBackendConfig`, `StoragePackingConfig`, `ledger_cache` become `StorageBackendOptions` and `StoragePoolOptions`; `is_retained` is the named seam `ClaimCheck`; `degraded()` added (section 7) |
| `cluster/placement.cpp` `capacity_placement_nodes` as the pool calls it (one copy) | Dannan's `placement::by_capacity` under the store's `data_scheme()` (`dagda/store/placement.{h,cpp}`): Macha's salts `macha/data-fallback/v7` and `macha/data-extra-domain/v7`, capacity in the advert `dagda/capacity`. For one copy both compute SHA-256(the id's first four bytes, salt, token) and sort by -ln(u)/capacity, ties to the lower id: identical, pinned by known answers (`RanksBackendsAsMachaDoes`) |
| `storage/io_pressure.hpp` (`DiskServiceMonitor`, `DiskServiceTimer`) | `dagda/store/io_pressure.{h,cpp}`: the monitor logs through its own `ScopedLogger`; the timer reads the injected `TimeSource` |
| `storage/unreferenced_since.{hpp,cpp}` | `dagda/ledger/unreferenced_since.{h,cpp}` (`dagda::ledger`: requirement L22 is the ledger's, and step 2's sweeps use it); the file's bytes pinned (`TheFileIsLayoutV18s`) |
| `contract/id_lookup.hpp` (`IdLookup`) | `dagda/contract/id_lookup.h` (`dagda::contract`) |
| `cluster/local_state.cpp:134-142` (the control store) | `dagda/store/control_store.{h,cpp}`: `ControlStoreOptions` (Macha's `metadata_store`), `open_control_store`; `ledger_cache_share` (`local_state.cpp:29-50`) in `dagda/store/local_store.h` |
| `storage/persistent_cache.{hpp,cpp}` without the metadata seed | `dagda/store/persistent_cache.{h,cpp}`: `CacheOptions` (Macha's `CacheConfig`, `prefer_metadata` named `seed`); the seed's reading and writing (`remember_metadata`, `metadata`, `DHTCMET1`) stay for step 4 behind `with_seed` |


## The pieces moved in step 2

| Macha | Dagda |
|---|---|
| `contract/walk.hpp` (`Budget`, `Cursor`, `Page`, `Stop`, `YieldSource`) | Dannan's `sched/walk.h`; `Budget::must_stop` takes the injected time |
| `contract/published.hpp` (`Published`) | Dannan's `contract/published.h` |
| `cluster/node_identity.cpp:15-40` (the state layout marker) | `dagda/store/state_layout.{h,cpp}` (`open_state_layout`), Dagda's |
| `contract/claim_store.hpp`, `contract/object_ledger.hpp` | `dagda/contract/claim_store.h`, `object_ledger.h` |
| `storage/retention.{hpp,cpp}` (`RetentionStore`) | `dagda/ledger/retention.{h,cpp}`; the MRTS0001 and MRTS0002 readers dropped, their names reserved |
| `ledger/retention_ledger.{hpp,cpp}`, `ledger/reference_counts.{hpp,cpp}` | `dagda/ledger/`, the same names |
| `contract/horizon.{hpp,cpp}`, `contract/gates.{hpp,cpp}` | `dagda/contract/`; `GarbageRef` and `TombstoneBatch` become the provider's `Garbage` and `GarbageBatch`, `catalogue_complete()` is `complete()` |
| `ledger/node_horizon_builder.{hpp,cpp}`, `contract/horizon_builder.hpp` | `dagda/ledger/node_horizon_builder.{h,cpp}` over `contract::ReachabilityProvider` (`dagda/contract/reachability.h`) |
| `service/claim_walk.{hpp,cpp}` | `dagda/ledger/claim_walk.{h,cpp}` |
| `service/maintenance.cpp:620-660`, `:866-1071`; `CatalogueManager::control_gc_step` (`catalogue.cpp:2355-2433`) | `dagda/ledger/collector.{h,cpp}` (`Collector`); the catalogue's root epoch is the provider's `ControlEpoch` |

## The pieces moved in step 3b

| Macha | Dagda |
|---|---|
| `cluster/data_work.hpp` (`DataWorkContext`, `DataResourceArbiter`, `DataResourceStats`) | `dagda/store/data_work.{h,cpp}`: "viewer" is Dannan's foreground class; the work context is Dannan's `sched::WorkContext` with a deadline on the injected time; every wait has a deadline (the no-progress window may not be zero; a caller with no deadline of its own waits under it, as Macha's does); the abandoned-wait warning, Macha's line word for word, goes to an optional scoped logger, outside the lock. New beside it: `IoReserve`, the device I/O reserve for control work (section 7 item 4), and `admit_data` / `admit_control` |
| `fuse/fuse_frontend.hpp` `WeightedLoaderService` (repair's share, `service/maintenance.cpp:309-313`) | **missing in Dannan (request)**: generic scheduling. File-local in `dagda/store/repair.cpp` (`Repair::Share`) with a `TODO(Dannan: ...)` until Dannan's `sched/` has one |
| `NodeRuntime::peer_viewers_active` (`cluster/cluster.cpp:717`) | Dannan's telemetry (`Context::telemetry().views`, per-class traffic), read in `Repair` |
| `ledger/availability.{hpp,cpp}` (`AvailabilitySurvey`, `PeerDiff`, `survey_by_diff`, `keep_unavailable`) | `dagda/ledger/availability.{h,cpp}`; `PeerLacks` moved in from the service |
| `service/availability_service.{hpp,cpp}` without its path table (`RemoteTrie`, `refresh`, `answer_trie`) | `dagda/store/availability.{h,cpp}` (`AvailabilityService`); the trie questions are answered by the storage server, which pins the pool's held union. The path table, `fill_path_table`, `update_path_table` and `availability/last-survey.bin` are Macha's (its namespace) |
| `cluster/distributed_store.cpp:2083-2095`, `:2291-2311`, `:2260-2284`, `:2324-3087` (`replicate_all`, `erase_all`, `ensure_local`, `repair_step` diff only) | `DistributedStore` (`dagda/store/distributed_store.{h,cpp}`); the cursor walks, `repair_position`, `validated_presence` as a probe and the `have_object` fallback are not moved (the maintainer, 2026-11-04) |
| Macha's `repair_step` as moved (push then pull in id order, both ends sending the same object) | Redesigned in Dagda (the maintainer, 2026-11-16; `TODO/2026-11-16-repair-redesign.md`): the survey keeps who holds what (`ledger::PeerHolds`), one queue by copies left, and an offer before each push (`offer_objects`, `dagda/store/offer_book.{h,cpp}`) so each copy moves once. No Macha counterpart; Macha's live nodes keep the old step until the cutover |
| `service/maintenance.cpp:674-852` (the repair stage: survey, claim walks with a credited restorer, the repair step, its gates) | `dagda/store/repair.{h,cpp}` (`Repair::pass`) |
| `cluster/storage_server.cpp` `have_valid_objects`, `delete_object` and each route's admission | `dagda/store/storage_server.{h,cpp}` |

