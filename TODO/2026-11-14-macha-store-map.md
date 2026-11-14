# Macha's store onto dagda::Node and Context: the call-by-call map (2026-11-14)

## The bar (Tom, 2026-11-14, to the Dagda session)

"Port through dagda::Node and Context is correct, but you must make ABSOLUTELY SURE that Macha can perform exactly as
well as it did before with Dagda as its storage backend. It is not enough for the concepts and primitives to be
'sorta' like Macha."

"There must be no 'shims' in Macha to make Dagda work for it. Both: parts of Macha may need to be rewritten to use
native Dagda effectively, and that's the NO LOSS of performance/reliability/etc - AND - Dagda MUST be capable of
handling DHT storage AT LEAST AS WELL as Macha."

So two requirements, both binding: Macha uses Dagda natively, with no adapter layer, rewritten where it must be and
losing nothing; and Dagda, measured on its own terms against Macha's store on the same workloads, is at least as good
a DHT store in every respect.

## The route (Tom, 2026-11-14)

Most of Macha's store calls (a write with a known id, put_here, deferred writes and many-ticket waits, hydrate,
cache_local, ensure_local, the activity clocks, reads with no deadline on the caller's thread, the collector's steps,
the retirement helpers, claimed collections) were reachable only through `Node`'s parts (`node.h:359`, "for an owner's
maintenance and for tests"); `Context` offered a content-addressed write and read with a required deadline, `has`,
`remove` and one ticket's wait, run on the store's four worker threads. **Ruled: `Context` grows to serve all of it,
with Macha's semantics** (a blocking form on the caller's own thread, no worker hop; a read with no deadline under the
admission window; many-ticket waits; known-id writes), and Macha calls `Context` only: one contract for Macha, Lugh and
Lir, contract version 2. Every row below whose Dagda side is a `Node` part is a gap until `Context` serves it.

## Method

Against Macha's develop `f0597a4` (0.94.2's code), extracted read-only at `build/macha-f0597a4/`. Written with the Macha
session, split by subsystem (agreed 2026-11-14): Macha writes the callers' side (filesystem and FUSE, catalogue and
hints, metadata, the namespace control store, ingest, torrent, media, HTTP and status, the composition root); Dagda
writes the store's side, below. Each reviews the other's against code, not against a summary. The Macha session's
`build/store-api-map.md` is a starting point only: its verdicts are unverified (four were wrong).

Every row states what Macha's callers rely on (inputs, results, errors, durability, ordering, threads, deadlines, the
log lines and figures operators and tests read), the Dagda call through `dagda::Node` or `Context` that serves it, and
a verdict: SAME, DIFFERS (exactly how) or MISSING, with file:line on both sides and the cases that pin it. Every gap is
fixed in Dagda, test-first (Tom: "fix gaps as found"), and the map records the fix. Nothing is inferred; what could not
be verified says so.

The map is the first proof. The others (Tom, 2026-11-14): Macha's own suites as the gate on Dagda; the side-by-side
harness on the Pis (Macha's store and Dagda on identical workloads), then the port on macnessa and inverbeg against
Macha's live baselines; the in-place migration on copies of real state; reliability under crash, restart and
partition. Key custody: Macha's nodes get Dannan key custody at the cutover.

## The store's side (Dagda's), by subsystem

| Subsystem | Section | State |
|---|---|---|
| Subsystem | Section | Rows (SAME / DIFFERS / MISSING) | Gaps |
|---|---|---|---|
| DistributedStore: put, get, deferred writes and tickets, the barrier, write copies, durability, reads and the server's handlers | `macha-store-map/distributed-store.md` | 41 (17 / 23 / 1) | 15 |
| StoragePool and LocalStore: durability domain, presence index, sealed journal, layout, device reserve, backends, scrub | `macha-store-map/pool-and-local-store.md` | 58 (43 / 13 / 2) | 9 |
| Retention, claims and reference counts; the ledgers (held ledger, object trie, trie diff and union, sightings, horizon) | `macha-store-map/retention-and-ledgers.md` | 46 (26 / 20 / 0) | 10 |
| Availability and the survey, repair, maintenance's store stages and pacing (node loop) | `macha-store-map/availability-repair-maintenance.md` | 43 (23 / 15 / 5) | 13 |
| GC and the deletion path (claim_deletion, the collector, tombstones) | `macha-store-map/gc-and-deletion.md` | 29 (15 / 12 / 2) | 9 |
| Cache, io pressure, data credit and admission, the device reserve | `macha-store-map/cache-pressure-credit.md` | 50 (36 / 13 / 1) | 8 |
| **All six (first pass, by six readers, 2026-11-14)** | | **267 (160 / 96 / 11)** | **64** |

Each section's claims are the readers'; the sharpest are checked against code before a fix, and the ones checked so far
held: the ticket cap (`distributed_store.h:276`, `.cpp:711-719`), Context's reach (`node.h:359`), the GC loop's
dropped `!metadata_dirty` (`node.cpp:552` against Macha `maintenance.cpp:575`), serial DATA claims (`distributed_store.cpp:2256-2260`
against Macha's `std::async` per node, `:924-954`), and the backend lock creating a missing mount's directory (Dannan's
`StateLock`, `node_identity.cpp:25`).

## The callers' side (Macha's)

Written by the Macha session after its heap-corruption investigation (its standing P0).

## Gaps and their fixes

The order: data safety first, then what blocks the port, then what loses performance, then the cosmetic. Each is fixed
test-first; its case and commit go here when it lands.

Data safety and correctness:
1. A missing backend directory is created by the backend lock and can be adopted as a new, empty backend, so writes
   meant for an unmounted disk land on the root filesystem; one that cannot be created refuses the whole node rather
   than starting degraded (pool section, gap 2).
2. The node loop collects garbage while metadata is dirty: `garbage_due = !busy`, Macha `!busy && !metadata_dirty`
   (GC section, gap 4).
3. A cancelled put that already landed a copy returns success; Macha fails it (DS section, gap 5).
4. Deferred-write tickets capped at 4,096, the oldest unsettled forgotten past it: a 16 GiB publication at 4 MiB
   extents loses its barrier (DS section, gap 1).
5. The node loop builds repair's inventory only in a GC pass, so new objects go unrepaired during an ingest
   (availability section, gap 1).
6. No GC quiet window after an event, a generation move or a rebuilt inventory (GC section, gap 1).
7. Repair's drop-local checks a claim and removes with nothing held between, in Macha and Dagda both (retention
   section, gap 9).

Blocks the port: `Context` serving every form Macha's store calls need, with Macha's semantics (the route above; DS
gaps 7 to 10, retention gap 4, GC gap 8); the pool and the control chain opened side by side, and Macha's startup
stage names (pool gap 1); a write ended short of its floor met at what landed (DS gap 4, when write copies exceed one).

Loses performance: serial DATA claims (retention gap 1); the barrier's early answer and its frame (DS gaps 2, 3); read
buffers and queued copies without retained memory (DS gap 6, cache gaps 1, 2); the node loop's fixed 1 s wake, stage
order, GC schedule and survey failure (availability gaps 2, 3, 4, 10; GC gaps 2, 3); the survey's discarded trie roots
(availability gap 7); a refused put sealed first (pool gap 3); a reload waiting on maintenance (pool gap 4); extra
copies on writes and a speculative fetch not raised (DS gaps 11, 12); the defaults Macha's options must set
(cache section: background concurrency, prompt read admission, the device reserve); the arbiter's activity clock (cache
gap 4).

Cosmetic: the remaining log lines, metrics, adverts, stage-hook names and exception types, each listed in its section.

Repair (availability gap 13), **ruled (Tom, 2026-11-14): push to the peers that answered.** Repair stays diff-only
but repairs toward every peer whose survey settled rather than waiting for all; a peer that cannot answer is asked
nothing (`StoreRepair.APushSendsNothingToAPeerTheSurveyCouldNotSettle` stands for that peer).

Reads, for Macha's FUSE on libfuse's low-level API (Tom's decision of 2026-11-14, relayed by the Macha session: after
the port Macha's FUSE reads become asynchronous and abandonable; Macha's heap investigation found today's
`FuseFrontend::dispatch` returns ETIMEDOUT while a worker still writes the caller's buffer). Every read `Context` offers
Macha must state and keep, each with its case:
1. No write into caller memory after the call has returned, ever: results Dagda owns and hands over (shared extent
   bytes, as Macha's `get_shared` does), or a caller buffer whose cancellation waits for the copy to stop.
2. Prompt cancellation: a deadline and a cancel token, honoured at fetch boundaries, a remote fetch in flight
   included, with the outcome told (cancelled, timed out, unavailable).
3. An async form completing on the caller's executor, beside the blocking form.
4. The read class per call (foreground, loader, speculative), as the frame type is today.
