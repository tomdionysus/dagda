# Dagda - Active Work

Last updated: 2026-11-14. `COMPLETED.md` is the record. The plan is the three-systems design,
`TODO/2026-11-13-three-systems-design.md`: its nouns, its slices, its measurements and Tom's rulings ("Ruled", 1 to 7).
The plan this file held until 2026-11-12 is at the end of `COMPLETED.md`, "Archived from ACTIVE.md, 2026-11-12".

## Where it stands

- **`0.1.5` is tagged and pushed** (main `c20f88c`): on Dannan 0.6, packaged for Debian, clean under GCC Release with
  warnings as errors. It is what Macha's port builds on through its flag day (stage S, then 8a and 8b; 8b is empty for
  Macha). The capability report (`TODO/2026-11-10-dagda-capabilities.md`) is brought to it.
- **Dagda serves Macha, Lugh and Lir as first-class** (Tom, 2026-11-13). The design is ruled: head spaces with
  placements (`capable`, `replicas`, `node_local`), a per-space k, change notices; the log as a noun; built-in register
  and counter merges; leases on head spaces; the advert aggregate; group commit for heads in slice 2; contract v1 frozen
  at `0.1.4`, v2 at slice 4, the catalogue as v3. Lir and Lugh reviewed it and agreed; their needs files are
  `../lir/TODO/2026-11-13-lir-needs.md` and `../lugh/TODO/2026-11-13-lugh-needs-of-the-stack.md`.
- **Acknowledgement levels are the stack's standard** (Tom, 2026-11-15, to this session; the standard is Kvasir's
  `../kvasir/TODO/2026-11-15-ack-levels-standard.md`): ephemeral (answered at once), persisted (this node's own disk
  has it), decided (a majority has agreed it; Dannan's agreement, 0.9.0, so not before), replicated (a majority of the
  replicas have it on disk); the setting is `ack_level` everywhere. Dagda changes to the standard's meanings:
  replicated waits for a majority of the replication set, not write_copies. Persisted, Tom's reading the same day
  (asked with Macha's edge and full-node case): "the disk that took it", one node's disk, this node's when it can
  hold the copy, else the next in placement order (as Dagda's persisted already was; `AFullPreferredNodeSpills
  DataButNotControl` stands). Dagda's durable (every available member) stays beside them until Tom says otherwise.
  Every Macha write moves to persisted (the Macha session, 2026-11-15): replicated at replication 2 would be both
  nodes, the WAN peer on every write.
- **Small-file CPU against Macha** (Tom, 2026-11-15): all three, in order. The advert aggregate (D11) now, agreed
  with Macha because its cache-advert names change; then fewer thread hand-offs and less placement per write; then
  remeasure on Dannan 0.9.0.
- **Agreement waits for Dannan** (Tom, 2026-11-15, relayed by the Dannan session): anything that can use consensus
  or agreement waits for Dannan's agreement primitive (0.9.0, after 0.8.3). Dagda builds no consensus, no lease
  standing in for it and no workaround meanwhile. Dagda's soft leases are a different guarantee and stand as they
  are; anything in Dagda wanting a linearizable decision waits. Dagda's part is the persisting acceptor store, on a
  node-local log once slice 4 builds it.
- **Louhi (compute, started by Tom 2026-11-15) is a fourth consumer** (its session's word, 2026-11-15). Tom first
  ruled that its scheduler waits for cluster-wide leases, then changed it the same day: Louhi places tasks with
  Dannan's placement over the roster, and uses a lease only to fence tasks that must not run twice, claiming no
  exactly-once from it (a lease is a soft claim; two holders can coexist for an absence window). What it needs of
  Dagda, in time: a ref it can pin (nothing is pushed while 0.2.0 is held), head spaces with watches, and claimed
  collections for allocating task addresses. Nothing is asked of Dagda now.
- **Slice 0 (the measurement) is done but for two figures that need later slices** (the channel cost per space,
  Lugh's leases as such). The bench is merged to develop (`d424572`, not pushed): `tools/dagda_bench.cpp` (the log
  prototype; builds standalone against Dannan 0.7 for the deferred reply, `build/bench070/CMakeLists.txt`) and
  `dagda/test/bench_heads_test.cpp` (`BenchHeads.*`, disabled). The figures, Mac and Pi, are in the design, "What
  slice 0 has measured". The verdict: the log passes Tom's PUBACK target on the Pi (p99 12.5 ms at a 10 ms tick, 23.5
  ms at 20 ms); heads fall short twice, a node's 85-95 commits a second (group commit) and a follower that falls
  behind in Lugh's shape on the Pi (seen 10-22 s later, growing).
- **Dannan**: 0.6.0 tagged (the ABI promise; SameMinorVersion; SONAME `libdannan.so.0.6`). **0.7.0 tagged and
  pushed** (2026-11-14, main `6ce0349`, CI green; SONAME `libdannan.so.0.7`, package `libdannan0.7`, .debs awaiting
  Tom's publish): the deferred reply slice 4 builds on (`Routes::bind_deferred`, an `Answer` given from any thread,
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

## Tom, 2026-11-14 (evening, to this session directly): Macha first

Macha's store cutover (step 1a, aliasing its store types to Dagda's) found five differing (ObjectTrie,
UnreferencedSince, DiskServiceMonitor and its timer, TrieUnion) and `Node::open_store`'s refusal without key custody.
Tom: "Port through dagda::Node and Context is correct, but you must make ABSOLUTELY SURE that Macha can perform exactly
as well as it did before with Dagda as its storage backend. It is not enough for the concepts and primitives to be
'sorta' like Macha." And: "there must be no 'shims' in Macha to make Dagda work for it. Both: parts of Macha may need
to be rewritten to use native Dagda effectively, and that's the NO LOSS of performance/reliability/etc - AND - Dagda
MUST be capable of handling DHT storage AT LEAST AS WELL as Macha." Then (asked in this session): the side-by-side
comparison runs on the Pis first (a harness driving Macha's store, built read-only from Macha's develop, and Dagda with
identical workloads), then on macnessa and inverbeg against Macha's live baselines; the call-by-call map is written by
Dagda and the Macha session together, split by subsystem, each reviewing the other's. Key custody, talked through and
ruled: **Macha's nodes get Dannan key custody**, a one-time step on each node made part of the cutover beside
`dagda-migrate`; the node-local key stays only on its node, as designed. A node that loses its credential loses its
ledgers, not its objects, and rebuilds what it holds by rescanning its disks. The map's split (agreed with the Macha
session, against Macha's develop `f0597a4`): Macha writes the callers' side; Dagda writes the store's side (Macha's
DistributedStore, StoragePool and LocalStore, retention, claims and reference counts, availability and survey, repair and
maintenance's store stages, GC and the deletion claim path, cache, io pressure and data credit) onto `dagda::Node` and
`Context`; each reviews the other's against code. Tom: start now, and fix each gap test-first as it is found. The map:
`TODO/2026-11-14-macha-store-map.md`. Two requirements, both binding: Macha uses Dagda
natively with no adapter layer, rewritten where needed and losing nothing; and Dagda, measured on its own terms against
Macha's store on the same workloads, is at least as good a DHT store in every respect. Ruled the same evening: **all slice work is paused** until Macha runs on Dagda as well as before,
proven; **0.2.0 stays tagged locally and unpushed** until Tom says. The proof (proposed to Tom): a call-by-call map from
Macha's develop code to `dagda::Node` and `Context`, exact in semantics, with every gap fixed in Dagda; Macha's own
suites as the gate on Dagda; performance against Macha's live baselines at the same load; data through the in-place
migration on copies of real state; reliability under crash, restart and partition. Open for Tom: key custody on Macha's
nodes, or the node-local key from another source.

## Running

Nothing; jupiter released 03:5xZ. The profile's data stay there (`/root/dagda-arm64/lag.perf.data`,
`lag-off.perf.data`, `offcpu-report.keep.txt`; the analyser is `build/offcpu.py` here). develop `5070e61` built clean
there at Release (the gate for `13fdda1`); the bench ran, the suite did not.

Tom ruled (2026-11-14, relayed by the Lugh session): Lugh's P8.5 does not land until a follower keeps up with Lugh's
rates on a Pi and a disable is seen on another node within about a second.

## Next

0. **Tom, 2026-11-14 (asked in this session):** slice 1 is released as `0.2.0`, tagged and pushed (main, develop and the
   tag); the no-rollback defence is both layers in Dannan (the design, node_local); the lag's fix is slice 2's first
   step.
   Dannan's half, 2026-11-14 (the Dannan session): the sealed counters are on Dannan's develop (`b53ac5e`) and ship in
   0.8.0, on the Context's Identity: `counter(label)`; `advance_counter(label, value)`, durable before it returns;
   `advance_counters({label: value, ...})`, one durable write, all or none; `note_counter(label, value)`, made durable
   by the roster loop within a second and at stop. A lower value throws `std::invalid_argument` naming the label; all
   four throw `std::logic_error` without custody. File `<state>/identity/counters` under HKDF(seal root,
   "dannan/counter/1"). The peers' witness is 0.9.0. When 0.8.0 is tagged (Dannan tells us), `NodeLocalGenerations`
   moves onto these counters; slices stay paused until Tom resumes them.
1. **The follower's lag** (Lugh's P8.5 waits on it, Tom 2026-11-14): measured and closed from inside (the design, "What
   slice 0 has measured (the Pi)", `feature/replicator-rounds`): the leader's replicator is saturated, about 31 ms a
   commit against 25 ms, three fifths in per-commit claim rounds to the peers, most of the rest in the entry-by-entry
   history push. Next: claims batched per round (one call a peer for every owed commit's claims), the push told what
   the peer holds, the accept as soon as a peer's history lands; then the same bench against Lugh's second. Slice 2's first step unless Tom pulls it ahead of slice 1 (asked, 2026-11-14). Lugh agrees
   with the order (per-round claims, a push told what the peer holds, an early accept) and waits on slice 2's first
   Pi figure against its bound of about a second: tell the Lugh session when there is one.
2. Slice 0's last two figures, when their slices exist: the channel cost per space (Lir wants it: its retained-topic
   index listens on the keys-changed notice), Lugh's leases as such (after slice 3). Tell Lir if the log's default
   tick is ever set past 40 ms (p99 is the tick plus about 2.5 ms).
3. **Slice 1** (Tom, 2026-11-14): built and proven on the Pi and merged to develop (`1065b18`, not pushed, not tagged;
   `COMPLETED.md` has the gate), and develop now builds on Dannan 0.7.0, the minor Lugh and Lir run, with Macha's
   fixtures passing there. To tag it: the Debian packages with lintian, the capability report and the release notes
   recording v1 frozen at `0.1.4`; the number is Tom's (recommended `0.2.0`: contract v2, new API, Dannan 0.7, while
   Macha stays on `0.1.5`). Lugh and Lir start on the tag. Prepared: `release/0.2.0` (local), develop plus the version
   commit (`CMakeLists.txt`, `debian/control`, the migrate tool's version case, `README.md` with its status,
   `docs/packaging.md`, `tools/build-deb.sh`). On Tom's word: merge it to `main`, tag `0.2.0` annotated with the message
   below, build the Debian packages from the tag on a Pi with Dannan 0.7.0's packages (installing them there replaces
   the Pi's Dannan 0.6 dev package: agree a Pi first), lintian, install and consume, update the capability report, push
   `main`, `develop` and the tag, and tell Lugh and Lir the number. The tag's message:
   "0.2.0: slice 1 on Dannan 0.7: node-local head spaces, the change notice, the installed cluster fixture; contract
   version 2 served beside version 1, frozen at 0.1.4". What it holds:
   - D13, the fixture installed (`90f2411`): seven fixture headers under `dagda/test/fixtures/`, `dagda::test_fixtures`,
     and `test/package/fixture_consumer.cpp`.
   - D4, the change notice (`3678a06`): `Space::watch`/`unwatch`/`notice_if_moved`/`changed`, the Context's
     `Heads::watch`/`unwatch`/`changed` as evidence; `heads_watch_test.cpp`. Contract version 2 served beside the
     frozen 1 (`context_version_frozen`); `Context.TheVersionCheckRefusesAMismatch` brought to it (Tom's ruling 3).
   - D3, node-local spaces (`f275ba0`, `1660d58`): `Policy::node_local()`; no exchange; the replica and a node pack
     (`heads/node_pack.h`) sealed under the node-local key; founded at open; upkeep in the maintenance turn; the
     no-rollback record (`NodeLocalGenerations`) pending Tom's answer on the counter (the design, "How slice 1 builds
     node_local"). A node-local space names no DATA object yet: its claims would have nothing to release them.
     `heads_node_pack_test.cpp`, `heads_node_local_test.cpp`.
4. Then slices 2 to 5 as the design orders them. A defect Macha's port finds still comes first.

How work runs now (Tom, 2026-11-14): **no builds, tests or benches on the Mac**, for any session; Linux gates are arm64
on the shared Pis (neptune 10.44.1.202, jupiter .169, saturn .170), claimed on Lir's board first, first come first
served, one heavy job per Pi, -j2, nice 19. The x86-64 gate is suspended. A release's gate: the Pi (GCC Release with
warnings as errors and tests, the Debian packages with their tests and lintian, installed and consumed).
Tom, 2026-11-14, to every session: no repeated test runs without a named reason (a flaky test, or odd behaviour), and
ASan and TSan runs only for releases: "We're spending 50% of our time heating silicon for no good reason, and it stops
now." The profile of the follower's lag (Next, 1) is one bench run, not a rerun.

## Open

From the report's "Open and unexplained" and "What I could not show without a running port", kept there in full:
- The heap aborts in the opt-in delete stress case (no mechanism found; no stress runs, Tom 2026-11-09). Macha is
  chasing a heap corruption of its own on the Pis (ASan); if it lands in Dagda's code, it comes first.
- The `mutex lock failed` abort of one 2026-11-05 macOS TSan run.
- The pool's `get` stopping at the first backend's empty answer: Macha's, kept.
- The trie pins aged on the node's time source where Macha reads the real clock (the same on live nodes).
- The Pis' under-voltage under heavy builds: their supplies are Tom's to look at; journals are persistent now.
- The store's journal lines: what still differs from Macha's is in `2026-11-12-journal-mapping.md`, section 8.
- Everything only the port can show: Macha's merge hash for hash, delta replay through Macha's codec, end to end,
  performance against the baselines, a real state started, reliability over time.

## The goal

Tom, 2026-11-13: "It needs to support Macha, Lir, and Lugh and not as afterthoughts", and to the stack: Dannan and
Dagda "support Macha, Lugh and Lir as first class citizens. This was the purpose of splitting out the technologies."
Macha's port still loses nothing: the question for Macha stays Tom's of 2026-11-06, "does Macha move onto Dagda with
no change in data, functionality, reliability and performance?", answered Yes with nothing attached, proven on its
nodes; the Macha session reviews, builds the port, and pauses it for a defect Dagda must fix with tests.

## Decisions

The rulings of 2026-11-13 and 2026-11-14 are in the design's "Ruled" section; those below stand where it does not
supersede them.

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
  generalising it. ~~Macha's needs come first; Lugh's and Lir's are
  generalisations.~~ Superseded 2026-11-13: the three are first-class.
- **Move the store code** from Macha into Dagda and generalise it in
  place, bearing in mind it runs on Dannan: Macha's transport, crypto,
  durable files, logging and time give way to Dannan's as it arrives.
- ~~**Formats stay compatible**, "because they work": layout v18 and the
  store encodings, byte for byte.~~ Superseded 2026-11-04 night (below:
  one layout, v18 migrated in place). Small values must be handled well.
- **Header tagline: "Dagda - The Distributed Object Store"** (Tom, 2026-11-15; it was "Dagda - The Store").
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

Settled earlier: the plan (approved 2026-11-04), "Dagda - The Store" (now "Dagda - The Distributed Object Store",
Tom, 2026-11-15),
Dagda's own version number from `0.1.0` pinning a Dannan tag, Dannan
0.2.0, free message registration from 512, byte-for-byte compatibility
(superseded 2026-11-04 night: one layout, migrated in place).
Decided by the Dagda session under the rules: the store's layout marker
is Dagda's; the gates' and observations' "catalogue" wording stays as
Macha's until Macha no longer reads it (its Status must stay
byte-identical through the port).
