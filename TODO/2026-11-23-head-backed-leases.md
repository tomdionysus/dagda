# Head-backed leases (D5): the design to build from

The second half of 0.5.0, with D6 public (the coordinator, at the maintainer's request, 2026-11-23). Today a
lease's records are one node's memory (`lease/local_records.h`; the special-roles audit's D-1): two nodes acquiring
the same lease both get it. D5 moves the records into head spaces, so every node holding a scope sees one record per
lease, merged the same way everywhere. The lease template stays (E1-E9, `lease/keeper.h`); epochs and the fence are
unchanged.

Who waits: Lir (wills and client-id uniqueness, the other gate of its 0.3.0), Macha (torrent claims, the catalogue
scanner, the sweeper), AthenaSIP's trunk lease (its one compare-and-set).

## The merge: the lease order, not the register map's

The three-systems design (2026-11-13) said a lease scope's space would be "of the register-map merge". That was
before D6's order was ruled (Q2, 2026-11-20: (stamp, node id), the greater wins). The lease order is another one:
the higher epoch, then within an epoch the earlier claim, then the lower node id (`lease::beats`), and the fence
depends on it: an epoch never goes back. Under (stamp, node id), a concurrent write of a lower epoch with a later
stamp would win, and a stale holder would stand again.

So a lease scope is its own head-space owner, carrying a `LeaseTable` (`lease/lease_table.h`, "the value a lease
space carries in step 4"), merged per name by `lease::merge`: commutative, associative and idempotent, a pure
function of the two heads, as `Merge` requires (H8). Its tree, codec and founding follow the register map's shape
(names as keys, one entry inline per key); a removed lease keeps its tombstone for `LeaseEntryTraits::retention`.

## The records

`HeadLeaseRecords : LeaseRecords`, one per node, a space per scope (`lease/<scope>`), opened on first use:
- `read(scope, name)`: the entry at the space's current view; none for no record or a tombstone.
- `join(scope, name, proposed)`: a mutation that joins `proposed` into the parent's entry by `lease::merge` and
  commits it; the completion re-reads and answers the standing record, as `Joined` says. A join that loses to the
  record (a higher epoch already there) commits nothing and answers the record.
- Placement is the scope owner's: every-node by default, or a replicas policy so the holder is drawn from a shard's
  replica set (Lir N5).

The keeper decides on what it reads, as now; two nodes may each take a lease on their own view, and the fold then
leaves the one the order prefers, so the other's `standing()` says it lost. That is the template's stated soft
claim: two holders can coexist for at most one fold, and the epoch fence keeps the stale one from acting.

## The contract

`Context::leases()` (the noun of version 1) takes its records from heads when the node has them, the local records
only on a node with `NodeOptions::heads` off. The call shapes do not change; what a caller gets is now cluster-wide.

## Cases, written before the code

1. A lease taken on A is read on B after the fold: the same holder and epoch.
2. Two nodes take one vacant lease on their own views: after the fold both read one holder, the order's, and the
   other's standing is lost.
3. A takeover at epoch n+1 on A against a concurrent re-claim at epoch n on B: n+1 stands on both, whatever the
   stamps or arrival order.
4. The merge is a pure function: both reconcilers of a pair mint one commit; joining a record with itself changes
   nothing.
5. A release (tombstone) folds against an older claim and wins; against a newer epoch it loses.
6. A restart keeps the record (it is in the space), where the local records forgot it.
7. Through the contract on a cluster: acquire on A, the same lease refused on B while A is present, taken on B
   once A is absent past the window, A's fence then refusing A's stale epoch.
