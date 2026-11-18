# Dagda - Completed Work

The record. Items move here from `ACTIVE.md` as they land, with what
shipped and the gate counts.

## 2026-11-14 (05:25Z): develop on Dannan 0.7.0 (`feature/dannan-070`, merged)

Lir and Lugh run Dannan 0.7.0 and can link only one Dannan, so slice 1's tag must be on 0.7 for them to link Dagda
(the Lir session, 2026-11-14). Macha stays on `0.1.5` (Dannan 0.6); a defect its port finds is fixed on a `0.1.x`
branch from that tag. The move: `DAGDA_DANNAN_MINIMUM` 0.7.0, `libdannan-dev (>= 0.7.0), (<< 0.8~)`,
`Version.BuiltAgainstTheDannanItLinks` at minor 7, the build notes; the library needed nothing, and three test doubles
took 0.7's new pure virtuals (`Routes::bind_deferred`; `Nodes::partition`, `watch_partition`, `unwatch_partition`,
`members`, `retire`), found by diffing Dannan's `context.h` between the tags.

Gate, node-b, Dannan 0.7.0 built from its tag into a prefix, Dagda GCC 14.2 Release with warnings as errors:
**0 errors, 0 warnings**; `dagda_tests` with Macha's fixtures (without `state-live-a`): **1065 ran, 1062 passed, 3
skipped** (the opt-in NodeSeams pair and StoreRepair's root question), **0 failed**: slice 1's Macha-fixture run.
Installed to the prefix and consumed: `fixture_consumer` passes; `consumer` passes once the prefix is on the loader
path (it links only libdagda, whose install carries no run path to a private prefix's Dannan; installed as packages
there is no such prefix).

## 2026-11-14 (04:45Z): slice 1 built and proven on the Pi, merged to develop (`1065b18`, not tagged)

Written test-first from the three-systems design on `feature/slice1`, merged to develop:
- D13, the fixture installed: `cluster.h`, `heads.h`, `heads_merge.h`, `heads_space.h`, `ledger.h`, `store.h`, `tree.h`
  under `dagda/test/fixtures/`, the exported `dagda::test_fixtures`; `test/package/fixture_consumer.cpp` builds a
  two-node cluster against the installed package (`docs/packaging.md`; `libdagda-dev` suggests `libgtest-dev`).
- D4, the change notice: `Space::watch`, `unwatch`, `notice_if_moved` (from `accept` and the import path), and
  `Space::changed` over `Tree::diff`; the Context's `Heads::watch`, `unwatch`, `changed` as evidence (`HeadWatch`,
  `HeadChanges`). Contract version 2 is served, version 1 (`context_version_frozen`) beside it, as the maintainer ruled;
  `Context.TheVersionCheckRefusesAMismatch` brought to that.
- D3, node-local spaces: `Policy::node_local()`; no exchange, so nothing advertised, owed, answered or repaired; the
  replica and the tree nodes sealed under the node-local key with the node-local domain in every AAD; the tree nodes in
  a `NodePack` of the space's own (a `SealedJournal` that refuses a frame that will not open rather than cut it); founded
  at open; upkeep in the maintenance turn (history re-rooted, the pack compacted to what the head reaches); the
  no-rollback record `NodeLocalGenerations`, outside the space's directory, pending the maintainer on the counter. A node-local
  space names no DATA object yet.

Gate, node-b, GCC 14.2 Release with warnings as errors at `6d503de`: **0 errors, 0 warnings**; `dagda_tests`
**1065 ran, 992 passed, 71 skipped** (all Macha-fixture cases: no fixtures in this run), **2 failed**, both the tests'
own faults, fixed and rerun (`0cdc8e7`, `253da4b`; both pass): a copy into a `heads/` directory b did not yet have, and
a peer watching from genesis, which `changed` rightly answered with the founding's keys as well (a quiet node stands at
genesis until something reads the space). Installed to a prefix and consumed: `consumer` and `fixture_consumer` pass.
The churn case (10,000 records made and expired) leaves the space within a page of where it began. Not yet run: the
Macha fixtures, which the tag needs since `accept` and the import path now look for watches (nothing when there are none).

## 2026-11-14 (02:05Z): slice 0 on the Pi; 0.1.5's packages from the tag; the bench merged

On node-b (arm64, NVMe, never throttled), claimed 01:25Z to 02:03Z and released. Figures in the design,
"What slice 0 has measured (the Pi)":
- The log passes the maintainer's PUBACK target on a real device sync: p99 12.5 ms at a 10 ms tick, 23.5 ms at 20 ms, the same at
  256 logs on the shared segment; nothing refused at 8,000 a second. p99 is the tick plus about 2.5 ms. A segment per
  log fails again. The scan burst: 13,462 entries in 11.4 ms; 18 MiB and 12 files at 256 or 1,280 logs. Told to Lir,
  which recorded the verdict (its `3cb493b`) and asks to hear if the default tick passes 40 ms, and for the channel
  cost per space.
- Heads: 67-194 commits a second; one node 84-95 a second whatever the writers (group commit's ground, as on the Mac).
  **Found: in Lugh's shape node b falls behind on the Pi** (seen at p50 10.1 s, p99 22.0 s, growing; the Mac 0.1 to
  0.35 s). Suspected, not proven: a follower materializes every imported delta at O(state)
  (`Replica::import_history`), Macha's open "O(state) per delta". Told to Lugh, whose sign-in (its P8.5) waits on
  slice 2 bounding it.
- 0.1.5's Debian packages from the tag with Macha's fixtures (without `state-live-a`): `dagda_tests` 1,041 ran,
  1,038 passed, 3 skipped (the opt-in NodeSeams pair and StoreRepair's root question), 0 failed, 6 disabled; the
  three parameterised fixture suites alone ran 83 cases (HeadsReplicaFixture 15, MigrateHeadsReal 20,
  HeadsReplicaDamagedCopy 48), and no fixture-gated case skipped (the 3 skips are not fixture cases); lintian clean.
  The Macha session copied the packages off node-b (sha256 matched). Its expected 68 was the whole fixture-gated set
  at `ddba991`, not these three suites; the set has grown since.
- `feature/bench` merged to develop (`d424572`) with one fix: the bench's refusal reasons are kept to printable ASCII
  (`13fdda1`, which also carries clang-format's reflow of the file). That last commit is not yet built: the next Pi
  build of develop is its gate.

## 2026-11-13 to 14: the three-systems design ruled; slice 0 on the Mac

The maintainer (2026-11-13): Dagda serves Macha, Lugh and Lir as first-class. The design
(`TODO/2026-11-13-three-systems-design.md`) was written with the Lir and Lugh sessions, reviewed by both and by
Macha's for what Macha sees, and ruled by the maintainer point by point ("Ruled", 1 to 7): the log as a noun; a 10-minute
tombstone window as QoS 2's stated limit; the slice order, slice 0 first; built-in merges and the advert aggregate;
0.1.5 on Dannan 0.6; the x86-64 gate suspended and every build on the Pis; group commit for heads in slice 2.
Raised with Dannan and taken there: the advert aggregate is Dagda's (adverts are for slow facts); the version rule
(Dannan 0.6.0's ABI promise); CPU pacing moving into Dannan; a deferred reply (Dannan 0.7.0, accepted by the bench);
the per-connection call path (Dannan's P5).

Slice 0 on the Mac (`feature/bench`; figures in the design, "What slice 0 has measured"):
- The log prototype passes the maintainer's PUBACK target (p99 under 50 ms) in the layout Lir needs only with one shared segment
  per node and a call per replica node per tick (p99 21-24 ms at 256 logs); a segment per log failed both ways. The
  design's on-disk form changed to match, with compaction for long-lived logs.
- Today's head commit: 85-90 commits a second per node whatever the writers or spaces (half CONTROL tree-node writes,
  half claims, one durability barrier): the ground for group commit. A node refuses its 256th space ("at most 64
  adverts"). Lugh's shape: "disabled on A, refused on B" in about 0.1 s typical, 0.35 s worst.
- Lir's scan burst (a third of a 5 s backlog, 13,460 entries, in 29 ms) and footprint (256 or 1,280 logs in about
  17 MiB and 12 files).

No gate: measurement code only; `dagda_tests` is unchanged outside the disabled `BenchHeads.*` cases.

## 2026-11-14: 0.1.5: on Dannan 0.6, packaged for Debian, clean under GCC Release

The maintainer: the Debian packages; then Dannan's ABI promise (0.6.0, renumbered from 0.5.7 because 0.5.4 had already changed
`libdannan.so.0.5`), which 0.1.5 builds on; the x86-64 gate suspended (2026-11-14: "use only the cluster").
- Dannan 0.6 (`find_package(dannan 0.6.0 CONFIG)`, SameMinorVersion; `Version.BuiltAgainstTheDannanItLinks` pins
  the minor, now 6). Packaged as Dannan is: `libdagda0.1.5` (SONAME the whole version), `libdagda-dev` (exactly its
  library, and `libdannan-dev (>= 0.6.0), (<< 0.7~)`), `dagda-tools` (`dagda-migrate` and its manual page);
  `tools/build-deb.sh`, `docs/packaging.md`.
- GCC 14 at Release with warnings as errors (the Macha session's finding on node-a; also on x86-64): a
  `-Wnonnull` in `migrate/compare.cpp` (each branch now tests the cursor it advances), `-Wstringop-overread` from
  libstdc++'s byte compare (the markers map and the loose sort take `BytesOrder`, the same order, under one scoped
  pragma), and `-Wmaybe-uninitialized` in a test's sort (a multimap, which still shows a duplicate). Nothing silenced
  in the build.
- On the way: a Docker build failed ten tests on the folder shared from the Mac (root cannot copy a read-only file
  there) and once ran out of memory; the maintainer then suspended the x86-64 gate and moved every Linux build to the Pis.

Gate, macOS plain: **1041 passed, 6 disabled, 0 warnings**. Gate, arm64 (node-b, GCC 14.2): the Debian
packages with their tests **970 passed, 71 skipped, 0 failed**, lintian clean, installed and consumed; GCC Release with
warnings as errors **0 warnings**, its tests **970 passed, 71 skipped, 0 failed**.

## 2026-11-13: 0.1.4: installable, as Dannan is

The maintainer gave the word; the Macha session reviewed 0.1.3 and builds stage one (S, then 8a and 8b) on Dagda. Its blocker: no
install rules, so no `find_package(dagda)`. Done as Dannan 0.5.6 does it, no behaviour change:
- `install(TARGETS dagda EXPORT dagdaTargets)`, the headers in their tree under `include/dagda/` (tests and fixtures not),
  the generated `version.h`; `dagdaConfig.cmake` finding `dannan` at `DAGDA_DANNAN_MINIMUM` (0.5.3, one number for the
  build and the config) and `dagdaConfigVersion.cmake` at ExactVersion, exporting `dagda::dagda`; SONAME the whole
  version, `libdagda.so.0.1.4` (was major.minor).
- `dagda-migrate` installed, with a manual page (`docs/man/dagda-migrate.1`); `docs/packaging.md`.
- `test/package`, a consumer of the install: on the Mac, from the prefix alone, it linked `libdagda.0.1.4` and Dannan 0.5.3
  and ran; asking for 0.1.3 is refused at configure. All 82 installed headers compile alone against the prefix.
- Debian packages (the maintainer: yes): written, but Dannan's packages exist only for 0.5.6, whose config is ExactVersion, so
  they cannot build on Dagda's 0.5.3 pin (nor can Macha's own deb). The maintainer: Dagda moves to Dannan 0.5.6 in 0.1.5, with the debs.

Gate, macOS plain: **1041 passed, 6 disabled, 0 warnings**, one run.

## 2026-11-13: 0.1.3: forked children on their own sanitizer reports, TSan on arm64

The Macha session asked for the forked-child TSan failures to be fixed; the maintainer: "Do it, no supressions", on build-host.
- `dagda::test::child_exit` (`test/fixtures/child_exit.h`): a `pthread_atfork` child handler zeroes a count that
  `__sanitizer_report_error_summary` raises, and a child with none of its own ends by the raw exit call, past TSan's `_exit`
  that answers the inherited count with 66. Every forked child's `_exit` in the migration and durability tests uses it.
- The two simulated crashes join the store's start-up threads first (`LocalStore::join_background_threads`), so TSan sees no
  finished thread left unjoined.
- From the Pi's plain run: GCC's `-Wshadow` on `storage_pool.cpp`'s inner catch (renamed), and a timing race in four window
  cases whose second step now clears the window however late the call notes its request sent.
- The maintainer chose clang 19 on the Pi, then 4K pages for the run (clang 19's TSan refuses the 16K kernel's 47-bit address space):
  `kernel=kernel8.img` for the run, `config.txt` restored from its copy and rebooted after; back on `6.18.50+rpt-rpi-2712`,
  rpiledconfig and redis active, the RPi LED Config session told.
- TSan's deadlock detector tracks 64 held locks per thread; two cases hold all 64 claim stripes and `_deleting_mutex`, and TSan
  hung in its own check (gdb: `CheckFailed` unwinding into `realloc` under TSan's slot lock). The maintainer: detector on, off per case.
  Those two passed alone with `detect_deadlocks=0`; everything else ran with it on.

Gate, arm64 (build-host, GCC 14.2), plain: **1038 passed, 0 failed, 3 skipped (root), 6 disabled, 0 warnings**, one run.
Gate, arm64 TSan (clang 19.1.7): **1038 passed, 0 failed, 3 skipped, no ThreadSanitizer report**, in four parts (668, 15, 353,
and the two alone). The Mac was not rerun at 0.1.3.

## 2026-11-13: 0.1.2: the manifest's partial held, the journal in Macha's words, TSan

The maintainer (2026-11-12): cleanup, testing, hardening and improvement in scope while the port waits: all four offered.
- The manifest's partial (`feature/manifest-lock`): a pass takes an exclusive flock on its `.partial` before it reads or
  writes it, so a `--resume` beside a running pass is refused before it reads a byte. The rehearsal's gap.
  `MigrateComparePass.AResumeOfAPartialAnotherPassIsWritingIsRefused`, failing first.
- `ACTIVE.md` rewritten to where things stand; its old plan archived word for word at the end of this file. Citations into
  either file corrected by their text; some had been stale since before the rewrite.
- The journal (`feature/journal-lines`): every store line Macha writes mapped against Dagda's by an agent and spot-checked
  (`TODO/2026-11-12-journal-mapping.md`): 65 the same, 23 reworded, 39 missing, 18 not Dagda's. The maintainer: "Restore everything".
  Done but where Dagda has no such event (its section 8). The warn, error and info lines are pinned in ten cases, each
  failing first on the old code. Behaviour changed only where Macha's line needs it: a failed leader schedule ends the fetch
  empty (Macha's catch), a non-standard exception in a heads run is caught as Macha's is, a backend refusal is classed as
  data storage, a local barrier whose placement changed is warned of. The Macha session drops the `dagda.` scope in its sink
  at the cutover, so the lines read as Macha's.
- TSan on the final tree: no report in Dagda's code (see the report's gates for the 17 forked-child failures).

Gate, macOS plain: **1041 passed, 6 disabled**, one run, nothing skipped.

## 2026-11-12: 0.1.1, Macha's purpose deadlines (feature/call-deadlines), and the migration rehearsal

Macha's port is on Dannan 0.5.3, Dagda's pin, so nothing to realign. Its transport cutover (0.94.0, live) names a deadline
per call by purpose (the maintainer, 2026-11-11: control 2 min, metadata 5 min, object transfer 30 min, presence probe 2 min) and turns
Dannan's node-wide watches off. The maintainer asked whether that was Dannan's: Dannan enforces only what each caller names, so after the
store cutover Dagda's calls must name them. The maintainer: adopt, tag 0.1.1. A put, a get and a repair push end at 30 min under the 300 s
watch kept; presence batches at 2 min; a delete at 2 min and a CONTROL fetch at 5, each also ending after 30 s idle; a CONTROL
put at 5; the trie question at 2 with a 30 s watch; the exchange at 5, its synchronous calls with a 30 s watch. Claims and
single probes keep Macha's bounded 30 s. Seven cases, each failing first (the exchange's case was rewritten once: `stall` holds
every probe, so it watches the first one end, not the pass). Gate, macOS plain: **1038 passed, 6 disabled**, one run.

The rehearsal (the maintainer: now, on build-host, the key copied there and deleted after), on live-a's 0.92.4 state copy with
`dagda-migrate` 0.1.0: before manifest; a manifest pass SIGKILLed and `--resume`d, PASS against it; forward killed after 3 s and
finished; `--heads`; `--reverse`; each compared PASS (the report's migration section has the counts). The first attempt's
failures were the script's (it killed a wrapper shell and left the pass running beside its own resume), and they showed that
nothing stops two passes writing one `.partial`: caught by the checksum, not fixed. Copy, custody and key deleted from
build-host; logs in `build/scratch/rehearsal202/`.

## 2026-11-12: arm64 at 0.1.0 on build-host

The maintainer (2026-11-12): "No more builds on live-b. There's an identical Pi you can use at 192.0.2.1." build-host, Pi 5, Debian 13,
GCC 14.2, Debian's Boost 1.83 and GoogleTest 1.16. Dannan 0.5.3 and Dagda `0.1.0` from git archives, Macha's fixtures copied without
`state-live-a`, under `/root/dagda-arm64`; one job at a time, nice 19, idle I/O, -j2 (`build/scratch/arm64_run_202.sh`, logs in
`build/scratch/a202/logs/`).

- Plain: 0 warnings (the 46 at `ddba991` are gone); 1028 passed, 0 failed, 3 skipped (the root cases), 6 disabled. Every fixture
  case ran.
- ASan with UBSan: 1028 passed, 0 failed, 3 skipped, 6 disabled, no sanitizer report. 14 build warnings, all `-Wmaybe-uninitialized`
  inside libstdc++'s `std::regex` (through gtest) in `contract_include_direction_test.cpp`, with sanitizers only: a GCC false
  positive in system headers, left alone.
- The board logged one "Undervoltage detected!" during the plain build, normalised in 2 s (`throttled=0x50000`). live-b is the same
  board and went down under an ASan build, so its supply is the likely cause; for the maintainer.

## 2026-11-12: the trie diff's pins on the node's time (`feature/trie-pin-clock`), and live-b's journal

At the maintainer's word (2026-11-12). The server aged the pins on `std::chrono::steady_clock`, as Macha does (98b816d
`availability_service.cpp:335`), against Dagda's rule that time is injected; stepped time could not reach the two-minute drop. The
pins now read `_context.time().source()` (`storage_server.cpp:129`). `StoreRepair.ATrieDiffPinIsDroppedAfterTwoMinutesUnused`, disabled
since `ff77156`, is enabled and failed first (the pin still answered after 121 s unused). A difference from Macha: on the live nodes the
time source is the real clock, so nothing changes there.

live-b's journal was volatile (Raspberry Pi OS's `40-rpi-volatile-storage.conf`), which is why its 2026-11-11 outage left no cause.
`/etc/systemd/journald.conf.d/50-persistent.conf` now sets `Storage=persistent`, `SystemMaxUse=1G`; journald restarted, the journal is
in `/var/log/journal`, and Macha kept logging. Nothing was built or run there (the Macha session's request, until the maintainer says otherwise).

Gate: `dagda_tests` 1031 passing, 6 disabled, one whole run at the final code.

## 2026-11-11: arm64 on live-b, root skips and GCC's warnings (`feature/gcc-and-root`)

At the maintainer's word (2026-11-11, as recommended). The arm64 run on live-b (Pi 5, GCC 14.2, Debian's Boost 1.83 found by Dannan 0.5.3's
config) at ddba991: plain build clean of errors with 46 warnings; 958 passed, 3 failed, 68 skipped (Macha's fixtures absent), 7
disabled. The three failures are root's: root ignores the directory modes the fault injection relies on. They now skip as root, as
Dannan's do (`node_seams_test.cpp` x2, `StoreRepair.ARootQuestionWhileHoldingsSeedIsRefused`). The 46 warnings:

- 38 `-Wconversion` at `std::span` of a record's bytes: the record had a conversion operator and is a contiguous range, two ways to
  make the span. The operator is gone (`heads/record.h`).
- 1 `-Wmaybe-uninitialized` in `get_remote` (`store/distributed_store.cpp`), a false positive over an optional deadline: no deadline
  is now `TimePoint::max()`, with the wait capped at a look at the clock.
- 4 dangling elses braced and a lambda parameter that shadowed gtest's `info` renamed (3 warnings, one per instantiation), in four
  test files.

Not finished on the Pi: live-b went down at about 01:30 while building ASan (-j2) beside the plain run with Macha's fixtures (143 of
1029 passed, none failed when it stopped), and came back at 18:34 with no persistent journal, so the cause is unknown. The maintainer's word:
leave the Pi alone. So the ASan and fixture runs on arm64 and GCC's count at these fixes are not done; GCC's zero is unconfirmed.

Gate: `dagda_tests` 1030 passing, 7 disabled on the Mac (Clang), one whole run at the final code.

## 2026-11-11: Macha's busy wake in the test's copy of its loop (`feature/busy-wake`)

At the maintainer's word (2026-11-11), asked for by the Macha session: the copy stands for Macha's loop as the port drives Dagda's steps, so a
missing wake is a fidelity gap. Macha's pass that began busy (a viewer or the loader present) wakes when the foreground quiet window
ends, or at once if it already has (`maintenance.cpp:1327-1336`); the copy's park had no such wake, so a busy pass with nothing else
due slept until an event. The park now has it, over the activity clocks' `idle_for`, against repair's quiet window, the one `busy` was
read against (Macha's single `foreground_quiet`; the copy's 50 ms constant would wake a still-busy pass and spin). Case
`StepsBed.ABusyPassWakesWhenTheQuietWindowEnds` failed first (no pass after the window). Test code only: Dagda's own loop waits at
most an interval.

Gate: `dagda_tests` 1030 passing, 7 disabled, one whole run at the final code.

## 2026-11-11: gate lifetimes, two raced cases, Dannan 0.5.3, and Macha 0.93.6's yielded wake (`feature/gate-lifetime`)

At the maintainer's word (2026-11-11, all four as recommended).

- Gate lifetimes, from the TSan run at ff77156 (3 races, a Gate destroyed before the Cluster whose threads still used it): in 17 cases the
  Gate is now declared before the Cluster. The whole plain run then hung for two hours in
  `StoreRepair.AWriteWithNoDeadlineFailsAPeerSilentForTheWindow`: its assertion failed, and the `std::async` future, declared after the
  GateOpener, waited on its thread before the gate opened. Every such future (10 in 9 cases) now has its own GateOpener after it, so a
  failure fails rather than hangs.
- Two raced cases: prompt replication counts a failure before it queues the retry and applies the 4096 cap
  (`distributed_store.cpp:1560-1626`), so `PromptReplicationKeepsAtMost4096Retries` and the schedule case wait for the drop (TSan found
  it). The silent-peer write had the budget case's race (2026-11-10): the local copy landing re-arms the window, so it now steps 301 s
  until the write ends, three steps at most.
- Dannan 0.5.3 (tag on main `899faea`): 0.5.2's library with the installed config asking for Boost 1.83, the floor it builds with, so
  the Pis build with Debian's Boost and no private 1.87 prefix. Built into `build/deps/prefix-0.5.3`; README and CONTRIBUTING name it.
- Macha 0.93.6 (`a931fe5`): maintenance slept up to 31 hours on both live nodes when a viewer arrived during a repair step, because
  the yielded step set no wake. Dagda's own loop cannot do this: every pass sets the next an interval on, and every wait is at most one
  interval (`node.cpp:912-969`). The test's copy of Macha's loop (`maintenance_steps_test.cpp`) had it, and now sets `repair_continue`
  for `RepairSettled::yielded` as 0.93.6 does. Its park read the pacing afresh where Macha's reads `repair_busy` from the pass's start
  (`maintenance.cpp:1362`); it now reads the pass's, or the new case could not fail. Case
  `StepsBed.RepairResumesAfterYieldingToAViewerThatArrivedMidStep` failed first (the copy never pushed). `repair.h` says a yielded turn
  owes another after an interval. Noted, not changed: the copy has no busy wake (Macha's `maintenance.cpp:1327-1336`), and the bed's
  repair quiet window is Dagda's 2 s, not the trace's 50 ms, which the case spaces its passes past.

Gate: `dagda_tests` 1029 passing, 7 disabled, one whole run at the final code on Dannan 0.5.3.

## 2026-11-10: Macha's unmoved metadata refuses a heads node (`feature/namespace-refusal`)

Settled with the Macha session (2026-11-10): the head space is `macha/namespace`, the migration's default, and a node is refused rather
than started empty beside Macha's metadata, as the maintainer's ruling for a v18 disk.

- `Node::open`, right after the state layout is accepted and before anything else is written: with `heads` on, Macha's
  `<state>/metadata` or its cache seed `<cache>/metadata/current.meta` refuses the node, naming `dagda-migrate --heads --state <state>`.
  With `heads` off (Macha's own replica running, the port's first stage) it opens; a state with neither opens as before. The two paths
  are `heads::machas_metadata_directory` and `heads::machas_metadata_seed`, which the migration's `heads_places` now reads too (one copy).
  Case `Node.MachasUnmovedMetadataRefusesAHeadsNodeNamingTheMigration`, which failed first (both nodes started empty).
- A race in yesterday's `StoreRepair.AWriteWithABudgetEndsWhenNothingMovesForIt`, found by this gate's first run hanging: the local copy
  landing re-arms the window, and when the write saw it after the test's single 31 s step the window re-armed at the new time and nothing
  stepped again. The budget is right (Macha's re-arms on a landed copy too); the case now steps 31 s until the write ends, three steps at
  most, 93 s, short of the call's 300 s, as its sibling does.

Gate: `dagda_tests` 1022 passing, 6 disabled, one whole run at the final code.

## 2026-11-09: the sweep's pass-wide deferral and Macha's publication budget (`feature/sweep-deferral`)

Both at the maintainer's word (2026-11-09: "Yes, fix those").

- The sweep (`StoragePool::gc_step`): the slice that completes a pass now reports whether anything in the whole pass was put off
  (`_gc_pass_deferred`, beside `_gc_cursor`), not only in that slice. Macha's loop, unchanged, then waits for the grace rather than an
  event with unripe objects on disk (the seed 102 stress failure). The pin became
  `StoreStoragePool.TheSliceThatCompletesASweepReportsEveryDeferralInItsPass`, which failed first and also proves the flag clears after a
  pass with nothing deferred. Dagda differs from Macha 98b816d here, safely; the report says so.
- The publication budget: `WriteOptions::no_progress_budget` and `progress`, a window over the whole write as Macha's put_impl
  (98b816d `distributed_store.cpp:352-386`): re-armed by a copy landing, a fallback starting, a pending call that moved within it, or the
  caller's counter moving; when it runs out the pending calls are cancelled and the write ends with what landed, the rest owed to prompt
  replication. Off by default; Macha's publication passes 30 s at the port. `call_no_progress` stays 300 s, above a peer's 120 s credit
  wait. Cases `StoreRepair.AWriteWithABudgetEndsWhenNothingMovesForIt` and `...AWritesBudgetIsReArmedByItsPipelinesProgress`; the first
  failed first (the write still waiting 31 s on).

Gate: `dagda_tests` 1021 passing, 6 disabled, one whole run at the final code (1019 before; two cases added, one rewritten).

## 2026-11-09: repin to Dannan 0.5.2 (`feature/repin-052`)

Dannan 0.5.2 (tag on main `14bb3d9`), the tag the port freezes on: a client stop no longer waits out a call's deadline while a duplicate
session is retired, a write's progress is recorded before its bytes are counted, and one supervisor test skips as root. No API change.
`find_package(dannan 0.5.2 CONFIG REQUIRED)`; CONTRIBUTING and README name the tag; built into `build/deps/prefix-0.5.2`. Nothing in Dagda
changed. Gate: `dagda_tests` 1019 passing, 6 disabled, one whole run.

## 2026-11-09: seed 102's leftover objects are Macha's sweep deferral (`feature/seed-102`)

The stress failure (n0, restarted without a rebuild, holding hundreds of unreferenced objects at the end of the settle bound) is Macha's
behaviour, not a Dagda defect. A sweep pass runs in slices; `StoragePool::gc_step` (`storage_pool.cpp:1111-1160`, Macha 98b816d
`storage_pool.cpp:1030-1083`) reports `deferred` for its own slice only. An unreferenced object is deferred the first time a pass sees it
(its sighting must ripen for the grace), so the first pass after a long shut gate only sights; the slice that completes the next pass
reports no deferral when every unripe object fell in an earlier slice, and the loop (Macha `maintenance.cpp:1077-1089`, the stress loop's
copy at `delete_stress_test.cpp:746-747`) sets the quiet window to "complete; waiting for an event" with those objects still on disk. In
the settle window the next event is the head's tick, every 30 s. n0 is hit because it is the biggest: its gate stayed shut (foreground
busy and the quiet window, Macha `maintenance.cpp:357-364`) from its restart to the settle, so it held about 13,000 objects, 8,800
unreferenced, and a pass of it took about 44 s; two passes, a lost deferral and a 30 s wait do not fit in 120 s. The ids left were
the ones a later pass had not yet reached: the packed index is walked in id order, hence `5c..5d` and up.

Evidence: one probed Release run of seed 102, restarts on, on 0.5.1 (231 s, load 51 falling to 28) that passed: n0's passes logged
13 slices and 13,282 objects, all deferred (8,779 strays left); then 14 slices, 13,660 objects, deferred in the pass but not in its
last slice, 302 strays left and the gate "complete; waiting for an event"; the next event 10 s later cleared them. n2's second pass lost
its deferral the same way with nothing left. The failed runs' "complete; waiting for an event" at 706 strays is the same state. Dannan
plays no part in the mechanism; 0.5.1 can only change timing. Seed 102 was not run on 0.5.0: it passed here on 0.5.1, so a pass on
0.5.0 would show nothing. Not verified: the failing runs' own slices (they were not probed).

Pinned, not changed: `StoreStoragePool.TheSliceThatCompletesASweepSaysNothingOfEarlierDeferrals`. The fix (the loop ORs `deferred`
across a pass, or `gc_step` carries it in the cursor until the pass completes) is Macha's loop's and the maintainer's to decide; until then the
stress case can fail on a slow node.

Gate: `dagda_tests` 1019 passing (1018 and the pin), 6 disabled, one whole run on the Mac (346 s, load 14 to 57).

## 2026-11-09: the repin to Dannan 0.5.1 (`feature/repin-051`)

Dannan 0.5.1 (`337cc48`) in `build/deps/prefix-0.5.1`. `find_package(dannan 0.5.1 CONFIG REQUIRED)`: Dannan's version file is SameMinorVersion,
so this asks for 0.5.1 or a later 0.5 patch. The README and CONTRIBUTING build lines name the tag. Nothing broke: the additions (PathStats
counters, finish_wait, drop hooks) are not used. One comment changed: `call_no_progress` now says a call is idle only when it has neither sent
nor received a byte, which 0.5.1 made true; the same change means `write_stall` spills only a put that moves nothing either way, as Macha's
`touch` on each sent fragment does (Macha `src/cluster/net.cpp:1571-1576`), no longer an upload still sending.

The delete stress case, Release on the Mac, runs of 90 s asked, settle bound 120 s, load average 12 to 220 (other sessions): 17 runs, 3601 s.
Restarts on: 5 runs, 1076 s, and a rerun of seed 102, 267 s. Restarts off: 5 runs, 979 s. Restarts and the instant delete off: 6 runs, 1279 s.
No heap abort in any run, so the Release abort (one in 14 before, with restarts and instant delete off) did not recur; that clears nothing at
that rate. No held-ledger entry for an object no disk lists. Two failures:
- Seed 102, restarts on, twice of two: n0, restarted in round 2 without a rebuild, keeps hundreds of unreferenced objects past the settle bound
  (461; on the rerun 706, then 615, the ones named all in `5c..5d`, as if one sweep was walking the id space slowly), its gate mostly
  "complete; waiting for an event", and the retired objects the end samples are read back from n0 (`holds a removed object`, other nodes'
  reads with source remote). The other nodes settled. Not looked into: whether n0's sweep is starved or misses them after the restart.
- Seed 203, restarts off: n2 and n3 held view and disks differ by about 20 at the end, `explain_disagreement` then finds view_held=1 for the
  objects it names: the end check's own race, recorded before.

Not done: no run of these seeds on 0.5.0, so whether seed 102 is new with 0.5.1 is not known; no sanitizer runs.
Gate: `dagda_tests` 1018 passing, 6 disabled, one whole run on the Mac at `14185e3` (323 s).

## 2026-11-08: compaction waits for appends to be indexed (`feature/compaction-gap`)

The stress findings' pack race, closed at the maintainer's word (2026-11-08: fix in Dagda now, in Macha after the freeze). A pack record is written under
`_pack_io_mutex` and indexed after `_mutex` is taken again; a compaction in that gap counted it dead, could unlink its pack, and lost a put or
brought a removed object back at the next start. The gap also let a compaction overwrite an index entry an in-flight remove or put had just
changed. Now an append counts itself in `LocalStore::_unindexed_appends` from the moment its record is in the pack until it returns holding
`_mutex`, from which every caller indexes without letting go (put, both touches, remove); `compact_packs_locked`, which holds `_pack_io_mutex`
throughout so no append can start, waits under `_mutex` for none before it reads the index. The two cases that pinned the loss as Macha's became
`StoreLocalStore.ACompactionWaitsForAPutInItsAppendGap` and `...ARemoveInItsAppendGap`: a compaction started on another thread in the gap must
not finish there, and the object is kept (or stays gone) in the process and after a restart. Both failed first on the old code (the compaction
finished in the gap, the put's object was indexed in the unlinked pack, the removed one was back). Macha 98b816d keeps the gap; Dagda is safer
here, and the capabilities report says so. Not changed: `StoragePool::get` stopping at the first backend's empty answer, Macha's too.

Gate: `dagda_tests` 1018 passing, 6 disabled, one whole run (the count stands: two cases rewritten, none added or removed).

## 2026-11-08: heads S7, the metadata migration, its comparison and conformance (`feature/heads-s7`)

`dagda-migrate --heads` (`dagda/migrate/heads_metadata.{h,cpp}`): Macha's `<state>/metadata/` becomes `<state>/heads/<space id hex>/` (space
`macha/namespace`, `--space` names another) and `<cache>/metadata/current.meta` becomes `<space id hex>.meta`, by the plan's 4.1. History and journal:
each frame that authenticates keeps length, nonce and ciphertext, its tag derived to v19's associated data; damaged spans and a torn tail are carried byte
for byte at their offsets; written whole beside itself, read back as the replica reads it (same spans, same offsets) before the rename. Checkpoint and seed
retagged, nonce and ciphertext kept; `heads.meta` and `author.meta` re-sealed under the node-local key, plaintext byte for byte; `set-aside.meta` sealed for the first
time; everything else goes with the directory, renamed last. Peak extra space: one history file. On a v19 state the pass runs alone under a new marker,
`dagda-state-layout-going-forward-heads`, which the store refuses naming `--heads` (`store/state_layout.{h,cpp}`, the one store change); a stopped pass is
finished by any run; a first-stage run refuses metadata left part way. `--reverse` always brings the metadata back, from any point. Steps: seed written,
seed removed, history, journal, checkpoint, heads, author, set-aside, directory, marker: a crash case after each, both ways (`MigrateHeadsCrash.*`).

The comparison: `metadata_manifest_reader()` is `HeadsMetadataReader`, nine categories (author, certificates, checkpoint, heads with their replay chain,
history by offset, journal, markers, seed, set-aside), read under whichever layout holds them. A damaged span or a file opening under neither is compared
by bytes and NOT CHECKED when the same, as a damaged pack span is. Manifest format 2. Every real fixture (20 nodes, `state-scratch` 0.92 included, torrent,
recovery, twoheads) migrated from v18 through both stages compares PASS, replicas open equal, and back compares PASS 18 to 18. A damage test per item.
`--resume` from a real partial through the command line: 600 objects, killed by SIGKILL after two durable checkpoints, resumed, equal to a whole pass.

C41 to C46 written. Forms on real bytes (opaque, by magic): SM3/SMA8 (old fixture), SM4, SM7, SM9 (checkpoints only); DLT5, 7, 9, 10, 11; genesis. Not
on real bytes: SM16, SM17, SM19 in history, DLT6, DLT8, a journal frame, a plain `set-aside.meta`. Torrent's README counts (DLT9 15 ...) are the head's
chain of 29; history on node a holds DLT9 18, DLT5 17, DLT10 5, DLT11 1. Replay is the chain only: applying Macha's deltas is the port branch's.
Found: the old `state-scratch` (pre-0.93) begins at the old genesis and holds forms the opaque owner cannot open, carried unchanged either way.
Gate: `dagda_tests` 1015 passing, 6 disabled, one whole run on the Mac at `2ba4666` (962 at the start). `g++-15 -fsyntax-only` clean on the
changed sources; not linked or run under GCC or on arm64; no live disk.

## 2026-11-08: three findings from the delete stress case (`feature/stress-findings`)

Release on the Mac (x86_64, libmalloc), under heavy load from other sessions (load average 120 to 490). `DAGDA_STRESS_INSTANT_PERCENT` (50 by default,
0 is Macha's: every delete left to the collector). Runs of 90 s asked (90 to 190 s of load each), settle bound 120 s:
- Heap aborts (A). Restarts off: 8 runs, 822 s of load, no abort. Restarts and the instant delete off: 14 runs, 1326 s, one silent SIGABRT
  77 s after start (`abort <- malloc_report <- HeldLedger::held_with_prefix <- LocalStore::verify_step <- StoragePool::verify_step <- StressLoop`):
  heap corruption found on a free, as before, so it is neither the restart's Dannan use-after-free nor the instant delete; it is in what runs
  without them (Dagda's moved code, Dannan's live sessions, or a library). With `MallocScribble`, `MallocGuardEdges` and `MallocCheckHeapEach`
  (200000): 6 runs, 755 s, nothing. ASan and UBSan on the same configuration (Dannan 0.5.0 rebuilt sanitized here; OpenSSL, GTest and libc++ not;
  `detect_container_overflow=0`, as GTest's uninstrumented containers trip it): 8 runs, 2090 s, no report. Writer not found, so no mechanism and no
  fix; the two aborting stacks share only reading an `ObjectTrie` (`collect` under `held_with_prefix`, `records_not_held`) and freeing its copies.
- A lost object (B), reproduced without restarts (run 101: "cannot open pack: No such file or directory" on n1, every read). A pack append lets
  the pack lock go before the index lock is taken again (`append_pack_record_locked`, `local_store.cpp:437-486`). Compaction takes both in that gap
  (`:1498`), snapshots an index without the put's record (`:1349`), counts it dead, may take the active pack as its victim, rewrites the rest
  (`:1391`, `:1451`) and unlinks it (`:1473`); the put then indexes its record in the unlinked pack and the held ledger says held (`:815-816`). Reads
  throw and are logged; after a restart the ledger alone says held, no pack or file holds it, `list()` misses it and `get` finds nothing, as the
  earlier runs saw. `StoragePool::get` then answers with the first holding backend's nothing without asking the next (`storage_pool.cpp:726-741`).
  The same gap on a remove carries the object forward past its own tombstone: what a remove reported gone is held again at the next start.
  Macha's lines at `98b816d` are the same (`local_store.cpp:821-867`, `:1170-1172`, `:1694`, `:1745`, `:1819-1820`, `:1842`;
  `storage_pool.cpp:591-610`): not fixed, for the maintainer. `LocalStoreOptions::after_pack_append` is a test seam; two cases pin the loss and the return.
- The status race (C), Dagda's: `ReferenceCounts::totals()` is read under its own lock; the census sets both figures under it. A case reads the
  totals while the census sets them.
- The quiet window: from reading, Macha's loop at `98b816d` (`maintenance.cpp:343-364`) and the stress loop alike arm the sweep's quiet window at
  a pass's start whenever an event or the generation moved, and that pass then skips the sweep; it runs only after a whole window with no storage,
  metadata, topology or claims event. A busy live node with objects arriving more often than the window (writes, replication, repair's own pulls,
  which notify `storage` at `distributed_store.cpp:2883`) never sweeps until the flow stops: Macha's design, a real starvation, not the test's.
  Under the sanitizers the stall the earlier runs saw is more of it: slow passes, repair still pulling, peers' pushes and the owners' garbage
  commits on the one shared head keep moving the count. Not changed. Under ASan here the Macha-loop nodes mostly settled; the strays that stayed
  were mostly n3's, the node's own maintenance thread, its log saying the sweep was enabled: not looked into.
- The case's end check compares the held view and `list()` taken at different moments while maintenance runs, so a few runs fail on a count
  that `explain_disagreement` then finds equal: the check's race, not the store's. Left.
Gate: `dagda_tests` 965 passing, 6 disabled, one whole run on the Mac at this code (962 at the start).

## 2026-11-06: heads S6, the reconciler (`feature/heads-s6`)

`Exchange::read_group` folds several heads (`mgr:1127-1315` at `98b816d`): two at a time in hash order, under a reconciliation lock taken lazily (`IoMutex`, after the space's mutation mutex), generation max plus one, previous the lower hash, one merge parent, the higher. `MergeUnavailable` and `DecodeError` set aside the head that is not this node's own; a stop goes through everything. A merge's claims are as Macha's: a dot its clock does not carry (R3), here only, nothing refused for an object not held, the peers' share deferred on failure. `heads::Merge` is now `merge(base?, MergeHeads)`, its result carrying the delta (kept only when strictly smaller), the objects to claim and conflict counts, and it holds Q5's contract; `base` is computed only for a merge whose `reads_base()` says so. A space with no merge keeps two heads and says so. `dagda.heads.<space>.reconciliations`, `conflicts_created`, `conflicts_superseded`; observations `heads.reconcile_us`, `heads.reconcile_wait_us`, which Macha's facade maps from `metadata.reconcile_*`. `converged_waits` and `repair_waits` gained the data device: a merge's claims check presence. The neutral owner's merge is `test/fixtures/heads_merge.h`, Macha's two merge functions with the filesystem out.

Macha's 11 S6 cases named in the plan's 5.1. Owner's and said so in each case: the rename and returning directory outcomes of NT:1505 and NT:1628, `metadata_voters` and `data_replication` of RPC:3114, `legacy_clock` of RPC:3737 and RPC:6378; RPC:3556's two 100 ms sleeps became assertions at the sender. C2's merge half, C8, C16's second half, C39, C40, merge baseline. Beside them: H11's seam (`AConflictsValuesAreClaimedAndRootedWhileItStands`), a stop through the merge, peers' claims deferred, a head with no tree, what a policy commit left, RPC:3060's fork.

Macha's bytes: `HeadsWireFixture.TheFoldMintsMachasOwnMergeFromItsTwoHeads`. Given the two heads and the merge's payload, the reconciler mints Macha's commit, hash for hash, on `-markers` node a (generation 14, stored whole). Only there: `-branch`'s merge and the other nodes have a delta among the parents, which needs Macha's codec, so on those only the shape is asserted (one merge parent, previous the lower hash, generation, delta smaller). `-twoheads` holds no merge; its survey stays S5's case. Macha's real merge over these is the port branch's.

Found, for the Macha session (none fixed): R13, a fold step over two heads neither of which holds this node's latest mutation rotates its author, by reading and pinned here on Dagda's replica, not run on Macha; the swap at `mgr:1213-1215` cannot fire, as `heads` is already sorted by hash. Macha's reconciler has no stop in it; Dagda's takes the pause it was told to.

Checked under GCC 15 by `-fsyntax-only` on the changed sources; not linked or run under GCC or on arm64. No mutation testing, by the maintainer's rule. Only the last code commit (`4c0161d`) was run; the commits before it were not each built.
Gate: `dagda_tests` 952 passing, 4 disabled, one whole run on the Mac (928 at the start).

## 2026-11-06: the repin to Dannan 0.5.0, class-waits and four differences from Macha (`feature/repin-050`)

Dannan 0.5.0 (`df839fe`) in `build/deps/prefix-0.5.0`; the pin, the README and CONTRIBUTING moved. Class-waits is on the branch: the arbiter and the device
reserve on `sched::ClassWaits`, with develop's window, Macha's words in the warning and the 100-year test. ClassWaits caps one sleep at 24 hours, not one.
`write_deadline`, `read_deadline` and `call_horizon` are gone: a peer's put or fetch runs under `CallWatch`, `call_no_progress` (300 s, past a peer's 120 s
credit wait), its Dannan deadline ten years off. A caller's own deadline stays. Repair's pushes and `put_on` too. CONTROL: the sweep's check marks the object on
`ClaimLocks` (Macha's `claim_deletion`), and a claim of either class holds the stripe and finds a mark not present; `before_control_removal` is the test's seam.
With the reserve off a claim was already never refused for admission: shown, not changed. The exchange's stop comment no longer says repair delivers claims.
Left, none closed here: Dannan marks progress on reply frames only, where Macha's `touch` also counts bytes sent (`net.cpp:1575`), so an upload with no reply
yet looks idle: a moving put is spilled past at `write_stall` and fails at the window. That needs Dannan: `Session::write_next`'s completion marking the
fragment's pending call. Macha's silent data call on a live session never fails; Dagda's does at 300 s. `WriteOptions` carries no no-progress budget and shared
counter (`replicate` `:352-386`, publication's 30 s). A claim or a put from a peer the server's roster does not list yet is 403 (`Identity::verify`), where Macha
takes any authenticated peer.
Gate: `dagda_tests` 938 passing, 4 disabled, one whole run on the Mac at the last code commit (928 at the start). `g++-15 -fsyntax-only` clean on the changed
sources; not linked or run under GCC or on arm64.

## 2026-11-06: a failed read of history cuts nothing (Macha `98b816d`)

Mirrors `load_history` and `next_history_frame` from Macha `98b816d`. A read failing in the main loop or the forward scan stops the load: nothing is
cut or kept aside, `_history_bytes` is the file's size so appends stay correct, and the next open reads the rest. The scan takes a stream; the path form,
for S7 and the comparison tool, throws on a failed read, so it can never read as nothing later. `ReplicaOptions::history_reader` is a test seam.
`AFailedReadInTheHistoryScanIsNotNothingLater` is Macha's case. `AFailedReadOfHistoryCutsNothing` runs the whole load at each failure, which Macha has by
reading for its main loop; it failed before the fix. Not on GCC or arm64.

## 2026-11-06: a damaged history frame loses only that frame (`feature/history-damage`)

Macha 0.93.5's `load_history` mirrored from its `9153011` (behaviour baseline; forms stay cut at `9a08846`). A frame that does not open looks forward for the
next that authenticates (`next_frame_from` in `sealed_file.h`, reachable for S7's migration and the comparison tool); the damaged bytes stay; nothing later is
a torn tail, cut and quarantined up to 16 MiB. A length over 2 GiB no longer reaches a seed. Dagda's one addition: the scan pauses per window, so a stop passes
through it (Macha's load has no stop). `HeadsReplica.ADamagedHistoryFrameLosesOnlyThatFrame` and `ATornHistoryTailIsStillCut` are Macha's, assertion for
assertion but for flipping the body byte where Macha's writes 0x5a over it. No existing case's assertion differed. `dagda/test/data/replica-0.93.4/` gains
`recovery` and `twoheads`; the byte proof damages copies of real history, six kinds against every frame of eight states. Plan: R12 resolved, D5's history half gone.
For S7: a suffix that does not open under the reader's key and associated data is now one torn tail, so a half-migrated history must never reach this reader.
Gate: `dagda_tests` 859 passing, 4 disabled, three whole runs on the Mac (797 at the start). `g++-15 -fsyntax-only` clean on the changed sources; not run under GCC.

## 2026-11-06: DATA credit waits as Macha's (`feature/credit-wait`)

`admit_data` takes `no_caller_deadline`: the credit wait runs under the arbiter's no-progress window, re-armed by any release, as Macha's at `9a08846`
(`src/cluster/data_work.hpp`). Every store and server path whose Macha counterpart names no deadline uses it; a caller's own deadline stays
absolute. `get_object`, `put_object` and `remove_objects` that give up answer 503 `stopping`; a probe that tries once answers 503 `storage_busy`.
The warning is Macha's line word for word. `AdmissionOptions::no_progress` defaults to 120 s. `admission_wait` now bounds control slots only, and
nothing waits on it with the reserve off. Macha's 0 is 100 years from the facade (plan S.3): the one difference left in the window, unseen unless a
config sets 0, and both live nodes set 120000. No existing test's assertion changed.
Then the three differences that entry first left open. Write and read deadlines are on the node's clock (`look_at_the_clock`, 25 ms; `read_trace`).
With no caller deadline, `write_deadline` starts when the local credit wait is over, so a whole-window stall lands on a peer and a slow admission still
seeks its second copy. A read's own deadline is one time across its stages. `prompt_read_admission`, default on, is off in Macha's facade (S.3): the
lease keeps a prompt read yielding to foreground and pressure, under the background ceiling and in a device slot, none of which Macha's has. `valid_on`
keeps its credit: only tests call it. Left: past those points `write_deadline` and `read_deadline` are absolute limits Macha has not.
`StoreRepair.RepairIsPacedNotStoppedWhileHigherClassesStayBusy` is on stepped time: a contended turn earns 19 times its real length as rest, so a turn of
about 526 ms outran its 10 s wall-clock bound. Pacing as designed, not a defect; the failure itself was not reproduced under CPU load.
Checked under GCC 15 by `-fsyntax-only` on the changed sources; not linked or run under GCC or on arm64.
Gate: `dagda_tests` 814 passing, 4 disabled, three whole runs on the Mac (797 at the start).

## 2026-11-06: heads S5, the exchange (`feature/heads-s5`)

`dagda/heads/`: `messages.{h,cpp}` (15 types, `Numbers` a parameter: Dagda's own from 784, Macha's table only in tests, `test::owner_numbers()`, and with `heads` off none registered, so Macha's own stands through the first stage and its facade passes these numbers at the later one; `SpaceSpec::unaddressed` is the one space whose payloads carry no id, others add 32 bytes), `exchange.{h,cpp}` (Macha's `MetadataManager` at `9a08846` from here out: import, push with its window of 8, publish, survey, founding, `repair_once`, head repair, truncation, the replicator, stalled peers; `MetadataServer`'s adverts and notices; the metadata half of the maintenance loop, with the virgin founder rule, the settle and the backoff), `convergence_demand.h`. `SpaceSpec::founding`, `generation_advert`, `write_copies_advert`. `Space`: `accept`, the view's lifetime and the remote epoch (H31), the policy commit to the active set, the local scope check, mutation timing. `ClaimBarrier`: `retain_on_peers`, `claimed` (`NodeEvent::claims`). The node: `NodeOptions::exchange`, `MaintenanceOptions::stage`, metadata repair before GC, `head-repair` and `history-truncate` between the claims' compaction and packs, each in its own try, step forms for an owner's pass, `dagda.heads.<space>.*`, `HeadsEvidence` carrying status, convergence and what is owed. Wakes for a metadata event; D12's polling otherwise stands.

Seams for S6: `Exchange::read_group` throws `HeadsNotReady` where Macha folds several heads (`mgr:1157`), so a read falls back to this node's own head and a run stays dirty; `publish_commit`, `Space::set_aside`, `SpaceSpec::merge`, `retain_on_peers` are there for it; no reconciliation lock yet. C16's different-founders half, C2's merge, C8, C39, C40.

Macha's 17 S5 cases and the 4 retirements are named in the plan's 5.1; `test_convergence_demand_coalesces_burst_and_keeps_same_generation_event`, `test_a_new_namespace_is_a_tree_from_its_first_commit`'s neutral form, NS:421's loop half and RPC:4756's policy half in 5.3. C10, C11, C14, C16's first half, C17, C18, C19, C27, C35, C37, C38. C5's retry through the seam stands from S4. Bytes: known answers by hand from Macha's encoders; every `heads.meta`, `checkpoint.meta` and history frame of Macha's fixtures, `-twoheads` and `-truncated` among them, re-encoded equal (three checked in, `HeadsWireCheckedIn`); and real history over Dannan on Macha's numbers, captured at the receiver (`HeadsWireFixture`, which skips without the fixtures). No real Macha process was on the other end. Numbers differ from the plan's 2.3 by the maintainer's rule (it now says so).

Found, for the Macha session (none fixed): RPC:2999's phantom-id hunt seeks a placement `discover_or_form` no longer reads; RPC:3212 opens its gate with the replicator possibly still delivering, so a straggler can make a third run (Dagda's waits first); `replication_loop` drops the claims it swapped out when stopped, against its comment that what is owed stays owed (`mgr:129-131`, `:242-245`).

The maintainer dropped mutation testing partway through this session, so none is claimed as a gate. Checked under GCC 15 by `-fsyntax-only`; not linked or run under GCC or on arm64. Only the final commit was run; the commits before it were not each built.
Gate: `dagda_tests` 847 passing, 4 disabled, one whole run on the Mac at this code, by the maintainer's change to the brief (one run, not three), under other sessions' load.

## 2026-11-07: a concurrent-delete stress case under the sanitizers (`feature/delete-stress`)

`dagda/test/delete_stress_test.cpp`, two opt-in cases sized by `DAGDA_STRESS_*`: a 4-node cluster with storms of deletes, Macha's loop on the real
clock, held readers, head spaces, stops and ledger rebuilds; and held tries through checkpoints and rewrites. No production code changed. Run on
macOS, with Dannan 0.4.1 rebuilt sanitized; OpenSSL, GTest and libc++ not. ASan and UBSan, 1894 s of stress: no report, but every cluster run ran out
of its settle bound, so its end asserts prove nothing. TSan, 1901 s counted: 11 of 13 cluster runs passed; one ended in a SEGV, one never settled.
Found, none fixed, for the maintainer: Dannan's `Session` delivers after its `Node` is gone when an in-process stop shares the io context (TSan: writes to a
`RetainedMemoryLedger` and a null `_server` at `scheduler.cpp:98`); `Node::publish_status` reads `ReferenceCounts::_totals` unlocked;
`HeldLedger::stats` races the journal's `_bytes`. Open, cause unknown: two unsanitized Release aborts inside libmalloc, and two runs with a
held-ledger entry for an object no disk lists. This clears nothing about live-a, a Linux node on glibc that does not run Dannan's `Session`.
Gate: `dagda_tests` 797 passing, 6 disabled.

## 2026-11-06: the node's seams for Macha's loop (`feature/node-seams`)

`Node::run_local_stages(stop)` throws `MaintenanceStopping` past its catch; `begin_local_stages`, `local_rebalance`, `retention_compact`,
`compact_packs`, `local_scrub`, `ledger_verify` are the same stages one by one, holding the node's lock each for its own length. `collector()`,
`data_sightings()`, `control_sightings()` hand an owner's pass the node's own (automatic off). `MaintenanceOptions::time`, `wall` and `trace`
put the pass, the survey (`AvailabilityOptions::time`), repair, the collector and the activity clocks on the owner's clocks and one hook.
`maintenance_steps_test.cpp`'s bed moved onto all of it, its `availability.share = 0` gone; all seven fixtures unchanged. B6's observations and
slow-stage lines are recorded in the steps, and about 35 figures added under `dagda.*`, `repair.last_credit_bytes` among them.
`AdmissionOptions::device_pressure` is Macha's `dht.io_pressure_*`, zero turning the gate off. A node with custody and no certificate now opens
and advertises no wrap key. The steps' rebalance rest ends on Macha's four events only (`maintenance_events()`; the composite keeps any event). A
stop inside a stage comes through it: in pack compaction's wait for the walk and between backends, as Macha's can fall; a pack being rewritten and
the claims' compaction run to their end, as Macha's do. Left: `cluster_stable` and `dagda.gc.*` under an owner's steps; custody is still required;
`no_progress` governs no store or server wait, because `admit_data` gives each an absolute `admission_wait`, where Macha's re-arms on any release,
and a zero is refused where Macha's waits for ever (for the facade and the capabilities report). Not run under GCC.
Gate: `dagda_tests` 653 passing, 3 disabled, three whole runs on the Mac (627 at the start).

## 2026-11-06: heads S4, one node commits (`feature/heads-s4`)

`dagda/heads/`: `space.{h,cpp}` (Macha's `MetadataManager` at `9a08846` as far as one node goes: `mutate_delta`, `mutate`, own head, the
policy commit's local half, the claims barrier, `current`, `local`, `converged`, paging, what is owed the peers), `spaces.{h,cpp}` (a node's
spaces at `<state>/heads/<space id hex>/`, the store's claims, one thread for the Context's forms, the wait guard), `retirement.{h,cpp}`
(tombstone batches, the magic a parameter). `Context::Heads` (`open`, `current`, `read`, `mutate`, `mutate_delta`, `heads`), evidence all the
way, the executor rule and deadlines. `NodeOptions::heads` and `spaces`, the `metadata` plane from real replay, stage `open heads`. `HeadFields`
gains `root` and `policy`. Owner's steps are `Mutation`, Dagda's the order (mgr:1636-1903). `Policy::replicas_of(N)` is refused naming L2.

Seams left. S5: `SpaceServices::converge` (survey, import, forming: a node with no head and no exchange is `HeadsNotReady`), `accepted` (adverts
and notices), `owed_changed` and `take_owed` (the replicator; a claims queue bounded at 32 MiB is here, nobody drains it), `maybe_reconfigure`
commits on this node alone (the policy commit's active set, C11, C17, C37), `cached_view` has no TTL or remote epoch (H31), no message types, no
`dagda.heads.*`, the barrier does not notify `NodeEvent::claims`. S6: `SpaceSpec::merge` is held and uncalled; `Space::set_aside` and the stamp
are there for it. Founding is S5's: tests found a head by hand. The Space's own forms take no deadline, as Macha's do not; `Context::Heads` is what
carries one, and a mutation it gave up on may yet land.

Macha's 4 S4 cases are named in the plan's 5.1; S4's half of NS:421 in 5.3. `test_every_write_records_where_it_came_from`, NS:497 and SM:4524, which
H14 and H23 cite, stay Macha's by the mapping (REWRITE, REWRITE, KEEP): each is FileSystem or the owner's codec. NE:187 stays Macha's and paging
is `HeadsSpace.AViewPagesItsTreeLikeTheWalk`. C2's commit half, C5, C6, C9, C13, C21 to C24, C36; the commit baseline, 100 k keys 24 nodes read and
12 written, equal record bytes. Fixtures: a space commits on `state-scratch-0.93.4` (checked in) with a clock-only mutation, its bytes opaque, and
the batch in `-delete`'s sealed store opens and is made again to its id. Finding D13 in the plan: `mutate_delta`'s retry is unreachable at `9a08846`.
Checked under GCC 15 by `-fsyntax-only` on the new sources; not linked or run under GCC or on arm64.
Gate: `dagda_tests` 685 passing, 3 disabled, three whole runs on the Mac. One earlier whole run under another session's load lost
`StoreRepair.RepairIsPacedNotStoppedWhileHigherClassesStayBusy`, a wall-clock bound in a case S4 does not touch; alone it passed five of five.

## 2026-11-06: the steps Macha's loop needs (`feature/maintenance-steps`)

`Collector::pass` is `inventory_step`, `tombstone_step`, `release_step`, `control_step`, `data_step`; `Repair::pass` is
`earn_credit`, `survey`, `paced_by`, `may_start`, `turn_wait`, `note_gate`, `begin_turn`, `claim_walks`, `lists`, `transfer`, `settle`:
public, with waits declared, returning evidence, nothing catching a stop. `RepairOptions` gains `time` and `trace` (the `claim-walk`
lines). The tombstones line is Macha's `erase=[...]`, and a stamp has its own line. `dagda/test/maintenance_steps_test.cpp` drives the
steps in Macha's order in a neutral loop, on real clusters, to all seven of Macha's fixtures, unchanged, at `9a08846`
(`dagda/test/data/maintenance-traces/`). What Macha's filesystem and catalogue answered is a scripted owner, so generation and live
counts are its script's; a one-node fixture has a non-hosting peer beside it. Not run under GCC or on Linux. Dagda's own loop stays 8c:
`Repair::pass` also wakes on a new survey list, which `claimed-objects-lost` forbids, and the survey has no injectable clock.
Gate: `dagda_tests` 543 passing, 3 disabled, three whole runs on the Mac; the new cases 15 repeats before the last three cases, 5 after.

## 2026-11-06: heads S3, the replica on disk (`feature/heads-s3`)

`dagda/heads/`: `record.{h,cpp}` (record and hash, entry, certificate, dots, the clock and merge-parent sections an owner frames),
`codec.h` (the owner's codec and `CostCounters`), `sealed_file.{h,cpp}` (frames, sealed files, space id, v19's AAD rules),
`replica.{h,cpp}` (Macha's `MetadataReplica` at `9a08846` over a codec, under section 4.1's split: history, checkpoint and journal
under the domain key; `heads.meta`, `author.meta` and a sealed `set-aside.meta` under the node-local key), `cache_seed.{h,cpp}`.
Macha's magics are parameters; none is in the library. The test owner is neutral, `dagda/test/fixtures/heads.h`.

Byte proof, `HeadsReplicaCheckedIn` and `HeadsReplicaFixture`: every frame of `history.log`, `checkpoint.meta`, `heads.meta` and
`author.meta` of both nodes of `state-scratch-0.93.4` and its `-branch`, `-delete`, `-markers`, `-truncated`, `-seed` opens, re-seals
to its own bytes under its own nonce, and matches the dump's listing; each whole frame re-hashes and materialises; a replica opens
over a copy and reaches the same head, certificates, links and author. Four states are checked in as hex,
`dagda/test/data/replica-0.93.4/`. A delta frame replays only where `checkpoint.meta` holds its result (the head of `-branch` and
`-delete`); the other 11 deltas across those states need Macha's codec and are left for the port branch (section 4.3). No frame of
`journal.log` is proved: 0.93.4 writes that file only empty.

Macha's 32 S3 cases are named in the plan's 5.1; the replica half of SM:3848, `test_metadata_delta_state_model`, SM:2492; C3, C4, C15,
C20, C25, C26; verify, replay and accept baselines. Not run under GCC or on arm64. Gate: `dagda_tests` 604 passing, 3 disabled,
three whole runs on the Mac.

## 2026-11-06: the before and after comparison, and crash cases per phase (`feature/migrate-compare`)

Items 6 and 7. `dagda-migrate --write-manifest` is a read-only pass and `--compare-before/--compare-after` reports every
difference by name (`dagda/migrate/manifest.{h,cpp}`, `manifest_format.{h,cpp}`, `compare.cpp`; `docs/migration.md`, "Proving nothing
was lost"): objects per store, removed, accounting, markers, retention marker, held, claims and dots, reference counts, sightings, every
other file by SHA-256. Metadata heads, certificates and history are NOT CHECKED, failing, until S7 fills `metadata_manifest_reader`.
At live-a's counts a manifest is 504 MB, 89 bytes an item, and a pass resumes. Every step of the migration has its case, 25 in
each direction, and a second and third run; the comparison passes after each. Over a copy of Macha 0.93.4's scratch state `a`: everything
SAME, metadata NOT CHECKED, so it fails, as it must.
Found and fixed, each with a case: a loose object's touched time reset; a torn tag rewrite lost as a damaged span; a crash's
temporaries never removed; a backend left off the command line stranded v18 under a v19 state; a journal of a torn frame refused for good; a
state crashed in either direction opened by Macha or the store. Left: S7's reader, the live runs. Not run under GCC, on arm64 or on a
real disk at 4 TB. A kill between a tag's write and restoring its time leaves one touched time moved, which the comparison names.
Gate: `dagda_tests` 606 passing, 4 disabled, three whole runs on the Mac.

## 2026-11-06: heads S2, the tree over keys (`feature/heads-s2`)

`dagda/heads/`: `tree.{h,cpp}` (build, update, apply, walk, lookup, scan, prefix, paged `entries`, diff, node diff, collect, stats,
the value codec and child-tree hook; magics, domain strings and limits are parameters), `control_node_store.{h,cpp}` (read, commit,
replay, local), `reference_census.h`. Moved from Macha at `9a08846`, and it makes every node Macha made in the order Macha made it,
the unreachable first passes of a packed level included. Bytes: all 25 of `tree-vectors-0.93.4` (20 checked in, 5 too large and
read from Macha's fixtures under a named skip) and all 19 tree nodes in `state-scratch-0.93.4`'s sealed store. Macha's 21 S2 cases
are named in the plan's 5.1; C7, C34. Macha's forms live in the test, none in the library. Not run under GCC or on arm64.
Gate: `dagda_tests` 520 passing, 3 disabled, three whole runs on the Mac.

## 2026-11-06: additions inert on a Macha node, and activity in Macha's meaning

For the maintainer's unconditional Yes. `AdmissionOptions::device_reserve` off leaves DATA uncapped, which Macha's facade sets. Claim parts
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

**Paused (the maintainer, relayed by the Macha session, 2026-11-05: "Pause until it lands"):** no port work until Macha's old-code
removal lands. The Macha session will send the commit and the forms that remain. On resuming: re-read the baseline,
conform the read-failure shape to 0.92.6, run this tree on live-a under GCC. Left on live-a: `~/dagda-linux`.

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

On live-a (Linux aarch64, NVMe, `8361c8b`, lowest priority beside the live node, load about 1): 422 of 422, 74 s.
Scale: trie built in 38.7 s, lookups 12 us, walked in 0.75 s; 100,000 objects written in 7.4 s, reopened in 0.48 s, `get`
30 us, a compaction slice 239 ms. Faster than the Mac throughout, so these are no ceiling for the USB disks, which
are not measured.

## 2026-11-05: read errors and no storage, on the maintainer's ruling

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

The Macha session ran `dagda-migrate --dry-run` (Dagda `db2ba92`) on live-a over a scratch copy of the
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

On live-a (the maintainer's choice: Debian 13, aarch64, g++ 14.2, kernel 6.18), under
`~/dagda-linux` at lowest priority with 2 compile jobs, against Dannan 0.4.1, Boost
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
deadline waits on the maintainer's ruling in Macha. Corrected the next day: the put forms were
first made persisted by default, which matches Macha only at `write_copies` 1 and
was wrong (the test mapping found it; `distributed_store.cpp:226`). They seek
`write_copies`, and the facade passes Macha's default of 1. Case:
`StoreDistributedStore.MachasFormsSeekWriteCopies`. Gate at the time: `dagda_tests` 411.

## 2026-11-04 night: matching Macha's fixed shapes (`feature/port-respec`)

Under the maintainer's ruling that the libraries match Macha's fixed behaviour (Macha
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

Step 7b's two findings, fixed on the maintainer's word, one commit each. Gate:
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
- For the maintainer: readiness planes are fixed at Dannan's node construction, so
  the owner lists `dagda::readiness_planes()` in `NodeConfig::planes`
  (recommended: ask Dannan for `Readiness::declare` before start). The
  collector and the maintenance loop stay the owner's until a second owner
  needs them.

## 2026-11-04 night: step 6b, one layout and the migration (`feature/one-layout`)

The maintainer: "We don't need backward compatibility": one layout, v19, for every
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

Branched from `develop` at "Record the maintainer's step 6 decisions". One commit.
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
  Recommended to the maintainer: a key probe at activate.

## 2026-11-04: the decided fixes (`feature/decided-fixes`)

The maintainer's answers to the seven open items (Decisions, "2026-11-04 evening"),
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

The maintainer: "Dagda doesn't need ingest, that will be handled by macha. Remove
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
  bounds, the tombstone window, and the two findings (Waiting on the maintainer 5
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
  The maintainer's instruction, not a regression.

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
    groups (`field_group(clock, take)`, each with its own clock: the maintainer,
    2026-11-04; helpers `written(ms, node)` and `dominating(flag, ms,
    node)` for a dominating cancel), the record's lease and tombstone, the
    duplicate rule (`duplicate_key`, `keeps`, `lose`), the record and key
    codec, `max_records` and `retention`. A record merges from the greater
    record, each group taken whole from the greater clock (equal clocks:
    the greater record), the lease by the lease order, the tombstone max.
    `merge(base?, left, right)` is the three-way merge over the key set
    (a null base is an empty one: the maintainer, 2026-11-04), then the duplicate
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
  file or `have_*` probe (the maintainer, 2026-11-04). `Repair::pass` runs the
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
- Legacy readers dropped (the maintainer, 2026-11-04): the monolithic checkpoint
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
- For the maintainer: the control store's `reserve_free` default (Macha's 0 or a
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
  drafted with the Dannan and Lugh sessions' reviews, under the maintainer's four
  rules for the stack. No code; no commits. Gate: none yet.

## Archived from ACTIVE.md, 2026-11-12

The plan as it stood before the rewrite, word for word, its headings one level down.

## Dagda - Active Work

Last updated: 2026-11-05. Dagda's plan follows Macha's code as it lands (0.92.5 is live; old-code removal is under way there), not the 2026-11-02 design
and not the `5a06d3c` baseline (the maintainer, 2026-11-05). Where the design and Macha's code
differ, Macha's behaviour is the plan and Dagda is reshaped to it. `COMPLETED.md` is
the record.

**Resumed 2026-11-06.** Macha's old-code removal landed: 0.93.4 live on both nodes, tag on merge `9a08846`. That is the
baseline. Forms remaining: SM14, 16, 17, 19, DLT5 to DLT11, envelope, hash and tree forms, the legacy clock. Genesis is a
fixed constant record, new clusters only. Torrent stays in Macha through the port (the maintainer): it moves after all of the port work, shelved until he asks. The 23 requests, the SM19 torrent section and DLT9 stay live, "`macha-metadata-dump` identical" holds as first written, and Macha has one space. Dagda still holds no torrent code: Macha's bytes are opaque vectors at S7. Goldens are to be re-cut on 0.93.4.

### The goal (the maintainer, 2026-11-06, relayed by the Macha session)

"development will continue on them with macha code frozen until they can answer Yes unconditionally, the entire reason
for their existence is this."

What it means (the maintainer, relayed, in answer to "do ruled changes count"): "Dannan and Dagda are generalisations of macha's cluster
and storage layer. The intent is to devlop them to the point where they are at least as capable as the macha code they are
based on, then replace the macha code with them. MachaDFS and cluster become systems implemented with Dannan and Dagda as
fundamental libraries. The fidelity point is that after that, Macha should work just as well as it ever did." So the test is
what an operator, client or user of Macha would lose or find worse, and every piece of data surviving, not whether internals
stay the same. v19 and ledgers under custody do not count in themselves. The device reserve, lease fence and claim parts are
judged by the baselines; the options to turn them off stay, for if they are worse. (The last two sentences are the Macha
session's reading, and mine.)

The question: does Macha move onto Dagda with no change in data, functionality, reliability and performance? The only
answer that counts is Yes, with nothing attached. A bounded claim, an exception list or a tolerance is a No. Macha stays
frozen at `9a08846`. There is no other purpose for Dagda; Lugh and Lir do not change that, and L items wait unless Macha
needs them. Tell the Macha session when the answer is Yes.

**How it ends (the maintainer, relayed, 2026-11-06):**
- This plan ends with a capabilities report to the Macha session, with the evidence for each, naming what the port needs from Dagda,
  and saying plainly what could not be shown without a running port. "you are the review", to the Macha session: its judgement of
  that report is the review, and the review by this session after a clear is gone.
- The Macha session builds the port, once satisfied, "which will be frozen during the port": Dagda frozen, `../macha` read only for
  Dagda. Macha stays at `9a08846` until the port starts.
- A defect the port finds: "The port will pause until the relevant agent has identified and fixed the fault, with testing. The port
  will then continue with the relevant codebase frozen again."
- Proof on the nodes: "We will convert live-a and live-b store backends and install there to test, prove and iterate." The real
  migration runs on the live disks after the port is built, with no 670 GB copy, spare hardware or separate timed run. So
  `dagda-migrate`'s resume, refusals and comparison tool must be ready before then. `--reverse` stays built, not a gate.

**The work, in this order. Each item is Dagda's to do.**

1. Heads, now, not behind anything: S2 tree over keys (done), S3 replica on disk, S4 one node commits, S5 exchange, S6 reconciler, S7
   conformance (`2026-11-05-heads-plan.md`, re-based on `9a08846`: H1 to H30 with gates, cases C2 to C46 Dagda must write, all
   95 gap-heads cases assigned). L1 to L9 wait: Macha needs none, and torrent stays in Macha's one space.
2. The metadata and cache seed migration in `dagda-migrate`, in S7. It reports "nothing to do" under a v19 marker today, so
   it needs a later pass.
3. The steps Macha's loop needs: `Repair::pass` and `Collector::pass` split so Macha's pass can order and trace them, with
   `test_maintenance_trace`'s fixtures unchanged. Dagda's own loop to the same fixtures at 8c.
4. Anything Dagda adds must leave Macha working as well. The device reserve can be off by option and is judged by the baselines;
   claim parts are one journal frame; the lease fence is unused unless asked for. Check every difference in requirements
   section 7 the same way.
5. The arbiter's foreground signal in Macha's meaning: Dannan's clocks count every inbound request, Macha's only object bytes.
6. The before and after comparison tool: objects by id and plaintext hash across DATA, CONTROL and cache, removed objects staying
   gone, held tries, claims and dots, retention marker, reference counts, sightings, markers, accounting, and metadata heads,
   certificates and history replay.
7. Crash cases per migration phase, ready before the first real run on the live disks, in process (a power cut is the nodes' to show).
   Done but for: `--resume` from a real partial through the command line (a fixture of 64 records never leaves one; durable every 256). S7 adds
   the metadata reader to the comparison, must read damaged bytes between good frames, and must never hand a half-migrated `history.log` to this
   reader, which cuts it as one torn tail. Rollback (Macha's rule): Macha's binary returns only after `--reverse` has finished, so `--reverse` is
   proved on a real state before the first live run, whatever else it gates. An object rotted before migration reaches only NOT CHECKED, for the
   operator to judge by hand. Macha's `98b816d`, mirrored: a failed read in `load_history` stops the load and cuts nothing.
8. Retirements compared assertion by assertion where they cannot yet run; those that need the port named in the report. The store's
   journal lines mapped against the goldens.
9. At the repin to Dannan's tag, with class-waits merged: peer calls under a no-progress bound (Dannan's `CallWatch`) in place of the absolute 60 s
   `write_deadline` and `read_deadline` past the local wait, since Macha has neither and a transfer still moving past 60 s ends in Dagda and
   goes on in Macha. Read with its own deadline shares a fetch where Macha's is private: check how that looks from outside.
   Linux, ASan, UBSan and TSan on the final tree, with class-waits merged. Answer the `mutex lock failed` report.
10. Dagda's own measurements on the Pis against Macha's baselines at 0.93.4, `../macha/TODO/2026-11-06-pi-baselines.md`, raw JSON in
    `../macha/build/baselines/results/` (uncommitted). Committed at Macha `9020425`. Its caveats: live-b was still filling, and live-a crashed four times from 17:18Z to 18:11Z with heap
    corruption, cause not found, so its figures may not be a healthy 0.93.4's. The maintainer (relayed): the corruption is left until after the port, so
    no sanitizer result comes from Macha before then. The comparison rule is not yet ruled. No baseline: direct store put and get on USB, claim journal latency alone, GC, scrub and compaction rates; the large
    publication test is only "over 120 s". The report says plainly that the cause is unknown, that the stacks show only where the damage surfaced,
    that the one store frame is `ObjectTrie` under `StoragePool::held_view`, and what Dagda did and did not check; none of that clears or
    implicates the moved code.
10b. Open, found 2026-11-07 and 08 (each named in the report until closed):
    - Dannan, asked of the Dannan session: bytes sent do not count as call progress (Macha `net.cpp:1575`, Dannan `session.cpp:98`), so a moving
      upload looks silent; a peer not yet in the roster gets 403 where Macha accepts; a use-after-free on an in-process restart (TSan,
      `session.cpp:107-116`). Macha's 30 s publication budget waits on the first.
    - Pinned at Dannan 0.5.2 (2026-11-09), the tag the port freezes on; suite 1019. Before it:
    - Repinned to Dannan 0.5.1 (2026-11-09, `620f172`, suite 1018, `build/deps/prefix-0.5.1`): bytes sent now count as progress, a peer
      is announced before its first request, a closing session no longer destroys its running handler. The stress case in 17 Release runs
      (3601 s) had no heap abort (at the old one in 14 that clears nothing) and no held-ledger orphan; new: seed 102 with restarts on
      fails twice in two, n0 (restarted without a ledger rebuild) still holding 460 to 700 objects nothing refers to at the end of the
      settle window, its sweep gate "complete; waiting for an event", ids clustered by prefix. Found (`0bce3e9`): Macha's behaviour, not
      Dannan's and not new with 0.5.1. `gc_step` reports `deferred` for its own slice only (`storage_pool.cpp:1134-1135`, Macha
      `storage_pool.cpp:1030-1083`), and Macha's loop reads it from the slice that completes the pass (`maintenance.cpp:1079-1087`), so a
      pass whose not-yet-mature objects all fell in earlier slices ends "complete; waiting for an event" with them on disk until the next
      event. n0, gated shut since its restart, needed two 44 s passes plus that wait, more than the 120 s settle. Pinned by
      the pin. Garbage is late, never lost or wrongly deleted. Fixed in Dagda at the maintainer's word (2026-11-09): `gc_step` carries the deferral
      across the pass (`_gc_pass_deferred`); Macha's loop after the freeze. The maintainer: no more stress runs.
    - Macha's 30 s publication budget is `fuse.publication_no_progress_deadline`, a write-wide no-progress window passed only by
      publication (Macha `config.hpp:132-140`, `fuse_frontend.cpp:3576-3582`, `distributed_store.cpp:352-386`): it restarts on any copy
      landing, a fallback starting, any call idle under 30 s or the shared counter moving; when it fires, unfinished calls are cancelled
      and the write succeeds if one copy landed (the rest owed to prompt replication), else StorageUnavailable. Dagda has no write-wide
      budget then. Added at the maintainer's word (2026-11-09): `WriteOptions::no_progress_budget` and `progress`, off by default, Macha's
      publication passes 30 s at the port; `call_no_progress` stays 300 s, above the 120 s credit wait (settles the 300 s question).
    - Earlier: Dagda was pinned at Dannan 0.5.0 (`build/deps/prefix-0.5.0`). Peer calls run under `call_no_progress`, 300 s by default: the agent's choice,
      past a peer's 120 s credit wait; Macha's is unbounded, Dagda's rule forbids that, so a silent peer on a live session is a named difference.
    - A full control store: live-a's filled 4G on 2026-11-06 and refused every control write for about 40 hours, silently (status healthy,
      73,842 "control object unavailable" lines, a namespace write answered 400), freed by nothing until the limit was raised and Macha restarted.
      Dagda must behave and recover the same; the silent status and unbounded retry are Macha's defects for after the freeze. live-b stood at 94% of 4G; it was raised to 8G on 2026-11-08.
    - W1 (a peer may sweep an old unreferenced object just as another node re-references it, before it hears of the commit): Macha's behaviour,
      kept, costs one replica until repair; the fix is the maintainer's, the Macha session recommends after the port. Nothing restores a lost claim record.
    - The delete stress case (opt-in): two Release-only heap aborts with no writer found, in runs with in-process restarts; a held-ledger entry
      for an object no disk lists; races in `Node::publish_status` against `ReferenceCounts` (Dagda only) and `HeldLedger::stats` (unused, as
      Macha's). Settling under sanitizers often stalled in the quiet window. None explains live-a's crashes, nor clears the moved code.
      Pinned down 2026-11-08 (`4002e9c`): the abort also happens with restarts and the instant delete both off (one in 14 Release runs,
      freed under `HeldLedger::held_with_prefix` from `verify_step`), so it is not Dannan's restart use-after-free; ASan, UBSan and the
      malloc checkers found nothing in 14 runs; no writer found. The held-ledger entry is pack compaction racing a packed put: the put's
      record is in the active pack but not yet indexed, compaction counts it dead, can take the active pack as victim and unlinks it, and
      the object is lost while the ledger still says held; on a remove the same gap brings a deleted object back. Macha `98b816d` has the
      same lines (`local_store.cpp:821-867`, `:1694`, `:1700-1714`, `:1842`), and `StoragePool::get` stops at the first backend's empty
      answer (`storage_pool.cpp:591-610`). The maintainer chose to fix it in Dagda now and in Macha after the freeze (2026-11-08): compaction now
      waits, under the pack lock it already holds, for every written record to be indexed (`LocalStore::_unindexed_appends`), proved by
      `StoreLocalStore.ACompactionWaitsForAPutInItsAppendGap` and `...ARemoveInItsAppendGap`, which failed first. Dagda is safer than Macha
      here; the report says so. The pool's first-empty-answer read is still Macha's and still open. The status race is fixed (`ReferenceCounts::_totals` under its own mutex). The quiet window is Macha's design: a node whose
      arrivals come faster than the window never sweeps until they stop.
    - A transient 409 when a DATA and a CONTROL id coincide (the Macha session's review of `f0cbb23..826a149`): a delete marks the id alone, so
      a claim of the other class on that id is refused until the delete ends. Macha's behaviour, kept: one `RetentionStore` per node
      (`local_state.cpp:156`) behind one `ClaimLocks` keyed by id (`claim_store.hpp:102-144`), checked at `storage_server.cpp:374-381`.
      Named in the report, not Dagda's to close.
    - Lost wakeups (the Macha session's audit, its found-issues item 14, 2026-11-08; for the report, no Macha change wanted now). Every
      notify in Dagda's tree checked (2026-11-08): none has the shape. Differences from Macha, each already closed in Dagda: the local
      writer's and the durability worker's waits take the stop token (`distributed_store.cpp:1892`, `durability_domain.cpp:193`), so
      `request_stop` wakes them through `condition_variable_any`'s own lock; prompt replication waits timed while a retry is queued
      (`:1512-1518`); the scan's completion and failure are set under the mutex their waiters read (`local_store.cpp:175-181`,
      `:1749-1754`); the exchange's replicator, `time_advanced` and maintenance wakes take the waiter's mutex before notifying
      (`exchange.cpp:1146-1152`, `node.cpp:607-614`, `data_work.cpp:204-211`). The same as Macha: the local writer's DATA admission
      carries no cancellation, so a store stopped while it waits for credit is held until a release or the no-progress window (120 s on
      the live nodes); Macha's flag without a notify gives the same bound. Where a caller's flag exists, ClassWaits polls it every 25 ms.
    - From S7, settled 2026-11-10: Macha's facade opens the head space as `macha/namespace` (the migration's default), and a heads node
      finding Macha's unmoved metadata is refused naming `dagda-migrate --heads` (`COMPLETED.md`, 2026-11-10).
    - The maintainer, 2026-11-10: live-b's control store is 8G (since 2026-11-08 12:49Z, 3.9G used); the baselines are done, and what the port
      finds is fixed after it. Dagda's arm64 run (plain, and ASan with UBSan, GCC 14) is on live-b at the lowest priority, as Dannan's,
      Macha untouched. The rehearsal on live-a's state copy (`--reverse`, `--resume` from a real partial) comes before the first live
      conversion, not before the port is built.
11. The capabilities report.

**Macha's freeze exception (the maintainer, relayed by the Macha session, 2026-11-06).** The history tail cut S3 found is fixed in Macha first,
as 0.93.5, committed at Macha `9153011` on develop, laptop gate only, not tagged and not on the nodes: a frame that does not open no longer ends `load_history`'s scan, it looks forward byte by byte for the
next frame that authenticates and leaves the damaged bytes in the file; a length over 2 GiB no longer throws; only when nothing later
authenticates is the rest a torn tail. Dagda mirrors it from that commit, with `test_a_damaged_history_frame_loses_only_that_frame`
and `test_a_torn_history_tail_is_still_cut`. The live nodes stay on 0.93.4 for the baselines. Until the mirror merges Dagda's replica has the old
shape. The other two read as unreachable and are reproduced: the cache's byte count at `reanchor_history`, and PREPARE then COMMIT with
no certificate. `set-aside.meta` cannot be cut on Macha's bytes; the report names it as not shown there.
No on-disk or wire form changes in 0.93.5, so the report names it: forms cut at `9a08846`, behaviour baseline `98b816d` (0.93.5, on `9153011`),
and the port branches from that commit. A `history.log` may now hold damaged bytes between good frames, which `dagda-migrate` and the
comparison tool must read. New words for the journal mapping: Status reads 0.93.5, a torn tail's `reason=` reads "last frame failed
AES-GCM authentication", and "record too large" is gone from this path.

**The maintainer (relayed, 2026-11-06): Dannan's tag and push are approved.** Dannan sends the tag when its last small commit is gated, which unblocks
`feature/class-waits`.

**Asked of the maintainer (2026-11-06), all answered above but the baselines' comparison rule.**

### The earlier table, kept for its gates and details; the goal above decides the order

The goal is the second cutover: Macha's store onto Dagda, in stages (Decisions, 2026-11-05), after the
transport cutover. Everything is ordered by what that needs. Nothing Lugh or Lir needs is on this path (P7).

**The documents, each the detail of a part of this plan:**
- [`2026-11-03-macha-store-requirements.md`](2026-11-03-macha-store-requirements.md):
  re-based; 0.92.3's rows are section 3.7, section 7 revisited.
- [`2026-11-05-second-cutover.md`](2026-11-05-second-cutover.md): steps S, 8a, 8b, 8c, 8d,
  each with Macha's files, Status keys, config keys and gate; the migration plan; what is
  MISSING in Dagda (A Status, B config, C behaviours B1 to B15).
- [`2026-11-05-test-mapping.md`](2026-11-05-test-mapping.md): all 786 of Macha's cases:
  143 retire, 167 rewrite, 354 keep, 27 gap, 95 wait for heads. For the Macha session to
  confirm; 49 uncertain ones are listed.
- [`2026-11-05-heads-plan.md`](2026-11-05-heads-plan.md): step 4 from `b080c38`: H1 to H30
  with gates, sessions S1 to S7, Lugh's and Lir's L1 to L9, the re-check list at the merge.

These were drafted from reading, with citations checked by script and spot-read, not all
by eye. Nothing in them has been run against Macha.

**The order.** Each gate is named in the document cited.

| # | Work | Gate | Waits on |
|---|---|---|---|
| P0 | The port's artefacts at `cac5998` (below): the Macha session's real state, dump output and goldens; Dagda's export of Macha for small generated states | Macha's own code produced them, at a named commit | The Macha session |
| P1 | Scale and platform (M10): done, Mac and live-a (`COMPLETED.md`). Left: TSan on Linux, for the Boost report only, and the USB disk, which the rehearsal measures | Whole suite on Linux aarch64; the scale figures recorded | Nothing |
| P2 | The mapping's 27 gaps: write path and durability 8, repair and maintenance 2, arbiter 4, control fetch 3, placement and selection 4, old state 1, predicates 5 | The named Macha case, moved to `dagda_tests` | The maintainer, for predicates |
| P3 | The node for the first stage under Macha's loop: its stages callable one by one, B1, B3, B6, B7, B8, B10 of second-cutover C, about 28 `dagda.*` figures, config keys and defaults. Dagda's scheduling, parking and trace wait for the later stage | S.4's named cases and VALIDATION sections | Nothing |
| P4 | Match Macha as it lands: old-code removal, then 0.92.6 (the three findings, the inventory walk's stop, the `followed` table, the exact read-failure shape), then its baselines | Macha's cases, in the shape of its fix | The Macha session |
| P4b | `DataResourceArbiter` and `IoReserve` on Dannan's `ClassWaits`: built on branch `feature/class-waits` against Dannan `develop` `415c6c4`, 449 passing unchanged. Merges when a tag has it. Required before stage one | `test_io_pressure` as moved; `DataResourceStats` keeps its meanings | Dannan's tag |
| P5 | Heads S1: the cache seed's migration, moved to 8c with the metadata files, as the maintainer ruled | Heads plan 2.2 | P8 |
| P6 | Dropped for now (the maintainer: "I'll decide later"): the real run, there and back, the rehearsal. The dry run over live-a's real state stands as done | Second-cutover "Migration plan", when he decides | The maintainer |
| P7 | First stage: S, 8a and 8b, one disk event | S.4, 8a.4, 8b.4; all 11 crash-matrix rows; the Status golden | P1 to P6, the first cutover |
| P8 | Later stage: heads S2 to S7, 8c with `dagda-migrate` moving `<state>/metadata/` and the seed, Dagda's loop with `test_maintenance_trace`, then 8d, against `808fa0f` | Heads plan section 3, re-read at `808fa0f`; 8c.4, 8d.4; `macha-metadata-dump` output identical | 95 gap-heads cases each get a session or a named retirement |
| P8b | Gone: the Macha session's judgement of the capabilities report is the review (the maintainer) | | |
| P9 | Lugh and Lir: L1 to L9, then `Context` v1 frozen, tag `0.1.0` | Stack document's requirements | P8 |

**Macha is frozen at `cac5998` until the libraries are ready (the maintainer, relayed by the Macha session,
2026-11-05: "Macha has a working, tested codebase, this shouldn't be modified until they're
ready.").** The libraries do the port's work; Macha's code does not change. Macha can still produce
artefacts and answer questions. So:
- **From the Macha session, read from `../macha/build/macha-fixtures/`:** the goldens are in
  `goldens/`, from two real processes: Status with a 161-path mask, 381 of 494 keys reached at most;
  the config dump is 46 fields by hand, not all of `Config`; `routes.txt` is read from source, not
  captured. Taken from live-a's stop (the maintainer): `/root/macha-state-20261105T145340Z` on the node, 3.0 GB of state and a
  257 MB DATA sample, at 0.92.4 (its `ORIGIN.txt` says what was copied stopped and what after), with
  `macha-metadata-dump.txt` beside it. Next: a dry run on live-a with the key staying there, `macha-metadata-dump` output, and there and back
  under Macha's own code. The source to build is at `~/dagda-linux/dagda-develop` there.
- **Dagda's own, as well:** `git archive cac5998` into `build/macha-cac5998/`, built and run there,
  never in Macha's tree. It lets Dagda generate as many small states as a test needs, and have
  Macha's own code open a disk after `--reverse`. This machine has no FFmpeg development files, so
  the export builds with `MACHA_BUILD_SERVER=OFF`.
- **Differences that stand for the freeze:** the survey's rethrow of the stop, the multiset of
  deletion marks and the `not_held` pause have no Macha fix to match yet. A later Macha fix or a
  ruling may rename or reshape them. The inventory walk's stop check will not appear in Macha
  during the freeze unless the maintainer says otherwise, so P4 has nothing further to wait for.
- **No tree-native merge under the freeze (the Macha session's finding, 2026-11-05, not a ruling):** heads
  is built against `cac5998` as it runs, and 0.92.5 when it lands. Both nodes run 0.92.4. Both live heads are tree-backed SM19; live deltas are DLT5.
  Genesis is `genesis_metadata()` (`metadata.cpp:2294`), an inline record built at runtime, not a
  constant, and `b080c38` does not change that. A new cluster at `cac5998` founds inline, so the
  scratch state must not gate a tree-era tool. An unchanged mutation commits, about 26 empty 61-byte
  deltas a minute live, and that is the requirement until the maintainer rules. The heads plan's `b080c38`
  citations and "settled at the merge" marks are to be re-read at `cac5998`.

**Seams with no owner (found 2026-11-06, reading both trees; sent to the Macha session for the maintainer):**
1. Custody and certificates: `dagda::Node` refuses a node without custody and `advertise_wrap_key` throws without a
   certificate, and Dannan's first cutover leaves both off on Macha's nodes. Nobody's plan provisions them. A blocker.
2. Message numbers: Dannan's first cutover registers Macha's store types at the numbers Dagda takes; nobody says who
   removes which at stage one.
3. Adverts: `macha/capacity` and `macha/used` against `dagda/capacity`, `dagda/used`, `dagda/extents`.
The maintainer ruled it does not: torrent moves after all of the port work, so the "dump identical" gate stands and Macha has one
space.

**Macha 0.92.5 (`808fa0f`, the baseline now, live on both nodes):** a stale tombstone held in a batch is
erased: the batch is dropped and the rest written again at its own retirement time, and a batch the
erase dropped is not dropped again as matured (`maintenance.cpp`, `test_a_stale_tombstone_in_a_batch_is_erased_once`).
No Dagda code changes: Dagda has no tombstone batches yet. It joins heads row H23 and the retirement
provider's case at S4 or S5. Empty commits went from about 20 a minute to 0 on the nodes.

**Macha 0.92.4 (`cac5998`, tag on `70cf4a4`), taken:** the claim deletion mark and the
survey's stop (`COMPLETED.md`). The inventory's walk has no stop check yet in Macha either.
Macha's trace flake was not credit or CPU, as first thought: a commit ahead of the survey flips
repair's push, and each flip owes a pass; the fixture was fixed, and no Dagda plan rests on the
old explanation. Sent to Macha and confirmed by its reading, going to the maintainer: the survey's per-peer
catch swallows the stop, and two deletes of one id share a mark. Dagda already has the rethrow
and a multiset, the shapes Macha will recommend; the exception's name and place may follow the maintainer.

**Open differences from Macha, kept until the maintainer rules there:** the device I/O reserve, the
data plane not ready with no backend online, the lease epoch fence, the claims guard held
4096 ids at a time. Requirements section 7 has them.

**Known gaps found by the plan, in P2 or P3:** maintenance polls where Macha parks and wakes
on a peer event (`node.cpp` `maintenance_loop`); GC rests on a fixed interval rather than
events or an exact grace time; no decision trace through the node; a stale inventory on a
node that stays busy; a corrupt remote copy reads as held until its own scrub, where Macha
heals it from the sender.

Dagda's specification is the requirements document above. How Dannan, Dagda and Lugh divide
the work is [`2026-11-03-stack.md`](2026-11-03-stack.md), the one copy for all three. The
2026-11-02 design documents are history; they do not decide anything.

### Dannan, as Dagda finds it (from the Dannan session, 2026-11-03 night)

**Dannan 0.4.1 is pinned** (2026-11-04 night): `Readiness::declare`, so
`dagda::Node` declares its own planes at open. `Principal::operator==`
is adopted (`WritePrincipal` defaults its comparison).

**Dannan 0.4.0 is cut** (`d657a9b`, 2026-11-04 night;
`COMPLETED.md`). Its additions for Dagda: step 6's three requests are in. `identity::
x25519_public(private)` (`fba0977`) replaces `x25519_shared(private, 9)`
in `domain/wrap`. `context.nodes().certificate(node)` (`45477bb`), the
certificate a peer proved in its latest v2 handshake with this node,
checked against the CA, empty for a peer never met by certificate, met
only by cluster key, or revoked since; and `context.identity().
ca_certificate()`: the wrap advert drops the certificate DER for peers met
by certificate, keeping it as the fallback for a peer never talked to (or
a call first, which establishes the session). `contract::Principal` has
`operator==` (`fcd1a6f`). Also there: `dannan::config` (`e375b08`, the
shared settings reader on yaml-cpp, linked privately), for a Dagda node
binary that takes a config file; `dannan::encoding` percent-encoding.
Dannan's gate 422.

**Dannan 0.3.0 is cut** (annotated tag on `main`, `277adab`, 2026-11-04;
ASan/UBSan and TSan clean, 408 tests each) **and Dagda pins it**
(`find_package(dannan 0.3 CONFIG)`, `feature/dannan-0.3`). Everything
below noted "untagged" is in it: the identity step (certificates as wire
v2, revocation, `dannan-ca`, the scope seam), `contract::Unlocked`, the
deadline that cancels a call (the race in `5f14127` fixed in `8b65dd1`,
inside the tag), `Time::source()` and `executor()`, `sched::DutyShare`,
`NodeEvents::membership` and `Nodes::absent_since`. Dagda's file-local
stand-ins are gone with the repin; `TimedLock` is not in 0.3.0, so the
TODO in `store/storage_pool.h` stays.

- Dannan is on `feature/wire-v1` (head `d9b023b`): wire v1, level 0 and
  `dannan::Context` (`dannan/context.h`, context version 1), not yet
  merged to `develop`. Gate there: `dannan_tests` 323, `dagda_tests` 1.
  Tag `0.1.0` predates wire v1 and the Context and is not used here.
- No install or export target and no tag carrying the Context. Dagda
  builds against the sibling checkout (`add_subdirectory` or
  `FetchContent` with `SOURCE_DIR`) until one exists. Target `dannan`
  (shared); Boost 1.87+, OpenSSL 3, Threads.
- Message numbers 1 to 511 are Dannan's and refused to anyone else.
  User types register freely from 512, no per-project blocks; a clash is
  refused at registration (the maintainer, 2026-11-04).
- **Dannan 0.2.0 is cut** (annotated tag on `main`, `fcfe5f4`, local,
  not pushed; ASan/UBSan clean at 378 + 1, TSan clean). Dagda pins it:
  `find_package(dannan 0.2 CONFIG)` and `dannan::dannan`, compatible
  within 0.2.x, or `FetchContent` of the tag. It carries wire v1, level
  0, channels, `Nodes::watch` and incarnations, the foundations, and the
  shared test fixtures (`test::ChildProcess` among them); a consumer of
  `dannan/test/fixtures/temp_dir.h` defines `DANNAN_TEST_TEMP_ROOT` to a
  directory in its own build tree.
- Store figures (capacity, used, generation, write floor) travel as
  adverts Dagda sets: at most 64 per node, each up to 1 KiB. A placement
  `Scheme` has no default salts; Dagda names its own and the advert that
  holds capacity (big-endian u64, `NodeRecord::advert_u64`).
- Dannan's API is not stable. Likeliest to change soon: Nodes and
  placement (told a peer's path class), Diagnostics.
- Level 0 is proven through real NATs (the compose bed, Dannan
  `0f0826e`): path classes, punches, relays, a healed partition, relay
  failover. What Dagda relies on from it: a path change is a transport
  reconnect under the same node identity; a call in flight when its
  session dies fails with a reason, and Dagda retries under its own
  policy; routes come back on the next call, or for a NATed node on its
  next heartbeat. Dagda never sees which path is in use.
- Secure memory has landed in Dannan: `dannan/identity/secure_memory.h`
  (`SecureBytes`, `Secret`, `harden_process`). Dagda's domain keys and
  node-local keys live in it; Dagda writes no secure memory of its own.
- Node key custody has landed (`dannan/identity/key_custody.h`,
  `99b7fbb`): Dagda's node-local domain keys are sealed under
  `KeyCustody::derive("dagda/node-local/1")`. Argon2id is Dannan's too
  (`identity/argon2.h`).
- `Identity::durability_epoch` is a random 128-bit id per process: it
  tells a restart from a reconnect but is not ordered. Dagda persists its
  own counter for anything that needs order across restarts.
- Dannan's identity step is done (`09a525f`, merged to its `develop`;
  design `../dannan/TODO/2026-11-04-identity.md`; untagged, 0.3.0 waits
  on the maintainer). For Dagda: the scope check is
  `context.identity().verify(contract::Principal{kind, id}, prefix)`
  returning `contract::ScopeDecision {allowed, generation, why}`, never
  waiting, with the no-source default Dagda proposed; domain keys wrap to
  `identity().certificate()` and `Established::peer_certificate`;
  `identity().sign(message)`; `derive_local(label)` unchanged;
  `Identity::revoke` and `revocations()`. Used from step 6.
- After 0.2.0, on Dannan's `feature/wire-v1` (`f32053e`), Dagda's three
  requests from 1a: `status::set_thread_name` (and supervised threads
  named after their name), `status::resident_bytes()` and
  `physical_memory_bytes()` (`dannan/status/process.h`), and
  `status::ObservedDuration` with `elapsed_us()` and `cancel()` on the
  injected time source (`dannan/status/observation.h`). Dagda moves its
  pin from 0.2.0 to that commit, or to Dannan's next tag, when a step
  first uses them.
- On Dannan's `develop` (`a26a210`), untagged, Dagda's two requests from
  3a: a `Routes::call` past its deadline is cancelled through its handle,
  its reply slot freed and a late reply dropped (`5f14127`), so the
  store's 120 s `call_horizon` goes at the repin; and
  `context.time().source()` / `context.time().executor()` (`7c1501e`),
  so the store takes its clock and executor from the Context instead of
  beside it.
- On Dannan's `develop` (`1d61b3f`), untagged, Dagda's requests from 3b:
  `sched::DutyShare` (`dannan/sched/duty_share.h`, `11b803e`) replaces
  the file-local share in `store/repair.cpp` at the repin; and
  `status::NodeEvents::membership` (`42a65f8`), a count that moves only on
  joins, leaves, place, capabilities, path class or incarnation, never on
  adverts: the survey re-asks on it instead of topology. Hosted event
  kinds now number from `NodeEvents::dannan_kinds`; Dagda names kinds
  through `events().kind(name)`.
- `Nodes::absent_since(node)` on Dannan's `develop` (`c201b6f`,
  `a7fdfd0`), untagged: the steady time a node was last heard of, on
  `context.time().source()`, surviving a restart; empty for self, an
  active node or an unknown one. "Left" fires about `dead_after` after
  it. Dagda's `lease/membership` drops its own absence start for it at
  the repin (its `TODO(Dannan: absence on the roster)`).
- `contract::Unlocked` (Dannan `35d14e0`, develop `b24db75`): a scoped
  unlock of a held `contract::Lock`, annotated. Dagda's file-local copy
  in `store/local_store.cpp` is replaced by it at the repin to 0.3.0.
  `secure_memory.h` now says secure memory is not inherited across fork.
- Notify channels have landed (`dannan/net/fanout.h`,
  `Context::channels()`, `2f16981`). Retained is Dagda's own: `retain()`
  on a notify channel plus a Dagda head; Dannan declares no seam for it.
- Secret-handling helpers have landed (`1de966f`): `SecureBuffer`,
  `net::BasicWriter<SecureBuffer>`, `aes_gcm_open_secure`,
  `hkdf_sha256_secret`, `Argon2Costs::valid()`. Dagda opens sealed
  records and derives domain keys into secure memory with these.
- Store messages keep Macha's payload encodings inside Dannan's wire v1
  framing, under Dagda's numbers from 512 (the maintainer's format decision, as the
  Dannan session reads it too).
- Dannan's `dagda/` stub is gone (Dannan `b8483e2`, merged to its
  `develop`): Dannan defines no `dagda` target, and its gate is
  `dannan_tests` alone.

### The earlier plan (approved by the maintainer, 2026-11-04), now the record of steps 0 to 7

Steps done are in `COMPLETED.md`. What remains of it is in the table above.

Bottom up, in the order Macha's store depends on itself, so each step
has a gate before the next starts. One step per session and `feature/`
branch, as in Dannan. Code moves from `../macha` (read only: copied out,
never edited there), is generalised here, and swaps Macha's transport,
crypto, durable files, logging and time for Dannan's as it arrives.
Requirement ids (S1, L4, H20...) are the requirements document's.

0. ~~**Skeleton.**~~ Done 2026-11-04 on `feature/skeleton`: links
   Dannan 0.2.0 by `find_package`, `dagda_tests` 3 (`COMPLETED.md`).
   `--version` arrives with Dagda's first tool.
1. **Local store** (`store/`, `domain/` v1), on `feature/local-store`,
   in four sessions, because `LocalStore` keeps the held trie and the
   pool and cache reach into config, placement and metadata:
   - ~~**1a**~~ Done 2026-11-04 (`COMPLETED.md`; `dagda_tests` 37).
     Foundations and sealing: Macha's foundations mapped onto
     Dannan's (codec, crypto, logging, durable files, thread safety,
     observation, work classes), with byte compatibility checked where a
     format depends on them; object ids, the storage key derived from the
     cluster key, sealing with the id as AAD; the presence index; the
     durability domain and tickets.
   - ~~**1b**~~ Done 2026-11-04 (`COMPLETED.md`; `dagda_tests` 67).
     The held trie (`ledger/`: object trie, held ledger, trie
     union), which `LocalStore` keeps.
   - ~~**1c**~~ Done 2026-11-04 (`COMPLETED.md`; `dagda_tests` 108).
     `LocalStore`: loose files and packs, torn tails, compaction,
     tombstones; write-behind.
   - ~~**1d**~~ Done 2026-11-04 (`COMPLETED.md`; `dagda_tests` 145).
     `StoragePool`, I/O pressure, `unreferenced_since` (in `ledger/`),
     the CONTROL store, the persistent cache without its metadata seed
     (`with_seed` is where step 4 attaches it).

   What remains of step 1's gate, all of it outside the code moved:
   - **The Macha fixture**: a state directory opened with `has` and
     `get` true for every object. Waits on the Macha session's step-1
     fixture (`build/macha-fixtures/step1-*/`), cut after Macha's
     tree-native branch merges. Run it through `StoragePool`, the control
     store and the cache, with the README's packing threshold and backend
     limits as the options.
   - ~~**"Refusing unversioned storage"**, its state half~~: done in
     step 2 (`store/state_layout`, `StoreStateLayout`), Dagda's, with
     Macha's bytes. The backend half is `ANonEmptyUnversionedBackendIsRefused`.
   - **For the maintainer, not blocking**: the control store's free-space reserve
     defaults to Macha's 0. A figure above 0 would keep CONTROL writable
     after DATA's reserve stops DATA writes on a shared filesystem.
   - `test_storage_v18`'s remaining cases are distributed (step 3), and
     the crash matrix's remaining rows are the FS's, the metadata's
     (step 4) and the catalogue's.

   The whole of step 1: `LocalStore`, loose files
   and packs, torn-tail truncation and compaction, `DurabilityDomain`
   generations and tickets, `StoragePool` over several backends with
   capacity ranking, the separate CONTROL store, the persistent cache;
   one domain sealed exactly as Macha seals (object id as AAD), so v18
   state opens unchanged. Small values: packs are the small-object path,
   measured here. Gate: the crash matrix, `test_storage_v18`,
   VALIDATION "Physical representation", "Admission and capacity",
   "Refusing unversioned storage", and a Macha fixture state directory
   opened with `has` and `get` true for every object.
2. ~~**Ledger, local**~~ Done 2026-11-04 on `feature/ledger`
   (`COMPLETED.md`; `dagda_tests` 198). Claims and retention (WAL,
   checkpoints; legacy readers dropped, slots reserved); reference
   counts; the three gates; horizons built from a `ReachabilityProvider`;
   the claim walk; GC's two sweeps in `ledger::Collector`, CONTROL GC
   included; the state layout marker. Gate met: `test_horizon_builder`,
   `test_ledger_gates`, `test_claim_walk`, the GC cases, a claim from an
   unseen branch surviving, the include-direction check. What remains,
   none of it local:
   - Step 3: the availability survey, repair by diff, the claim walk's
     restorer and its network credit, the pass's scheduling (quiet
     windows, events, the repair share) and the DATA sweep's yield to the
     store's idle times.
   - Step 6: several providers per domain, unioned (complete only if all
     are); one provider until domains exist.
   - At the port (8a): Macha registers its provider (the FS's cached walk
     and census, the catalogue's maintenance and retention objects, its
     root epoch as the CONTROL epoch) and its pass calls
     `Collector::pass`; `test_maintenance_trace.cpp` and
     `test_catalogue_maintenance.cpp` stay Macha's gate.
3. **Distributed store on Dannan** (`store/`), on
   `feature/distributed-store`, in two sessions:
   - ~~**3a**~~ Done 2026-11-04 (`COMPLETED.md`; `dagda_tests` 236).
     The write and read paths: the storage server's routes as Dagda
     message types on Dannan, placement over the roster and adverts, the
     four durability levels with deadlines and evidence by node id,
     tickets that outlive the call, spill past stalled peers and prompt
     replication, reads local then cache then latency-ordered remote,
     CONTROL objects offered and claimed, DATA claims.
   - ~~**3b**~~ Done 2026-11-04 (`COMPLETED.md`; `dagda_tests` 284).
     Repair and admission: the availability survey and repair by diff
     over the network (trie questions against one pinned snapshot; no
     cursor walk, `repair_position` file or `have_*` probe), the claim
     walk's restorer with its network credit, repair paced by a duty
     share and never stopped, repair reporting into tickets
     (`WriteEvidence::complete`, `wait_complete`), the DATA credit
     arbiter and the device I/O reserve for control work (section 7),
     the admission of each served request, `have_valid_objects`,
     `delete_object`, `trie_diff`, `replicate_all`. Rebalancing across
     nodes is repair's push and local drop: Macha has no other.

   ~~The whole of step 3~~, done 2026-11-04: Placement through
   Dannan's functions (owner salts, capacity adverts); the write path
   with the four durability levels and evidence by node id; the read
   path (local, cache, latency-ordered remote); CONTROL objects; the
   storage server as Dagda message types from 512; repair by diff only;
   tickets that outlive the call. Gate met for what is the store's: the
   store cases of `test_rpc_cluster`, `test_availability` (its survey;
   the path table is Macha's), `test_io_pressure` (all 13), VALIDATION
   "Placement and the write floor" and "Class separation", on in-process
   Dannan clusters. What remains, none of it the store's code:
   - Not moved, each Macha's or another step's: `test_rpc_cluster`'s
     `test_a_delete_is_reclaimed_while_a_node_is_away` (a whole node's
     pass over the FS's namespace) and `test_replication_policy_change_on_restart`
     (settings reload, section 7 item 2, with Context v1, step 7);
     `test_storage_v18`'s heavy `test_retain_data_batches_a_large_publication_within_bounded_time`.
   - The rest of Macha's maintenance pass (when GC is due, its quiet
     windows, the DATA sweep's `should_yield` from the activity clocks):
     Macha's loop at the port (8a) calls `Repair::pass` and
     `Collector::pass`; Dagda has the steps, not the loop.
   - Dannan: a duty-cycle share by weight in `sched/` (Repair's is
     file-local, `TODO(Dannan: ...)` in `store/repair.cpp`).

4. **Heads** (`heads/`), in sessions. The commit DAG made opaque (codec,
   merge, provider supplied by the owner); acceptance by durability
   level; head sets, dots, truncation; the reconciler with the common
   ancestor when held; durable conflicts; the generic Merkle tree over
   keys with an owner's leaf codec; genesis as a constant per space;
   `replicas: N` with the owner's placement salt and replica set as
   evidence; no per-delta whole-state encode. Macha's FS codec and merge
   live in the tests as the first owner, to prove every tree-form byte
   (SM14/16/17/19, DLT5-11) and every hash. Gate: `test_metadata_merge`,
   `test_namespace_tree`, `test_storage_metadata`, `test_absent_nodes`,
   the `rpc_cluster` metadata cases, `macha-metadata-dump` identical on
   a fixture, a `replicas: 3` space on five in-process nodes.
5. ~~**Leases** (`lease/`)~~ Done 2026-11-04 on `feature/leases`
   (`COMPLETED.md`; `dagda_tests` 322 and 3 disabled). The lease
   (`Lease`, its order, join and bytes), the claimed-collection template
   (`ClaimedCollection<Traits>`: field groups with their own clocks, a
   dominating cancel, the lease, a max-wins tombstone with the owner's
   window, the three-way merge with the base when held, the owner's
   duplicate rule, snapshot and delta sections with Macha's bounds),
   `LeaseTable`, `LeaseKeeper` over an injected `Membership`
   (`RosterMembership` on Dannan's roster) and the epoch fence (new).
   The template is tested over a neutral task record
   (`lease_claimed_collection_test.cpp`); Macha's torrent requests, the
   first instance, were removed with all ingest (the maintainer, 2026-11-04, see
   Decisions; `dagda_tests` 317 and 3 disabled). Gate met but for what
   waits:
   - Step 4: `LeaseRecords` (`lease/keeper.h`) over a head space holding a
     `LeaseTable`; the records living in a head.
   - For the maintainer (the findings in `COMPLETED.md`): the template's unsigned
     tombstone window and its delta accepting a record with no key
     (Waiting on the maintainer 5 and 6).
   - At the port (8b), Macha's: its torrent requests as an instance of
     the template, their bytes against its SM16/17/19 and DLT9 sections,
     its duplicate rule (three concurrent duplicates settle only at the
     next merge), and its coordinator's takeover until `acquire` has renew
     and release (requirements 5.4). Dagda holds none of it.
6. **What Lugh and Lir need beyond Macha.** ~~The parts that need no
   head~~ Done 2026-11-04 on `feature/domains` (`COMPLETED.md`;
   `dagda_tests` 347 and 1 disabled): node-local domains under custody,
   the X25519 wrap key per node and the wrap of a domain key to a member,
   member and blind roles at the local store and pool, layout v19 for new
   backends with the domain in the AAD, the scope check at the write
   floor with principal and prefix in `WriteOptions` and the put message,
   the binding test. Still to do, with step 4 (heads): the domain
   catalogue as a head space, blind replicas on the wire and in placement,
   maintenance on a blind pool, node-local spaces, the Replicated level's
   per-space k, retained values on Dannan's notify channels, the
   no-rollback test. Gate: each from the stack document's requirements.
6b. ~~**One layout, and the migration of Macha's disks**~~ Done 2026-11-04
   night on `feature/one-layout` (`COMPLETED.md`; `dagda_tests` 367 and 1
   disabled) but for its real-data gate, the Macha session's v18 state
   directory (dry run first). As built: The store knows layout v19 only: every
   backend, the control store, the cache and the state layout carry the
   domain, v1 (Macha's) included; every object's AAD binds the domain;
   every node's ledgers (held tries, journals, retention claims,
   reference counts, the cache's index) are sealed under the node-local
   key, so a blind node needs nothing it does not have. The v18 readers
   move into `dagda/migrate/` and the tool `tools/dagda_migrate.cpp`
   (`dagda-migrate`), which migrates a node's state and backends in
   place: an object's v19 ciphertext under the same key and nonce is its
   v18 ciphertext (only the GCM tag differs), so each loose object and
   pack record has its 16-byte tag rewritten in place and nothing is
   copied; the ledgers are re-sealed file by file under the node-local
   key; markers and the state layout are rewritten last; a crash leaves
   a disk the tool resumes (each record is v18 or v19, told by which AAD
   authenticates it). Peak extra space: one ledger file. Manual only: the
   store refuses a v18 disk with a message naming `dagda-migrate` and
   never migrates on its own. The key probe at activate. Gate: the v18 known-answer bytes, now the migration's
   tests, migrate to the v19 known answers; a Macha v18 state directory
   from the Macha session, migrated, opens and reads back every object
   and claim; `dagda_tests` loses the v18 store cases only, each named.
7a. ~~**The node, everything but heads**~~ Done 2026-11-04 night on
   `feature/node` (`COMPLETED.md`; `dagda_tests` 376 and 1 disabled). `dagda::Node`: opens a node's store from its
   `dannan::Context` and config (the state layout, the DATA pool, the
   control store, the cache, the claims, reference counts, the
   distributed store, availability, repair, the storage server), taking
   `StorageLock` on every root (step 6b's finding) and refusing a v18 disk
   naming `dagda-migrate`; registers Dagda's message types (C5);
   publishes its figures into Dannan's Status surface (C4); takes the
   owner's `ReachabilityProvider` per domain (C3); starts and stops in
   order. `dagda::Context` for the nouns that exist (Domains, Objects,
   Claims, Leases, Tickets), with Dannan's callback shape (C1) and the
   version check, its Heads, Retained and node-local spaces added at
   step 4 before the version is frozen. The "0.18" wording in
   `store/state_layout.cpp` reworded.
7b. ~~**Maintenance in the node, the per-domain wait, the instant delete**~~
   Done 2026-11-04 night on `feature/maintenance` (`COMPLETED.md`;
   `dagda_tests` 388 and 1 disabled). The collector and the
   maintenance loop (GC and repair passes on the node's timer) move into
   `dagda::Node`, the owner supplying only its `ReachabilityProvider`.
   The collector's grace (30 days, Macha's) becomes a per-domain setting.
   An instant delete in the Context: `remove(ids, observed clock,
   WriteScope)`, scope-checked like a write; each replica releases only
   the claims the caller's observed clock dominates, and removes the
   object at once when no claim remains and the current inventory does
   not refer to it; otherwise it stays, and the evidence names, node by
   node, what was removed and what claim kept the rest. Readiness: done
   on Dannan 0.4.1, the node declares its own planes.
7. **`dagda::Context` v1.** Domains, Objects, Claims, Heads, Leases,
   Tickets, Retained, node-local spaces, written as a spec; versioned,
   async, a mismatch refused. Grown through steps 1 to 6; this step
   closes it. Tag `0.1.0`. Lugh and Lir can port from here.
8. **Macha onto Dagda** (requirements section 6: the store under
   Macha's names, then 8a to 8d), **only when the maintainer says**; nothing in
   `../macha` changes before then.

Settled with it (the maintainer, 2026-11-04):
- **Tests move with the code**, converted to GoogleTest; each step's gate
  is the moved tests passing in `dagda_tests`. Macha keeps its own copies
  until the port.
- **Fixtures come from the Macha session** (a Macha state directory,
  `macha-metadata-dump` output): it produces them
  in its own way and tells Dagda where they are. The Dagda session never
  builds or runs Macha. **Order (the maintainer, to the Macha session, 2026-11-04):
  Macha's tree-native branch merges first, then the fixtures are cut
  from the merged code**, genesis as a constant hash included, so none
  needs regenerating. Generator under `build/macha-fixtures/gen/`;
  output per step under `build/macha-fixtures/step<n>-*/` with a README
  naming the Macha commit and the config the node ran with (packing
  threshold, backend limits). Step 1's code can move before its fixture
  exists; its gate cannot close until it does.

### The port respec (from the Macha session's audit, 2026-11-04 night)

The maintainer's instruction, relayed by the Macha session: Macha is the source of
truth for the port; where Dagda differs from Macha's behaviour, Dagda
changes. The evidence is
`../macha/TODO/2026-11-04-dannan-dagda-port-evaluation.md` (sections 4,
5.1, 5.3). Each item below was checked against the code here and in Macha.

Order: M2, M3, M4, M8, M9, M6, M5, M10; M1 when Macha merges.

Done so far: M3, M5, M6, M7, M8, M9, M4 but for the barrier's deadline (on Macha's list for
The maintainer), and M2 but for the real state directory, which the Macha
session cuts after its merge (`COMPLETED.md`). Not measured at
scale: each loose object still takes one fsync, about 1 million on live-a. The Macha session accepted the
yield-rule dispute and M2's narrowing, and M9's vectors are at
`../macha/build/macha-fixtures/placement/placement-vectors.json`. With heads
also come head repair (`maintenance.cpp:1137`, paced, runs when busy) and
history truncation (`:1146`, gated on `!busy`).

- **M1. Heads** (step 4), with the `metadata` plane, the cache seed,
  `ordered` registration, lease records in a head with renew and release,
  the reference-count diff seam, commit and verify cost baselines. Waits on
  Macha's tree-native merge.
- **M2. `dagda-migrate`.** Confirmed: `is_trie_dir` (`migrate.cpp:220`)
  matches `<state>/metadata/`, which holds `journal.log`
  (`../macha/src/metadata/metadata.cpp:2336`), and the refusal comes after
  the objects (`:533` before `:549`). To do:
  - ledgers walked by name only, never by what a directory holds;
  - every refusal before the first write: ledgers probed first, an
    unmarked legacy checkpoint refused (the rule of `retention.cpp:304`;
    with the `ledger` marker they are removed at open, so only unmarked
    ones refuse), the node id checked;
  - the node id: a marker keeps Macha's id (`:310-312`) and the pool
    refuses another's (`storage_pool.cpp:289`), so the tool compares it
    with `--node-state`'s and refuses before writing; stated in
    `docs/migration.md`;
  - the cost: a record opened once, under v18, and the v19 tag derived
    from the old tag with no second pass (see below); progress and rate;
  - a test over a real Macha state directory once the Macha session cuts
    one, and a synthetic `metadata/` in the fixture now;
  - the reverse, v19 to v18 (Waiting on the maintainer).
  The tag: GHASH is linear and the ciphertext's terms depend only on their
  distance from the end, so two AADs over one ciphertext differ only in
  their AAD and length blocks. new tag = old tag xor delta, where delta
  needs H, the AADs and the ciphertext's block count: no ciphertext.
- **M3. Maintenance parity.** `Node` adds DATA pack compaction, local
  rebalance, scrub, ledger verify and `compact_if_needed(4096)`, on
  Macha's schedule; the pool's pieces exist (`storage_pool.h:230-253`).
  The yield rule stays as it is, because it is Macha's: disputed, below.
- **M4. Write semantics.** `put`, `put_deferred` and `put_here` take
  `persisted` when the caller names none; `Context` keeps naming the level.
  `put_deferred_here` added. A deadline on `StoragePool::durability_barrier`.
- **M5. Settings reload through the node**, and
  `test_replication_policy_change_on_restart` moved. Done, to what Macha does:
  its live reload takes backends and cache (`local_state.cpp` `reconfigure`) and
  refuses `dht.replicas` (`service_config.cpp:23-25`), so those take a restart.
- **M6. No change in Dagda.** The file Macha keeps is its path table, which is
  Macha's (`availability_service.cpp:252-269`, `availability.h:53`), and Macha's
  topology event is place and membership, no advert (`cluster.cpp:222-224`, `:890`).
  The Macha session accepted this and M5's narrowing, and confirmed
  `membership_changed` (`cluster.cpp:867-871`) counts no capacity or used. It does
  count `metadata_write_replicas_required`, an advert here, which Dannan's
  membership event leaves out: not a re-ask here until heads make it matter.
- **M7. Torrent-request known answers.** Settled by the maintainer: they stay in Macha's
  tests; Dagda pins the template on its neutral record.
- **M8. The claims guard bounded.** Confirmed
  (`storage_server.cpp:316-324`): stripes released between sub-batches,
  and `test_retain_data_batches_a_large_publication_within_bounded_time`
  moved.
- **M9. An R>1 placement known answer** against Macha's
  `capacity_placement_nodes` with failure domains.
- **M10. Scale and platform.** A trie of 1 million ids, 100,000 objects
  across packs; a Linux run with `syncfs`; sanitizers before `0.1.0`.

**Disputed with the Macha session:** "a pass is not skipped while loader
work is present" is not Macha's behaviour. `busy` includes the loader
(`../macha/src/service/maintenance.cpp:417-425`, law 3), and GC,
rebalance, retention compaction, pack compaction and scrub are all gated
on `!busy` (`:871`, `:1099`, `:1127`, `:1156`, `:1161`, `:1240`). Only
repair and the ledger verify are paced instead (`:430-431`, `:1209`).
`Node::higher_class_active` (`node.cpp:355`) is that rule. The audit's line
citations for scrub and verify are also swapped (scrub is `:1165`, verify
`:1214`).

### What step 4 waits on (the Macha session, 2026-11-04)

Macha's `feature/tree-native-namespace` is green at `b080c38`, no merge
date. Its remaining steps, in order: genesis as a constant hash; the
legacy head authority removed (cas, seed, install_committed_delta,
floor 0); the inline snapshot and delta formats removed with their wire
slots kept; a gate run on live-a, then merge and deploy to both nodes.
The operator may put the LocalStore race fixes first. Genesis and the
final tree forms are settled only at that merge, so Dagda's heads move
from the merged code; the Macha session will say exactly what changed.
Steps 5 and the parts of 6 that need no heads go first.


## Handoff, 2026-11-16 (small hours): hand-offs, Macha's log lines, measured; local reads; two retirement cases

Branch `feature/macha-store-gaps`, nothing pushed. Gates on the Pis, Dannan 0.8.0 prefix, GCC Release, warnings as
errors, Macha's fixtures: `3df3b8d` 1109 ran / 1106 passed / 3 skipped (node-c); `6adc5ed` 1112 / 1109 / 3 (node-a);
`156fea2` 1113 / 1109 / 3 / 1 failed (node-c: the silent-peer read race, below); **`1bfde30` (the tip's code)
1113 ran / 1110 passed / 3 skipped / 0 failed** (node-c). 0 warnings in each. Seen failing
first at their own commits: `a019e2f` (the synchronous wait, 15 s against 5), `b54784a` (three log-line cases).

- **The synchronous waits** run their barriers on the caller's thread, told under a condition, with no timer,
  worker job or executor post a ticket, as Macha's durability_barrier (`616df37`); the barrier round reads the roster
  only for a peer place. **One ranking a write** when every node is live (`3df3b8d`).
- **DS gap 14** (`6adc5ed`): the barrier round writes Macha's re-derived line (one a round, local copies and every
  peer probed counted) and its unmet line, before telling its settle-waits; the quorum line's result= as Macha's.
- **Measured on node-b** (harness plan): small files Macha 0.698 s, the advert 0.745 s (was 0.775), the tip 0.738 s,
  about 6% over Macha's CPU (was 11%), all else equal or better. **Local reads** (phase 2's first workload,
  `461bed3` then `ce37fe5` after the Macha session's review of its FUSE reader): one reader Dagda better on every
  figure (241 against 230 MB/s); four readers no steady gap after an off-CPU profile and a second set, Dagda bimodal.
- **The silent-peer read race** (`1bfde30`): two StoreRepair cases moved the stepped clock before the client had
  recorded its request's progress (Dannan's session.cpp:337-350); now they wait for the node's sent bytes to rise.
  20 repeats each: 38 of 40 passed before, 40 of 40 after.
- **For Macha's retirements** (`156fea2`): `StoreLocalStore.AStoreTellsABadRecordFromAFailedRead` (new, ported) and
  the two-deletes block of `StoreClaimLocks.ADeleteMarksItsObjectAndHoldsNoStripe` (both orders now).
- Rules recorded: TSan only to hunt a named bug (every build Pi runs it since 2026-11-16 00:18Z; never node-d; the Mac
  only when hunting); node-b was on 4K pages throughout, so the harness figures stand across that change.

Next is ACTIVE.md's "Next": remeasure on Dannan 0.9.0 when tagged, harness phase 2's other workloads.

Later the same morning (gate `ab76b43`, node-c on its 16K kernel: **1117 ran / 1114 passed / 3 skipped / 0 failed**,
0 warnings):
- **Recovery measured** (harness phase 2, workload 8, designed with the Macha session; `e454ad7`, `1771670`; the
  fixture's `Cluster` over a kept directory): at parity within 2%, after kill -9 and after a clean stop; the time to
  ready is mostly a fixed cost common to both, recorded, not pursued (the real-state rehearsal is a gate, the maintainer 2026-11-14).
- **For Macha's retirements:** its three survey cases (`StoreRepair.AStopDuringTheDiffIsNotAFailedPeer`,
  `.AFailedDiffIsAskedAgainOnATimer`, `LedgerAvailability.WhatIsNotHeldPausesOverTheIdsBesideTheView`), its pull-list
  pair (`.TheStepThatFirstSeesANewListWalksIt` with pull_examined, `.APullListNewWhileThePushRunsIsNotPassedOver` in
  Macha's shape), the bad record by the scrub (`StoreStoragePool.AReadErrorKeepsTheObject`).
- **Fixed, failing first:** the survey's due() named a passed retry after a second failure (`ab76b43`).
- **Found, waiting on the maintainer:** repair's pull starved by a sending push, in both stores (ACTIVE.md).

## Handoff, 2026-11-16 (evening): repair redesigned, stages 1 and 2; the two-Pi benchmark built

Branch `feature/macha-store-gaps`, nothing pushed. The maintainer ruled that repair is redesigned, not patched
(`TODO/2026-11-16-repair-redesign.md`: his words, the design, the stages, what each check found); the offer is
allowed; the benchmark runs on two Pis over the WiFi between them as it is. Gates on node-c, Dannan 0.8.0 prefix,
GCC Release, warnings as errors, Macha's fixtures, kernel 6.18.62-v8-va48+, 0 warnings in each:
`09921be` 1120 ran / 1116 passed / 3 skipped / 1 failed (a new fixture case whose premise the run disproved,
removed in `66a0b76`); `0d048e0` 1120 / 1117 / 3 / 0; `502cf57` 1123 / 1120 / 3 / 0; **`2a8d24b` 1125 ran / 1122
passed / 3 skipped / 0 failed**. Every new case was seen failing on the code before it.

- **The plan rationalised** (`877585c`): ACTIVE.md brought up to date, what was done or settled archived below.
- **Stage 0's checks.** The credit path of the pull's starvation proven (`39fe114`). Who knows a reference: an
  owner may not yet, in Macha and in Lir (payloads over 64 KiB); never in Lugh. No multi-object fetch or put
  exists; `have_objects` is the offer's shape.
- **A gap against Macha found and fixed** (`0d048e0`): Dagda's own loop read a provider registered after the node
  started, or a new generation, only when the collector's rest ended (five minutes). Two cases.
- **The fixture's cluster spans processes** (`09921be`): a kept CA, a given address, port and seeds.
- **The benchmark** (harness workload 7): `tools/harness` has a repair node for both stores (Macha's a whole
  Service, as its session required and then accepted), driven by `tools/harness/pi-repair-run.sh` from the Mac. Built on
  node-b at `7d12ca8` and copied to node-c and node-d; smoke-tested on one Pi and across two.
- **The baseline measured** (node-b and node-d, 2.4 GHz WiFi at about 2.9 MB/s, 18:53 to 19:54Z; harness plan):
  Dagda's repair today at least as fast as Macha's in every scenario (large rejoin 78.4 s against 81.3 s, small
  25.8 s against 38.9 s); both stores send each object 1.5 to 2.05 times, the holder pushing while the joiner
  pulls; on small objects the 16-call count sets the rate. One Macha run lost to the driver, fixed (`250ac68`).
- **The cluster, for the record:** I held node-b and node-d whole for a run bound by the link, and dropped the
  page cache under another session's build; corrected the same hour on the maintainer's word (claim what the job uses; no
  cache drop on a shared Pi). node-b and node-a then moved to 5 GHz, so tonight's times compare with nothing
  later.
- **Stage 1** (`502cf57`): the survey keeps, per peer, what it holds of what this node lacks.
- **Stage 2** (`2a8d24b`): repair from one queue, fewest known copies first; both starvation paths have passing
  cases; no ported case changed.

Asked of the maintainer at the end, interactively: `0.2.0` stays held; the slow-disk run is wanted, when a Pi is free; the
benchmark's controller is in the repository (`tools/harness/pi-repair-run.sh`); the instant delete on a node with
no inventory went to Lugh and Lir first, both answered, and the maintainer ruled that such a node keeps the copy (not built:
first in ACTIVE.md's "Next"). Lir's shared-payload point was closed in Lir: it never shares an object.

Next: the instant delete's fix, small; then stage 3 (the offer), designed and not built, then old against new measured in one session; stage 4 (the
window, the count gone), its shape to the Macha session first. Nothing running, no Pi claimed.

## Handoff, 2026-11-16 (night) to 2026-11-17: the instant delete's fix; repair stage 3, the offer, measured and its regression fixed; stage 4's shape agreed

Branch `feature/macha-store-gaps`, nothing pushed. Every new case was seen failing on the code before it.

- **The instant delete keeps the copy on a node with no inventory published** (the maintainer's ruling; gc-and-deletion gap
  9): `750cad3` the case, `5f3c458` the fix. The delete cases that expect a copy to go now publish an empty
  inventory first. Gate on node-a, Dannan 0.8.0 prefix, GCC Release, warnings as errors, Macha's fixtures,
  kernel 6.18.62-v8-va48+: **1126 ran / 1123 passed / 3 skipped / 0 failed, 0 warnings**.
- **Repair stage 3, the offer** (`deafb7d` the wire pair, `21013d6` four cases, `d2872de` the code; the redesign
  document, "What was built"): a holder offers before reading, sends only what is wanted; the receiver answers
  from its index and an `OfferBook`, and does not pull a promised copy. Gate on node-b, Dannan 0.8.2 prefix
  (0.8.0's ABI), otherwise as above, Macha's fixtures copied there: **1131 ran / 1128 passed / 3 skipped / 0
  failed, 0 warnings**. No ported case changed.
- **Measured old against new** (the controller runs a `dagda-old` beside the tip: `e369fd0`, `7d4b5a0`; the
  baseline's binaries kept in `build-old/` on node-b and node-d, the tip's in `build-tip/`): each copy once in
  every scenario, the unpaced rejoins a third to a half faster; **a rejoin under a viewer about three times
  slower**, in all three rounds. Mechanism suspected, not proven; a fix recommended; waiting on the maintainer.
- **The Macha session**: told both gated commits; it needs nothing for stage 3 (its pass calls `transfer`, and
  `dagda::Node` hands its server the book). **Stage 4's shape agreed** with it (the redesign document), with the
  wake on a drained window; the maintainer ruled, through it, that `offer_bytes` and `in_flight` join Macha's /status at
  the cutover.

**Then, the maintainer approving the recommended fix (2026-11-17, "Yes"):** `63e76a0` two cases (both seen failing on
`d2872de`, the first proving the mechanism), `0dfe22a` the fix (a holder offers only what its credit and batch
will send, sized from the index; a promised pull looked at each second), `d119d14` the size taken from the
payload, not the sealed record (Macha's `StoreRepair.ATransferWaitsForItsCredit` caught it). The controller runs
chosen scenarios (`e5cee19`). Gate on node-b, as above: **1133 ran / 1130 passed / 3 skipped / 0 failed, 0
warnings**. Remeasured old against new, three rounds: under a viewer 32.5-38.3 s against 34.4-37.9 s, at parity
with each copy moved once; unpaced 20.6-29.3 s against 36.2-52.5 s.

- **Dannan 0.9.0** (the Dannan session, cutting it on 2026-11-17): Dagda said what the tag must hold; its
  `storage::FileSystem` has the exclusive lock and the directory sync Dagda's raw syncs need. Recorded in ACTIVE.md.
- **The documents brought up to date** at the end: the redesign document reorganised (each stage's record in one
  place, how to measure with where the binaries are, where stage 4 starts), the harness plan pointed at the new
  figures, the README and the foundations map, ACTIVE.md with a "Start here".

Left ACTIVE.md's "Next", both done: the instant delete's fix (was item 1) and repair's stage 3 (was item 2).

Next: stage 4, a session of its own. Nothing running, no Pi claimed.

## Handoff, 2026-11-17 (evening): repair's stage 4; the port's state-layout defect

Branch `feature/macha-store-gaps`, nothing pushed. Every new case was seen failing on the code before it (node-c).

- **Repair stage 4** (the redesign document, "What stage 4 built"): `c12e36d` five cases (seen failing on
  `d119d14`), `ec77dda` the window and the call count gone, `b9aeed7` the fetch of many's wire pair with its case,
  `79f6f76` two cases (seen failing on `ec77dda`), `690dc9c` the fetch of many, `c0bd7a4` (`copies_pushed` as a copy
  lands, StoreRepair.ATicketReportsTheCopiesRepairMade), `6e1f4dd` (a drain's observations,
  StepsBed.APassRecordsMachasObservations), `5e5e7fc` the harness's `dagda-<name>` stores and figures. Gate at
  `6e1f4dd`, node-c, Dannan 0.8.0 prefix, GCC Release, warnings as errors, Macha's fixtures, kernel
  6.18.62-v8-va48+: **1141 ran / 1138 passed / 3 skipped / 0 failed, 0 warnings**. Cases changed, none removed, all
  named in the redesign document for the Macha session.
- **Measured against stage 3** (`build-s3/` on node-b and node-d): the small rejoin three times faster (13.1-13.4 s
  to 4.0-4.3 s), under a viewer faster (23.4-25.6 s to 14.9-17.4 s), **the unpaced large rejoin slower** (14.5-15.8 s
  to 16.6-19.5 s). One mechanism found and fixed (`6794938` the case, `82e5201`: a holder's pulls split over the
  fetches the credit and window cover); remeasured, the large rejoin did not move (18.4-25.5 s). **Open, ACTIVE.md's
  first item.**
- **The port's defect** (the Macha session, its first full suite on node-a: 173 of 177 failures): a store began
  only in an empty state directory, and Macha's is Dannan's too. `844c557` three cases, `8b544d6` the fix: an
  unmarked state is refused only when it holds what a store keeps there. Gate at `8b544d6`: **1144 / 1141 / 3 / 0**.
  The Macha session confirmed the list against its code and builds on `8b544d6`.
- **Last gate, `82e5201`**: node-c as above, **1145 ran / 1142 passed / 3 skipped / 0 failed, 0 warnings**.
- For the other sessions: told Dannan that Dagda adopts 0.9.0 after stage 4; answered its hybrid clock question
  (keep the durable ceiling without custody; the maintainer ruled so). Kvasir's sanitizer reminder: no sanitizer in any Dagda
  gate or plan.

Next: ACTIVE.md's "Next", item 1, the large rejoin. Nothing running, no Pi claimed.

**Then, the same evening, the maintainer asking to push and continue:** `feature/macha-store-gaps` pushed to origin (no tag).
The large rejoin traced (`9d078eb`, `TRACE=1`): the link's estimate collapsed under the window (0.69 MB/s on an 8
MB/s link), holding the joiner idle on credit. `6231c98` the case (failing at its commit), `79e055d` the fix (repair
samples the link from its window). Gate at `79e055d` on node-b (Dannan 0.8.2 prefix): **1146 / 1142 / 3 skipped /
1 failed**, `ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit`, outside repair, passing three times alone
straight after; in ACTIVE.md's Open. Remeasured: the large rejoin 16.7-20.9 s against stage 3's 14.1-17.1 (was
16.6-25.5); small and viewer as before. The remaining 3 s is ACTIVE.md's first item. Also: this session's clangd
stopped at the maintainer's word (no build of this session's ran on the Mac; the editor's background index did). Nothing
running, no Pi claimed.

**Later the same night (the maintainer: push, then continue):** pushed to `8e509cd`. A second trace (`6319a6b`) narrowed the
large rejoin's gap to its start: the joiner's first four pulls take 6.4 s where the link alone needs about 2.
node-d went down as the third traced round began (22:37Z), back at 22:54Z under-voltage; the maintainer: try another pair.
The pulls' stages are now timed (`7a8dd96`). Then a second port defect from the Macha session, first: a failed
plane was marked on readiness only when both planes had ended. `ac96a5a` the case (failing on the code before),
`97a5bed` the fix; gate on node-a: **1147 / 1144 / 3 skipped / 0 failed, 0 warnings**; sent to the Macha session.
Next: the timed trace on a free pair (node-b and node-c were full, node-d left out). Nothing running, no Pi claimed.

**Into 2026-11-18:** The maintainer: push (refused by the session's permission check; left to the maintainer), wait for node-b. The timed
trace on node-b and node-c (`6d1d0e7`): the large rejoin's first transfers run at about 5.5 MB/s for 13 s, stores
fast; the holder's side is next. A third report from the Macha session, a SIGSEGV in `open()`, proven to be its
fixture stopping the node during `open_store()`; Dagda guards it anyway (`591669a` failing first, `fbdd50a`). Gate on
node-c: **1148 / 1145 / 3 skipped / 0 failed**. Then the maintainer, relayed: the cluster is Miach's; everything released.
Local commits since the last push: `6319a6b` to the record of this. Nothing running, no Pi claimed.

## 2026-11-18 (afternoon): the Macha move only; Dannan 0.9.2 and four port fixes

The maintainer, to Macha, Dannan and Dagda: work only on Macha's move to Dagda until ready for the cutover on live-a and
live-b. The three sessions agreed the cutover deploys on **Dannan 0.9.2** (Dannan's lease outliving its ledger at
shutdown, and a new directory not synced into its parent, both fixed only in 0.9).
- **Dannan 0.9.2** (`7967737`, `eac71b6`, `11fbd69`): `DAGDA_DANNAN_MINIMUM` 0.9.2; the replica's default replacer a
  lambda (`durable_replace_file` gained a defaulted `FileSystem&`); the roster double's pushed `advertise`; the version
  case at minor 9; `debian/control` (it still said 0.7) and the build notes. Gate on node-b's 0.9.2 Release prefix:
  1148 ran, 1144 passed, 3 skipped, 1 failed (only the version case's pin, then fixed and passing).
- **ASan and UBSan** over it (node-b, GCC RelWithDebInfo, Macha's fixtures): **no sanitizer report**; 1148 ran, 1144
  passed, 3 skipped, 1 failed (`ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit`, below); 14 warnings, all
  `-Wmaybe-uninitialized` in libstdc++'s `std::regex`, as at 0.1.0.
- **A claim seeks write_copies holders again** (`f8a9f0d` the case, `8398ec4`): Macha's
  `test_a_file_touch_commits_while_a_holder_is_away`. `retain_data` placed a short object again with a default write,
  which since the acknowledgement standard answers at a majority, so the third holder was left to prompt replication
  and never claimed. It now puts the object on the ranked nodes that lack it until write_copies hold it, as Macha's put did.
- **`Node::local_due()`** (`9b8d2c0` the case, `e6418ce`), asked by the Macha session for its pass to park as main's
  did (maintenance.cpp:1306-1321, :1366): rebalance's and the scrub's credit reaching an extent, else the next
  campaign's date; none when only an event gives work.
- **A credit waiter turned away for pressure wakes when the refusal can end** (`c06e022` the cases, `053ee6e`), the
  intermittent `ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit` of 2026-11-17 and of the ASan run: a loader
  that stood aside for a recent foreground user on a pressured device looked again only at a release or its window
  (Macha's arbiter the same). It now looks again when the user's window closes (`foreground_presence_ends`), and the
  device monitor leaving pressure notifies (`on_release`); nothing polls in a node. Shape agreed with the Macha session.
- Answered for the Macha session: group 4 (a backend taken away) is `NodeOptions::server.refresh_interval` set to
  Macha's heartbeat, as Macha's server refreshed every heartbeat; the claims fault's Dagda half is
  `HeadsExchange.ACommitIsClaimedHereWhenThePeerNeverAnswersTheClaim`, its seam `NodeOptions::front`; `dagda-migrate`'s
  command line and what it expects of a stopped node (`docs/migration.md`).
- `06b3960`: `StorageServerOptions::serve_timing`, the holder's side of a served read timed, for the large rejoin.
- Found, not fixed: directories Dagda makes before a durable write are not synced into their parents (ACTIVE.md, 0b).

Each case failed at its commit before its fix (node-b). **Gate at `053ee6e`**, node-b, Dannan 0.9.2 Release prefix,
GCC 14 Release, warnings as errors, Macha's fixtures, kernel 6.18.62-v8-va48+: **1152 ran, 1149 passed, 3 skipped, 0
failed, 0 warnings** (`throttled=0x50000`: past under-voltage). The run at `e6418ce`, before the arbiter fix, failed the
served-requests case again. Sent to the Macha session. Nothing running, no Pi claimed.

## Archived from ACTIVE.md, 2026-11-16

Moved out when the plan was rationalised (the maintainer, 2026-11-16). The repair item was answered by the redesign
(`TODO/2026-11-16-repair-redesign.md`); the rest was done or settled earlier.

### "Next (in order, the maintainer's rulings of 2026-11-15)", as it stood

1. ~~Measure the figures advert~~ and 2. ~~fewer hand-offs and less placement a write~~: **done and measured**
   (2026-11-15 night, the harness plan's last section). Small files: Macha 0.698 s, the advert 0.745 s (was 0.775),
   with the synchronous wait (`616df37`) and one ranking (`3df3b8d`) 0.738 s: **about 6% over Macha's CPU, was 11%**;
   wall time, latency, bytes and memory equal or better. Left alone as like for like: the peer re-advertising on
   every put (Macha's server does the same), the events counter, prompt replication's probe-then-put.
2b. ~~Four concurrent local readers about 6% slower~~: profiled and remeasured the same night, no steady gap (harness
   plan, "Phase 2: reads, local", finding 2); Dagda's four-reader runs are bimodal (about 344 or 363-407 MB/s), in Open.
3. **Remeasure on Dannan 0.9.0** when it is tagged (the net-layer copies go).
4. **Harness phase 2**: reads-local written and reviewed by the Macha session (`ce37fe5`: its FUSE reader, one
   synchronous loader read an extent, no read-ahead, 1 MiB requests; compares the two stores, not baseline.py's
   numbers, being cold and at offset 0 of fresh files); then reads over a shaped link (a note on the claim board first), CONTROL graphs, deletes
   and GC, repair, recovery (kill -9), disk pressure (dm-delay), cache; strace fsync counts on a separate rerun.
5. **Route Dagda's raw syncs through Dannan's file-system interface** once 0.9.0 has it: `ledger/sealed_journal.cpp`,
   the durability domain and storage pool barriers, `heads/replica.cpp`, `heads/node_pack.cpp`,
   `store/local_store.cpp`, `migrate/migrate.cpp`, `migrate/manifest.cpp` (and its flock); `LocalStoreFiles` becomes an
   adapter over it.
6. Cosmetic gaps left from the store map: the pool's gap 9 TimedLock (Dannan), 8c's loop items (gc-and-deletion gaps
   1-4 are the loop's 8c work; 5 is Dagda's own stamp decision, its line kept; 9, a null inventory letting an instant
   delete go, is a design question for the maintainer with Lugh and Lir). DS gap 14's log lines done (`6adc5ed`, tests `b54784a`;
   node-a 1112 / 1109 / 3).

Paused with the slices, kept for when the maintainer resumes them: `0.2.0`'s release steps (`COMPLETED.md`, "Archived from
ACTIVE.md, 2026-11-15"), the follower's lag (Lugh's P8.5 waits on it; slice 2's first step), slice 0's last two
figures, then slices 2 to 5 as the design orders them.

### "Waiting on the maintainer": repair's pull starved by a sending push

**Repair's pull starved by a sending push (found 2026-11-16, in both stores; put to the maintainer by the Macha session).** Push and
pull share one operation budget a step (Macha's `distributed_store.cpp:2530-2538`; Dagda's `repair_step`'s
`reserve_operation`); only the scan budget is the pull's own. A push that sends every step holds a new pull list
until its pass ends, though both stores' comment says it must not. Live on the 0.94.2 nodes. The Macha session's
recommendation: Dagda fixes it test-first (a share of the operations kept for the pull while its list is new, at
least one), Macha records it as a known defect until the cutover. **Held until the maintainer answers.** Dagda's port of
Macha's case is in Macha's shape (a push that sends nothing, `467a175`).
The maintainer objected to "an arbitrary 16 slots and bullying" (2026-11-16, to the Macha session). The Macha session's
alternative, a design argument and not measured: drop the operation count, so a step ends on spent credit, an
elapsed slice or a yield. Push and pull alternate, charged in bytes, with probes charged in time, and either takes
the whole step when the other is idle. Checked here, for that: Dannan's `DutyShare::should_yield`
(`../dannan/dannan/sched/duty_share.cpp:61`) returns false and clears whenever nothing contends. So on an idle
node the share bounds no step. The byte credit bounds only operations that move bytes. Today, probes and diffs on
an idle node are bounded only by the operation count and the scan budget derived from it (`repair.cpp:184`).
Without the count, the step needs a time bound of its own that holds when uncontended.

### "Waiting on the maintainer": settled earlier

Earlier: the seven items of 2026-11-04 were all answered that evening
(see Decisions, "the maintainer, 2026-11-04 evening") and done the same evening
(`COMPLETED.md`, "the decided fixes"): the control store's 256 MiB
reserve, the refused missing marker, the fresh future tombstone and the
refused unkeyed delta record. The device reserve was already
`IoReserveOptions` with those defaults; an operator reaches it through the
node's options at step 7. For Macha at the port (8b): the tombstone and
the unkeyed-record fixes, the same.

Decided by the Dagda session under the rules (step 5): a lease's
`scope` is the head space its record lives in (plan 163); three
concurrent duplicate adds keep Macha's answer for byte identity (replicas
settle at the next merge), and new owners' duplicate rules must not
depend on which record kept the key.

Settled earlier: the plan (approved 2026-11-04), "Dagda - The Store" (now "Dagda - The Distributed Object Store",
The maintainer, 2026-11-15),
Dagda's own version number from `0.1.0` pinning a Dannan tag, Dannan
0.2.0, free message registration from 512, byte-for-byte compatibility
(superseded 2026-11-04 night: one layout, migrated in place).
Decided by the Dagda session under the rules: the store's layout marker
is Dagda's; the gates' and observations' "catalogue" wording stays as
Macha's until Macha no longer reads it (its Status must stay
byte-identical through the port).

## Handoff, 2026-11-15 (night): the side-by-side harness, Macha's gaps, the acknowledgement standard

Branch `feature/macha-store-gaps`, nothing pushed. Gates on the Pis, Dannan 0.8.0 prefix, GCC Release, warnings as
errors, Macha's fixtures: `07ad6d2` 1106 ran / 1103 passed / 3 skipped (node-a); `5502327` 1106 / 1103 / 3
(node-a); `6ea9d04` 1107 / 1104 / 3 (node-a); `62b4f41` 1108 / 1105 / 3 (node-c); `6bfb5b2` 1108 / 1105 / 3
(node-c). 0 failed and 0 warnings in each. Tests written first and seen failing at their own commits: `2a734c6`
(encode), `f82aede` (logical capacity, a compile failure), `4bbcc85` (replicated, 5 failures), `0178220` (figures).

The harness (`tools/harness/`, plan and every figure in `TODO/2026-11-15-side-by-side-harness.md`, raw lines in
`build/harness/results/2026-11-15/`):
- Phase 1's Dagda side was misconfigured and is superseded: test cadences (`33d9489`, the fixture's heartbeat,
  telemetry, status and refresh now options) and packing everything up to the pack target (`3250405`).
- Method: alternating order, fstrim and a 30 s rest, cold cache, get_throttled before and after each run; an idle
  workload; `settled` (time, CPU and bytes until both nodes hold every extent, `3e0d3b7`); per-extent latency;
  replicated-at-barrier (`d40ffd0`); perf confined to the measured span (`HARNESS_PERF_CTL`, `f595941`); each roster
  record's adverts printed at start (`4f4f223`). Analysers: `build/fold.py`, `who.py`, `selfdiff.py`, `offcpu.py`,
  `summarise.py`.
- The publication CPU gap was the encode: every write encoded its payload for a peer call even when only the local
  copy launched, into a buffer that doubled (`07ad6d2`). After it, publications and the viewer are at parity or better.
- Small files stay about 11% over Macha's CPU. Tried without a measurable gain: one roster reading per write
  (`5502327`, kept for consistency) and Dannan 0.8.2's placement. An advert-count experiment (4 against 10) gave about
  3%, so the figures advert (`a103c86`) was built; it is not yet measured.

Also on the branch: `Context::Objects::logical_capacity()` as Macha's statfs (`8323952`, `6ea9d04`); a write's set and
order from one snapshot (`5502327`); the acknowledgement standard (`2aef2d8` replicated a majority, `62b4f41` the
store's one-copy puts at persisted, `6bfb5b2` `ack_level`); the harness writes at persisted (`6580cd6`); the tagline
"Dagda - The Distributed Object Store" in every header (`c445348`).

Answered for Macha's port: R2's capacity (above); R3's seed (Dagda's CacheSeed after 8c; until then Macha's own
sealed file under its state path, deleted at 8c), its census (owner surface: `Node::reference_counts()` and
`heads::ReferenceCensus`) and reachability (owner surface). Reviewed for Dannan: placement copies (0.8.2), the hybrid
clock (a persisted ceiling; refusal is the clock's), agreement (replica-set identity; forget), the file-system
interface (errno classes, list with sizes, space, lock, sync_all). Advised Louhi, Mimir and Kvasir from the design.

Next is ACTIVE.md's "Next": measure the figures advert first.

## Archived from ACTIVE.md, 2026-11-15

The top of ACTIVE.md as it stood before this rewrite (where it stood, running, and next as of 2026-11-14), kept whole:

### (was) Dagda - Active Work

Last updated: 2026-11-14. `COMPLETED.md` is the record. The plan is the three-systems design,
`TODO/2026-11-13-three-systems-design.md`: its nouns, its slices, its measurements and the maintainer's rulings ("Ruled", 1 to 7).
The plan this file held until 2026-11-12 is at the end of `COMPLETED.md`, "Archived from ACTIVE.md, 2026-11-12".

## Where it stands

- **`0.1.5` is tagged and pushed** (main `c20f88c`): on Dannan 0.6, packaged for Debian, clean under GCC Release with
  warnings as errors. It is what Macha's port builds on through its flag day (stage S, then 8a and 8b; 8b is empty for
  Macha). The capability report (`TODO/2026-11-10-dagda-capabilities.md`) is brought to it.
- **Dagda serves Macha, Lugh and Lir as first-class** (the maintainer, 2026-11-13). The design is ruled: head spaces with
  placements (`capable`, `replicas`, `node_local`), a per-space k, change notices; the log as a noun; built-in register
  and counter merges; leases on head spaces; the advert aggregate; group commit for heads in slice 2; contract v1 frozen
  at `0.1.4`, v2 at slice 4, the catalogue as v3. Lir and Lugh reviewed it and agreed; their needs files are
  `../lir/TODO/2026-11-13-lir-needs.md` and `../lugh/TODO/2026-11-13-lugh-needs-of-the-stack.md`.
- **Acknowledgement levels are the stack's standard** (the maintainer, 2026-11-15, to this session; the standard is Kvasir's
  `../kvasir/TODO/2026-11-15-ack-levels-standard.md`): ephemeral (answered at once), persisted (this node's own disk
  has it), decided (a majority has agreed it; Dannan's agreement, 0.9.0, so not before), replicated (a majority of the
  replicas have it on disk); the setting is `ack_level` everywhere. Dagda changes to the standard's meanings:
  replicated waits for a majority of the replication set, not write_copies. Persisted, the maintainer's reading the same day
  (asked with Macha's edge and full-node case): "the disk that took it", one node's disk, this node's when it can
  hold the copy, else the next in placement order (as Dagda's persisted already was; `AFullPreferredNodeSpills
  DataButNotControl` stands). Dagda's durable (every available member) stays beside them until the maintainer says otherwise.
  Every Macha write moves to persisted (the Macha session, 2026-11-15): replicated at replication 2 would be both
  nodes, the WAN peer on every write.
- **Small-file CPU against Macha** (the maintainer, 2026-11-15): all three, in order. The advert aggregate (D11) now, agreed
  with Macha because its cache-advert names change; then fewer thread hand-offs and less placement per write; then
  remeasure on Dannan 0.9.0.
- **Agreement waits for Dannan** (the maintainer, 2026-11-15, relayed by the Dannan session): anything that can use consensus
  or agreement waits for Dannan's agreement primitive (0.9.0, after 0.8.3). Dagda builds no consensus, no lease
  standing in for it and no workaround meanwhile. Dagda's soft leases are a different guarantee and stand as they
  are; anything in Dagda wanting a linearizable decision waits. Dagda's part is the persisting acceptor store, on a
  node-local log once slice 4 builds it.
- **Louhi (compute, started by the maintainer 2026-11-15) is a fourth consumer** (its session's word, 2026-11-15). The maintainer first
  ruled that its scheduler waits for cluster-wide leases, then changed it the same day: Louhi places tasks with
  Dannan's placement over the roster, and uses a lease only to fence tasks that must not run twice, claiming no
  exactly-once from it (a lease is a soft claim; two holders can coexist for an absence window). What it needs of
  Dagda, in time: a ref it can pin (nothing is pushed while 0.2.0 is held), head spaces with watches, and claimed
  collections for allocating task addresses. Nothing is asked of Dagda now.
- **Slice 0 (the measurement) is done but for two figures that need later slices** (the channel cost per space,
  Lugh's leases as such). The bench is merged to develop (`d424572`, not pushed): `tools/dagda_bench.cpp` (the log
  prototype; builds standalone against Dannan 0.7 for the deferred reply, `build/bench070/CMakeLists.txt`) and
  `dagda/test/bench_heads_test.cpp` (`BenchHeads.*`, disabled). The figures, Mac and Pi, are in the design, "What
  slice 0 has measured". The verdict: the log passes the maintainer's PUBACK target on the Pi (p99 12.5 ms at a 10 ms tick, 23.5
  ms at 20 ms); heads fall short twice, a node's 85-95 commits a second (group commit) and a follower that falls
  behind in Lugh's shape on the Pi (seen 10-22 s later, growing).
- **Dannan**: 0.6.0 tagged (the ABI promise; SameMinorVersion; SONAME `libdannan.so.0.6`). **0.7.0 tagged and
  pushed** (2026-11-14, main `6ce0349`, CI green; SONAME `libdannan.so.0.7`, package `libdannan0.7`, .debs awaiting
  The maintainer's publish): the deferred reply slice 4 builds on (`Routes::bind_deferred`, an `Answer` given from any thread,
  bounded by `ServerLimits::max_deferred`), P2's partition view, Macha's fixes, the cancel and timing fixes. The bench's
  0.7 build (`build/bench070/`) was written against `feature/deferred-reply`; check its calls against the tagged names
  when slice 4 moves to 0.7. Dagda stays on 0.6 until then (0.1.5 is Macha's port base). Dannan's 0.8.0 has started
  (`feature/owner-test-seam`). CPU pacing moves into Dannan's class-aware pool (its P4, design
  `../dannan/TODO/2026-11-14-p4-worker-pool.md`, for 0.8.0; reviewed by Dagda 2026-11-14: tasks block only within
  their declared waits, with a worker reserve so blocked background tasks never take every worker; the rest measured
  in thread CPU time; the contended reading never blocks). Dagda deletes `store/cpu_pacing.h` at the slice that first
  links Dannan 0.8. Its maintenance loop stays on its own thread through Macha's port; after it, repair and scrub
  batches move onto the node pool, and slice 2 puts the head exchange's per-space loops there so spaces stop costing
  threads.

## The maintainer, 2026-11-14 (evening, to this session directly): Macha first

Macha's store cutover (step 1a, aliasing its store types to Dagda's) found five differing (ObjectTrie,
UnreferencedSince, DiskServiceMonitor and its timer, TrieUnion) and `Node::open_store`'s refusal without key custody.
The maintainer: "Port through dagda::Node and Context is correct, but you must make ABSOLUTELY SURE that Macha can perform exactly
as well as it did before with Dagda as its storage backend. It is not enough for the concepts and primitives to be
'sorta' like Macha." And: "there must be no 'shims' in Macha to make Dagda work for it. Both: parts of Macha may need
to be rewritten to use native Dagda effectively, and that's the NO LOSS of performance/reliability/etc - AND - Dagda
MUST be capable of handling DHT storage AT LEAST AS WELL as Macha." Then (asked in this session): the side-by-side
comparison runs on the Pis first (a harness driving Macha's store, built read-only from Macha's develop, and Dagda with
identical workloads), then on live-a and live-b against Macha's live baselines; the call-by-call map is written by
Dagda and the Macha session together, split by subsystem, each reviewing the other's. Key custody, talked through and
ruled: **Macha's nodes get Dannan key custody**, a one-time step on each node made part of the cutover beside
`dagda-migrate`; the node-local key stays only on its node, as designed. A node that loses its credential loses its
ledgers, not its objects, and rebuilds what it holds by rescanning its disks. The map's split (agreed with the Macha
session, against Macha's develop `f0597a4`): Macha writes the callers' side; Dagda writes the store's side (Macha's
DistributedStore, StoragePool and LocalStore, retention, claims and reference counts, availability and survey, repair and
maintenance's store stages, GC and the deletion claim path, cache, io pressure and data credit) onto `dagda::Node` and
`Context`; each reviews the other's against code. The maintainer: start now, and fix each gap test-first as it is found. The map:
`TODO/2026-11-14-macha-store-map.md`. Two requirements, both binding: Macha uses Dagda
natively with no adapter layer, rewritten where needed and losing nothing; and Dagda, measured on its own terms against
Macha's store on the same workloads, is at least as good a DHT store in every respect. Ruled the same evening: **all slice work is paused** until Macha runs on Dagda as well as before,
proven; **0.2.0 stays tagged locally and unpushed** until the maintainer says. The proof (proposed to the maintainer): a call-by-call map from
Macha's develop code to `dagda::Node` and `Context`, exact in semantics, with every gap fixed in Dagda; Macha's own
suites as the gate on Dagda; performance against Macha's live baselines at the same load; data through the in-place
migration on copies of real state; reliability under crash, restart and partition. Open for the maintainer: key custody on Macha's
nodes, or the node-local key from another source.

## Running

Nothing; node-b released 03:5xZ. The profile's data stay there (`/root/dagda-arm64/lag.perf.data`,
`lag-off.perf.data`, `offcpu-report.keep.txt`; the analyser is `build/offcpu.py` here). develop `5070e61` built clean
there at Release (the gate for `13fdda1`); the bench ran, the suite did not.

The maintainer ruled (2026-11-14, relayed by the Lugh session): Lugh's P8.5 does not land until a follower keeps up with Lugh's
rates on a Pi and a disable is seen on another node within about a second.

## Next

0. **The maintainer, 2026-11-14 (asked in this session):** slice 1 is released as `0.2.0`, tagged and pushed (main, develop and the
   tag); the no-rollback defence is both layers in Dannan (the design, node_local); the lag's fix is slice 2's first
   step.
   Dannan's half, 2026-11-14 (the Dannan session): the sealed counters are on Dannan's develop (`b53ac5e`) and ship in
   0.8.0, on the Context's Identity: `counter(label)`; `advance_counter(label, value)`, durable before it returns;
   `advance_counters({label: value, ...})`, one durable write, all or none; `note_counter(label, value)`, made durable
   by the roster loop within a second and at stop. A lower value throws `std::invalid_argument` naming the label; all
   four throw `std::logic_error` without custody. File `<state>/identity/counters` under HKDF(seal root,
   "dannan/counter/1"). The peers' witness is 0.9.0. When 0.8.0 is tagged (Dannan tells us), `NodeLocalGenerations`
   moves onto these counters; slices stay paused until the maintainer resumes them.
1. **The follower's lag** (Lugh's P8.5 waits on it, the maintainer 2026-11-14): measured and closed from inside (the design, "What
   slice 0 has measured (the Pi)", `feature/replicator-rounds`): the leader's replicator is saturated, about 31 ms a
   commit against 25 ms, three fifths in per-commit claim rounds to the peers, most of the rest in the entry-by-entry
   history push. Next: claims batched per round (one call a peer for every owed commit's claims), the push told what
   the peer holds, the accept as soon as a peer's history lands; then the same bench against Lugh's second. Slice 2's first step unless the maintainer pulls it ahead of slice 1 (asked, 2026-11-14). Lugh agrees
   with the order (per-round claims, a push told what the peer holds, an early accept) and waits on slice 2's first
   Pi figure against its bound of about a second: tell the Lugh session when there is one.
2. Slice 0's last two figures, when their slices exist: the channel cost per space (Lir wants it: its retained-topic
   index listens on the keys-changed notice), Lugh's leases as such (after slice 3). Tell Lir if the log's default
   tick is ever set past 40 ms (p99 is the tick plus about 2.5 ms).
3. **Slice 1** (the maintainer, 2026-11-14): built and proven on the Pi and merged to develop (`1065b18`, not pushed, not tagged;
   `COMPLETED.md` has the gate), and develop now builds on Dannan 0.7.0, the minor Lugh and Lir run, with Macha's
   fixtures passing there. To tag it: the Debian packages with lintian, the capability report and the release notes
   recording v1 frozen at `0.1.4`; the number is the maintainer's (recommended `0.2.0`: contract v2, new API, Dannan 0.7, while
   Macha stays on `0.1.5`). Lugh and Lir start on the tag. Prepared: `release/0.2.0` (local), develop plus the version
   commit (`CMakeLists.txt`, `debian/control`, the migrate tool's version case, `README.md` with its status,
   `docs/packaging.md`, `tools/build-deb.sh`). On the maintainer's word: merge it to `main`, tag `0.2.0` annotated with the message
   below, build the Debian packages from the tag on a Pi with Dannan 0.7.0's packages (installing them there replaces
   the Pi's Dannan 0.6 dev package: agree a Pi first), lintian, install and consume, update the capability report, push
   `main`, `develop` and the tag, and tell Lugh and Lir the number. The tag's message:
   "0.2.0: slice 1 on Dannan 0.7: node-local head spaces, the change notice, the installed cluster fixture; contract
   version 2 served beside version 1, frozen at 0.1.4". What it holds:
   - D13, the fixture installed (`90f2411`): seven fixture headers under `dagda/test/fixtures/`, `dagda::test_fixtures`,
     and `test/package/fixture_consumer.cpp`.
   - D4, the change notice (`3678a06`): `Space::watch`/`unwatch`/`notice_if_moved`/`changed`, the Context's
     `Heads::watch`/`unwatch`/`changed` as evidence; `heads_watch_test.cpp`. Contract version 2 served beside the
     frozen 1 (`context_version_frozen`); `Context.TheVersionCheckRefusesAMismatch` brought to it (the maintainer's ruling 3).
   - D3, node-local spaces (`f275ba0`, `1660d58`): `Policy::node_local()`; no exchange; the replica and a node pack
     (`heads/node_pack.h`) sealed under the node-local key; founded at open; upkeep in the maintenance turn; the
     no-rollback record (`NodeLocalGenerations`) pending the maintainer's answer on the counter (the design, "How slice 1 builds
     node_local"). A node-local space names no DATA object yet: its claims would have nothing to release them.
     `heads_node_pack_test.cpp`, `heads_node_local_test.cpp`.
4. Then slices 2 to 5 as the design orders them. A defect Macha's port finds still comes first.

How work runs now (the maintainer, 2026-11-14): **no builds, tests or benches on the Mac**, for any session; Linux gates are arm64
on the shared Pis (node-a 192.0.2.1, node-b .169, node-c .170), claimed on Lir's board first, first come first
served, one heavy job per Pi, -j2, nice 19. The x86-64 gate is suspended. A release's gate: the Pi (GCC Release with
warnings as errors and tests, the Debian packages with their tests and lintian, installed and consumed).
The maintainer, 2026-11-14, to every session: no repeated test runs without a named reason (a flaky test, or odd behaviour), and
ASan and TSan runs only for releases: "We're spending 50% of our time heating silicon for no good reason, and it stops
now." The profile of the follower's lag (Next, 1) is one bench run, not a rerun.


