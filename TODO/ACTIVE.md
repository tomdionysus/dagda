# Dagda - Active Work

Last updated: 2026-11-13. `COMPLETED.md` is the record; the plan this file held until today (the ordered work of
2026-11-06, the earlier table, Dannan as found, the approved plan of 2026-11-04, the port respec and what step 4
waited on) is at the end of `COMPLETED.md`, "Archived from ACTIVE.md, 2026-11-12", word for word.

## Where it stands

- **Every item of the 2026-11-06 order is done, and the capability report is sent**: `TODO/2026-11-10-dagda-capabilities.md`.
  Dagda is at the tag the port freezes on, on Dannan 0.5.3 (Macha's port pins the same, `../macha/CMakeLists.txt:154`).
- **Macha's port**: the transport is Dannan's, 0.94.0 live on both nodes (branch `port/dannan-dagda`). Next there: the
  CI kit, clang-tidy and 0.94.1, then the Macha session's review of the report by reading Dagda's code at the tag
  (its task 8), then the store cutover on it. A defect the port finds pauses it until it is fixed here, with tests,
  and Dagda freezes again (Tom, relayed 2026-11-06).
- **The migration rehearsal is done** (2026-11-12, corvus-develop, macnessa's 0.92.4 copy): every step and comparison
  passed. The Macha session supplies, at its review: macnessa's state as it is now under 0.94.0, and a timing at 4 TB.

## Open

From the report's "Open and unexplained" and "What I could not show without a running port", kept there in full:
- The heap aborts in the opt-in delete stress case (no mechanism found; no stress runs, Tom 2026-11-09).
- The `mutex lock failed` abort of one 2026-11-05 macOS TSan run.
- The pool's `get` stopping at the first backend's empty answer: Macha's, kept.
- The trie pins aged on the node's time source where Macha reads the real clock (the same on live nodes).
- inverbeg's outage of 2026-11-11 and the Pis' under-voltage: their supplies are Tom's to look at.
- The store's journal lines: mapped and restored to Macha's (0.1.2); what still differs is in `2026-11-12-journal-mapping.md`, section 8.
- Lir (2026-11-13, via Tom): Dagda is written for Macha and does not do replicas. Objects are replicated (the four
  levels, with evidence); head spaces held on N nodes (L2), a per-space k (L3), node-local spaces (L5) and Lir's scale (L6)
  are not built. L2 can be added beside the every-node path without changing Macha (the policy is chosen at open and is not
  on the wire or in the records; the wire already addresses a space other than the first; only the exchange's active-set
  sends branch). Told to Lir; its place in the order is Tom's.
- Everything only the port can show: Macha's merge hash for hash, delta replay through Macha's codec, end to end,
  performance against the baselines, a real state started, reliability over time, macnessa's heap corruption.

## While the port waits (Tom, 2026-11-12)

Cleanup, testing, hardening and improvement in scope, and nothing else: the manifest's partial held by its pass
(`0.1.2`), TSan on the Mac at the tag, the journal mapping, and this file; then, at the Macha session's ask, forked test
children ending on their own sanitizer reports, with TSan on arm64 (`0.1.3`). No change after the tag the Macha session
reviews unless the port or Tom asks for it.

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
frozen at `9a08846`. ~~There is no other purpose for Dagda; Lugh and Lir do not change that, and L items wait unless Macha
needs them.~~ **Superseded (Tom, 2026-11-13): "It needs to support Macha, Lir, and Lugh and not as afterthoughts."**
Next: a design serving all three as first-class, with the Lir and Lugh sessions, to Tom before anything changes; then
`0.1.5` (Dannan 0.5.6 and the Debian packages, on `feature/dannan-056`). Tell the Macha session when the answer is Yes.

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
