# Packaging

Dagda is packaged for Debian trixie, on x86-64 and arm64, as Dannan is:

- `libdagda<version>` (for example `libdagda0.3.3`): the shared library. Its
  SONAME is the whole version, `libdagda.so.0.3.3`, because no release
  promises another's ABI yet; each release is its own package, so two can be
  installed side by side and a host built against one refuses to load another.
- `libdagda-dev`: the headers, the `libdagda.so` link and the CMake package
  configuration. It depends on exactly its own release's library and on
  `libdannan-dev`; `find_package(dagda <version> CONFIG)` accepts only that
  exact version, and finds Dannan through it.
- `dagda-tools`: `dagda-migrate`, which moves a Macha node's layout v18 store
  to v19 in place and proves the data survived (`docs/migration.md`).
- Debug symbols for each, as `-dbgsym` packages.

The packages are built with debhelper from `debian/`, driving the CMake build;
the CMake install rules decide where every file goes, and `debian/rules` splits
them between the packages.

## Without packages

The same install rules serve a prefix, which is how the Macha port consumes
Dagda beside Dannan:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=<dannan prefix> -DCMAKE_INSTALL_PREFIX=<prefix>
cmake --build build/release -j8 && cmake --install build/release
```

`test/package` is a consumer of that install: configure it with
`-DDAGDA_EXPECTED_VERSION=<version>` and both prefixes on `CMAKE_PREFIX_PATH`,
build it and run `consumer <version>`; it exits 0 when it linked that version.
It also builds `fixture_consumer`, which needs GoogleTest: one case on the
installed in-process cluster fixture (`dagda::test_fixtures`, the headers under
`dagda/test/fixtures/`), as Lugh's and Lir's multi-node tests use it. Run it; it
exits 0 when the case passes.

## Building

On Debian trixie, natively for the machine's architecture, from a git
checkout, with Dannan's `libdannan-dev` installed:

```sh
sudo apt install dpkg-dev
sudo apt build-dep ./
tools/build-deb.sh
```

The packages are written to `build-deb/`. The tests run inside the build;
`DEB_BUILD_OPTIONS=nocheck` skips them.

## Versions

The version comes from `git describe`. The tag `0.3.3` gives `0.3.3`; a build
three commits later gives `0.3.3+git3.gabc1234`, which sorts after `0.3.3` and
before `0.3.4`. Release tags sit on `main`, so a build on `develop`, where
none is reachable, gives the tree's version and the commit,
`0.3.3+git.gabc1234`. Uncommitted changes add `.dirty`. A build of a tag must
match `project(dagda VERSION ...)` in `CMakeLists.txt`, and the library's
package named in `debian/control` must be `libdagda` and that version, or the
script refuses it. A release's version commit changes all three. Only builds of
a tag are released.
