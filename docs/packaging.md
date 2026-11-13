# Packaging

Dagda installs as Dannan does:

- the shared library, whose SONAME is the whole version (`libdagda.so.0.1.4`),
  because no release promises another's ABI yet, so a host built against one
  refuses to load another;
- the headers under `include/dagda/`, in their tree, so `"dagda/<subsystem>/x.h"`
  includes the same installed as in the repository; the tests and their
  fixtures are not installed;
- the CMake package configuration under `lib/cmake/dagda/`:
  `find_package(dagda <version> CONFIG)` accepts only that exact version, and
  finds Dannan, at the version Dagda pins, through it; then link `dagda::dagda`;
- `dagda-migrate` and its manual page, which move a Macha node's layout v18
  store to v19 in place and prove the data survived (`docs/migration.md`).

## Into a prefix

How the Macha port consumes Dagda beside Dannan:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=<dannan prefix> -DCMAKE_INSTALL_PREFIX=<prefix>
cmake --build build/release -j8 && cmake --install build/release
```

`test/package` is a consumer of that install: configure it with
`-DDAGDA_EXPECTED_VERSION=<version>` and both prefixes on `CMAKE_PREFIX_PATH`,
build it and run `consumer <version>`; it exits 0 when it linked that version.
