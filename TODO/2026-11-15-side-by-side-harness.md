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
   to 6 MB/s on the live pair); repair while a viewer reads (paced, never stopped).
8. Recovery: the time to ready with a large store (pack replay, ledger seeding, presence warm-up), clean and after a
   `kill -9`.
9. Disk pressure: a slow device (`dm-delay` over a loop file, so no real disk is touched) and the pressure onset and
   release, with a foreground reader present.
10. Cache: the persistent block cache's hits, misses and evictions under a playback-like scan larger than the cache.

Where Macha's `build/baselines/baseline.py` already has a phase, its sizes are taken, so the numbers compare with
Macha's results-0.94.2.

## What is still to settle before anything runs

- The Dannan 0.5 patch Macha 0.94.2 runs on (the Macha session).
- 5 and 6 drive Macha's metadata and catalogue CONTROL paths: on Macha's side through `NamespaceControlStore` and the
  catalogue's objects in `macha_core`; on Dagda's through Context::Control and Claims, until 8c gives both heads. The
  Macha session to say which Macha calls stand for "a commit" and "a manifest".
- 9's `dm-delay` needs device-mapper on the Pi kernel and root: checked, not installed.
- The order: 1, 2, 4 and 8 on one Pi first (they need no link), then 3 and 7 on two, then 5, 6, 9, 10.

## Results

Kept under `build/harness/results/<date>/`, one JSON line a figure, and summarised here with both stores' numbers side
by side, each difference a finding: Dagda at least as good, or a gap fixed test-first before the port.
