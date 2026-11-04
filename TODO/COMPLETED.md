# Dagda - Completed Work

The record. Items move here from `ACTIVE.md` as they land, with what
shipped and the gate counts.

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
