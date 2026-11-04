# Dagda - Active Work

Last updated: 2026-11-03, night. The repository has had `git init` and
holds no commits. Dagda's plan is not set: steps 7 and 8 of
`../dannan/TODO/ACTIVE.md` moved here (the maintainer, 2026-11-03 night: "Dagda is
its own project, with its own agent") and wait on the maintainer for their order
and first step. `COMPLETED.md` is the record once there is one.

Dagda's specification is
[`2026-11-03-macha-store-requirements.md`](2026-11-03-macha-store-requirements.md):
Macha's store as it is (develop `5a06d3c` plus the tree-native branch's
direction, tree form only), 92 requirement rows by subsystem, the formats,
the FS seams to cut, the port steps, what not to reproduce, and open
questions for the maintainer (its section 8).

The design is the maintainer's, 2026-11-02: "Dannan: Distributed Substrate Library
Plan" (the store sections) and "Macha Core Extraction Plan", both Claude
Docs and the masters, rendered in `../dannan/TODO/2026-11-02-*.md`. The
store rows of `../dannan/TODO/2026-11-03-macha-function-inventory.md`
say what Macha does today; they do not decide Dagda's design.

How Dannan, Dagda and Lugh divide the work and fit as a stack is
[`2026-11-03-stack.md`](2026-11-03-stack.md), the one copy for all three
projects, with recommendations for the maintainer.

## Dannan, as Dagda finds it (from the Dannan session, 2026-11-03 night)

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

## The plan (approved by the maintainer, 2026-11-04)

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
5. **Leases** (`lease/`). The lease and the claimed-collection template,
   field groups with their own clocks, the epoch fence (new: Macha never
   checks it); `TorrentRequest` as the first instance, bytes identical.
   Gate: the torrent-request cases and a byte round trip against an
   SM16/17/19 and DLT9 fixture.
6. **What Lugh and Lir need beyond Macha.** Domains in full (several,
   member and blind roles, keys wrapped to node certificates once Dannan
   step 10 has them, the domain in the AAD for new domains); node-local
   spaces under `KeyCustody::derive("dagda/node-local/1")`; the
   Replicated level's per-space k; retained values on Dannan's notify
   channels; the scope check through `Identity::verify`; binding and
   no-rollback tests. Gate: each from the stack document's requirements.
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
  torrent-request bytes, `macha-metadata-dump` output): it produces them
  in its own way and tells Dagda where they are. The Dagda session never
  builds or runs Macha. **Order (the maintainer, to the Macha session, 2026-11-04):
  Macha's tree-native branch merges first, then the fixtures are cut
  from the merged code**, genesis as a constant hash included, so none
  needs regenerating. Generator under `build/macha-fixtures/gen/`;
  output per step under `build/macha-fixtures/step<n>-*/` with a README
  naming the Macha commit and the config the node ran with (packing
  threshold, backend limits). Step 1's code can move before its fixture
  exists; its gate cannot close until it does.

## Hosted systems waiting on Dagda

- Lugh (`../lugh`, the identity/SSO server; the maintainer has told it to align with
  and use Dannan and Dagda). Its store today is one sealed log per table
  (14 tables), version-conditional puts, kept-forever tombstones, unique
  secondary indexes rebuilt at open, per-table DEKs under a node master
  key. Under the maintainer's rules (2026-11-03 night) Lugh uses Dagda for
  everything it persists, shared and node-local, and deletes its store
  and keyring: **Lugh's persistence waits on Dagda entirely.** Until
  then Lugh keeps its store and keyring frozen and deletes them the day
  Dagda carries its spaces (the maintainer, in the Lugh session: "Keep the store
  until Dagda"). Its
  requirements on Dagda (node-local spaces; binding to domain and object
  id; no rollback of a head space) are in the stack document. Lugh takes
  Dannan's callback shape, so Dagda's Context does too. Recommended to
  The maintainer: Dagda's first step is Lugh's slice, single-node first (stack
  document, "Waiting on the maintainer" 1).

## Decisions

The maintainer's, to the Dagda session, 2026-11-03 night:

- **Dagda replaces Macha's DHT** by extracting how Macha's store works and
  generalising it. Macha's needs come first; Lugh's and Lir's are
  generalisations.
- **Move the store code** from Macha into Dagda and generalise it in
  place, bearing in mind it runs on Dannan: Macha's transport, crypto,
  durable files, logging and time give way to Dannan's as it arrives.
- **Formats stay compatible**, "because they work": layout v18 and the
  store encodings, byte for byte. Small values must be handled well too.
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

Open, none blocking (each has a default that keeps Macha's behaviour):

1. **The control store's free-space reserve** defaults to Macha's 0. A
   figure above 0 keeps CONTROL writable after DATA's reserve stops DATA
   writes on a shared filesystem. Recommended: a small reserve (for
   example 256 MiB).
2. **A missing `retention/ledger` marker over non-empty claims tries**:
   Macha rebuilds both tries empty and replays the journal, so every
   checkpointed claim is lost (`retention.cpp:383-400`, `:457-458` at
   develop; shown by reading, not by a test). Dagda does the same for
   now. Recommended: refuse to open and say why (discipline 3), since a
   lost claim lets GC remove what someone still needs.
3. **Dannan 0.3.0**: Dannan's identity step, `contract::Unlocked` and the
   helpers Dagda asked for are on its `develop`, untagged. Dagda moves its
   pin when the maintainer cuts 0.3.0 (Dagda's scope check and domains need it from
   step 6).

4. **The device reserve's figures and scope** (step 3b): one reserve per
   node over every device its stores use, 8 operations in flight of
   which 2 are kept for control work. Recommended: keep these until
   measured on fi-1 under an import, then make the reserve per device
   only if a node's state path and DATA backends are on different
   disks there (Dagda cannot tell which share a device today).

Settled earlier: the plan (approved 2026-11-04), "Dagda - The Store",
Dagda's own version number from `0.1.0` pinning a Dannan tag, Dannan
0.2.0, free message registration from 512, byte-for-byte compatibility.
Decided by the Dagda session under the rules: the store's layout marker
is Dagda's; the gates' and observations' "catalogue" wording stays as
Macha's until Macha no longer reads it (its Status must stay
byte-identical through the port).
