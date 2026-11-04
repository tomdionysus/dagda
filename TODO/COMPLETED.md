# Dagda - Completed Work

The record. Items move here from `ACTIVE.md` as they land, with what
shipped and the gate counts.

## 2026-11-04: step 0, the skeleton (`feature/skeleton`)

- First commit on `main` (`8249997`): conventions, the stack document,
  the plan and the requirements; `develop` branched from it.
- The shared layout: top-level `CMakeLists.txt` (`project(dagda VERSION
  0.1.0)`, `DAGDA_BUILD_TESTING`, `DAGDA_WARNINGS_AS_ERRORS`,
  `DAGDA_BUILD_FUZZING`), the `dagda` shared library (`dagda::dagda`) in
  `dagda/`, generated `dagda/version.h`, `dagda_tests` in `dagda/test/`.
- Dannan pinned at 0.2.0 by `find_package(dannan 0.2 CONFIG)`, linking
  `dannan::dannan`; built from the tag into `build/deps/prefix`
  (`CONTRIBUTING.md` has the lines). Dannan's header-only fixtures are
  used from the install, `DANNAN_TEST_TEMP_ROOT` set to `build/test-tmp`.
- Gate: builds with warnings as errors; `dagda_tests` 3 (library matches
  its headers; built against the Dannan it links, 0.2; Dannan's
  `TempDir` writes inside the build tree).
- Next: step 1, the local store. Its code can move now; its gate waits on
  the Macha session's step-1 fixture, which follows Macha's tree-native
  branch merge.

## 2026-11-03, night: the project starts

- `git init`; basemind set up (`basemind.toml`, rules in the gitignored
  `CLAUDE.local.md`) and the index built.
- Dannan's conventions taken whole: `.clang-format`, `.gitignore`,
  `LICENSE`, `docs/principles-and-laws.md` (Macha's master, with Dannan's
  source note), `CLAUDE.md` and `CONTRIBUTING.md` in Dannan's form.
- `TODO/2026-11-03-stack.md`, the delineation of Dannan, Dagda and Lugh,
  drafted with the Dannan and Lugh sessions' reviews, under Tom's four
  rules for the stack. No code; no commits. Gate: none yet.
