# Changelog

## 0.4.0: on Dannan 0.11.1; repair holds no memory once its window drains

A minor on Dannan 0.11.1 (from 0.9.2), carrying 0.3.3's fix. The contract is unchanged. The SONAME
is the whole version, so a host rebuilds against `libdagda0.4.0`, and it needs `libdannan0.11`
(0.11.1 or later).

Fixed: after 0.3.3, a node's resident memory still grew with what repair moved (a live node of 8 GB
reached about 1.5 GB), held by glibc in its per-thread arenas rather than in use. The case
`StoreRepair.RepairHoldsNoMemoryOnceItsWindowDrains` measured 400 to 600 MiB held with 2 MiB in use;
it now holds within four extents of where it began, round after round. What changed:

- An extent is served and stored in one buffer: read and opened in the reply, sealed in the request
  or the reply that carried it, with Dannan's in-place AES-GCM. A serve or a store held three
  buffers of the extent before.
- Every object path takes its buffers from the node's transfer-buffer pool (Dannan's
  `Context::buffers()`) and gives them back, so blocks of an extent's size are reused rather than
  freed to glibc, whose sliding mmap threshold kept them in per-thread arenas. Dannan 0.11.0 and
  0.11.1 do the same on their side: a message reassembled at exactly its size, its frames pooled,
  and every payload it drops given back.
- Repair's window stays under the pool's bound less four extents, so nothing it moves is freed for
  the bound.

Also from the 0.4.0 line: sanitizer builds take Dannan's one switch, `DANNAN_SANITIZE`, for Dagda's
targets and the Dannan they link; each subsystem has a fuzz target on Dannan's harness; a wrap advert
whose certificate key does not decode is refused, not thrown.

What a host does on adopting 0.4.0:

- Build against Dannan 0.11.1 or later (`find_package(dannan 0.11.1 CONFIG)`, `libdannan-dev
  (>= 0.11.1)`), and rebuild against `libdagda0.4.0`.
- Set no allocator variable for Dagda (no `MALLOC_MMAP_THRESHOLD_`, no `MALLOC_ARENA_MAX`): none is
  needed. A setting kept for another reason may stay.
- Size Dannan's `NodeConfig::transfer_buffers_bound` (256 MiB by default) to the memory the node
  can keep for transfers. Repair's window follows it: at most the bound less four extents, and at
  most `RepairOptions::max_window_bytes`.

## 0.3.3: repair's memory is bounded whatever its link estimate says

A patch on 0.3.2, on Dannan 0.9.2, for a node that was killed for memory during repair (a
live node of 8 GB, 2026-11-21). The contract is unchanged. The SONAME is the whole version,
as for every release, so a host rebuilds against `libdagda0.3.3`.

Fixed:

- Repair's estimate of the link could jump a thousandfold. A sample was taken at each window's
  worth landed and reset the span it measured, so a transfer still in flight landing just after
  was taught as an extent over microseconds: on a 10 MiB/s link the estimate reached 20 GiB/s.
  A sample now spans from the window leaving empty, never cut short. However a sample is taken
  (writes and reads teach the estimate too), it raises the estimate at most fourfold at once and
  never past 10 Gbit/s.
- Repair's burst, five seconds of that estimate, bounded what one step reads, so the bad estimate
  let a step read about 100 GiB. `RepairOptions::max_window_bytes`, 256 MiB by default, now bounds
  the burst whatever the estimate.
- A push step read every object it planned before sending any. It now sends each object as it is
  read, so a step holds one object's read bytes at a time beside what Dannan's outbound queue holds
  (at most 128 MiB a peer connection).

What a host does on adopting 0.3.3:

- Set `RepairOptions::max_window_bytes` from its memory budget. It bounds the bytes repair has in
  flight; resident, that is the window in the outbound queue plus about one extent being read.
- Remove any repair bandwidth cap (`max_bandwidth`) set only to stop this. Keep one set for its own
  reason.
