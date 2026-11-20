# Special roles audit: what the maintainer's rule means for Dagda

The maintainer, 2026-11-20 (relayed by Lir Development Cluster): "There is to be no concept of a special role or node. Any node
may disappear at any time, have patchy connectivity, may be permanently retired, be slow, or be degraded in some
other way. The purpose of every one of your projects is so that is never the user's problem except in exceptional
circumstances."

A read-only audit of Dagda's code, at develop `7c8c98a`, with file:line references. Before this note, I re-read the
places marked "checked".

## Special roles

| Role | Where | Verdict |
|---|---|---|
| The lowest node id forms a virgin space | `heads/exchange.cpp:1328-1335` (checked) | A role, in background repair only. A foreground read or write forms the space on any founding node without it (`discover_or_form`, `:946`). A space nobody writes can stay unformed if the lowest node never opens it. Remove it: founding is deterministic, so any node may form, as the foreground path already does |
| Joiner and founder asymmetry | `exchange.cpp:939-948` (checked), `ExchangeOptions::joiner` | A joiner alone waits for a "bootstrap peer". It forms only once **every** active peer answers the checkpoint survey (`survey.complete = failed == 0`), so one gossip-active but unreachable peer blocks a fresh joiner. Goes with founding as an explicit once-only act (Lir Development Cluster's proposal) |
| Leases | `context.h:307-312`, `lease/local_records.h` | Held in one node's memory. Two nodes acquiring the same lease both get it. The keeper's logic is symmetric; the records are not shared. Head-backed leases (D5) fix it |
| "Repair owner" | `exchange.h:99` | Not a role: each node's own maintenance thread |
| Collector and GC | `ledger/collector.*` | Not a role: per node, on local gates |
| Placement, write copies, repair | `store/distributed_store.cpp:766-771` | Not a role: rendezvous ranking computed alike on every node |

## Failure modes, gaps ranked by how likely a user sees them

1. **Leases are one node's only** (above). Any hosted system using them for cluster-wide exclusion is silently
   wrong today.
2. **A retired node is "known" for 30 days**, Dannan's `forget_after`, and there is no retire verb.
   - Status shows the cluster degraded.
   - Write placement ranks over every known node, dead ones included, whenever not all are live
     (`distributed_store.cpp:784-792`, checked).
   - The replicated floor counts dead members (`:1059-1062`, checked; the stack's acknowledgement standard, the maintainer
     2026-11-15).
   - Live fallbacks still meet it, so the cost is copies and latency, not failure.
   - Data only a retired node held is lost if nothing drained it.
3. **One node's wall clock decides tombstone maturity** (`ledger/collector.cpp:71-91`, checked).
   - A clock ahead by more than the grace erases every tombstone at once.
   - A clock behind stamps retirements as old.
   - A partitioned peer can then revive deleted entries.
4. **A slow peer holds up head replication**: claims and heads are offered to peers serially
   (`exchange.cpp:1095-1123`). The slice-2 item is paused.
5. **Durable writes wait on the slowest live member**: durable never spills (`distributed_store.cpp:1188-1207`).
   It is bounded only by per-call no-progress, so it is felt on mixed hardware.
6. **A set-aside head is dropped after `garbage_grace`** (30 days) with a warning only (`exchange.cpp:1229-1230`).
   Until then history cannot compact (`replica.cpp:1972-1974`). It is the same in Macha.
7. **Author ids accumulate in every clock**: no pruning found (inferred).
8. **A full or degraded store is not reported upward**. Disk full falls through to fallbacks per write (`:1118`,
   `:1230-1237`), but nothing tells Dannan's readiness.

What holds today:
- A sudden loss: non-durable writes spill after `write_stall`, head commits skip a stalled peer after 2.5 s, and
  any node accepts a mutation alone.
- Repair rebuilds copies over active hosts.
- Reads prefer fast replicas.
- A backend offline is taken out of service.
- The device pressure gate sheds speculative work on a slow disk.

## What Dagda needs from Dannan

- A retire verb (or forgetting a long-absent node sooner).
- A partition view.
- A clock-skew signal.
- An explicit once-only founding act.
- Readiness and a health advert Dagda can feed.

## Lir Development Cluster's six items for Dagda (2026-11-20): Dagda's answers

1. Retire and drain: agreed, Dagda's. It evacuates a node's objects and claims before Dannan retires the identity.
   Head spaces are every-node, so they need no evacuation, but a retired node's author ids fold into a base and its
   set-aside heads resolve. Membership lists come from the catalogue (slice 5).
2. "No peers" as its own answer: already in the caught-up design (`alone`). Founding to Dannan: agreed. Then
   Dagda drops the lowest-id rule and the joiner's every-peer survey, and any node forms a space once the cluster is
   founded.
3. A set-aside head never dropped silently: agreed, in Dagda's heads. Merge what can be read, or keep a standing
   conflict with a Status code; never a 30-day drop.
4. Store health upward: agreed. Dagda supplies the inputs (pool or control store full, backend offline, durability
   failure); Dannan owns readiness and the advert.
5. Single-holder duties on leases: agreed. A duty any node can take over is not a special node. It needs
   head-backed leases (D5) first, since today's are one node's.
6. `DataWorkContext` co-owning its counters: confirmed. `store/data_work.h:49,59` holds the progress counter, and
   the cancel flag, by raw pointer. Dagda changes it to co-ownership before Macha moves.
