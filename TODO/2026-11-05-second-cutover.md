# Second cutover: Macha's store onto Dagda, step by step

Status: plan, draft for the maintainer, 2026-11-05. Written by the Dagda session. Nothing here
is built, and nothing in `../macha` was changed. It follows
`2026-11-03-macha-store-requirements.md` section 6 (the order of the steps) and
`../macha/TODO/2026-11-04-dannan-dagda-port-evaluation.md` phases 3 and 4.

**Where this was checked.** Macha at develop `00861dd` (0.92.3; the checkout's only
uncommitted changes are two `TODO/` files, so `src/`, `tests/`, `tools/` and `docs/`
read as that commit). Dagda at `9c190a9`, plus what another session left uncommitted in this
tree while this was written (the write-form correction, B4 below, and
`TODO/2026-11-05-test-mapping.md`); citations into `dagda/store/distributed_store.{h,cpp}` are to
that working tree. Dannan at `4742cb4` (0.4.1). I did not ssh
anywhere, so nothing about the live nodes is verified beyond what the documents say;
the sizes are the evaluation's and the maintainer's. Every path, test name, Status key and
`dagda.*` key below was checked by script against those trees
(`build/scratch/verify_plan.py`). `file:line` means: `src/`, `tests/`, `tools/`,
`docs/`, `systemd/` are Macha's; `dagda/` and `docs/migration.md` are this repository's;
`dannan/` is Dannan's. Files marked NEW do not exist and are names proposed here.

**Rulings this plan takes as given.** Two cutovers: Macha on Dannan's transport first
with its own store, a soak, then the Dagda disk migration as one flag day. The reverse
migration is kept and gates nothing. The per-record open stays. Macha's code at
`00861dd` is the plan where a document disagrees. A Macha case whose subject becomes
Dagda's may be retired against a named `dagda_tests` case, mapped case by case;
everything Macha-visible stays. Defects are fixed in Macha first.

**The audit the evaluation names.** Its "section 6 of the Macha audit" is not a file in
any repository. It is the report of a subagent of the Macha session, found in that
session's transcript (`a86ed3b5-.../subagents/agent-a954820ec70df88f7.jsonl`), at
`5776392`. I used its Status list as a checklist and re-read every key in
`src/api/status_api.cpp` at `00861dd`; the line numbers below are from that reading.

## 0. Where things stand, and what has to be true first

Not one of these is done at the commits above:

1. **The first cutover.** Macha still has its own transport (`src/cluster/net.cpp`).
   This plan names store files only and assumes `Config`, `NodeRuntime` and
   `tests/test_support.hpp` have been split behind a facade by that cutover. Paths in
   `src/cluster/` may have moved by then, so each step's file list is re-checked when
   its branch is cut.
2. **Macha's two P0s are fixed in 0.92.4** (`cac5998`): the trace flake, in the fixture,
   and the stop hanging in the survey's walk. Dagda has taken the stop's shape and the
   claim deletion mark; two follow-ups are with the maintainer through Macha (`TODO/ACTIVE.md`).
   Macha's goldens, then its tree-native merge, come next.
3. **Macha's tree-native merge** (`feature/tree-native-namespace`, `b080c38`, not an
   ancestor of develop). Step 8c and every fixture wait on it, and test names in step
   8c change with it: `tests/test_namespace_migration.cpp` becomes
   `test_namespace_service.cpp` on the branch, and that name does not exist at `00861dd`.
4. **Fixtures.** `../macha/build/macha-fixtures/` holds `gen/` and `placement/` only.
   No state directory, no `macha-metadata-dump` output.
5. **Goldens and baselines** (evaluation section 6, items 3 and 4). No Status golden and
   no config golden under `tests/fixtures/`; no baseline file under `../macha/TODO/`.
   "Byte-identical Status" has nothing to compare against until the golden exists, so it
   is a precondition of step S's gate and not part of it.
6. **Dagda's own list**: M1 (heads; `dagda/heads/` does not exist) and M10 (scale, a
   Linux run with `syncfs`, sanitizers). `TODO/ACTIVE.md` records M10 as waiting on the maintainer
   for which machine. Step 4 of Dagda's plan closes `dagda::Context` v1, so
   `dagda/context.h` is still growing and has no Heads noun.
7. **The test mapping** (evaluation section 6 item 5) now exists:
   `TODO/2026-11-05-test-mapping.md`, 786 cases: 143 RETIRE, 167 REWRITE, 354 KEEP, 27 GAP,
   95 GAP-HEADS, nothing retired yet. It is the fate of each case; this plan does not repeat
   it. **How the gates below read with it:** a named case passes in Macha, rewritten over the
   new fixtures where the mapping says REWRITE, or is retired against the `dagda_tests` case the
   mapping names where it says RETIRE. A GAP case stays until Dagda has written what the mapping
   says it owes. The lists below are by step; the mapping is by case.

**Superseded (the maintainer, 2026-11-05, `TODO/ACTIVE.md` Decisions):** the cutover is in stages, as many stops as
needed, moving on when tests pass; `<state>/metadata/` is migrated at 8c; Macha's loop runs until 8c.
What follows on a single flag day, questions 1 to 8 and the soak are read with that.

**How the steps deploy.** A v19 disk is refused by anything but Dagda and Dagda refuses
v18 (`dagda/node.cpp:128-129`), so the flag day is the deployment of step S. But
`dagda::Node` without a provider removes nothing (`dagda/node.cpp:398-408`), and Macha's
own loop cannot sweep Dagda's stores. So S, 8a and 8b are separate branches and separate
gates, in process, and **deploy together as one release on the flag day**. 8c and 8d
deploy later and need no object rewrite. Whether 8c needs anything rewritten under
`<state>/metadata/` is question 1 below.

**What the step gates do not replace.** VALIDATION's rule stands at every step: a clean
`./run-tests.sh build` with all three executables, on a host with the documented
dependencies. Today's count on live-a is 745 + 20 + 21 (`../macha/TODO/ACTIVE.md`).
A count that drops is a finding, unless the mapping names the case.

## Step S: the store under Macha's names

Before 8a in the requirements. The local stores, the pool, the domains, the distributed
store, the ledger and repair become Dagda's, and Macha opens a `dagda::Node`.

### S.1 Macha files

**Removed, the code lives in Dagda** (all verified to exist at `00861dd`):

- `src/storage/durability_domain.{hpp,cpp}`, `src/storage/io_pressure.hpp`,
  `src/storage/local_store.{hpp,cpp}`, `src/storage/persistent_cache.{hpp,cpp}`,
  `src/storage/presence_index.{hpp,cpp}`, `src/storage/retention.{hpp,cpp}`,
  `src/storage/sealed_journal.{hpp,cpp}`, `src/storage/storage_pool.{hpp,cpp}`,
  `src/storage/unreferenced_since.{hpp,cpp}`.
- `src/ledger/availability.{hpp,cpp}`, `src/ledger/held_ledger.{hpp,cpp}`,
  `src/ledger/object_trie.{hpp,cpp}`, `src/ledger/reference_counts.{hpp,cpp}`,
  `src/ledger/retention_ledger.{hpp,cpp}`, `src/ledger/trie_diff.{hpp,cpp}`,
  `src/ledger/trie_union.{hpp,cpp}`.
- `src/cluster/distributed_store.{hpp,cpp}`, `src/cluster/storage_server.{hpp,cpp}`,
  `src/cluster/control_objects.{hpp,cpp}`, `src/cluster/data_work.hpp`,
  `src/cluster/placement.{hpp,cpp}`, `src/cluster/replica_selector.{hpp,cpp}`.
- `src/contract/claim_store.hpp`, `src/contract/gates.{hpp,cpp}`,
  `src/contract/horizon.{hpp,cpp}`, `src/contract/id_lookup.hpp`,
  `src/contract/object_ledger.hpp`, `src/contract/object_store.hpp`.
- `src/service/claim_walk.{hpp,cpp}`.
- `src/contract/predicates.hpp` and `src/ledger/predicate_query.cpp`: Dagda has no
  counterpart. I found no production caller outside the ledger and
  `tests/test_predicates.cpp`, by search of `src/service`, `src/cluster`, `src/api`,
  `src/filesystem` and `src/catalogue`. Keep them in Macha or retire them with the mapping.

**Replaced by a facade over `dagda::Node`:**

- `src/cluster/local_state.{hpp,cpp}`: `LocalState` today builds the pool, control store,
  cache, retention, reference counts and `MetadataReplica`, in that order, after the
  transport is up. The replica stays Macha's until 8c, so `LocalState` shrinks to it.
- `src/cluster/local_services.{hpp,cpp}`, `src/cluster/node_resources.{hpp,cpp}`,
  `src/cluster/node_identity.cpp` (it includes the store's header for
  `load_or_create_node_id`; Dannan has its own).
- `src/service/node_services.{hpp,cpp}`, `src/service/service.{hpp,cpp}`,
  `src/service/main.cpp` (the 500 ms durability window at `src/service/main.cpp:53`).

**Changed callers:**

- `src/service/maintenance.{hpp,cpp}`: keeps metadata repair, catalogue repair, head
  repair and history truncation (`src/service/maintenance.cpp:475-558`, `:562-579`,
  `:1143-1159`) until 8c. The local stages (`:1107-1234`) go to the node.
- `src/service/availability_service.{hpp,cpp}`: the survey goes to
  `dagda/store/availability.h`; the path table stays Macha's (M6;
  `src/service/availability_service.cpp`, `fill_path_table`, `update_path_table`,
  `availability/last-survey.bin`, `src/service/node_services.cpp:51`).
- `src/filesystem/filesystem.{hpp,cpp}`, `src/filesystem/hydration.cpp`.
- `src/catalogue/catalogue.{hpp,cpp}`, `src/catalogue/media_catalogue.{hpp,cpp}`
  (`DurabilityBatch`).
- `src/metadata/metadata_server.cpp` (`remember_metadata`, `src/metadata/metadata_server.cpp:96`),
  `src/metadata/metadata_manager.cpp`, `src/metadata/namespace_control_store.{hpp,cpp}`.
- `src/torrent/torrent_disk_io.{hpp,cpp}`, `src/torrent/torrent_manager.{hpp,cpp}` (the
  arbiter).
- `src/media/media_segments.cpp` and `src/write_behind.{hpp,cpp}`: Dagda has
  `dagda/store/write_behind.h`, and Macha's media code uses write-behind too, so by the
  stack's rule 3 it links one copy. Which one is decided with Dannan's session.
- `src/api/status_api.{hpp,cpp}`: section S.2.
- `src/cluster/cluster.hpp`: drops the store headers.
- `tools/metadata_history_dump.cpp`: opens the control store with `LocalStore`, so it
  must read v19 from this step on. `tools/namespace_migrate.cpp` likewise, unless the
  tree-native merge has removed it, as it does on the branch.
- `systemd/macha.service`: custody's key source (section "Migration", Dannan state).
- `CMakeLists.txt`, `docs/storage.md`, `docs/durability.md`, `docs/configuration.md`,
  `VALIDATION.md`, `macha.yaml.example`.
- Fixtures: `tests/test_support.hpp`, `tests/test_backend_support.hpp`,
  `tests/fake_cluster_node.hpp` (`StoreBench`; it hand-builds store replies, so it follows
  Dagda's messages), `tests/interposed_local_store_files.hpp`.

### S.2 The facade: Status keys and what feeds them

All keys verified in `src/api/status_api.cpp`. "Local" is this node; "peers" is what
`GET /api/v1/status` shows for other nodes, which today is gossiped telemetry
(`src/cluster/telemetry.cpp:568-571`, `:580`, `:597-599`). Macha's facade owns the JSON
(`../dannan/TODO/2026-11-04-macha-parity.md` section 7). "NO KEY" means Dagda holds the
figure but publishes no `dagda.*` for it, and the accessor is named; MISSING means Dagda
has no figure at all.

`GET /api/v1/status`, storage and cache (`src/api/status_api.cpp:289-310`, `:805-814`):

| Macha key | Local | Peers |
|---|---|---|
| `nodes[].storage.capacity_bytes` | `dagda.storage.limit` | advert `dagda/capacity` (`dagda/store/storage_server.cpp:79`) |
| `nodes[].storage.used_bytes` | `dagda.storage.used` | advert `dagda/used` (`:80`) |
| `nodes[].storage.free_bytes`, `.available` | computed by the facade, `src/api/status_api.cpp:69-81` | same |
| `nodes[].storage_backends_online` | `dagda.storage.backends_online` | MISSING (needs a Macha hosted telemetry field) |
| `nodes[].cache.capacity_bytes` | MISSING as a figure; `max_blocks * extent_size` as Macha computes it (`src/cluster/storage_server.cpp:55`), from `NodeOptions.cache.max_blocks` and `store.extent_size` | MISSING (hosted field) |
| `nodes[].cache.used_bytes` | `dagda.cache.blocks` times `extent_size` (`src/cluster/storage_server.cpp:56`) | MISSING (hosted field) |
| `nodes[].cache.hits`, `.misses`, `.evictions` | `dagda.cache.hits`, `dagda.cache.misses`, `dagda.cache.evictions` | MISSING (hosted field) |
| `nodes[].roles` (`storage`, `cache`) | derived from the two capacities, `src/api/status_api.cpp:312-319` | same |
| `cluster.storage_known`, `cluster.storage_online`, `cluster.cache_known`, `cluster.cache_online` | the facade's rollup of the rows above, `src/api/status_api.cpp:805-814` | same |
| `cluster.nodes_hosting_extents`, `cluster.nodes_hosting_extents_online` | advert `dagda/extents` (`dagda/store/placement.h:35`) | same |
| `startup.data_storage`, `startup.control_storage`, `startup.cache`, `startup.retention` | Dannan's `Readiness`, planes declared by the node (`dagda/node.cpp:32-35`, `:56`); see the first defect below | not shown for peers |

The telemetry record's last-known persistence (`src/api/status_api.cpp:568-577`) needs the
same hosted fields to be stored by Dannan's telemetry file, which is why the layout has to
be fixed before any `DNNTEL01` file is written (section "Migration").

`GET /api/v1/status/diagnostics`, `repair` (`src/api/status_api.cpp:1316-1380`):

| Macha key | Feed |
|---|---|
| `diagnostics.repair.available` | the facade: true while the node runs |
| `diagnostics.repair.unsourceable_objects` | `dagda.repair.unsourceable` |
| `diagnostics.repair.local_unreadable_objects` | `dagda.repair.local_unreadable` |
| `diagnostics.repair.unsourceable_sample` | NO KEY: `node.store().repair_diagnostics().unsourceable_sample` |
| `diagnostics.repair.push_examined`, `.pull_examined` | NO KEY: same struct (`dagda/store/distributed_store.h:300-301`) |
| `diagnostics.repair.bytes_transferred` | `dagda.repair.bytes` |
| `diagnostics.repair.passes_completed` | `dagda.repair.passes` |
| `diagnostics.repair.push_phase_complete` | NO KEY: same struct (`:304`). Dagda still sets it (`dagda/store/distributed_store.cpp:2826`) |
| `diagnostics.repair.pass_gates.ran`, `.share`, `.quiescent`, `.credit` | `dagda.repair.gate.ran`, `dagda.repair.gate.share`, `dagda.repair.gate.quiescent`, `dagda.repair.gate.credit` |
| `diagnostics.repair.last_credit_bytes` | MISSING: `RepairPass::credit` is the pass's return value and `dagda/node.cpp:644` discards it |
| `diagnostics.repair.pace` | computed by the facade from `dagda.repair.gate.last` and `paced_by`, `src/api/status_api.cpp:1355-1366` |
| `diagnostics.repair.paced_by` | NO KEY: `node.repair().gates().paced_by` (`dagda/store/repair.h:84`). Bit 1 is `paced_by_foreground`; the JSON stays `viewer`, `loader`, `peer_viewer` |
| `diagnostics.prompt_replication.queued`, `.copies`, `.failures`, `.skipped_no_room`, `.dropped` | `dagda.prompt.queued`, `dagda.prompt.copies`, `dagda.prompt.failures`, `dagda.prompt.skipped_no_room`, `dagda.prompt.dropped` |

`diagnostics.data_resources` (`src/api/status_api.cpp:1009-1037`). Everything comes from
`node.credit()->stats()` (`dagda/store/data_work.h:58-87`):

| Macha key | Feed |
|---|---|
| `diagnostics.data_resources.used_bytes` | `dagda.admission.credit_used` |
| `diagnostics.data_resources.device_pressured` | `dagda.admission.device_pressured` |
| `diagnostics.data_resources.pressure_refusals` | `dagda.admission.pressure_refusals` |
| `diagnostics.data_resources.capacity_bytes`, `.peak_used_bytes`, `.background_limit`, `.background_active`, `.peak_background_active`, `.device_service_us`, `.device_worst_us`, `.device_slowdown_percent`, `.device_pressure_onsets`, `.cancelled_waits`, `.loader_admissions`, `.speculative_admissions`, `.loader_waits`, `.speculative_waits` | NO KEY: the same name in `DataResourceStats` |
| `diagnostics.data_resources.viewer_reserve_bytes`, `.viewer_admissions`, `.viewer_waits` | NO KEY: `foreground_reserve_bytes`, `foreground_admissions`, `foreground_waits`. The JSON keeps `viewer` |

`diagnostics.data_store` (`src/api/status_api.cpp:1081-1104`). Everything comes from
`node.data().diagnostics()` (`dagda/store/storage_pool.cpp:1266-1271`):

| Macha key | Feed |
|---|---|
| `diagnostics.data_store.available` | the facade |
| `diagnostics.data_store.loose_reaffirmation_fast_paths`, `.loose_reaffirmation_full_validations`, `.presence_index_entries`, `.pack_recovery_truncated_tails`, `.pack_recovery_skipped_regions`, `.pack_recovery_skipped_bytes` | NO KEY: the same names |

`diagnostics.metadata` and `diagnostics.convergence` are not affected: both stay Macha's
until 8c.

**Published figures against live ones.** Macha reads these at request time from atomics
(`src/api/status_api.cpp:889-891`). `dagda::Node` publishes every `status_interval`, 10 s by
default (`dagda/node.h:172`). A facade reading only `dagda.*` would answer up to 10 s stale,
and `test_status_is_light_and_diagnostics_have_their_own_route` and the status cases read
fresh values. Recommended: the facade calls the accessors named above for everything, so
`dagda.*` are the operators' view and the keys the table names are the mapping to prove.

### S.3 Config keys

YAML keys verified in `src/config.cpp:190-233`, `:236-279`, `:864-921`. `NodeOptions` is
`dagda/node.h:142`. A `Node` takes a restart for everything but backends and cache
(`NodeReload`, `dagda/node.h:189-192`), as Macha does (`src/service/service_config.cpp:23-25`
refuses replicas; `src/cluster/local_state.cpp:186-190`).

| Macha YAML key | Macha default | Dagda option | Dagda default, where it differs |
|---|---|---|---|
| `state_path` | none | `NodeOptions.state` | |
| `key_file` | none | `NodeOptions.key` = `StorageKey::derive_v1` of the cluster key | |
| `storage.hosts_extents` (true, false, auto) | auto | `StorageServerOptions.hosts_extents` (bool, fixed at open) | MISSING: the tristate is Macha's, and see behaviour B3 |
| `storage.data.backends[].path`, `.limit`, `.reserve_free` | none | `StorageBackendOptions.path`, `.limit`, `.reserve_free` | |
| `storage.data.packing.threshold`, `.target_size` | 1 MiB, 64 MiB | `StoragePoolOptions.pack_threshold`, `.pack_target_size` | |
| `storage.ledger_cache` | 64 MiB | `NodeOptions.ledger_cache_bytes`, same division (`src/cluster/local_state.cpp:29-50`) | |
| `storage.metadata.path`, `.limit` | `<state>/metadata-objects`, 4 GiB | `ControlStoreOptions.path`, `.limit` | |
| `storage.metadata.packing.threshold`, `.target_size` | as DATA | `ControlStoreOptions.pack_threshold`, `.pack_target_size` | |
| none (Macha passes 0, `src/cluster/local_state.cpp:134`) | 0 | `ControlStoreOptions.reserve_free` | 256 MiB, ruled by the maintainer |
| `cache.path`, `cache.max_blocks` | none, 0 | `CacheOptions.path`, `.max_blocks` | |
| `cache.prefer_metadata` | true | `CacheOptions.seed` | |
| `dht.replicas` | 3 | `DistributedStoreOptions.replication` | |
| `dht.write_copies` | 1 | `DistributedStoreOptions.write_copies`, **and** the write's level; see behaviour B4 | **default 2**, the facade must set it |
| `dht.metadata_write_copies` | 2 | `DistributedStoreOptions.metadata_write_copies` | |
| `dht.retention_check_batch_size`, `dht.retention_check_concurrency` | 2000, 8 | same names in `DistributedStoreOptions` | |
| `dht.write_stall_ms` | 2500 | `DistributedStoreOptions.write_stall` | |
| `dht.extent_size` | 16 MiB | `DistributedStoreOptions.extent_size` (the node copies it to the server and repair) | |
| `dht.data_inflight_bytes` | 128 MiB | `AdmissionOptions.data_inflight_bytes` | |
| `dht.data_viewer_reserve_bytes` | 32 MiB | `AdmissionOptions.foreground_reserve_bytes` | |
| `dht.data_credit_no_progress_deadline_ms` | 120 s; 0 waits unbounded | `AdmissionOptions.no_progress` (`store::default_no_progress`, 120 s) | governs every wait whose caller named no deadline. The arbiter refuses 0: the facade passes 100 years (3153600000000 ms; `StoreDataWork.AHundredYearWindowDoesNotOverflow`). The one difference left in code: a wait with no release anywhere fails at 100 years, where Macha's goes on. To see it, a config must set 0 (both live nodes set 120000) and nothing in the arbiter must release for a century |
| `dht.io_pressure_overhead_ms`, `dht.io_pressure_per_mib_ms`, `dht.io_pressure_slowdown_percent`, `dht.io_pressure_release_percent`, `dht.io_pressure_outlier_percent` | 25, 120, 300, 150, 1000 | no `NodeOptions` field; `node.data().configure_service_monitor(Thresholds)` after open (`dagda/store/storage_pool.h:179`). Slowdown 0 disables in Macha (`src/cluster/local_state.cpp:96`), so the facade keeps that test | MISSING as an option |
| `dht.io_pressure_min_background` | 1 | `AdmissionOptions.min_background_under_pressure` | |
| `dht.read_ahead` | 3 | none: `src/config.hpp:672`, the FS's | stays Macha's |
| `dht.metadata_cache_ms`, `dht.metadata_materialization_cache_bytes` | 250 ms, 128 MiB | MISSING until 8c | |
| `maintenance.interval_ms` | 1000 | `MaintenanceOptions.interval` | |
| `maintenance.foreground_quiet_ms` | 2000 | `RepairOptions.foreground_quiet` (the node's `higher_class_active` reads it too) | |
| `maintenance.garbage_grace_ms` | 30 days | `NodeOptions.garbage_grace` | |
| `maintenance.busy_bandwidth_fraction` | 0.0 | `MaintenanceOptions.busy_bandwidth_fraction` | |
| `maintenance.idle_bandwidth_fraction` | 0.10 | `MaintenanceOptions.idle_bandwidth_fraction` **and** `RepairOptions.idle_bandwidth_fraction` | two fields, one key |
| `maintenance.foreground_weight`, `.repair_weight` | 95, 5 | `RepairOptions.foreground_weight`, `.repair_weight` | |
| `maintenance.cpu_target` | 0.10 | `MaintenanceOptions.cpu_target` **and** `RepairOptions.cpu_target` | two fields, one key |
| `maintenance.background_concurrency` | 0 = half the hardware threads, at least 1 (`src/config.hpp:88-91`) | `AdmissionOptions.background_concurrency` | **default 4** and no zero rule: the facade resolves it |
| `maintenance.initial_bandwidth`, `.max_bandwidth` | 32 MiB, 0 | `RepairOptions.initial_bandwidth`, `.max_bandwidth` (the local stages read them too) | |
| `maintenance.scrub_fraction`, `.scrub_interval_ms` | 0.02, 30 days | `MaintenanceOptions.scrub_fraction`, `.scrub_interval` | |
| `maintenance.no_progress_backoff_ms` | 300 s | `MaintenanceOptions.gc_interval` **and** `CollectorBudgets.retry_window` | two fields, one key; and behaviour B2 |
| `maintenance.convergence_settle_ms`, `.convergence_max_delay_ms` | 1000, 10000 | MISSING until 8c: Macha's loop keeps them | |
| `network.telemetry_interval_ms` | 10 s | `RepairOptions.peer_window` = max(3 x interval, 3 s) (`src/service/maintenance.cpp:432-433`) | **default 3 s**, a tenth of Macha's 30 s at the default interval; the facade must set it |
| `runtime.memory_bytes` | 0 | Dannan's `fit_to_limit` shrinks hosted parts (`dannan/node.cpp:351-361`); the facade must pass `storage.ledger_cache` as a part. Not checked further | |

Dagda options with no Macha key, which keep their defaults unless the maintainer says otherwise:
`StoragePoolOptions.durability_batch_window` (500 ms, Macha's constant at
`src/service/main.cpp:53`), `IoReserveOptions.slots` and `.control_reserve` (8, 2; ruled),
`DistributedStoreOptions.write_deadline` and `.read_deadline` (60 s), `.call_horizon` (120 s),
`.admission_wait` and `StorageServerOptions.admission_wait` (30 s). The last two bound
control-slot waits only, and with the reserve off, as Macha's facade has it, nothing waits
on them. `write_deadline` starts when the local credit wait is over, and `read_deadline`
bounds a read's remote stage alone; past those points they are limits Macha has not, which
its cases may catch; none is measured on a Pi. `DistributedStoreOptions.prompt_read_admission`
(default on) the facade **must turn off**: Macha's prompt read takes no credit.

### S.4 The gate

**VALIDATION sections:** "Physical representation", "Admission and capacity", "Placement
and the write floor", "Class separation", "Refusing unversioned storage", "Memory".

**Crash-matrix rows** (`docs/durability.md:146-156`), all eleven, with the cases I take to
prove each. The mapping is by name and by the requirements' proof column; I did not read
every case's body, so the row-to-case mapping needs checking when the test mapping is written.

| Row | Cases |
|---|---|
| "Before local FUSE admission is durable" | `test_fuse_admission_backpressure`, `test_fuse_data_journal_recovery` |
| "Durable FUSE spool/journal, no DATA placement" | `test_fuse_data_journal_recovery` |
| "Some provisional DATA placements, no required barriers" | `test_publication_generation_barrier_precedes_metadata_commit`, `test_deferred_object_barrier_rejects_stale_process_epoch` |
| "DATA barriers complete, metadata commit not accepted" | `test_publication_writer_failure_stays_invisible`, `test_publication_writers_stage_and_commit_generations` |
| "Metadata committed, local FUSE completion not recorded" | `test_fuse_namespace_journal_recovery` |
| "Pack tail torn" | `test_pack_recovery_settles_each_kind_of_damage` |
| "Pack compaction after replacement install but before old deletion" | `test_packs_store_compact_and_survive_restart` |
| "DATA accounting dirty" | `test_accounting_dirty_marker_is_process_session_scoped`, `test_unclean_accounting_recovery_establishes_durable_baseline`, `test_clean_accounting_checkpoint_is_trusted_with_packs` |
| "Catalogue control object missing from one replica" | `test_catalogue_control_objects_recover_on_metadata_replica` |
| "Cache missing/corrupt" | `test_persistent_cache_is_explicitly_ephemeral`, `test_storage_pool_and_persistent_cache` |
| "GC unlink lost" | `test_authoritative_delete_is_lazy_durability` |

**Named cases that must pass in Macha** (file, then every case, with its line):

- `tests/test_storage_v18.cpp` (21, all): `test_packs_store_compact_and_survive_restart` (109), `test_admission_respects_reserve_and_capacity` (163), `test_pack_recovery_settles_each_kind_of_damage` (217), `test_held_device_io_blocks_only_its_own_object` (321), `test_presence_is_known_without_reading_objects` (467), `test_metadata_control_store_is_independent_of_data_quota` (539), `test_unversioned_state_is_refused` (574), `test_a_full_preferred_node_spills_data_but_not_control` (734), `test_repair_passes_unavailable_and_counts_unsourceable_objects` (807), `test_a_busy_peer_still_says_what_it_does_not_hold` (909), `test_durability_barrier_across_peer_restarts` (967), `test_min_write_two_uses_fallback_when_preferred_replica_is_full` (1034), `test_min_write_floor_publishes_then_repair_converges_to_r2` (1076), `test_nodes_replicating_to_each_other_do_not_hold_their_slots_across_the_peer` (1110), `test_retain_data_batches_a_large_publication_within_bounded_time` (1152), `test_a_control_graph_larger_than_the_connection_budget_still_publishes` (1198), `test_a_control_offer_sends_only_what_the_peer_lacks` (1265), `test_catalogue_control_objects_recover_on_metadata_replica` (1306), `test_edge_node_never_owns_and_its_writes_land_on_owners` (1373), `test_repair_keeps_a_copy_it_has_nowhere_to_send` (1451), `test_node_that_stops_hosting_drains_through_repair` (1481).
- `tests/test_object_ledger.cpp` (25, all): `test_a_sealed_journal_replays_what_was_appended_in_order` (52), `test_a_torn_journal_keeps_what_came_before_the_tear` (73), `test_a_damaged_or_refused_frame_ends_the_replay_there` (103), `test_a_reset_journal_is_empty_and_keeps_appending` (142), `test_the_trie_holds_exactly_what_was_written` (217), `test_the_trie_hash_depends_only_on_its_records` (253), `test_a_subtree_reads_the_same_in_every_trie_holding_it` (341), `test_a_diff_finds_exactly_what_differs` (433), `test_a_diff_costs_what_differs_not_the_size` (484), `test_records_not_held_are_what_no_held_trie_holds` (510), `test_a_union_of_tries_reads_as_one_trie_of_their_records` (551), `test_a_reopened_trie_is_the_one_that_was_closed` (624), `test_a_crash_at_any_step_of_a_checkpoint_loses_nothing` (655), `test_a_trie_rewrites_its_node_file_and_stays_bounded` (711), `test_a_trie_larger_than_its_cache_reads_what_it_needs` (744), `test_a_trie_with_a_torn_journal_keeps_what_was_whole` (768), `test_a_format_1_trie_opens_and_is_rewritten` (796), `test_a_snapshot_is_frozen_through_writes_and_rewrites` (920), `test_a_large_trie_opens_within_the_bound` (991), `test_a_store_with_its_ledger_starts_knowing_what_it_holds` (1060), `test_verification_corrects_the_ledger_from_the_disk` (1112), `test_the_ledger_lists_what_the_packs_hold` (1168), `test_a_flush_does_not_wait_for_the_seed` (1217), `test_verification_resumes_after_a_restart` (1255), `test_reference_counts_follow_and_survive_a_restart` (1276).
- `tests/test_availability.cpp` (13, all): `test_a_trees_children_are_exactly_its_extent_references` (111), `test_the_path_table_follows_a_change_to_the_tree` (134), `test_trie_questions_and_answers_survive_the_wire` (264), `test_a_survey_a_peer_could_not_finish_keeps_what_was_unavailable` (322), `test_the_survey_by_diff_finds_what_no_node_holds` (360), `test_what_this_node_lacks_is_the_referenced_set_less_every_disk` (454), `test_a_files_code_follows_its_extent_counts` (493), `test_items_take_their_best_file_and_sets_count_their_members` (512), `test_the_item_table_is_rebuilt_only_when_a_snapshot_changes` (594), `test_a_file_the_survey_cannot_decide_takes_its_last_known_count` (620), `test_what_only_a_host_out_of_reach_holds_is_unavailable` (714), `test_two_nodes_survey_what_neither_holds` (792), `test_a_disk_coming_back_is_surveyed_without_a_namespace_change` (1052).
- `tests/test_io_pressure.cpp` (13, all): `test_an_ordinary_large_write_is_not_a_slow_device` (25), `test_pressure_onset_and_release_are_each_logged_once` (43), `test_a_device_far_slower_than_it_should_be_is_noticed` (79), `test_the_single_sample_trip_scales_with_the_operation` (108), `test_a_read_is_measured_with_the_bytes_it_returned` (140), `test_maintenance_reads_are_part_of_the_service_time_they_spend` (188), `test_a_viewer_is_never_refused_because_the_disk_is_slow` (224), `test_the_loader_runs_freely_under_pressure_with_no_viewer` (249), `test_the_loader_yields_to_a_viewer_on_a_slow_device` (275), `test_a_viewer_between_two_extents_is_still_a_viewer` (311), `test_background_work_always_drains_on_a_pressured_device` (340), `test_control_cannot_enter_the_data_arbiter_at_all` (370), `test_with_no_device_admission_is_exactly_what_it_was` (390).
- `tests/test_presence.cpp` (9, all): `test_presence_index_matches_its_model_over_every_short_history` (95), `test_presence_index_warmup_race_cases_by_name` (126), `test_has_answers_a_put_in_progress_without_waiting_for_it` (171), `test_a_put_that_fails_midway_is_never_present` (211), `test_has_does_not_consult_the_device_after_warm_up` (237), `test_presence_is_exact_across_a_restart` (257), `test_a_backend_offline_and_readopted_leaves_presence_exact` (279), `test_the_held_identity_names_exactly_what_the_pool_holds` (310), `test_the_held_identity_is_the_same_on_any_number_of_disks` (371).
- `tests/test_baselines.cpp` (4, all): `test_baseline_data_store_has` (70), `test_baseline_claim_walk_per_object` (92), `test_baseline_claim_walk_on_the_ledger_per_object` (118), `test_baseline_observation_probe_costs` (146).
- `tests/test_root_parts.cpp` (5, all): `test_control_fetch_stores_only_the_object_it_asked_for` (48), `test_control_fetch_passes_over_a_failing_peer` (103), `test_control_fetch_fails_when_the_store_cannot_take_the_object` (160), `test_node_resources_yield_the_loader_to_a_recent_viewer_under_pressure` (184), `test_recovery_progress_keeps_the_first_failure_and_wakes_its_waiters` (227).
- `tests/test_predicates.cpp` (5, all): `test_each_predicate_is_its_row_over_every_combination_of_facts` (46), `test_each_query_pages_to_the_predicate_over_the_whole_set` (187), `test_queries_without_horizons_examine_nothing` (212), `test_garbage_is_not_queryable_at_stage_0` (227), `test_a_cancelled_budget_stops_a_query_before_it_examines` (239).
- `tests/test_torrent_disk_io.cpp` (21, all): `test_a_real_swarm_downloads_through_the_backend_and_leaves_nothing_behind` (286), `test_extents_publish_once_their_pieces_verify_and_journal_the_manifest` (389), `test_publication_progress_is_reported_until_every_extent_is_published` (456), `test_a_removed_torrent_keeps_its_files_alive_until_publication_lets_go` (495), `test_lost_piece_alerts_are_recovered_from_the_held_bitfield` (555), `test_a_restarted_backend_does_not_republish_journalled_extents` (583), `test_pieces_spanning_files_round_trip_through_the_payload_layout` (619), `test_a_hash_issued_before_its_writes_complete_sees_them` (660), `test_every_read_write_and_hash_is_admitted_and_measured` (688), `test_a_blocked_admission_pushes_back_on_the_network` (713), `test_abort_releases_jobs_blocked_in_admission` (761), `test_resume_is_trusted_only_while_files_still_hold_it` (791), `test_delete_removes_the_payload_and_nothing_else` (815), `test_a_three_hundred_file_torrent_holds_a_bounded_number_of_descriptors` (829), `test_loader_admission_waits_for_credit_and_yields_on_abort` (844), `test_a_held_torrent_is_not_restarted_by_the_queue` (913), `test_a_torrent_held_at_add_is_never_checked` (935), `test_resume_data_lets_a_restart_skip_the_recheck` (949), `test_resume_data_saved_while_held_does_not_keep_a_released_job_held` (994), `test_a_queued_check_is_told_apart_from_a_running_one` (1031), `test_shutdown_cancels_a_publication_in_flight` (1052).

And these cases of larger files:

- `tests/test_invariants.cpp` (27 of 40): `test_service_answers_health_and_status_through_startup` (1208), `test_a_recovering_node_answers_peers_and_is_shown_recovering` (1336), `test_status_is_light_and_diagnostics_have_their_own_route` (1412), `test_status_renders_each_node_from_what_it_knows` (1523), `test_status_follows_a_peer_through_telemetry_and_departure` (1817), `test_replica_repair_does_not_count_corrupt_remote_as_healthy` (2239), `test_rebalance_reads_nothing_with_one_backend_online` (2285), `test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy` (2307), `test_a_durability_barrier_is_timed_per_filesystem` (2412), `test_authoritative_deferred_generation_batches_stable_storage_barriers` (2427), `test_durability_batch_retains_exact_nondominated_frontier` (2479), `test_accounting_dirty_marker_is_process_session_scoped` (2514), `test_clean_accounting_checkpoint_is_trusted_with_packs` (2552), `test_unclean_accounting_recovery_establishes_durable_baseline` (2622), `test_restart_durable_reaffirmation_requires_no_new_barrier` (2681), `test_authoritative_delete_is_lazy_durability` (2716), `test_local_store_barrier_remembers_complete_physical_cut` (2751), `test_durability_domain_group_commits_independent_publications` (2803), `test_storage_pool_tokens_name_physical_domain_and_backend_incarnation` (2850), `test_immediate_reaffirmation_flushes_provisional_generation` (2896), `test_claims_owed_to_slow_peers_stay_within_their_bound` (2928), `test_publication_generation_barrier_precedes_metadata_commit` (2975), `test_persistent_cache_is_explicitly_ephemeral` (3012), `test_control_plane_hint_admission_is_storage_durable` (3056), `test_deferred_object_barrier_rejects_stale_process_epoch` (3071), `test_rpc_durability_barrier_group_commits_independent_publications` (3126), `test_rpc_durability_barrier_reuses_already_covered_generation` (3205).
- `tests/test_rpc_cluster.cpp` (30 of 114): `test_store_put_is_loader_activity_not_viewer_activity` (430), `test_store_put_publishes_at_the_floor_and_copies_promptly` (448), `test_store_put_replaces_a_replica_that_cannot_be_launched` (505), `test_store_claims_ask_nothing_more_about_objects_nobody_holds` (544), `test_store_put_to_a_silent_replica_returns_on_the_local_copy` (585), `test_store_concurrent_readers_share_one_fetch_and_buffer` (627), `test_repair_push_probes_by_window_and_sends_together` (741), `test_repair_push_sends_nothing_to_a_peer_that_cannot_say` (807), `test_repair_pushes_past_a_peer_with_no_room` (833), `test_repair_pushes_from_what_peers_hold_without_asking` (891), `test_repair_pushes_only_what_the_list_names` (929), `test_repair_probes_without_credit_and_transfers_only_with_it` (966), `test_repair_pass_keeps_its_place_across_live_sets_and_restarts` (1009), `test_repair_bandwidth_estimate_ignores_small_transfers` (1057), `test_repair_decides_already_held_without_reading_the_extent` (1075), `test_repair_keeps_a_pull_that_was_in_flight_when_its_turn_ended` (1090), `test_store_heals_a_bad_local_copy_from_a_peer` (1136), `test_store_runtime_cache_keeps_a_playback_fetch` (1173), `test_store_control_objects_take_no_data_credit_and_commit_alone` (1198), `test_startup_gate_fires_on_silence_not_on_slow_progress` (1226), `test_repair_is_paced_not_stopped_while_higher_classes_stay_busy` (1328), `test_data_credit_wait_gives_up_when_nothing_is_moving` (2774), `test_data_credit_wait_survives_genuine_contention` (2805), `test_storage_data_credit_reserves_viewer_headroom_and_control` (2828), `test_retained_missing_copy_repairs_without_namespace_reachability` (4376), `test_held_retention_claims_cost_repair_no_credit` (4461), `test_replication_policy_change_on_restart` (4677), `test_replacement_node_recovers_namespace_and_replication` (4796), `test_a_joining_node_pulls_its_objects_through_maintenance` (5154), `test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim` (5581).
- `tests/test_storage_metadata.cpp` (12 of 68): `test_retention_claims_are_causal_durable_and_observed_remove` (79), `test_retention_prune_cursor_cannot_starve_later_tombstones` (139), `test_retention_prune_keeps_a_row_claimed_while_it_checks_presence` (175), `test_retention_claims_checkpoint_into_the_ledger_and_restart` (205), `test_retention_checkpoints_after_enough_changes` (245), `test_retention_start_checkpoints_a_long_journal` (265), `test_retention_prune_survives_a_restart` (309), `test_retention_shard_checkpoint_moves_into_the_ledger` (341), `test_retention_claim_is_physical_gc_barrier` (447), `test_local_store` (3510), `test_storage_pool_and_persistent_cache` (3607), `test_repair_step_is_bounded_and_yields` (4350).
- `tests/test_filesystem_fuse.cpp` (6 of 17): `test_publication_writers_stage_and_commit_generations` (343), `test_publication_writer_failure_stays_invisible` (762), `test_fuse_admission_backpressure` (1749), `test_fuse_publication_backlog_wider_than_ledger_completes` (2128), `test_fuse_namespace_journal_recovery` (2314), `test_fuse_data_journal_recovery` (2889).
- `tests/test_foundations.cpp` (3 of 47): `test_config` (897), `test_capacity_placement` (1027), `test_configuration_tristates_and_hosting_rules` (1712).
- `tests/test_models.cpp` (1 of 3): `test_capacity_placement_invariants_model` (182).
- `tests/test_node_services.cpp` (2 of 6): `test_a_reload_reaches_every_service_with_live_limits` (151), `test_observation_gauges_follow_activity_repair_and_the_mount` (306).
- `tests/test_hydration_catalogue.cpp` (2 of 44): `test_hydration_scheduler_orders_runs_and_follows_playback` (158), `test_ingest_blocks_while_storage_is_unavailable` (3601).
- `tests/test_runtime_dependencies.cpp` (2 of 14): `test_example_configuration_loads` (314), `test_yaml_config` (330).

**Cases that cannot pass as they stand, or whose subject is gone:**

- `test_a_recovering_node_answers_peers_and_is_shown_recovering` and
  `test_service_answers_health_and_status_through_startup`: behaviour B1.
- `test_repair_pass_keeps_its_place_across_live_sets_and_restarts`,
  `test_repair_push_probes_by_window_and_sends_together`,
  `test_repair_push_sends_nothing_to_a_peer_that_cannot_say`: half their subject is cursor walks
  and probes, which the maintainer ruled out with "repair diff only" (evaluation section 5.3). The mapping
  RETIREs them with that doubt listed under its "Uncertain".
- `test_a_format_1_trie_opens_and_is_rewritten`,
  `test_retention_shard_checkpoint_moves_into_the_ledger`: legacy readers Dagda dropped by
  ruling. The mapping RETIREs them; that needs neither live node to hold such a file
  (not verified: section "Migration", "On each node first").
- `test_min_write_two_uses_fallback_when_preferred_replica_is_full`: the mapping's GAP for it
  predates B4's correction; re-read it once that is committed.
- `test_trace_backend_offline_and_back`: 8a's gate, and P0 today in Macha.

**On a real state directory:** the fixture from section 0, migrated, then `has` and `get`
true for every object and claim through `dagda::Node`. This is also Dagda's open gate 6b.

## Step 8a: the reachability provider

### 8a.1 Macha files

- Removed: `src/ledger/node_horizon_builder.{hpp,cpp}`, `src/contract/horizon_builder.hpp`.
- Changed: `src/catalogue/catalogue.{hpp,cpp}`: `control_gc_step` (`src/catalogue/catalogue.hpp:533`)
  and its cursor (`:367`) go, since `ledger::Collector::control_gc_step` has them; the
  `CatalogueManager` keeps `maintenance_objects` and `retention_objects` (`:528-529`) as
  what the provider calls, and supplies the epoch.
- Changed: `src/filesystem/filesystem.{hpp,cpp}`: `maintenance_objects_cached`,
  `namespace_references` (`src/filesystem/filesystem.hpp:588`, `:594`) as above.
- Changed: `src/service/maintenance.{hpp,cpp}`: the inventory, gates, tombstones, release and
  sweeps (`src/service/maintenance.cpp:620-1098`) go to the node's collector;
  `maintain_garbage_metadata` and `maintain_tombstone_batches` become the provider's
  `collect_garbage` (`dagda/contract/reachability.h:109`).
- Changed: `src/service/node_services.{hpp,cpp}`: the sightings' paths
  (`src/service/node_services.cpp:48-49`) go; the node opens the same files
  (`dagda/node.cpp:259-262`), and they are sealed on migration.
- NEW `src/service/reachability_provider.{hpp,cpp}`: Macha's implementation of
  `dagda::contract::ReachabilityProvider`, over the FS, the catalogue and the head.

### 8a.2 Facade

No key in the seven groups changes its feed at this step. The GC keys are not in Macha's
Status at all: Macha reports them through observations and the journal, and `dagda.gc.*` is
new and additive. The observation names change: behaviour B6.

### 8a.3 Config

`maintenance.garbage_grace_ms`, `maintenance.no_progress_backoff_ms` and
`maintenance.foreground_quiet_ms` as in S.3. The sweep's budgets (64 data, 32 control, 64
release, 64 prune) are constants in Macha (`src/service/maintenance.cpp:959`, `:968`, `:973`,
`:1001`, `:1040`, `:1079`) and `CollectorBudgets` here, with the same defaults: no YAML key
on either side, and none is added.

### 8a.4 The gate

**VALIDATION:** none is specific to this step; the "Class separation" and "Admission and
capacity" sections stay green. **Crash matrix:** "GC unlink lost". **Must not change:** the
three gates, the grace, "a copy with any local claim is untouchable", release only of dots
the observed clock dominates, no incomplete release published, no inventory used
destructively in the pass that built it. A new include-direction case fails on any `ledger/`
or `contract/` include of `catalogue/` or `filesystem/` (Macha's own tree, since Dagda's is
already pinned by its `ContractIncludeDirection` cases).

- `tests/test_horizon_builder.cpp` (6, all): `test_inventory_is_both_live_sets_and_the_catalogue_control_set` (154), `test_inventory_control_set_holds_the_namespace_tree` (189), `test_release_over_a_tree_matches_the_pass_build` (246), `test_an_unreadable_catalogue_root_leaves_the_release_incomplete` (284), `test_an_unreadable_tree_node_leaves_the_release_incomplete` (306), `test_an_empty_head_releases_against_nothing` (325).
- `tests/test_ledger_gates.cpp` (7, all): `test_referenced_sets_are_sorted_unique_and_per_class` (33), `test_inventory_splits_tombstones_by_whether_data_still_refers_to_them` (51), `test_release_horizon_carries_its_head_and_clock` (69), `test_tombstone_gate_is_the_pass_condition_over_every_input` (125), `test_control_gate_is_the_pass_condition_over_every_input` (137), `test_data_gate_is_the_pass_condition_and_reason_over_every_input` (154), `test_each_shut_gate_names_the_first_condition_that_failed` (187).
- `tests/test_claim_walk.cpp` (8, all): `test_retention_ledger_pages_every_live_claim_once` (41), `test_retention_ledger_stops_for_cancellation_first` (98), `test_retention_ledger_holds_by_class` (114), `test_retention_ledger_publishes_horizons_and_refuses_an_incomplete_release` (133), `test_retention_ledger_forwards_claims_releases_prunes_and_compacts` (174), `test_claim_walk_matches_the_walk_it_replaced_exhaustively` (336), `test_claim_walk_matches_the_walk_it_replaced_at_the_bound` (356), `test_claim_walk_follows_a_changing_ledger` (373).
- `tests/test_catalogue_maintenance.cpp` (15, all): `test_without_metadata_nothing_installs_and_the_read_is_incomplete` (103), `test_the_read_is_complete_only_when_the_view_is_at_the_head` (122), `test_a_head_behind_the_known_generation_reads_the_record_first` (147), `test_a_commit_installs_what_it_wrote_once` (177), `test_an_unchanged_root_is_never_installed_again` (195), `test_another_writers_commit_is_installed_on_the_next_head` (212), `test_a_root_that_cannot_be_read_keeps_the_view_serving` (234), `test_an_install_racing_a_commit_takes_what_it_wrote` (257), `test_resident_bytes_count_what_the_catalogue_holds` (278), `test_a_view_holds_each_entry_once_in_its_shard` (296), `test_what_a_commit_installs_is_what_a_load_of_its_root_reads` (347), `test_a_load_reuses_every_shard_the_new_root_still_names` (427), `test_a_write_shares_every_shard_it_did_not_touch` (458), `test_data_is_released_only_when_nothing_refers_to_it` (482), `test_artwork_is_staged_in_the_class_of_whoever_asked` (517).
- `tests/test_maintenance_trace.cpp` (10, all): `test_manual_clock_moves_and_wakes_only_when_told` (327), `test_system_clock_reads_and_waits_in_real_time` (404), `test_normalise_names_ids_by_first_appearance` (439), `test_trace_quiet_node` (449), `test_trace_tombstones_maturing` (461), `test_trace_revived_tombstone` (490), `test_trace_backend_offline_and_back` (511), `test_trace_claimed_objects_lost` (536), `test_trace_peer_unreachable_and_back` (561), `test_trace_incomplete_catalogue_and_release_horizon` (620).
- `tests/test_absent_nodes.cpp` (6 of 8): `test_an_object_matures_a_grace_after_its_first_sighting` (29), `test_an_object_referenced_again_starts_its_wait_afresh` (43), `test_a_clock_set_back_restarts_the_wait` (53), `test_a_completed_sweep_drops_sightings_of_objects_no_longer_held` (64), `test_sightings_survive_a_restart` (80), `test_the_sightings_codec_round_trips_and_refuses_damage` (109).
- `tests/test_storage_metadata.cpp` (3 of 68): `test_retention_claims_are_causal_durable_and_observed_remove` (79), `test_retention_claim_is_physical_gc_barrier` (447), `test_metadata_conflict_alternatives_are_gc_roots` (2140).
- `tests/test_rpc_cluster.cpp` (2 of 114): `test_a_delete_is_reclaimed_while_a_node_is_away` (4270), `test_unlink_retires_an_object_from_the_maintenance_inventory` (5084).
- `tests/test_namespace_migration.cpp` (4 of 15): `test_the_inventory_and_the_media_index_follow_the_tree` (255), `test_tombstones_live_in_batches_in_a_tree_backed_head` (398), `test_the_control_collector_keeps_an_unclaimed_namespace` (1026), `test_a_merge_claims_what_it_introduces` (1142).
- `tests/test_namespace_tree.cpp` (2 of 33): `test_every_tree_node_is_reachable_for_the_collector` (1182), `test_a_commit_claims_the_nodes_it_introduced_and_prunes_the_rest` (1203).
- `tests/test_filesystem_fuse.cpp` (2 of 17): `test_disconnected_maintenance_sleeps_until_peer_event` (3777), `test_coalesced_delete_burst_wakes_at_exact_garbage_grace` (3866).
- `tests/test_hydration_catalogue.cpp` (1 of 44): `test_catalogue_sync_search_and_artwork_gc` (4785).
- `tests/test_invariants.cpp` (1 of 40): `test_catalogue_keeps_what_the_namespace_holds` (1971).
- `tests/test_node_services.cpp` (1 of 6): `test_a_commit_is_published_only_once_its_objects_are_claimed` (362).

"A claim from an unseen branch survives" is `test_retention_claims_are_causal_durable_and_observed_remove`
by its name; check its body when the mapping is written.

**Cannot pass as they stand:** all of `tests/test_maintenance_trace.cpp` (behaviour B5), and
`test_disconnected_maintenance_sleeps_until_peer_event`,
`test_coalesced_delete_burst_wakes_at_exact_garbage_grace` (behaviour B2). See question 3.

## Step 8b: the lease template

### 8b.1 Macha files

- Changed: `src/torrent/torrent_request.{hpp,cpp}`: `TorrentRequest` becomes an instance of
  `dagda::lease::ClaimedCollection` (`dagda/lease/claimed_collection.h`) with traits for its
  groups, claim, duplicate key and record codec. `merge_torrent_requests` becomes the
  template's merge.
- Changed: `src/metadata/metadata.{hpp,cpp}` (the section at the SM16, SM17, SM19 position and
  in DLT9) and `src/metadata/metadata_merge.cpp`: they call the template.
- Unchanged in substance: `src/torrent/torrent_coordinator.{hpp,cpp}`: the takeover stays
  Macha's until `Leases::acquire` has renew and release, which wait on heads
  (`dagda/context.h:190-215`: records in memory, one node, lost on restart).

Macha 0.92.3 already took the two rule changes (a future-stamped tombstone is fresh, an
unkeyed delta record is refused; `8e0308e`), so they are no longer port differences.

### 8b.2 Facade

No key in the seven groups is affected.

### 8b.3 Config

None in the store. The 7-day tombstone window and the bounds are constants
(`src/torrent/torrent_request.hpp`, `src/torrent/torrent_coordinator.hpp`), which the
traits carry. `LeaseOptions` (`dagda/lease/keeper.h:33`) is unused at this step.

### 8b.4 The gate

**VALIDATION and crash matrix:** none is specific. **By the maintainer's ruling** these cases stay in
Macha and pin the bytes; Dagda pins the template on its neutral record.

- `tests/test_storage_metadata.cpp` (3 of 68): `test_torrent_requests_join_without_conflicts` (4568), `test_torrent_requests_ride_the_snapshot_and_its_deltas` (4764), `test_a_delta_refuses_a_torrent_request_with_no_id` (4841).
- `tests/test_hydration_catalogue.cpp` (5 of 44): `test_an_ingest_and_its_torrent_read_one_catalogue_state` (2233), `test_a_torrent_jobs_publication_travels_the_wire_and_reaches_the_api` (2270), `test_torrent_requests_belong_to_the_cluster_and_are_driven_by_their_owner` (2381), `test_a_torrent_job_is_saved_only_when_its_record_changes` (2653), `test_the_torrent_plugin_restores_its_jobs_and_runs_them_through_the_engine` (2765).
- `tests/test_rpc_cluster.cpp` (2 of 114): `test_torrent_listing_is_served_from_memory_never_a_survey` (1294), `test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node` (5907).

A byte-for-byte round trip of the torrent section against a fixture captured before the step
(SM16, SM17, SM19, DLT9). That fixture does not exist yet. The requirements' line numbers for
these cases are stale: the code is at `tests/test_storage_metadata.cpp:4568`, `:4764` and
`tests/test_hydration_catalogue.cpp:2381`.

## Step 8c: opaque heads

**Blocked:** `dagda/heads/` does not exist (H1 to H30), and Dagda's step 4 waits on the
tree-native merge. This section is what the step will cover, at `00861dd` names. It must be
re-verified at the merge commit.

### 8c.1 Macha files

- Moved, generic half into Dagda, owner's half into `src/filesystem/` as NEW files:
  `src/metadata/metadata.{hpp,cpp}`, `src/metadata/metadata_manager.{hpp,cpp}`,
  `src/metadata/metadata_server.{hpp,cpp}`, `src/metadata/namespace_tree.{hpp,cpp}`,
  `src/metadata/namespace_control_store.{hpp,cpp}`, `src/metadata/metadata_merge.cpp`.
- Becomes Macha's facade: `src/contract/metadata_view.hpp`.
- Changed: `src/service/convergence_demand.hpp`, `src/service/maintenance.{hpp,cpp}` (the metadata
  stages, head repair, truncation); `src/cluster/local_state.{hpp,cpp}` (the replica goes, so
  the class goes); `src/filesystem/filesystem.{hpp,cpp}`, `src/fuse/fuse_frontend.cpp`,
  `src/torrent/torrent_coordinator.{hpp,cpp}`, `src/api/manage_api.{hpp,cpp}`,
  `src/catalogue/catalogue.{hpp,cpp}`, `src/catalogue/media_catalogue.{hpp,cpp}` (each
  mutates `MetadataSnapshot` through `mutate_delta`); `src/cluster/telemetry.cpp` if the
  generation moves; `src/api/status_api.{hpp,cpp}`.
- Tools: `tools/metadata_history_dump.cpp`, `tools/metadata_inspect.cpp`.

### 8c.2 Facade

All of these are served by Macha's own code up to this step, and **all are MISSING in Dagda**,
because there are no heads. The list is what Dagda's `dagda.heads.*` (or the facade's call to
the Heads noun) has to supply, or what stays with the owner:

| Macha key | After 8c |
|---|---|
| `diagnostics.metadata.available`, `.historical_requests`, `.historical_reconstructions`, `.historical_deltas_applied`, `.materialization_cache_hits`, `.materialization_cache_misses`, `.materialization_cache_evictions`, `.materialization_cache_entries`, `.materialization_cache_bytes`, `.materialization_cache_limit_bytes`, `.history_records`, `.history_file_bytes`, `.history_resident_payload_bytes`, `.accepted_head_persistence_writes`, `.accepted_head_persistence_bytes`, `.accepted_head_persistence_failures` | MISSING: Dagda's head store (`src/api/status_api.cpp:911-934`) |
| `diagnostics.metadata.conflicts_superseded`, `.conflicts_resolved`, `.head_holders`, `.head_holders_present`, `.heads_set_aside`, `.mutations`, `.mutation_retention_ms_total`, `.mutation_retention_ms_max`, `.mutation_publish_ms_total`, `.mutation_publish_ms_max` | MISSING: Dagda's head store (`:953-970`) |
| `diagnostics.metadata.conflicts`, `.namespace_conflicts`, `.catalogue_conflicts`, `.tombstones` | stay the owner's, from the blob (`:939-951`) |
| `diagnostics.convergence.available`, `.events_received`, `.runs_scheduled`, `.runs_completed`, `.requested_epoch`, `.completed_epoch`, `.latest_generation`, `.scheduled` | MISSING: Dagda's reconciler, if convergence moves there (`:1292-1309`) |
| `cluster.metadata_generation`, `cluster.metadata_replicas`, `cluster.metadata_replicas_online`, `cluster.metadata_min_write_replicas`, `cluster.metadata_voters`, `cluster.metadata_voters_online`, `cluster.metadata_quorum_required`, `cluster.metadata_availability`, `cluster.metadata_read_available`, `cluster.metadata_quorum_available`, `cluster.metadata_write_available`, `cluster.metadata_replica_set_validated`, `cluster.metadata_quorum_validated`, `cluster.metadata_replica_set_validated_at_unix_ms`, `cluster.metadata_quorum_validated_at_unix_ms` | MISSING: the head set's status (`:786-804`) |
| `startup.metadata`, `startup.services` | MISSING: the node declares no metadata plane (`dagda/node.cpp:53-56`) |
| `nodes[].metadata_generation` | MISSING: an advert or a hosted field; `metadata_write_replicas_required` likewise (`TODO/ACTIVE.md`, M6) |

### 8c.3 Config

| Macha YAML key | Dagda option |
|---|---|
| `dht.metadata_write_copies` | as S.3, and the heads' merge copies: MISSING |
| `dht.metadata_cache_ms`, `dht.metadata_materialization_cache_bytes` | MISSING |
| `maintenance.convergence_settle_ms`, `.convergence_max_delay_ms` | MISSING |
| `cache.prefer_metadata` | `CacheOptions.seed`: the seam exists (`dagda/store/persistent_cache.h:99`), the seed does not |
| `filesystem.root_uid`, `.root_gid`, `.root_mode`, `bootstrap` as read by genesis | the owner's, at founding |
| history truncation's 256 records and 64 MiB | constants: no YAML key on either side |

### 8c.4 The gate

**VALIDATION:** "Memory". **Crash matrix:** "DATA barriers complete, metadata commit not
accepted", "Metadata committed, local FUSE completion not recorded", and the batch paragraph
under the matrix (`docs/durability.md:158-162`).

- `tests/test_metadata_merge.cpp` (19, all): `test_a_file_one_head_never_saw_is_kept` (115), `test_a_file_one_head_removed_stays_removed` (128), `test_an_edit_survives_a_concurrent_removal` (142), `test_the_newer_of_two_values_wins_without_a_conflict` (152), `test_concurrent_edits_install_the_later_and_keep_both` (162), `test_the_same_content_written_twice_is_no_conflict` (198), `test_a_rename_meets_a_concurrent_edit_of_the_same_file` (209), `test_a_rename_onto_an_existing_file_replaces_it` (238), `test_deciding_for_the_other_file_is_kept` (255), `test_two_renames_of_one_file_keep_one_name` (281), `test_the_same_rename_on_both_heads_is_one_file_and_no_conflict` (290), `test_a_removed_directory_returns_for_a_file_added_under_it` (303), `test_a_directory_with_entries_wins_a_clash_with_a_file` (317), `test_entries_from_before_provenance_merge_by_the_legacy_clock` (337), `test_a_returning_node_from_before_provenance_loses_nothing` (369), `test_a_decision_is_kept_against_a_head_that_has_not_seen_it` (421), `test_cluster_values_take_the_greater_and_both_heads_conflicts_stand` (459), `test_three_heads_merge_to_the_same_namespace_in_any_order` (495), `test_the_catalogue_root_follows_its_dot` (528).
- `tests/test_namespace_tree.cpp` (33, all): `test_the_tree_round_trips_every_namespace_it_is_given` (78), `test_the_tree_carries_each_entrys_provenance` (100), `test_the_root_is_a_function_of_the_entry_set_and_nothing_else` (136), `test_changing_one_file_rewrites_a_path_to_the_root_and_not_the_library` (159), `test_appending_an_extent_does_not_rewrite_the_extent_list` (188), `test_a_lookup_reads_a_path_to_the_root_rather_than_the_namespace` (218), `test_a_leaf_stays_small_however_many_extents_a_file_has` (236), `test_a_corrupt_node_is_refused_rather_than_trusted` (261), `test_a_stat_only_lookup_fetches_no_extent_nodes` (313), `test_an_sm14_record_points_at_the_namespace_instead_of_carrying_it` (406), `test_the_record_stops_growing_with_the_library` (435), `test_a_snapshot_never_carries_its_namespace_in_two_places` (452), `test_a_stat_against_a_decoded_record_never_materialises_the_namespace` (508), `test_a_damaged_record_is_refused_rather_than_trusted` (529), `test_a_snapshot_handed_to_a_reader_names_its_namespace_once` (555), `test_a_reader_sees_the_same_namespace_in_either_form` (579), `test_a_detached_namespace_without_a_store_refuses_rather_than_reporting_empty` (615), `test_an_incremental_update_produces_the_tree_a_rebuild_would` (641), `test_a_write_rewrites_a_path_rather_than_the_library` (704), `test_a_delta_applied_to_the_tree_lands_where_the_map_lands` (733), `test_history_replay_applies_a_delta_to_the_tree_not_the_map` (792), `test_a_stat_only_read_costs_no_extents_in_either_form` (835), `test_a_prefix_scan_descends_rather_than_walking_the_library` (874), `test_a_listing_reads_the_directory_not_its_descendants` (932), `test_a_scan_seeks_and_stops_where_it_is_told` (1003), `test_an_update_reaches_the_root_a_full_build_reaches` (1050), `test_an_update_reads_the_path_it_changes_not_the_tree` (1137), `test_every_tree_node_is_reachable_for_the_collector` (1182), `test_a_commit_claims_the_nodes_it_introduced_and_prunes_the_rest` (1203), `test_a_diff_is_the_paths_that_differ_and_reads_only_their_leaves` (1448), `test_distant_changes_read_no_leaf_between_them` (1489), `test_a_tree_merge_is_the_merge_of_the_materialised_heads` (1517), `test_baseline_reconcile_materialised_against_tree` (1640).
- `tests/test_namespace_migration.cpp` (15, all): `test_every_write_records_where_it_came_from` (160), `test_the_inventory_and_the_media_index_follow_the_tree` (255), `test_tombstones_live_in_batches_in_a_tree_backed_head` (398), `test_operations_waiting_on_a_commit_share_the_next_one` (487), `test_a_migrated_node_serves_and_writes_its_library` (557), `test_every_node_computes_the_same_record_from_the_same_head` (645), `test_a_migration_refuses_what_it_cannot_re_root_safely` (687), `test_the_pre_migration_state_is_kept_not_deleted` (762), `test_reconciling_two_tree_backed_branches_keeps_the_namespace` (806), `test_a_lagging_node_adopts_the_leaders_record_only_if_it_agrees` (876), `test_a_namespace_change_is_visible_as_a_change` (909), `test_a_tree_backed_delta_reconstructs_its_record` (946), `test_a_catalogue_change_on_a_tree_leaves_the_tree_alone` (988), `test_the_control_collector_keeps_an_unclaimed_namespace` (1026), `test_a_merge_claims_what_it_introduces` (1142).
- `tests/test_namespace_entries.cpp` (8, all): `test_pages_visit_what_the_callback_walk_visits_on_every_shape` (88), `test_a_resumed_page_reads_only_its_own_path` (108), `test_a_cursor_between_paths_resumes_after_it` (129), `test_cancellation_stops_before_an_entry` (148), `test_a_missing_node_or_store_throws` (159), `test_the_metadata_view_pages_a_live_namespace` (187), `test_a_node_that_is_not_a_tree_node_throws` (225), `test_a_manager_without_a_store_pages_a_map_backed_view` (242).
- `tests/test_storage_metadata.cpp` (53 of 68): `test_metadata_record_payload_copy_is_shared` (488), `test_metadata_seed_sibling_replays_after_restart` (518), `test_metadata_hash_streaming_matches_canonical_encoding` (564), `test_metadata_delta_in_place_preserves_namespace_storage` (578), `test_metadata_branch_merge_history_roundtrip` (619), `test_metadata_commit_store_acceptance_heads_roundtrip` (737), `test_accept_commit_refreshes_checkpoint_on_both_repair_and_ordinary_paths` (874), `test_merge_delta_primary_may_precede_merge_generation` (931), `test_merge_delta_with_generation_gap_reconstructs_after_cache_eviction_and_reopen` (1004), `test_unreconstructable_accepted_head_is_kept_at_startup_and_reanchored_live` (1109), `test_full_fallback_boundary_heals_when_parent_arrives` (1271), `test_compacted_direct_predecessor_cannot_resurface_as_head` (1353), `test_unreconstructable_accepted_head_is_rate_limited_not_hammered` (1425), `test_history_transfer_follows_only_materialization_dependencies` (1519), `test_pristine_joiner_adopts_compacted_cluster_head` (1539), `test_a_head_with_no_clock_is_nobodys_ancestor` (1678), `test_a_head_from_before_truncation_is_recognised_by_its_clock` (1697), `test_returning_replica_adopts_a_head_whose_ancestry_a_peer_truncated` (1748), `test_a_checkpoint_write_holds_no_reader_of_the_replica` (1807), `test_a_journal_compaction_holds_no_reader_of_the_replica` (1867), `test_a_head_set_aside_for_the_horizon_is_dropped` (1923), `test_the_set_aside_time_of_a_head_no_longer_held_is_forgotten` (1982), `test_metadata_recovery_cache_seed_is_not_accepted_authority` (2019), `test_metadata_journal_mid_frame_corruption_truncates_not_reseeds` (2064), `test_metadata_conflict_alternatives_are_gc_roots` (2140), `test_metadata_legacy_prepare_journal_replays_deterministically` (2183), `test_protocol20_checkpoint_without_acceptance_never_self_promotes` (2243), `test_an_author_sequence_never_regresses` (2286), `test_a_broken_author_chain_starts_a_new_author` (2312), `test_a_lost_author_record_starts_a_new_author` (2346), `test_protocol20_delta_preserves_governance_snapshot_encoding` (2364), `test_metadata_delta_chain_reuses_bounded_materialized_head` (2420), `test_metadata_history_payloads_are_disk_backed_and_byte_bounded` (2544), `test_cold_accepted_head_validation_caches_only_owned_results` (2603), `test_metadata_materialization_cache_never_validates_corrupt_delta` (2665), `test_metadata_dlt7_presence_flags_round_trip` (2756), `test_metadata_dlt8_append_extents_delta` (2846), `test_metadata_merge_over_append_ordered_tombstones_is_a_delta` (2911), `test_metadata_superseded_conflicts_leave_the_snapshot` (2944), `test_metadata_merge_delta_preserves_standing_conflicts` (3046), `test_metadata_delta_child_of_merge_commit_reconstructs` (3132), `test_metadata_delta_tombstone_edits_are_linear` (3206), `test_an_entrys_provenance_survives_snapshot_and_delta` (3298), `test_a_written_entry_is_stamped_against_what_was_there` (3334), `test_the_legacy_clock_survives_snapshot_and_delta` (3420), `test_protocol20_state_rejects_legacy_authority_mutators` (3468), `test_metadata_codec_and_replica` (3839), `test_metadata_identity_rpc` (4315), `test_genesis_root_configuration` (4422), `test_metadata_delta_rejects_unrepresentable_garbage_reordering` (4441), `test_local_metadata_store_falls_back_from_invalid_delta` (4472), `test_decoded_extent_vectors_carry_no_allocator_slack` (4499), `test_tombstone_batches_ride_the_head_and_its_deltas` (4658).
- `tests/test_models.cpp` (1 of 3): `test_metadata_delta_state_model` (43).
- `tests/test_absent_nodes.cpp` (2 of 8): `test_a_node_unheard_of_for_the_horizon_is_forgotten` (153), `test_forgetting_counts_from_when_the_node_was_last_heard_of` (190).
- `tests/test_rpc_cluster.cpp` (22 of 114): `test_joiner_cannot_form_genesis` (2914), `test_bootstrap_joiner_requires_complete_checkpoint_survey` (2935), `test_nodes_seeking_different_copy_counts_share_one_namespace` (2995), `test_two_node_mutual_bootstrap_metadata_write_floor` (3049), `test_service_metadata_repair_coalesces_real_generation_burst` (3147), `test_failed_convergence_retries_once_per_burst_of_events` (3263), `test_service_same_generation_sibling_notice_triggers_reconciliation` (3491), `test_a_head_from_an_unknown_history_is_merged` (3669), `test_a_head_that_cannot_be_merged_is_set_aside` (3732), `test_concurrent_reads_during_divergence_produce_one_reconciliation` (3785), `test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` (3958), `test_a_file_touch_commits_while_a_holder_is_away` (4135), `test_commit_replicas_ordered_local_then_nearest` (4238), `test_disjoint_metadata_pairs_branch_and_reconcile` (4511), `test_a_durable_trio_founds_commits_and_reads_through_every_node` (5039), `test_metadata_commits_with_whichever_nodes_remain` (5069), `test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes` (5509), `test_a_catalogue_conflict_decided_for_the_root_in_place_is_removed` (5637), `test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again` (5679), `test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges` (6214), `test_history_is_not_truncated_while_two_heads_are_held` (6299), `test_unreconstructable_accepted_head_is_repaired_live_from_a_peer` (6363).
- `tests/test_invariants.cpp` (2 of 40): `test_convergence_demand_coalesces_burst_and_keeps_same_generation_event` (112), `test_metadata_availability_logs_only_transitions` (1870).
- `tests/test_service_root.cpp` (2 of 12): `test_standing_conflicts_are_listed_and_resolved_over_http` (496), `test_conflict_routes_say_when_metadata_is_unavailable` (637).

Beyond the cases: `macha-metadata-dump` output identical on the fixture; the mixed-build test
as the requirements redefine it (commits both ways, a divergence merged, the result identical
from either side); a `replicas: 3` space on five in-process nodes (H20).

**Corrections to the requirements' gate.** `test_metadata_history_checkpoint_concurrent_proposers_converge`
does not exist at `00861dd`, and I found no such name in `tests/`. The nearest by name are
`test_disjoint_metadata_pairs_branch_and_reconcile` and
`test_concurrent_reads_during_divergence_produce_one_reconciliation`. The "fi-1" soak names a
node that is gone: it is live-a and live-b.

**Likely to go with the tree-native merge, so not to be counted as a drop then:** the three
migration cases of `tests/test_namespace_migration.cpp`
(`test_a_migrated_node_serves_and_writes_its_library`,
`test_a_migration_refuses_what_it_cannot_re_root_safely`,
`test_the_pre_migration_state_is_kept_not_deleted`) and the legacy-authority cases of
`tests/test_storage_metadata.cpp` (`test_metadata_legacy_prepare_journal_replays_deterministically`,
`test_protocol20_state_rejects_legacy_authority_mutators`). I read the branch only by the
commit summaries in the requirements document, so the Macha session confirms this at the merge.

## Step 8d: the context as the API

### 8d.1 Macha files

- `src/subsystem/subsystem.hpp` (`SubsystemContext`, `src/subsystem/subsystem.hpp:41`, stops
  handing out `filesystem`, `hydration`, `ingest` and `local_state`),
  `src/subsystem/subsystem.cpp`, `src/subsystem/subsystem_abi.hpp`,
  `src/subsystem/subsystem_registry.{hpp,cpp}`, `src/subsystem/subsystem_supervisor.{hpp,cpp}`.
- `src/fuse/fuse_subsystem.{hpp,cpp}`, `src/fuse/fuse_plugin.cpp`, `src/torrent/torrent_plugin.cpp`:
  look their services up by name. `tests/plugins/` follows.
- `src/service/node_services.{hpp,cpp}`, `src/service/service.{hpp,cpp}`: the root builds the
  FS through `dannan::Context` and `dagda::Context` alone. Macha's blocking forms become waits
  on completions.
- `src/filesystem/filesystem.{hpp,cpp}`, `src/filesystem/hydration.{hpp,cpp}`: the constructor
  takes contexts, not `DistributedStore`, `Membership` and `LocalState`.
- `src/cluster/node_resources.{hpp,cpp}`, `src/cluster/local_services.{hpp,cpp}`: gone or empty.

### 8d.2 Facade

No key in the seven groups changes its meaning. The facade stops reaching `node.store()`,
`node.credit()`, `node.data()` and `node.repair()` if the root holds only the contexts:
everything marked NO KEY in S.2 must by then be a published `dagda.*` or a Context operation,
or the root still names the node. See question 5. `subsystems[]`, `threads[]` and
`diagnostics.filesystem` keep their sources.

### 8d.3 Config

`plugin_path` and `fuse.*` stay as they are. No store key moves.

### 8d.4 The gate

**VALIDATION:** "Mountpoint preflight". **Crash matrix:** "Before local FUSE admission is
durable", "Durable FUSE spool/journal, no DATA placement", "Metadata committed, local FUSE
completion not recorded". Macha's root compiles against the two contexts' headers alone, and
the FS runs as a hosted system on a two-node cluster with the full suite.

- `tests/test_subsystem_supervisor.cpp` (4, all): `test_plugin_discovery_settles_each_kind_of_file` (112), `test_builtin_lifecycle_settles_each_outcome` (188), `test_stop_cancels_a_blocked_factory` (257), `test_service_stops_plugins_before_the_store` (285).
- `tests/test_fuse_subsystem.cpp` (7, all): `test_fuse_subsystem_mounts_and_publishes_its_frontend` (139), `test_fuse_subsystem_clean_stop_is_not_a_fault` (170), `test_fuse_subsystem_rebuilds_the_mount_after_losing_it` (197), `test_fuse_subsystem_construction_fault_leaves_the_rest_of_the_node_serving` (244), `test_fuse_subsystem_without_a_mount_path_is_unavailable` (302), `test_fuse_subsystem_without_a_mount_driver_is_unavailable` (327), `test_fuse_plugin_loads_over_the_real_dlopen_path` (351).
- `tests/test_node_services.cpp` (6, all): `test_services_never_started_stop_nothing_and_answer_until_destroyed` (117), `test_a_reload_reaches_every_service_with_live_limits` (151), `test_media_routes_answer_from_this_nodes_files_and_engine` (193), `test_a_node_without_streaming_says_it_cannot_profile` (288), `test_observation_gauges_follow_activity_repair_and_the_mount` (306), `test_a_commit_is_published_only_once_its_objects_are_claimed` (362).
- `tests/test_service_root.cpp` (12, all): `test_a_service_never_started_stops_in_order_once` (81), `test_a_failed_startup_is_what_every_caller_is_told` (189), `test_a_failed_startup_with_no_reason_still_says_it_failed` (197), `test_a_stall_before_the_control_plane_names_it_starting` (205), `test_a_stalled_startup_with_nobody_to_tell_ends_the_process` (246), `test_a_stop_once_local_state_has_recovered_builds_no_services` (304), `test_a_stop_once_services_are_built_starts_none_of_them` (308), `test_a_request_is_admitted_by_session_capability_and_role` (337), `test_each_route_reaches_the_api_that_owns_it` (399), `test_an_api_request_is_viewer_work_and_a_viewer_present` (470), `test_standing_conflicts_are_listed_and_resolved_over_http` (496), `test_conflict_routes_say_when_metadata_is_unavailable` (637).
- `tests/test_filesystem_fuse.cpp` (17, all): `test_fuse_signal_exit_is_a_clean_service_shutdown` (224), `test_a_node_clears_only_its_own_abandoned_write_files` (231), `test_fuse_journal_frame_scanner_exhaustive_tail_model` (260), `test_publication_writers_stage_and_commit_generations` (343), `test_publication_writer_failure_stays_invisible` (762), `test_local_snapshot_view_is_local_before_cluster_forms` (788), `test_fuse_frontend_local_semantics` (810), `test_fuse_publication_scheduling` (1321), `test_fuse_admission_backpressure` (1749), `test_fuse_publication_failures_back_off_and_park` (2003), `test_fuse_publication_backlog_wider_than_ledger_completes` (2128), `test_fuse_namespace_journal_recovery` (2314), `test_fuse_data_journal_recovery` (2889), `test_management_api_reports_and_acts_on_fuse_state` (3369), `test_removing_empty_directories_in_a_burst_keeps_the_node_up` (3680), `test_disconnected_maintenance_sleeps_until_peer_event` (3777), `test_coalesced_delete_burst_wakes_at_exact_garbage_grace` (3866).
- `tests/test_foundations.cpp` (2 of 47): `test_fuse_mountpoint_preflight_counts_stray_entries` (1573), `test_fuse_mountpoint_preflight_refuses_unrelated_filesystem` (1585).

## Migration plan

**The tool as it stands** (`docs/migration.md`, and this repository's `dagda_migrate.cpp` under its `tools/`): in place, one
record at a time, each opened once under v18 (the ruling keeps this, so rot is listed), its
16-byte tag derived and overwritten; pack records 48 bytes; ledgers re-sealed file by file;
markers last; every refusal before the first write; a crash finished by running it again.
Not proven: no run on a real Macha state, none at scale, and a dry run reads every byte
(`TODO/COMPLETED.md:685-686`). `TODO/ACTIVE.md` records about one fsync per loose object, not
measured at scale: about 1 million on live-a.

### Order

1. The fixture (below), migrated and opened by `dagda::Node`.
2. M10's Linux run with `syncfs`, which no run has exercised on Linux
   (evaluation section 4.2 item 5).
3. The rehearsal on a copy of live-b.
4. The window decided from its measurement.
5. The flag day: live-b and live-a.

### The fixtures

They come after Macha's tree-native merge, cut from the merged code by the Macha session:
`build/macha-fixtures/step<n>-*/` with a README naming the commit and config, and a real
state directory with `metadata/` included, plus `macha-metadata-dump` output (the maintainer's order,
`TODO/ACTIVE.md`, "Settled with it"). Nothing there today but `gen/` and `placement/`. Dannan
wants the same directory for `dannan-import-macha`. The Dagda session never builds or runs
Macha. What the fixture must exercise: `metadata/` non-empty, `retention/ledger` marker
present, `ledger/referenced/{extents,nodes}`, `retention/unreferenced-{data,control}.bin`,
`backend-identities/*.id`, a torn pack tail, a pack with tombstones, a cache root with
`metadata/current.meta`.

### Dannan's side of the state

The second cutover finds the first one's files. What must be true before `dagda-migrate` runs,
on each node:

- **`<state>/node-id` is Macha's id, unchanged.** Dannan's file is compatible and left alone
  (`../dannan/TODO/2026-11-04-macha-parity.md` section 1). The tool reads
  `<node-state>/node-id`, never creates it, and refuses a backend naming another id before any
  write (`dagda/migrate/migrate.cpp:631-638`).
- **Roster, inbound and telemetry files are Dannan's:** `membership/known-nodes.bin` is
  `DNNMEM01`, `connectivity/inbound.bin` is `DNNINB01`, `telemetry/last-known.bin` is
  `DNNTEL01`, the originals beside them as `*.macha`. This is `dannan-import-macha`, planned in
  the parity document and **not built**: it is not in `../dannan/tools/` at `4742cb4`. The
  first cutover's soak proves it, or the first cutover has not happened.
- **Macha's hosted telemetry layout is fixed before the first `DNNTEL01` is written.** The
  fields S.2 marks "hosted field" must be in it from the first cutover, or the last-known
  fallback in `src/api/status_api.cpp:568-577` loses them across the second. Field numbers from
  1024 (`../dannan/dannan/status/telemetry.h:63`).
- **Custody exists, under its final key source: `<node-state>/identity/custody`.** Every ledger
  is sealed under the node-local key derived from it, and it is never replaced
  (`../dannan/dannan/identity/key_custody.cpp:155`). The first cutover does not need it, so it is
  made with `dannan-node --state DIR --init-custody CUSTODY`, node stopped, before the rehearsal
  of that node. **Losing it after migration loses the claims and the reference counts** on that
  node (the held tries reseed from disk; I did not test what a node does with neither). So: the
  custody file and the credential root are backed up off the node first. See question 2.
- **The state directory question.** `NodeOptions.state` says "A directory of its own, never
  Dannan's state path" (`dagda/node.h:143-144`), and `docs/migration.md:35-36` says either use
  Macha's state directory as Dannan's or copy `node-id` across. The locks do not collide
  (`.macha.lock`, `dagda/store/local_store.h:50`; `.dannan.lock`,
  `../dannan/dannan/identity/node_identity.h:23`). Macha holds all of it under `state_path` today.
  See question 4.
- **The wrap key's advert:** at most 64 adverts per node, 1 KiB each
  (`TODO/ACTIVE.md`), and `domain::advertise_wrap_key` publishes one with the certificate's DER
  when one exists (`dagda/domain/wrap.cpp:158-161`). Not a problem under a cluster key by my
  reading; not tested on these nodes.

### On each node first (not verified: I have no access)

- `retention/ledger` exists; no `claims.meta`, `claims.current` or `checkpoints/` under
  `retention/`. The tool refuses otherwise, before any write.
- Free space on the state filesystem: the largest ledger once more
  (`docs/migration.md:98-99`), and 256 MiB afterwards, since `ControlStoreOptions.reserve_free`
  is new: a state disk with less refuses CONTROL puts after the flag day though it accepted them
  before.
- The configured backends, control path and cache path from each node's own YAML.
  `tests/fixtures/node-configs/` has `fi-1.yaml` and `gbni-1.yaml` only, and neither node exists.

### The rehearsal: a copy of live-b

583 GB at the evaluation's date, and still filling; measure it again on the day. Why live-b:
its DATA is replaceable from live-a. Why a copy: nothing live is at risk and it can be
repeated. The copy needs 583 GB free on storage of the **same kind** as the real disk, attached
to a Pi 5 the same way, or its timing says nothing about live-a's USB disk. Where that is, I
do not know. See question 6.

1. Stop live-b, copy `<state>` and the backend, record the copy's object counts, start
   live-b. Stopping it during the copy is a measured cost of its own; the copy could also be
   a live copy plus a stopped final pass. Unmeasured.
2. Take the listing: every object id, loose and packed, in DATA and CONTROL, and every claim,
   from the v18 copy. No tool does this today: a small verifier is written under `build/`,
   linked against `dagda/migrate/`'s readers, and it runs again after step 5.
3. `--dry-run`. Time it; compare its report with the listing.
4. The real run, under `time`. Keep the whole of standard output: counts, unreadable records
   by file and id, and the progress lines every 4096 records.
5. `dagda::Node` opens the copy, with `dagda-migrate`'s `--node-state`: `has` and `get` for every
   id in the listing, `retained` for every claim, on the copy's Dannan state. A record left
   unreadable by the tool is compared against live-a's copy, as rot or as the tool's fault.
6. Run the tool again: it should report nothing to do. Time it.
7. On a second copy of a subset, if the space exists: `kill -9` the tool mid-run and in the
   ledger phase, run again, and check the listing.
8. `--reverse` on the copy, once. The reverse gates nothing; the run is recorded so that its
   state is known, not because anything waits on it. Macha 0.92.3 opening a reversed disk has
   never been done (`TODO/COMPLETED.md:685-686`), and this is the cheap place to try.
9. Cold start on the migrated copy: time to ready and peak RSS, against the baseline when
   evaluation section 6 item 4 has measured one.

### What the rehearsal must measure, because nothing has

| Figure | State |
|---|---|
| The tool's time on live-b's copy: total, DATA, CONTROL, ledgers | unmeasured |
| Its read rate, from the progress lines | unmeasured |
| Fsyncs per loose object, and their cost on that disk | one each by the code (`dagda/migrate/migrate.cpp:110`); cost unmeasured |
| The dry run's time | unmeasured; expected close to the real run, since it reads every byte |
| live-b's loose object and pack counts | unknown here |
| RSS during the run | unmeasured |
| First start after migration: pack scan, ledger seed, the first survey | unmeasured on v19. live-a's v18 start is recorded as data storage ready at +17 s, metadata at +35 to +40 s, services at +42 s (`../macha/TODO/ACTIVE.md`, item 2) |
| Size and object count of each node's cache | unknown here |
| live-a's own time | unmeasured until it happens |

### Deciding live-a's window

The tool's cost has at least two terms: bytes read and objects fsynced. So live-b's run gives
a rate per byte and a rate per object only if the progress lines separate them; one total does
not. From the run: a per-byte rate over the phase with large objects, and a per-record rate from
the same lines. Then live-a's estimate is:

    bytes x its rate + objects x its rate + its fixed work

with live-a's figures: 4 TB, about 1.03M loose objects (1,031,852 in the evaluation), 17 packs,
1.7 GB CONTROL, 161 MB retention and 145 MB ledgers to re-seal. Its 42 MB of metadata is not
touched. That averages about 3.9 MB per loose object; live-b's average may differ, which is why
one number scaled by bytes is not enough. Add the first start, and a margin the maintainer sets. The rule,
if the measured rate is not trusted across the two disks: run it once on live-a with `--dry-run`
in a window of its own, which is non-destructive and itself measures live-a's read rate, at the
cost of doing that read twice. No duration is proposed here, because none has been measured.

### The flag day

1. Both nodes at the same release, gate green on live-a, the soak of the first cutover closed.
2. Backups off each node: custody, `node-id`, `storage-layout`, `backend-identities/`, each node's
   YAML. No backup of DATA exists at this size; the reverse and live-a's and live-b's copies
   of each other are what there is.
3. Stop both.
4. `dagda-migrate --dry-run` only if the window allows. The real run:
   `dagda-migrate --state S --node-state N --cluster-key K --credential NAME --backend B... [--control C] [--cache D]`.
   Exit 0 is v19. Exit 1 is refused, with the reason on standard error.
5. live-b and live-a run at the same time: nothing in the tool talks to a peer. See question 7
   for which starts first.
6. Keep every report. Unreadable records are expected to be few or none; each is listed, and
   repair restores it from the other node if the other holds it. An object on live-a alone
   that is unreadable is lost, as it was before the migration, only now it is known.

**Acceptance, in order:** `has` and `get` against the listing taken before (each node's listing is
taken after the stop, by the rehearsal's verifier; its time is part of the window and is unmeasured). The
survey: `method=diff`, peers answered. Repair converges: `diagnostics.repair.unsourceable_objects`
not climbing, `diagnostics.repair.passes_completed` moving. Reads and writes through FUSE and the
API. The evaluation's baselines where they exist. Then the soak, whose length is question 8.

**If it fails.** Before a v19 write, nothing has changed: the node refuses and names the reason.
Partway, the disk is finished by a second run, or taken back by `--reverse`, which the rulings keep
available and gating nothing. After v19 and after writes: `--reverse` carries what Dagda wrote back
with it, and the node goes back to the first cutover's release, whose claims and references are
those the reverse re-sealed. No rollback beyond that has been tried.

## MISSING in Dagda

Complete as far as this read goes. I compared the public names of Macha's store classes with
Dagda's by script, read `src/api/status_api.cpp` in full, `src/config.cpp`'s store keys, and the
parts of `src/service/maintenance.cpp` named below. I did not compare every internal behaviour of
files moved earlier; those rest on Dagda's moved tests and known answers.

### A. Status keys

"Figure exists" means Dagda holds it and publishes no `dagda.*` key; the Macha facade can read it.

| Macha key(s) | Evidence in Macha | In Dagda |
|---|---|---|
| `diagnostics.metadata.*` other than the four below (all 26) | `src/api/status_api.cpp:909-970` | no figure: heads |
| `diagnostics.metadata.conflicts`, `.namespace_conflicts`, `.catalogue_conflicts`, `.tombstones` | `src/api/status_api.cpp:947-950` | no figure; stays the owner's |
| `diagnostics.convergence.*` (8) | `src/api/status_api.cpp:1293-1304` | no figure |
| `cluster.metadata_*` (15) | `src/api/status_api.cpp:786-804` | no figure: heads |
| `startup.metadata` | `src/api/status_api.cpp:828` | no plane (`dagda/node.cpp:56`) |
| `nodes[].metadata_generation` | `src/api/status_api.cpp:272-276` | no figure: heads |
| `nodes[].cache.capacity_bytes` | `src/cluster/storage_server.cpp:55` | no figure; computable locally |
| `nodes[].cache.*`, `nodes[].storage_backends_online` for peers | `src/cluster/telemetry.cpp:570-571`, `:580`, `:597-599` | no advert: Macha's hosted telemetry fields |
| `diagnostics.repair.last_credit_bytes` | `src/api/status_api.cpp:1342` | no figure: the node discards `RepairPass::credit` (`dagda/node.cpp:644`) |
| `diagnostics.repair.unsourceable_sample`, `.push_examined`, `.pull_examined`, `.push_phase_complete` | `src/api/status_api.cpp:1324-1335` | figure exists, `RepairDiagnostics` |
| `diagnostics.repair.paced_by` | `src/api/status_api.cpp:1346-1354` | figure exists, `RepairGates::paced_by` |
| `diagnostics.data_resources.*` but `used_bytes`, `device_pressured`, `pressure_refusals` (17) | `src/api/status_api.cpp:1011-1036` | figure exists, `DataResourceStats` |
| `diagnostics.data_store.*` (6) | `src/api/status_api.cpp:1087-1097` | figure exists, `StoragePool::diagnostics` |

### B. Config keys

| Macha YAML key | Evidence | In Dagda |
|---|---|---|
| `dht.metadata_cache_ms` | `src/config.cpp:228-229` | none: heads |
| `dht.metadata_materialization_cache_bytes` | `src/config.cpp:230-232` | none: heads |
| `maintenance.convergence_settle_ms`, `maintenance.convergence_max_delay_ms` | `src/config.cpp:273-278` | none |
| `dht.io_pressure_overhead_ms`, `dht.io_pressure_per_mib_ms`, `dht.io_pressure_slowdown_percent`, `dht.io_pressure_release_percent`, `dht.io_pressure_outlier_percent` | `src/config.cpp:214-223`, `src/cluster/local_state.cpp:96-101` | not in `NodeOptions`; `StoragePool::configure_service_monitor` after open |
| `storage.hosts_extents` auto | `src/config.hpp:589-592`, `src/cluster/cluster.cpp:51-61` | bool only; the owner resolves |
| `maintenance.background_concurrency` 0 | `src/config.hpp:88-91` | no zero rule; the owner resolves |

Defaults that differ, each of which breaks Macha's behaviour if the facade forgets it:
`dht.write_copies` (1, `src/config.hpp:647`; `dagda/store/distributed_store.h:221`, 2);
`maintenance.background_concurrency` (`dagda/node.h:61`, 4); `RepairOptions.peer_window`
(`src/service/maintenance.cpp:432-433`, 30 s at the default interval; `dagda/store/repair.h:43`, 3 s).

### C. Behaviours

**Needed for the flag day:**

- **B1. The transport before the store.** Macha starts its node, then recovers local state on
  another thread (`src/service/service.cpp:582-586`, `:515`), so a recovering node answers peers and
  Status. `dagda::Node` must be constructed before the Dannan node starts
  (`dagda/node.cpp:96-100`, `:108-115`) and its constructor waits on the devices (`dagda/node.h:226-227`),
  so the control plane is dark while the pool, ledgers and control store open. Needs a two-part open:
  register and declare first, open after start.
- **B2. GC's wake.** Macha sleeps until an event and arms an exact wake for the earliest retirement
  and the orphan grace (`src/service/maintenance.cpp:1086-1098`, `:1248-1279`). The node rests a fixed
  `gc_interval` after a settled pass (`dagda/node.cpp:640`). It also never sets
  `PassDue::gc_waiting_for_event` (`dagda/node.cpp:419-423`), which Macha feeds to the gates
  (`src/service/maintenance.cpp:879`). And the node polls everything every `interval`, where Macha's
  settled node stays quiescent (`src/config.hpp:79-81`); its idle cost is unmeasured.
- **B3. `hosts_extents` changes at run time.** `auto` follows the dial-back
  (`src/cluster/cluster.cpp:426`). `StorageServerOptions.hosts_extents` is fixed at open
  (`dagda/store/storage_server.h:42`, `dagda/store/storage_server.cpp:81-84`), and `NodeReload` does
  not carry it.
- **B4. `dht.write_copies`.** Macha's put seeks `min(write_copies, nodes)` copies
  (`src/cluster/distributed_store.cpp:227`). At `9c190a9` Dagda's Macha-named forms defaulted to
  `persisted`, floor 1 whatever `write_copies` says (`dagda/store/distributed_store.cpp:710`), which is
  wrong above 1. Another session has corrected this in the working tree, uncommitted: named no level
  they seek `write_copies` (`dagda/store/distributed_store.h:404-406`,
  `StoreDistributedStore.MachasFormsSeekWriteCopies`). What stays the facade's: set
  `store.write_copies` from `dht.write_copies`, since the store's default is 2 and Macha's is 1.
- **B5. The decision trace through the node.** `tests/test_maintenance_trace.cpp` compares traces against
  `tests/fixtures/maintenance-traces/`. `ledger::Collector` takes a `Trace`
  (`dagda/ledger/collector.h:124`); `dagda/node.cpp:413-415` passes none, `NodeOptions` has no field
  for it, and repair's own lines are absent: `gate.repair` (`src/service/maintenance.cpp:594`) and the
  claim walk's (`:701`, `:707`) have no counterpart in `dagda/store/repair.cpp`.
- **B6. Observations.** Absent from Dagda: `maintenance.repair.step_us.*`, `.bytes.*`,
  `.push_examined.*`, `.pull_examined.*`, `.retained_repairs` (`src/service/maintenance.cpp:816-825`),
  `maintenance.inventory.build_us` (`:646`), `maintenance.release_horizon.build_us` (`:930`),
  `maintenance.gc.step_us` and `.per_object_ns` (`:1058`, `:1062`), the `cluster_stable` event (`:867`),
  and the slow-stage lines (`log_slow_stage`). The operator's memory and watches read these. The test
  is `test_observation_gauges_follow_activity_repair_and_the_mount`.
- **B7. `metadata_dirty`.** Tombstone collection needs the owner's state converged:
  `garbage_due = !busy && !metadata_dirty` (`src/service/maintenance.cpp:597`). The node sets
  `due.garbage_due = !busy` (`dagda/node.cpp:420`), with nowhere for the owner to say it is dirty.
- **B8. Inventory while busy.** Macha rebuilds the inventory when repair alone is due, even while busy
  (`src/service/maintenance.cpp:624`, `:635-645`). In the node only the collector builds it, only when
  GC or tombstones are due (`dagda/ledger/collector.cpp:98`), and those are `!busy`
  (`dagda/node.cpp:418-421`). So on a node always loading, repair and the survey run on an old
  inventory. An ingesting node going without GC is Macha's own rule (evaluation section 8 item 5); this is
  the inventory, which Macha does not hold back. `Node::publish_inventory` exists and nothing calls it
  on a schedule.
- **B9. Pass order.** Macha's pass is inventory, survey, claim walk, repair, then the gates and sweeps
  (`src/service/maintenance.cpp:625`, `:677`, `:730`, `:792`, `:883`). The node runs GC, then repair
  and the local stages (`dagda/node.cpp:638-650`). A trace fixture holds the order, so B5 depends on this.
- **B10. Store operations with no counterpart in `dagda::store::DistributedStore`,** by name,
  called from outside the store:
  `hydrate`, `hydration_available` (`src/cluster/distributed_store.hpp:457-458`,
  `src/filesystem/hydration.cpp:601`, `:669`); `cache_local` (`:456`,
  `src/filesystem/filesystem.cpp:405`); `held_here` (`:402`, `src/catalogue/catalogue.hpp:507`);
  `retain_data_here` (`:429`, `src/service/node_services.cpp:510`); `reachability_epoch` and the
  repair retries it resets (`:195`, `:507`, `src/fuse/fuse_frontend.cpp`); `DurabilityBatch` and
  `durability_barrier(batch)` (`:134`, `src/cluster/distributed_store.cpp:504`,
  `src/filesystem/filesystem.hpp`, `src/catalogue/catalogue.cpp`), whose counterpart is the ticket and
  needs a mapping, not just a rename; `get_shared` (`:441`), whose counterpart is `read().object`;
  `foreground_activity`, `interactive_activity`, `loader_activity`, `idle_for`, `take_*_bytes`
  (`:461-463`), whose counterpart is Dannan's activity clocks, though I did not check `take_*` semantics.
- **B11. The cache seed for Macha's replica.** `MetadataReplica` takes `cache_->metadata()` and the
  cache remembers each commit (`src/cluster/local_state.cpp:166`, `:172`,
  `src/metadata/metadata_server.cpp:96`, `src/storage/persistent_cache.hpp:68-69`). Dagda's cache has
  only `with_seed` (`dagda/store/persistent_cache.h:99`), so until 8c the facade keeps the seed itself
  in that place. Not hard, but nothing does it, and `test_metadata_recovery_cache_seed_is_not_accepted_authority`
  is its gate.
- **B12. Predicates.** `src/contract/predicates.hpp`, `src/ledger/predicate_query.cpp`,
  `tests/test_predicates.cpp`. No counterpart, and no production caller that I could find. The mapping
  holds all five cases as GAP pending whether they move.
- **B13. A deadline on the local barrier** remains open on both sides (`TODO/ACTIVE.md`, "M4 but for
  the barrier's deadline"). It is Macha's list for the maintainer, repeated here because the store step moves it.
- **B14. A stop during a long walk**: the same hang as Macha's P0 (`TODO/ACTIVE.md`, "Waiting on Macha's
  fix"). live-a's walk is the one that showed it.
- **B15. What the test mapping's GAPS section owes**, each named there with its Macha case and nearest
  Dagda case (`TODO/2026-11-05-test-mapping.md`, "GAPS", 27 cases): barriers over the wire and a
  stale-epoch refusal; the nondominated frontier of `DurabilityBatch`
  (`test_durability_batch_retains_exact_nondominated_frontier`, which B10's mapping must answer); a
  sender's repair healing a corrupt remote copy; the arbiter's background ceiling, reserve and class
  stats; `ControlObjectFetch` against wrong answers and failing peers; R=1 monotonic placement when a
  node is added; the replica selector's speculative rotation; write-behind's carry and bytes; a `Node`
  up over an unversioned backend. Those are test gaps as much as code; I have not re-derived them.

**Needed for 8c and after:** all of H1 to H30, the `metadata` plane, `ordered` registration, the
reference-count diff seam, head repair and truncation (`src/service/maintenance.cpp:1143-1159`), lease
records in a head with renew and release (M1), and Context operations for whatever S.2 marks NO KEY.

**Not proven, and not missing:** Dagda's scale (a 1M-id trie, 100,000 objects across packs) and its
Linux `syncfs` path (M10); `dagda-migrate` on a real state; every performance figure.

### Looks like a defect

Reported, not fixed; both trees are as they were.

1. **Dagda: `data_storage` stays unready forever after an all-offline start.** `dagda/node.cpp:172-175`
   marks the plane ready only in `open()`, and no other caller exists in `dagda/node.cpp`, including
   `reconfigure`. A node that starts with its disk absent and gets it back never reports ready without a
   restart. Macha marks the plane once the pool is built, whatever is online
   (`src/cluster/local_state.cpp:114`), so this also differs from Macha's answer: such a node shows
   `startup.data_storage` as recovering. I read this and did not run it.
2. **Dagda: B8**, if it is not intended. `dagda/node.h:219-224` says repair "runs on whatever inventory
   was last published", so it may be deliberate; but it is not Macha's behaviour.
3. **Dagda: B4**, found independently here and by the test mapping, and being corrected in the working
   tree as this is written.
4. **Dagda: a stale comment.** `dagda/node.h:143-144` contradicts `docs/migration.md:35-36` on whether
   the two state paths may be one directory.
5. **Macha: a comment.** `src/config.hpp:594-595` says the control store keeps "an eighth" of the ledger
   cache; `src/cluster/local_state.cpp:38` gives it a sixteenth. Dagda matches the code.
6. **Requirements document.** Names a test that does not exist (8c.4), and its line numbers for the torrent
   cases are stale (8b.4). Its "Before step 8a" gate says "if v18 is kept": it was not, and this plan's S.4
   reads it as migrated. Its "fi-1" is gone.
7. **Evaluation section 4.1 item 3, 4.1 item 4, 4.2 item 4 and 4.1 item 9 are out of date for Dagda:**
   maintenance stages, put forms, the migration's four passes and reload have all landed since
   (`TODO/COMPLETED.md`). Its section 1 "there is no reverse migration" likewise.

## Questions for the maintainer

1. **Does 8c rewrite anything on disk?** `docs/migration.md:56-57` leaves `<state>/metadata/` alone "until
   heads define it", and Macha's bytes "bind nothing any more" (Decisions, 2026-11-04 night). If heads change
   a format, the second cutover is two disk events, not the one flag day you ruled. Recommended: heads read
   today's `metadata/` files as they are at 8c, and any change to them is a later release, so the flag day stays
   one. The alternative is holding the flag day for heads, which delays everything behind Macha's merge and
   Dagda's step 4.
2. **Custody's key source on the two nodes.** A systemd credential or a passphrase. Macha has restarted
   unattended after power cuts (`../macha/TODO/ACTIVE.md`, "Paused at 0.91.0"), and `systemd/macha.service`
   takes no secret today. A passphrase means a node waits for someone after every power cut. Recommended: a
   credential, with the root backed up off the node. You decide where that backup lives. I did not check what
   protects a credential on these Pis (no TPM that I know of).
3. **The fixtures of `tests/test_maintenance_trace.cpp`.** You ruled maintenance into the node, and `TODO/ACTIVE.md` keeps these
   cases as Macha's gate. Their traces fix an order the node does not run (B5, B9). Recommended: the node takes
   Macha's order and a `Trace`, so the fixtures stand byte for byte. The alternative, regenerating them, turns the
   proof into a record of whatever Dagda does.
4. **One state directory or two?** `<state>` for Dagda and `<node-state>` for Dannan. Recommended: one, Macha's
   `state_path` as it is, so nothing moves on the flag day and `--node-state` equals `--state`. Dagda's comment
   then changes. Two separate directories would move Dannan's files at the first cutover, which then costs the
   first cutover something for the second's sake.
5. **8d and the NO KEY figures.** If Macha's root holds only the two contexts, every figure S.2 reads from
   `node.*` has to be on the Context or published. Recommended: publish them all under `dagda.*` in Dagda now, and
   read the Surface in 8d, with `publish_status` called on demand so Status stays live. This adds about 30 keys.
6. **Where does the rehearsal copy live?** It needs 583 GB of the same kind of disk on a Pi 5. If there is no such
   disk, the choice is a different disk (time not comparable), or rehearsing in place on live-b, whose data is
   replaceable from live-a. Recommended: in place on live-b if no disk exists, taking `--dry-run` first; but a
   failed in-place rehearsal costs live-b a refill from live-a, whose duration is unmeasured.
7. **Which node comes back first on the flag day?** They migrate independently. live-b will finish first by its
   size. It could start and serve what it holds, with live-a's arrival as a partition healing, which is exercised
   in the soak but puts the first v19 merge on the flag day. Or both wait for the slower. Recommended: both wait,
   unless live-a's window turns out too long for the viewers.
8. **The soak after the flag day.** The evaluation says "several days" for the first cutover and only "the soak" for
   this one. How long, and with what deliberately done in it (viewers, ingest, a partition, a restart under load)?
   No recommendation on length: nothing has been measured that would inform one.
9. **Who runs what on the live nodes.** The rehearsal, M10 and the baselines all need running on live-a or live-b,
   which Dagda's session may not touch without your word (`TODO/ACTIVE.md`, "Waiting on the maintainer"). M10 is settled: live-a (the maintainer,
   2026-11-05). Recommended: the Macha session runs the rehearsal, as it owns the nodes.
10. **The barrier's deadline** (B13) is already yours in Macha's list; it is here only so that it is settled before step
    S moves the code.
