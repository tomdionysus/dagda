# Repair, redesigned: one queue of copies to make (2026-11-16)

Tom, 2026-11-16, on the pull starved by a sending push and the two patches offered for it (a reserved share of the
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
   case. The second path has no test yet.
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
   (Tom, 2026-11-04 and 2026-11-14).
2. **One queue of transfers, ordered by copies left.** An item is "object X onto owner Y". Fewest live copies
   first; within a count, id order. Direction is not a scheduling concept, so neither can starve the other. An
   object with more copies waiting behind an endless supply with fewer is the right answer, not starvation.
3. **Each copy moves once.** One party is responsible for each transfer. The form depends on check B below:
   - If every owner knows what is referenced, the receiver pulls and a holder never initiates. No new message, and
     diff only holds as it stands.
   - If an owner may not know, a holder must be able to initiate, and then the receiver agrees before bytes move:
     the sender offers ids, the receiver declines what it holds or is fetching. That is a question to a peer, which
     the "diff only, no probe" ruling does not allow as written: **it goes to Tom before it is built.**
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
  is set aside for this by Tom's word above.
- **The wire** changes only if check B needs the offer, or check C a multi-object message. No backward
  compatibility is owed (Tom, 2026-11-04).

## Stages

Each is test-first, gated on a Pi (one whole-suite run at its last commit), and measured on the benchmark of stage 0
against the stage before, on the same Pi and kernel, both stores rerun where Macha's is the bar.

**0. Measure and check, no design code.**
- The benchmark: the harness's workload 7 (`2026-11-15-side-by-side-harness.md`), written for both stores and
  reviewed by the Macha session. A node away while objects are written, then back; and a node rejoining empty.
  Figures: time to full replication, MB/s, bytes sent against bytes needed (the double transfer shows here), round
  trips, the foreground reader's latency while repair runs. Large extents and small values, each.
- Latency matters to it: serial pulls only show on a link with a round-trip time. Proposed: `tc netem` on the
  loopback of one Pi claimed whole (delay and rate as the plan's inverbeg to macnessa figures), with a note on the
  board, removed in the script's trap. That shapes one Pi's loopback, not the shared network, so it needs no
  second Pi.
- Check A: how often the same copy is pushed and pulled (from the benchmark's bytes).
- Check B: whether every owner knows an object is referenced, for Macha's domain and for Lugh's and Lir's (read
  the inventory's source, the owner's ReachabilityProvider, and ask the three sessions).
- Check C: whether a multi-object fetch or put exists, or must be added.
- A failing test for the credit path of the starvation (finding 1), beside the one for the count.

**1. The survey keeps holders.** `AvailabilitySurvey` and the service publish, per peer, what it holds of what
this node lacks. Memory is the sum of the lists, as `PeerLacks`.

**2. One queue by copies left.** Repair's two walks and their cursors become one ordered walk over transfers.
Both starvation tests pass here. Retries keep their backoff.

**3. Each copy once.** As check B decides; to Tom first if it needs the offer.

**4. The window, and the count gone.** Concurrent transfers both ways against the byte limit; small objects
batched; `operation_budget` removed. The shape of `Repair`'s steps agreed with the Macha session first.

## Not decided

- The offer (stage 3), if check B needs it.
- Whether the claim walks' restorer (`Repair::Restorer`, an extent of credit a missing claim) joins the queue. It
  restores claimed objects through the same credit; it is left as it is until stage 2 shows whether it should.
