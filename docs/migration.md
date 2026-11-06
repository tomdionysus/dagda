# Migrating a Macha node's disks to Dagda's layout

Dagda's store has one on-disk layout, v19. Macha's nodes wrote layout v18.
The two differ in three things and nothing else:

- what a sealed object's tag authenticates: v18 bound the object id alone,
  v19 binds `"dagda/seal/19" || domain id || object id` (Macha's domain is
  domain v1, `827b07101d79d9d84a433d8e9cb262ca`);
- the key the node's own ledgers are sealed under: v18 used the domain's
  key, v19 uses the node-local key derived from the node's key custody
  (`dagda/node-local/1`);
- the markers: the backend marker (`.macha.backend` and its mirror under
  `<state>/backend-identities/`) and the state layout marker
  (`<state>/storage-layout`) name the layout and, under v19, the domain and
  a key check.

The store never migrates a disk on its own. A pool, control store, cache or
state directory that finds a v18 marker refuses to open with a reason that
names this tool and the state path, and changes nothing on that disk. The
migration is run once, by the operator, with the node stopped:

```
dagda-migrate --state DIR --node-state DIR --cluster-key FILE KEY \
              [--backend DIR]... [--control DIR] [--cache DIR] [--dry-run]
```

- `--state`: Dagda's state directory (the `storage-layout` marker, the
  ledgers, the control store under `metadata-objects`).
- `--node-state`: Dannan's state directory, where the node's key custody
  lives (`identity/custody`) and its `node-id`. The node must have custody:
  the ledgers are sealed under the key derived from it and under nothing
  else. It must also hold the id the disks carry: a migrated marker keeps
  the node id it was written with and a pool refuses another node's disk,
  so a backend naming another id is refused before anything is written.
  Either use Macha's state directory as Dannan's, or copy its `node-id`
  there first.
- `--cluster-key`: the cluster key file, which gives domain v1's storage
  key, exactly as Macha derived it. Objects are not re-encrypted.
- `KEY` opens the custody, as `dannan-node` takes it: `--passphrase-env VAR`
  (the passphrase in that environment variable) or `--credential NAME
  [--credentials-dir DIR]` (a systemd credential).
- `--backend DIR`, repeated: every DATA backend, by the path the node's
  configuration names it.
- `--control DIR`: the control store's path when it is not
  `<state>/metadata-objects`.
- `--cache DIR`: the persistent cache's path, if the node keeps one.
- `--dry-run`: classify and report; change no data. It still takes each
  store's lock, which can leave an empty lock file. The refusals are the same
  as a real run's.

Every refusal comes before the first write: the node id, the key (proved
on the first record that opens), a retention checkpoint from before the
claims ledger or claims with no `ledger` marker, and every ledger probed.
The tool reads only the ledgers it knows by name: `ledger/control`,
`ledger/data-*`, `ledger/referenced/{extents,nodes}`,
`retention/{claims.log,ledger-data,ledger-control}` and the two
`retention/unreferenced-*.bin`. Everything else under the state is left
byte for byte, `metadata/` among it until heads define it.

Exit code 0 means the state is v19 (migrated now, or already); 1 means
refused or failed, with the reason on standard error; 2 is a usage error.
Standard output carries the evidence: counts of loose objects and pack
records migrated, already v19 and unreadable; ledgers re-sealed, already
and empty; markers rewritten; bytes read and written; and every record left
as it was, by file and id.

## Proving nothing was lost

There is no copy to hold a migrated disk against, so the proof is two read-only
passes and a comparison, by the same tool:

```
dagda-migrate --state DIR --node-state DIR --cluster-key FILE KEY [--backend DIR]... \
              [--control DIR] [--cache DIR] --write-manifest /elsewhere/before        # node stopped
dagda-migrate --state DIR ... [--backend DIR]...                                      # the migration
dagda-migrate --state DIR ... [--backend DIR]... --write-manifest /elsewhere/after    # before the node starts
dagda-migrate --compare-before /elsewhere/before --compare-after /elsewhere/after
```

The compare's exit code is 0 only when nothing differs and everything was
checked, and its last line is `RESULT: PASS` or `RESULT: FAIL: N differences, M
not checked`. Every difference and everything not checked is a line by name. Each
category is a line with its count on both sides: `SAME`, `DIFFERENT (n)`, `NOT
CHECKED`, or `ZERO` for a category that held nothing, which is never called the
same. A manifest that is truncated, altered or has bytes after its end is a
failure, and so is any manifest taken under another layout, cluster key, node or
set of stores. `--expect-before` and `--expect-after` (18 or 19) compare other
journeys: 18 then 18 is there and back, whose tags must be equal too.

**The before pass takes the place of `--dry-run`:** it reads the same bytes and
refuses the same way, and it leaves no lock files. Take the after pass before the
node starts: the store truncates torn tails, prunes empty files and rewrites its
accounting at its first open, and the comparison will report each one.

### What a pass does

It opens every sealed record with AES-GCM and checks the plaintext's size and
SHA-256 against the id, so an object listed as opened is the object its id names:
the id is the plaintext hash, and it is checked, not assumed. A pass then records,
by category:

| Category | What is compared | Named how |
|---|---|---|
| objects, per store (DATA, control, cache) | Every loose file: id, plaintext size, file size, nonce, touched time; and that it opened under the manifest's layout | `object <id> is lost`, `... no longer opens: its bytes have changed`, `... is new`, `... is v18 in a manifest of layout v19: not migrated`, `... touched time moved` |
| | Every pack file and every record at its offset: put, touch or tombstone, its time, id, sizes, nonce; a torn tail or damaged span by length and SHA-256 | `record at pack 2 offset 925 is lost (touch of <id>)` |
| removed, per store | The ids whose last pack record is a tombstone and that have no loose file, as the store's replay reaches them | `removed object <id> is resurrected` |
| accounting, per store | The slot in force: sequence, used, operation; and the file's SHA-256 | `used bytes A before and B after` |
| markers | The state's, each backend's, each mirror: layout (with the v19 key check proved), node, token | `marker ... is lost`, `marker ... says "...", not layout v19` |
| retention marker | `retention/ledger` | `retention/ledger is lost` |
| held | Every record of `ledger/control` and `ledger/data-*`, read as a store would with the journal applied; `seeded`, `verify-next` | `object <id> is no longer held` |
| claims and dots | Every record of `retention/ledger-{data,control}`, decoded; every frame of `retention/claims.log`, whole | `claim on <id> changed from adds {...} removed {...} to ...`, `frame 3 is lost: add class 0 dot ...` |
| reference counts | Every count in `ledger/referenced/{extents,nodes}`; `state` (root and totals) | `count of <id> changed from 2 to 3` |
| sightings | Every first sighting of each table, in the clear before and sealed after | `sighting of <id> changed from A to B` |
| other files | Every other regular file under the state and each store, by size and SHA-256, so nothing is left out for not being known | `file availability/last-survey.bin has changed` |
| metadata | Heads, certificates and history replay. **NOT CHECKED today, which fails:** heads session S7 adds the reader (`metadata_manifest_reader`, `dagda/migrate/manifest.h`) | |

**What is NOT CHECKED, and so can never pass:** a record that opens on neither
side, whose plaintext cannot be proved, even though the migration did not hurt it;
a damaged span in a pack, whose objects cannot even be named; a ledger that does
not read under the layout's key, or whose journal holds a whole frame that will not
open; a backend the state's mirrors name and no `--backend` does; anything under a
store that is not a regular file. A real disk with a rotted object therefore does
not get a PASS: the operator gets the names and judges them, and a repair from the
other node's copy can be compared afterwards with the store's own tools.

Torn tails, on a pack or a journal, are what the store's recovery drops. They are
counted and do not fail. What a crash left beside a file being replaced
(`<name>.tmp.<pid>.<n>`) is named and not compared: no store reads one, and a second
run removes it. A stale `nodes-<n>.bin` beside a trie's published generation is
likewise not read.

### What it must not do

A pass writes nothing under the state, a backend, the control store or the cache:
not a lock file, not a directory. A node's lock is an exclusive `flock` on
`.macha.lock`, so the pass opens that file if it exists and holds a shared `flock`
on it. A node holding it refuses the pass, naming the store; a node starting
meanwhile refuses to start. Where no lock file exists there is none to hold, so the
pass fails if one has appeared by its end. The manifest itself is refused anywhere
inside those trees and over any file that exists. It is written as
`<manifest>.partial` and renamed when whole, owner-only: it holds object ids, node
ids and claims in the clear, as v18's file names and sightings already did.

### How big, and how long

A manifest's size follows the records and not their bytes: 89 bytes an item,
whether the object is 64 bytes or 128 MiB. `dagda_tests
--gtest_also_run_disabled_tests --gtest_filter='Scale.*Manifest*'` builds a state
of live-a's counts and measures it: 1,031,857 loose objects, 59 packs holding
314,390 + 340,000 puts and 1,883,019 touches and tombstones, a held ledger and a
claims ledger of a million each. That is 5,646,994 items and **504 MB a manifest**,
so give the two about 1.2 GB on another disk, with room for reference counts of the
same order. Its objects are 64 bytes, so its times are what the per-file and
per-record work costs and say nothing of the bytes. On a Mac's disk: before pass 19.4
minutes, migration 19.7, after pass 17.8, comparison 32 seconds.

A pass is as slow as the disk, since it reads every byte: 4 TB at 150 MiB/s, if a
node's USB disk gives that, is about 7.5 hours, more for a million opens on a
spinning disk, and the node is stopped throughout. What makes that bearable:

- **It resumes.** `--resume` goes on from `<manifest>.partial`, which is made durable
  every 256 records or 1 GiB, so a stopped pass reads again at most that much (a Pi's
  power or an operator's ^C costs nothing else). A state that has changed since (a
  loose file or pack of another size or time, moved accounting) refuses the resume:
  a manifest built on what is no longer so would lie.
- **Progress with an end:** the total is known from listing the stores, so every 4096
  records a line gives records, MiB of the total, the rate and the minutes left. The
  listing itself is a stat of each file; measure it on the day.
- **It holds little:** a record at a time, and one listing and one replay a store.
  The comparison is a merge of two streams and its memory does not grow with them.
- **One thread.** A Pi 5 has AES and SHA-2 instructions, so the disk should be the
  bound and threads would buy nothing. This has not been measured on the nodes.
- **No sampling and no skipping,** by design: nothing is there to trade for time.

Three reads in all, then, with the node stopped: before, the migration's own, after.
On live-a, at that guessed rate, about 22 hours, plus 1.03M fsyncs for the
migration. No figure here has been measured on that disk.

## Going back

`--reverse` takes a v19 node back to v18, so it can return to Macha 0.92.x. It
runs the same way in the other direction with the same rules: no second copy,
every refusal before the first write, a crash finished by a second run. It also
accepts a v18 marker, so a forward run that crashed part-way can be taken back
instead of finished. From its first write to its last the state's marker says it
is on its way back: the store and a forward run refuse it, since part of it is
each layout, and running `--reverse` again finishes it. Only Macha's domain goes back. Objects and markers come
back byte for byte as Macha wrote them; a ledger holds what it held, sealed under
a fresh nonce. What Dagda wrote while the node ran v19 goes back with the rest.

## What it does, in place

Nothing is copied and no second copy of the data is needed. For each
backend, then the control store, then the cache:

- every loose object (`objects/xx/yy/<id>.obj`) has its 16-byte GCM tag
  overwritten at its fixed offset in the header;
- every pack put record has its tag and the SHA-256 over its header prefix
  overwritten, 48 bytes in one write; touches and tombstones carry nothing
  sealed and are untouched;
- the nonce and the ciphertext are not changed. AES-GCM's ciphertext depends
  on the key, the nonce and the plaintext alone, so the record opened under
  v18's associated data and sealed again under v19's with the same nonce
  gives the same ciphertext and only another tag. Sealing twice under one
  key and nonce with the same plaintext reveals nothing new.

Then every ledger under the state (`ledger/data-<token>`, `ledger/control`,
`retention/claims.log` and the claims tries, the reference counts) is
re-sealed from the domain key to the node-local key, file by file: a
journal is read frame by frame and written whole to a sibling file that is
renamed over it; a trie has its journal folded in and every live node
written into its next generation's file under the new key, then its root
published (one atomic replace). The extra space needed at any moment is one
ledger file (for a trie, its live nodes once more).

Last, the markers: each backend's `.macha.backend` and its mirror under
`<state>/backend-identities/`, then `<state>/storage-layout`. From the first write
until that last one `<state>/storage-layout` says
`dagda-state-layout-going-forward`, because a disk that is part v18 and part v19
must open as neither: Macha refuses any marker but its own, and a service restarted
by a power cut would otherwise read the moved records as corrupt.

## What a crash leaves, and how to resume

Run the tool again. What a crash left beside a file being replaced (a
`<name>.tmp.<pid>.<n>`, as large as a ledger) is removed first. Each record is either v18 or v19, told by which
associated data authenticates it, so a second run skips the records already
done and migrates the rest. Each ledger file is under the old key or the
new, told by which key opens it, so it is likewise skipped or re-sealed.
Until the run completes Macha and the store both refuse the disk, and the store
names the tool. A state whose layout marker is v19 is reported as nothing to do.

A pack record's 48-byte rewrite is one write; if the power goes in the middle
of it (it crosses a sector edge for about one record in 85), the header's
checksum no longer holds and the tag on the disk is part old and part new. The
two differ by a value known from the associated data alone, so the second run
tries what the tear can have left, and a tag that authenticates finishes the
record; only a record no such tag opens stays listed as damaged. A loose
object's tag the same. The report counts them as cut short and finished. Run
again, then, before the store opens the disk: the store has no such recovery and
would read that record as a damaged span. A torn tail on a pack (a crash of Macha's
before the migration) is left for the store to truncate at open; a torn tail
on a journal is dropped as the ledger's own replay would drop it. Both are
counted in the report.

## What it refuses

Before writing anything:

- a state with no `storage-layout` marker, or one that names another layout
  or another domain's v19;
- a backend that is not a directory or has no `.macha.backend`, or whose
  marker is neither v18 nor domain v1's v19;
- a store whose lock is held (the node is running);
- a backend the state's `backend-identities` name and no `--backend` does, since
  that disk would stay v18 under a state marked v19, where no later run looks;
  a v19 state given a backend still v18 likewise;
- a cluster key under which the first eight records found open under
  neither layout (the wrong key, or not a Dagda disk).

A ledger that opens under neither the node-local key nor the domain key (another
node's, or damaged) is refused by name, before any write too. A single record that opens under neither layout once the key has
been proved is not a refusal: it is listed, left as it was, and the store
treats it as corrupt (repaired from replicas) after the migration.
