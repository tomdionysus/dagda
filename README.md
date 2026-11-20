# Dagda - The Distributed Object Store

[![CI](https://github.com/tomdionysus/dagda/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/dagda/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/tag/tomdionysus/dagda?sort=semver&label=release)](https://github.com/tomdionysus/dagda/tags)
[![codecov](https://codecov.io/gh/tomdionysus/dagda/graph/badge.svg)](https://codecov.io/gh/tomdionysus/dagda)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Dagda is the store for a cluster of trusted machines. It is a C++20 library that keeps
objects, claims, leases and shared state across a set of nodes, sealed at rest, with
every write carrying a durability level the caller chooses and every answer saying which
nodes took part. It runs on [Dannan](https://github.com/tomdionysus/dannan), the network,
and is the datastore for the systems hosted above it.

## What it gives a hosted system

- **Domains and sealing.** Named domains, each with its own keys; node-local spaces sealed
  under the node's own key.
- **Objects.** Immutable and content-addressed, placed by rendezvous over the cluster's
  roster, written at a durability level (ephemeral, persisted, replicated or durable). The
  answer is evidence: which nodes acknowledged, which are pending, and a ticket that
  reports again when they land.
- **Claims and collection.** Reference-counted claims; the collector removes only what the
  ledger proves unreferenced, and an instant delete beside it.
- **Repair.** Each node learns by difference what its peers lack and sends each copy once,
  paced so it never gets in the way of foreground work.
- **Heads.** Shared values in spaces, on every node or on one node alone, whose merge the
  owner supplies; a change notice; and a register map, keyed values with conditional
  writes, listed and paged in key order.
- **Leases.** A lease with an epoch fence, taken over from a node the cluster has lost.

No call waits without a deadline, every completion is posted to the executor the caller
passed, and a read that finds nothing succeeds with an empty value.

## Install

Dagda needs a C++20 compiler, CMake 3.24 or later, Boost 1.83 or newer (Asio), OpenSSL 3,
and an installed Dannan of the pinned minor (`find_package(dannan 0.10.0 CONFIG)`);
GoogleTest for the tests. Dannan is packaged for Debian 13; see
[its README](https://github.com/tomdionysus/dannan#install).

From a release tag:

```bash
git clone --branch 0.4.0 --depth 1 https://github.com/tomdionysus/dagda.git
cd dagda
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<dannan prefix>
cmake --build build -j8
cmake --install build --prefix /opt/dagda
```

Add your Boost and OpenSSL prefixes to `CMAKE_PREFIX_PATH` where they are not on the
default path. Dannan, Dagda and the system that links them must be built against the same
Boost.

`cmake --install` installs the library, its headers, its CMake package, the in-process
cluster test fixture and `dagda-migrate`. On Debian 13, `tools/build-deb.sh` builds the
packages `libdagda<version>`, `libdagda-dev` and `dagda-tools`;
[docs/packaging.md](docs/packaging.md) says what goes where.

## Use

Link `dagda::dagda`, naming the exact version (each release is its own ABI):

```cmake
find_package(dagda 0.4.0 CONFIG REQUIRED)  # Dannan comes with it
target_link_libraries(my_system PRIVATE dagda::dagda)
```

Open a `dagda::Node` on your Dannan node before that node starts, and use its
`dagda::Context`:

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
write.ack_level = dagda::Durability::replicated;  // on disk on a majority of the replication set
store.objects().write(bytes, write, std::chrono::seconds(10), executor, [](dagda::WriteEvidence evidence) {
  // evidence.id, evidence.replicas (by node id), evidence.ticket
});
```

A ticket outlives the call: `wait(ticket, deadline, ...)` reports again when the pending
copies land. The contract is versioned; a hosted system built against another version is
refused.

### Migrating a Macha node

`dagda-migrate` moves a Macha node's layout v18 store to Dagda's layout in place, without
needing twice the storage, and proves the data survived with before and after manifests:
[docs/migration.md](docs/migration.md).

## Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDAGDA_BUILD_TESTING=ON -DCMAKE_PREFIX_PATH=<dannan prefix>
cmake --build build -j8
./build/dagda_tests
```

GoogleTest, one binary. Time is injected and stepped, never slept.

## The stack

Dagda is one of a set of projects written to one set of conventions, with no code
duplicated between them: a piece more than one needs lives once, in the lowest project that
needs it.

- [Dannan](https://github.com/tomdionysus/dannan): the network. Reachability, routes,
  channels, membership, node identity, key custody, crypto, durable files, logging and time.
- Dagda: the store, on Dannan.
- Systems hosted on both: [Lugh](https://github.com/tomdionysus/lugh), the directory and
  single sign-on; [Lir](https://github.com/tomdionysus/lir), a multimaster MQTT broker;
  [Macha](https://github.com/tomdionysus/macha), the media filesystem Dagda was extracted
  from; [Louhi](https://github.com/tomdionysus/louhi),
  [Mimir](https://github.com/tomdionysus/mimir) and
  [Kvasir](https://github.com/tomdionysus/kvasir); and
  [AthenaSIP](https://github.com/blackravenltd/athenasip).

Hosted systems never address one another: they exchange frames through Dannan and share
state through Dagda.

## Documentation

- [docs/packaging.md](docs/packaging.md): the Debian packages and the install layout.
- [docs/migration.md](docs/migration.md): moving a Macha node's store.
- [docs/principles-and-laws.md](docs/principles-and-laws.md): the laws and disciplines
  every change follows.
- [CONTRIBUTING.md](CONTRIBUTING.md): conventions, shared with the stack.

## Code of conduct

Everyone taking part in Dagda's issues, pull requests and discussions follows the
[Code of Conduct](CODE_OF_CONDUCT.md).

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
