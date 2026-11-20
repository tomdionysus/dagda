# Louhi's asks: reads, prefix listings, caught up, FetchContent

Asked by the Louhi session, 2026-11-20 (a new hosted system: its records, meaning projects, quotas, task definitions,
tasks and statuses, in one every-node head space under `louhi/`, on Dagda 0.3.2). The maintainer, the same day: the cutover no
longer holds Dagda's queue; "caught up" is a clock comparison; all three are ordinary work. Louhi's answers to the
shape are pinned below. Test first, in a release after 0.3.2.

## 1. Read one key, list by prefix (Context v2, `Heads`)

Louhi's answers:
- An absent key succeeds with an empty value (Dagda's rule: a read that finds nothing succeeds empty).
- The view is the caller's: this node's own (`local`, read-your-writes) by default, `converged` for an operator's
  describe later. The caller passes the view it read, as `Space::entries` takes one.
- Listings are pages of up to a few hundred records, with a resumable cursor across calls, every page from the one
  view the caller passes. The agent reconciles against a whole listing and must never see half of two views.

Shape:
- `Heads::get(tree, space, view, key) -> HeadGet {finished, value (empty for absent), why}`. It is
  `Tree::lookup` on the view's root: O(log n) node fetches, a node not held here fetched from a peer. It never reads
  a missing node as an absent key; that fails with `why`.
- `Heads::list(tree, space, view, prefix, cursor, budget) -> HeadChanges-like {finished, page, why}`. The page holds
  the entries whose keys start with `prefix`, after the cursor, in key order, one budget operation each. The page is
  complete (`Stop::end`) at the first key past the prefix, so the end of the prefix is the end of the listing.
- Under it, a new `Tree::entries_with_prefix(root, store, prefix, from, budget, time)`: `entries`' paging bounded
  by `tree_prefix_upper_bound(prefix)`, starting at the prefix when the cursor is empty. Subtrees outside
  `[prefix, upper)` are never fetched, as `for_each_with_prefix` already does.
- Both take the view, never a fresh one, so a listing is from one view by construction. The view is
  `SpaceView`, which the caller got from `read(space, HeadRead::local | converged, ...)`.
- Waits: `entries_waits` (local, plus network for a node not held here).

Cases first:
- An absent key is found empty.
- A key is found in the view passed and not in a later view, and the reverse.
- A missing tree node fails the read and is never read as absent.
- A prefix listing over several pages, resumed by cursor, equals one walk of the same view, even with commits made
  between pages.
- A prefix listing stops at the prefix's end and fetches no subtree outside it (counted on the node store).
- An empty prefix lists everything.

## 2. Caught up (per space; a query and a watch)

Louhi's answers: per space; both a query and a notice when it turns true. The agent asks before any pass that would
remove something, and the notice prompts a pass.

The maintainer's ruling: a node is caught up in a space when its view's clock covers the clocks its active peers advertise for
it.

What a peer advertises today: the space's committed generation, as a u64 advert. A Dannan advert's value holds at
most 1024 bytes (`dannan/membership/node_record.h:50`). A clock is 40 bytes an author, and authors rotate, so a whole
clock does not fit for long.

Proposed shape (exact, and small on the wire):
- Each node advertises, beside the generation, its space's committed head identity (generation and hash, 40
  bytes), under a new advert key per space. This is an additive wire change: an older node carries an unknown advert
  without reading it.
- `caught_up(view)` holds when, for every active peer that advertises the space:
  - this node holds that head (accepted or in history, `Replica::materialized`), and
  - the view's clock covers that head's clock.
  The clock is read from the held record, so it is exact, however many authors there are.
- An active peer whose advertised head is not held here makes the answer false. It names the peer and the head, and
  it is also a reason for the exchange to import. A peer that does not advertise the space (has not opened it) is
  named in the evidence and does not hold the answer back.
- A node alone (no active peer) is caught up.
- Evidence, not a boolean: `CaughtUp {caught_up, behind: [{peer, head}], unheard: [peer], view}`.
- Watch: `Heads::watch_caught_up(space, executor, notice)` posts when the answer turns true, re-checked on head
  moves (the existing `watch`), roster changes and advert changes. It is posted to the caller's executor, never
  inline.
- Waits: none for the query (adverts are in the roster and the check is local). If a head must be decoded for its
  clock, that is a local read: `local_waits`.

Cases first:
- A joining node with an empty view and a peer advertising a head it lacks is not caught up, and names the peer.
- After import and fold it is caught up and the notice is posted once.
- A peer's new commit makes it behind again until the commit arrives.
- A node alone is caught up.
- A peer without the space is unheard and holds nothing back.
- The notice is posted to the executor, never inline.

Open for the maintainer: the advert holds the head identity, not the clock (recommended, because of the 1024-byte bound).

## 3. FetchContent beside a fetched Dannan

Today `CMakeLists.txt:32` does `find_package(dannan 0.9.2 CONFIG REQUIRED)`. A FetchContent'd Dannan satisfies it
only when declared with `OVERRIDE_FIND_PACKAGE`.

Proposed:
- If `TARGET dannan::dannan` exists, use it and skip `find_package`, after checking that Dannan's version variable,
  when set, is at least the minimum.
- As a subproject (`NOT PROJECT_IS_TOP_LEVEL`), install and export only when `DAGDA_INSTALL` is on, default off. A
  fetched Dannan need not install an export set, and Dagda's `install(EXPORT)` names it.
- `DAGDA_BUILD_TESTING` stays off by default.

Case: `test/package/fetch`, a consumer that FetchContents Dannan and Dagda from local paths, builds, and runs the
same version check as `test/package`. It runs on a Pi with the gate.
