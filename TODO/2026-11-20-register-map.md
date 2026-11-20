# The register map (D6): the design to build from

The first item of Dagda's next release in the stack's converged plan
(`../lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md`, "Dagda", item 2). Its users:
- Louhi's records;
- Lir's retained values;
- Lugh's JWKS, lugh/scope and lugh/secrets (Lugh deletes its own `merge::winner` once this exists);
- Macha's UserRecord and AuthSession, later;
- the lease records (D5, built next on this);
- the domain catalogue (slice 5).

Ruled first on 2026-11-13 (the three-systems design, "Built-in merges for heads", ruled item 4). The shape was
settled by the group on 2026-11-20. Open: the stamp's kind (the maintainer's Q2, hlc or Lamport).

## What it is

A built-in owner of a head space: Codec, Merge, Mutation and tree value. Dagda ships it in the library
(`dagda/heads/register_map.h`), so a hosted system opens a space of it, not writing its own.
- A key (a string, the tree's key) maps to a register: a value (opaque bytes) or a tombstone, plus a stamp, the
  writing node's id, and an optional expiry.
- Values stay small enough to live inline in the tree's leaf records (the tree's record limit). Anything larger is
  the owner's object, named by id in the value.

## The order and the merge

- Two registers for one key are ordered by `(stamp, node id)`; the greater wins. A tombstone is a register like any
  other, so a remove wins against an older write and loses to a newer one.
- The merge of two heads is per key, the greater register, and nothing else. It is a pure function of the two
  heads, as `Merge` requires (H8): no base is read, and every reconciler mints the same commit.
- Equal `(stamp, node id)` with different bytes cannot come from one node, because a node never reuses a stamp.
  Should it happen, the merge takes the lexically greater bytes, so it stays deterministic, and counts a conflict.
- The stamp kind sits behind one seam, `RegisterStamp`:
  - **Lamport** (built now): a counter. A write takes one more than the highest stamp in its parent's map, or in
    the key's register, whichever the seam states. Every node's next stamp is then above everything it has seen.
  - **HLC** (Q2): Dannan's hybrid clock stamp. A stamp from a peer goes through `HlcClock::admit`, and one admitted
    with `merged=false` is refused, never applied. That waits on Dannan's next minor.
- The stamp kind is fixed per space at open, and recorded in the space's head, so two nodes cannot disagree.

## Tombstones

Per space, chosen at open:
- **A window:** a tombstone older than the window, on the stamp's own time for HLC or on the commit that
  stamped it for Lamport, is dropped by a mutation, never by the merge. That is the three-systems design's window.
- **Never:** tombstones are kept for good (Lugh, which never reuses a removed key).

## Expiry per key

- A register may carry an expiry. Past it, the key reads as absent. It is dropped with no commit of its own, and
  removed from the tree by the next mutation that touches its leaf (MQTT 5's message expiry on a retained value).
- Expiry is judged on the stamp's clock. For Lamport spaces it is wall time, and a stated limit.

## The conditional write

- A mutation may name the version it read: the key's `(stamp, node id)`, or "absent". It is refused with a
  conflict if this node's parent register differs.
- **It is checked against this node's view only.** Two nodes can each pass at once, and the merge then decides by
  order. It prevents stale overwrites through one node. It is not a lock, and anything that must be unique
  cluster-wide stays on agreement registers (Lugh's uid allocation, Louhi's revisions).

## The interface (for the cases)

- `RegisterMap::spec(name, options)` gives a `SpaceSpec` with the codec, the merge and the tree.
  - Options: the stamp kind, the tombstone policy (window or never), and the founding entries (none by default).
- Mutations: `put(key, bytes, expiry?, expect?)` and `remove(key, expect?)`, several in one commit. `expect` is
  absent, `version`, or `absent`.
- Reads, through the space's views, which take a view (as Louhi's reads do):
  - `get(view, key) -> Register?`: absent for none, a tombstone or an expired key;
  - `list(view, prefix, cursor, budget)`.
- `Register { bytes value; bool tombstone; uint64 stamp; NodeId node; optional<int64> expires_ns; }`.

## Cases, written before the code

Merge:
1. Two writes to one key on two nodes: after the fold, both nodes hold the greater `(stamp, node id)`.
2. A remove against a concurrent older write: the remove wins. Against a newer write: the write wins.
3. The merge is a pure function: both reconcilers of one pair mint the same commit hash.
4. Disjoint keys from two branches: both kept.

Lamport:
5. A node's next stamp is above every stamp in its parent's map, so a write after a fold beats everything it saw.

Tombstones:
6. Under a window: a tombstone past the window is dropped by the next mutation, never by a merge, and a fold with a
   branch still holding the old write does not revive it inside the window.
7. Under "never": kept across any number of mutations.

Expiry:
8. A key past its expiry reads as absent, and the next mutation touching its leaf removes it.

Conditional write:
9. With `expect` matching, the write commits.
10. With `expect` stale, the write is refused with a conflict and nothing commits.
11. With `expect` absent and the key present, the write is refused.
12. Two nodes each pass their check at once: after the fold, the greater order wins. Stated, not prevented.

Reads:
13. `get` of an absent key, of a tombstone and of an expired key are all absent, and a missing tree node fails
    rather than reading as absent.
14. `list` by prefix over pages equals one walk of the same view.

Stamp seam:
15. The stamp kind is recorded in the space's head, and a node opening it with another kind is refused.

## Additions from the users' review (Lugh, Lir, Louhi, 2026-11-20)

Settled by the goals. Each one, if left out, would make a user keep its own copy of what D6 does.

**Order and tombstones:**
- **Tombstone wins (Lugh: the maintainer's P8 ruling, "removal over edit and never resurrected").** A per-space option, used
  with "never". A tombstone is greater than any value register for its key, and `(stamp, node id)` orders only
  value against value and tombstone against tombstone. It is still a pure per-key function.
  - Case 16: a remove on A and a newer write on B; after the fold, a tombstone on both.
- **Never reuse a removed key (Lugh).** Under never plus tombstone-wins, a put onto a tombstone is refused on this
  node's view, and `expect absent` fails against a tombstone.
  - `get_register(view, key)` answers the register with its tombstone flag (Lugh's 410, not 404). `get` stays
    absent for a tombstone.
  - Case 17.
- **The window's default is `garbage_grace` (30 days), Dannan's `forget_after` (Louhi, 4).**
  - A fold inside the window never revives a removed key.
  - A node away longer than the window, coming back with an old write of a removed key, does revive it. This is a
    stated limit: such a node is also past Dannan's forget and rejoins as new.
  - A space that must never revive uses "never" (Lugh), or keeps a key that absorbs, as Louhi's stop-key does.
  - Case 18: a branch older than the window revives, stated.

**Reads and writes:**
- **Before and after on change (Lugh 3, Louhi 1).**
  - `changed(view_before, view_after, prefix, cursor, budget)` pages `(key, before?, after?)` registers, over the
    tree's own diff. It costs the change, not the map.
  - It reads any view the node holds, which includes a notice's `before` and `after`.
  - Case 19: the keys a commit or a fold changed are listed, with both sides, and nothing else.
- **A write names what it wrote (Louhi 2).** The commit's outcome carries each key's new `(stamp, node id)`, for
  the next conditional write.
- **The value bound: 64 KiB per value, refused at put** (Lugh 4, Louhi 3).
  - Values are inline in leaf records. A leaf holds at most 128 entries, so a leaf is at most about 8 MiB, inside a
    CONTROL object and a Dannan message (128 MiB).
  - Anything larger is a Dagda object named in the register (next point).

**Objects named by a register (Lir 3).**
- A register may name objects (`refs`, ids, alongside its bytes). A commit claims them under its dot (`Introduced`).
- The space's reachability (`ReachabilityProvider`) names every object the current head's registers refer to. So
  the collector frees an object once no register in the head names it, after any put, remove or fold. The owner
  tracks nothing.
- Case 20: an object named by a register, superseded on another node and folded, is released by the collector,
  and one still named is kept.

**Placement and acknowledgement (Lir 1, 2), stated:**
- D6 opens under every policy Space supports: today every-node; `capable(lir)` when D1 (slice 2) exists.
- On a node holding the space, `get` and `list` read the local view. They ask a peer only for a tree node not yet
  arrived, as every space's reads do.
- put and remove commit through `Heads::mutate`. Waiting on "replicated" for that commit is D2's per-space
  k-level, slice 2, not in D6. Until then a commit is accepted here and replicated after, as today.

**The expiry clock (Lir 4).**
- For a Lamport space, expiry is judged on the reading node's wall clock (`SpaceServices::wall`), and the map
  exposes it (`RegisterMap::now()`), so an owner computes "time left" on the same clock.
- For HLC, it is the HLC's physical time.

## Order of work

`feature/register-map`, off `develop`, test first. Cases 1-15 fail first on a Pi, then the code. Then the whole
suite, ASan/UBSan and TSan once, then one squash commit onto `develop`. Head-backed leases (D5) follow on it.
