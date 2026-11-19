# Dagda - Active Work

Last updated: 2026-11-18 (evening). `COMPLETED.md` is the record; its newest handoff is "2026-11-18 (afternoon): the
Macha move only; Dannan 0.9.2 and four port fixes".

## Start here (a fresh session)

**The maintainer, 2026-11-18, to Macha, Dannan and Dagda: "work together and focus on the Macha move to Dagda. Work on nothing
else until this is done. Work autonomously until you're ready to do the cutover on live-a and live-b."** Everything
below serves that; the slices, Lugh and the rest wait.

1. Read "Where it stands" and "Next" here.
2. Ask the Macha session (`Macha Server` in ListAgents) for any port failure it holds for Dagda. A Dagda-caused
   failure comes first, test-first, gated on a Pi, the commit sent back.
3. Then "Next" in order.

The work is on `feature/macha-store-gaps`. Its tip `5c2e91e` and everything since `8e509cd` is local and unpushed:
pushing needs the maintainer's "push". Nothing is running on the Pis unless the board says so (`claims_list`).

## Where it stands

- **The branch is on Dannan 0.9.2** (`DAGDA_DANNAN_MINIMUM` 0.9.2, `debian/control` `>= 0.9.2, << 0.10~`), the
  Dannan the cutover deploys (agreed by the three sessions, 2026-11-18). **Last full gate, `053ee6e`**, node-b,
  Dannan 0.9.2 Release prefix, GCC 14 Release, warnings as errors, Macha's fixtures: **1152 ran, 1149 passed, 3
  skipped, 0 failed, 0 warnings**. ASan and UBSan at `7967737` (the same library less the three fixes below): no
  report, 14 known `std::regex` false-positive warnings.
- **Port fixes so far**, each a case failing first, then the fix, sent to the Macha session:
  - state layout beside the host's files (`8b544d6`)
  - a failed plane marked as it fails (`97a5bed`)
  - a stop waits for an open in progress (`fbdd50a`)
  - a claim places a short object on write_copies holders (`8398ec4`)
  - `Node::local_due()` for Macha's parking pass (`e6418ce`)
  - a credit waiter turned away for pressure woken when its refusal can end (`053ee6e`; this was the intermittent
    `ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit`)
- **Answered for the port without a change in Dagda:**
  - Group 4 (a backend taken away is not seen): Macha maps `NodeOptions::server.refresh_interval` to its heartbeat,
    as Macha's server refreshed every heartbeat.
  - The claims fault: the Dagda half is `HeadsExchange.ACommitIsClaimedHereWhenThePeerNeverAnswersTheClaim`; the
    seam is `NodeOptions::front`.
  - `dagda-migrate`'s command line and what it expects of a stopped node: `docs/migration.md` (`--node-state` is
    `--state`, custody by systemd credential, every backend named, manifests before and after on another disk).
- **The Macha session's plan to the cutover:**
  1. Its suite green on `053ee6e` (it was 540 passed, 4 failed on `7b2a7be`; the four are now answered).
  2. The config key mapping against both live `macha.yaml` files.
  3. A migration rehearsal on copies of both nodes' state on a claimed Pi, timed, every id listed before and after,
     with a written rollback.
  4. Custody provisioned and backed up off-node.
  5. The deploy tarball carrying libdagda, libdannan and `dannan-node`.
  6. The API changes announced to Core and the clients: `dagda.<name>` registrations, and `offer_bytes` and
     `in_flight` in `/status`.
- **Repair is redesigned** (`TODO/2026-11-16-repair-redesign.md`). Stages 1 to 4 are done and gated. Small rejoin: 3
  times faster than stage 3. Rejoin under a viewer: faster. **The unpaced large rejoin: still about 3 s slower than
  stage 3 (16.7 to 20.9 s against 14.1 to 17.1); cause not yet found.** Macha's live nodes keep today's repair until
  the cutover.
- **The side-by-side harness** (`TODO/2026-11-15-side-by-side-harness.md`, `tools/harness/`), measured against Macha:
  - At parity or better: publications, the viewer, local reads, recovery (within 2%).
  - Small files: about 6% over Macha's CPU.
  - Not run: reads over a link, CONTROL graphs, deletes and GC, disk pressure, cache.
  - Figures before repair's are on node-b's old kernel. Repair's are on a WiFi link that varies, so a stage is
    measured old against new in one session.
- **`0.1.5`** (main `c20f88c`, Dannan 0.6) is the last pushed release. **`0.2.0`** (slice 1) is tagged locally only
  and held (the maintainer, 2026-11-14 and 2026-11-16). The branch contains it.

## Next (in order): the cutover

1. **Any failure Macha's suite finds in Dagda**, first.
2. **The large rejoin's remaining 3 s.** The holder's side is now timed: `06b3960`, `StorageServerOptions::serve_timing`,
   printed by the harness's `TRACE=1` as `serve-time` lines beside `repair-time`, both with `wall=` for lining the two
   Pis up.
   - A traced run is done and not yet read (2026-11-18 18:48-18:51Z): node-b holding, node-c joining, store
     `dagda-092` (`/root/harness/build-092`, copied to node-c with `/root/dagda-092/dannan-prefix`), link 13.5 MB/s.
     Rounds took 16.3, 17.6 and 9.7 s, where the link alone takes about 8 s. Trace:
     `build/harness/results/2026-11-18-holder/repair-err.txt`. Claims released.
   - Read b's first fetches against a's `serve-time` (arrived, admitted, read) by wall clock. Whatever the holder
     doesn't account for is the wire or Dannan's send path. The method and leads are in the redesign document,
     "The holder's side, traced".
3. **Release prep** for the deploy's tag:
   - ASan and UBSan again at the tag's commit.
   - The Debian packages built on a Pi (`tools/build-deb.sh`): `libdagda<ver>`, `libdagda-dev`, `dagda-tools` with
     `dagda-migrate`; installed and consumed (`test/package`); lintian.
   - The version in `CMakeLists.txt` and `debian/control` set to the number the maintainer gives ("Waiting on the maintainer").
4. After the cutover, not before:
   - Route Dagda's raw syncs through Dannan's `storage::FileSystem`: `ledger/sealed_journal.cpp`, the durability
     domain and pool barriers, `heads/replica.cpp`, `heads/node_pack.cpp`, `store/local_store.cpp`,
     `migrate/migrate.cpp`, `migrate/manifest.cpp` and its flock; `LocalStoreFiles` becomes an adapter.
   - With that, directories Dagda makes before a durable write get synced into their parents: `node.cpp:769`
     (`maintenance/`), `storage_pool.cpp:217`, `unreferenced_since.cpp:129`, the tries', retention's and the node
     pack's at open. This is the class of Dannan's `ef3b212`. Harmless on a migrated node: those directories already
     exist bar `maintenance/`, whose `scrub.next` only times the next scrub. Prove it with Dannan's simulated disk
     (`dannan/test/fixtures/sim/disk.h`).
   - The rest of harness phase 2, both stores rerun. The slow-disk run is approved (the maintainer, 2026-11-16): device-mapper
     delay over a loop file on one Pi, with a note on the board.
   - The store map's gaps: the pool's TimedLock (Dannan), and 8c's loop items.
   - Paused with the slices: `0.2.0`'s release steps (`COMPLETED.md`, "Archived from ACTIVE.md, 2026-11-15"), the
     follower's lag (Lugh's P8.5), slice 0's last two figures, slices 2 to 5.

## Waiting on the maintainer

- **The deploy's tag.** Macha needs a Dagda tag to deploy (or the maintainer's word on deploying a bundle). `0.2.0` is held, and
  this branch is 0.2.0 plus the Macha store gaps, repair stages 1 to 4 and the port fixes, on Dannan 0.9.2.
  Recommendation: tag **0.3.0** from this branch once Macha's suite is green on it, after the release prep in Next 3,
  and push `main`, `develop` and the tag at his word. That also releases slice 1 (0.2.0's content), which Lugh asked
  for on 2026-11-16.
- **A push** of `feature/macha-store-gaps` (local since `8e509cd`): the Macha session builds from this machine's
  repository, so nothing waits on it, but the remote is behind.

In Macha's list, and Dagda follows: the remaining design defects there (the torrent merge, the fence on actions
under a claim, the local barrier's deadline, wall versus steady ages, the policy disagreement loop, O(state) per
delta, wire-sized allocations, reload ignoring `write_copies`, the I/O and control reserves).

**Written down so it is not lost (the maintainer, 2026-11-05):** two metadata behaviours heads keeps as Macha's for now,
to change later as a deliberate step, in Macha first: a mutation that changes nothing still commits its clock
(`metadata_manager.cpp:1919` unreachable); a policy commit carries no dot (`:1376`), so after truncation only a
merge can collapse it.

## How work runs

- **No builds, tests or benches on the Mac.** Linux gates run on the shared Pis over ssh:
  - Claim on the board first (`claims_list`, then `claim_take` with only the cores the job's `-j` uses and an honest
    end).
  - nice 19 and ionice idle.
  - Release the claim the moment the job ends or fails.
  - When none is free, `claim-board wait ... &` in the background; never poll.
- **Where things are on the Pis:**
  - **node-b (.169):** the 0.9.2 gate under `/root/dagda-092`. That holds `dannan-prefix` (Dannan 0.9.2 Release),
    `repo` (a clone fed by `dagda-gaps*.bundle`), `src`, `build` (Release, warnings as errors), `build-asan`, and the
    gate scripts `pi-*.sh` writing `status.txt`.
  - **node-b, harness:** under `/root/harness`. `build-092` is the tip on 0.9.2; `build-tip` and `build-s3` are on
    0.8.2.
  - **Macha's fixtures:** `/root/dagda-release/macha-fixtures` on node-b and node-a.
  - **node-c (.170):** `/root/dagda-080` (0.8.0), and a copy of node-b's 0.9.2 prefix and `build-092` harness.
  - **node-a (.202):** the Macha session's port builds.
  - **node-d (.228):** 4 GB; left out of benchmarks until the maintainer says (it went down mid-run on 2026-11-17).
  - All on WiFi, kernel 6.18.62-v8-va48+; all report past under-voltage (`throttled=0x50000`).
- **Shipping the branch:** it travels as a git bundle (`git bundle create build/ship/dagda-gaps.bundle
  feature/macha-store-gaps`, then scp). Gate scripts are written there with `cat > pi-x.sh <<EOF` over ssh and run
  with `nohup ... &`. Wait on a marker line in `status.txt` with a background `until ssh ... grep -q marker; do sleep
  60; done`.
- **Test-first is shown on the Pi:** build at the commit that adds the cases and see them fail, then build the fix
  and run the whole suite once. One whole-suite run at a step's last commit; no rerun without a named reason.
  Sanitizers only for a release or to hunt one named bug.
- **Measurements are like for like:** old binaries against new in one session, the link recorded each round.
  `TRACE=1` runs find mechanisms and are never figures.
- **Local tools:** the hook blocks grep, sed, awk, head, tail and cat over local files. Use basemind (`code` mode
  grep, outline, find) and the Read tool; a remote grep over ssh is fine.
- **The other sessions:**
  - **Macha Server:** the port; it receives every gated commit and sends Dagda-caused failures with their mechanism.
  - **Dannan:** the network layer; Dagda never edits `../dannan`, `../macha`, `../lugh` or `../lir`.
  - The maintainer's relayed words come through them. A Macha defect is sent to the Macha session before Dagda deviates from
    Macha's behaviour.

## Open

- The heap aborts in the opt-in delete stress case (no mechanism found; no stress runs, the maintainer 2026-11-09).
- The `mutex lock failed` abort of one 2026-11-05 macOS TSan run.
- Dagda's four-reader local reads come out in two clusters (about 344 MB/s twice in six runs, else 363-407), where
  Macha's sit at 379-388 bar one outlier; the off-CPU profile shows no wait of Dagda's own.
- The pool's `get` stopping at the first backend's empty answer: Macha's, kept.
- The trie pins aged on the node's time source where Macha reads the real clock (the same on live nodes).
- The Pis' under-voltage under heavy builds: their supplies are the maintainer's to look at.
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

**The maintainer, 2026-11-18:** the Macha move to Dagda only, until ready for the cutover on live-a and live-b (above). Agreed
among the three sessions the same day: the cutover deploys on Dannan 0.9.2. The Macha session agreed that Dagda fixes
the arbiter's lost wake in Dagda (Macha's arbiter goes at the cutover) and chose its shape: wake when the refusal can
end, never poll.

**The maintainer, 2026-11-16, directly: repair is redesigned, not patched.** On a reserved share of the 16-call budget and on
push and pull taking turns: "We're not building a toy or playing least-effort whack a mole here. What's the
correct, performant way to do it?" To the design (one queue of transfers ordered by copies left, the survey keeping
who holds what, each copy moved once, a window of transfers against the byte credit, the call count gone, a
benchmark first): "yes, I think we can do that." The design and its stages: `TODO/2026-11-16-repair-redesign.md`.
Macha's live nodes keep today's repair until the cutover; the rule below that a Macha defect is fixed in Macha
first is set aside for this, as Macha's store is deleted on its port branch.
Asked the same day, two answers. **The offer is allowed**: a holder may ask an owner which ids it wants before
sending bytes, as a handshake at the moment of transfer; the diff stays the only way repair finds what to do
("repair is diff only" otherwise stands). **The benchmark's link is the real one between two Pis**, both claimed
whole and their interface shaped, as the harness plan first said, not one Pi's loopback.
Then, the link measured (WiFi, 0.78 MB/s): **the two Pis on the WiFi as it is, unshaped**, with the sizes cut
about tenfold so a run takes minutes; the link's rate and round trip recorded before each round.
Asked again that evening, four answers. **`0.2.0` stays held**: Lugh waits; nothing changes in "Macha first".
**The slow-disk run is wanted**: set up the device-mapper delay target on one Pi when one is free, over a loop
file, with a note on the board. **The benchmark's controller goes in the repository**, under `tools/harness`.
**The instant delete on a node with no inventory: ask Lugh and Lir first**, then it comes back to him.
Both answered (Lugh: it does not arise, it writes no DATA objects; Lir: keep the copy, it needs no space back at
once), and the maintainer ruled: **a node with no inventory published keeps the copy**, as one with an incomplete inventory
does; a test-first fix ahead of repair's stage 3. Raised by Lir on the way and closed in Lir by the maintainer the same
evening: two messages never share a large payload's object (each object begins with its message's dot), so the
instant delete releasing every claim its clock dominates costs Lir nothing, and Dagda's contract is unchanged.

**The maintainer, 2026-11-17.** To stage 3's regression under a viewer and the fix recommended (a holder offers only what
the step will send; a promised pull is looked at each second), before stage 4: "Yes." Built and measured.
Relayed by the Macha session the night before: `offer_bytes` and `in_flight` join Macha's /status repair section
at the cutover, announced to Core and every client with the other cutover API changes.

**The maintainer, 2026-11-15:** acknowledgement levels are the stack's standard: ephemeral, persisted, decided (Dannan's
agreement), replicated (a majority of the replication set); the setting is `ack_level`; Dagda's durable stays beside
them until the maintainer says otherwise. Agreement waits for Dannan: Dagda builds no consensus or stand-in. The header tagline is
"Dagda - The Distributed Object Store".

**The maintainer, 2026-11-05, later, to the Macha session and relayed by it; where it differs from the list below,
this is the later word:**
- The real migration on a second copy, the there-and-back and the live-b rehearsal are dropped for now: "No drop
  that, I'll decide later." Nothing in the plan may wait on them. **Decided later:** The maintainer had a rehearsal run on a copy
  of live-a's state on 2026-11-12 (COMPLETED.md), and on 2026-11-14 made "the migration rehearsal on copies of real
  state" one of the gates before the flag day (Macha's `TODO/2026-11-14-port-rewrites.md`, "The gates").
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
- **Every write carries a durability level** (superseded 2026-11-15 by the stack's acknowledgement
  standard, above) chosen by the caller
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
