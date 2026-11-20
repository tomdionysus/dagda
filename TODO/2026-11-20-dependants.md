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
   - **Reconcile:** Mimir names "Claims retain and release". Dagda has no release call. Objects are kept by
     what a head names and freed by the collector. So the checkpoint head should be a D6 register map with one key
     per shard whose refs name its chunks. Mimir then tracks nothing, as for Lir and Kvasir. To confirm with Mimir.
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

## Awaited

Macha, Lugh, Kvasir, Lir.
