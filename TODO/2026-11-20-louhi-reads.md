# Louhi's asks: reads, prefix listings, caught up, FetchContent

Asked by the Louhi session, 2026-11-20 (a new hosted system: its records, meaning projects, quotas, task definitions,
tasks and statuses, in one every-node head space under `louhi/`, on Dagda 0.3.2). Tom, the same day: the cutover no
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

Tom's ruling: a node is caught up in a space when its view's clock covers the clocks its active peers advertise for
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

Open for Tom: the advert holds the head identity, not the clock (recommended, because of the 1024-byte bound). Tom,
2026-11-20: "Are other projects waiting for this also? What works for everyone?", so every hosted system and Dannan
were asked.

### What the other systems need (asked 2026-11-20)

**Lugh** (from its P8 plan; P8 waits on Tom): yes.
- Spaces: lugh/scope (every node), lugh/secrets (Lugh nodes only) and the sign-in back-off space. Not node_local
  spaces, which cannot be behind.
- What a lagging view gets wrong:
  - genesis run on an empty scope;
  - a uidNumber or gidNumber issued twice, or a duplicate name;
  - a revoked or disabled session accepted on a node that can reach its peers and is merely behind.
- Shape: per space, a query and a notice. The notice sets /ready once scope and secrets are caught up. The query
  goes before allocating writes and before genesis. Lugh combines spaces itself.
- Not caught up: /ready false; sign-in, token and bind answer 503; allocating writes and genesis are refused. Reads
  of what the caller holds go ahead.
- It must handle:
  - Peers that do not open the space neither count nor hold back: compare only with peers advertising a head for it.
  - **"No peers to compare" kept distinct from caught up**, at least as a count of peers compared. Otherwise a lone
    node with stale data reports caught up.
  - Recomputed on every active-set change, so a peer that leaves never blocks the answer for ever.
  - An advertised head older than the peer's latest is fine, since covering it is enough.
  - Three or four adverts per node.
- The head-identity advert serves Lugh, given the "no peers" distinction.

Consequence for the shape: the answer becomes three-valued, `caught_up`, `behind` (naming peers and heads) and
`alone` (no peer advertises the space). It carries the count of peers compared, and `alone` is never folded into
`caught_up`.

**Dannan** (2026-11-20): **pack the heads into one advert.**
- `max_adverts = 64` per node is one budget for every hosted system and Dannan's own users: channel interests,
  agreement's `agree.slots/<space>`, Lir's shared-subscription groups. The 65th is refused, not coarsened.
- So: one key, `dagda/heads`, 40 bytes a space (about 25 spaces), paged into `dagda/heads/1` and on past that.
- Cost: any advert change makes the whole node record newer, and it travels whole in gossip. So the change is
  rate-limited to the advert interval, and the push form (advertise with a deadline) is never used per commit.

Consequence for the shape:
- One packed advert of (space id prefix, generation, hash) entries, written at most once per advert interval.
- "Peers that do not open the space" means peers whose packed advert has no entry for it.
- A head older than the peer's latest, because of the rate limit, is fine: covering it is enough (Lugh's point).

Awaited: Lir, Kvasir, Mimir.

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
