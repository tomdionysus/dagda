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

## Going back

`--reverse` takes a v19 node back to v18, so it can return to Macha 0.92.x. It
runs the same way in the other direction with the same rules: no second copy,
every refusal before the first write, a crash finished by a second run. It also
accepts a v18 marker, so a forward run that crashed part-way can be taken back
instead of finished. Only Macha's domain goes back. Objects and markers come
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
`<state>/backend-identities/`, then `<state>/storage-layout`. A disk is v18
until everything on it is v19.

## What a crash leaves, and how to resume

Run the tool again. What a crash left beside a file being replaced (a
`<name>.tmp.<pid>.<n>`, as large as a ledger) is removed first. Each record is either v18 or v19, told by which
associated data authenticates it, so a second run skips the records already
done and migrates the rest. Each ledger file is under the old key or the
new, told by which key opens it, so it is likewise skipped or re-sealed.
The markers are rewritten last, so until the run completes the store still
refuses the disk and names the tool. A state whose layout marker is v19 is
reported as nothing to do.

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

After the objects, a ledger that opens under neither the node-local key nor
the domain key (another node's, or damaged) is refused by name; the objects
already migrated are valid and a second run skips them, and the markers are
still v18. A single record that opens under neither layout once the key has
been proved is not a refusal: it is listed, left as it was, and the store
treats it as corrupt (repaired from replicas) after the migration.
