# Contributing to Dagda

## Read first

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Dagda is
the store of a stack of three projects with one set of conventions: Dannan
the network layer (`../dannan`), Dagda (`../dagda`) and Lugh the directory
(`../lugh`). `TODO/2026-11-03-stack.md` says what each owns. Dagda and Lugh
use Dannan as their network layer, Lugh uses Dagda as its datastore, and no
code is duplicated between the three (the maintainer, 2026-11-03). Macha (`../macha`)
is the first customer: Macha is ported onto Dannan and Dagda with no loss of
function. Dagda is written from the design, never transcribed from Macha.

`../dannan/CONTRIBUTING.md` is the reference text for the conventions all
three share; this file is kept identical to it in everything but the
project's name, its build line and its "Read first".

The four laws and five disciplines (`docs/principles-and-laws.md`, a verbatim copy
of Macha's) govern every change here, cited by number. A change that touches
scheduling, admission, priority, retry or recovery says which law it serves and
which discipline it follows.

## Branches

- `main` is the last stable state. Nothing is committed to it directly; stable
  states reach it by merging `develop`.
- `develop` is where work happens.
- Work that needs isolation branches off `develop` as `feature/<short-name>` and
  merges back into `develop`.

## Tags and releases

- Tags are exactly `x.y.z` semver: annotated, no `v` prefix, no suffix, cut from
  `main`.
- The tag, the CMake `project(... VERSION ...)` and `--version` are the same
  number.

## Commits

- Subjects are short imperative prose. No emoji, no ticket prefixes.
- Keep unrelated changes in separate commits: a move, a rename, a fix and a feature
  are four commits. A commit that moves files does not change their content.
- A commit message ends with its last line of prose. No tool attribution trailers.

## Building and testing

Dagda links an installed Dannan of the pinned minor version
(`find_package(dannan 0.2 CONFIG)`). Once per checkout, build the pinned tag
from the sibling repository into `build/deps/` (this reads `../dannan` and
writes nothing there):

```
git clone --branch 0.2.0 --depth 1 file://$PWD/../dannan build/deps/dannan-0.2.0
cmake -S build/deps/dannan-0.2.0 -B build/deps/dannan-0.2.0-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$PWD/build/deps/prefix -DCMAKE_PREFIX_PATH="/opt/local/libexec/boost/1.88;/opt/local"
cmake --build build/deps/dannan-0.2.0-build -j8 && cmake --install build/deps/dannan-0.2.0-build
```

Then:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDAGDA_BUILD_TESTING=ON -DDAGDA_WARNINGS_AS_ERRORS=ON \
  -DCMAKE_PREFIX_PATH="$PWD/build/deps/prefix;/opt/local/libexec/boost/1.88;/opt/local"
cmake --build build -j8
./build/dagda_tests
```

Run the test binary rather than `ctest`. Everything runs on a developer machine
(MacPorts for every dependency on the laptop); there is no hosted CI.
Sanitizers run before a release, or to hunt a bug that cannot be pinned any
other way. Multi-node beds (`test/compose/`) are run by hand, never in the
per-commit gate.

Write tests from the design, the laws and the function inventory, not from the
current behaviour.

## Dependencies

Dannan, Boost (Asio) and OpenSSL, plus GoogleTest for the test build. Prefer
writing something by hand over adding a library, and say so if a dependency
really is the right call.

## Layout

One layout for all three projects, as `../dannan/CONTRIBUTING.md` "Layout"
gives it, with `<p>` = `dagda`: the library target `dagda` in `dagda/`,
public headers in `dagda/include/dagda/`, subsystems in
`dagda/<subsystem>/` (`domain`, `store`, `ledger`, `heads`, `lease`),
interfaces in `dagda/contract/`, tests in `dagda/test/` (one binary
`dagda_tests`), fuzz targets in `dagda/fuzz/`, tools in `tools/`, beds in
`test/compose/`. No presets.

## Shared code

What more than one project needs lives once, in the lowest project that
needs it, and the others link it. Dagda links Dannan's crypto primitives,
durable files, byte codec, thread-safety contract, time sources, logging (a
logger from `Context::logger`), Status surface and test fixtures, and writes
none of its own. When Dagda finds itself writing something another project
has, or needs, it asks that project's session instead.

## Errors

An operation that could not happen throws (`std::invalid_argument` for a
caller's mistake, `std::runtime_error` or a named subclass for bytes or a
system that refused). An operation that happened returns its result, and one
that happened on more than one node returns its evidence (who answered, what
failed and why), never a bare boolean. A read that finds nothing succeeds
with an empty value.

## Style

- Formatting is `.clang-format` (Google base, 160 columns): run `clang-format -i`
  on the files you changed. Two-space indent, `#pragma once`, braces on the same
  line, headers `.h`.
- `snake_case` for functions, methods, variables and files; `PascalCase` for
  types; private and protected members prefixed with `_`. Namespaces are
  `<project>::<subsystem>`.
- Every source file starts with the Dagda header block (`CLAUDE.md` has it).
- No emoji anywhere.
- Dependency injection over globals; the only process global is the io context.
- Each component logs through its own scoped logger.
- Thread-safety annotations on every guarded member; contracts declare their waits
  and thread safety beside each operation.
- Comments are sparse and explain intent, not mechanics.

## Plan

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Move items
across as they land.
