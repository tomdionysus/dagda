# Dagda - Active Work

Last updated: 2026-11-05. Dagda's plan follows Macha's code as it lands (0.92.5 is live; old-code removal is under way there), not the 2026-11-02 design
and not the `5a06d3c` baseline (Tom, 2026-11-05). Where the design and Macha's code
differ, Macha's behaviour is the plan and Dagda is reshaped to it. `COMPLETED.md` is
the record.

**Resumed 2026-11-06.** Macha's old-code removal landed: 0.93.4 live on both nodes, tag on merge `9a08846`. That is the
baseline. Forms remaining: SM14, 16, 17, 19, DLT5 to DLT11, envelope, hash and tree forms, the legacy clock. Genesis is a
fixed constant record, new clusters only. Torrent stays in Macha through the port (Tom): it moves after all of the port work, shelved until he asks. The 23 requests, the SM19 torrent section and DLT9 stay live, "`macha-metadata-dump` identical" holds as first written, and Macha has one space. Dagda still holds no torrent code: Macha's bytes are opaque vectors at S7. Goldens are to be re-cut on 0.93.4.

## The goal (Tom, 2026-11-06, relayed by the Macha session)

"development will continue on them with macha code frozen until they can answer Yes unconditionally, the entire reason
for their existence is this."

What it means (Tom, relayed, in answer to "do ruled changes count"): "Dannan and Dagda are generalisations of macha's cluster
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

**How it ends (Tom, relayed, 2026-11-06):**
- This plan ends with a capabilities report to the Macha session, with the evidence for each, naming what the port needs from Dagda,
  and saying plainly what could not be shown without a running port. "you are the review", to the Macha session: its judgement of
  that report is the review, and the review by this session after a clear is gone.
- The Macha session builds the port, once satisfied, "which will be frozen during the port": Dagda frozen, `../macha` read only for
  Dagda. Macha stays at `9a08846` until the port starts.
- A defect the port finds: "The port will pause until the relevant agent has identified and fixed the fault, with testing. The port
  will then continue with the relevant codebase frozen again."
- Proof on the nodes: "We will convert macnessa and inverbeg store backends and install there to test, prove and iterate." The real
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
    `../macha/build/baselines/results/` (uncommitted). Committed at Macha `9020425`. Its caveats: inverbeg was still filling, and macnessa crashed four times from 17:18Z to 18:11Z with heap
    corruption, cause not found, so its figures may not be a healthy 0.93.4's. Tom (relayed): the corruption is left until after the port, so
    no sanitizer result comes from Macha before then. The comparison rule is not yet ruled. No baseline: direct store put and get on USB, claim journal latency alone, GC, scrub and compaction rates; the large
    publication test is only "over 120 s". The report says plainly that the cause is unknown, that the stacks show only where the damage surfaced,
    that the one store frame is `ObjectTrie` under `StoragePool::held_view`, and what Dagda did and did not check; none of that clears or
    implicates the moved code.
10b. Open, found 2026-11-07 and 08 (each named in the report until closed):
    - Dannan, asked of the Dannan session: bytes sent do not count as call progress (Macha `net.cpp:1575`, Dannan `session.cpp:98`), so a moving
      upload looks silent; a peer not yet in the roster gets 403 where Macha accepts; a use-after-free on an in-process restart (TSan,
      `session.cpp:107-116`). Macha's 30 s publication budget waits on the first.
    - Dagda is pinned at Dannan 0.5.0 (`build/deps/prefix-0.5.0`). Peer calls run under `call_no_progress`, 300 s by default: the agent's choice,
      past a peer's 120 s credit wait; Macha's is unbounded, Dagda's rule forbids that, so a silent peer on a live session is a named difference.
    - A full control store: macnessa's filled 4G on 2026-11-06 and refused every control write for about 40 hours, silently (status healthy,
      73,842 "control object unavailable" lines, a namespace write answered 400), freed by nothing until the limit was raised and Macha restarted.
      Dagda must behave and recover the same; the silent status and unbounded retry are Macha's defects for after the freeze. inverbeg stood at 94%.
    - W1 (a peer may sweep an old unreferenced object just as another node re-references it, before it hears of the commit): Macha's behaviour,
      kept, costs one replica until repair; the fix is Tom's, the Macha session recommends after the port. Nothing restores a lost claim record.
    - The delete stress case (opt-in): two Release-only heap aborts with no writer found, in runs with in-process restarts; a held-ledger entry
      for an object no disk lists; races in `Node::publish_status` against `ReferenceCounts` (Dagda only) and `HeldLedger::stats` (unused, as
      Macha's). Settling under sanitizers often stalled in the quiet window. None explains macnessa's crashes, nor clears the moved code.
11. The capabilities report.

**Macha's freeze exception (Tom, relayed by the Macha session, 2026-11-06).** The history tail cut S3 found is fixed in Macha first,
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

**Tom (relayed, 2026-11-06): Dannan's tag and push are approved.** Dannan sends the tag when its last small commit is gated, which unblocks
`feature/class-waits`.

**Asked of Tom (2026-11-06), all answered above but the baselines' comparison rule.**

## The earlier table, kept for its gates and details; the goal above decides the order

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
| P1 | Scale and platform (M10): done, Mac and macnessa (`COMPLETED.md`). Left: TSan on Linux, for the Boost report only, and the USB disk, which the rehearsal measures | Whole suite on Linux aarch64; the scale figures recorded | Nothing |
| P2 | The mapping's 27 gaps: write path and durability 8, repair and maintenance 2, arbiter 4, control fetch 3, placement and selection 4, old state 1, predicates 5 | The named Macha case, moved to `dagda_tests` | Tom, for predicates |
| P3 | The node for the first stage under Macha's loop: its stages callable one by one, B1, B3, B6, B7, B8, B10 of second-cutover C, about 28 `dagda.*` figures, config keys and defaults. Dagda's scheduling, parking and trace wait for the later stage | S.4's named cases and VALIDATION sections | Nothing |
| P4 | Match Macha as it lands: old-code removal, then 0.92.6 (the three findings, the inventory walk's stop, the `followed` table, the exact read-failure shape), then its baselines | Macha's cases, in the shape of its fix | The Macha session |
| P4b | `DataResourceArbiter` and `IoReserve` on Dannan's `ClassWaits`: built on branch `feature/class-waits` against Dannan `develop` `415c6c4`, 449 passing unchanged. Merges when a tag has it. Required before stage one | `test_io_pressure` as moved; `DataResourceStats` keeps its meanings | Dannan's tag |
| P5 | Heads S1: the cache seed's migration, moved to 8c with the metadata files, as Tom ruled | Heads plan 2.2 | P8 |
| P6 | Dropped for now (Tom: "I'll decide later"): the real run, there and back, the rehearsal. The dry run over macnessa's real state stands as done | Second-cutover "Migration plan", when he decides | Tom |
| P7 | First stage: S, 8a and 8b, one disk event | S.4, 8a.4, 8b.4; all 11 crash-matrix rows; the Status golden | P1 to P6, the first cutover |
| P8 | Later stage: heads S2 to S7, 8c with `dagda-migrate` moving `<state>/metadata/` and the seed, Dagda's loop with `test_maintenance_trace`, then 8d, against `808fa0f` | Heads plan section 3, re-read at `808fa0f`; 8c.4, 8d.4; `macha-metadata-dump` output identical | 95 gap-heads cases each get a session or a named retirement |
| P8b | Gone: the Macha session's judgement of the capabilities report is the review (Tom) | | |
| P9 | Lugh and Lir: L1 to L9, then `Context` v1 frozen, tag `0.1.0` | Stack document's requirements | P8 |

**Macha is frozen at `cac5998` until the libraries are ready (Tom, relayed by the Macha session,
2026-11-05: "Macha has a working, tested codebase, this shouldn't be modified until they're
ready.").** The libraries do the port's work; Macha's code does not change. Macha can still produce
artefacts and answer questions. So:
- **From the Macha session, read from `../macha/build/macha-fixtures/`:** the goldens are in
  `goldens/`, from two real processes: Status with a 161-path mask, 381 of 494 keys reached at most;
  the config dump is 46 fields by hand, not all of `Config`; `routes.txt` is read from source, not
  captured. Taken from macnessa's stop (Tom): `/root/macha-state-20261105T145340Z` on the node, 3.0 GB of state and a
  257 MB DATA sample, at 0.92.4 (its `ORIGIN.txt` says what was copied stopped and what after), with
  `macha-metadata-dump.txt` beside it. Next: a dry run on macnessa with the key staying there, `macha-metadata-dump` output, and there and back
  under Macha's own code. The source to build is at `/home/tom/dagda-linux/dagda-develop` there.
- **Dagda's own, as well:** `git archive cac5998` into `build/macha-cac5998/`, built and run there,
  never in Macha's tree. It lets Dagda generate as many small states as a test needs, and have
  Macha's own code open a disk after `--reverse`. This machine has no FFmpeg development files, so
  the export builds with `MACHA_BUILD_SERVER=OFF`.
- **Differences that stand for the freeze:** the survey's rethrow of the stop, the multiset of
  deletion marks and the `not_held` pause have no Macha fix to match yet. A later Macha fix or a
  ruling may rename or reshape them. The inventory walk's stop check will not appear in Macha
  during the freeze unless Tom says otherwise, so P4 has nothing further to wait for.
- **No tree-native merge under the freeze (the Macha session's finding, 2026-11-05, not a ruling):** heads
  is built against `cac5998` as it runs, and 0.92.5 when it lands. Both nodes run 0.92.4. Both live heads are tree-backed SM19; live deltas are DLT5.
  Genesis is `genesis_metadata()` (`metadata.cpp:2294`), an inline record built at runtime, not a
  constant, and `b080c38` does not change that. A new cluster at `cac5998` founds inline, so the
  scratch state must not gate a tree-era tool. An unchanged mutation commits, about 26 empty 61-byte
  deltas a minute live, and that is the requirement until Tom rules. The heads plan's `b080c38`
  citations and "settled at the merge" marks are to be re-read at `cac5998`.

**Seams with no owner (found 2026-11-06, reading both trees; sent to the Macha session for Tom):**
1. Custody and certificates: `dagda::Node` refuses a node without custody and `advertise_wrap_key` throws without a
   certificate, and Dannan's first cutover leaves both off on Macha's nodes. Nobody's plan provisions them. A blocker.
2. Message numbers: Dannan's first cutover registers Macha's store types at the numbers Dagda takes; nobody says who
   removes which at stage one.
3. Adverts: `macha/capacity` and `macha/used` against `dagda/capacity`, `dagda/used`, `dagda/extents`.
Tom ruled it does not: torrent moves after all of the port work, so the "dump identical" gate stands and Macha has one
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
old explanation. Sent to Macha and confirmed by its reading, going to Tom: the survey's per-peer
catch swallows the stop, and two deletes of one id share a mark. Dagda already has the rethrow
and a multiset, the shapes Macha will recommend; the exception's name and place may follow Tom.

**Open differences from Macha, kept until Tom rules there:** the device I/O reserve, the
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

## Dannan, as Dagda finds it (from the Dannan session, 2026-11-03 night)

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
  refused at registration (Tom, 2026-11-04).
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
  on Tom). For Dagda: the scope check is
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
  framing, under Dagda's numbers from 512 (Tom's format decision, as the
  Dannan session reads it too).
- Dannan's `dagda/` stub is gone (Dannan `b8483e2`, merged to its
  `develop`): Dannan defines no `dagda` target, and its gate is
  `dannan_tests` alone.

## The earlier plan (approved by Tom, 2026-11-04), now the record of steps 0 to 7

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
   - **For Tom, not blocking**: the control store's free-space reserve
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
   first instance, were removed with all ingest (Tom, 2026-11-04, see
   Decisions; `dagda_tests` 317 and 3 disabled). Gate met but for what
   waits:
   - Step 4: `LeaseRecords` (`lease/keeper.h`) over a head space holding a
     `LeaseTable`; the records living in a head.
   - For Tom (the findings in `COMPLETED.md`): the template's unsigned
     tombstone window and its delta accepting a record with no key
     (Waiting on Tom 5 and 6).
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
   Macha's names, then 8a to 8d), **only when Tom says**; nothing in
   `../macha` changes before then.

Settled with it (Tom, 2026-11-04):
- **Tests move with the code**, converted to GoogleTest; each step's gate
  is the moved tests passing in `dagda_tests`. Macha keeps its own copies
  until the port.
- **Fixtures come from the Macha session** (a Macha state directory,
  `macha-metadata-dump` output): it produces them
  in its own way and tells Dagda where they are. The Dagda session never
  builds or runs Macha. **Order (Tom, to the Macha session, 2026-11-04):
  Macha's tree-native branch merges first, then the fixtures are cut
  from the merged code**, genesis as a constant hash included, so none
  needs regenerating. Generator under `build/macha-fixtures/gen/`;
  output per step under `build/macha-fixtures/step<n>-*/` with a README
  naming the Macha commit and the config the node ran with (packing
  threshold, backend limits). Step 1's code can move before its fixture
  exists; its gate cannot close until it does.

## The port respec (from the Macha session's audit, 2026-11-04 night)

Tom's instruction, relayed by the Macha session: Macha is the source of
truth for the port; where Dagda differs from Macha's behaviour, Dagda
changes. The evidence is
`../macha/TODO/2026-11-04-dannan-dagda-port-evaluation.md` (sections 4,
5.1, 5.3). Each item below was checked against the code here and in Macha.

Order: M2, M3, M4, M8, M9, M6, M5, M10; M1 when Macha merges.

Done so far: M3, M5, M6, M7, M8, M9, M4 but for the barrier's deadline (on Macha's list for
Tom), and M2 but for the real state directory, which the Macha
session cuts after its merge (`COMPLETED.md`). Not measured at
scale: each loose object still takes one fsync, about 1 million on macnessa. The Macha session accepted the
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
  - the reverse, v19 to v18 (Waiting on Tom).
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
- **M7. Torrent-request known answers.** Settled by Tom: they stay in Macha's
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

## What step 4 waits on (the Macha session, 2026-11-04)

Macha's `feature/tree-native-namespace` is green at `b080c38`, no merge
date. Its remaining steps, in order: genesis as a constant hash; the
legacy head authority removed (cas, seed, install_committed_delta,
floor 0); the inline snapshot and delta formats removed with their wire
slots kept; a gate run on macnessa, then merge and deploy to both nodes.
The operator may put the LocalStore race fixes first. Genesis and the
final tree forms are settled only at that merge, so Dagda's heads move
from the merged code; the Macha session will say exactly what changed.
Steps 5 and the parts of 6 that need no heads go first.

## Hosted systems waiting on Dagda

- Lugh (`../lugh`, the identity/SSO server; Tom has told it to align with
  and use Dannan and Dagda). Its store today is one sealed log per table
  (14 tables), version-conditional puts, kept-forever tombstones, unique
  secondary indexes rebuilt at open, per-table DEKs under a node master
  key. Under Tom's rules (2026-11-03 night) Lugh uses Dagda for
  everything it persists, shared and node-local, and deletes its store
  and keyring: **Lugh's persistence waits on Dagda entirely.** Until
  then Lugh keeps its store and keyring frozen and deletes them the day
  Dagda carries its spaces (Tom, in the Lugh session: "Keep the store
  until Dagda"). Its
  requirements on Dagda (node-local spaces; binding to domain and object
  id; no rollback of a head space) are in the stack document. Lugh takes
  Dannan's callback shape, so Dagda's Context does too. Recommended to
  Tom: Dagda's first step is Lugh's slice, single-node first (stack
  document, "Waiting on Tom" 1).

## Decisions

**Tom, 2026-11-05, later, to the Macha session and relayed by it; where it differs from the list below,
this is the later word:**
- The real migration on a second copy, the there-and-back and the inverbeg rehearsal are dropped for now: "No drop
  that, I'll decide later." Nothing in the plan may wait on them.
- Old-code removal in Macha comes first, "right now, before anything else": found as a tree, `mutate()` and the
  migration tool out, genesis no longer decoded inline, inline encoders and decoders out, legacy authority out.
  Then 0.92.6 (the four exceptions, the `followed` table fix, predicates retired), then baselines, then Dagda's work
  on that, then an independent review by fresh agents, which is the gate. So Macha's code is not frozen at
  `cac5998` or `808fa0f`, and Dagda's baseline moves with each commit Macha sends.
- Dagda builds nothing on legacy forms: SM5 to SM13, SM15, SM18, `required == 0` and the inline map are going.
- Dannan's generic class admission is required before stage one.

**Tom, 2026-11-05, directly, in answer to the outstanding questions:**
- **The cutover is in stages, as many stops as needed.** "Honestly we don't need to be so worried about a Flag
  Day. We've control over the entire cluster, its availability isn't critical." First stage: the store, the
  reachability provider and the lease template, with Macha's loop still running. Later stage: heads, the metadata
  migration and Dagda's loop together. Two disk events are fine. Each stage moves on as soon as its tests pass;
  there is no calendar soak. This supersedes "one flag day".
- **The loop:** Macha's pass runs a Macha node until heads move at 8c, calling Dagda's stages as steps; then
  Dagda's. Dagda's own loop serves a Dagda-only node meanwhile, off through `automatic = false`.
- **`<state>/metadata/` is migrated on disk at 8c**, so everything meets the one layout then.
- **The cluster key may be copied to this machine for tests**, "but keep it out of the repo, obviously".
- **The rehearsal:** the Macha session, in place on inverbeg. Custody from a systemd credential. One state
  directory, Macha's `state_path`.
- **Macha 0.92.6, a freeze exception:** the three stop and delete findings, and the inventory walk's stop check.
- **Read errors, fixed in both now:** a read failing on I/O keeps the object; only a record that reads and fails
  to open is removed.
- **No storage online:** ready, but degraded, in both, so a node does not crash-loop on its startup timeout.
- **Predicates:** retired in Macha after the freeze, not moved.
- **Heads keeps Macha's commit behaviours for now**, written down under "Waiting on Tom".
- **Pushed** `develop` and `main`, and the real run on a second copy of macnessa's state is to go ahead.

(Tom, 2026-11-05) macnessa runs Dagda's Linux tests, under one scratch directory at lowest
priority. Relayed by the Macha session: replan from Macha's `00861dd`, as above.

(Tom, 2026-11-04 night, relayed by the Macha session) Macha's tests keep pinning
the torrent bytes and are the port gate; Dagda pins only the template on its
neutral record and holds no torrent code. The reverse migration is kept but gates
nothing: a node may be migrated before there-and-back is proven. A migration keeps
its per-record open, so rot is listed; skipping it was not ruled on. The port is
two cutovers: Macha on Dannan's transport with its own store, a soak, then the disk
migration. A Macha case whose subject becomes Dagda's may be retired against a named
`dagda_tests` case, mapped case by case; everything Macha-visible stays. Macha
0.92.3 has the defect fixes; the tree-native merge and the fixtures follow its two
open P0s.

(Tom, 2026-11-04 night, relayed by the Macha session) **A genuine Macha
defect is fixed in Macha first, and the libraries match Macha's fixed
behaviour.** Dagda's fixes to the five LocalStore races, the claim
check-then-delete windows and the repair list identity stand, and Macha
adopts them. No further deviation on the grounds of a Macha defect without
sending it to the Macha session first. Where a fix changes a format or an
answer, Tom rules on it in Macha and Dagda takes the result.

Tom's, to the Dagda session, 2026-11-03 night:

- **Dagda replaces Macha's DHT** by extracting how Macha's store works and
  generalising it. Macha's needs come first; Lugh's and Lir's are
  generalisations.
- **Move the store code** from Macha into Dagda and generalise it in
  place, bearing in mind it runs on Dannan: Macha's transport, crypto,
  durable files, logging and time give way to Dannan's as it arrives.
- ~~**Formats stay compatible**, "because they work": layout v18 and the
  store encodings, byte for byte.~~ Superseded 2026-11-04 night (below:
  one layout, v18 migrated in place). Small values must be handled well.
- **Header tagline: "Dagda - The Store".**
- **Every write carries a durability level** chosen by the caller
  (2026-11-03, refined 2026-11-04): Ephemeral (in memory, queued, best
  effort), Persisted (on disk on at least one node), Replicated (on disk
  on at least k nodes, k per space, default 2), Durable (on disk on all
  available nodes of the replication set; known-dead nodes are not
  waited on; never undone; evidence names who is pending). Every write
  takes it: objects, head commits, claims, node-local spaces.
- (2026-11-04) Merges get the common ancestor when held; lease records
  keep per-field-group clocks; repair is diff only; legacy checkpoint
  readers are dropped; the GC provider is `ReachabilityProvider`. Full
  text: requirements document, section 8, head.
- (2026-11-04) **Ingest is Macha's**: "Dagda doesn't need ingest, that
  will be handled by macha. Remove all code from dagda that handles
  ingest." Dagda holds no ingest, torrent or import code, in the library
  or its tests; `ContractIncludeDirection.TheLibraryNamesNoIngest`
  keeps the library clear. The general machinery Macha's ingest drives
  stays: the `loader` work class (Dannan's), deferred writes and their
  tickets, the DATA credit arbiter and the device reserve.
- (Tom, 2026-11-04 evening, the seven open items) Merge the step
  branches to `develop` first, then repin to Dannan 0.3.0 with the
  stand-ins replaced, then step 6 without waiting on Macha. The control
  store's free-space reserve is configurable with a 256 MiB default. A
  missing retention marker over non-empty claims refuses to open and says
  why. The device reserve is configurable, defaults as they are (one per
  node, 8 slots, 2 for control) until measured. A tombstone stamped by a
  clock ahead counts as fresh; a delta carrying a record with no key is
  refused; both in Dagda now and in Macha at the port.
- (Tom, 2026-11-04 evening, step 6) **Key wrap**: a node certificate's
  key is a signing key, so each node derives an X25519 wrap key from its
  custody (`derive_local("dagda/wrap/1")`), publishes the public half as
  a Dagda advert signed with its certificate key, and a domain key is
  sealed to each member under HKDF of the X25519 shared secret with
  AES-GCM, the domain and member bound in the AAD. **Step 6 now builds
  everything that needs no head**; the catalogue is a head space at step
  4. **New domains are layout v19**, a new version with its own readers
  and the domain in the AAD (v18 was to stay; superseded the same night,
  next bullet). **The scope check**: `WriteOptions` carry the principal
  (default this node) and the prefix (default the domain's name); the
  put message carries the principal; every storing replica checks
  through `Identity::verify` before storing and refuses with evidence.
- (Tom, 2026-11-04 night) **"We don't need backward compatibility":
  drop v18 entirely.** One layout, v19, for every domain, Macha's
  included: the domain in every AAD, the node-local key over every node's
  own ledgers, the v18 readers out of the store (they live only in the
  migration tool) and their known-answer tests with them (Tom's
  instruction, not a regression: the count is recorded). **There MUST be
  a working migration tool that does not need 2x the storage**: Macha's
  disks are migrated in place, resumable after a crash, and the Macha
  session's real v18 state directory is its gate. The CONTROL offer check
  under the node principal stays. A member probes its key at activate
  before trusting a blind-adopted backend. **The migration is manual
  only** (Tom, the same night, on the Macha session's flag that nothing
  on a live node is repaired by hand): a node that finds a v18 disk
  refuses to start and names `dagda-migrate`; nothing migrates unbidden.
  The store encodings (packs,
  the tree forms, the Merkle tree) are free to change where a step has
  reason; Macha's bytes bind nothing any more.
- (Tom, 2026-11-04 night) **Claims and space.** Claims stay released by
  the collector against the owner's head (safe across a partition: a
  release covers only writes the head has seen). Added: **an instant
  delete**, safe because a replica releases only the claims the caller's
  observed clock dominates and keeps any other; and **the collector's
  wait settable per domain**. **Maintenance moves into the node now**
  (the collector and the GC and repair loop), not left to each owner.
- **Do not modify `../macha` until told to.**

From `../dannan/TODO/ACTIVE.md`, binding Dagda, not reopened without
asking:

- (2026-11-02, design) Dagda is a separate library and a peer, one hosted
  system among the others; it links Dannan; a node may run Dannan alone.
- (2026-11-02, design) Head stays in the core: Dagda knows what divergence
  is and calls the owner's merge; it never merges itself.
- (2026-11-03, Tom) The store is Dagda: target `dagda`, namespace `dagda`.
- (2026-11-03, Tom) Head spaces carry a replica policy from the start:
  `replicas: N` at `open()`.
- (2026-11-03, Tom) Scope is looked up in the Lugh head at write time.
- (2026-11-03 night) Ordered execution is a property of a type's
  registration (`Registration.ordered`).
- (2026-11-03 night) Store figures travel as Dannan adverts (wire v1).
- (2026-11-03 night, Tom) Dagda is its own project, with its own agent.

## Waiting on Tom

Nothing open from Dagda's list (answered 2026-11-05, see Decisions). In Macha's list, and Dagda follows: the
remaining design defects there (the torrent merge, the fence on actions under a claim, the local barrier's
deadline, wall versus steady ages, the policy disagreement loop, O(state) per delta, wire-sized allocations,
reload ignoring `write_copies`, the I/O and control reserves).

**Written down so it is not lost (Tom, 2026-11-05):** two metadata behaviours heads keeps as Macha's for now,
to change later as a deliberate step, in Macha first: a mutation that changes nothing still commits its clock
(`metadata_manager.cpp:1919` unreachable); a policy commit carries no dot (`:1376`), so after truncation only a
merge can collapse it.

Earlier: the seven items of 2026-11-04 were all answered that evening
(see Decisions, "Tom, 2026-11-04 evening") and done the same evening
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

Settled earlier: the plan (approved 2026-11-04), "Dagda - The Store",
Dagda's own version number from `0.1.0` pinning a Dannan tag, Dannan
0.2.0, free message registration from 512, byte-for-byte compatibility
(superseded 2026-11-04 night: one layout, migrated in place).
Decided by the Dagda session under the rules: the store's layout marker
is Dagda's; the gates' and observations' "catalogue" wording stays as
Macha's until Macha no longer reads it (its Status must stay
byte-identical through the port).
