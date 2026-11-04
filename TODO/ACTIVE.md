# Dagda - Active Work

Last updated: 2026-11-04, night. `develop` is pushed to
`git@github.com:tomdionysus/dagda.git`, on Dannan 0.4.1, gate
`dagda_tests` 389 (1 disabled). Steps 1 to 3, 5, the parts of 6 and 7
that need no heads, 6b (`dagda-migrate`) and 7b (maintenance in the node,
the instant delete) are done (`COMPLETED.md`). What remains waits on
Macha: step 4 (heads) on its tree-native merge, which completes and
freezes `dagda::Context` v1; 6b's gate on the Macha session's v18
fixture; step 8 (the port) on the maintainer.

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

## The port respec (from the Macha session's audit, 2026-11-04 night)

The maintainer's instruction, relayed by the Macha session: Macha is the source of
truth for the port; where Dagda differs from Macha's behaviour, Dagda
changes. The evidence is
`../macha/TODO/2026-11-04-dannan-dagda-port-evaluation.md` (sections 4,
5.1, 5.3). Each item below was checked against the code here and in Macha.

Order: M2, M3, M4, M8, M9, M6, M5, M10; M1 when Macha merges.

Done so far: M2's ledgers by name, every refusal before the first write,
and the node id rule (`dagda_tests` 392). The Macha session accepted the
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
  `test_replication_policy_change_on_restart` moved.
- **M6. The last survey persisted** (`availability/last-survey.bin`) and
  re-asked on a peer's advert changes.
- **M7. Torrent-request known answers.** Held for the maintainer: Waiting on the maintainer.
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
slots kept; a gate run on live-a, then merge and deploy to both nodes.
The operator may put the LocalStore race fixes first. Genesis and the
final tree forms are settled only at that merge, so Dagda's heads move
from the merged code; the Macha session will say exactly what changed.
Steps 5 and the parts of 6 that need no heads go first.

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

(the maintainer, 2026-11-04 night, relayed by the Macha session) **A genuine Macha
defect is fixed in Macha first, and the libraries match Macha's fixed
behaviour.** Dagda's fixes to the five LocalStore races, the claim
check-then-delete windows and the repair list identity stand, and Macha
adopts them. No further deviation on the grounds of a Macha defect without
sending it to the Macha session first. Where a fix changes a format or an
answer, the maintainer rules on it in Macha and Dagda takes the result.

The maintainer's, to the Dagda session, 2026-11-03 night:

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

From the port respec (2026-11-04 night), recommendations in the session:

1. M7: the torrent-request known answers against "no ingest, torrent or
   import code, in the library or its tests" (Decisions, 2026-11-04).
2. M2: does the reverse migration gate the first real migration
   (the audit's own question 3)?
3. M2: whether a migration reads every byte, or derives tags without
   reading and leaves rot to the scrub.
4. M10: which Linux machine runs the tests.

Earlier: the seven items of 2026-11-04 were all answered that evening
(see Decisions, "the maintainer, 2026-11-04 evening") and done the same evening
(`COMPLETED.md`, "the decided fixes"): the control store's 256 MiB
reserve, the refused missing marker, the fresh future tombstone and the
refused unkeyed delta record. The device reserve was already
`IoReserveOptions` with those defaults; an operator reaches it through the
node's options at step 7. For Macha at the port (8b): the tombstone and
the unkeyed-record fixes, the same.

**What blocks further work** (needs Macha):
- Step 4 (heads), and with it retained values and the heads part of
  `dagda::Context` (step 7), wait on Macha's tree-native branch merging
  (its order is the operator's). The parts of steps 6 and 7 that need
  no head are done.

**For the domain catalogue (step 6, with heads):** whether the wrap advert
keeps its certificate. An advert is one gossiped value, so it cannot carry
the certificate for some peers and not others. Either it stays and a
verifier prefers the certificate Dannan's `Nodes::certificate` proved, or
it goes and a verifier that has never met the peer calls it first.
Nothing outside the tests verifies an advert until the catalogue does, so
this is decided with that caller, with the certificate's real size in
gossip in hand. Recommended then: keep it, prefer the proved one.

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
