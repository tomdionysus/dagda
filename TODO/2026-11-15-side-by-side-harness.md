# The side-by-side harness: Macha's store and Dagda on the same workloads (2026-11-15)

The maintainer's ruling (2026-11-15): Dagda builds Macha's store from its read-only extract of f0597a4 (`build/macha-f0597a4/`),
copied to a Pi and built there, and drives both stores with one workload driver of Dagda's; `../macha` is never touched,
and the Macha session reviews the driver before it runs. It is the second proof of the bar ("Dagda MUST be capable of
handling DHT storage AT LEAST AS WELL as Macha"), after the call-by-call map.

## What builds

- **Macha's side.** The extract's `macha_core` shared library (CMakeLists.txt:374), which holds the store with
  metadata, auth, http and catalogue sources compiled in but links no FUSE, libtorrent, FFmpeg or yaml-cpp (:393-394).
  Configured out of tree with `-DMACHA_BUILD_SERVER=OFF -DMACHA_BUILD_TESTS=OFF -DMACHA_ENABLE_BOOKS=OFF
  -DMACHA_ENABLE_FUSE=OFF -DMACHA_WARNINGS_AS_ERRORS=OFF`, `--target macha_core`. It wants Dannan 0.5.x, at least
  0.5.3 (:160): built from Dannan's tag into its own private prefix (`/root/macha-harness/dannan-0.5`), beside the
  0.8.0 prefix Dagda's gate uses; nothing system-wide. node-c already has what the core needs (OpenSSL, libcurl, zlib,
  Boost 1.83, CMake 3.31); no package is installed. Which 0.5 patch: the one Macha 0.94.2 ships on (asked of the Macha
  session); 0.5.6 if any.
- **Dagda's side.** `feature/macha-store-gaps` on Dannan 0.8.0, Release, as gated.
- **The driver.** Two small executables from one source tree under `tools/harness/` here: `harness-macha`, linking
  `macha_core`, and `harness-dagda`, linking `dagda`. Each runs a named workload against its store and writes the same
  JSON lines. The workload definitions (sizes, counts, concurrency, phases) are one header both include, so a workload
  is the same bytes in the same order on both. The Macha half copies what it needs from the extract's
  `tests/test_support.hpp` (`config_for`, `BareNode`, `distributed_store_for`) and `tests/fake_cluster_node.hpp`,
  never including them (they drag in catalogue, fuse, service and the test framework).

## How a run goes

- One Pi, one disk, one store at a time: Macha's, then Dagda's, then Macha's again (the second Macha run says how
  much a run varies). Cold (page cache dropped, `echo 3 > /proc/sys/vm/drop_caches`) and warm where Macha asks.
- Nodes in one process, each on its own loopback port and state directory, one cluster key, bootstrap at node 0:
  Macha's `BareNode`s over Dannan 0.5, Dagda's `Node`s over Dannan 0.8, configured alike (replicas 2, write_copies 1,
  extent 4 MiB, packing 1 MiB / 64 MiB packs, Macha's defaults for everything else; Dagda's facade values as the port
  sets them).
- Two Pis only where a link matters (workload 3's remote reads and 7's repair): shaped with `tc netem` on the two Pis'
  interface (60 ms RTT, about 10 MB/s, live-b to live-a). That changes the shared machines' network for everyone:
  a note on the claim board first, both Pis claimed whole, and the shaping removed in the same script's trap.
- Figures for each workload: throughput; latency p50, p99 and max; CPU seconds per GB (`getrusage` on the process);
  RSS and its high-water mark (`/proc/self/status` VmRSS and VmHWM); fsync count (the stores' own counters, and
  `strace -c -f -e fsync,fdatasync,syncfs` on one rerun only, since tracing slows it); bytes written to the disk
  (`/proc/self/io` write_bytes, and the device's `/sys/block/<dev>/stat` sectors written).

## The workloads (the Macha session's list, 2026-11-15)

1. Publication: 4 MiB extents as deferred writes with one barrier a file; one 512 MiB file; then 10 concurrent
   publications, each barriered over its own tickets.
2. Small files: 200 x 64 KiB, each made durable, then all deleted: packing, touches, removes, compaction (the pattern
   live-b crashed under).
3. Reads: one reader of 512 MiB and four of 128 MiB, local; then remote over the shaped link, readers sharing fetches;
   read-ahead as FUSE does it (128 KiB requests, two extents ahead).
4. A viewer under load: foreground read latency (p50, p99, max) while a loader saturates writes, and control-call
   latency under the same writer. The viewer must not wait.
5. CONTROL graphs: metadata history entries and commits, and a catalogue manifest of 64 shards, put, fetched to local,
   offered and claimed; the claims barrier at commit.
6. Deletes and GC: a tree deleted, the collector under the garbage grace on a stepped clock, sightings, the CONTROL
   collection; nothing referenced removed (checked, not assumed).
7. Repair: one node away, then back; the repair rate in MB/s at idle_bandwidth_fraction 0.5 (Macha measured about 3.6
   to 6 MB/s on the live pair); repair while a viewer reads (paced, never stopped). **Pulled first (the maintainer,
   2026-11-16):** it is the benchmark of the repair redesign, with a node rejoining empty, small values, bytes sent
   against bytes needed and round trips added (`2026-11-16-repair-redesign.md`, stage 0).
8. Recovery: the time to ready with a large store (pack replay, ledger seeding, presence warm-up), clean and after a
   `kill -9`.
9. Disk pressure: a slow device (`dm-delay` over a loop file, so no real disk is touched) and the pressure onset and
   release, with a foreground reader present.
10. Cache: the persistent block cache's hits, misses and evictions under a playback-like scan larger than the cache.

Where Macha's `build/baselines/baseline.py` already has a phase, its sizes are taken, so the numbers compare with
Macha's results-0.94.2.

## Settled with the Macha session (2026-11-15, checked in Macha's code at f0597a4)

- **Dannan 0.5.3 exactly** for Macha's side: f0597a4's `find_package(dannan 0.5.3 CONFIG REQUIRED)`, and the live
  nodes run 0.94.2 on `libdannan.so.0.5.3`.
- **A metadata commit** (workload 5): N new tree nodes, a few KiB to tens of KiB each, N from a file created in a deep
  path up to a 200-file directory, each put as `ControlNamespaceNodeStore::put` does (namespace_control_store.cpp:15-36:
  `has(id)`, else a local control put, no peer asked), then the publication's claims barrier over them
  (node_services.cpp:346-554: DATA claims on their holders, CONTROL claims here and the peers' share). Reads as `::get`
  does (:38-44): local control get, else `ensure_control_local`. Dagda's side: `Context::Control::put` and `get` /
  `ensure_local`, then `Claims::retain` for DATA and CONTROL.
- **A catalogue install** (workload 5): changed shards and the manifest put with `local_.control().put`
  (catalogue.cpp:1858-1866); convergence makes them local on each node with `ensure_control_local` (:1126-1141) and
  offers them with `store_.offer_control(objects)` (:1146), whose figures (objects_sent, bytes_sent, held_by) are
  compared. A manifest of 64 shards, with 1, 8 and all 64 changed. Dagda's side: `Context::Control::put`,
  `ensure_local` and `offer`.
- **The fixtures copied come from the extract at f0597a4** (`BareNode`, `distributed_store_for`, `config_for`): on
  Macha's port branch `BareNode` builds a Dagda node, so that copy would be the wrong one.
- **The transport differs.** Macha's side runs Dannan 0.5.3 and Dagda's 0.8.0. Immaterial for one node's workloads;
  for 3 and 7 the transport differs as well as the store, so each figure carries its Dannan version, and a 3 or 7
  difference is not put down to the store alone.
- **Same compiler and flags on both:** RelWithDebInfo, -O2, as Macha deploys.
- **Cold runs** do `sync; echo 3 > /proc/sys/vm/drop_caches` first and say so.
- **Three runs a workload**, the median and spread reported: enough for a spread, not endless (the maintainer's rule on test
  volume).

## What is still to settle before anything runs

- 9's `dm-delay` needs device-mapper on the Pi kernel and root: checked, not installed.
- The order: 1, 2, 4 and 8 on one Pi first (they need no link), then 3 and 7 on two, then 5, 6, 9, 10.

## Results

Kept under `build/harness/results/<date>/`, one JSON line a figure, and summarised here with both stores' numbers side
by side, each difference a finding: Dagda at least as good, or a gap fixed test-first before the port.

### Phase 1 (2026-11-15, node-b)

Machine: a Raspberry Pi 5, 16 GB, NVMe, the whole machine claimed; `vcgencmd get_throttled` 0x50000 before and after
(under-voltage and throttling happened since boot, none during the runs). Builds: RelWithDebInfo -O2, GCC.
"macha f0597a4 on dannan 0.5.3" against "dagda 0.2.0 on dannan 0.8.0" (Dagda `fa65232`). Three runs each, alternating
Macha then Dagda, cold. Medians, with the range in brackets; `build/harness/results/2026-11-15/phase1.jsonl`.

| Workload | Figure | Macha | Dagda |
|---|---|---|---|
| publication-one (512 MiB) | MB/s | 114.2 [113.1-114.4] | 115.3 [114.3-115.8] |
| | CPU s/GB | 12.76 | 11.99 |
| | write amplification | 1.785 | 1.589 |
| | RSS high-water | 191 MB | 174 MB |
| | barrier | 511 ms | 503 ms |
| publication-ten (10 x 512 MiB) | MB/s | 222.6 [123.4-224.0] | 171.5 [120.1-206.2] |
| | CPU s/GB | 9.25 | 9.11 |
| | write amplification | 1.238 | 1.126 |
| | barrier p50 / p99 | 480 / 533 ms | 476 / 593 ms |
| small-files (200 x 64 KiB, 8 publishers) | seconds | 12.78 | 12.79 |
| | CPU s/GB | 53.8 [53.3-54.3] | 89.4 [88.9-90.1] |
| | file p50 / p99 | 502 / 514 ms | 502 / 504 ms |
| | GC removal and compaction | 215 ms | 226 ms |
| viewer-under-load (30 s) | viewer alone p50 / p99 | 10.5 / 13.7 ms | 9.9 / 13.1 ms |
| | viewer loaded p50 / p99 / max | 9.7 / 19.9 / 24.5 ms | 8.7 / 18.3 / 24.5 ms |
| | loader MB/s | 36.9 | 40.3 |

Findings:
1. **publication-ten: not yet a finding, the method.** Both stores slowed run on run (Macha 24.1, 24.0, 43.5 s; Dagda
   26.0, 31.3, 44.7 s): the NVMe takes 6 GB a run, and Dagda always ran second in a round, on a disk each time fuller and
   later. To rerun with the order alternating (Dagda first in alternate rounds), `fstrim` and a rest between runs, before
   any difference is put down to the store.
2. **small-files: Dagda spends more CPU.** 1.17 s against 0.70 s for the 200 files on two nodes, the same in every round
   (about 2.3 ms more a file), with the same wall time (both bound by the 500 ms window) and the same bytes written. A gap:
   profile both (perf on the Pi) and find where Dagda's per-write cost goes (candidates from the map: a ticket and its
   evidence a write, the scope check on every local copy, the Context's copies), then fix it test-first.
3. Everything else is Dagda as good or better: one publication's throughput, CPU and write amplification; the viewer's
   latency alone and under load, with more loader throughput beside it.

**Superseded the same day: phase 1's Dagda side was not configured as Macha's.** Profiling the small-files CPU found
two harness faults, both in how the driver built Dagda's nodes from the test fixture, not in the store:
- **Test cadences.** The fixture runs Dannan's heartbeat at 100 ms, telemetry and status at 200 ms and the storage
  server's refresh at 500 ms, so a case converges quickly. Macha's side ran a 1 s heartbeat (its storage server
  refreshing at the same) and 10 s telemetry. At idle for 20 s that cost Dagda 0.57 s of CPU against Macha's 0.075 s
  (`idle`, a workload added for it). The fixture now takes the four cadences as options (its defaults unchanged), and
  the driver sets Macha's: 1 s heartbeat, 10 s telemetry and status, 1 s refresh (`33d9489`). Idle is then 0.077 s
  against 0.069 s.
- **Packing.** Given a pack target, the fixture packed everything up to it, so Dagda's side packed its 4 MiB extents
  into 64 MiB packs while Macha's packed only below 1 MiB. That serialised every extent append under the pack lock
  while it held one of the two loader leases (76% of the publishers' off-CPU time was waiting for those leases). It
  also made Dagda's write amplification look lower. The fixture now takes a threshold of its own, and the driver sets
  1 MiB (`3250405`).

### Phase 1 rerun (2026-11-15, node-b, matched configuration)

The same machine and builds as phase 1, plus three method changes: the order alternates (Macha first in odd rounds,
Dagda first in even), each run follows `fstrim /` and a 30 s rest from a cold page cache, and `vcgencmd
get_throttled` is read before and after every run (no run had bits 0-3 set; 0x50000 throughout). Publications now also
report `settled`: the time, CPU and bytes until both nodes hold every extent, so replication the barrier does not wait
for is counted and the stores compare at equal work (`3e0d3b7`). Medians [range];
`build/harness/results/2026-11-15/rerun2-matched.jsonl` and `rerun3-settled.jsonl` (publications from rerun 3).

| Workload | Figure | Macha | Dagda |
|---|---|---|---|
| idle (20 s) | CPU | 0.069 s | 0.077 s |
| publication-one | MB/s to the barrier | 115.0 [110.1-115.9] | 95.1 [94.7-101.1] |
| | write amplification at the barrier | 1.777 | 1.988 |
| | seconds until both nodes hold it | 5.93 | 5.77 |
| | CPU s/GB until both hold it | 14.39 | 16.26 |
| | write amplification until both hold it | 2.004 | 2.004 |
| | extent write p50 | 27.7 ms | 33.4 ms |
| publication-ten | MB/s to the barrier | 222.0 [184.1-223.7] | 219.2 [147.0-221.4] |
| | seconds until both nodes hold it | 62.0 | 61.2 |
| | CPU s/GB until both hold it | 15.71 | 16.36 |
| | write amplification, barrier / settled | 1.234 / 2.004 | 1.238 / 2.004 |
| small-files | CPU | 0.704 s | 0.786 s |
| | file p50 / p99, GC and compaction | 502 / 503 ms, 215 ms | 502 / 504 ms, 221 ms |
| viewer-under-load | viewer alone p50 / p99 | 10.7 / 14.3 ms | 9.1 / 12.7 ms |
| | viewer loaded p50 / p99 / max | 9.2 / 17.0 / 19.7 ms | 7.7 / 14.8 / 18.0 ms |
| | loader MB/s | 42.5 | 41.4 |

Findings:
1. **Time to two copies: Dagda as good** (5.77 against 5.93 s, 61.2 against 62.0 s, the same bytes). Its lower
   throughput to the barrier on one publication is when the second copy is made, not more work: Dagda's prompt
   replication makes it during the publication (write amplification 1.99 at the barrier), Macha's mostly after (1.78).
   For Macha to rule on: whether a loader's barrier should come first and replication after, as Macha's does.
2. **CPU: Dagda spends more, still a gap.** +13% to settle one publication, +4% for ten, +12% on small files. Part
   was thought to be Dannan's placement, which copied every roster record with its adverts about five times a call
   (376 perf samples against Macha's 139 on small files). Dannan removed those copies on `feature/placement-copies`
   (6145bcc, for 0.8.2), reviewed by Dagda. But the before-and-after shows no measurable CPU change: small-files 0.772 s
   [0.706-0.787] before, 0.797 s [0.785-0.803] after, copy samples 234 to 188
   (`placement-before-after.jsonl`). What copies remain are per message in Dannan's net layer (told to Dannan, not
   asked for). The gap is mostly Dagda's own: next, profile publication-one, which shows it largest (+13%).
3. **The viewer: Dagda better** alone and under load, at a 3% lower loader rate.
4. The publication-ten variance seen in phase 1 was the method: with fstrim and a rest both stores are steady,
   bar one slow run each (Macha 184, Dagda 147 MB/s), different rounds and orders.

### The publication CPU gap: found and fixed (2026-11-15)

Profiled with perf confined to the measured span (`HARNESS_PERF_CTL`, `f595941`), publication-one showed Dagda at
105,335 task-clock samples against Macha's 99,614. 4,835 of them were in `run_write`'s `message::encode_object`:
every write encoded its payload for a peer call, even when only this node's copy was launched (write_copies 1). The
encode also grew its buffer once the principal was appended after a 4 MiB payload, so each extent was copied twice and
4 MiB of fresh pages zeroed (capacity 8,388,694 for 4,194,373 bytes). Fixed test-first: `StoreMessages.
APutOrAClaimCarriesItsPrincipalAfterMachasFields` asserts one allocation of the exact size (failed at `2a734c6`,
passes at `07ad6d2`; the wire bytes unchanged, by the golden hex beside it). The payload is now encoded at the first
peer call only. Gate on node-a at `07ad6d2`, Dannan 0.8.0: 1106 ran, 1103 passed, 3 skipped, 0 failed, 0 warnings.

After the fix (`rerun4-after-encode.jsonl`, the same protocol; Macha then Dagda, medians):

| Workload | Figure | Macha | Dagda |
|---|---|---|---|
| publication-one | MB/s to the barrier | 114.7 | 117.8 |
| | CPU s/GB to the barrier / settled | 12.59 / 14.49 | 12.06 / 14.11 |
| | seconds until both nodes hold it | 5.88 | 5.76 |
| | replicated at the barrier | 0.766 | 0.766 |
| publication-ten | CPU s/GB to the barrier / settled | 9.98 / 16.14 | 8.64 / 15.56 |
| | MB/s to the barrier, median [range] | 179.2 [142.1-224.3] | 220.5 [123.7-229.3] |
| | seconds until settled, median [range] | 69.6 [65.8-81.9] | 73.4 [69.4-82.0] |
| small-files | CPU | 0.701 s | 0.768 s |

- One publication: Dagda now as good or better on every figure. The earlier "Dagda replicates during the
  publication" (finding 1 above) was the encode slowing Dagda's writer, not a faster replicator: replication at the
  barrier is now equal, so Macha's question 2 needs no change.
- Ten publications: Dagda spends less CPU. Throughput and time to settle varied widely on both sides in this round
  (a slow run each), so no difference is claimed there.
- **Small files: still a gap**, 9.4% (about 0.33 ms a file), now the only one in phase 1. Next: the same in-span
  profile on small-files.

### Small files: what the in-span profile shows (2026-11-15)

perf confined to the span: Dagda 8,508 task-clock samples against Macha's 8,100. No single hotspot.
- libc time: about 1,670 against 1,320 (allocation, copies, locking).
- NodeRecord copies and destructions, attributed to callers: 528 samples against 218. They are spread over:
  - the write's two placements: replication_set over roster(), and ranked through Nodes::placement;
  - Dannan's net layer: Session::deliver, a reply handler capturing the record, CallOutcome and RequestScheduler::Job
    each carrying one;
  - prompt replication and the barrier worker.
- futex wake-ups: a write crosses more threads than Macha's.

Tried, each gated, neither a measurable gain on small files:
- **One roster reading per write** (`5502327`): run_write takes its set and its order from one Nodes::snapshot(),
  narrowed in place. Kept for consistency (a write's set and order can no longer come from two rosters). Small-files
  CPU 0.768 s before, 0.768 s after; copy samples 528 before, 549 after (`rerun5-snapshot.jsonl`).
- **Dannan 0.8.2** (placement ranks pointers, copies only the answer): Dagda 0.775 s against Macha's 0.695 s
  (`rerun6-dannan082.jsonl`).

Where it stands: about 11% (0.4 ms a file), wall time and bytes equal. The rest is a record carrying its whole
advert map through every message and placement (Dannan's 0.9.0 list: net-layer copies by value) and Dagda's thread
hand-offs per write. Neither has one fix; the next step is the maintainer's call (below).

An experiment on the advert aggregate (D11, slice 2) before proposing it: each roster record carries 10 adverts,
765 bytes (`dagda.wrap`, the five cache figures, backends online, capacity, used, extents; printed by the harness at
start). A build advertising only 4 (wrap, capacity, used, extents), on the Pi's scratch copy, never committed: small
files 0.741 s [0.741-0.744] against 0.767 s [0.700-0.790] with 10, three alternating rounds each
(`experiment-advert-count.jsonl`). About 3%. Folding Dagda's figures into one advert would take that much, and leave
about 6% against Macha's 0.70 s. It also changes the advert names Macha's port reads for its cache figures, so it is
Macha's to agree as well as the maintainer's to order.

### Small files: the figures advert and the synchronous wait, measured (2026-11-15 night, node-b)

Built and gated before measuring: the figures advert (`a103c86`, 4 adverts, about 659 bytes a record); the
synchronous waits on the caller's thread with no timer, worker job or post (`616df37`); one ranking a write when
every node is live (`3df3b8d`). Three stores, three rounds, the order rotating each round (Macha, the advert, the
tip; then advert, tip, Macha; then tip, Macha, advert), fstrim, a 30 s rest and a cold cache before each run,
get_throttled 0x50000 throughout (nothing during the runs). Both Dagda builds on Dannan 0.8.2, RelWithDebInfo -O2.
`advert-and-wait.jsonl` (`build/pi-advert.sh`). Medians [range]:

| Figure | Macha | Dagda `e9e93f0` (advert) | Dagda `461bed3` (and the wait) |
|---|---|---|---|
| CPU | 0.698 s [0.647-0.712] | 0.745 s [0.740-0.753] | 0.738 s [0.737-0.741] |
| file p50 / p99 | 501.7 / 503.2 ms | 501.9 / 504.0 ms | 501.7 / 503.5 ms |
| publish / GC and compaction | 12.55 s / 215 ms | 12.55 s / 222 ms | 12.55 s / 221 ms |
| disk written, RSS high-water | 30.76 MB, 25.2 MB | 30.76 MB, 24.6 MB | 30.76 MB, 24.7 MB |

- The advert: 0.775 s (rerun 6, the same Pi and Dannan) to 0.745 s, about 4% (the experiment said 3%).
- The wait and the one ranking: a further 1% at most (0.745 to 0.738; the ranges barely part). The hand-offs were not
  where the CPU goes; their gain is latency under a busy executor, which the case `ASynchronousWaitNeedsNoExecutor
  Thread` pins (15 s before, at once after).
- **Where it stands: about 6% over Macha's CPU** (0.04 s for 200 files, 0.2 ms a file), was 11%. Wall time, latency,
  bytes and memory equal or better. Not profiled since: the earlier in-span profile put the rest in record copies
  through Dannan's net layer (its 0.9.0 list) and libc, so the next measure is on 0.9.0 (ACTIVE.md, Next 3), with a
  fresh in-span profile beside it.

### Phase 2: reads, local (2026-11-16, node-b)

The workload as the Macha session reviewed it (`ce37fe5`): files published on node 0 and held by both, the page cache
dropped, then read on node 0 as Macha's FUSE reader reads (one synchronous loader-class read an extent as the reader
enters it, the extent before dropped first, no read-ahead, 1 MiB requests): 512 MiB by one reader, then 4 x 128 MiB
by four at once. **These compare the two stores under one harness, not with baseline.py's numbers**: baseline.py reads
library files from a random aligned offset with a warm page cache; here fresh files from offset 0, cold (the Macha
session's caveat). Three rounds alternating, the same method as above; both harnesses built from `ce37fe5` (Dagda's
store code is `6adc5ed`'s). `reads-local.jsonl` (`build/pi-reads.sh`). Medians [range]:

| Figure | Macha | Dagda |
|---|---|---|
| one reader: MB/s | 230.2 [227.6-233.5] | 240.8 [240.7-241.3] |
| one reader: CPU s/GB | 2.34 | 2.16 |
| one reader: first byte, request p50 / p99 / max | 18.0 ms, 0.20 / 18.1 / 22.0 ms | 17.4 ms, 0.18 / 17.4 / 17.7 ms |
| four readers: MB/s | 387.0 [380.1-388.3] | 363.0 [343.1-376.5] |
| four readers: CPU s/GB | 3.51 | 3.26 |
| four readers: first byte (max), request p99 / max | 63.6 ms, 62.9 / 84.8 ms | 63.2 ms, 63.2 / 81.7 ms |
| RSS high-water | 238 MB | 209 MB |

Findings:
1. One reader: Dagda better on every figure.
2. ~~Four readers: Dagda about 6% slower~~ **Superseded the same night: no steady gap, a bimodal Dagda.** An
   off-CPU profile of the read span on each (sched_switch, dwarf stacks, reader threads only; `offcpu-reads-*.txt`)
   found no wait of Dagda's own: the readers are off the CPU in the file read (4.98 s Macha, 4.99 s Dagda) and in
   the DATA arbiter's acquire (Macha's design, kept; 255 and 251 switches), nothing else above 4%. A second set of
   three rounds (`reads-local-2.jsonl`), named for Dagda's 10% spread against Macha's 2%: four readers Macha 379.3
   [226.2-385.7], Dagda 400.1 [344.1-407.2]. Over six rounds each, Macha 379-388 MB/s bar one run at 226; Dagda in two
   clusters, about 344 (twice) or 363-407. One reader stayed Dagda's, 240.8 against 230 MB/s, in every run. Not
   chased further: what puts a Dagda run in the lower cluster is unexplained, recorded here and in ACTIVE.md's Open.

**The kernel.** Every figure above was taken on node-b's 4K-page kernel (6.18.50+rpt-rpi-v8, up since
2026-11-14): node-b was already the 4K "TSan Pi" when the other build Pis moved from 16K to 4K pages at 2026-11-16
00:18Z, so these figures are unaffected by that change and comparable with later ones on node-b.

### Phase 2: recovery (workload 8), as agreed with the Macha session (2026-11-16)

- A fill run: node 0 and its peer, 8 GiB (512 MiB files and about 20,000 small files of a few KiB to 64 KiB, so the
  object count is tested as well as bytes), then a clean stop, or a `kill -9` by the script. A 1 GiB fill beside it if
  time allows, to tell bytes from objects. The state directories survive the run.
- A reopen run: **node 0 alone** (local recovery must not depend on a peer; a run with the peer up is labelled apart and
  not averaged in), the page cache dropped first, timed from construction to:
  a. data storage open (Macha: the data_storage plane, after StoragePool's constructor and its pack replay);
  b. **local state ready** (Macha: RecoveryProgress complete, as /health and the peers see it): the headline;
  c. the first foreground read of a held extent that completes (a random one of the fill's), read after b;
  d. every local store's scan complete (background after b on Macha; whichever comes first is recorded).
  Each against the same point on Dagda's side; no separate "presence warmed" point unless both sides show one.
- Plus the CPU and disk read until b.

### Phase 2: recovery, measured (2026-11-16, node-b)

As designed above (`e454ad7`, `1771670` after the Macha session's review; Dagda's store code `1bfde30`'s). Macha's side
is **develop f0597a4 on Dannan 0.5.3**, not the port branch. Fill: 14 x 512 MiB and 20,000 small files (8.21 GB,
21,792 objects); fill-small: 1 x 512 MiB and the same small files (about 1.2 GB, 20,128 objects). Three rounds each,
the order alternating, fstrim and a 30 s rest before each fill, a cold cache before each reopen, node 0 alone,
get_throttled 0x50000 throughout. `recovery.jsonl` (`build/pi-recovery.sh`). Medians [three runs], milliseconds from
the node's construction:

| Reopen | Figure | Macha | Dagda |
|---|---|---|---|
| fill, after kill -9 | ready (b) | 1714 [1705-1727] | 1725 [1662-1743] |
| | first read (c) | 1738 | 1750 |
| fill, after a clean stop | ready (b) | 1729 [1727-1731] | 1756 [1705-1758] |
| | first read (c) | 1752 | 1780 |
| fill-small, after kill -9 | ready (b) | 1669 [1639-1683] | 1651 [1650-1698] |
| fill-small, after a clean stop | ready (b) | 1654 [1651-1665] | 1639 [1616-1670] |
| every reopen | CPU to ready, disk read to ready | 0.5-0.6 s, 98-100 MB | 0.5-0.6 s, 98-100 MB |
| the fill itself | fill / fill-small seconds | 148.8 / 113.7 | 177.5 / 87.0 |

Findings:
1. **Recovery: at parity**, within 2% either way on every reopen, no failure, a kill -9 reopen no slower than a clean
   one on either. Data open (a) is within a few milliseconds of ready on both (on Macha's side three of twelve fell
   inside one 1 ms poll of ready and read -1, "not seen"); the store's build after ready (Macha) costs about 1 ms.
2. **The workload does not yet isolate the store's recovery.** Ready barely moves from 8.2 GB to 1.2 GB at the same
   object count, and every reopen reads the same 99 MB: about 1.6 s is a fixed cost common to both, most likely the
   program and its libraries read from the cold cache. **Not pursued** (the Macha session, 2026-11-16: parity is what
   Macha needed here; a synthetic ten-times fill would still sit far below the live nodes' millions of extents and
   cost hours of silicon without a reason). Recovery at live scale is the real-state migration rehearsal's to show:
   dropped for now on 2026-11-05, but the maintainer had one run on 2026-11-12 (COMPLETED.md) and made it a gate before the flag
   day on 2026-11-14 ("Macha's suites on Dagda; the side-by-side harness on the Pis; the migration rehearsal on copies
   of real state; reliability under crash, restart and partition; then the port against the live baselines", Macha's
   `TODO/2026-11-14-port-rewrites.md`). If it shows recovery diverging, it is isolated then, the binaries warmed after
   the cache drop.
3. The fills: the large ones vary run to run on both (Macha 135-177 s, Dagda 146-184 s); the small one is Dagda's,
   87 s against 114 s, in every round.

**The kernel changed again after the recovery runs.** On 2026-11-16 (morning) node-b and the other build Pis moved to a
4K-page, 48-bit-VA kernel (6.18.62-v8-va48+). Every figure in this file was taken on node-b's 6.18.50 kernel: figures
taken later are not like for like with these, and a comparison across the change runs both stores again.
