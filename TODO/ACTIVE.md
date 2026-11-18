# Dagda - Active Work

Last updated: 2026-11-17 (evening). `COMPLETED.md` is the record; its newest handoff is "2026-11-17 (evening):
repair's stage 4". The plan for slices is the three-systems design, `TODO/2026-11-13-three-systems-design.md`; the
slices are paused (below).

## Start here (a fresh session)

**Tom, 2026-11-18 (to Macha, Dannan and Dagda): "work together and focus on the Macha move to Dagda. Work on nothing
else until this is done. Work autonomously until you're ready to do the cutover on macnessa and inverbeg."** So
"Next" below is the cutover's list; everything else waits. Macha's plan (the Macha session, 2026-11-18): its suite on
`fbdd50a`, fixed group by group (`../macha/TODO/2026-11-17-port-suite-triage.md`), then the config mapping against both
live `macha.yaml`, a migration rehearsal on copies of both nodes' state on a claimed Pi (`docs/migration.md`), custody,
the deploy tarball with libdagda and libdannan, and the API changes announced. Agreed the same day: **the cutover
deploys on Dannan 0.9.2**, provided this branch builds and gates on it (Dannan's two fixes: a retained-memory lease
used after its node at shutdown, `79dfa2e`; a new directory not synced into its parent, `ef3b212`). What Macha needs
from Dagda: a tag for the deploy (Tom's word; `0.2.0` is held), `dagda-migrate`'s command line (sent), and a fix for
any port failure whose cause is Dagda's.

## Where it stands

- **Macha first; slices paused; `0.2.0` held** (Tom, 2026-11-14). Work is on `feature/macha-store-gaps` (local,
  nothing pushed). `0.1.5` (main `c20f88c`, Dannan 0.6) is the last pushed release; `0.2.0` (slice 1) is tagged
  locally only.
- **The branch** builds on Dannan 0.8.x (`DAGDA_DANNAN_MINIMUM` 0.8.0; 0.8.1 to 0.8.5 keep 0.8.0's ABI. 0.8.5
  fixes a node stopped while its io ran, then destroyed after the io stopped, hanging in its second stop: a
  teardown hang in Dagda's tests would look there first). **Last full gate: `6e1f4dd`**, saturn, Dannan 0.8.0
  prefix, GCC Release, warnings as errors, Macha's fixtures, kernel 6.18.62-v8-va48+, not throttled: 1141 ran,
  1138 passed, 3 skipped, 0 failed, 0 warnings (eight cases more than `d119d14`'s 1133, none removed). Then
  `8b544d6` (the port's state-layout defect): 1144 ran, 1141 passed; `82e5201`, saturn as above: 1145 ran, 1142
  passed, 3 skipped, 0 failed, 0 warnings. **Last full gate, `79e055d`**, jupiter, Dannan 0.8.2 prefix: 1146 ran,
  1142 passed, 3 skipped, 1 failed (`ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit`, no repair code,
  passed three times alone after; in Open).
- **The branch is pushed** (Tom, 2026-11-17) to `8e509cd`; later commits are local. No tag pushed; `0.2.0` held.
- **Fixed for Macha's port (2026-11-17, late): a failed plane is marked on readiness as it fails** (`ac96a5a` the
  case, `97a5bed`): the refusal wrapping the whole open told a plane's failure only when both planes had ended.
  **Gate at `97a5bed`**, neptune, Dannan 0.8.0 prefix, Macha's fixtures: **1147 ran, 1144 passed, 3 skipped, 0
  failed, 0 warnings**. The Macha session was sent the commit (it waited on it).
- **A stop while the store opens waits for the open** (2026-11-18, `591669a` the case, `fbdd50a`): Macha's fixture
  stopped its node while its recovery thread was in `open_store()`, and the open crashed in the server's
  constructor (proven by the Macha session's backtraces; its fixture is fixed). Dagda guards it: the open holds a
  lock, a stop from another thread waits, and the open ends refused at its next stage. **Last full gate,
  `fbdd50a`**, saturn, Dannan 0.8.0 prefix, Macha's fixtures: **1148 ran, 1145 passed, 3 skipped, 0 failed**.
- The cluster was Miach's for the night of 2026-11-17 only (relayed by Lir at 00:25Z); in ordinary shared use again
  by 2026-11-18 16:30Z (the Dannan session).
- **corvus-uranus went down at about 22:37Z** during a traced repair round on it, and came back at 22:54Z already
  showing past under-voltage (`0x50000` at one minute up); it keeps no persistent journal. Left out of benchmarks
  until Tom says.
- **Fixed for Macha's port (2026-11-17): a store begins in a state directory holding its host's files**
  (`844c557` the cases, `8b544d6`). Macha's first full suite (port at 8b40c4a) refused every fresh node, as its
  state directory is Dannan's too. An unmarked state is refused only when it holds what a store keeps there.
  The Macha session confirmed nothing on the list is written before the store opens, and builds on `8b544d6`.
- **Repair is redesigned in stages** (Tom, 2026-11-16; `TODO/2026-11-16-repair-redesign.md` has each stage's
  state, what was built, the rulings, the figures and how to measure). Stages 1 to 4 are done and gated: one queue
  by copies left, each copy crossing the link once, a window of transfers in place of the 16-call count, and
  pulls fetched many to a message. Stage 4's API changes (`repair_step`'s window, `repair_drain`,
  `Repair::drain`, `credit_spent`, `in_flight`, `offer_bytes`, `get_objects`) and the cases it changed are listed
  in the redesign document for the Macha session.
- **Dannan 0.9.0 is tagged** (main `59b7abc`, 2026-11-17, the Dannan session). The hybrid clock's ceiling on a
  node without custody stays a durable file (Tom's ruling, Dagda's preference; Dagda refuses to open without
  custody, so it is unaffected).
- **The instant delete keeps the copy on a node with no inventory published** (Tom's ruling; `5f3c458`).
- **Macha's port** (the Macha session) builds against this branch: R1 to R5 done, R6 and R7 mostly, at `deceac1`
  on its `port/dannan-dagda`, compile-only so far (the Macha session, 2026-11-16). Macha is given each gated
  commit (it has `d119d14`), and retires its own cases against named `dagda_tests` cases, mapped one by one. No
  ported case has changed. Stage 3 asks nothing of its pass.
- **The side-by-side harness** (`TODO/2026-11-15-side-by-side-harness.md`, `tools/harness/`). Measured:
  publications and the viewer at parity or better; small files about 6% over Macha's CPU (was 11%), all else
  equal; local reads better with one reader and no steady gap with four; recovery at parity within 2%; repair
  (the baseline against Macha, then stages 1 to 3 against the baseline's Dagda; Macha not rerun since). Not run:
  reads over a shaped link, CONTROL graphs, deletes and GC, disk pressure, cache. The figures before repair's
  are on jupiter's previous kernel (6.18.50); repair's are on a WiFi link that varies. A comparison with later
  figures runs both sides again.
- **Acknowledgement levels are the stack's standard** (Tom, 2026-11-15): ephemeral, persisted, decided (Dannan's
  agreement, 0.9.0), replicated; the setting is `ack_level`. Done on the branch and in the stack document. Dagda's
  durable stays beside them until Tom says otherwise.
- **Agreement waits for Dannan** (Tom, 2026-11-15): Dagda builds no consensus or stand-in; its part is a persisting
  acceptor store on a node-local log (slice 4).
- **New consumers, nothing asked of Dagda now:** Louhi, Mimir and Kvasir, each answered from the design.
- **Dannan 0.9.0** (tagged 2026-11-17, main `59b7abc`; its .debs staged for Tom to publish) carries agreement, the hybrid
  clock, `storage::FileSystem` and peer records shared per session (the net layer's copies gone). SONAME
  `libdannan.so.0.9`: a minor bump, so Dagda raises `DAGDA_DANNAN_MINIMUM` when it adopts it. FileSystem meets
  Dagda's two conditions: `lock(path)` (flock `LOCK_EX|LOCK_NB` for the handle's life, a second holder refused
  `ErrorClass::busy`, for `migrate/manifest.cpp`) and `sync_directory(path)` (fsync of an `O_DIRECTORY` fd;
  `sync_all` is syncfs). Dagda told the Dannan session what the tag must hold; DTLS, the high-water mark and the
  rest can wait for 0.10.0.
- **Lugh asked** (2026-11-16) when slice 1 can carry its spaces so P8.0 can start. Tom, asked the same day:
  `0.2.0` stays held. Lugh is told, and that the repair redesign changes nothing it relies on.

## Next (in order): the cutover

0. ~~**Dannan 0.9.2**~~ done, with three port fixes (COMPLETED.md, "2026-11-18 (afternoon)"). **Last full gate,
   `053ee6e`**, jupiter, Dannan 0.9.2 Release prefix (`/root/dagda-092/dannan-prefix`), Macha's fixtures: **1152 ran,
   1149 passed, 3 skipped, 0 failed, 0 warnings**. ASan and UBSan at `7967737`: no report.
0a. **Any failure Macha's suite finds in Dagda**, first. Answered so far: group 4 is Macha's mapping of
   `server.refresh_interval` to its heartbeat; the touch-commit claim, the pass's park (`local_due`) and the arbiter's
   wake are fixed in Dagda.
0b. **Directories Dagda makes before a durable write** are not synced into their parents (the same class as Dannan's
   `ef3b212`, found 2026-11-18): `node.cpp:769`, `storage_pool.cpp:217`, `unreferenced_since.cpp:129`, the tries',
   retention's and the node pack's directories at open. Objects are safe on Linux (the barrier is a syncfs). On a
   migrated Macha node each of those directories already exists (v18 has `retention/`, `ledger/referenced/`) but
   `maintenance/`, whose `scrub.next` only times the next scrub: nothing a crash there can lose matters. So it goes
   with item 5 (Dagda's writes through Dannan's `FileSystem`, where the simulated disk, `dannan/test/fixtures/sim/disk.h`,
   can prove it by a crash), not ahead of the cutover.
0c. **Release prep** for the tag Tom is asked for: ASan and UBSan again at the tagged commit (done at `7967737`, clean),
   the Debian packages (`dagda-tools` with `dagda-migrate`) built and installed on a Pi, the version.
1. **Repair stage 4's unpaced large rejoin is still about 3 s slower than stage 3's** (16.7 to 20.9 s against
   14.1 to 17.1). One cause found by tracing and fixed (`79e055d`: the link's estimate collapsed under the
   window); the rest is not yet known. The redesign document, "The unpaced large rejoin: what the trace found",
   has the trace's method and the leads. The small rejoin (three times faster) and the rejoin under a viewer
   (faster) are fine.
2. **Adopt Dannan 0.9.0** (tagged): `DAGDA_DANNAN_MINIMUM` to 0.9.0, a build and one whole-suite run on its
   prefix (a Pi needs the 0.9.0 prefix built first), then item 5 below on it.
3. **The rest of harness phase 2**, both stores rerun: reads over a link, CONTROL graphs, deletes and GC, disk
   pressure, cache; strace fsync counts on a separate rerun. **The slow-disk run is approved** (Tom,
   2026-11-16): when a Pi is free, a note on the board, then the device-mapper delay target over a loop file on
   that one Pi (no real disk touched), removed afterwards. None of these workloads is written yet, and the Macha
   session reviews each before it runs.
4. **Remeasure on Dannan 0.9.0** (the net-layer copies go).
5. **Route Dagda's raw syncs through Dannan's file-system interface** once 0.9.0 has it: `ledger/sealed_journal.cpp`,
   the durability domain and storage pool barriers, `heads/replica.cpp`, `heads/node_pack.cpp`,
   `store/local_store.cpp`, `migrate/migrate.cpp`, `migrate/manifest.cpp` (and its flock); `LocalStoreFiles` becomes
   an adapter over it.
6. **Gaps left from the store map:** the pool's gap 9 TimedLock (Dannan); 8c's loop items (gc-and-deletion gaps 1-4
   are the loop's 8c work; 5 is Dagda's own stamp decision, its line kept; 9 is done).

A defect Macha's port finds comes before any of these.

Paused with the slices, kept for when Tom resumes them: `0.2.0`'s release steps (`COMPLETED.md`, "Archived from
ACTIVE.md, 2026-11-15"), the follower's lag (Lugh's P8.5 waits on it; slice 2's first step), slice 0's last two
figures, then slices 2 to 5 as the design orders them.

## How work runs

- No builds, tests or benches on the Mac. Linux gates run on the shared Pis, claimed on the board first, nice 19,
  released at once: saturn (.170) and neptune (.202) hold the Dannan 0.8.0 prefix and Macha's fixtures under
  `/root/dagda-080`; jupiter (.169) holds the suite's build on the 0.8.2 prefix under `/root/dagda-s3` (fixtures
  at `/root/dagda-release/macha-fixtures`) and the harness under `/root/harness`; uranus (.228) has 4 GB and the
  harness's binaries. The branch travels as a git bundle; the gate scripts are kept beside each build there.
- Test-first is shown on the Pi: build at the commit that adds the cases, see them fail, then build the fix and
  run the whole suite once.
- **Claim what the job uses** (Tom, 2026-11-16): the cores of its `-j`, an end time from a measured rate. A run
  bound by the network or a disk needs a core or two, not a whole Pi for hours. Drop the page cache or fstrim
  only on a Pi held whole. A heavy transfer between Pis slows every session's ssh and rsync: a note on the board
  first.
- One whole-suite run at a step's last commit. No rerun without a named reason. ASan only for releases. TSan only
  to hunt one named bug (Tom, 2026-11-16), on any Pi; on the Mac only when hunting.
- All four Pis run a 4K-page, 48-bit-VA kernel (6.18.62-v8-va48+) since 2026-11-16, with ASan, UBSan and TSan
  working on each (clang 19 on all but uranus; `free -m` first there). All are on WiFi (`eth0` down): jupiter and
  neptune on 5 GHz since 2026-11-16 20:10Z, saturn and uranus on 2.4 GHz.
- Measurements are like for like: `uname -r`, page size, uptime and the link checked before each, both sides in
  one session.
- The local hook blocks grep, sed, awk, head, tail and cat over local files: basemind and the Read tool instead;
  a remote grep over ssh is fine.

## Open

From the report's "Open and unexplained" and "What I could not show without a running port", kept there in full:
- The heap aborts in the opt-in delete stress case (no mechanism found; no stress runs, Tom 2026-11-09). Macha is
  chasing a heap corruption of its own on the Pis (ASan); if it lands in Dagda's code, it comes first.
- The `mutex lock failed` abort of one 2026-11-05 macOS TSan run.
- Dagda's four-reader local reads come out in two clusters (about 344 MB/s twice in six runs, else 363-407), where
  Macha's sit at 379-388 bar one outlier; the off-CPU profile shows no wait of Dagda's own (harness plan, phase 2).
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

**Tom, 2026-11-16, directly: repair is redesigned, not patched.** On a reserved share of the 16-call budget and on
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
once), and Tom ruled: **a node with no inventory published keeps the copy**, as one with an incomplete inventory
does; a test-first fix ahead of repair's stage 3. Raised by Lir on the way and closed in Lir by Tom the same
evening: two messages never share a large payload's object (each object begins with its message's dot), so the
instant delete releasing every claim its clock dominates costs Lir nothing, and Dagda's contract is unchanged.

**Tom, 2026-11-17.** To stage 3's regression under a viewer and the fix recommended (a holder offers only what
the step will send; a promised pull is looked at each second), before stage 4: "Yes." Built and measured.
Relayed by the Macha session the night before: `offer_bytes` and `in_flight` join Macha's /status repair section
at the cutover, announced to Core and every client with the other cutover API changes.

**Tom, 2026-11-05, later, to the Macha session and relayed by it; where it differs from the list below,
this is the later word:**
- The real migration on a second copy, the there-and-back and the inverbeg rehearsal are dropped for now: "No drop
  that, I'll decide later." Nothing in the plan may wait on them. **Decided later:** Tom had a rehearsal run on a copy
  of macnessa's state on 2026-11-12 (COMPLETED.md), and on 2026-11-14 made "the migration rehearsal on copies of real
  state" one of the gates before the flag day (Macha's `TODO/2026-11-14-port-rewrites.md`, "The gates").
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

Nothing of Dagda's is waiting on Tom. (Dannan's 0.9.0 tag waits on his word, in the Dannan session.)

In Macha's list, and Dagda follows: the remaining design defects there (the torrent merge, the fence on actions
under a claim, the local barrier's deadline, wall versus steady ages, the policy disagreement loop, O(state) per
delta, wire-sized allocations, reload ignoring `write_copies`, the I/O and control reserves).

**Written down so it is not lost (Tom, 2026-11-05):** two metadata behaviours heads keeps as Macha's for now,
to change later as a deliberate step, in Macha first: a mutation that changes nothing still commits its clock
(`metadata_manager.cpp:1919` unreachable); a policy commit carries no dot (`:1376`), so after truncation only a
merge can collapse it.
