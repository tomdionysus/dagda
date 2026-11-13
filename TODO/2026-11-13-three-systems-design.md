# Dagda for Macha, Lugh and Lir: a design (draft, 2026-11-13)

Tom, 2026-11-13: "It needs to support Macha, Lir, and Lugh and not as afterthoughts", and to the stack: "You all
need to discuss your needs and ensure the the more fundamental projects, namely Dannan and Dagda, support Macha, Lugh
and Lir as first class citizens. This was the purpose of splitting out the technologies."

A proposal, for Tom to rule on. Nothing here is built or changes Dagda's plan until he does. The inputs are
`../lir/TODO/2026-11-13-lir-needs.md` (G1-G9, N1-N6, and Lir's answers), `../lugh/TODO/2026-11-13-lugh-needs-of-the-stack.md`
(G1-G15, N1-N8), the stack document's seams (`TODO/2026-11-03-stack.md:266-464`), the heads plan's L1-L9
(`TODO/2026-11-05-heads-plan.md:417-429`) and the code at `0.1.4`. Lir and Lugh review this draft before it goes to Tom.

## What changes, in one paragraph

Dagda stops being Macha's store with deferred generalisations and becomes one store with three owners. The existing
nouns stay as they are for Macha. Head spaces gain the placements the stack decided on 2026-11-03 and Dagda deferred
(replicas: N with the owner's salt, and node-local), a per-space k with evidence, and a change notice. A new noun, the
log, carries what is a sequence rather than a state: Lir's inflight and session queues, its message history, and Lugh's
audit chain. Small merges every owner would otherwise write alike (last-writer-wins registers, per-origin counters)
come with Dagda. Leases move onto head spaces. A node's spaces share one advert. The commit rate is measured before any
of it fixes a format.

## What each owner needs, merged

Built (named cases are in the capability report):

| Need | Who | Where |
|---|---|---|
| Objects at four levels with per-node evidence, tickets, claims, instant delete | all; Lir G1 | `context.h:162-236` |
| Every-node head space with the owner's codec and merge (base and heads) | Macha; Lugh G1, G3 | `context.h:265-324`, `heads/space.h:124` |
| Reading without waiting (`current()`) | Lugh G6 (part) | `heads/space.h:406`, C13 |
| Conditional, callback-shaped commit: deadline, executor, `done`, retries, an external identity | Lugh G4 | `Context::Heads::mutate` (`context.h:302-313`). Lugh's "synchronous" is `heads::Space`; the Context noun is callback-shaped. If-Match is the mutation's own check against the view it is handed |
| The write-floor scope check | Lugh G13 | H29, `StoreScope.*` |
| Placement with salts and a capability filter | Lugh N4, Lir N5 | Dannan `placement::by_capacity`, `with_advert` (`dannan/placement/placement.h:65`, `:79`) |

Not built, merged across owners:

| # | Need | Lir | Lugh | Heads plan |
|---|---|---|---|---|
| D1 | Head spaces on N nodes, with the owner's salt, restricted to nodes advertising a capability, the replica set as evidence | G5 | G2 (lugh/secrets on Lugh nodes) | L2 |
| D2 | The Replicated level per space: a commit acknowledged once k replicas hold it, naming them | G5 | G5 | L3 |
| D3 | Node-local spaces, never rolled back, bounded under churn | G4 | G7, G8 | L5, L9 |
| D4 | A notice when a space's head moves, and the keys changed between two views at the cost of the change | | G6 | |
| D5 | Head-backed leases, aligned with a space's replica set | G7 | G11 | L1 |
| D6 | Retained values: per key, last value wins by (Lamport, node id); a clear is a tombstone; seen across nodes | G3 | G9 (JWKS) | L7 |
| D7 | Per-origin counters, merged across nodes | | G10 | E10 |
| D8 | A rate: about 7,000-8,000 small replicated writes a second cluster-wide, 27-32 a second per shard | G6 | G15 (50/s storms) | L6 |
| D9 | A replicated log: append, group commit, Replicated evidence, truncation by acknowledgement, bounded by count and age | G6, G9 | audit chain | |
| D10 | The domain catalogue: domains made at genesis, a domain key wrapped to nodes that advertise a capability and unwrapped when they stop; blind holders | | G12 | L8 |
| D11 | Space count not bounded by Dannan's 64 adverts | N2 | | F5 |
| D12 | A contract to pin | G8 | | |
| D13 | An installed in-process multi-node fixture with heads on, stepped time | | G14 | |
| D14 | Alpine (musl, GCC 15) | | Raven | |

## The nouns

`dagda::Context` keeps Domains, Objects, Claims, Tickets, Leases and Heads, and adds Logs. Every operation keeps the
stack's shape: a deadline, the caller's executor, a completion posted there, evidence rather than a boolean.

### Head spaces: placement, k, notices

`SpaceSpec::policy` (`heads/space.h:54-61`) becomes:

- `every_node`: today's, Macha's. Its path, wire and adverts do not change (below, "What Macha sees").
- `capable(capability)`: every node advertising `capability` holds it, as every_node does for all nodes, and a node
  that starts or stops advertising gains or drops it. Lugh's lugh/secrets: every Lugh node resolves a sign-in from
  memory, so no Lugh node may be outside the set (Lugh's review, 2).
- `replicas(N, scheme, capability)`: held on N nodes chosen by Dannan's `by_capacity` over the space id with the
  owner's `placement::Scheme` (salts the owner names, so Lir computes the same set for its lease holder), drawn only
  from nodes advertising `capability` when one is named (`with_advert`).
- Placement is not confidentiality (Lugh's review, 1). A space is sealed under its domain's key, and only a node
  holding that key can open it. Dagda already wraps domain keys to nodes and has member and blind roles
  (`domain/wrap.cpp`; the capability report's domain cases). Slice 2 opens a space under a domain whose key is wrapped
  only to members named in configuration, until the catalogue (slice 5) makes membership follow the capability. Case:
  a Macha-only node handed the space's bytes cannot open them.
  `Heads::replicas(space)` returns the set and how each stands, as evidence. A node outside the set holds nothing of
  the space; a read there is forwarded to the nearest holder (`Routes::call` with the caller's deadline) and fails
  closed. The exchange's sends to every active node (`heads/exchange.cpp:359`, `:650`, `:935`, `:968`, `:1112`,
  `:1186`, `:1292`) take the space's set instead, on a branch the every-node path never enters.
- `node_local`: this node only; never advertised, exchanged, offered or repaired; sealed under a node-local domain
  whose key is Dannan's node custody (`derive_local`, Lugh N2); same codec, merge and calls as any space (stack
  document, "Node-local spaces"). A commit is durable on this node before it returns. No rollback: a head that is an
  ancestor of the held head changes nothing (as every space), and a restored older file is refused by a generation
  kept beside the node's custody, not in the space's own directory. Bounded under churn (Lugh G8): case, 10,000
  node-local records made, each expiring within 60 s, under stepped time; the space's disk and memory afterwards as
  before.

`SpaceSpec` gains `copies` (k) for the Replicated level, at most N. A commit takes a `Durability`:
`persisted` returns when this node holds it (today's behaviour, and Macha's); `replicated` when k replicas have
accepted it; `durable` when every available replica has, a replica Dannan sees as dead named pending. The evidence
is `MutationEvidence` with the per-node states objects already carry (`store::ReplicaState`). On a side that cannot
reach k the commit returns at the deadline with what it reached; what the owner does with that (Lir: Persisted or
refuse, per node) is the owner's.

`Heads::watch(space, executor, notice)`: posted when this node's head moves, with the old and new view's generation.
`Heads::changed(space, before, after, from, budget)`: the keys whose entries differ, paged, reading only where the
trees differ (the tree already diffs: `heads/tree.h:284`, `:650`). Lugh's "disabled on A, refused on B at once" is
B's notice on the commit's arrival plus the changed keys.

### Logs (new)

A head space is a state with a merge; most of Lir's hot path is a sequence with acknowledgements. Building that as
head commits pays, per commit, a durable dot, tree nodes stored as CONTROL objects, the claim barrier and the
mutation lock across device writes (`heads/space.h:356-358`): right for Macha's namespace (about 1 a second sustained under import, bursts of 10 a second, deltas of 0.2 to 1 KB;
the Macha session's figures from the live nodes, 2026-11-13),
and the wrong cost model at 30 a second per shard. So a log, whose semantics are Lir's review (R1-R8) and Lugh's (3):

- `Logs::open(LogSpec)`: a named log with a placement (as a space: replicas(N, scheme, capability), capable or
  node_local), k, a domain, a tick, bounds (count, age, bytes; and count per stream), what a bound does (drop the
  oldest, or refuse the append), and the tombstone window.
- `Logs::append(log, entries, level, deadline, executor, done)`: entries are (dot, stream key, bytes up to the inline
  limit, expiry). Appends arriving within a tick are one group commit: one write to the log's segment, one sync, one
  message to each replica carrying all of them. A tick of zero commits each append at once, for an owner that batches
  already (R7). `replicated` returns when k replicas have synced the group; the evidence names them.
- `Logs::ack(log, dot, stage, bytes)`: an ack is an entry naming another by dot, carrying a stage and a few owner bytes
  (at most 256), so a per-message state machine (QoS 2: PUBLISH sent with its packet id, PUBREC, PUBREL, PUBCOMP) lives
  in its acks (R3). Dagda never knows what an ack means or which acks an entry needs (R1).
- `Logs::retire(log, dots)`: the owner's final ack. A retired entry and its acks go. Their dots are kept as tombstones
  for the log's window and never re-admitted by a merge, so an entry removed on one side of a split does not come back
  on heal (R2). A split longer than the window can deliver a QoS 2 message twice; that limit goes to Tom as stated.
- Reads return live entries with their acks: `read(log, stream, after, budget)` pages one stream, each origin's entries
  in seq order, after a merge too (R4); `scan(log, after, budget)` pages every live entry of the log, for a new lease
  holder resuming what is owed (R5); `live(log, stream)` counts a stream, and a per-stream count bound applies at
  append (R6). `watch` as for spaces.
- The dot is the owner's, taken as given (Lir's question 4): it is the owner's dedup key on its own wire. Dagda orders
  by it and treats a second append of a dot it holds as the same entry. A replica refuses an entry whose dot's origin
  is not the appending node, as it refuses a write the scope check refuses. That check is on appends only: an ack or a
  retire may name any entry's dot, whoever wrote it (Lir's lease holder retires a dead publisher's entries, and every
  target node acks entries others appended), and passes subject only to the scope check. An ack is identified by its
  writer, the dot it names and its stage, so the writer of the ack is always the node sending it. Dagda does not tie
  retires to a lease: the log knows nothing of leases, and the owner's own rule decides who retires.
- Any node may append, not only a replica (Lir's question 1). The appender sends the entries to the log's k replicas at
  once, as an object write goes to its replicas; each replica group-commits them with the other appends of its tick,
  and the evidence names each replica that synced. There is no relay through a holder, so the PUBACK path is one round
  to the replicas plus their sync.
- Each replica keeps an index of its live entries in memory (dot to segment offset), so `scan` reads only live
  entries, not whole segments (Lir's question 2). A lease change moves about 3/N of a cluster's shards at once, and
  each new holder scans its shard; that burst is in the measurement.
- Merge is fixed, not the owner's: entries are the union by dot less the tombstones, and an entry's acks the union of
  its acks. That is Lir's split rule (union of entries; per entry, what is still owed is what its acks do not cover),
  and it needs no lease to stay correct; the lease keeps the steady state to one writer.
- At a bound a log drops its oldest (Lir's history) or refuses the append and says so. An audit log refuses: an entry
  goes only by `retire`, once its export is acknowledged (Lugh's review, 3). Case: a full audit log refuses an append,
  keeps every entry, and takes appends again after an export is retired.
- A node-local log has the no-rollback of a node-local space: a restored older segment is refused by the generation
  kept beside the node's custody. Case: a segment copied back from before is refused at open.
- On disk: append-only segments per log, sealed per entry under the domain like an object, rotated and deleted whole
  when everything in them is retired or expired; nothing is a CONTROL object and the collector is not involved.
- A payload over the inline limit stays an object with claims, claimed by the owner and deleted at once by
  `Objects::remove` on the final ack (R8); the entry carries its id.
- Lir: one log per shard for inflight (stream key: the topic), one per shard for session queues (stream key: the
  client id, 10,000 a stream at most), one per topic filter for opt-in history (bounded by count and age). Session
  state proper (filters, will, expiry, the takeover counter) is a replicas(3) head space per shard with Lir's merge,
  at the low rate (connect, subscribe, take-over). Lugh: its audit chain is a node-local log per node, retired by
  export.
- Count, per shard: an inflight log, a queue log, a retained space, a session-state space and a lease space: about
  1,280 spaces and logs at 256 shards, each with a notify channel. Measured (below) for memory, file handles and
  channels at 256 and at 1,280; Lir cuts the count (64 shards, or leases with session state) if it must.

### Built-in merges for heads

Owners still bring their own codec and merge. Dagda also offers two it would otherwise see written three times:

- A register map: key to (value or tombstone, Lamport counter, node id), last writer by counter then node id,
  tombstones dropped after a window the owner names, and an expiry per key after which the key reads as absent and
  is dropped with no commit (MQTT 5's message expiry on a retained value; Lir R9). Lir's retained (G3) and Lugh's
  JWKS (G9) are each a space of this; "retained" is then not a separate noun.
  A node that does not hold a space can still follow which keys change: `Heads::watch_keys(space or name prefix)`
  subscribes, one subscription for a family (`lir/retained/` covers all 256; Dannan's channels subscribe by prefix;
  Lir's question 3), to the
  space's notify channel, on which each commit publishes its changed keys (not values). Lir keeps its index of retained
  topics on every node from that, and a wildcard SUBSCRIBE reads values from the holders with a forwarded paged
  `entries` (Lir R10). Both are served; the notice is the one Lir prefers. Its cost (changed keys times subscribing
  nodes) is in the measurement.
- A counter map: key to per-origin slots, each origin's slot merged by max, the value their sum (a grow-only
  counter, with a decrement slot for a PN counter). Each slot carries its last change; with a window the owner names,
  a slot older than the window counts as zero and is dropped, so a count forgets (Lugh's back-off forgets an account
  after an hour with no failure; Lugh's review, 4). Lugh's per-origin sign-in counters (G10). Case: failures on two
  nodes count together, and an hour later with none the count is zero on both.

### Leases

The lease template stays (E1-E9; `lease/keeper.h`). Its records move from one node's memory
(`context.h:238-243`) to a head space per lease scope, of the register-map merge, placed as the owner says: Lir's
`lir/shard/<n>` leases in a space placed with the same scheme as the shard, so the holder is drawn from the replica set
(D5, Lir N5); Lugh's rotation lease in an every-node or replicas space. Epochs and the fence are unchanged.

### The domain catalogue

An every-node space of the register-map merge, made at genesis, holding each domain's description, its member
capability and its key wrapped to each member node's certificate (plan 122). A node that advertises the capability is
wrapped to by the next commit of a node that holds the key; one that stops is unwrapped and, for a domain whose
members must change keys, the key is rotated and rewrapped. Blind holders (members by durability only, holding sealed
bytes they cannot open) come with it. Until then, G2 ("a Macha-only node can never read a password hash") is met by the
key, not the placement: slice 2 seals lugh/secrets under a domain whose key is wrapped only to the members named in
configuration, and `capable(lugh)` only decides which nodes hold the bytes. For the operator: until slice 5, a node
that starts advertising lugh holds lugh/secrets but cannot open it, and sign-in fails there, until configuration
names it a member (acceptable to Lugh; Lugh's documentation carries it).

### Adverts

Today each space advertises two keys (`heads/exchange.cpp:106`, `:976`) against Dannan's 64 per node
(`dannan/membership/node_record.h:47`). The count is the lesser problem. Any advert change makes the node's whole
record newer, which is gossiped whole and persisted by every peer (the Dannan session; `dannan/membership/membership.h:77-87`,
`membership.cpp:185-195`): a generation that moves with commits, at Lir's 27-32 a second per shard, is a gossip and
disk storm at any cap. Adverts are for slow facts. Proposed:

- Every space but Macha's every-node one advertises nothing of its own. Its generation moves on a notify channel
  (`dannan::Context::channels`) named for the space and subscribed by its replica set, which is also how a replica
  learns a commit to fetch.
- The node advertises one slow key, `dagda.spaces`: a digest of which spaces it holds (not their generations),
  changing only when it opens or drops one; a peer seeing it change asks for the list, paged. It fits in Dannan's
  1 KiB value.
- Logs advertise nothing; their replicas talk directly.
- Macha's every-node space keeps its two adverts as they are: its generation moves about once a second at most, and its wire and
  roster stay what its gate checks.

Space count is then bounded by memory and the exchange's budget. Dannan's cap need not change.

### The commit rate, measured first

Before D1-D9 fix any format: a benchmark in the tree (`dagda-bench`, recorded, never asserted, as Dannan's), on the
Mac and on corvus-develop:

1. Today's head commit at 1, 16 and 256 spaces on one node and on three, with a small register-map state and with a
   tree: commits a second, p50 and p99, where the time goes (dot, tree nodes, claim barrier, sync).
2. A prototype log with Lir's real pattern (Lir's review, order): batched appends, per-edge acks with stage bytes, a
   final retire, ticks of 0 and 10-20 ms, k = 2 over three nodes: appends a second and the latency from append to
   `replicated`; and the same at 256 and 1,280 logs and spaces on a node, for memory, file handles and channels.
3. Lugh's shape (Lugh's review, 5): one space of 50,000 keys (values under 4 KiB) taking 50 commits a second for a
   minute from several nodes, and the time from a commit on node A to `watch()` and `changed()` on node B: the
   "disabled on A, refused on B" latency.
   And Lugh's leases (Tom, 2026-11-13: TOTP replay protection takes a per-user lease, so leases are on the sign-in
   path): acquire and release at 50 a second cluster-wide over 10,000 user keys, each held under a second, timed at
   p50 and p99, since the acquire adds to a person's sign-in. One lease space with a key per user, not a scope per
   user (10,000 spaces). An acquire is a commit on the lease space that must reach its replicas before it is granted,
   so this times that commit, not a local one.
4. Dannan's small-message rate between two real machines (Lir's N1; Dannan's to run, Dagda's figure depends on it).

If (1) carries Lir's session and retained rates, heads stay heads for those; the log is still the noun for inflight
and queues. The pass mark for (2) is Tom's PUBACK target (2026-11-13, relayed by the Lir session): p99 under 50 ms at QoS 1
with k = 2, for the whole path (Lir's tick, the append, the group commit synced on k replicas), at about 8,000
appends a second over three nodes, reported at p50 and p99 for ticks of 0 and 10-20 ms, on the Mac and then the Pis.
The appends come from a node outside the replica set, as Lir's publishing node usually is, and the run includes a
lease change that moves a third of the shards, timing each new holder's scan. On the Mac, three node processes on
loopback; on the Pis, three processes on corvus-develop, taking turns with Macha (Tom, 2026-11-13). The prototype is
throwaway, inside `dagda-bench`; slice 4's log is written test-first from this design.
If (2) misses it, the log's design changes before anything is built on it.

### The contract

`context_version` 1 is frozen at what `0.1.4` serves: what Macha's port builds on. Version 2 adds the nouns above.
The library serves both while Macha is on 1, so Macha relinks (the package is exact) but changes no source.
Version 2 is frozen at slice 4, once the log lands: everything Lir needs. The domain catalogue (slice 5) is additive,
version 3, with 2 still served (Lir's review, order). Lugh and Lir pin version 2 at the tag that freezes it, and every
noun in it has named cases before it is frozen; neither ships on an unfrozen contract.
Until then they build on slice tags (Lugh's P8 on slices 1 to 3), so: each slice's tag keeps everything the previous
slice's tag gave, or its release notes and `COMPLETED.md` say exactly what changed and why (Lugh's review, 7).

### The fixture

Dagda installs its in-process cluster (`dagda/test/fixtures/cluster.h`: `Cluster`, `ClusterNode`, stepped time,
heads on) as `dagda::test_fixtures`, as Dannan installs its own, so Lugh's and Lir's multi-node tests use it and keep
no fixture of their own (rule 3).

## What stays Dannan's (to the Dannan session)

- A partition view, majority or minority of the roster this node knows (Lir N3, Lugh's N10): one rule, in Dannan,
  not three copies. Dagda's own minority behaviour (a commit not reaching k) needs nothing from it.
- Small-message rate and latency between real machines (Lir N1).
- The version rule (Lir N4, Lugh N9), the Dannan session's proposal to Tom, which Dagda supports: a patch release
  keeps the ABI (checked by abidiff against the previous tag on both architectures); the SONAME and package go back to
  major.minor and the CMake package to SameMinorVersion; an ABI change is a minor release, and only that makes Dagda
  re-release; each Dagda release names the Dannan minor it was built on, and Lugh and Lir take that minor. Dagda
  would follow the same rule for its own releases once its contract is frozen. Dagda `0.1.5` moves to Dannan `0.5.6`
  (gated on the Mac at that pin; parked behind this design) and to the minor-version rule if Tom takes it.
- CPU admission by class: Dagda's `store/cpu_pacing.h` (Macha's background pacing) moves down into a class-aware
  worker pool in Dannan, which Lugh's Argon2 storm needs too (its N6); Dagda then links it (the Dannan session's
  proposal).
- A small-message mode for `dannan-bench` (64 B to 1 KiB, calls and notify, p50 and p99), which answers Lir's N1;
  Dagda's measurement (above) reuses its harness.
- A handler that answers later (found by slice 0's log prototype, 2026-11-13): a replica's group commit answers each
  append after its sync, but `RequestHandler` answers synchronously and holds one of its class's bounded workers while
  it waits (`dannan/net/scheduler.h:101-103`). On the Mac at 8,000 single-message calls a second a replica grouped at
  most 2 calls, and Dannan refused 309,385 of 479,993; batched per tick, nothing was refused. Dagda's log (slice 4)
  needs a reply handle callable once from any thread, so thousands of calls can wait on a sync while holding no
  worker. Raised with the Dannan session; its place in Dannan's order is Tom's.
- Not needed from Dannan: a larger advert cap (Dagda aggregates, and puts fast generations on channels), fan-out
  (Lir's own, on Dannan's channels).
- The write floor already asks Dannan: every replica storing a write, a claim, a delete or a head commit calls
  `Identity::verify` (`store/distributed_store.cpp:697`, `:2471`; `store/storage_server.cpp:125`, `:356`;
  `heads/exchange.cpp:1036`). The cross-node case with a directory principal decided by an installed `ScopeSource`
  is `StoreScope.AnInstalledSourceRefusesOnePrefixOnEveryReplica` (`store_scope_test.cpp:124`), with claims in
  `StoreScope.ClaimsAreCheckedAsWritesAre` (`:179`).

## What Macha sees

Nothing in behaviour, if the gates hold (the Macha session's review, 2026-11-13):

- Its every-node space keeps its path: the exchange's every-node sends, its two adverts, its wire bytes (the first
  space's payloads carry no space id, `heads/messages.h:78-100`), its formats. Every new placement is a branch it
  never takes. That path is not idle: on the live nodes it carries about 1 commit a second sustained under import,
  bursts of 10 a second, deltas of 0.2 to 1 KB on a 76 KB tree root, and a handful of merges in 12 days.
- New nouns (logs, built-in merges, the catalogue) are new code and new message numbers.
- Contract version 1 stays served.
- Leases: Macha calls no lease noun at the port (the capability report). Slice 3 moves the lease records onto a head
  space and keeps `Context::Leases` as it is, so anything built on the template's calls is unchanged.
- CPU pacing moving into Dannan changes the code under Macha's background work (repair's credit is bandwidth times
  fraction times the CPU scale; it is what keeps repair off the viewer). That move is Dannan's release and Dagda's
  relink, with Macha's pacing cases (`test_repair_is_paced_not_stopped_while_higher_classes_stay_busy`,
  `maintenance_trace`) and a baseline run on the nodes as its proof.
- Packaging: under the Dannan session's version rule Macha's deb depends on `libdannan0.5` and `libdagda0.1` at a
  minimum patch, not an exact one.
- Proof, for each tag that changes a path Macha runs (the exchange's sends, the store): the `HeadsWireFixture` and
  every-node cases unchanged, `dagda_tests` whole, the Macha session's own gate on the tag, and Macha's baseline
  comparison on the nodes (0.94.2's figures) before Macha takes it. A pacing regression shows only under real load.

What it touches in the port's order: `0.1.5` (Dannan 0.5.6, the Debian packages) is on the port's critical path,
because Macha must take it before the flag day, not relink under live data after; so it comes first in slice 0, ahead
of the measurement. Beyond that, nothing before Macha's stage S, 8a and 8b. Slice 0's benchmarks on corvus-develop
take turns with Macha's gate there, by message, as agreed.

## Order

Each slice is a tag with its gates; each noun has named cases written from this design before its code, as Macha's
were. Sessions, not dates: one session is about a day of this project's work as it has gone.

| Slice | What | Unblocks | Sessions |
|---|---|---|---|
| 0 | `0.1.5` first (Dannan 0.5.6, Debian packages; on the port's critical path), then the measurement (above, 1 to 3, with Lir's log prototype and the 1,280 count) | Macha's flag day; formats; Lir N4 | 3 |
| 1 | Node-local spaces (D3), the change notice (D4), the fixture installed (D13); the tag records v1 as frozen at `0.1.4` | Lugh P8 starts (grants, TOTP, indexes); Lir's seq counter | 3 |
| 2 | capable(capability) and replicas(N, scheme, capability), a space under a configured-member domain, per-space k (D1, D2), the advert aggregate (D11) | Lugh's lugh/secrets on every Lugh node, unreadable elsewhere; Lir's per-shard spaces | 4 |
| 3 | Register map and counter map merges (D6, D7), head-backed leases (D5) | Lir's retained and leases; Lugh's JWKS, back-off, key rotation | 3 |
| 4 | Logs (D9) at the measured rate (D8); contract v2 frozen | Lir's inflight, session queues and history; Lugh's audit chain; Lir pins | 5 |
| 5 | The domain catalogue and blind holders (D10), as contract v3 beside v2 | Lugh's genesis and capability-following keys | 4 |
| - | Alpine (D14): not now (Tom, 2026-11-13, as for Dannan's Alpine CI); with Dannan's Alpine work, since Dagda builds on Dannan | Raven | - |

Slices 1 to 3 run beside Macha's port (Macha builds on `0.1.4` now, and on `0.1.5`). A defect the port finds still
comes first, as Tom ruled.

## Ruled (Tom, 2026-11-13)

1. The log is a noun of Dagda's.
2. A log's tombstone window is 10 minutes, Lir's dedup window: a split longer than that can deliver a QoS 2 message
   twice, a stated limit.
3. The order as above, slice 0 first; contract v1 frozen at `0.1.4` and served beside v2; v2 frozen at slice 4; the
   catalogue as v3.
4. The built-in register-map and counter-map merges, retained as a register-map space; the advert aggregate in Dagda,
   generations on channels, Dannan's cap unchanged.
5. Later the same day: 0.1.5 builds on Dannan 0.5.7, the ABI-promise release (SONAME `libdannan.so.0.5`,
   SameMinorVersion), not 0.5.6; it is tagged after the x86-64 Docker build (debs, tests, lintian) and the arm64 build
   on corvus-develop (GCC Release with warnings as errors, tests, debs) both pass. Slice 0's measurement starts on the
   Mac meanwhile. Alpine is not now, for Dagda as for Dannan.
