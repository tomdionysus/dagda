# Dagda - Active Work

Last updated: 2026-11-20. `COMPLETED.md` is the record; its newest handoff is "2026-11-19 (night): the short file;
0.3.2 tagged".

## Start here (a fresh session)

**Tom, 2026-11-20: the cutover is no longer Dagda's concern** (Decisions). Macha builds on 0.3.2 by version; the
2026-11-18 rule of working on nothing else is lifted.

1. Read "Where it stands" and "Next" here.
2. A failure the Macha session reports in a Dagda release still comes first, test-first, gated on a Pi, the commit
   sent back.
3. Then "Next" in order.

Work goes on `develop` or a feature branch from it. `main`, `develop` and the `0.3.1` and `0.3.2` tags are pushed
(Tom, 2026-11-19 and 2026-11-20); the tags `0.2.0` and `0.3.0` are local only. Push only on Tom's "push". Nothing is running on the Pis
unless the board says so (`claims_list`).

## Where it stands

- **0.3.2 is the release for the cutover** (Tom, 2026-11-19 night: "both you and macha should cut a new version, and
  restart the cutover plan from there"). Tagged on main `a24db42`; pushed (Tom, 2026-11-20). It is 0.3.1 plus `Space::covers(view,
  dot)`. Gate in `COMPLETED.md`: plain 1170 of 1173, 0 failed; ASan and TSan 0 failed, no report. The cutover plan
  restarts from Macha's next version on it: the rebuild, then rehearsals R1 to R4 and the two-writer round.
- **The short file** (`COMPLETED.md`, 2026-11-19 night): Macha's cached view kept a peer's head over this node's
  own commit, and FUSE took the lagging view as a replacement. It is fixed in Macha (`port/dannan-dagda` b0b037c).
  Dagda's Space has the same ordering and follows Macha's fixed rules after the cutover (Next 2).
- 0.3.1 (main `2793458`, pushed) carries, past 0.3.0:
  - `dagda-migrate` at the disk's rate (Tom's B): records moved from their headers with a progress record; the
    manifest pass reading loose objects in the device's order on worker threads; `--read-every-record`.
    `docs/migration.md`, "How a run moves records (0.3.1)"; the plan and findings `TODO/2026-11-19-migrate-speed.md`.
  - Repair's large rejoin closed: a fetch of many asks again as it lands; an idle node's repair earns the whole link,
    `idle_bandwidth_fraction` only while a higher class is present (`TODO/2026-11-16-repair-redesign.md`).
  - Gate in `COMPLETED.md`: plain 1169 of 1172, 0 failed; TSan at the tag 0 failed, no report; ASan/UBSan no report.
- **The cutover is the Macha session's**, on Tom's word, not scheduled. As last told (2026-11-19 night): it is
  preparing for a clear; then Macha rebuilt on 0.3.1 with its suite and the deploy tarball; then small rehearsals it
  will propose to Tom; then the cutover, both nodes stopped together, macnessa first. Three passes a node (manifest
  before, the migration, manifest after): about 29 h for macnessa (5.9 TB, 1.54 M loose objects), 16 h for inverbeg
  (3.3 TB, 0.90 M), from the Macha session's measured rates. **Nothing of Dagda's touches macnessa or inverbeg.**
- **For the runbook, answered from code:** 0.94.2's store and Dagda's must not replicate together (capacity adverts
  differ, 0.94.2 answers 769-774 with 501, and its `Reader::finish()` refuses Dagda's appended principal on every put
  and claim); prompt replication's copies do land there. The runbook keeps the mixed window to minutes with
  `collect_garbage` false. `dagda-migrate` is purely local; its credential by `systemd-run -p
  LoadCredentialEncrypted=dannan-root:<sealed> ... --credential dannan-root`, no `--credentials-dir`. Every
  crash case of the migration and which stage it covers was sent to the Macha session; **a power cut is not proven**
  (every case kills a process; Tom: power-cut cases after the cutover, Next 2).
- **Worth confirming with the Macha session:** that its mapping carries the live nodes' `dht.io_pressure_*` into
  `AdmissionOptions::device_pressure` (an older table had it missing). The device pressure gate refuses speculative
  work on a slow device by design; under sanitizers it trips on the Pis (316-482% of the expected service time).
- **Port fixes, each a case failing first** and sent to the Macha session: state layout beside the host's files
  (`8b544d6`), a failed plane marked as it fails (`97a5bed`), a stop waits for an open in progress (`fbdd50a`), a
  claim places a short object on write_copies holders (`8398ec4`), `Node::local_due()` (`e6418ce`), a credit waiter
  woken when its pressure refusal can end (`053ee6e`), a read asks the active peers placement does not name
  (`b997ba1`). Macha's suite: 545 of 545 on 053ee6e plus the last.
- **The side-by-side harness** (`TODO/2026-11-15-side-by-side-harness.md`, `tools/harness/`): at parity or better on
  publications, the viewer, local reads, recovery; small files about 6% over Macha's CPU; not run: reads over a link,
  CONTROL graphs, deletes and GC, disk pressure, cache. Repair is measured old against new in one session.

## Next (in order)

1. **Any failure Macha's suite or its rehearsals find in Dagda**, first.
2. **The next release** (Decisions, 2026-11-20), test first, in this order, agreed through Lir Development Cluster:
   - the register map (D6);
   - head-backed leases (D5) on it, which closes the special-roles audit's D-1;
   - the AcceptorStore;
   - the caught-up signal;
   - reads and listings;
   - FetchContent;
   - then D-5 (parallel head replication, a durable write's bound past the slowest member) and D-2's interim
     (tombstone maturity on Dannan's hybrid clock), both released by the group.

   **The stack's converged plan** (2026-11-20, agreed by Dagda): Dagda's full list is "The converged plan" ->
   "Dagda" in `../lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md`, items 1-13. Besides the above,
   it holds:
   - retire and drain (evacuate objects and claims; the joiner's survey over the peers that answered);
   - the catalogue's admitted readers (slice 5);
   - store health into Dannan's readiness;
   - a set-aside head never dropped silently;
   - Space taking Macha's short-file fix, and a heads migration for Macha;
   - `DataWorkContext` co-owning its counters;
   - group commit generalised;
   - raw syncs through `storage::FileSystem`, the RecordingSink copy gone, Dannan's callback vocabulary;
   - keep owning what Macha aliases.

   D6's stamp kind is the one open choice (Tom's Q2, hlc or Lamport); for hlc, Dannan's `HlcClock::admit` (a stamp
   with `merged=false` is refused, never applied).

   Audits: `TODO/2026-11-20-duplication-audit.md`, `TODO/2026-11-20-special-roles-audit.md`.
   `feature/dannan-reuse` (`97a079e`): Dannan's CPU pacing and RetryState replace Dagda's copies.

   The shapes and every system's answers are in `TODO/2026-11-20-louhi-reads.md`:
   - The durable AcceptorStore: a node-local log per agreement space, whole or delta records, group commit (section
     4).
   - The register map (D6), and its two additions only if Lir or Lugh needs one.
   - Context v2: read one key; list by key prefix with paging, over a head space.
   - The caught-up signal: per space, a query and a notice. It is three-valued: caught up, behind and alone, with
     the count compared. Peers' heads come from a survey of the peers holding the space.
   - Dagda consumable by FetchContent beside a FetchContent'd Dannan 0.9.2 (today `find_package(dannan)` at
     `CMakeLists.txt:32` needs Dannan's `OVERRIDE_FIND_PACKAGE`; `install(EXPORT)` as a subproject untested).
3. **Lir's two needs**, answered 2026-11-20:
   - **The offline queue** is served by D9 as designed: the publisher matches the filters and appends under its own
     dot, any node may append, and a second append of a dot is the same entry. One change: a log's identity is
     (stream key, dot), so one message lands once in each matching session's stream, and acks and retire name
     (stream, dot). It is folded into D9 when D9 is built; Lir to agree.
   - **The will** is Lir's own, on Dagda's head-backed leases (D5): no second project needs it.
4. No longer held for the cutover (Tom, 2026-11-20):
   - **Space follows Macha's short-file rules** (b0b037c), test first, with its
     `test_a_set_aside_head_leaves_the_view` as the model:
     - `cache_record` never moves the view off a record covering this node's latest commit onto one that does not;
     - the sole usable head displaces a cached record not in its past, set aside or dropped;
     - a view-ordering contract in clocks for `watch()` and `current_generation()`.
     Then tell Lugh and Lir that `covers()` is how to compare a view with what they wrote.
   - **Power-cut cases for `dagda-migrate`** (Tom, 2026-11-19: after the cutover): route Dagda's raw syncs through
     Dannan's `storage::FileSystem` (`ledger/sealed_journal.cpp`, the durability domain and pool barriers,
     `heads/replica.cpp`, `heads/node_pack.cpp`, `store/local_store.cpp`, `migrate/migrate.cpp`,
     `migrate/manifest.cpp` and its flock, its loose journal, the progress record; `LocalStoreFiles` an adapter), and
     sync directories made before a durable write into their parents (`node.cpp:769`, `storage_pool.cpp:217`,
     `unreferenced_since.cpp:129`, the tries', retention's and the node pack's at open); then cases on Dannan's
     simulated disk (`dannan/test/fixtures/sim/disk.h`) that drop unsynced writes at each step.
   - The rest of harness phase 2, both stores rerun; the slow-disk run (approved 2026-11-16: device-mapper delay over
     a loop file on one Pi, a note on the board).
   - The store map's gaps: the pool's TimedLock (Dannan), and 8c's loop items.
   - Paused with the slices: `0.2.0`'s release steps (`COMPLETED.md`, "Archived from ACTIVE.md, 2026-11-15"), the
     follower's lag (Lugh's P8.5), slice 0's last two figures, slices 2 to 5.

## Waiting on Tom

- The Debian packages and lintian for 0.3.1 (they need Dannan 0.9.2's packages on a shared Pi), if wanted.
- Whether to push the local tags `0.2.0` (held since 2026-11-14) and `0.3.0`.
- Telling Lugh and Lir that 0.3.1 carries slice 1, while the slices pause.

In Macha's list, and Dagda follows: the remaining design defects there (the torrent merge, the fence on actions
under a claim, the local barrier's deadline, wall versus steady ages, the policy disagreement loop, O(state) per
delta, wire-sized allocations, reload ignoring `write_copies`, the I/O and control reserves).

**Written down so it is not lost (Tom, 2026-11-05):** two metadata behaviours heads keeps as Macha's for now,
to change later as a deliberate step, in Macha first: a mutation that changes nothing still commits its clock
(`metadata_manager.cpp:1919` unreachable); a policy commit carries no dot (`:1376`), so after truncation only a
merge can collapse it.

## How work runs

- **No builds, tests or benches on the Mac.** Linux gates run on the shared Pis over ssh:
  - Claim on the board first (`claims_list`, then `claim_take` with only the cores the job's `-j` uses and an honest
    end). No claim is held without a live process (Tom): release the moment a job ends or fails.
  - nice 19 and ionice idle.
  - When none is free, `claim-board wait <hosts> --cores N --job ... --for 90m` as a background command; never poll.
- **Where things are on the Pis** (wiped to bare OS 2026-11-18; toolchains back: GCC 14, clang 19, cmake, ninja,
  Boost 1.83, OpenSSL, GoogleTest 1.16):
  - `/root/dagda-031` on uranus (plain and ASan) and neptune (TSan): 0.3.1's gate, each with its Dannan 0.9.2 prefix
    and `macha-fixtures`. `/root/dagda-030` (0.3.0's) on saturn and neptune; `/root/dagda-idle` (neptune) and
    `/root/dagda-stream` (uranus) built trees with Dannan prefixes, reusable for incremental builds.
  - The harness on jupiter and neptune (and saturn): `/root/harness/build-tip` and `build-030`, links into
    `/root/dagda-*`.
  - **Macha's fixtures:** on those Pis, and the only copy off them in `build/pi-logs/macha-fixtures`.
  - **jupiter and neptune `/home/tom/deps`:** the Macha session's builds; not Dagda's to touch.
  - **uranus:** 4 GB, builds at `-j2`; left out of benchmarks until Tom says.
  - All on WiFi, kernel 6.18.62-v8-va48+, 4K pages; all report past under-voltage.
- **Shipping a commit:** `git archive --format=tar.gz -o build/<name>.tar.gz <commit>` (Dannan's tag the same way
  from `../dannan`), copied with rsync; a gate script like `build/pi-release031.sh` writes `status.txt` (and stops
  with no runs if the build fails: a failed build leaves the old binary). Start it detached in a subshell so the ssh
  returns: `ssh -n host 'cd dir && (setsid nohup sh x.sh > x.out 2>&1 < /dev/null &)'`. Wait on a marker line with a
  background `until ssh -n ... grep -q marker; do sleep 120; done`. Extract a later archive over a built tree with
  `tar -m`, or make sees nothing newer.
- **Test-first is shown on the Pi:** build at the commit that adds the cases and see them fail, then build the fix
  and run the whole suite once. No rerun without a named reason. Sanitizers only for a release or to hunt one named
  bug.
- **Measurements are like for like:** old binaries against new in one session, the link recorded each round.
  `TRACE=1` runs find mechanisms and are never figures (`build/harness/holder_align.py` lines up a trace by wall).
- **Local tools:** the hook blocks grep, sed, awk, head, tail and cat over local files. Use basemind (`code` mode
  grep, outline, find) and the Read tool; a remote grep over ssh is fine.
- **The other sessions:**
  - **Macha Server:** the port; it receives every gated commit and sends Dagda-caused failures with their mechanism.
  - **Dannan:** the network layer; Dagda never edits `../dannan`, `../macha`, `../lugh` or `../lir`.
  - Tom's relayed words come through them, but a commit, tag or push waits for Tom's own word in this session. A
    Macha defect is sent to the Macha session before Dagda deviates from Macha's behaviour.

## Open

- The heap aborts in the opt-in delete stress case (no mechanism found; no stress runs, Tom 2026-11-09).
- The `mutex lock failed` abort of one 2026-11-05 macOS TSan run.
- Dagda's four-reader local reads come out in two clusters (about 344 MB/s twice in six runs, else 363-407), where
  Macha's sit at 379-388 bar one outlier; the off-CPU profile shows no wait of Dagda's own.
- The pool's `get` stopping at the first backend's empty answer: Macha's, kept.
- The trie pins aged on the node's time source where Macha reads the real clock (the same on live nodes).
- The Pis' under-voltage under heavy builds: their supplies are Tom's to look at.
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

**Tom, 2026-11-20, in this repository's session:** the cutover is no longer Dagda's concern: "Macha keys on version
for cutover, it's no longer your problem"; "Cutover has ceased to be your concern." This supersedes 2026-11-18's
"work on nothing else": Macha builds on 0.3.2 by version, and Dagda's queue and the hosted systems' asks are open
again. On Louhi's asks (a new hosted system: its records in an every-node head space under `louhi/`, on 0.3.2):
**a node is caught up when its view's clock covers the clocks its active peers advertise for the space**, not a
timer or a flag. Context v2 reads and prefix listings, and FetchContent beside a fetched Dannan, are ordinary work.
Then, the same day, three answers:
- **The durable AcceptorStore** for `dannan::agree` goes in Dagda's next release, as relayed by Louhi.
- **Peers' heads for caught up come from a survey** of the peers holding the space, not an advert. The ruling of
  2026-11-13 stands: adverts carry slow facts only.
- **A feature is built if Louhi and at least one other project need it.** In Louhi's session: "Dagda only does
  this if other projects that use Dagda can use this feature."
  - So the ruled register map (D6) comes into the next release: Lir's retained, Lugh's JWKS, the catalogue and the
    leases use it.
  - Its two possible additions, an owner's ordering stamp and a per-record value-merge hook, go in only if Lir or
    Lugh needs one (asked).

The rulings of 2026-11-13 and 2026-11-14 are in the design's "Ruled" section; those below stand where it does not
supersede them.

**Tom, 2026-11-18 and 19, in this repository's session:** a read whose ranked owners and fallbacks are spent asks
the active peers without the extents advert (chosen over a test-only fix in Macha); then "yes, go": commit the fix,
run the release sanitizers, merge, cut `0.3.0` for Macha's deploy, tell the Macha session. Nothing pushed. On the
large rejoin, the fix where a reply that leaves ids sends the next fetch at once ("yes"), and "clean things up":
the merged local branches deleted (`feature/bench`, `feature/dannan-070`, `feature/macha-store-gaps`,
`feature/read-memory`, `feature/slice1`, `release/0.2.0`, which the tag keeps), `build/` cut from 10.2 GB to 0.76 GB
(the Mac build and sanitizer trees, bundles, the bench's stores; every log, script and note kept). Then, (b) having
measured no faster because the joiner kept to `idle_bandwidth_fraction` 0.5: **"Let idle repair use the link"**:
with nothing of a higher class present, repair's credit accrues at the whole link estimate; the fraction applies
only while one is, and the duty share still holds repair's turns. The option keeps its name for Macha's mapping.
Asked after: 0.3.1 "Wait"; the migration's power-cut cases (syncs through Dannan's `storage::FileSystem`, cases on
its simulated disk) "after the cutover as planned".

**Tom, 2026-11-18:** the Macha move to Dagda only, until ready for the cutover on macnessa and inverbeg (above). Agreed
among the three sessions the same day: the cutover deploys on Dannan 0.9.2. The Macha session agreed that Dagda fixes
the arbiter's lost wake in Dagda (Macha's arbiter goes at the cutover) and chose its shape: wake when the refusal can
end, never poll.

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

**Tom, 2026-11-15:** acknowledgement levels are the stack's standard: ephemeral, persisted, decided (Dannan's
agreement), replicated (a majority of the replication set); the setting is `ack_level`; Dagda's durable stays beside
them until Tom says otherwise. Agreement waits for Dannan: Dagda builds no consensus or stand-in. The header tagline is
"Dagda - The Distributed Object Store".

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
