# Dagda - Completed Work

The record. Items move here from `ACTIVE.md` as they land, with what
shipped and the gate counts.

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
  drafted with the Dannan and Lugh sessions' reviews, under the maintainer's four
  rules for the stack. No code; no commits. Gate: none yet.
