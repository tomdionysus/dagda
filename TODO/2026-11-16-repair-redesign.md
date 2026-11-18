# Repair, redesigned: one queue of copies to make (2026-11-16)

## Status (2026-11-17)

| Stage | What | State |
|---|---|---|
| 0 | Measure and check | Done. Baseline measured; the three checks answered |
| 1 | The survey keeps who holds what | Done, gated (`502cf57`) |
| 2 | One queue, fewest copies first | Done, gated (`2a8d24b`); measured with stage 3 |
| 3 | Each copy once (the offer) | Done, gated (`d119d14`), measured: each copy once, unpaced rejoins a third to a half faster, parity under a viewer |
| 4 | A window of transfers; the call count gone; pulls fetched many to a message | Built and gated (`c12e36d` to `79e055d`); measured against stage 3: small rejoin three times faster, under a viewer faster, **unpaced large rejoin about 3 s slower: one cause fixed (the link's estimate), the rest open, next** |

Last full gate: see `TODO/ACTIVE.md`. Everything is on `feature/macha-store-gaps`, local, nothing pushed.

## The maintainer's rulings

- On the pull starved by a sending push and the two patches offered for it (a reserved share of the 16-call
  budget; push and pull taking turns): "an arbitrary 16 slots and bullying", then "We're not building a toy or
  playing least-effort whack a mole here. What's the correct, performant way to do it?", and to the design
  below: "yes, I think we can do that."
- **The offer is allowed** (stage 3): a holder may ask an owner which ids it wants before sending bytes, as a
  handshake at the moment of transfer. The diff stays the only way repair finds what to do; "repair is diff
  only, no probe" otherwise stands.
- **The benchmark runs on two Pis over the WiFi between them as it is**, unshaped, the sizes a tenth of the
  harness plan's.
- On the measured double transfer: "seems a hell of a waste?" Stage 3 removed it.
- On stage 3's regression under a viewer and the fix recommended for it (2026-11-17): "Yes."
- Relayed by the Macha session (2026-11-16 night): `offer_bytes` and `in_flight` join Macha's /status repair
  section at the cutover, announced with the other cutover API changes.

## What was wrong (as the code stood at `7d12ca8`)

In `DistributedStore::repair_step` and `Repair`, moved from Macha and the same on its live nodes (0.94.2).

1. **The pull starved, two ways.** The step ran the push, then the pull on what was left. The push could spend
   the 16-call count, or run out of byte credit, after which the pull was skipped for the step. Both proven by
   cases. *Fixed by stage 2.*
2. **Urgency was ignored.** Both walks went in id order. *Fixed by stage 2.*
3. **Both ends do the same work.** A holder pushes an object to an owner that lacks it while that owner pulls
   it. Measured: the normal case (below). *Fixed by stage 3.*
4. **Pulls are serial**: one `get_remote` an object, waited for. *Stage 4.*
5. **Pushes stop and wait**: a batch of up to 8 sends or two extents, and the step waits for the slowest.
   *Stage 4.*
6. **The survey discarded what repair needs**: which peers hold what this node lacks. *Fixed by stage 1.*
7. **Three budgets for one cost**: the byte credit, the duty share, and `operation_budget` (16 calls a step)
   with a scan budget derived from it. Measured: on small objects the count, not the link, sets the rate.
   *Stage 4.*

## The design

1. **The survey keeps who holds what**, from the diffs it already makes. Each object gets a copy count.
2. **One queue of transfers, ordered by copies left**; within a count, by id. Direction is not a scheduling
   concept, so neither can starve the other.
3. **Each copy moves once.** A holder must be able to initiate (check B), so the receiver agrees before bytes
   move: the sender offers ids, the receiver declines what it holds or is fetching.
4. **A sliding window, not steps.** Transfers stay in flight up to a byte limit, each completion starting the
   next most urgent. The limit is the credit's burst (five seconds of the link or one extent): no new number.
   A share of the window a peer. Small objects travel many to a message.
5. **Two budgets.** Bytes on the wire come out of the credit; time comes out of the duty share. The call count
   and the scan budget go.

The laws stand: a viewer and the loader pace repair through the share, and a node that is always busy still
repairs (law 3). What is in flight when repair yields completes and is kept.

## What it changes for others

- **Macha's port** drives `Repair`'s public steps from its own pass (`earn_credit`, `may_start`, `begin_turn`,
  `claim_walks`, `transfer`, `settle`) and its Status reads the gates. Stages 1 to 3 leave those alone, and it
  confirmed (2026-11-16, port at `deceac1`) that stage 3 asks nothing of it: its pass calls
  `repair.transfer(turn, repair.lists())`, which carries `holds` and takes the offers' bytes from the credit, and
  `dagda::Node` hands its server the offer book. Stage 4's window outlives a step: its shape is agreed (below).
- **The cases ported from Macha**: none changed or removed through stage 3. The Macha session is told of each
  that does; stage 4 names the trace-fixture cases whose order the window changes.
- **Macha's live nodes** keep today's repair until the cutover; Macha's session has recorded it as a known live
  defect. The rule "a Macha defect is fixed in Macha first" is set aside for this by the maintainer's word.
- **The wire** gained the offer at stage 3 (771, 772) and gains a multi-object message at stage 4. No backward compatibility is
  owed (the maintainer, 2026-11-04).

## What was built

**Stage 1 (`2f8bda1` the cases, `502cf57` the code).** `ledger::PeerDiff::holds` and `ledger::PeerHolds`,
published by `AvailabilityService::peer_holds()` beside `peer_lacks()`: per peer that answered, what it holds of
what is referenced and this node lacks. Nothing more is asked of a peer; what a peer holds that nothing refers
to is left out. A survey that asks nobody (a gain alone) leaves the last one standing. Cases:
`LedgerAvailability.TheSurveyKeepsWhoHoldsWhatThisNodeLacks`,
`StoreRepair.TheSurveyPublishesWhoHoldsWhatThisNodeLacks`.

**Stage 2 (`cf2ee55` the cases, `1eed251` the rewrite, `bb912f2`, `2a8d24b`).** `repair_step` keeps its
signature, its result and its callers; `RepairLists` gains `holds`, with a default.
- *The queue* (`RepairState::queue`) is built when a list changes: a push is an id in some peer's lacks, its
  copies this node's plus every answering peer that does not lack it; a pull is an id this node lacks, its
  copies the peers known to hold it. Sorted by copies, then id. A pull whose holders are not known (nobody
  reachable holds it, a peer could not say, or lists made without `holds`) sorts first.
- *The walk*: consecutive pushes go out together; a pull, or an object needing nothing, sends what is waiting
  first. Planning, fetching, the local drop and the retries' backoff are as before. Retries due go first.
- *A list changing under a pass*: what the pass had passed is left out of the rebuilt queue, so nothing is sent
  again, while what is new is met in its order; the pass after it is over everything listed.
- *A copy that did not arrive* does not end the step; the pass comes back to it next step and is not complete
  while there is one.
- Cases: `StoreRepair.APushOutOfCreditDoesNotHoldBackThePull` and `.TheObjectWithFewestCopiesGoesFirst`, both
  seen failing on the old step. `StoreRepair.APushKeepsItsPlaceAcrossANewList` (existing) caught the first
  rewrite resending after a new list.
- To watch on the benchmark: pushes and pulls alternate in one walk, so pushes are batched only while no pull
  sits between them; the queue is rebuilt and sorted whenever a list changes.

**Stage 3 (`deafb7d` the wire pair, `21013d6` the cases, `d2872de` the code).**
- *The wire*: `offer_objects` (771) is the ids as a presence names them, at most `max_offer_batch`;
  `offer_objects_reply` (772) a byte each, `OfferAnswer` wanted, held, fetching or no room. Control lane, any
  class. Case: `StoreMessages.AnOfferNamesItsIdsAndItsAnswerSaysOfEachWhetherItIsWanted`.
- *The receiver*: `OfferBook` (`store/offer_book.h`), the store's, handed to the server. An offer is answered
  held from the index; no room when no disk is online or an extent would not fit; fetching when this node's
  repair is fetching the id or another peer holds an unexpired promise; otherwise wanted, and the promise is
  (re)made for `object_transfer_call`. A put that lands ends the promise. Repair's pull asks the book first
  (`begin_fetch`): a promise from a peer present and unexpired defers the pull to its retry backoff; otherwise
  the pull is marked in flight until it ends.
- *The sender* (`push_some`): plans each object's keepers and the owners that lack it from what is known, reads
  nothing, sends one offer a peer for the batch, then reads and sends only what is wanted, in order, until the
  credit, the calls, admission or the batch's two extents run out; what was offered past there is offered again
  next, renewing the promise. Held: a keeper, remembered in `pushed`. Fetching: `deferred`, retried on the
  backoff. No room: passed. Unanswered: a copy that did not arrive (no backward compatibility is owed). Offers
  are not counted against the call count; their bytes are `RepairResult::offer_bytes`, which `Repair::transfer`
  takes from the credit beside the objects' and which settle nothing.
- Cases, each seen failing at `21013d6`: `StoreRepair.APushToAPeerThatHoldsTheObjectMovesNoObjectBytes` (and no
  probe), `.APeerFetchingAnObjectDeclinesItsOfferAndIsSentNothing`,
  `.APeerThatWantedAnOfferedObjectFetchesItOnlyIfTheCopyNeverComes`, `.ARejoiningNodeIsSentEachObjectOnce`.
- *The fix after measuring* (`63e76a0` the cases, `0dfe22a`, `d119d14`; the maintainer's "Yes", 2026-11-17): a holder
  offers only what its credit and the batch's two extents will send in this step, each object sized from the
  index and never read (`LocalStore::payload_size` and `StoragePool::payload_size`: the pack index, or the loose
  file less its header), so a promise covers a copy about to move; and a pull a promise holds back is looked at
  again after `retry_first` each time (`RepairState::retry_soon`), not on the doubling backoff. Cases, both seen
  failing on `d2872de`: `StoreRepair.AHolderOffersOnlyWhatItsCreditWillSend` (it proved the mechanism: a holder
  with credit for one object promised four and the joiner fetched none) and
  `.APromisedPullIsLookedAtAgainEachSecond` (a stepped clock: fetched at 7 s, not at the promise's 5 s). Macha's
  `.ATransferWaitsForItsCredit` caught the first version sizing by the sealed record.
- No ported case changed. For Macha's port: the server needs `Stores::offers` (the node passes its store's) or
  every offer of an absent object is wanted; a node that answers no offer receives no push.

**Beside the stages, found by the benchmark's smoke test: Dagda's own loop read a new reference up to five
minutes late (`0d048e0`).** The loop's first pass found no provider registered, which rested the collector as a
settled sweep does, so a provider registered once the node ran (or a new generation) was not read until the rest
ended. Macha's pass rebuilds its inventory in any pass where repair is due (`maintenance.cpp:611-627` at f0597a4,
read by the Macha session): a gap against Macha. The loop now takes the collector's inventory step before each
repair turn. Cases: `NodeMaintenance.AProviderRegisteredLaterIsReadWhileTheCollectorRests` and
`.ANewGenerationIsReadWhileTheCollectorRests`. Macha's port is unaffected: it drives its own pass.

## What was measured (stage 0)

The baseline, today's repair in both stores, is in the harness plan (`2026-11-15-side-by-side-harness.md`,
"Workload 7, the baseline"): node-b and node-d over 2.4 GHz WiFi at about 2.9 MB/s, 2026-11-16.

- Dagda's repair is at least as fast as Macha's in every scenario.
- **Both stores send each object about one and a half to two times** when nothing paces the holder (a sends 1.5
  to 2.05 times the bytes b needed; Dagda made 43 transfers for 26 objects; Macha's own byte counters agree).
  Under a viewer on the holder each object moves once (1.09) and the rejoin is no slower: about 40% of the
  link's time in an unpaced large rejoin goes on copies nobody needed.
- **On small objects the 16-call count sets the rate**: 410 objects at 16 a step and a step a second is 25.6 s,
  against 25.8 s measured.
- Expected, to be checked: stage 3 takes an unpaced large rejoin from about 80 s towards the 38 s the link
  allows; stage 4 takes the small rejoin from 26 s towards 9 s.
- Not in these figures: the cost of serial pulls over a link with a real round trip; the plan's full sizes.

The checks:
- **A, the double transfer:** measured, above.
- **B, who knows a reference:** in Macha an owner can lack the knowledge for a while (replication lag, a build
  that cannot finish, divided heads), and on the degraded cluster the holder is often the only copy in exactly
  that window; in Lir the same for payloads over 64 KiB; in Lugh it does not arise (everything is in head
  spaces). So a holder must be able to initiate. Each from that session's reading of its code or design.
- **C, multi-object messages:** none exists for fetch or put (`get_object`, `put_object` carry one each);
  `have_objects` asks about many ids at once.

## What stages 1 to 3 measured (2026-11-16 23:21 to 2026-11-17 00:15Z)

Old against new in one session: the baseline's Dagda (`7d12ca8`, `build-old/`, store `dagda-old`) and the tip's
(`d2872de`), the harness's repair workload, node-b (a, holding; 5 GHz) and node-d (b, joining; 2.4 GHz) over the
WiFi as it is, about 4.8 to 5.9 MB/s raw, round trip 6 to 10 ms average; kernel 6.18.62-v8-va48+ on both; two
cores claimed on each, no page-cache drop (`NOCOLD=1`); three rounds, the order alternating. Figures in
`build/harness/results/2026-11-16-stage3/` (not in the repository). Seconds for b to hold everything:

| Scenario | Old, rounds 1-3 | New, rounds 1-3 | Bytes a sent, old / new |
|---|---|---|---|
| Rejoin, 26 x 4 MiB | 45.1, 44.8, 50.5 | 22.9, 31.9, 34.3 | 163-188 MB / 114.5 MB |
| Rejoin, 410 x 64 KiB | 25.1, 25.9, 24.8 | 13.5, 16.2, 13.6 | 45-55 MB / 28.2 MB |
| Away and back, 13 x 4 MiB | 30.5, 24.5, 20.1 | 21.4, 27.7, 20.8 | 79-98 MB / 57.2 MB |
| Away and back, 205 x 64 KiB | 12.1, 12.1, 13.0 | 7.3, 8.5, 9.7 | 25-26 MB / 14.1 MB |
| Rejoin under a viewer on a, 26 x 4 MiB | 38.5, 51.1, 64.1 | **127.5, 143.1, 163.2** | 119-127 MB / 114.6 MB |

- **Each copy moves once**: the new store's bytes are the objects' and no more in every scenario (pushed plus
  pulled equal the objects needed: 216 + 194 = 410), where the old sent 1.4 to 2 times. Unpaced, the large
  rejoin is about a third faster and the small nearly twice as fast; away and back is faster or level.
- **A regression under a viewer, about three times slower in all three rounds, found here and fixed** ("What
  was built", stage 3's fix). The paced holder offered a batch of 8, b promised all 8 and did not pull them, the
  holder's small credit sent them a slow step at a time, and each deferral doubled b's retry backoff.

Remeasured at `d119d14` the same way (2026-11-17 01:01 to 01:20Z, link 5.7 to 6.7 MB/s, figures in
`build/harness/results/2026-11-17-viewer/`), three rounds:

| Scenario | Old, rounds 1-3 | New, rounds 1-3 | Bytes a sent, old / new |
|---|---|---|---|
| Rejoin under a viewer, 26 x 4 MiB | 34.4, 37.9, 36.4 | 32.5, 38.3, 36.7 | 123-128 MB / 114.3 MB |
| Rejoin, 26 x 4 MiB | 39.2, 36.2, 52.5 | 20.6, 23.7, 29.3 | 176-185 MB / 114.4 MB |

Under a viewer: parity in time, each copy once (pushed 2 to 4, pulled 22 to 24 of 26). Unpaced: the gain kept.
The small rejoin and away-and-back were not rerun at `d119d14`; their figures above are `d2872de`'s. Stage 0
expected an unpaced large rejoin to approach what the link allows sent once: at about 5 MB/s that is 22 s, and
the new store took 21 to 34 s. The small rejoin (13.5 s for 410 objects) is still set by the 16-call count:
stage 4's.

## What stage 4 built (2026-11-17)

Cases first, each seen failing on the code before it (node-c, Dannan 0.8.0 prefix): `c12e36d` (the window's five)
and `79f6f76` (the fetch of many's two); the wire pair `b9aeed7` with its own case.

- *The window* (`ec77dda`). `repair_step(lists, byte_budget, RepairWindow, should_yield)` applies what landed since
  the last step, then tops up the window from the one queue and returns. Pushes are `put_object` calls not waited
  for; a pull is a fetch whose completion has the store's workers store it as it lands. The owner applies both at
  its next step or a drain (`repair_drain`, `Repair::drain`), in the order they started, so traces stay in decision
  order. The window is `Repair`'s credit burst (no new number); one transfer always starts into an empty window. A
  push holds its bytes, a pull an extent (its size is not known until it lands). The call count and the scan budget
  derived from it are gone; offers cover what the credit and the window will send (up to `max_offer_batch`). A copy
  that did not arrive goes again first from the next step (it used to hold the pass's place); a pass is complete
  only when its walk has ended, everything it started has landed and been applied, and nothing waits to go again.
  The window drained from past half to half or less raises the storage event, once a drain.
- *The figures*: `RepairResult` gains `credit_spent` (each push as it starts, each pull as it lands, the offers) and
  `in_flight`; `repair_diagnostics()` gains `offer_bytes` and `in_flight` (for Macha's /status at the cutover).
  `copies_pushed` counts as each copy lands (`c0bd7a4`); a drain records what it reports in the pass's observations
  as a step does (`6e1f4dd`). `settle` says `more_work` while anything is in flight.
- *The fetch of many* (`b9aeed7` the wire, `690dc9c` the code). `get_objects` (773) names the ids a puller wants
  from one holder; `get_objects_reply` (774) carries as many as an extent holds, in order, each sized from the index
  before it is read, and says of each of the rest absent, unavailable or left. Repair fetches what the survey says
  a present peer holds from it many to a message, an owner asked first, one extent of the window a message; what a
  reply left goes again first; what it could not bring is tried alone on the retry's backoff, which asks every
  owner. A fetch of one id stays `get_object`'s. A holder's ids gathered in a step are split over as many fetches
  as the credit and the window cover, an extent each (`6794938` the case, `82e5201`), so a step brings a window's
  worth of large objects and not one extent's. Pushes stay one `put_object` a copy: their sizes are known, so many
  are in the window at once already; `put_objects` waits on the measurement showing it is needed.
- *Cases changed, for the Macha session*: the store cases that stepped repair synchronously let each step's
  transfers land first (`landed_step`); `APushKeepsItsPlaceAcrossANewList` bounds a step by credit (sixteen
  objects' bytes) where the call count did; `APullListNewWhileThePushRunsIsNotPassedOver` holds its pass open with a
  pull in flight, as no scan budget does now; `AFetchOfManyStopsAtAnExtentAndTheRestFollow` (stage 4's own) takes
  an extent of credit a step; `MaintenanceStepsPeers.ATurnKeepsThePeerReadingItBeganWithWhenAsked` waits for its copy to
  land. The trace fixtures' loop (`MachaLoop`) drains after its transfer, as Macha's pass waited for its transfers,
  so no trace fixture's order changed. Nothing removed.

## What stage 4 measured (2026-11-17 20:31 to 21:16Z)

Stage 3 (`d119d14`, kept as `build-s3/`, store `dagda-s3`) against stage 4, the harness's repair workload, node-b
(a, holding) and node-d (b, joining), `NOCOLD=1`, two cores claimed on each, three rounds alternating; the link
7.1 to 9.5 MB/s raw, round trip 6 to 8 ms; kernel 6.18.62-v8-va48+; both Pis reported past under-voltage
(`throttled=0x50000`) before and after. Twice: at `6e1f4dd`, then at `82e5201` (the split, below). Figures in
`build/harness/results/2026-11-17-stage4/` and `-stage4-split/` (not in the repository). Seconds for b to hold
everything:

| Scenario | Stage 3, six rounds | Stage 4 at `6e1f4dd` | Stage 4 at `82e5201` |
|---|---|---|---|
| Rejoin, 410 x 64 KiB | 13.1 to 13.4 | 4.3, 4.1, 4.0 | 3.6, 4.9, 4.9 |
| Rejoin, 26 x 4 MiB | 14.5 to 16.0 | 17.7, 16.6, 19.5 | 19.3, 18.4, 25.5 |
| Rejoin under a viewer on a, 26 x 4 MiB | 23.4 to 31.7 | 17.4, 14.9, 15.0 | 19.2, 12.9, 13.2 |

- **The small rejoin is three times faster** and near the link (5.4 to 7.6 MB/s of 7 to 9.5): the call count no
  longer sets it. Stage 0 expected 9 s at 2.9 MB/s.
- **Under a viewer, faster** (7.3 to 8.5 MB/s in most rounds, against 4.2 to 4.7).
- **The unpaced large rejoin is slower, open**: 4.3 to 6.6 MB/s against stage 3's 6.8 to 7.5. Each copy still
  crosses once (a sends 114 MB in both). What differs: stage 3's joiner pulled 9 or 10 of the 26 while the holder
  pushed the rest; stage 4's joiner pulls 1 to 4 and the holder pushes the rest, more often stopped on its credit
  (`gate_credit` 3 to 16 against 1 to 5). The first explanation tried (a holder's pulls in one fetch a step, one
  large object a pass) was real and is fixed (`6794938`, `82e5201`: a holder's ids split over the fetches the
  credit and window cover, gated), but did not move this figure. A second, that one node's credit is half its link
  estimate and stage 3 was faster by spending both nodes' halves, does not fit the viewer runs, where stage 4
  reaches near the link. **Not yet known; the next step is to find it** (below).

## Stage 4, the shape agreed (built: above)

What changes for a caller of `Repair`'s steps, and what does not:

- *Unchanged*: `earn_credit`, `paced_by`, `may_start`, `turn_wait`, `note_gate`, `begin_turn`, `claim_walks`,
  `lists`, `settle`'s signature, and `transfer(Turn&, const RepairLists&)`'s. The duty share and law 3 stand.
- *`transfer` no longer waits for what it starts.* It tops up a window of transfers (pushes, pulls, offers),
  most urgent first from the one queue, until the window's bytes are spent, the credit is, or the share says
  yield, and returns. The window is the credit's own burst (five seconds of the link, or one extent): no new
  number. Each transfer completes on the store's executor and is kept whatever the turn is doing (as a pull in
  flight is kept today); its completion frees its bytes in the window. Completions do not start new transfers
  by themselves: the next `transfer` does, so pacing stays where it is, in the pass and the share.
  Recommendation, against a pump that refills from completions: it keeps the owner's pass in charge of when work
  starts, which is what Macha's maintenance relies on, at the cost of a pass interval's latency between a
  window draining and refilling (the pass's own interval, 1 s by default, against transfers of seconds).
- *`RepairResult` reports what completed since the last step*, not what one step moved: `bytes_transferred`
  and `offer_bytes` as completions landed, and a new `in_flight` (bytes still in the window). `settle` treats
  anything in flight as `more_work`, so a node never goes quiescent with copies on the wire.
- *`operation_budget` goes* from `RepairOptions` and from `repair_step`, and the scan budget derived from it.
  A step is bounded by the window, the credit and the share. Macha's config key for it, if it has one, is read
  and ignored, or removed: the Macha session's call.
- *Small objects travel many to a message*: `put_objects` and `get_objects` (Dagda's own numbers after 772),
  each carrying up to an extent of objects, so the call count no longer sets the small-object rate.
- *Stopping*: the store's destructor already waits for its calls; a node stopping mid-window loses only what
  had not landed, which the next survey lists again.

**Agreed with the Macha session (2026-11-16 night, its port at `deceac1`), no objection:**
- Nothing in Macha relies on `transfer` finishing its I/O, nor on `remote_operations` or `operation_budget`: the
  16 was a constexpr in its old pass and the port has no key for it, so it simply goes.
- Its stop order stops its services, then `dagda::Node`: what is in flight is the node's to finish or cancel
  before the transport goes. Its Status shows `bytes_transferred` a step later, harmlessly.
- *The wake, to build.* Macha's pass refills one interval (1 s) after `more_work`, so a window that drains
  faster would sit empty. Today the node's "storage" event is raised by a pull that lands and a put received,
  not by a push completing on its sender. So the store raises the storage event when the window drains to half
  or less, once a drain; Macha's pass waits on `maintenance_events()` and refills at once. `settle` needs no
  time.
- *The figures, the maintainer's ruling.* `offer_bytes` and `in_flight` join Macha's /status at the cutover: stage 4
  exposes both through `repair_diagnostics()` and tells the Macha session their names when built.
- When built, the Macha trace fixtures' cases whose order the window changes are named to it, as for stage 2.

**Where to start (a session of its own, test-first):** cases from this shape (a step returns with transfers in
flight; what lands after a step is in the next step's result; `settle` says `more_work` while anything is in
flight; the window never exceeds the credit's burst; a drain raises the storage event once; many small objects
go in one message and the small rejoin is no longer bounded by a call count; a node stopping mid-window loses
nothing that landed). Then the code in `DistributedStore::repair_step` and `Repair::transfer`, the message pair,
one whole-suite run, and old against new on the Pis (the small rejoin is the figure to watch).

## How to measure a stage

`tools/harness/pi-repair-run.sh` drives one node a Pi over ssh from the Mac: `smoke`, `link`, `run`. Settings:
`A` and `B` (the holding and joining Pis), `STORES` (two of `macha`, `dagda`, `dagda-old`), `SCENARIOS` (any of
`rejoin-large rejoin-small away-large away-small viewer`), `OUT`, and `NOCOLD=1`, which skips the page-cache
drop and must be set on any Pi not held whole. As run for stage 3:
`A=192.0.2.2 B=192.0.2.4 NOCOLD=1 STORES="dagda-old dagda" OUT=build/harness/results/<name> ... run`.

- **Where the binaries are** (node-b and node-d, the same paths under `/root/harness`): `build-old/` is the
  baseline's Dagda (`7d12ca8`: `harness-dagda`, `dagda/libdagda.so.0.2.0`), `build-tip/` is `d119d14`,
  `macha-build/` Macha's. Rebuild the tip on node-b (`src-tip` fetched from a bundle, `cmake --build build-tip
  --target harness-dagda`) and copy `harness-dagda` and `libdagda.so.0.2.0` to node-d. **To measure stage 4
  against stage 3, copy `build-tip` to a second old directory first** and give the script a store name for it;
  `build-old` stays the baseline.
- **The suite's build** for a gate on node-b is `/root/dagda-s3` (`repo` a clone fed by a bundle, `src` its
  checkout, `build` on the Dannan 0.8.2 prefix `/root/harness/dannan-082`); Macha's fixtures are at
  `/root/dagda-release/macha-fixtures` on node-b and node-a; node-a and node-c keep `/root/dagda-080`.
- The link varies (2.9 MB/s at the baseline, 4.8 to 6.7 since node-b moved to 5 GHz; node-d is on 2.4 GHz): a
  stage is measured as old binaries against new in one session, alternating, never against earlier numbers.
- Claim what the run uses (two cores on each Pi, an honest end time: a full run of five scenarios and three
  rounds took 55 minutes, two scenarios 20), post a note on the board, keep it short.

## The unpaced large rejoin: what the trace found (2026-11-17 21:37 to 22:24Z)

`TRACE=1` (`9d078eb`) has each harness node write repair's decisions and a quarter-second sample of its credit,
window and link estimate. One traced round of the large rejoin showed:

- **The link's estimate collapsed under the window** (found, proven, fixed). b's four concurrent pulls, sharing the
  link with a's eight pushes, each taught the estimate its own bytes over its own time: 0.69 MB/s on an 8 MB/s link.
  b's credit, earned at half that, held it idle from 8 s to 21 s with 22 objects outstanding; a pushed everything on
  its own credit, its estimate also a share (5.1 MB/s). Stage 3's transfers were serial, so a sample saw the link.
  `6231c98` the case (seen failing at its commit, node-b), `79e055d` the fix: repair teaches the estimate from what
  its window moved, at each drain and each window's worth, and its own transfers no longer sample one by one.
  Gate at `79e055d`, node-b, Dannan 0.8.2 prefix, Macha's fixtures: 1146 ran, 1142 passed, 3 skipped, **1 failed**:
  `ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit`, which touches no repair code, passed three times
  alone straight after, and is in ACTIVE.md's Open.
- Remeasured (`build/harness/results/2026-11-17-stage4-estimate/`, link 8.0 to 9.7 MB/s): the large rejoin 17.5,
  20.9, 16.7 s against stage 3's 14.6, 17.1, 14.1 (it was 16.6 to 25.5); the joiner now pulls 6 or 7 of the 26
  (stage 3's pulled 10; before the fix, 1 to 4). The small rejoin 3.9 to 4.5 s against 13.4 to 13.5; under a viewer
  14.1 to 19.3 against 23.6 to 26.4.

**Traced again at `79e055d`** (22:34Z, `build/harness/results/2026-11-17-trace2/`, rounds 1 and 2: 18.8 and 20.3 s;
round 3 lost when node-d went down as it began, before its node joined). Round 1:
- After about 10 s the rejoin runs near the link: the last 14 objects (56 MiB) land in about 8 s, about 7 MB/s.
- **The start is slow.** b's first four pulls (16 MiB, four fetches of one) start at 1.1 s and land at 7.5 s. Until
  4.1 s nothing else is on the link (a's pushes wait for a's survey to see b's lacks), so the link alone would take
  about 2 s. Not yet known where the 6 s go: the holder's server (its DATA admission for a fetch, its reads), the
  link, or b's stores (a pull lands after its write). The trace marks decisions, not those stages.
- b then learns 2.6 MB/s from those four, earns 1.3 MB/s, and pulls about one object every 2 s.

**Timed** (2026-11-18 00:00Z, node-b a and node-c b, both 5 GHz and 2.4 GHz as they are; `7a8dd96`'s hook;
`build/harness/results/2026-11-18-timed/`; 22.6, 17.3, 17.9 s). Round 1:
- b's stores are not the cause: each pull stored 23 to 67 ms after its answer.
- **The first transfers are slow on the wire or at the holder.** b's first four fetches of many (one object each,
  16 MiB) asked at 1.1 s, answered at 9.1 s, alone on the link until 5.2 s; a's first twelve pushes (48 MiB) asked
  at 5.2 s, answered at 13.3-13.5 s. Later transfers of one 4 MiB object take 0.5 to 1.2 s, the link's rate. The
  first 13 s move about 5.5 MB/s against about 8 after.
- Next: time the holder's side of b's first fetches (its DATA admission wait and its reads, just after writing all
  104 MiB; its survey's trie questions at the same moment) and the connection's first seconds, against stage 3's
  first pulls the same way.

**Next: the remaining gap, about 3 s.** The stages are now timed (`7a8dd96`: `DistributedStoreOptions::repair_timing`,
printed by the harness's `TRACE=1` as `repair-time` lines: each push and pull asked, answered, stored); the harness
needs rebuilding at the tip on both Pis of the pair first. Then trace again (`TRACE=1`, `STORES=dagda`, `SCENARIOS=rejoin-large`) and read
both nodes' samples as before. Leads, none proven: each node's first pass earns no credit (`earn_credit` with no
earlier time earns for none), and stage 3's joiner pulled at once on its initial estimate; a promises what its
window will send, so b's pulls of those wait for copies a sends at a's pace; a span's bytes are counted as a pull is
stored, after its disk write. Then a case for the mechanism, the fix, the scenarios against `build-s3`.

## Not decided

- Whether the claim walks' restorer (`Repair::Restorer`, an extent of credit a missing claim) joins the queue.
- `idle_bandwidth_fraction` 0.5 did not hold repair to half the link in the baseline (about 2.4 MB/s on the wire
  of 2.9): not looked into.
