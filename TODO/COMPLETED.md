# Dagda - Completed Work

The record. Items move here from `ACTIVE.md` as they land, with what
shipped and the gate counts.

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
