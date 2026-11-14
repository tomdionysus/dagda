# Macha's store onto dagda::Node and Context: the call-by-call map (2026-11-14)

## The bar (the maintainer, 2026-11-14, to the Dagda session)

"Port through dagda::Node and Context is correct, but you must make ABSOLUTELY SURE that Macha can perform exactly as
well as it did before with Dagda as its storage backend. It is not enough for the concepts and primitives to be
'sorta' like Macha."

"There must be no 'shims' in Macha to make Dagda work for it. Both: parts of Macha may need to be rewritten to use
native Dagda effectively, and that's the NO LOSS of performance/reliability/etc - AND - Dagda MUST be capable of
handling DHT storage AT LEAST AS WELL as Macha."

So two requirements, both binding: Macha uses Dagda natively, with no adapter layer, rewritten where it must be and
losing nothing; and Dagda, measured on its own terms against Macha's store on the same workloads, is at least as good
a DHT store in every respect.

## The route (the maintainer, 2026-11-14)

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
fixed in Dagda, test-first (the maintainer: "fix gaps as found"), and the map records the fix. Nothing is inferred; what could not
be verified says so.

The map is the first proof. The others (the maintainer, 2026-11-14): Macha's own suites as the gate on Dagda; the side-by-side
harness on the Pis (Macha's store and Dagda on identical workloads), then the port on live-a and live-b against
Macha's live baselines; the in-place migration on copies of real state; reliability under crash, restart and
partition. Key custody: Macha's nodes get Dannan key custody at the cutover.

## The store's side (Dagda's), by subsystem

| Subsystem | Section | State |
|---|---|---|
| DistributedStore: put, get, deferred writes and tickets, the barrier, write copies, durability, reads and the server's handlers | `macha-store-map/distributed-store.md` | in progress |
| StoragePool and LocalStore: durability domain, presence index, sealed journal, layout, device reserve, backends, scrub | `macha-store-map/pool-and-local-store.md` | in progress |
| Retention, claims and reference counts; the ledgers (held ledger, object trie, trie diff and union, sightings, horizon) | `macha-store-map/retention-and-ledgers.md` | in progress |
| Availability and the survey, repair, maintenance's store stages and pacing | `macha-store-map/availability-repair-maintenance.md` | in progress |
| GC and the deletion path (claim_deletion, the collector, tombstones) | `macha-store-map/gc-and-deletion.md` | in progress |
| Cache, io pressure, data credit and admission, the device reserve | `macha-store-map/cache-pressure-credit.md` | in progress |

## The callers' side (Macha's)

Written by the Macha session after its heap-corruption investigation (its standing P0).

## Gaps and their fixes

Collected from the sections as they land, each with its case and commit.
