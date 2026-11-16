# Repair, redesigned: one queue of copies to make (2026-11-16)

## Status (2026-11-16, late evening)

| Stage | What | State |
|---|---|---|
| 0 | Measure and check | Done. Baseline measured; the three checks answered |
| 1 | The survey keeps who holds what | Done, gated (`502cf57`) |
| 2 | One queue, fewest copies first | Done, gated (`2a8d24b`); not yet benchmarked |
| 3 | Each copy once (the offer) | Designed, not built. **Next** |
| 4 | A window of transfers; the call count gone | Not started; its shape goes to the Macha session first |

Last full gate: `2a8d24b`, node-c, 1125 ran, 1122 passed, 3 skipped, 0 failed, 0 warnings. Everything is on
`feature/macha-store-gaps`, local, nothing pushed.

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
- On the measured double transfer: "seems a hell of a waste?" It is what stage 3 removes.

## What was wrong (as the code stood at `7d12ca8`)

In `DistributedStore::repair_step` and `Repair`, moved from Macha and the same on its live nodes (0.94.2).

1. **The pull starved, two ways.** The step ran the push, then the pull on what was left. The push could spend
   the 16-call count, or run out of byte credit, after which the pull was skipped for the step. Both proven by
   cases. *Fixed by stage 2.*
2. **Urgency was ignored.** Both walks went in id order. *Fixed by stage 2.*
3. **Both ends do the same work.** A holder pushes an object to an owner that lacks it while that owner pulls
   it. Measured: the normal case (below). *Stage 3.*
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
  `claim_walks`, `transfer`, `settle`) and its Status reads the gates. Stages 1 to 3 leave those alone. Stage 4's
  window outlives a step and changes that shape: agreed with the Macha session before it is built. Its pass
  should hand `AvailabilityService::peer_holds()` to `RepairLists::holds`, as `Repair::lists()` does; without it
  a pull's copies are unknown and it sorts first.
- **The cases ported from Macha**: none changed or removed so far. The Macha session is told of each that does.
- **Macha's live nodes** keep today's repair until the cutover; Macha's session has recorded it as a known live
  defect. The rule "a Macha defect is fixed in Macha first" is set aside for this by the maintainer's word.
- **The wire** gains the offer at stage 3 and a multi-object message at stage 4. No backward compatibility is
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

## Stage 3, designed, not built: the offer

- *The message.* A new pair, `offer_objects` and its reply, not `have_objects`: the cases that hold repair to
  "no probe" count `have_objects` and its kin. The sender names the ids it is about to send a peer; the reply
  says of each: wanted, held, being fetched, or no room.
- *The sender* (in `push_some`, `distributed_store.cpp`): after it knows which owners lack an object and before
  it reads the bytes, one offer a peer a batch. Held: the peer is a keeper, remembered as a pushed copy is.
  Being fetched: nothing sent, tried again on the retry's backoff. No room: passed. Only what is wanted is read
  and sent. The offer's own bytes come out of the credit.
- *The receiver* (`storage_server.cpp` and the store): it answers from its index and from two small tables: what
  its own repair is fetching now, and what it has said it wants from a peer. An id it wants from a peer is not
  pulled by its own repair meanwhile (met again on the pull's retry backoff). An entry goes when the copy
  arrives, when the offering peer is no longer active, or after the transfer deadline the store already has
  (`object_transfer_call`): no new number.
- *The cases, from the design:* a push to a peer that already holds the object moves no object bytes; a peer
  fetching an object declines its offer and is sent nothing; a peer that wanted an offered object does not fetch
  it too, and fetches it after all if the copy never comes; an offer is not counted as a probe; the bytes on the
  wire for a rejoining node are the objects once.
- *To check when building:* the node's list of registered message types has a case (`node_test.cpp`); the new
  type joins `StorageServer::served()` and `message::register_types`.

## How to measure a stage

`build/pi-repair-run.sh` (on the Mac, not in the repository) drives one node a Pi over ssh: `smoke`, `link`,
`run`. `B=<address>` chooses the joining Pi; `NOCOLD=1` skips the page-cache drop and must be set on any Pi not
held whole. The binaries are built on node-b under `/root/harness` (`macha-build/harness-macha`,
`build-tip/harness-dagda`) and copied with their libraries to the other Pi at the same paths.

- The baseline's binaries (`7d12ca8`) are on node-b, node-d and node-c now. **Before rebuilding `build-tip` on
  node-b, keep a copy of `harness-dagda` and `libdagda.so.0.2.0` as the old side**, and teach the script a
  second Dagda store name for it.
- The link changed after the baseline: node-b and node-a are on 5 GHz, node-c and node-d on 2.4 GHz. Tonight's
  times compare with nothing later. A stage is measured as old binaries against new in one session, alternating.
- Claim what the run uses (a core or two on each Pi, an honest end time), post a note on the board, keep it
  short: the traffic slows every session's ssh and rsync.

## Not decided

- Whether the claim walks' restorer (`Repair::Restorer`, an extent of credit a missing claim) joins the queue.
- `idle_bandwidth_fraction` 0.5 did not hold repair to half the link in the baseline (about 2.4 MB/s on the wire
  of 2.9): not looked into.
