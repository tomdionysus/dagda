# Duplication audit: what Tom's rule means for Dagda

Tom, 2026-11-20, to the stack (relayed by Lir Development Cluster): "Any functionality that is required by two or
more projects that share a project dependency, that functionality goes into the shared dependency." Dannan is the
root. Dagda depends on Dannan. Macha, Lugh, Louhi, Lir, Kvasir and Mimir depend on Dagda, or will for storage.

Dagda faces both ways:
- Down: what Dagda wrote that Dannan has, or should have.
- Up: what its consumers wrote, or are about to, that belongs in Dagda.

This is from Dagda's code, its plan, and the answers every consumer gave on 2026-11-20. It is not from reading
their trees.

## 1. Duplicated now

| What | Where twice | Resolution |
|---|---|---|
| Head spaces: the replica, exchange, fold, decoded view and history | Macha's `MetadataManager` and `MetadataReplica` (still Macha's own on 0.95.0, which migrates without `--heads`), and Dagda's `heads::Space` and `Exchange`, extracted from it | Macha's metadata moves onto Dagda's heads (the cutover's later stage), and Macha's copy goes. Until then every fix is made twice: the short file's fix is in Macha (b0b037c), and Space still lacks it |
| Durable file writes | Dagda does its own raw `fsync` in 9 library files (heads replica and node pack, the local store, the sealed journal, the durability domain, the migration). Dannan has `storage::FileSystem` | Route Dagda's syncs through Dannan's (already in Next, held "after the cutover", now unheld). Rule 3 of 2026-11-03 already says this |

## 2. About to be duplicated, and now Dagda's

Each needed by two or more consumers. All ruled or confirmed for Dagda on 2026-11-20 unless noted.

| What | Who needs it | Status |
|---|---|---|
| Durable `AcceptorStore` for `dannan::agree` | Louhi, Kvasir, Mimir (ballots and deltas only), Lir, Lugh | Next release |
| A node is caught up in a space | Louhi, Lugh, Kvasir; Mimir later | Next release; peers' heads by survey |
| Read a key, list by prefix, from one view | Louhi; Lugh and Lir go one layer down today | Next release |
| Keyed records: the register map (D6) | Louhi, Lir (retained), Lugh (JWKS), the catalogue, leases | Next release; owner's stamp only if Tom rules hlc |
| Consumable by FetchContent, library only | Louhi, Lugh | Next release (`DAGDA_BUILD_TOOLS`) |
| Leases in a head space (D5) | Lir, Lugh, Louhi; Lir's will needs it | Slice 3; not yet scheduled |
| A replicated or node-local log (D9) | Lir (inflight, queues), Lugh (audit), Kvasir and Mimir (acceptor logs) | The AcceptorStore is its first node-local form |
| An action once a key's time ends | Lir (will). Louhi: no (node-local timers, or agreement on its placement registers, settle every lapse). Lugh: no (every expiry is checked on read; an idempotent sweep of lugh/secrets would use it if built, but is not a need) | Lir alone: it fails the two-project rule. So it is Lir's own, built on Dagda's head-backed leases (D5), which Lir, Lugh and Louhi need. Dannan and Dagda's joint recommendation to Tom said Dagda's; this corrects it |

Nothing in this table was asked of Dannan that Dagda should hold instead, or the reverse, except the timed action.
Dannan and Dagda agree that it is Dagda's: it is durable state, and Dannan stores nothing.

## 3. In Dagda but needed by one project only

The rule does not forbid these. They are listed so nothing is mistaken for shared:
- Macha's forms in Context v2 (`put_here`, `hydrate` and the rest).
- `dagda-migrate` (v18 to v19).
- The counter map (D7, Lugh's back-off only so far).
- The no-values AcceptorStore form (Mimir only; Tom asked).

## 4. To check with Dannan (down)

Dannan, 2026-11-20: both added to its own audit, answered from searches of Lir, Lugh, Louhi, Mimir and Kvasir, not
from memory. Its prior view: agreement uses ballots and the HLC, and no Dannan user but Dagda and Macha keeps causal
dots.

Candidates where Dagda may hold what Dannan's other users need. Not yet confirmed:
- **Version clocks and dots:** `HeadClock` and `Dot`, `clock_covers`, and the claim `RetentionDot`. Macha keeps a
  third copy (`MetadataDot`). Kvasir's and Mimir's agreement uses ballots, not dots. If another Dannan user needs
  dots outside Dagda, they belong in Dannan.
- **Group commit:** Dagda's durability domain and the AcceptorStore's batching. Lir's own fast path and Kvasir's
  log may want the primitive without Dagda.
