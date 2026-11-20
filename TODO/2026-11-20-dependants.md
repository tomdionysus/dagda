# Dagda's dependants: what each uses and needs

Tom, 2026-11-20, to the stack: "Every project that is a dependency should follow suite - confirm with their
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
  - Blocks P8.3 and P8.6, with no stand-in (Tom: no interim).
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
  - Blocks P8.5. Tom's bound: a disable seen on another Pi within about a second, with "applied, n nodes pending"
    until then.
- **The catalogue's readers (slice 5):** L-2; config list until then.

Not used: D5 (Tom replaced Lugh's leases with agreement registers), Macha's items, DataWorkContext, group commit.

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

## Awaited

Macha, Kvasir, Lir.
