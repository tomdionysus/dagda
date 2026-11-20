# Dagda's dependants: what each uses and needs

The maintainer, 2026-11-20, to the stack: "Every project that is a dependency should follow suite - confirm with their
dependants that the functionality they need is covered."
- Asked of Macha, Lugh, Louhi, Kvasir, Lir and Mimir: A. what they use today; B. per item, the calls, what it
  blocks, the test that proves it, and the shape's misfits; C. their constraints.
- Reconciled here, then sent back for confirmation before code that changes a shape.

## Louhi (2026-11-20)

A. **Nothing called yet.** It fetches 0.3.2 (`EXCLUDE_FROM_ALL`, Dannan `OVERRIDE_FIND_PACKAGE`) and links
`dagda::dagda`. Only the fetch and link can break.

B. Needs:
1. **D6.**
   - `RegisterMap::spec("louhi/records", {stamp: hlc, tombstones: never})` through `Context::heads().open`.
   - Several puts and removes per commit, the outcome naming each key's version.
   - `get` and `list` over `local()`; `changed(before, after, prefix, ...)` on each watch notice.
   - Done when Louhi's store_test passes on a two-node `dagda::test::Cluster`, with a write on one node read and
     watched on the other.
   - Shape as recorded.
2. **Caught-up.** The query plus a notice; it acts only on `caught_up`.
   - Done when a joiner reports false until it has folded the peers' heads, then true with one notice.
3. **The durable AcceptorStore** for space "louhi", served through Dannan's `Context::agreement().serve(...)`.
   - Small values (revision counter about 5 KiB; admission set up to about 240 KiB), no deltas.
   - Done when louhi_admit's simulation cases pass with Dagda's store under a restart.
   - **Reconcile:** 240 KiB is under the store's value bound only if the AcceptorStore's own bound is set above
     it. That is its own log, not D6's 64 KiB. To state in its shape.
4. **FetchContent** without the override.
5. **Store health** into Dannan's readiness, consumed by Dannan's placement.
6. **Retire and drain:** through Dannan; nothing further.

Not used: Objects directly, Claims, Leases or D5, Tickets, Control, Devices, Activity, D9, D1/D2, the catalogue,
Macha's items.

C. Constraints:
- It calls from io threads through per-component strands, and from API handlers. Completions are never inline and
  every call has a deadline.
- Reads come from its own cache, refreshed on notices.
- Writes take 5-30 s.
- Keys up to about 120 bytes and values under 64 KiB; 10^4 keys now, 10^5-10^6 planned; one space.
- Acknowledged at the local commit.

## Mimir (2026-11-20; ruling 43: Mimir's persistence is Dagda's)

A. **Nothing yet.** Stage 5 starts on Dagda's release current then.

B. Needs:
1. **The durable AcceptorStore, value-less.**
   - `DurableAcceptorStore(context, space, Level{persisted|replicated}, values = none)`.
   - `load()` answers `has_value` with no bytes, and `value()` is delegated to Mimir by a `ValueSource` callback
     given at construction.
   - Promise, ballot, summary and delta in one append, grouped across slots.
   - need_whole: all or nothing, idempotent at the ballot, only when the base is truly absent.
   - Fenced; not ready until catch_up; completions posted; owner-called `checkpoint(slot or space, up_to)` and
     `truncate`.
   - Done when a three-node cluster at replicated survives SIGKILL of every node with every acknowledged write
     read back, and a restored older log refuses to open.
2. **Value checkpoints:** chunks as Objects, and one small head per space naming each shard's latest checkpoint.
   - **Reconciled (Mimir agreed):** the checkpoint head is a D6 register map.
     - Key `<node id>/<shard>`, since replicas checkpoint at different versions and each node needs its own values
       back. Its refs name the chunks, and its value holds the agreed versions per range.
     - Sequence: chunks written, put (durable on this node at completion), then store checkpoint(up_to) and
       truncate.
     - The collector frees superseded chunks. A retired node's keys are removed by the retire, or by Mimir as it
       leaves.
     - So D6 is used by Mimir: put, get_register, remove; one writer per key.
3. **Caught-up:** a per-node query over the checkpoint space; no notice.
4. **D9 logs** for its merge class (stage 4), possibly. It prefers one log shared with the AcceptorStore.
   Shaped at stage 4.
5. **FetchContent**, tools off.

Not used: D6 key reads, D5, Leases, Tickets, Control, Devices, Activity, retire, Macha's items, D1/D2, the catalogue.

C. Constraints:
- It calls from Dannan's agreement executor; checkpoints run as speculative-class work.
- keep() within 5 s at replicated; a checkpoint may take minutes but is interruptible.
- Up to 10^6 names per space per node; values held by Mimir; one space with 8192 slots; 3-32 nodes.
- 10^4-10^5 keeps a second per node, group committed.
- Levels: ephemeral never touches Dagda; persisted is the node's append synced; replicated is a majority's.

## Lugh (2026-11-20; pin 0.3.2 fetched beside Dannan 0.9.2, feature/p8-0-dagda f464747)

A. **Tests only** (P8.0's rig, `lugh/test/fixtures/cluster_rig.h`).
- `context.h` Heads: `id_of`; `read(space, HeadRead::local, deadline)` (blocking, from the test thread);
  `mutate_delta(space, factory, deadline)` returning `MutationEvidence`; `watch`/`unwatch`, `HeadWatch`;
  `HeadReadEvidence`.
- `heads/space.h` and `record.h`: `SpaceView`, `HeadMoved`, `HeadIdentity` and its `==`.
- **The installed fixture:**
  - `cluster.h`: `Cluster` add, start_all, converged, dead_to, io().context, operator[]; `ClusterNode`
    start, stop, restart, id, name, api; `ClusterNodeOptions` spaces, exchange.joiner, maintenance.automatic,
    interval, gc_interval;
  - `heads_space.h`: founded_spec, neutral_mutation, `NeutralEditing::set_note`;
  - `heads.h`: `NeutralCodec::as_head(...)->notes`.
  **Kept stable** until Lugh moves onto D6 (P8.2 on).
- Production: nothing yet. P8.1 opens `dagda::Node` in lugh-node.

B. Needs:
- **D6.** `RegisterMap::spec(name, {stamp, never, tombstone_wins})`: put and remove with expect, get, get_register,
  list, changed.
  - Spaces: lugh/scope, lugh/secrets, JWKS, plus node-local ones.
  - Blocks P8.2-P8.4.
  - Done when Lugh's merge cases pass on it: a removal beats a newer concurrent edit; two `alice`s from a split are
    both kept (Lugh's index marks them ambiguous); a stale expect conflicts; a put onto a tombstone is refused.
- **New: node-local register-map spaces with per-key expiry**, for OAuth's short-lived state (codes, PAR URIs,
  device grants, replay caches, handles, pending logouts).
  - A code's single use is a conditional remove expecting the version read.
  - Confirmed here: D6 opens under node_local (Space supports it). On one node's own space the conditional write
    is exact, with no other writer.
  - Done when a code redeemed once is refused again, across a restart too, and an expired PAR URI reads absent.
- **Back-off (P8.6):** an every-node register map keyed `<account>/<node>`, each node writing its own slot, with
  expiry of about an hour giving the forgetting. Lugh sums on read. Expiry is judged on the reading node's clock;
  an hour suits.
- **Durable AcceptorStore:** TOTP last step, recovery codes, key rotation, and POSIX numbers (P8.3).
  - Blocks P8.3 and P8.6, with no stand-in (the maintainer: no interim).
  - Done when a TOTP step accepted before a restart is refused after it.
- **Caught-up:** acts on caught_up only, with the evidence in Status and audit.
  - Blocks /ready (P8.1) and P8.3's gates.
  - Done when a node behind a peer answers 503 to sign-in until caught up.
- **Key reads and changed:** for the name and email indexes.
- **FetchContent:** configures and builds today on 0.3.2 with the override.
  - Done when Lugh's install with `DAGDA_BUILD_TOOLS=OFF` carries libdagda and no dagda-migrate or man page.
- **Retire:** indirectly; a retired node's admission to lugh/secrets goes with it.
- **Store health:** through Dannan's /ready.
- **D9 (P8.7, audit chain):** a node-local log.
  - append returning its position; read from a cursor in pages; retire up to a cursor once exported; a byte bound
    that refuses an append (never drops); continuous export from a cursor.
  - Done when an append at the bound is refused, exported entries retire, and a restart keeps the cursor and
    entries.
- **D1/D2 (slice 2):** lugh/scope on every node, lugh/secrets on capable(lugh).
  - Blocks P8.5. The maintainer's bound: a disable seen on another Pi within about a second, with "applied, n nodes pending"
    until then.
- **The catalogue's readers (slice 5):** L-2; config list until then.

Not used: D5 (the maintainer replaced Lugh's leases with agreement registers), Macha's items, DataWorkContext, group commit.

C. Constraints:
- Asynchronous forms from io threads with Lugh's executor; never a blocking form on an io thread; completions
  posted.
- Decisions read within about 2 s; writes up to 10 s.
- Keys under 128 bytes; values up to 64 KiB (most under 4 KiB); scope and secrets about 10^5 keys each; node-local
  OAuth state about 10^4 keys expiring in minutes; audit entries about 1 KiB, bound about 1 GiB.
- Levels:
  - directory and credential changes: replicated;
  - OAuth state, audit and back-off: persisted locally;
  - TOTP and rotation: decided, through agreement.
- Never blocks: token verification and the sign-in hot path.

## Kvasir (2026-11-20; pin 0.3.2 on Dannan 0.9.2; a leaderless cluster that may be one node)

**Changed:** a SQL commit is made durable by agreement on the range's register, with the durable AcceptorStore
as its log, from step 2b. The D6 manifest is only folding's output. A manifest commit per transaction would have
needed one writer per range, which is single-node code. So 2b needs the AcceptorStore and D6.

A. Nothing linked yet.

A'. At 2b:
- `Node` and `NodeOptions`;
- `Context` Objects (write at persisted, read, remove), `Claims::retain` (restore window, step 5), Heads (open,
  read local, mutate, watch);
- `SpaceSpec`/`Policy` every-node;
- D6 (`RegisterMap::spec`, put and remove with expect, get, get_register, list, changed, refs);
- the durable AcceptorStore;
- the Cluster fixture.

B. Needs:
- **D6.**
  - Space "kvasir/manifests", key `manifest/<table>/<range>` (never reused, so the window policy is fine), value a
    header of a few KiB.
  - **refs up to 1,024 per register**: about 4 snapshot chunks plus fewer than 256 unfolded deltas.
  - put with expect by the range's folder, several ranges per commit; list at start; changed on notices. HLC only.
  - Done when a fold's crash between chunks and manifest leaves the old manifest, with the new chunks collected, and
    a stale folder's put is refused.
  - **Reconcile:** refs are a register's child records, not its inline bytes. Above the tree's
    `child_inline_max` (8) they go to a child sequence of their own (`TreeChildWriter`), so a leaf stays small
    however many refs a register names. The bound is stated as **4,096 refs per register**, separate from the
    64 KiB value bound. To add to D6's design.
- **Durable AcceptorStore**, values held, a level per agreement space; its recorded needs stand.
  - Done when, on SimNodes with SimDisk, a kill mid group commit loses no acknowledged keep, load replays to the
    tip, and a restored older segment refuses to open.
- **Caught-up** on "kvasir/manifests": a query with a deadline and a watch; it acts on caught_up only, deferring
  releases and drops.
- **FetchContent** with Dannan fetched without `OVERRIDE_FIND_PACKAGE`: "if `TARGET dannan::dannan` exists, use
  it", with tools, testing and install off as a subproject. That is section 3's plan.
  - Done when Kvasir's GitHub CI configures and links.
- **Retire and drain** (step 4): drain's start as a notice, so Kvasir moves ranges off first.
- **Store health** into readiness: "Dagda store ready".
- **A set-aside manifest head** reported, never dropped.
- **Group commit:** one fsync per batch across slots and promises, batching 10-20 ms per range.
- **D1/D2:** chunk copies per the table's replica count (ruling 29), and the manifest space at k-level before old
  chunks are released (step 4).
- **N1, objects at persisted in a batch with one sync.**
  - **Reconcile:** this exists in contract v2. `Objects::put_deferred_here(id, bytes)` writes each object here
    with its sync deferred, and `Tickets::wait(tickets, deadline)` makes them all durable with one barrier per
    place. A fold writes its chunks deferred, waits once, then puts the manifest.

Not used: store/*, Leases, D5, Tickets beyond N1, Control, Devices, Activity, D9 (maybe at step 6), Macha's items,
the catalogue.

C. Constraints:
- Calls from a range's strand; completions posted; nothing blocks an io thread.
- A keep answers within the group window plus one fsync.
- Objects up to 16 MiB; acceptor values up to 256 MiB, mostly KiB-to-MiB deltas; about 16,000 ranges a TB;
  1 to dozens of nodes.
- Replicated means a majority of acceptor keeps, through agreement, not D2.

## Macha (2026-11-20; 0.3.2, port/dannan-dagda)

A. **The widest user: the set not to break.**
- `node.h`:
  - `Node`, `NodeOptions`, `NodeReload`, `AdmissionOptions`, `LocalTurnOptions`, `MaintenanceEvents`,
    `Node::OpenLater`;
  - `open_store`, `stop`, `context`, `server().set_hosts_extents`, `data`, `control`, `cache`, `claims`, `ledger`;
  - the collector's steps (inventory, tombstone, release, control, data), `begin_local_stages`,
    `local_rebalance`, `retention_compact`;
  - the options in `src/cluster/store_node.cpp:43-131`: heads off, automatic maintenance off, admission's device
    pressure, and the rest.
- `context.h`:
  - Objects: write, read, put_here, put_deferred_here, has, cache_local, hydrate, hydration_available,
    locally_available, reachability_epoch, logical_capacity. Not remove.
  - Claims: retain_data_here, and retain(class, ids, dot, {}, deadline, here or every_node).
  - Tickets::wait.
  - Control: put, get, has, ensure_local, offer.
  - Devices::admit_data.
  - Activity.
  - No Heads.
- `store/`:
  - `LocalStore` and its options;
  - Write/ReadOptions, ReadEnd, Durability, ReplicaEvidence, BarrierLoss, ObjectData, WriteEvidence,
    StorageUnavailable;
  - repair's lists, gate, settled, readings and diagnostics; DataResourceStats; figures and adverts;
    AvailabilitySnapshot.
- `ledger/`: ObjectTrie::Snapshot, ReferenceCounts, InventoryStep, PassDue.
- `contract/`: retention types, the reachability provider and its horizons, ObjectLedger.
- `domain/`: `StorageKey::derive_v1`, `domain_v1`.
- Tests use the store's message registration and figures; no Dagda fixtures.
- `dagda-migrate` is run by the cutover kit and must stay buildable from the tag.

B. Needs:
- **D6** for users and sessions (replacing Macha's gossip, unless sign-on moves to Lugh first), and later ingest
  jobs and cluster policy.
  - **Uniqueness of usernames is not D6's:** the conditional write is one node's view. That needs an agreement
    register, as Lugh does. Moot if sign-on moves to Lugh.
- **D5** for torrent claims, the catalogue scanner and the tombstone sweeper, each among capable nodes.
  - **Fencing is required:** a deposed holder's write to the record must be refused or lose. So D5's design states
    how the lease epoch fences writes: the record's writes carry the epoch, and a write expecting an older epoch
    is refused.
  - Case: the holder is killed mid-job, another takes it within the lease, and the old holder's late write cannot
    win.
- **Caught-up** for formation and FUSE's initial-namespace wait.
  - Macha asked for "no peers" distinct from "caught up". The group settled on one uniform answer (the maintainer's
    no-single-node rule), with founding as Dannan's explicit once-only act (T4). To tell Macha.
- **Key reads and changed:** once its metadata is on heads; changed() may serve FUSE's adoption diffs.
- **Retire and drain** (new to Macha). Case: retire a node holding sole copies; every object is elsewhere first,
  and health is ok after.
- **Store health** into readiness, replacing `data_storage_degraded`.
- **A set-aside head never dropped silently** (Macha only warns at expiry, `metadata_manager.cpp:966`).
- **Space taking Macha's rules and the heads migration:** the cache ordering with the own-commit refusal and the
  sole-head displacement (b0b037c), own_head, mutate_delta's identity and dots, the publication claims barrier
  order, history truncation, and set-aside. FUSE's adoption rules stay Macha's.
- **DataWorkContext co-owning:** Macha passes `shared_ptr<const atomic_uint64_t>` through `WriteOptions.progress`.
- **Group commit**, keeping payload files synced before the shared descriptor.
- **D1/D2:** possibly; unverified.

Not used: the durable AcceptorStore, FetchContent (CI installs the tag), D9, the catalogue's readers.

C. Constraints:
- Called from FUSE publication and extent workers, the maintenance thread, HTTP and playback readers, the
  hydrator, libtorrent disk I/O (admit_data in 1 s slices), ingest and catalogue workers, the claims barrier and
  the store-open recovery.
- Every write at persisted, made durable by a tickets wait at the barrier; a BarrierLoss re-puts.
- DATA no-progress 120 s; a viewer reserve of 32 of 128 MiB, never pressure-gated.
- Extent 4 MiB live; packing 1 MiB into 64 MiB; control store 8 GiB; garbage_grace 30 days; 1.54 M and 0.90 M
  loose objects.
- Never blocks: control work, viewer reads, FUSE request threads, metadata reads.

## Awaited

Lir.
