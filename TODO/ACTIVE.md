# Dagda - Active Work

Last updated: 2026-11-15 (night). `COMPLETED.md` is the record (its last entry is this session's handoff). The plan
for slices is the three-systems design, `TODO/2026-11-13-three-systems-design.md`; the slices are paused (below).

## Where it stands

- **Macha first; slices paused; `0.2.0` held** (the maintainer, 2026-11-14, "Macha first", below). Work is on
  `feature/macha-store-gaps` (local, nothing pushed). `0.1.5` (main `c20f88c`, Dannan 0.6) is the last pushed release;
  `0.2.0` (slice 1: node-local spaces, the change notice, the installed fixture, contract v2) is tagged locally only.
- **The branch** builds on Dannan 0.8.x (`DAGDA_DANNAN_MINIMUM` 0.8.0; 0.8.1, 0.8.2 and 0.8.3 are tagged with 0.8.0's
  ABI, so no bump). Last full gate: `62b4f41` on node-c, Dannan 0.8.0 prefix, GCC Release, warnings as errors, Macha's
  fixtures: 1108 ran, 1105 passed, 3 skipped, 0 failed. The rename `6bfb5b2` is gated in `COMPLETED.md`'s handoff.
- **Macha's port** (the Macha session) builds against this branch: R2 (writes and barriers on tickets, all writes at
  persisted), R3 (metadata and control store) in progress, R4 to R6 after. Macha is given each gated commit.
- **The side-by-side harness** (`TODO/2026-11-15-side-by-side-harness.md`, `tools/harness/`): phase 1 done on
  node-b. Publications and the viewer are at parity or better; **small files cost about 11% more CPU** than
  Macha's store (0.775 s against 0.695 s for 200 files; equal wall time and bytes). Phases 2 and on (reads over a shaped
  link, CONTROL graphs, deletes, repair, recovery, disk pressure, cache) are not run.
- **Acknowledgement levels are the stack's standard** (the maintainer, 2026-11-15): ephemeral, persisted ("the disk that took
  it": this node's when it can, else the next in placement order), decided (Dannan's agreement, 0.9.0), replicated (a
  majority of the replicas on disk); the setting is `ack_level`. Done on the branch: `2aef2d8`, `62b4f41`, `6bfb5b2`;
  recorded in the stack document. Dagda's durable stays beside them until the maintainer says otherwise.
- **Agreement waits for Dannan** (the maintainer, 2026-11-15): Dagda builds no consensus or stand-in; its part is a persisting
  acceptor store on a node-local log (slice 4).
- **New consumers, nothing asked of Dagda now:** Louhi (compute: a pinnable ref, head spaces with watches, claimed
  collections), Mimir (Redis-compatible: the log, snapshots as objects), Kvasir (SQL: agreement as the truth, acceptor
  state on a node-local log, chunks as objects, a manifest head). Each was answered from the design.
- **Dannan 0.9.0** will carry what Dagda reviewed today: agreement (CASPaxos, replica-set identity, forget), the hybrid
  clock (a persisted ceiling; refusal is the clock's, not the data's), the file-system interface for the simulation
  harness (errno classes, list with sizes, space, lock, sync_all), and the net layer's per-message record copies.

## Next (in order, the maintainer's rulings of 2026-11-15)

1. ~~Measure the figures advert~~ and 2. ~~fewer hand-offs and less placement a write~~: **done and measured**
   (2026-11-15 night, the harness plan's last section). Small files: Macha 0.698 s, the advert 0.745 s (was 0.775),
   with the synchronous wait (`616df37`) and one ranking (`3df3b8d`) 0.738 s: **about 6% over Macha's CPU, was 11%**;
   wall time, latency, bytes and memory equal or better. Left alone as like for like: the peer re-advertising on
   every put (Macha's server does the same), the events counter, prompt replication's probe-then-put.
2b. **Four concurrent local readers are about 6% slower than Macha's at 7% less CPU** (harness plan, "Phase 2:
   reads, local"): an off-CPU profile of that span on both, the cause named, then the fix test-first.
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

How work runs: no builds, tests or benches on the Mac; Linux gates on the shared Pis (node-b .169 with NVMe for
measurement, node-c .170 and node-a .202 hold the Dannan 0.8.0 prefix and Macha's fixtures under `/root/dagda-080`),
claimed on the board first, -j2, nice 19. No reruns without a named reason; ASan only for releases (on the Pis). TSan
only to hunt one named bug, never as a gate or routine check (the maintainer, 2026-11-16): on node-b first, the one Pi
where it runs (39-bit VA; CC=clang-19 CXX=clang++-19, Makefiles), else on the Mac; g++ 14's TSan fails on the others for now (the maintainer: all the cluster Pis will run it soon).

## Open

From the report's "Open and unexplained" and "What I could not show without a running port", kept there in full:
- The heap aborts in the opt-in delete stress case (no mechanism found; no stress runs, the maintainer 2026-11-09). Macha is
  chasing a heap corruption of its own on the Pis (ASan); if it lands in Dagda's code, it comes first.
- The `mutex lock failed` abort of one 2026-11-05 macOS TSan run.
- `StoreRepair.AReadWithNoDeadlineMovesPastAPeerSilentForTheWindow` failed once (node-c, 2026-11-15, `616df37`, Lir's
  build beside it): the read did not move on within 30 s of the stepped clock's 301 s advance. Suspected the case's
  race (a call's progress noted after the advance restarts its window); passed in the next whole run. Not looked into.
- The pool's `get` stopping at the first backend's empty answer: Macha's, kept.
- The trie pins aged on the node's time source where Macha reads the real clock (the same on live nodes).
- The Pis' under-voltage under heavy builds: their supplies are the maintainer's to look at; journals are persistent now.
- The store's journal lines: what still differs from Macha's is in `2026-11-12-journal-mapping.md`, section 8.
- Everything only the port can show: Macha's merge hash for hash, delta replay through Macha's codec, end to end,
  performance against the baselines, a real state started, reliability over time.

## The goal

The maintainer, 2026-11-13: "It needs to support Macha, Lir, and Lugh and not as afterthoughts", and to the stack: Dannan and
Dagda "support Macha, Lugh and Lir as first class citizens. This was the purpose of splitting out the technologies."
Macha's port still loses nothing: the question for Macha stays the maintainer's of 2026-11-06, "does Macha move onto Dagda with
no change in data, functionality, reliability and performance?", answered Yes with nothing attached, proven on its
nodes; the Macha session reviews, builds the port, and pauses it for a defect Dagda must fix with tests.

## Decisions

The rulings of 2026-11-13 and 2026-11-14 are in the design's "Ruled" section; those below stand where it does not
supersede them.

**The maintainer, 2026-11-05, later, to the Macha session and relayed by it; where it differs from the list below,
this is the later word:**
- The real migration on a second copy, the there-and-back and the live-b rehearsal are dropped for now: "No drop
  that, I'll decide later." Nothing in the plan may wait on them.
- Old-code removal in Macha comes first, "right now, before anything else": found as a tree, `mutate()` and the
  migration tool out, genesis no longer decoded inline, inline encoders and decoders out, legacy authority out.
  Then 0.92.6 (the four exceptions, the `followed` table fix, predicates retired), then baselines, then Dagda's work
  on that, then an independent review by fresh agents, which is the gate. So Macha's code is not frozen at
  `cac5998` or `808fa0f`, and Dagda's baseline moves with each commit Macha sends.
- Dagda builds nothing on legacy forms: SM5 to SM13, SM15, SM18, `required == 0` and the inline map are going.
- Dannan's generic class admission is required before stage one.

**The maintainer, 2026-11-05, directly, in answer to the outstanding questions:**
- **The cutover is in stages, as many stops as needed.** "Honestly we don't need to be so worried about a Flag
  Day. We've control over the entire cluster, its availability isn't critical." First stage: the store, the
  reachability provider and the lease template, with Macha's loop still running. Later stage: heads, the metadata
  migration and Dagda's loop together. Two disk events are fine. Each stage moves on as soon as its tests pass;
  there is no calendar soak. This supersedes "one flag day".
- **The loop:** Macha's pass runs a Macha node until heads move at 8c, calling Dagda's stages as steps; then
  Dagda's. Dagda's own loop serves a Dagda-only node meanwhile, off through `automatic = false`.
- **`<state>/metadata/` is migrated on disk at 8c**, so everything meets the one layout then.
- **The cluster key may be copied to this machine for tests**, "but keep it out of the repo, obviously".
- **The rehearsal:** the Macha session, in place on live-b. Custody from a systemd credential. One state
  directory, Macha's `state_path`.
- **Macha 0.92.6, a freeze exception:** the three stop and delete findings, and the inventory walk's stop check.
- **Read errors, fixed in both now:** a read failing on I/O keeps the object; only a record that reads and fails
  to open is removed.
- **No storage online:** ready, but degraded, in both, so a node does not crash-loop on its startup timeout.
- **Predicates:** retired in Macha after the freeze, not moved.
- **Heads keeps Macha's commit behaviours for now**, written down under "Waiting on the maintainer".
- **Pushed** `develop` and `main`, and the real run on a second copy of live-a's state is to go ahead.

(the maintainer, 2026-11-05) live-a runs Dagda's Linux tests, under one scratch directory at lowest
priority. Relayed by the Macha session: replan from Macha's `00861dd`, as above.

(the maintainer, 2026-11-04 night, relayed by the Macha session) Macha's tests keep pinning
the torrent bytes and are the port gate; Dagda pins only the template on its
neutral record and holds no torrent code. The reverse migration is kept but gates
nothing: a node may be migrated before there-and-back is proven. A migration keeps
its per-record open, so rot is listed; skipping it was not ruled on. The port is
two cutovers: Macha on Dannan's transport with its own store, a soak, then the disk
migration. A Macha case whose subject becomes Dagda's may be retired against a named
`dagda_tests` case, mapped case by case; everything Macha-visible stays. Macha
0.92.3 has the defect fixes; the tree-native merge and the fixtures follow its two
open P0s.

(the maintainer, 2026-11-04 night, relayed by the Macha session) **A genuine Macha
defect is fixed in Macha first, and the libraries match Macha's fixed
behaviour.** Dagda's fixes to the five LocalStore races, the claim
check-then-delete windows and the repair list identity stand, and Macha
adopts them. No further deviation on the grounds of a Macha defect without
sending it to the Macha session first. Where a fix changes a format or an
answer, the maintainer rules on it in Macha and Dagda takes the result.

The maintainer's, to the Dagda session, 2026-11-03 night:

- **Dagda replaces Macha's DHT** by extracting how Macha's store works and
  generalising it. ~~Macha's needs come first; Lugh's and Lir's are
  generalisations.~~ Superseded 2026-11-13: the three are first-class.
- **Move the store code** from Macha into Dagda and generalise it in
  place, bearing in mind it runs on Dannan: Macha's transport, crypto,
  durable files, logging and time give way to Dannan's as it arrives.
- ~~**Formats stay compatible**, "because they work": layout v18 and the
  store encodings, byte for byte.~~ Superseded 2026-11-04 night (below:
  one layout, v18 migrated in place). Small values must be handled well.
- **Header tagline: "Dagda - The Distributed Object Store"** (the maintainer, 2026-11-15; it was "Dagda - The Store").
- **Every write carries a durability level** (superseded 2026-11-15 by the stack's acknowledgement
  standard, "Where it stands") chosen by the caller
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
- (the maintainer, 2026-11-04 evening, the seven open items) Merge the step
  branches to `develop` first, then repin to Dannan 0.3.0 with the
  stand-ins replaced, then step 6 without waiting on Macha. The control
  store's free-space reserve is configurable with a 256 MiB default. A
  missing retention marker over non-empty claims refuses to open and says
  why. The device reserve is configurable, defaults as they are (one per
  node, 8 slots, 2 for control) until measured. A tombstone stamped by a
  clock ahead counts as fresh; a delta carrying a record with no key is
  refused; both in Dagda now and in Macha at the port.
- (the maintainer, 2026-11-04 evening, step 6) **Key wrap**: a node certificate's
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
- (the maintainer, 2026-11-04 night) **"We don't need backward compatibility":
  drop v18 entirely.** One layout, v19, for every domain, Macha's
  included: the domain in every AAD, the node-local key over every node's
  own ledgers, the v18 readers out of the store (they live only in the
  migration tool) and their known-answer tests with them (the maintainer's
  instruction, not a regression: the count is recorded). **There MUST be
  a working migration tool that does not need 2x the storage**: Macha's
  disks are migrated in place, resumable after a crash, and the Macha
  session's real v18 state directory is its gate. The CONTROL offer check
  under the node principal stays. A member probes its key at activate
  before trusting a blind-adopted backend. **The migration is manual
  only** (the maintainer, the same night, on the Macha session's flag that nothing
  on a live node is repaired by hand): a node that finds a v18 disk
  refuses to start and names `dagda-migrate`; nothing migrates unbidden.
  The store encodings (packs,
  the tree forms, the Merkle tree) are free to change where a step has
  reason; Macha's bytes bind nothing any more.
- (the maintainer, 2026-11-04 night) **Claims and space.** Claims stay released by
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
- (2026-11-03, the maintainer) The store is Dagda: target `dagda`, namespace `dagda`.
- (2026-11-03, the maintainer) Head spaces carry a replica policy from the start:
  `replicas: N` at `open()`.
- (2026-11-03, the maintainer) Scope is looked up in the Lugh head at write time.
- (2026-11-03 night) Ordered execution is a property of a type's
  registration (`Registration.ordered`).
- (2026-11-03 night) Store figures travel as Dannan adverts (wire v1).
- (2026-11-03 night, the maintainer) Dagda is its own project, with its own agent.

## Waiting on the maintainer

Nothing open from Dagda's list (answered 2026-11-05, see Decisions). In Macha's list, and Dagda follows: the
remaining design defects there (the torrent merge, the fence on actions under a claim, the local barrier's
deadline, wall versus steady ages, the policy disagreement loop, O(state) per delta, wire-sized allocations,
reload ignoring `write_copies`, the I/O and control reserves).

**Written down so it is not lost (the maintainer, 2026-11-05):** two metadata behaviours heads keeps as Macha's for now,
to change later as a deliberate step, in Macha first: a mutation that changes nothing still commits its clock
(`metadata_manager.cpp:1919` unreachable); a policy commit carries no dot (`:1376`), so after truncation only a
merge can collapse it.

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
