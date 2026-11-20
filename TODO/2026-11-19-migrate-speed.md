# dagda-migrate at the disk's rate (2026-11-19)

**Status: built, gated and released in 0.3.1** (tag `0.3.1`, main `2793458`; `COMPLETED.md`, "2026-11-19 (evening)").
As built, loose objects are ordered by FIEMAP after a sync (not before: a fresh file's blocks moved between runs),
the progress record names the file at its point, and the manifest pass journals its loose values beside the
partial. The per-pass figures below are the Macha session's rates; no whole pass has been timed on the nodes.

## Why

The cutover was aborted (Tom, 2026-11-19, relayed by the Macha session): on macnessa (a 10 TB USB hard disk, Pi 5)
`dagda-migrate --dry-run` read at a steady 64-65 MiB/s, one thread at about 50% CPU and in D state the rest, the
disk delivering about 73 MB/s. That is about 24 h a full pass over 5.9 TB, and the runbook made four (dry run,
manifest before, migration, manifest after): about 4 days down for macnessa.

Found in the code (`migrate/migrate.cpp`, `migrate/manifest.cpp` at 0.3.0):
- Every record is read and authenticated on one thread, nothing overlapped: a pack record's header and payload in
  two preads, then a full AES-GCM open under v18 or v19, then the 48-byte tag and checksum rewrite; a loose object
  read whole, opened, its 16-byte tag written and the file fsynced. The disk waits on the CPU and the CPU on the
  disk.
- Loose objects are visited in id order, not disk order.
- The migration does not need the bytes: a record's v19 tag is derived from its v18 tag, the two associated data and
  the ciphertext's length (`migrate/retag.h`). The full read only authenticates each record first, which tells v18
  from v19 on a resumed run, finds a torn tag, and lists damage.

## Tom's choice: B (2026-11-19)

The migration moves tags from headers alone; the before and after proof stays two independent full manifest
passes, each at the disk's rate. The dry run leaves the runbook: everything structural is refused before the first
write, and damage is what the manifests and the rehearsal list.

### The manifest pass, pipelined

`migrate/pipeline.h`: an ordered pipeline. A producer yields items in order, workers transform them on their own
threads, and the caller consumes the results in item order, at most a window ahead. The manifest keeps its order,
its checkpoints and its format, so `--resume` and the comparison are unchanged.
- Loose objects: each item is an index; a worker reads, decodes and authenticates the file, so several reads are
  outstanding.
- Pack records: the producer walks the pack and reads each record (sequential within the file, the kernel's
  readahead streaming it); workers authenticate.
- Workers: the cores less one, at least one. Window: bounded by records and by bytes.

### The migration, header-only

- A record is moved from its header alone: a pack record's 125-byte header, a loose object's first bytes. Its tag
  is derived and written; its payload is not read.
- **The progress record** (`<state>/migration-progress`, written durably): the direction, and the point everything
  before which is moved and on the device (the store, loose or packs, the index or the pack and offset). It is
  written before the state's going-forward marker, and after each batch's sync.
- A batch is up to 256 records (one with a test's step hook). Loose objects of a batch are synced together (one
  `syncfs` on Linux), pack records with one `fsync`.
- **The resume window**: a run that finds a progress record authenticates the records from its point up to one
  batch past it, as today (v18, v19, a torn tag); beyond that, header-only.
- **A check every batch**: the first record of each batch is authenticated in full. If it is already in the target
  layout, the progress record does not say what the disk holds, and the rest of that store is migrated as today,
  every record authenticated. A tag derived twice is the tag it started as, so a stale record could only undo, never
  damage; the check keeps it from undoing.
- **No progress record** with the state marked part way (a run of an earlier version, or the record lost): every
  record authenticated, as today.
- The key is proved before the first write, as today: the probe authenticates the first record of each store.
- What is no longer done in the migration: listing damaged records (the manifests list them, and the comparison
  names them). A damaged record's tag is derived like any other and stays damaged.
- `--dry-run` keeps authenticating everything: it is the one pass that writes nothing and reports damage.
- Going back: the same, with its own direction in the progress record.

### Expected

Per full manifest pass, the disk's sequential rate. The migration itself is bound by seeks between records: about
1.5 M records at 5-10 ms, 2 to 4 h for 5.9 TB. The Macha session is measuring macnessa's disk (dd at two or three
places, `read_ahead_kb`, loose against pack counts).

### Tests

Test-first, gated on a Pi. New cases:
- the manifest pass gives the same manifest pipelined as serial, and resumes from every checkpoint as before;
- the migration reads only headers outside the resume window (its bytes read are headers and the window's records);
- a progress record that says less than the disk holds (a stale one) is caught by the batch check and the run
  finishes correctly;
- no progress record under a part-way marker: every record authenticated, and the run finishes;
- every existing crash case still passes (die after any step, forward and back, twice, torn writes).
