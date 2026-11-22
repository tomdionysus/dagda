# Changelog

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
