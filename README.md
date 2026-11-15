# Dagda - The Distributed Object Store

Dagda is the store for a cluster of trusted machines. It is a C++20 library that keeps
objects, claims, leases and shared state across a set of nodes, sealed at rest, with
every write carrying a durability level the caller chooses and every answer saying which
nodes took part. It is the middle of a stack: it runs on Dannan, the network, and is the
datastore for everything above it.

## Why it exists

Macha, a media filesystem that runs across several machines, grew its own distributed
store: a DHT of immutable extents and control objects, a claims ledger and a garbage
collector that never removes what someone still needs, a Merkle trie over what each node
holds, repair by diff, opaque heads whose merge the filesystem supplies, and leases that
let one node own a job until it falls away. It works, and it has been in use for a long
time. It is also welded to Macha: its transport, its crypto, its logging and its clock
are Macha's, and nothing else can use it.

The other systems in the same cluster need exactly that store. Lugh, the directory and
single sign-on, has records that must be on every node and secrets that must be on a few,
with merge rules of its own. Lir, the event system, has retained messages and
subscriptions that must survive a node. Each would otherwise write its own store, and
each would get it a little wrong.

Dagda is Macha's store taken out and generalised, so that it is written once:

- **It is Macha's.** Macha is ported onto Dagda with no loss of function, gated by
  named tests; its existing disks are migrated in place by a tool that needs no extra
  storage; and Dagda handles the small values of the other systems as well as Macha's
  media extents.
- **It runs on Dannan.** Transport, node identity, crypto, secure memory, durable files,
  logging, time and the thread-safety contracts are Dannan's. Dagda writes none of its
  own, so there is one copy of each in the stack.
- **It is honest about what happened.** A write names its durability level: ephemeral,
  persisted, replicated or durable. The answer is evidence, never a bare boolean: which
  nodes acknowledged, which are pending, and a ticket that reports again when they land.
  No call waits without a deadline, and a read that finds nothing succeeds with an empty
  value.
- **It never loses a claim.** Garbage collection runs from an owner's reachability and
  removes only what the ledger proves unreferenced; a claim walk repairs what a claim
  names.

## What it does

- **Domains and sealing.** Named domains, each with its own keys, sealed with Dannan's
  primitives; node-local spaces whose keys are sealed under the node's key custody.
- **Objects.** Immutable, content-addressed, on one or more DATA backends per node with
  a CONTROL store beside them; packed or loose; written through admission that keeps a
  reserve of credit and device operations for control work so a bulk load cannot queue
  a commit behind it.
- **Placement and durability.** Rendezvous placement over Dannan's roster; per-write
  durability levels with evidence and tickets; prompt replication of the copies a write
  still owes; a write-behind path for a provisional copy.
- **The ledger.** Claims with reference counts, a held trie per node, the union and
  diff of tries across nodes, retention and the collector, and node horizons so a node
  that was away catches up by difference, never by scan.
- **Availability and repair.** A survey of what each peer lacks, diff only, and repair
  paced by a duty share against foreground work: always running, never in the way.
- **Leases and claimed collections.** A lease with an epoch fence, a keeper that takes
  over from a node the roster has lost after a window measured from when it was last
  heard of, and a collection template whose records merge field group by field group,
  each group on its own clock.
- **Heads** (next). Opaque values with a replica policy, whose merge the owner supplies
  and whose common ancestor the merge is given when held.

## The stack

Dagda is one of four projects written to one set of conventions, each its own library
and repository, with no code duplicated between them: a piece needed by more than one
lives in exactly one, the lowest that needs it, and the others link it.

```
   Lugh            Lir             Macha, AthenaSIP, ...      hosted systems
     |               |                  |
   Dagda: the store  (domains, sealing, objects, heads, leases)
     |
   Dannan: the network  (reachability, routes, channels, membership,
                         identity, placement, work classes, status)
```

- **[Dannan](../dannan)** is the network: it makes a set of nodes, on any mix of home
  and rented machines and behind any NAT, behave as one network, and knows who the
  nodes are. Dagda is a hosted system on it, a peer of the others, not a floor under
  them: a node may run Dannan alone. Dagda takes from it the node and its roster, the
  transport and message registration, node identity and the scope check, key custody,
  secure memory, the crypto primitives, durable files, work classes and the duty share,
  logging, time, the thread-safety contracts and the test fixtures. Dagda is pinned to
  a Dannan release in `CMakeLists.txt`.
- **[Lugh](../lugh)** is the directory and single sign-on. It keeps everything it
  persists in Dagda, in two domains: `lugh/scope` on every node and `lugh/secrets` on
  Lugh's, under its own merge rules. Lugh is also where Dannan's scope check looks up
  who may write what.
- **[Lir](../lir)** is the event system, a distributed multimaster MQTT broker on
  Dannan and Dagda: retained messages and subscriptions live in Dagda.
- **Macha** is the media filesystem Dagda was extracted from, and the first system
  ported onto it. Ingest, the catalogue and the filesystem stay Macha's; the store is
  Dagda's.

Hosted systems never address one another. They exchange frames through Dannan, share
state through Dagda, and learn who someone is from Lugh.

## How to use it

### Build

A C++20 compiler, CMake 3.24 or later, Boost 1.83 or newer (Asio), OpenSSL 3 and
GoogleTest, and an installed Dannan of the pinned minor, at least the pinned patch. Build the pinned Dannan
tag from the sibling checkout into the build tree once, then configure against it:

```bash
git clone --branch 0.8.0 --depth 1 file://$PWD/../dannan build/deps/dannan-0.8.0
cmake -S build/deps/dannan-0.8.0 -B build/deps/dannan-0.8.0-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$PWD/build/deps/prefix
cmake --build build/deps/dannan-0.8.0-build -j8 && cmake --install build/deps/dannan-0.8.0-build

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDAGDA_BUILD_TESTING=ON -DDAGDA_WARNINGS_AS_ERRORS=ON \
  -DCMAKE_PREFIX_PATH=$PWD/build/deps/prefix
cmake --build build -j8
```

Add your Boost and OpenSSL prefixes to `CMAKE_PREFIX_PATH` where they are not on the
default path. Warnings are errors in that configuration, and the lock annotations are
checked, not decorative.

`cmake --install build` installs the library, its headers, its CMake package and
`dagda-migrate` under `CMAKE_INSTALL_PREFIX`; [docs/packaging.md](docs/packaging.md) says
what goes where and how a consumer finds it.

### Link it

A hosted system links `dagda::dagda`, opens a `dagda::Node` on its Dannan node before
that node starts (the node registers its message types and declares its readiness
planes itself), and holds the node's `dagda::Context`:

```cmake
find_package(dagda 0.2.0 CONFIG REQUIRED)  # the exact version; Dannan comes with it
target_link_libraries(my_system PRIVATE dagda::dagda)
```

```cpp
#include "dagda/context.h"
#include "dagda/node.h"

dagda::NodeOptions options;
options.state = state_path / "dagda";
options.key = std::make_shared<const dagda::domain::StorageKey>(dagda::domain::StorageKey::derive_v1(cluster_key));
options.pool.backends.push_back({data_path, limit_bytes, reserve_bytes});
dagda::Node node(dannan_node.context(), options);  // registers Dagda's message types
dannan_node.start();

auto& store = node.context();
store.require_version(dagda::context_version);
dagda::WriteOptions write;
write.level = dagda::Durability::replicated;  // on disk on k nodes
store.objects().write(bytes, write, std::chrono::seconds(10), executor, [](dagda::WriteEvidence evidence) {
  // evidence.id, evidence.replicas (by node id), evidence.ticket
});
```

Every completion is posted to the executor the caller passes, never called inline. A
ticket outlives the call: `wait(ticket, deadline, ...)` reports again when the pending
copies land. The contract is versioned and asynchronous from its first version; a hosted
system built against another version is refused.

### Test

```bash
./build/dagda_tests
```

GoogleTest only, one binary, no Python. Time is injected and stepped, never slept.
Macha's own tests are the port gate and stay Macha's.

## Repository layout

```text
dagda/<subsystem>/  the library: headers and sources side by side
                    (domain, store, ledger, lease, contract; heads next)
dagda/test/         GoogleTest, flat, one binary (dagda_tests); fixtures in dagda/test/fixtures/
docs/               how it works, and the map from Macha's foundations to Dannan's
TODO/               now and next (ACTIVE.md), designs, and what shipped (COMPLETED.md)
```

Conventions, shared with the stack: [CONTRIBUTING.md](CONTRIBUTING.md). What each
project of the stack owns: [TODO/2026-11-03-stack.md](TODO/2026-11-03-stack.md). What
Macha needs of Dagda, requirement by requirement:
[TODO/2026-11-03-macha-store-requirements.md](TODO/2026-11-03-macha-store-requirements.md).

## Status

Version 0.2.0, on Dannan 0.7.0; the work for Macha's port (feature/macha-store-gaps) builds on Dannan 0.8.0. Built and tested: the local store and its
one layout (v19, the domain in every seal), the pool, the CONTROL store and the cache;
the ledger and the collector; the distributed store on Dannan with durability levels,
evidence, tickets, availability and repair; an instant delete beside the collector's
per-domain wait; leases and claimed collections; domains with node-local keys, wrapped
domain keys, member and blind roles, and the scope check at the write floor;
`dagda-migrate`, which moves Macha's v18 disks to v19 in place; and `dagda::Node`, which
opens all of it on a Dannan node, runs maintenance and serves `dagda::Context`; head
spaces, every-node as Macha's are, and node-local, a node's alone, sealed under its
node-local key; the change notice (`Heads::watch`, `Heads::changed`); and the in-process
cluster fixture installed as `dagda::test_fixtures`. `dagda::Context` version 1 is frozen
at 0.1.4 and still served, for Macha's port on 0.1.5; version 2 grows slice by slice
(`TODO/2026-11-13-three-systems-design.md`) and is frozen at slice 4. Every step ends
green in `dagda_tests`, with sanitizers before a release.

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
