# Repair, redesigned: one queue of copies to make (2026-11-16)

The maintainer, 2026-11-16, on the pull starved by a sending push and the two patches offered for it (a reserved share of the
16-call budget; push and pull taking turns): "an arbitrary 16 slots and bullying", then "We're not building a toy or
playing least-effort whack a mole here. What's the correct, performant way to do it?", and to the design below: "yes,
I think we can do that."

Nothing here is measured yet. What is wrong was read from the code; the design is argued from it. Stage 0 measures
before anything is built.

## What is wrong today

All in `DistributedStore::repair_step` (`dagda/store/distributed_store.cpp:3111`) and `Repair` (`dagda/store/repair.cpp`),
moved from Macha and the same on its live nodes.

1. **The pull starves, two ways.** The step runs the push, then the pull on what is left. The push can spend the
   16-call count (`reserve_operation`, :3170), which is the defect found on 2026-11-16. It can also run out of byte
   credit, and then the pull is skipped for the step (`!result.credit_limited`, :3418). On a real link sending
   16 MiB extents the credit ends a step before the count does, so a reserved call would not have fixed the common
   case. The second path is proven: `StoreRepair.DISABLED_APushOutOfCreditDoesNotHoldBackThePull` (`39fe114`),
   run once on node-a and seen failing as predicted (the object not pulled, `credit_limited=true`, one extent
   pushed); disabled until stage 2 makes it pass.
2. **Urgency is ignored.** Both walks go in id order. An object down to one copy waits behind objects with two.
3. **Both ends do the same work.** A holder pushes an object to an owner that lacks it while that owner pulls it.
   Nothing coordinates them. How often the bytes cross twice is not measured (stage 0).
4. **Pulls are serial.** One `get_remote` an object, waited for (:3470, :3530). A node that rejoins empty is bound by
   round trips, not the link; worst for small values (Lugh's records, Lir's messages).
5. **Pushes stop and wait.** A batch of up to `sends_in_flight` (8) or two extents goes out through `call_all`, and
   the step waits for the slowest before starting more (:3286, :3360).
6. **The survey discards what repair needs.** `survey_by_diff` diffs this node's held trie against every peer's, and
   keeps per peer what it lacks of what is held here (`PeerLacks`). For what this node lacks it keeps only
   available, unavailable or unknown (`ledger/availability.h:39`): not which peers hold it, nor how many.
7. **Three budgets for one cost.** The byte credit (the link's measured rate, `idle_bandwidth_fraction`, shrunk by
   CPU), the duty share against higher classes (95:5, slice 25 ms), and `operation_budget` (16, "calls one step may
   make") with a scan budget derived from it (x4, clamped 16..64). A call may be a miss or a 16 MiB extent, so the
   count tracks neither bytes nor time. On an idle node `DutyShare::should_yield` returns false
   (`../dannan/dannan/sched/duty_share.cpp:61`), so the count is the only bound on work that moves no bytes.

## The design

1. **The survey keeps who holds what.** For every referenced object short of its copies: the owners that hold it.
   The diff already learns this; it is kept instead of discarded, per peer and sorted, as `PeerLacks` is. That
   gives each object a copy count and each pull a source. Still diff only: nothing walks the store or probes a peer
   (the maintainer, 2026-11-04 and 2026-11-14).
2. **One queue of transfers, ordered by copies left.** An item is "object X onto owner Y". Fewest live copies
   first; within a count, id order. Direction is not a scheduling concept, so neither can starve the other. An
   object with more copies waiting behind an endless supply with fewer is the right answer, not starvation.
3. **Each copy moves once.** One party is responsible for each transfer. The form depends on check B below:
   - If every owner knows what is referenced, the receiver pulls and a holder never initiates. No new message, and
     diff only holds as it stands.
   - If an owner may not know, a holder must be able to initiate, and then the receiver agrees before bytes move:
     the sender offers ids, the receiver declines what it holds or is fetching. That is a question to a peer, which
     the "diff only, no probe" ruling does not allow as written: **it goes to the maintainer before it is built.**
4. **A sliding window, not steps.** Transfers stay in flight up to a byte limit; each completion starts the next
   most urgent. The limit is the credit's burst (five seconds of the link or one extent, `repair.cpp:110`): no new
   number. A share of the window a peer, so one slow peer cannot hold it. Small objects travel many to a message
   (check C).
5. **Two budgets.** Bytes on the wire come out of the credit, whichever way they go and whether or not the call
   found anything. Time comes out of the duty share. `operation_budget` and the scan budget go. The loop looks at
   stop, yield and the lists between starts, so no step needs a bound of its own on an idle node.

The laws stand: a viewer and the loader pace repair through the share, and a node that is always busy still
repairs (law 3). What is in flight when repair yields completes and is kept, as now.

## What it changes for others

- **Macha's port** drives `Repair`'s public steps from its own pass (`earn_credit`, `may_start`, `begin_turn`,
  `claim_walks`, `transfer`, `settle`) and its Status reads the gates (ran, share, quiescent, credit). A window
  that outlives a step changes that shape: agreed with the Macha session before stage 4, not after.
- **The trace cases ported from Macha** assert push then pull within a step. They change with the design; none is
  deleted, and the test mapping (`2026-11-05-test-mapping.md`) and Macha's retirements follow each change.
- **Macha's live nodes (0.94.2)** keep today's repair, starvation included, until the cutover. Macha's store is
  deleted on its port branch, so there is nowhere to fix it first; the rule "a Macha defect is fixed in Macha first"
  is set aside for this by the maintainer's word above.
- **The wire** changes only if check B needs the offer, or check C a multi-object message. No backward
  compatibility is owed (the maintainer, 2026-11-04).

## Stages

Each is test-first, gated on a Pi (one whole-suite run at its last commit), and measured on the benchmark of stage 0
against the stage before, on the same Pi and kernel, both stores rerun where Macha's is the bar.

**0. Measure and check, no design code.**
- The benchmark: the harness's workload 7 (`2026-11-15-side-by-side-harness.md`), written for both stores and
  reviewed by the Macha session. A node away while objects are written, then back; and a node rejoining empty.
  Figures: time to full replication, MB/s, bytes sent against bytes needed (the double transfer shows here), round
  trips, the foreground reader's latency while repair runs. Large extents and small values, each.
- Latency matters to it: serial pulls only show on a link with a round-trip time. **The maintainer, 2026-11-16: two Pis and
  the real link between them**, as the harness plan says (60 ms RTT, about 10 MB/s, `tc netem` on both Pis'
  interface): a note on the board first, both claimed whole, the shaping removed in the script's trap. So the
  harness must run a cluster across two processes on two machines, which it does not yet.
- Check A: how often the same copy is pushed and pulled (from the benchmark's bytes).
- Check B: whether every owner knows an object is referenced, for Macha's domain and for Lugh's and Lir's (read
  the inventory's source, the owner's ReachabilityProvider, and ask the three sessions).
- Check C: whether a multi-object fetch or put exists, or must be added.
- A failing test for the credit path of the starvation (finding 1), beside the one for the count.

**1. The survey keeps holders.** `AvailabilitySurvey` and the service publish, per peer, what it holds of what
this node lacks. Memory is the sum of the lists, as `PeerLacks`.
**Done (`2f8bda1` the cases, `502cf57` the code).** `ledger::PeerDiff::holds` and `ledger::PeerHolds`, published
by `AvailabilityService::peer_holds()` beside `peer_lacks()`, from the same diffs and nothing more asked: what a
peer holds that nothing refers to is left out. A survey that asks nobody (a gain alone) leaves the last one
standing. Both cases seen failing before the code; gate at `502cf57`, node-c, 1 core: 1123 ran, 1120 passed, 3
skipped, 0 failed, 0 warnings. Repair does not read it yet: that is stage 2. Built after the baseline's binaries
(`7d12ca8`), so the baseline is still today's repair.

**2. One queue by copies left.** Repair's two walks and their cursors become one ordered walk over transfers.
Both starvation tests pass here. Retries keep their backoff.

**Stage 2, designed (2026-11-16), not built.** Inside `DistributedStore::repair_step`, keeping its signature,
its result and its callers:

- *The queue.* Built once for each set of lists (they are immutable and compared by identity, as `push_source`
  is now) by one ordered merge across the peers' sorted lists. A push candidate is an id in some peer's lacks:
  its copies are this node's plus every answering peer that does not lack it. A pull candidate is an id in some
  peer's holds: its copies are the peers that hold it. Each goes into the bucket of its copy count, in id
  order, marked push or pull (an id is never both). What this node lacks and no answering peer holds goes last:
  the unavailable passed over and traced as now, the unknown tried as now. Memory is the sum of the lists.
- *The walk.* Buckets in order of copies, a cursor in each; what needs nothing is passed without spending the
  scan budget, as now (a pushed pair remembered, a pulled object held). A push item is planned as now (the first
  owners with room, what each is known to hold, the local read, the drop of a copy this node should not keep);
  a pull item is fetched as now, through `get_remote` (a known source is stage 4's). Retries keep their backoff,
  one map for both kinds, those due going first as now.
- *The budgets.* The credit is spent in queue order, so whatever is most at risk gets it, whichever way it
  travels; the pull is no longer skipped when a push ends the step out of credit. The call count and the scan
  budget stay until stage 4 and only end a step: they can no longer hold one direction behind the other.
- *What a step reports.* `push_examined`, `pull_examined`, `complete`, the pass count and the phase figures keep
  their meaning; `complete` is every bucket walked with nothing stopped.
- *What changes for the cases.* The traces ported from Macha that show a step's pushes before its pulls show
  queue order instead. Each such case is named to the Macha session as it changes; none is deleted.
  `StoreRepair.DISABLED_APushOutOfCreditDoesNotHoldBackThePull` is enabled here.

**3. Each copy once.** As check B decides; to the maintainer first if it needs the offer.

**4. The window, and the count gone.** Concurrent transfers both ways against the byte limit; small objects
batched; `operation_budget` removed. The shape of `Repair`'s steps agreed with the Macha session first.

## Stage 0, as answered

**Check B, for Macha (the Macha session, 2026-11-16, from Macha's develop source; not observed or tested): an
owner can lack the knowledge while a holder has the object, for a while.** Each node builds its referenced set
itself from its own metadata head (`NodeHorizonBuilder::inventory`); nothing hands it another node's. In steady
state every node derives the same inventory. The windows: replication lag (the writer references a new file's
extents at once, an owner only when the metadata reaches it and its inventory is rebuilt); a build that cannot
finish (an unreadable tree node or catalogue root keeps the older inventory); divided heads under a partition. A
pull alone converges in all three once the owner learns. A holder that initiates closes the window sooner, and only
it can act while an owner's build is stuck; on the degraded cluster (single-copy writes accepted, the maintainer 2026-09-28)
the holder is often the only copy in exactly that window. So a holder must be able to initiate, and design 3 takes
its second form. **The maintainer, 2026-11-16: the offer is allowed**, as a handshake at the moment of transfer; the diff
stays the only way repair finds what to do.

The second starvation path is in Macha's develop too (its `distributed_store.cpp:2939`, the same guard), untested
there as here.

**Check B, for Lir (the Lir session, 2026-11-16, from its approved step 5 design; Lir writes nothing to Dagda
yet): the same window, for payloads over 64 KiB only.** Messages and retained values are inline in log, queue and
register-map entries and need no objects. A payload over 64 KiB (up to 1 MiB by default) is written as an object
at Replicated and claimed, then the log entry naming it is appended: so the object is on its replicas before the
reference reaches anyone, and its holders are placed apart from the log's replicas, so a holder may learn of the
reference only through the claim. A publisher that dies between the write and the append leaves an object held
and claimed but never referenced, which repair leaves alone. Sizes for the benchmark, none measured: entries 0.3
to 1.5 KiB, objects 64 KiB to 1 MiB and rare, about 10,000 publishes a second across a cluster (the maintainer, 2026-11-04).

**Check B, for Lugh (the Lugh session, 2026-11-16, from its P8 plan; Lugh stores nothing in Dagda yet): it does
not arise.** Lugh keeps everything in head spaces (records of a few hundred bytes, capped at 256 KiB) and writes
no DATA objects of its own. Its assumption: values stay in the head; if Dagda ever moved large values out into
objects of its own accord, the window would be Dagda's to close.

**Check C: there is no multi-object fetch or put.** `get_object` (516) and `put_object` (517) carry one object
each (`dagda/store/messages.h:39-41`), so small values need one added at stage 4. `have_objects` (552) already
asks a peer about many ids in one message and is answered per id: it is the offer's shape, though it answers only
"held", not "being fetched" or "no room", so stage 3 extends it or adds a sibling.

**Found by the benchmark's smoke test (node-c, 2026-11-16, two processes on loopback, 20 x 64 KiB): Dagda's own
maintenance loop learns a new reference only when its collector next runs.** The inventory the survey and repair
work from is rebuilt only inside the GC pass (`Node::maintenance_loop`, `dagda/node.cpp:1086-1093`), which rests
`gc_interval` (5 minutes) after a sweep that completed with nothing deferred. Nothing pulls that forward when the
owner's provider moves to a new generation: `wake_maintenance` wakes the loop, not the collector. Seen: the
joining node took 300.2 s to hold 1.3 MB, all of it the rest (61 repair turns ran, 239 were quiescent); with a
two-minute limit nothing moved at all. Prompt replication covers the ordinary write, so this bites what it
missed: a write while a peer was away, a node rejoining, a degraded single-copy write. It is in Dagda's loop
only: Macha's port drives its own pass until stage 8c. **A gap against Macha, fixed (`0d048e0`).** Macha's pass
runs its inventory stage whenever repair is due and rebuilds on a generation change (`maintenance.cpp:611-627`
at f0597a4, read by the Macha session). The cause here was narrower than the sweep's rest: the loop's first pass
found no provider registered, which rests the collector as a settled sweep does, so a provider registered once
the node ran was not read for five minutes. The loop now takes the collector's inventory step before each repair
turn, held back while metadata is not ready and never the first, and makes the sweep due again when what is
referenced changed. Two cases, both seen failing on the old loop and passing on the new (node-c):
`NodeMaintenance.AProviderRegisteredLaterIsReadWhileTheCollectorRests` and
`.ANewGenerationIsReadWhileTheCollectorRests` (`054b604`, `f77b59d`). Gate at `0d048e0`, node-c, 1 core: 1120
ran, 1117 passed, 3 skipped, 0 failed; the second case was added after and run on its own. The same run showed both ends working at once: the holder pushed 16 of the 20 and the joiner
pulled 4.

## Not decided

- Whether the claim walks' restorer (`Repair::Restorer`, an extent of credit a missing claim) joins the queue. It
  restores claimed objects through the same credit; it is left as it is until stage 2 shows whether it should.
